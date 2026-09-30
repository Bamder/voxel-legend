#pragma once

// GLSL 330 shader sources, embedded as raw string literals.

namespace shaders {

inline constexpr const char* WORLD_VERT = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aNormal;
layout(location = 3) in float aFaceShade;
layout(location = 4) in float aAO;
layout(location = 5) in float aAlpha;
layout(location = 6) in float aBlockLight;

uniform mat4 uMVP;
uniform vec3 uChunkOffset;   // chunkOriginWorld - cameraPos (camera-centered, small numbers)
uniform float uBlockScale;   // block edge length in world units

out vec3 vLocalPos;
out vec2 vUV;
out vec3 vNormal;
out float vFaceShade;
out float vAO;
out float vAlpha;
out float vBlockLight;

void main() {
    vLocalPos = aPos * uBlockScale; // world-space position (chunk-local, world units)
    vUV = aUV;
    vNormal = aNormal;
    vFaceShade = aFaceShade;
    vAO = aAO;
    vAlpha = aAlpha;
    vBlockLight = aBlockLight;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

inline constexpr const char* WORLD_FRAG = R"GLSL(
#version 330 core
in vec3 vLocalPos;
in vec2 vUV;
in vec3 vNormal;
in float vFaceShade;
in float vAO;
in float vAlpha;
in float vBlockLight;

uniform sampler2D uAtlas;
uniform vec3 uSunDir;        // world-space direction toward the sun
uniform vec3 uSunColor;      // color * intensity
uniform vec3 uAmbient;       // ambient sky light color
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform vec3 uChunkOffset;
uniform vec3 uBreakRel;       // block-center relative to camera (same space as vLocalPos+uChunkOffset)
uniform float uBreakProgress; // 0 intact .. 1 broken
uniform float uBreakSod;      // 1 = punch holes in sod overlay of the target dirt
uniform vec3 uBreakNrm;       // hit-face normal; only that sod sheet thins
uniform float uBlockScale;
uniform float uCrackReveal;   // 1 = mining overlay
uniform float uCrackFolds;    // corners on each new random line (0 = straight)
uniform vec3 uCrackColor;     // linear 0..1
uniform vec3 uCrackSeed;      // block integer coords
uniform float uCrackShown;    // how many stored hits to draw
uniform float uCrackLenMu0;   // μ = μ0 + μ1 * (hitStep / block max durability)
uniform float uCrackLenMu1;
uniform float uCrackLenSig;
uniform float uCrackWidMu0;
uniform float uCrackWidMu1;
uniform float uCrackWidSig;
uniform float uCrackStep[12]; // per-hit (step / maxDurability)
uniform float uCrackGaussZ;   // |z| cap of the truncated normal
uniform vec4 uCrackEdge;      // back-support on -U +U -V +V
uniform vec4 uCrackCorner;    // back-support on four diagonal neighbors
uniform vec4 uBorderXZ;       // playable minX, maxX, minZ, maxZ
uniform float uRimHalf;       // world units; opaque at this distance past the edge. 0 = off
uniform vec3 uCameraPos;

out vec4 fragColor;

float borderFogAt(vec2 xz) {
    if (uRimHalf <= 0.001) return 0.0;
    float ox = 0.0;
    float oz = 0.0;
    if (xz.x < uBorderXZ.x) ox = uBorderXZ.x - xz.x;
    else if (xz.x > uBorderXZ.y) ox = xz.x - uBorderXZ.y;
    if (xz.y < uBorderXZ.z) oz = uBorderXZ.z - xz.y;
    else if (xz.y > uBorderXZ.w) oz = xz.y - uBorderXZ.w;
    return clamp(max(ox, oz) / uRimHalf, 0.0, 1.0);
}

float crackHash(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float crackGauss(vec3 a, vec3 b) {
    float zCap = max(uCrackGaussZ, 0.01);
    // Keep Box-Muller away from log(0); the radius at u1=exp(-zCap²/2) already
    // reaches |z|=zCap, so anything smaller only feeds the truncated tail.
    float u1Min = exp(-0.5 * zCap * zCap);
    float u1 = clamp(crackHash(a), u1Min, 1.0 - 1e-6);
    float u2 = crackHash(b);
    return clamp(sqrt(-2.0 * log(u1)) * cos(6.2831853 * u2), -zCap, zCap);
}

bool crackBacked(vec2 uv) {
    bool lu = uv.x < 0.0, ru = uv.x > 1.0, lv = uv.y < 0.0, rv = uv.y > 1.0;
    if (!lu && !ru && !lv && !rv) return true;
    if (lu && lv) return uCrackCorner.x > 0.5;
    if (ru && lv) return uCrackCorner.y > 0.5;
    if (lu && rv) return uCrackCorner.z > 0.5;
    if (ru && rv) return uCrackCorner.w > 0.5;
    if (lu) return uCrackEdge.x > 0.5;
    if (ru) return uCrackEdge.y > 0.5;
    if (lv) return uCrackEdge.z > 0.5;
    return uCrackEdge.w > 0.5;
}

// Signed: negative = inside tapered stroke. Both ends pinch to a point.
float crackStroke(vec2 uv, float stepId, vec3 seed, int nSeg) {
    vec3 s = seed + vec3(stepId * 13.17, stepId * 7.91, 2.3);
    vec2 p = mix(vec2(0.14), vec2(0.86), vec2(
        crackHash(s + vec3(1.0, 0.0, 0.0)),
        crackHash(s + vec3(0.0, 1.0, 0.0))));
    float ang = crackHash(s + vec3(0.0, 0.0, 1.0)) * 6.2831853;
    float zL = crackGauss(s + vec3(2.4, 0.7, 1.1), s + vec3(0.3, 5.2, 8.8));
    float zW = crackGauss(s + vec3(9.1, 1.4, 3.3), s + vec3(4.6, 0.2, 7.7));
    float dmg = uCrackStep[int(stepId)];
    float tot = clamp((uCrackLenMu0 + uCrackLenMu1 * dmg) + uCrackLenSig * zL, 0.10, 1.20);
    float wid = clamp((uCrackWidMu0 + uCrackWidMu1 * dmg) + uCrackWidSig * zW, 0.0035, 0.028);
    float seg = tot / float(nSeg);
    float best = 9.0;
    float n = float(nSeg);
    for (int i = 0; i < 9; i++) {
        if (i >= nSeg) break;
        float turn = (crackHash(s + vec3(float(i), 9.3, 1.7)) - 0.5) * 1.6;
        vec2 q = p + vec2(cos(ang), sin(ang)) * seg;
        q = clamp(q, vec2(-1.15), vec2(2.15));
        vec2 pa = uv - p;
        float L = max(dot(q - p, q - p), 1e-8);
        float t = clamp(dot(pa, q - p) / L, 0.0, 1.0);
        float d = length(pa - (q - p) * t);
        float g = (float(i) + t) / n;
        float halfW = wid * 4.0 * g * (1.0 - g);
        best = min(best, d - halfW);
        p = q;
        ang += turn;
    }
    return best;
}

void main() {
    if (uCrackReveal > 0.5) {
        if (!crackBacked(vUV)) discard;
        int nSeg = int(clamp(uCrackFolds, 0.0, 8.0) + 0.5) + 1;
        int shown = int(clamp(uCrackShown, 0.0, 12.0) + 0.5);
        float best = 9.0;
        for (int s = 0; s < 12; s++) {
            if (s >= shown) break;
            best = min(best, crackStroke(vUV, float(s), uCrackSeed, nSeg));
        }
        if (best > 0.0) discard;
        vec3 nrm = normalize(vNormal);
        float diff = max(dot(nrm, uSunDir), 0.0);
        vec3 light = uAmbient + uSunColor * diff + vec3(1.20, 0.62, 0.22) * vBlockLight;
        vec3 col = uCrackColor * light * vFaceShade;
        float dist = length(vLocalPos + uChunkOffset);
        float f = 1.0 - exp(-uFogDensity * dist);
        col = mix(col, uFogColor, clamp(f, 0.0, 1.0));
        vec3 world = uCameraPos + vLocalPos + uChunkOffset;
        col = mix(col, vec3(0.78, 0.79, 0.80), borderFogAt(world.xz));
        fragColor = vec4(col, 0.94);
        return;
    }
    vec4 tex = texture(uAtlas, vUV);
    if (tex.a < 0.1) discard;
    // Sod wear is stored in vAO (remaining coverage 0..1) so thinning survives
    // after the cursor leaves the block. Dirt under the overlay stays drawn.
    float tile = floor(vUV.y * 8.0) * 8.0 + floor(vUV.x * 8.0);
    bool sodTile = tile >= 21.0 && tile <= 24.0;
    if (sodTile && vAO < 0.999) {
        vec2 pix = floor(vUV * vec2(512.0, 512.0));
        float n = fract(sin(dot(pix, vec2(12.9898, 78.233))) * 43758.5453);
        if (n > vAO) discard;
    }
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, uSunDir), 0.0);
    vec3 light = uAmbient + uSunColor * diff + vec3(1.20, 0.62, 0.22) * vBlockLight;
    float ao = sodTile ? 1.0 : vAO;
    vec3 col = tex.rgb * light * vFaceShade * ao;
    float dist = length(vLocalPos + uChunkOffset);
    float f = 1.0 - exp(-uFogDensity * dist);
    col = mix(col, uFogColor, clamp(f, 0.0, 1.0));
    vec3 world = uCameraPos + vLocalPos + uChunkOffset;
    col = mix(col, vec3(0.78, 0.79, 0.80), borderFogAt(world.xz));
    fragColor = vec4(col, tex.a * vAlpha);
}
)GLSL";

inline constexpr const char* SKY_VERT = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;   // fullscreen triangle NDC
out vec3 vDir;
uniform mat4 uInvVP;
void main() {
    gl_Position = vec4(aPos, 1.0, 1.0);
    vec4 w = uInvVP * vec4(aPos, 1.0, 1.0);
    vDir = w.xyz / w.w;
}
)GLSL";

inline constexpr const char* SKY_FRAG = R"GLSL(
#version 330 core
in vec3 vDir;
out vec4 fragColor;

uniform vec3 uSunDir;
uniform vec3 uMoonDir;
uniform vec3 uZenith;
uniform vec3 uHorizon;
uniform vec3 uBelow;
uniform vec3 uSunColor;
uniform vec3 uMoonColor;
uniform float uSunDisc;     // cos(angular radius)
uniform float uMoonDisc;
uniform float uStarAmount;  // 0..1
uniform vec4 uBorderXZ;
uniform float uRimHalf;
uniform vec3 uCameraPos;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float hash3(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }

void main() {
    vec3 d = normalize(vDir);
    float h = d.y;

    vec3 sky = mix(uHorizon, uZenith, clamp(pow(max(h, 0.0), 0.55), 0.0, 1.0));
    if (h < 0.0) sky = mix(uBelow, uHorizon, clamp(1.0 + h * 1.8, 0.0, 1.0));

    // Sun
    float sd = dot(d, uSunDir);
    if (sd > uSunDisc) sky = mix(sky, uSunColor, smoothstep(uSunDisc, uSunDisc + 0.03, sd));
    // Sun glow
    float glow = max(sd - uSunDisc, 0.0);
    sky += uSunColor * pow(glow, 3.0) * 0.6;

    // Moon
    float md = dot(d, uMoonDir);
    if (md > uMoonDisc) sky = mix(sky, uMoonColor, smoothstep(uMoonDisc, uMoonDisc + 0.03, md));

    // Stars (fixed on the world-space sky sphere)
    if (uStarAmount > 0.0001 && d.y > 0.02) {
        vec3 p = d * 18.0;
        vec3 id = floor(p);
        float star = step(0.996, hash3(id));
        star *= uStarAmount;
        star *= smoothstep(0.02, 0.18, d.y);
        sky += vec3(star);
    }

    if (uRimHalf > 0.001) {
        vec2 o = uCameraPos.xz;
        vec2 r = d.xz;
        vec2 bmin = vec2(uBorderXZ.x - uRimHalf, uBorderXZ.z - uRimHalf);
        vec2 bmax = vec2(uBorderXZ.y + uRimHalf, uBorderXZ.w + uRimHalf);
        bool outside = o.x < bmin.x || o.x > bmax.x || o.y < bmin.y || o.y > bmax.y;
        float t = 0.0;
        if (!outside) {
            float tx = 1e9;
            float tz = 1e9;
            if (r.x > 1e-5) tx = (bmax.x - o.x) / r.x;
            else if (r.x < -1e-5) tx = (bmin.x - o.x) / r.x;
            if (r.y > 1e-5) tz = (bmax.y - o.y) / r.y;
            else if (r.y < -1e-5) tz = (bmin.y - o.y) / r.y;
            t = min(tx, tz);
            if (t < 0.0) t = 1e9;
        }
        float amt = clamp(1.0 - t / 360.0, 0.0, 1.0);
        float elev = smoothstep(0.62, 0.08, d.y);
        sky = mix(sky, vec3(0.78, 0.79, 0.80), amt * elev);
    }

    fragColor = vec4(sky, 1.0);
}
)GLSL";

inline constexpr const char* FLAT_VERT = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

inline constexpr const char* FLAT_FRAG = R"GLSL(
#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)GLSL";

inline constexpr const char* PARTICLE_VERT = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

inline constexpr const char* PARTICLE_FRAG = R"GLSL(
#version 330 core
in vec2 vUV;
uniform vec4 uColor;
uniform float uSoftness;
uniform float uRing;
out vec4 fragColor;
void main() {
    float d = length(vUV * 2.0 - 1.0);
    float soft = clamp(uSoftness, 0.01, 0.48);
    float alpha = 1.0 - smoothstep(1.0 - soft, 1.0, d);
    if (uRing > 0.0)
        alpha *= smoothstep(uRing, min(0.99, uRing + soft), d);
    if (alpha <= 0.001) discard;
    fragColor = vec4(uColor.rgb, uColor.a * alpha);
}
)GLSL";

inline constexpr const char* HUM_VERT = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uMVP;
out vec4 vColor;
out vec3 vPos;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
    vPos = aPos;
}
)GLSL";

inline constexpr const char* HUM_FRAG = R"GLSL(
#version 330 core
in vec4 vColor;
in vec3 vPos;
out vec4 fragColor;
uniform float uLit;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform vec3 uFogColor;
uniform float uFogDensity;
void main() {
    if (uLit < 0.5) { fragColor = vColor; return; }
    vec3 n = normalize(cross(dFdx(vPos), dFdy(vPos)));
    float diff = max(dot(n, normalize(uSunDir)), 0.0);
    vec3 col = vColor.rgb * (uAmbient + uSunColor * diff);
    float f = 1.0 - exp(-uFogDensity * length(vPos));
    col = mix(col, uFogColor, clamp(f, 0.0, 1.0));
    fragColor = vec4(col, vColor.a);
}
)GLSL";

inline constexpr const char* HUM_TEX_VERT = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
uniform mat4 uMVP;
out vec4 vColor;
out vec2 vUV;
out vec3 vPos;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
    vUV = aUV;
    vPos = aPos;
}
)GLSL";

inline constexpr const char* HUM_TEX_FRAG = R"GLSL(
#version 330 core
in vec4 vColor;
in vec2 vUV;
in vec3 vPos;
uniform sampler2D uAtlas;
uniform float uLit;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform vec3 uFogColor;
uniform float uFogDensity;
out vec4 fragColor;
void main() {
    vec4 t = texture(uAtlas, vUV);
    if (t.a < 0.1) discard;
    vec3 col = vColor.rgb * t.rgb;
    float a = vColor.a * t.a;
    if (uLit >= 0.5) {
        vec3 n = normalize(cross(dFdx(vPos), dFdy(vPos)));
        float diff = max(dot(n, normalize(uSunDir)), 0.0);
        col *= (uAmbient + uSunColor * diff);
        float f = 1.0 - exp(-uFogDensity * length(vPos));
        col = mix(col, uFogColor, clamp(f, 0.0, 1.0));
    }
    fragColor = vec4(col, a);
}
)GLSL";

inline constexpr const char* UI_VERT = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;   // pixel coordinates, origin top-left, y down
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform vec2 uScreenSize;
out vec2 vUV;
out vec4 vColor;
void main() {
    vec2 ndc = vec2(aPos.x / uScreenSize.x * 2.0 - 1.0, 1.0 - aPos.y / uScreenSize.y * 2.0);
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)GLSL";

inline constexpr const char* UI_TEX_FRAG = R"GLSL(
#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    vec4 t = texture(uTex, vUV);
    fragColor = vec4(t.rgb * vColor.rgb, t.a * vColor.a);
}
)GLSL";

inline constexpr const char* UI_TEXT_FRAG = R"GLSL(
#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    float a = texture(uTex, vUV).r;   // glyph coverage stored in red channel
    fragColor = vec4(vColor.rgb, a * vColor.a);
}
)GLSL";

} // namespace shaders
