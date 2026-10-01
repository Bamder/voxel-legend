#include "renderer.hpp"
#include "shaders.hpp"
#include "textures.hpp"
#include "player_draw.hpp"
#include "block_geo.hpp"
#include "../material/registry.hpp"
#include "../material/model_mesh.hpp"
#include "../world/asset_pack.hpp"
#include "../world/player_model.hpp"
#include "../world/player_skin.hpp"
#include "../world/hair_voxels.hpp"
#include "../world/animation.hpp"
#include "../world/hold_bind.hpp"
#include "../plugin/plugin.hpp"
#include "../world/loot.hpp"
#include "../world/drop_geom.hpp"
#include "../world/wear.hpp"
#include "../world/matchmap.hpp"
#include "../world/structure.hpp"
#include "../world/ritual.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

using namespace gl;

namespace {

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = gl::CreateShader(type);
    gl::ShaderSource(s, 1, &src, nullptr);
    gl::CompileShader(s);
    GLint ok = GL_FALSE;
    gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei len = 0;
        gl::GetShaderInfoLog(s, sizeof(log), &len, log);
        fprintf(stderr, "[shader] compile error:\n%s\n", log);
        gl::DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkProgram(const char* vs, const char* fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) gl::DeleteShader(v);
        if (f) gl::DeleteShader(f);
        return 0;
    }
    GLuint p = gl::CreateProgram();
    gl::AttachShader(p, v);
    gl::AttachShader(p, f);
    gl::LinkProgram(p);
    GLint ok = GL_FALSE;
    gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei len = 0;
        gl::GetProgramInfoLog(p, sizeof(log), &len, log);
        fprintf(stderr, "[shader] link error:\n%s\n", log);
        gl::DeleteProgram(p);
        p = 0;
    }
    gl::DeleteShader(v);
    gl::DeleteShader(f);
    return p;
}

GLuint makeTexture(const uint8_t* data, int w, int h, bool mipmap, bool linear) {
    GLuint t = 0;
    gl::GenTextures(1, &t);
    gl::BindTexture(GL_TEXTURE_2D, t);
    GLint minF = linear ? (mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR)
                        : (mipmap ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST);
    GLint magF = linear ? GL_LINEAR : GL_NEAREST;
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magF);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    if (mipmap) gl::GenerateMipmap(GL_TEXTURE_2D);
    gl::BindTexture(GL_TEXTURE_2D, 0);
    return t;
}

void setVertexAttribs() {
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, px));
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, nx));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, faceShade));
    gl::EnableVertexAttribArray(3);
    gl::VertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, ao));
    gl::EnableVertexAttribArray(4);
    gl::VertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, alpha));
    gl::EnableVertexAttribArray(5);
    gl::VertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, blockLight));
    gl::EnableVertexAttribArray(6);
}

Vec3 lerpVec(const Vec3& a, const Vec3& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

float smoothstep01(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void splitHotbarHudX(float screenW, float slot, float gap, float margin, float& leftX, float& rightX) {
    float barW = cfg::HAND_SLOTS * slot + (cfg::HAND_SLOTS - 1) * gap;
    leftX = margin;
    rightX = screenW - margin - barW;
}

float splitHotbarSlotX(int i, float leftX, float rightX, float slot, float gap) {
    int local = (i < cfg::HAND_SLOTS) ? i : (i - cfg::HAND_SLOTS);
    return ((i < cfg::HAND_SLOTS) ? leftX : rightX) + local * (slot + gap);
}

bool splitHotbarSelected(int i, const UIState& ui) {
    if (i < cfg::HAND_SLOTS) return i == ui.selectedLeft;
    return (i - cfg::HAND_SLOTS) == ui.selectedRight;
}

bool crackBackSolid(const World& world, int phys, int x, int y, int z) {
    uint8_t b = (phys >= 0) ? world.getPhysBlock(phys, x, y, z) : world.getBlock(x, y, z);
    return isSolid(b);
}

void crackFaceUVAxes(const geo::FaceDef& F, int du[3], int dv[3]) {
    int i00 = 0, i10 = 0, i01 = 0;
    for (int c = 0; c < 4; c++) {
        if (F.t[c][0] < 0.5f && F.t[c][1] < 0.5f) i00 = c;
        if (F.t[c][0] > 0.5f && F.t[c][1] < 0.5f) i10 = c;
        if (F.t[c][0] < 0.5f && F.t[c][1] > 0.5f) i01 = c;
    }
    for (int k = 0; k < 3; k++) {
        du[k] = (int)std::lround(F.p[i10][k] - F.p[i00][k]);
        dv[k] = (int)std::lround(F.p[i01][k] - F.p[i00][k]);
    }
}

} // namespace

bool Renderer::init(int w, int h) {
    scrW = w;
    scrH = h;

    progWorld = linkProgram(shaders::WORLD_VERT, shaders::WORLD_FRAG);
    progSky = linkProgram(shaders::SKY_VERT, shaders::SKY_FRAG);
    progFlat = linkProgram(shaders::FLAT_VERT, shaders::FLAT_FRAG);
    progParticle = linkProgram(shaders::PARTICLE_VERT, shaders::PARTICLE_FRAG);
    progUI = linkProgram(shaders::UI_VERT, shaders::UI_TEX_FRAG);
    progUIText = linkProgram(shaders::UI_VERT, shaders::UI_TEXT_FRAG);
    progHum = linkProgram(shaders::HUM_VERT, shaders::HUM_FRAG);
    progHumTex = linkProgram(shaders::HUM_TEX_VERT, shaders::HUM_TEX_FRAG);
    if (!progWorld || !progSky || !progFlat || !progParticle || !progUI || !progUIText || !progHum || !progHumTex) return false;

    gl::UseProgram(progWorld);
    uMVP = gl::GetUniformLocation(progWorld, "uMVP");
    uChunkOffset = gl::GetUniformLocation(progWorld, "uChunkOffset");
    uAtlas = gl::GetUniformLocation(progWorld, "uAtlas");
    uSunDir = gl::GetUniformLocation(progWorld, "uSunDir");
    uSunColor = gl::GetUniformLocation(progWorld, "uSunColor");
    uAmbient = gl::GetUniformLocation(progWorld, "uAmbient");
    uFogColor = gl::GetUniformLocation(progWorld, "uFogColor");
    uFogDensity = gl::GetUniformLocation(progWorld, "uFogDensity");
    uBorderXZ = gl::GetUniformLocation(progWorld, "uBorderXZ");
    uRimHalf = gl::GetUniformLocation(progWorld, "uRimHalf");
    uCameraPos = gl::GetUniformLocation(progWorld, "uCameraPos");
    uBlockScale = gl::GetUniformLocation(progWorld, "uBlockScale");
    uBreakRel = gl::GetUniformLocation(progWorld, "uBreakRel");
    uBreakProgress = gl::GetUniformLocation(progWorld, "uBreakProgress");
    uBreakSod = gl::GetUniformLocation(progWorld, "uBreakSod");
    uBreakNrm = gl::GetUniformLocation(progWorld, "uBreakNrm");
    uCrackReveal = gl::GetUniformLocation(progWorld, "uCrackReveal");
    uCrackFolds = gl::GetUniformLocation(progWorld, "uCrackFolds");
    uCrackColor = gl::GetUniformLocation(progWorld, "uCrackColor");
    uCrackSeed = gl::GetUniformLocation(progWorld, "uCrackSeed");
    uCrackShown = gl::GetUniformLocation(progWorld, "uCrackShown");
    uCrackLenMu0 = gl::GetUniformLocation(progWorld, "uCrackLenMu0");
    uCrackLenMu1 = gl::GetUniformLocation(progWorld, "uCrackLenMu1");
    uCrackLenSig = gl::GetUniformLocation(progWorld, "uCrackLenSig");
    uCrackWidMu0 = gl::GetUniformLocation(progWorld, "uCrackWidMu0");
    uCrackWidMu1 = gl::GetUniformLocation(progWorld, "uCrackWidMu1");
    uCrackWidSig = gl::GetUniformLocation(progWorld, "uCrackWidSig");
    uCrackStep = gl::GetUniformLocation(progWorld, "uCrackStep");
    uCrackGaussZ = gl::GetUniformLocation(progWorld, "uCrackGaussZ");
    uCrackEdge = gl::GetUniformLocation(progWorld, "uCrackEdge");
    uCrackCorner = gl::GetUniformLocation(progWorld, "uCrackCorner");
    gl::Uniform1i(uAtlas, 0);
    gl::Uniform1f(uBlockScale, cfg::BLOCK_SCALE);
    gl::Uniform3f(uBreakRel, 0, 0, 0);
    gl::Uniform1f(uBreakProgress, 0);
    gl::Uniform1f(uBreakSod, 0);
    gl::Uniform3f(uBreakNrm, 0, 1, 0);
    gl::Uniform1f(uCrackReveal, 0);
    gl::Uniform1f(uCrackFolds, (float)loot::kDefaultCrackFolds);
    gl::Uniform3f(uCrackColor, loot::kDefaultCrackR / 255.0f, loot::kDefaultCrackG / 255.0f, loot::kDefaultCrackB / 255.0f);
    gl::Uniform3f(uCrackSeed, 0, 0, 0);
    gl::Uniform1f(uCrackShown, 0);
    gl::Uniform1f(uCrackLenMu0, loot::kCrackLenMu0);
    gl::Uniform1f(uCrackLenMu1, loot::kCrackLenMu1);
    gl::Uniform1f(uCrackLenSig, loot::kCrackLenSigma);
    gl::Uniform1f(uCrackWidMu0, loot::kCrackWidMu0);
    gl::Uniform1f(uCrackWidMu1, loot::kCrackWidMu1);
    gl::Uniform1f(uCrackWidSig, loot::kCrackWidSigma);
    gl::Uniform1f(uCrackGaussZ, loot::kCrackGaussZAbs);
    gl::Uniform4f(uCrackEdge, 0, 0, 0, 0);
    gl::Uniform4f(uCrackCorner, 0, 0, 0, 0);

    gl::UseProgram(progSky);
    uInvVP = gl::GetUniformLocation(progSky, "uInvVP");
    uSkySunDir = gl::GetUniformLocation(progSky, "uSunDir");
    uSkyMoonDir = gl::GetUniformLocation(progSky, "uMoonDir");
    uZenith = gl::GetUniformLocation(progSky, "uZenith");
    uHorizon = gl::GetUniformLocation(progSky, "uHorizon");
    uBelow = gl::GetUniformLocation(progSky, "uBelow");
    uSkySunColor = gl::GetUniformLocation(progSky, "uSunColor");
    uSkyMoonColor = gl::GetUniformLocation(progSky, "uMoonColor");
    uSunDisc = gl::GetUniformLocation(progSky, "uSunDisc");
    uMoonDisc = gl::GetUniformLocation(progSky, "uMoonDisc");
    uStarAmount = gl::GetUniformLocation(progSky, "uStarAmount");
    uSkyBorderXZ = gl::GetUniformLocation(progSky, "uBorderXZ");
    uSkyRimHalf = gl::GetUniformLocation(progSky, "uRimHalf");
    uSkyCamera = gl::GetUniformLocation(progSky, "uCameraPos");

    gl::UseProgram(progFlat);
    uFlatMVP = gl::GetUniformLocation(progFlat, "uMVP");
    uFlatColor = gl::GetUniformLocation(progFlat, "uColor");

    gl::UseProgram(progParticle);
    uParticleMVP = gl::GetUniformLocation(progParticle, "uMVP");
    uParticleColor = gl::GetUniformLocation(progParticle, "uColor");
    uParticleSoftness = gl::GetUniformLocation(progParticle, "uSoftness");
    uParticleRing = gl::GetUniformLocation(progParticle, "uRing");

    gl::UseProgram(progHum);
    uHumMVP = gl::GetUniformLocation(progHum, "uMVP");
    uHumLit = gl::GetUniformLocation(progHum, "uLit");
    uHumSunDir = gl::GetUniformLocation(progHum, "uSunDir");
    uHumSunColor = gl::GetUniformLocation(progHum, "uSunColor");
    uHumAmbient = gl::GetUniformLocation(progHum, "uAmbient");
    uHumFogColor = gl::GetUniformLocation(progHum, "uFogColor");
    uHumFogDensity = gl::GetUniformLocation(progHum, "uFogDensity");
    gl::UseProgram(progHumTex);
    uHumTexMVP = gl::GetUniformLocation(progHumTex, "uMVP");
    uHumTexAtlas = gl::GetUniformLocation(progHumTex, "uAtlas");
    uHumTexLit = gl::GetUniformLocation(progHumTex, "uLit");
    uHumTexSunDir = gl::GetUniformLocation(progHumTex, "uSunDir");
    uHumTexSunColor = gl::GetUniformLocation(progHumTex, "uSunColor");
    uHumTexAmbient = gl::GetUniformLocation(progHumTex, "uAmbient");
    uHumTexFogColor = gl::GetUniformLocation(progHumTex, "uFogColor");
    uHumTexFogDensity = gl::GetUniformLocation(progHumTex, "uFogDensity");

    gl::UseProgram(progUI);
    uUIScreen = gl::GetUniformLocation(progUI, "uScreenSize");
    uUITex = gl::GetUniformLocation(progUI, "uTex");
    gl::UseProgram(progUIText);
    uUITextScreen = gl::GetUniformLocation(progUIText, "uScreenSize");
    uUITextTex = gl::GetUniformLocation(progUIText, "uTex");
    gl::UseProgram(0);

    // Block atlas. Every tile is image-driven: loaded material images overwrite
    // the procedural fallback tiles.
    mat::initMaterials();
    std::vector<uint8_t> atlas;
    tex::generateAtlas(atlas);
    for (int t = 0; t < TEX_COUNT; t++) {
        if (mat::g_tileImages[t].ok()) {
            tex::overwriteTile(t, mat::g_tileImages[t].rgba.data(), atlas);
        }
    }
    atlasTex = makeTexture(atlas.data(), tex::ATLAS_W, tex::ATLAS_H, true, false);

    uint8_t wpx[4] = { 255, 255, 255, 255 };
    whiteTex = makeTexture(wpx, 1, 1, false, false);
    {
        std::string camPath = pack::join(pack::root(), "ui/spectator_camera.png");
        mat::Image cam = mat::loadPNG(camPath.c_str());
        if (cam.ok())
            cameraIconTex = makeTexture(cam.rgba.data(), cam.w, cam.h, false, false);
    }

    auto uploadGen = [&](const char* path, const std::vector<uint8_t>& rgba, int w, int h) -> unsigned {
        std::filesystem::create_directories(pack::entitiesDir());
        if (!pm::fileExists(path)) mat::savePNG(path, w, h, rgba.data());
        mat::Image img = mat::loadPNG(path);
        if (!img.ok()) {
            img.w = w;
            img.h = h;
            img.rgba = rgba;
        }
        if (!img.ok()) return 0;
        return makeTexture(img.rgba.data(), img.w, img.h, false, false);
    };
    {
        pm::EntityFile ef = pm::buildPlayerEntity();
        std::string skinStem = ef.skin.empty() ? "player" : ef.skin;
        int sw = ef.skinW > 0 ? ef.skinW : pm::kSkinW;
        int sh = ef.skinH > 0 ? ef.skinH : pm::kSkinH;
        std::vector<uint8_t> rgba;
        tex::generatePlayerSkin(rgba);
        std::string skinP = pack::entityPng(skinStem);
        skinTex = uploadGen(skinP.c_str(), rgba, sw, sh);

        auto putGarment = [&](uint8_t item) {
            const wear::GarmentAsset& g = wear::garmentAsset(item);
            if (g.parts.empty() || g.sheetW <= 0 || g.sheetH <= 0 || g.png.empty()) return;
            std::vector<uint8_t> sheet;
            pm::fillSheetRgba(sheet, g.sheetW, g.sheetH, g.parts);
            garmentTex[item] = uploadGen(g.png.c_str(), sheet, g.sheetW, g.sheetH);
        };
        putGarment(SHIRT);
        putGarment(SHORTS);
        putGarment(SHOES);

        auto putOverlay = [&](const std::string& name, int w, int h, const std::string& kind) {
            if (name.empty() || overlayTex.count(name)) return;
            std::string path = pack::resolvePng(name);
            mat::Image img = mat::loadPNG(path.c_str());
            if (img.ok()) {
                overlayTex[name] = makeTexture(img.rgba.data(), img.w, img.h, false, false);
                return;
            }
            bool eye = kind == "eye" || pm::isEyeTex(name);
            bool lid = kind == "eyelid" || pm::isEyelidTex(name);
            bool mouth = kind == "mouth" || pm::isMouthTex(name);
            if (!eye && !lid && !mouth) return;
            int ow = w, oh = h;
            if (eye) { tex::generatePlayerEye(rgba); ow = pm::kEyeSize; oh = pm::kEyeSize; }
            else if (lid) { tex::generatePlayerEyelid(rgba); ow = pm::kEyelidSize; oh = pm::kEyelidSize; }
            else { tex::generatePlayerMouth(rgba, pm::mouthIndex(name)); ow = pm::kMouthW; oh = pm::kMouthH; }
            if (ow <= 0) ow = 8;
            if (oh <= 0) oh = 8;
            std::string dst = pack::entityPng(name);
            overlayTex[name] = uploadGen(dst.c_str(), rgba, ow, oh);
        };
        for (const pm::OverlaySpec& o : ef.overlays) putOverlay(o.name, o.w, o.h, o.kind);
        for (const std::string& n : ef.mouthTex) putOverlay(n, pm::kMouthW, pm::kMouthH, "mouth");
        for (const pm::Part& p : ef.parts) {
            if (!p.tex.empty()) putOverlay(p.tex, 0, 0, p.kind);
        }
        putOverlay("leaf", 0, 0, "leaf");
    }
    std::filesystem::create_directories(pack::extrasDir());
    for (auto& ent : std::filesystem::directory_iterator(pack::extrasDir())) {
        if (!ent.is_regular_file()) continue;
        auto ext = ent.path().extension().string();
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (ext != ".png") continue;
        mat::Image img = mat::loadPNG(ent.path().string().c_str());
        if (!img.ok()) continue;
        extraMatTex[ent.path().stem().string()] = makeTexture(img.rgba.data(), img.w, img.h, false, false);
    }

    // Fullscreen sky triangle.
    float skyVerts[6] = { -1, -1, 3, -1, -1, 3 };
    gl::GenVertexArrays(1, &skyVAO);
    gl::GenBuffers(1, &skyVBO);
    gl::BindVertexArray(skyVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, skyVBO);
    gl::BufferData(GL_ARRAY_BUFFER, sizeof(skyVerts), skyVerts, GL_STATIC_DRAW);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    gl::EnableVertexAttribArray(0);

    // Unit-cube wireframe for the block highlight.
    const float c0 = 0.0f, c1 = 1.0f;
    const float cubeVerts[24][3] = {
        {c0,c0,c0},{c1,c0,c0}, {c1,c0,c0},{c1,c0,c1}, {c1,c0,c1},{c0,c0,c1}, {c0,c0,c1},{c0,c0,c0},
        {c0,c1,c0},{c1,c1,c0}, {c1,c1,c0},{c1,c1,c1}, {c1,c1,c1},{c0,c1,c1}, {c0,c1,c1},{c0,c1,c0},
        {c0,c0,c0},{c0,c1,c0}, {c1,c0,c0},{c1,c1,c0}, {c0,c0,c1},{c0,c1,c1}, {c1,c0,c1},{c1,c1,c1},
    };
    gl::GenVertexArrays(1, &outlineVAO);
    gl::GenBuffers(1, &outlineVBO);
    gl::BindVertexArray(outlineVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, outlineVBO);
    gl::BufferData(GL_ARRAY_BUFFER, sizeof(cubeVerts), cubeVerts, GL_STATIC_DRAW);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    gl::EnableVertexAttribArray(0);

    // Camera-facing particle quad. The radial shader turns this into a soft
    // glow, spark or ring without requiring a texture atlas entry.
    const float particleVerts[6][5] = {
        {-.5f,-.5f,0, 0,0}, {.5f,-.5f,0, 1,0}, {.5f,.5f,0, 1,1},
        {-.5f,-.5f,0, 0,0}, {.5f,.5f,0, 1,1}, {-.5f,.5f,0, 0,1},
    };
    gl::GenVertexArrays(1, &particleVAO);
    gl::GenBuffers(1, &particleVBO);
    gl::BindVertexArray(particleVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, particleVBO);
    gl::BufferData(GL_ARRAY_BUFFER, sizeof(particleVerts), particleVerts, GL_STATIC_DRAW);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    gl::EnableVertexAttribArray(0);
    gl::EnableVertexAttribArray(1);

    gl::GenVertexArrays(1, &fallVAO);
    gl::GenBuffers(1, &fallVBO);
    gl::BindVertexArray(fallVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, fallVBO);
    setVertexAttribs();

    // Humidity-visualization mesh (position + RGBA, rebuilt per frame).
    gl::GenVertexArrays(1, &humVAO);
    gl::GenBuffers(1, &humVBO);
    gl::BindVertexArray(humVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, humVBO);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    gl::EnableVertexAttribArray(1);

    gl::GenVertexArrays(1, &humTexVAO);
    gl::GenBuffers(1, &humTexVBO);
    gl::BindVertexArray(humTexVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, humTexVBO);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)0);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(3 * sizeof(float)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(7 * sizeof(float)));
    gl::EnableVertexAttribArray(2);
    gl::GenVertexArrays(1, &m_batch.vao);
    gl::GenBuffers(1, &m_batch.vbo);
    gl::BindVertexArray(m_batch.vao);
    gl::BindBuffer(GL_ARRAY_BUFFER, m_batch.vbo);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(2 * sizeof(float)));
    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(4 * sizeof(float)));
    gl::EnableVertexAttribArray(0);
    gl::EnableVertexAttribArray(1);
    gl::EnableVertexAttribArray(2);

    gl::Enable(GL_DEPTH_TEST);
    gl::DepthFunc(GL_LEQUAL);
    gl::Enable(GL_CULL_FACE);
    gl::FrontFace(GL_CCW);
    gl::CullFace(GL_BACK);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl::ClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    if (gl::WglSwapIntervalEXT) gl::WglSwapIntervalEXT(1);

    gl::BindVertexArray(0);
    gl::BindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

unsigned int Renderer::renderTextTexture(const std::string& utf8, int& outW, int& outH) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(wlen > 0 ? wlen : 1, L'\0');
    if (wlen > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wide[0], wlen);
    } else {
        wide = L"?";
    }

    HDC hdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(hdc);
    HFONT hfont = CreateFontW(-24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(mem, hfont);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));

    SIZE sz{ 0, 0 };
    GetTextExtentPoint32W(mem, wide.c_str(), (int)wide.size(), &sz);
    const int pad = 4;
    int w = sz.cx + pad * 2;
    int h = sz.cy + pad * 2;
    if (w < 2) w = 2;
    if (h < 2) h = 2;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    unsigned int tex = 0;
    if (bmp && bits) {
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        std::memset(bits, 0, (size_t)w * h * 4);
        TextOutW(mem, pad, pad, wide.c_str(), (int)wide.size());
        tex = makeTexture((const uint8_t*)bits, w, h, false, true);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
    }
    SelectObject(mem, oldFont);
    DeleteObject(hfont);
    DeleteDC(mem);
    ReleaseDC(nullptr, hdc);
    outW = w;
    outH = h;
    return tex;
}

void Renderer::shutdown() {
    for (auto& kv : m_chunkGL) destroyChunkGL(kv.second);
    m_chunkGL.clear();
    if (atlasTex) gl::DeleteTextures(1, &atlasTex);
    if (whiteTex) gl::DeleteTextures(1, &whiteTex);
    if (cameraIconTex) gl::DeleteTextures(1, &cameraIconTex);
    if (deployTex) gl::DeleteTextures(1, &deployTex);
    if (skinTex) gl::DeleteTextures(1, &skinTex);
    for (auto& kv : overlayTex) if (kv.second) gl::DeleteTextures(1, &kv.second);
    overlayTex.clear();
    for (auto& kv : extraMatTex) if (kv.second) gl::DeleteTextures(1, &kv.second);
    extraMatTex.clear();
    for (auto& kv : m_textCache) gl::DeleteTextures(1, &kv.second.tex);
    m_textCache.clear();
    if (skyVAO) gl::DeleteVertexArrays(1, &skyVAO);
    if (skyVBO) gl::DeleteBuffers(1, &skyVBO);
    if (outlineVAO) gl::DeleteVertexArrays(1, &outlineVAO);
    if (outlineVBO) gl::DeleteBuffers(1, &outlineVBO);
    if (particleVAO) gl::DeleteVertexArrays(1, &particleVAO);
    if (particleVBO) gl::DeleteBuffers(1, &particleVBO);
    if (fallVAO) gl::DeleteVertexArrays(1, &fallVAO);
    if (fallVBO) gl::DeleteBuffers(1, &fallVBO);
    if (humVAO) gl::DeleteVertexArrays(1, &humVAO);
    if (humVBO) gl::DeleteBuffers(1, &humVBO);
    if (humTexVAO) gl::DeleteVertexArrays(1, &humTexVAO);
    if (humTexVBO) gl::DeleteBuffers(1, &humTexVBO);
    if (m_batch.vao) gl::DeleteVertexArrays(1, &m_batch.vao);
    if (m_batch.vbo) gl::DeleteBuffers(1, &m_batch.vbo);
    if (progWorld) gl::DeleteProgram(progWorld);
    if (progSky) gl::DeleteProgram(progSky);
    if (progFlat) gl::DeleteProgram(progFlat);
    if (progParticle) gl::DeleteProgram(progParticle);
    if (progUI) gl::DeleteProgram(progUI);
    if (progUIText) gl::DeleteProgram(progUIText);
    if (progHum) gl::DeleteProgram(progHum);
    if (progHumTex) gl::DeleteProgram(progHumTex);
}

void Renderer::setScreenSize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    scrW = w;
    scrH = h;
}

void Renderer::uploadChunk(ChunkGL& cg, const World::Chunk& ch) {
    if (!cg.created) {
        gl::GenVertexArrays(1, &cg.vaoO);
        gl::GenBuffers(1, &cg.vboO);
        gl::GenVertexArrays(1, &cg.vaoT);
        gl::GenBuffers(1, &cg.vboT);
        cg.created = true;
    }
    gl::BindVertexArray(cg.vaoO);
    gl::BindBuffer(GL_ARRAY_BUFFER, cg.vboO);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(ch.meshOpaque.size() * sizeof(Vertex)),
                   ch.meshOpaque.empty() ? nullptr : ch.meshOpaque.data(), GL_STATIC_DRAW);
    setVertexAttribs();
    gl::BindVertexArray(cg.vaoT);
    gl::BindBuffer(GL_ARRAY_BUFFER, cg.vboT);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(ch.meshTransparent.size() * sizeof(Vertex)),
                   ch.meshTransparent.empty() ? nullptr : ch.meshTransparent.data(), GL_STATIC_DRAW);
    setVertexAttribs();
    gl::BindVertexArray(0);
    gl::BindBuffer(GL_ARRAY_BUFFER, 0);
    cg.opaqueCount = (int)ch.meshOpaque.size();
    cg.transparentCount = (int)ch.meshTransparent.size();
}

void Renderer::destroyChunkGL(ChunkGL& cg) {
    if (!cg.created) return;
    gl::DeleteVertexArrays(1, &cg.vaoO);
    gl::DeleteBuffers(1, &cg.vboO);
    gl::DeleteVertexArrays(1, &cg.vaoT);
    gl::DeleteBuffers(1, &cg.vboT);
    cg.created = false;
}

void Renderer::sync(const World& world) {
    for (const auto& [key, ch] : world.chunks()) {
        ChunkGL& cg = m_chunkGL[key];
        if (ch.hasMesh && !ch.uploaded) {
            uploadChunk(cg, ch);
            const_cast<World::Chunk&>(ch).uploaded = true;
        }
    }
    for (auto it = m_chunkGL.begin(); it != m_chunkGL.end();) {
        if (world.chunks().find(it->first) == world.chunks().end()) {
            destroyChunkGL(it->second);
            it = m_chunkGL.erase(it);
        } else {
            ++it;
        }
    }
}

void Renderer::computeSky(float timeOfDay, Sky& s) {
    const float PI = 3.14159265358979f;
    float t = timeOfDay / (float)cfg::TICKS_PER_DAY;
    // 0:00 = midnight (sun at nadir), 6:00 = sunrise, 12:00 = noon, 18:00 = sunset.
    float theta = t * 2.0f * PI - PI * 0.5f;
    Vec3 sd{ std::cos(theta), std::sin(theta), 0.35f };
    s.sunDir = sd.normalized();
    s.moonDir = Vec3{ -s.sunDir.x, -s.sunDir.y, s.sunDir.z }.normalized();
    float sunH = s.sunDir.y;
    float day = smoothstep01(-0.12f, 0.12f, sunH);
    float sunset = clampf(1.0f - std::fabs(sunH) / 0.28f, 0.0f, 1.0f);

    Vec3 zenithDay{ 0.25f, 0.5f, 0.95f }, zenithNight{ 0.02f, 0.03f, 0.10f };
    Vec3 horizonDay{ 0.75f, 0.82f, 0.92f }, horizonNight{ 0.06f, 0.08f, 0.16f };
    Vec3 belowDay{ 0.55f, 0.60f, 0.68f }, belowNight{ 0.02f, 0.03f, 0.06f };

    s.zenith = lerpVec(zenithNight, zenithDay, day);
    s.horizon = lerpVec(horizonNight, horizonDay, day);
    s.below = lerpVec(belowNight, belowDay, day);

    Vec3 sunsetCol{ 1.0f, 0.45f, 0.20f };
    s.horizon = lerpVec(s.horizon, sunsetCol, sunset * 0.6f * (0.3f + 0.7f * day));

    Vec3 sunLow{ 1.0f, 0.55f, 0.30f }, sunHigh{ 1.0f, 1.0f, 0.95f };
    Vec3 sunBase = lerpVec(sunLow, sunHigh, smoothstep01(0.0f, 0.35f, sunH));
    float sunIntensity = clampf(sunH, 0.0f, 1.0f);
    s.sunColor = sunBase * (0.22f + 0.78f * sunIntensity);

    Vec3 ambDay{ 0.45f, 0.50f, 0.62f }, ambNight{ 0.10f, 0.11f, 0.18f };
    s.ambient = lerpVec(ambNight, ambDay, day);
    s.fogColor = s.horizon;

    s.moonColor = { 0.85f, 0.87f, 0.95f };
    s.sunDisc = 0.9985f;
    s.moonDisc = 0.9992f;
    s.starAmount = 1.0f - day;
}

void Renderer::reloadPlayerAssets() {
    anim::reloadPlayerModel();
    pm::EntityFile ef = pm::loadEntity(pack::entityModel("player").c_str());
    pm::fillEntityDefaults(ef);
    std::string skinStem = ef.skin.empty() ? "player" : ef.skin;
    mat::Image img = mat::loadPNG(pack::entityPng(skinStem).c_str());
    if (img.ok()) {
        if (skinTex) gl::DeleteTextures(1, &skinTex);
        skinTex = makeTexture(img.rgba.data(), img.w, img.h, false, false);
    }
    for (const pm::OverlaySpec& o : ef.overlays) {
        if (o.name.empty()) continue;
        mat::Image ov = mat::loadPNG(pack::resolvePng(o.name).c_str());
        if (!ov.ok()) continue;
        auto it = overlayTex.find(o.name);
        if (it != overlayTex.end() && it->second) gl::DeleteTextures(1, &it->second);
        overlayTex[o.name] = makeTexture(ov.rgba.data(), ov.w, ov.h, false, false);
    }
}

void Renderer::drawMenuPortrait(const World& world, float timeOfDay, UIState& ui) {
    float split = std::max(420.0f, scrW * 0.42f);
    if (split > (float)scrW - 160.0f) split = (float)scrW * 0.55f;
    ui.portraitX = split;
    gl::Viewport(0, 0, scrW, scrH);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Sky sky;
    computeSky(timeOfDay, sky);
    Vec3 feet = ui.menuFeet;
    Vec3 cam = feet + Vec3{ 0.20f, 1.62f, 1.50f };
    Vec3 at = feet + Vec3{ 0.0f, 1.52f, 0.0f };
    Vec3 look = at - cam;
    float ll = look.length();
    if (ll > 1e-4f) look = look / ll;
    else look = { 0.0f, -0.15f, -1.0f };
    float aspect = (float)scrW / (float)std::max(scrH, 1);
    Mat4 proj = Mat4::perspective(42.0f, aspect, 0.05f, 80.0f);
    // Keep the character on the right so the wider view fills the left side behind the scrim.
    float shift = split / (float)std::max(scrW, 1);
    proj.m[8] = -shift;
    Mat4 view = Mat4::lookAt(Vec3{ 0, 0, 0 }, look, Vec3{ 0, 1, 0 });
    Mat4 worldVP = proj * view;
    drawSky(sky, inverse(worldVP));
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_TRUE);
    drawWorld(world, cam, worldVP, sky, Vec3{ 0, 0, 0 }, 0.0f, 0.0f, Vec3{ 0, 1, 0 },
              cfg::FOG_DENSITY, sky.fogColor);

    const anim::Clip& idle = anim::playerClips().idle;
    static float idleClock = 0.0f;
    float rdt = (ui.fps > 1.0f) ? (1.0f / ui.fps) : (1.0f / 60.0f);
    float fps = (idle.fps > 0.1f) ? idle.fps : 8.0f;
    idleClock += rdt * fps;
    float L = (idle.length > 0) ? (float)idle.length : 20.0f;
    while (idleClock >= L) idleClock -= L;
    // Face the viewer, turned toward the buttons on the left of the screen.
    const float face = 3.14159265f + 0.32f;
    drawPlayerModel(feet, face, face, -0.04f, cam, worldVP, false,
                    &idle, idleClock, AIR, AIR, AIR, nullptr, nullptr, 0.0f, &sky,
                    SHIRT, SHORTS, SHOES);

}

void Renderer::render(const World& world, const Player& player, float timeOfDay, UIState& ui) {
    Sky sky;
    computeSky(timeOfDay, sky);
    const bool menuWorld = ui.menuWorld;
    const bool portrait = (ui.appScreen == AppScreen::Start || ui.appScreen == AppScreen::PlayerProfile);
    Vec3 head = player.eye();
    Vec3 look = player.lookDir();
    Vec3 renderLook = look;
    Vec3 eye = head;
    if (menuWorld) {
        eye = ui.menuEye;
        look = ui.menuTarget - ui.menuEye;
        float ll = look.length();
        if (ll > 1e-4f) look = look / ll;
        else look = { 0.0f, -0.2f, -1.0f };
        renderLook = look;
        head = eye;
    }
    // Head-up stays aligned with yaw even near ±90° pitch (world up would roll).
    Vec3 camUp{ 0.0f, 1.0f, 0.0f };
    if (ui.camMode == 1) { // third-person: crosshair on the right side of the head
        const float kShoulder = 0.28f; // past the head (half-width 0.18), over the right shoulder
        Vec3 aim = head + player.right() * kShoulder;
        eye = aim - look * 2.2f;
        camUp = player.right().cross(look);
    } else if (ui.camMode == 2) { // second-person: on the look ray, crosshair on the head
        eye = head + look * 3.0f;
        renderLook = look * -1.0f;
        camUp = player.right().cross(look);
    }
    gl::Viewport(0, 0, scrW, scrH);
    Mat4 proj = Mat4::perspective(cfg::FOV_Y, (float)scrW / (float)scrH, cfg::NEAR_PLANE, cfg::FAR_PLANE);
    Mat4 view = Mat4::lookAt(Vec3{ 0, 0, 0 }, renderLook, camUp);
    Mat4 vp = proj * view;
    Mat4 invVP = inverse(vp);

    gl::ClearColor(sky.below.x, sky.below.y, sky.below.z, 1.0f);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float rimHalf = 0.0f, bminX = 0.0f, bmaxX = 0.0f, bminZ = 0.0f, bmaxZ = 0.0f;
    if (ui.borderActive) {
        const float S = cfg::BLOCK_SCALE;
        rimHalf = 0.5f * (float)(matchmap::kRim * cfg::CHUNK_X) * S;
        bminX = (float)(matchmap::playMin() * cfg::CHUNK_X) * S;
        bmaxX = (float)((matchmap::playMax() + 1) * cfg::CHUNK_X) * S;
        bminZ = bminX;
        bmaxZ = bmaxX;
    }
    drawSky(sky, invVP, eye, rimHalf, bminX, bmaxX, bminZ, bmaxZ);

    if (ui.appScreen == AppScreen::Playing || menuWorld) {
        gl::Enable(GL_DEPTH_TEST);
        gl::DepthMask(GL_TRUE);
        Vec3 breakRel{ 0, 0, 0 };
        float breakProg = 0.0f, breakSod = 0.0f;
        if (ui.hasBreakOverlay && ui.targetPhys < 0) {
            const float S = cfg::BLOCK_SCALE;
            breakRel = {
                (ui.targetBlock.x + 0.5f) * S - eye.x,
                (ui.targetBlock.y + 0.5f) * S - eye.y,
                (ui.targetBlock.z + 0.5f) * S - eye.z
            };
            breakProg = ui.breakProgress;
            if (ui.breakSod) breakSod = 1.0f;
        }
        Vec3 breakNrm{ 0, 1, 0 };
        if (ui.targetFace >= 0 && ui.targetFace < 6) {
            const int* n = geo::kFaces[ui.targetFace].n;
            breakNrm = { (float)n[0], (float)n[1], (float)n[2] };
        }
        drawWorld(world, eye, vp, sky, breakRel, breakProg, breakSod, breakNrm,
                  cfg::FOG_DENSITY, sky.fogColor, rimHalf, bminX, bmaxX, bminZ, bmaxZ);

        if (!menuWorld) {
        const bool firstPerson = (ui.camMode == 0);
        const anim::Clip& playerClip = anim::clipByName(player.animName);
        uint8_t heldR = AIR, heldL = AIR, carried = AIR;
        if (ui.inventory) {
            if (ui.selectedSlot >= 0 && ui.selectedSlot < cfg::HOTBAR_SLOTS)
                heldR = ui.inventory[ui.selectedSlot].block;
            if (ui.selectedLeft >= 0 && ui.selectedLeft < cfg::HAND_SLOTS)
                heldL = ui.inventory[ui.selectedLeft].block;
        }
        if (ui.carrySlot && !ui.carrySlot->empty()) carried = ui.carrySlot->block;
        const anim::StrikeView swing = anim::resolveStrike(
            player.strikeName, player.strikeCharge, player.strikeCool,
            player.mineCharge, player.mineCooldown, player.pickRaised);
        const anim::Clip* strike = swing.clip;
        const float strikeAt = swing.frame;
        uint8_t wearUpper = AIR, wearLower = AIR, wearShoes = AIR;
        if (ui.wear) {
            if (!ui.wear[wear::Upper].empty()) wearUpper = ui.wear[wear::Upper].block;
            if (!ui.wear[wear::Lower].empty()) wearLower = ui.wear[wear::Lower].block;
            if (!ui.wear[wear::Shoes].empty()) wearShoes = ui.wear[wear::Shoes].block;
        }
        const bool netBody = ui.netAnim && !firstPerson;
        const anim::Clip& shownClip = netBody ? anim::clipFromNet(ui.netClip) : playerClip;
        float shownFrame = netBody ? ui.netFrame : player.animClock;
        float shownBody = netBody ? ui.netBodyYaw : player.bodyYaw;
        float shownYaw = netBody ? ui.netYaw : player.yaw;
        float shownPitch = netBody ? ui.netPitch : player.pitch;
        const anim::Clip* shownStrike = strike;
        float shownStrikeAt = strikeAt;
        if (netBody) {
            shownStrike = anim::strikeFromNet(ui.netStrike);
            shownStrikeAt = ui.netStrikeFrame;
        }
        if (!ui.spectating && !ui.hideAvatar) {
            drawPlayerModel(player.pos, shownBody, shownYaw, shownPitch, eye, vp, firstPerson,
                            &shownClip, shownFrame, heldR, heldL, carried,
                            nullptr, shownStrike, shownStrikeAt, &sky, wearUpper, wearLower, wearShoes);
        }
        for (const RemoteAvatar& rp : ui.remotes) {
            if (rp.spectator || rp.dead) continue;
            const anim::Clip& clip = anim::clipFromNet(rp.clip);
            const anim::Clip* overlay = anim::strikeFromNet(rp.strike);
            Vec3 reacted = rp.pos;
            if (rp.hitFlash) reacted.y += 0.045f;
            drawPlayerModel(reacted, rp.bodyYaw, rp.yaw, rp.pitch, eye, vp, false, &clip, rp.frame,
                            rp.heldR, rp.heldL, rp.carried, &rp.vitals, overlay, rp.strikeFrame, &sky,
                            rp.wearU, rp.wearL, rp.wearS);
        }
        drawArcaneEffects(eye, vp, player, ui, firstPerson);

        // Observation dummy: loops walk in place. Head/body yaw stay a runtime overlay.
        if (ui.dummyActive) {
            static float dummyClock = 0.0f;
            static float dummyPhase = 0.0f;
            dummyPhase += 0.08f;
            const anim::Clip& walk = anim::playerClips().walk;
            float rdt = (ui.fps > 1.0f) ? (1.0f / ui.fps) : (1.0f / 60.0f);
            dummyClock += rdt * ((walk.fps > 0.1f) ? walk.fps : 20.0f);
            float L = (walk.length > 0) ? (float)walk.length : 20.0f;
            while (dummyClock >= L) dummyClock -= L;
            float dummyYaw = 0.6f + 0.25f * std::sin(dummyPhase * 0.18f);
            drawPlayerModel(ui.dummyPos, dummyYaw, dummyYaw, 0.0f, eye, vp, false, &walk, dummyClock,
                            AIR, AIR, AIR, nullptr, nullptr, 0.0f, &sky);
        }

        if (ui.targets) {
            const anim::Clip& idle = anim::playerClips().idle;
            for (const TrainingTarget& t : *ui.targets)
                drawPlayerModel(t.feet, t.yaw, t.yaw, 0.0f, eye, vp, false,
                                &idle, 0.0f, AIR, AIR, AIR, &t.vitals, nullptr, 0.0f, &sky,
                                AIR, AIR, AIR, true);
        }

        drawGuardians(world, player, ui, eye, vp, &sky);

        if (ui.humidityMode) drawHumidity(world, eye, vp);

        if (ui.targetAim >= 0 && ui.targets && ui.targetAim < (int)ui.targets->size()) {
            const TrainingTarget& t = (*ui.targets)[(size_t)ui.targetAim];
            drawOutlineAt(vp, eye, { t.feet.x, t.feet.y + 0.90f, t.feet.z },
                          { 0.84f, 1.86f, 0.84f }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
                          1.0f, 1.0f, 1.0f, 0.95f);
        } else if (ui.targetGuardian >= 0) {
            float hurt = clampf(ui.guardianHurt, 0.0f, 1.0f);
            drawOutlineAt(vp, eye, ui.guardianCenter, ui.guardianSize,
                          { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
                          1.0f, 1.0f - hurt, 1.0f - hurt, 0.95f);
        } else if (ui.targetDrop >= 0 && ui.targetDrop < (int)world.drops().size()) {
            const loot::Drop& d = world.drops()[(size_t)ui.targetDrop];
            const dropgeom::Shape& sh = dropgeom::cached(d.item);
            Vec3 box{ sh.half.x * 2.16f, sh.half.y * 2.16f, sh.half.z * 2.16f };
            drawOutlineAt(vp, eye, d.pos, box, d.ax, d.ay, d.az, 1.0f, 1.0f, 1.0f, 0.95f);
        } else if (ui.hasTarget) {
            if (ui.targetPhys >= 0 && ui.targetPhys < (int)world.physicsIslands().size()) {
                const PhysicsIsland& t = world.physicsIslands()[(size_t)ui.targetPhys];
                drawOutlineOriented(vp, eye, t, ui.targetBlock.x, ui.targetBlock.y, ui.targetBlock.z,
                                    0.0f, 0.0f, 0.0f, 0.85f);
            } else {
                drawOutline(vp, eye, ui.targetBlock, 0.0f, 0.0f, 0.0f, 0.85f);
            }
        }
        drawBreakOverlay(vp, eye, ui, world);
        if (ui.humidityMode && ui.hasHumidityBlock)
            drawOutline(vp, eye, ui.humidityBlock, 1.0f, 0.9f, 0.15f, 0.95f);
        if (ui.hasPlacePreview) {
            gl::Disable(GL_DEPTH_TEST);
            drawOutline(vp, eye, ui.placePreview, 1.0f, 0.95f, 0.45f, 1.0f);
            gl::Enable(GL_DEPTH_TEST);
        }

        // Project the pointed air block's center to screen for its humidity label.
        if (ui.hasHumidityBlock) {
            const float S = cfg::BLOCK_SCALE;
            Vec3 c{ (ui.humidityBlock.x + 0.5f) * S - eye.x,
                    (ui.humidityBlock.y + 0.5f) * S - eye.y,
                    (ui.humidityBlock.z + 0.5f) * S - eye.z };
            Vec4 clip = vp * Vec4{ c.x, c.y, c.z, 1.0f };
            if (clip.w > 0.0001f) {
                float ndcX = clip.x / clip.w, ndcY = clip.y / clip.w;
                ui.humidityScreenX = (ndcX * 0.5f + 0.5f) * (float)scrW;
                ui.humidityScreenY = (0.5f - ndcY * 0.5f) * (float)scrH;
            } else {
                ui.hasHumidityBlock = false;
            }
        }
        }
    }

    if (portrait) drawMenuPortrait(world, timeOfDay, ui);

    drawUI(world, player, timeOfDay, ui);
}

void Renderer::drawSky(const Sky& s, const Mat4& invVP, const Vec3& eye,
                       float rimHalf, float bminX, float bmaxX, float bminZ, float bmaxZ) {
    gl::Disable(GL_DEPTH_TEST);
    gl::DepthMask(GL_FALSE);
    gl::Disable(GL_CULL_FACE);
    gl::UseProgram(progSky);
    gl::UniformMatrix4fv(uInvVP, 1, GL_FALSE, invVP.m);
    gl::Uniform3f(uSkySunDir, s.sunDir.x, s.sunDir.y, s.sunDir.z);
    gl::Uniform3f(uSkyMoonDir, s.moonDir.x, s.moonDir.y, s.moonDir.z);
    gl::Uniform3f(uZenith, s.zenith.x, s.zenith.y, s.zenith.z);
    gl::Uniform3f(uHorizon, s.horizon.x, s.horizon.y, s.horizon.z);
    gl::Uniform3f(uBelow, s.below.x, s.below.y, s.below.z);
    gl::Uniform3f(uSkySunColor, s.sunColor.x, s.sunColor.y, s.sunColor.z);
    gl::Uniform3f(uSkyMoonColor, s.moonColor.x, s.moonColor.y, s.moonColor.z);
    gl::Uniform1f(uSunDisc, s.sunDisc);
    gl::Uniform1f(uMoonDisc, s.moonDisc);
    gl::Uniform1f(uStarAmount, s.starAmount);
    gl::Uniform4f(uSkyBorderXZ, bminX, bmaxX, bminZ, bmaxZ);
    gl::Uniform1f(uSkyRimHalf, rimHalf);
    gl::Uniform3f(uSkyCamera, eye.x, eye.y, eye.z);
    gl::BindVertexArray(skyVAO);
    gl::DrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
}

void Renderer::drawWorld(const World& w, const Vec3& eye, const Mat4& vp, const Sky& s,
                         const Vec3& breakRel, float breakProgress, float breakSod,
                         const Vec3& breakNrm, float fogDensity, const Vec3& fogColor,
                         float rimHalf, float bminX, float bmaxX, float bminZ, float bmaxZ) {
    gl::UseProgram(progWorld);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, atlasTex);
    gl::Uniform1i(uAtlas, 0);
    gl::Uniform1f(uBlockScale, cfg::BLOCK_SCALE);
    gl::Uniform3f(uSunDir, s.sunDir.x, s.sunDir.y, s.sunDir.z);
    gl::Uniform3f(uSunColor, s.sunColor.x, s.sunColor.y, s.sunColor.z);
    gl::Uniform3f(uAmbient, s.ambient.x, s.ambient.y, s.ambient.z);
    gl::Uniform3f(uFogColor, fogColor.x, fogColor.y, fogColor.z);
    gl::Uniform1f(uFogDensity, fogDensity);
    gl::Uniform4f(uBorderXZ, bminX, bmaxX, bminZ, bmaxZ);
    gl::Uniform1f(uRimHalf, rimHalf);
    gl::Uniform3f(uCameraPos, eye.x, eye.y, eye.z);
    gl::Uniform3f(uBreakRel, breakRel.x, breakRel.y, breakRel.z);
    gl::Uniform1f(uBreakProgress, breakProgress);
    gl::Uniform1f(uBreakSod, breakSod);
    gl::Uniform3f(uBreakNrm, breakNrm.x, breakNrm.y, breakNrm.z);

    gl::Enable(GL_CULL_FACE);
    gl::CullFace(GL_BACK);
    gl::FrontFace(GL_CCW);
    gl::Disable(GL_BLEND);

    auto drawRange = [&](bool transparent) {
        for (const auto& [key, ch] : w.chunks()) {
            if (!ch.hasMesh || !ch.uploaded) continue;
            auto it = m_chunkGL.find(key);
            if (it == m_chunkGL.end()) continue;
            const ChunkGL& cg = it->second;
            if ((transparent ? cg.transparentCount : cg.opaqueCount) == 0) continue;
            double ox = (double)(chunkCX(key) * cfg::CHUNK_X) * cfg::BLOCK_SCALE - (double)eye.x;
            double oy = (double)(chunkCY(key) * cfg::CHUNK_Y) * cfg::BLOCK_SCALE - (double)eye.y;
            double oz = (double)(chunkCZ(key) * cfg::CHUNK_Z) * cfg::BLOCK_SCALE - (double)eye.z;
            Vec3 off{ (float)ox, (float)oy, (float)oz };
            Mat4 model = Mat4::translate(off) * Mat4::scale({ cfg::BLOCK_SCALE, cfg::BLOCK_SCALE, cfg::BLOCK_SCALE });
            Mat4 mvp = vp * model;
            gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
            gl::Uniform3f(uChunkOffset, off.x, off.y, off.z);
            gl::BindVertexArray(transparent ? cg.vaoT : cg.vaoO);
            gl::DrawArrays(GL_TRIANGLES, 0, transparent ? cg.transparentCount : cg.opaqueCount);
        }
    };

    drawRange(false);
    drawFallingTrees(w, eye, vp, s.sunDir);
    drawDrops(w, eye, vp, s);

    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Disable(GL_CULL_FACE);
    gl::DepthMask(GL_FALSE);
    drawRange(true);
    gl::DepthMask(GL_TRUE);
    gl::Disable(GL_BLEND);
    gl::BindVertexArray(0);
    gl::Uniform1f(uBreakProgress, 0.0f);
    gl::Uniform1f(uBreakSod, 0.0f);
}

void Renderer::drawFallingTrees(const World& w, const Vec3& eye, const Mat4& vp, const Vec3& sunDir) {
    if (!fallVAO) return;
    const float S = cfg::BLOCK_SCALE;
    gl::BindVertexArray(fallVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, fallVBO);

    for (const PhysicsIsland& t : w.physicsIslands()) {
        if (t.meshOpaque.empty()) continue;
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(t.meshOpaque.size() * sizeof(Vertex)),
                       t.meshOpaque.data(), GL_STREAM_DRAW);
        setVertexAttribs();

        Vec3 sunLocal = tree_fall::unrotate(t, sunDir);
        gl::Uniform3f(uSunDir, sunLocal.x, sunLocal.y, sunLocal.z);
        Vec3 originM{ (float)t.originX * S, (float)t.originY * S, (float)t.originZ * S };
        Mat4 R = Mat4::fromBasis(t.ax, t.ay, t.az);
        Mat4 model = Mat4::translate(t.com - eye) * R *
                     Mat4::translate(originM - t.restCom) *
                     Mat4::scale({ S, S, S });
        Mat4 mvp = vp * model;
        gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
        gl::Uniform3f(uChunkOffset, t.com.x - eye.x, t.com.y - eye.y, t.com.z - eye.z);
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)t.meshOpaque.size());
    }
    gl::Uniform3f(uSunDir, sunDir.x, sunDir.y, sunDir.z);
}

static void emitDropCube(uint8_t item, std::vector<Vertex>& out) {
    out.clear();
    const BlockInfo& info = blockOf(item);
    float alpha = (item == GLASS) ? 0.45f : 1.0f;
    for (int f = 0; f < 6; f++) {
        const geo::FaceDef& F = geo::kFaces[f];
        int tile = (f == 0) ? info.texTop : (f == 1 ? info.texBottom : info.texSide);
        float u0, v0, u1, v1;
        tex::tileUV(tile, u0, v0, u1, v1);
        Vertex vv[4];
        for (int c = 0; c < 4; c++) {
            vv[c] = {
                F.p[c][0] - 0.5f, F.p[c][1] - 0.5f, F.p[c][2] - 0.5f,
                u0 + (u1 - u0) * F.t[c][0], v0 + (v1 - v0) * F.t[c][1],
                (float)F.n[0], (float)F.n[1], (float)F.n[2],
                F.shade, 1.0f, alpha
            };
        }
        out.push_back(vv[0]); out.push_back(vv[1]); out.push_back(vv[2]);
        out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[3]);
    }
}

void Renderer::drawDrops(const World& w, const Vec3& eye, const Mat4& vp, const Sky& sky) {
    if (!fallVAO) return;
    const auto& drops = w.drops();
    if (drops.empty()) return;

    auto bindWorld = [&]() {
        gl::UseProgram(progWorld);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, atlasTex);
        gl::Uniform1i(uAtlas, 0);
        gl::Uniform1f(uBlockScale, cfg::BLOCK_SCALE);
    };
    bindWorld();
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Disable(GL_CULL_FACE);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_TRUE);

    auto drawAtlas = [&](const Mat4& mvp, const Vec3& chunkOff, float blockScale) {
        if (m_fallMesh.empty()) return;
        bindWorld();
        gl::Uniform1f(uBlockScale, blockScale);
        gl::BindVertexArray(fallVAO);
        gl::BindBuffer(GL_ARRAY_BUFFER, fallVBO);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m_fallMesh.size() * sizeof(Vertex)),
                       m_fallMesh.data(), GL_STREAM_DRAW);
        setVertexAttribs();
        gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
        gl::Uniform3f(uChunkOffset, chunkOff.x, chunkOff.y, chunkOff.z);
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)m_fallMesh.size());
    };
    auto drawGarmentTex = [&](const std::vector<float>& tv, unsigned tex, const Mat4& mvp) {
        if (tv.empty() || !tex || !humTexVAO) return;
        gl::UseProgram(progHumTex);
        gl::Uniform1f(uHumTexLit, 1.0f);
        gl::UniformMatrix4fv(uHumTexMVP, 1, GL_FALSE, mvp.m);
        gl::Uniform1i(uHumTexAtlas, 0);
        gl::Uniform3f(uHumTexSunDir, sky.sunDir.x, sky.sunDir.y, sky.sunDir.z);
        gl::Uniform3f(uHumTexSunColor, sky.sunColor.x, sky.sunColor.y, sky.sunColor.z);
        gl::Uniform3f(uHumTexAmbient, sky.ambient.x, sky.ambient.y, sky.ambient.z);
        gl::Uniform3f(uHumTexFogColor, sky.fogColor.x, sky.fogColor.y, sky.fogColor.z);
        gl::Uniform1f(uHumTexFogDensity, cfg::FOG_DENSITY);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, tex);
        gl::BindVertexArray(humTexVAO);
        gl::BindBuffer(GL_ARRAY_BUFFER, humTexVBO);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tv.size() * sizeof(float)), tv.data(), GL_STREAM_DRAW);
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(tv.size() / 9));
        gl::BindVertexArray(0);
    };

    for (const loot::Drop& d : drops) {
        if (d.item == AIR || d.count == 0) continue;
        const dropgeom::Shape& sh = dropgeom::cached(d.item);
        Mat4 R = Mat4::fromBasis(d.ax, d.ay, d.az);
        Mat4 placed = Mat4::translate(d.pos - eye) * R;
        Vec3 chunkOff{ d.pos.x - eye.x, d.pos.y - eye.y, d.pos.z - eye.z };

        if (sh.form == dropgeom::Form::Model) {
            const float S = cfg::BLOCK_SCALE;
            Vec3 o = sh.center;
            auto xform = [&](float x, float y, float z) {
                return Vec3{ (x - o.x) * S, (y - o.y) * S, (z - o.z) * S };
            };
            m_fallMesh.clear();
            const mat::Model& mdl = mat::itemModel(d.item);
            if (!mdl.quads.empty() || mat::modelHasSolidTex(mdl))
                mat::emitModelMesh(mdl, m_fallMesh, xform, blockOf(d.item).icon);
            if (blockEmission(d.item) > 0) {
                for (Vertex& v : m_fallMesh) v.blockLight = 1.0f;
            }
            std::vector<float> solid;
            if (!mdl.solids.empty())
                mat::emitSolidMesh(mdl.solids, solid, xform, false, 1.0f);
            Mat4 mvp = vp * placed;
            drawAtlas(mvp, chunkOff, 1.0f);
            if (!solid.empty()) drawHumSolid(solid, mvp, &sky, cfg::FOG_DENSITY);
            if (!m_fallMesh.empty() || !solid.empty()) continue;
        } else if (sh.form == dropgeom::Form::Garment) {
            const std::vector<pm::Part>& parts = wear::garment(d.item);
            Vec3 o = sh.center;
            auto xform = [&](const pm::Part&, float x, float y, float z) {
                return Vec3{ x - o.x, y - o.y, z - o.z };
            };
            const wear::GarmentAsset& gasset = wear::garmentAsset(d.item);
            unsigned gtex = 0;
            auto git = garmentTex.find((int)d.item);
            if (git != garmentTex.end()) gtex = git->second;
            pdraw::Mesh mesh;
            pdraw::build(parts, xform, [](const std::string&) { return false; },
                         false, false, mesh, false, gtex ? gasset.sheetW : 0, gtex ? gasset.sheetH : 0);
            Mat4 mvp = vp * placed;
            if (!mesh.solid.empty()) drawHumSolid(mesh.solid, mvp, &sky, cfg::FOG_DENSITY);
            drawGarmentTex(mesh.sheet, gtex, mvp);
            if (!mesh.solid.empty() || (!mesh.sheet.empty() && gtex)) continue;
        }

        emitDropCube(d.item, m_fallMesh);
        if (m_fallMesh.empty()) continue;
        Mat4 model = placed * Mat4::scale({ cfg::DROP_SIZE, cfg::DROP_SIZE, cfg::DROP_SIZE });
        drawAtlas(vp * model, chunkOff, cfg::BLOCK_SCALE);
    }
    bindWorld();
    gl::Enable(GL_CULL_FACE);
    gl::Disable(GL_BLEND);
}

void Renderer::drawOutlineAt(const Mat4& vp, const Vec3& eye, const Vec3& center, const Vec3& size,
                             const Vec3& ax, const Vec3& ay, const Vec3& az,
                             float r, float g, float b, float a) {
    gl::UseProgram(progFlat);
    Mat4 R = Mat4::fromBasis(ax, ay, az);
    Mat4 model = Mat4::translate(center - eye) * R *
                 Mat4::scale(size) *
                 Mat4::translate({ -0.5f, -0.5f, -0.5f });
    Mat4 mvp = vp * model;
    gl::UniformMatrix4fv(uFlatMVP, 1, GL_FALSE, mvp.m);
    gl::Uniform4f(uFlatColor, r, g, b, a);
    gl::BindVertexArray(outlineVAO);
    gl::DrawArrays(GL_LINES, 0, 24);
    gl::BindVertexArray(0);
}

void Renderer::drawParticle(const Mat4& vp, const Vec3& eye, const Vec3& center,
                            float width, float height, float r, float g, float b, float a,
                            float softness, float ring) {
    if (!particleVAO || width <= 0.0f || height <= 0.0f || a <= 0.0f) return;
    Vec3 facing = (eye - center).normalized();
    if (facing.lengthSq() < 1e-8f) facing = {0,0,1};
    Vec3 right = Vec3{0,1,0}.cross(facing).normalized();
    if (right.lengthSq() < 1e-8f) right = Vec3{1,0,0};
    Vec3 up = facing.cross(right).normalized();
    Mat4 model = Mat4::translate(center - eye) * Mat4::fromBasis(right, up, facing) *
                 Mat4::scale({width, height, 1.0f});
    Mat4 mvp = vp * model;
    gl::UseProgram(progParticle);
    gl::UniformMatrix4fv(uParticleMVP, 1, GL_FALSE, mvp.m);
    gl::Uniform4f(uParticleColor, r, g, b, a);
    gl::Uniform1f(uParticleSoftness, softness);
    gl::Uniform1f(uParticleRing, ring);
    gl::BindVertexArray(particleVAO);
    gl::DrawArrays(GL_TRIANGLES, 0, 6);
    gl::BindVertexArray(0);
}

void Renderer::drawArcaneEffects(const Vec3& eye, const Mat4& vp, const Player& player,
                                 const UIState& ui, bool firstPerson) {
    const Vec3 X{1,0,0}, Y{0,1,0}, Z{0,0,1};
    static float clock = 0.0f;
    clock += clampf(ui.fps > 1.0f ? 1.0f / ui.fps : 1.0f / 60.0f, 0.0f, 0.05f);
    auto hash01 = [](uint32_t n) {
        n ^= n >> 16; n *= 0x7feb352du; n ^= n >> 15; n *= 0x846ca68bu; n ^= n >> 16;
        return float(n & 0x00ffffffu) / float(0x01000000u);
    };
    auto burstSeed = [&](const ArcaneBurstView& burst) {
        uint32_t x = (uint32_t)std::lround(std::fabs(burst.pos.x) * 97.0f);
        uint32_t y = (uint32_t)std::lround(std::fabs(burst.pos.y) * 193.0f);
        uint32_t z = (uint32_t)std::lround(std::fabs(burst.pos.z) * 389.0f);
        return x ^ (y << 7) ^ (z << 13) ^ ((uint32_t)burst.kind * 0x9e3779b9u);
    };

    gl::Enable(GL_BLEND);
    gl::Disable(GL_CULL_FACE);
    gl::DepthMask(GL_FALSE);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE);

    for (const ArcaneProjectileView& projectile : ui.arcaneProjectiles) {
        float pulse = 0.5f + 0.5f * std::sin(clock * 13.0f + projectile.id * 1.7f);
        bool ice = projectile.kind == 2;
        float outer = ice ? .48f : .68f;
        drawParticle(vp, eye, projectile.pos, outer + pulse*.10f, outer + pulse*.10f,
                     ice ? .18f : 1.0f, ice ? .68f : .10f, 1.0f, .58f, .42f);
        drawParticle(vp, eye, projectile.pos, outer*.62f, outer*.62f,
                     ice ? .40f : 1.0f, ice ? .90f : .55f, ice ? 1.0f : .03f, .88f, .24f);
        drawParticle(vp, eye, projectile.pos, outer*.28f, outer*.28f,
                     1.0f, ice ? 1.0f : .92f, ice ? 1.0f : .45f, 1.0f, .18f);

        Vec3 dir = projectile.vel.normalized();
        if (dir.lengthSq() < 1e-8f) dir = {0,0,1};
        Vec3 side = dir.cross(Y).normalized();
        if (side.lengthSq() < 1e-8f) side = X;
        Vec3 lift = side.cross(dir).normalized();
        for (int i = 0; i < 23; ++i) {
            float t = (i + 1) / 23.0f;
            float phase = clock * (ice ? 7.0f : 11.0f) + projectile.id*.71f + i*.93f;
            float spread = .025f + t * (ice ? .12f : .22f);
            Vec3 at = projectile.pos - dir * (.08f + i * .072f) +
                      side * (std::cos(phase) * spread) + lift * (std::sin(phase) * spread);
            float size = (ice ? .16f : .22f) * (1.0f - t*.68f);
            float alpha = .92f * (1.0f - t*.78f);
            drawParticle(vp, eye, at, size, size * (ice ? 1.65f : 1.0f),
                         ice ? .30f : 1.0f, ice ? .82f : (.68f - t*.42f),
                         ice ? 1.0f : .03f, alpha, .32f);
        }
        for (int i = 0; i < 12; ++i) {
            float phase = clock * 4.0f + projectile.id + i * 2.39996f;
            float drift = .20f + hash01(projectile.id * 31u + (uint32_t)i) * .38f;
            Vec3 at = projectile.pos - dir * (hash01(projectile.id + (uint32_t)i*17u) * .90f) +
                      side * (std::cos(phase) * drift) + lift * (std::sin(phase) * drift);
            float size = .045f + hash01(projectile.id + (uint32_t)i*53u) * .065f;
            drawParticle(vp, eye, at, size, size*(ice ? 2.2f : 1.35f),
                         ice ? .55f : 1.0f, ice ? .92f : .42f, ice ? 1.0f : .02f, .72f, .28f);
        }
    }

    for (const ArcaneBurstView& burst : ui.arcaneBursts) {
        float u = clampf(burst.age / .65f, 0.0f, 1.0f);
        float alpha = (1.0f - u) * (1.0f - u);
        float ease = 1.0f - (1.0f-u)*(1.0f-u);
        uint32_t seed = burstSeed(burst);
        if (burst.kind == 3) {
            drawParticle(vp, eye, burst.pos, .45f + ease*.75f, .45f + ease*.75f,
                         .18f, 1.0f, .30f, alpha*.42f, .42f, .60f);
            for (int ring = 0; ring < 2; ++ring) {
                float radius = .28f + ease * (1.15f + ring*.34f);
                drawParticle(vp, eye, burst.pos + Vec3{0,.05f + ring*.18f,0}, radius, radius,
                             ring ? 1.0f : .22f, 1.0f, ring ? .22f : .38f,
                             alpha*(ring ? .70f : .48f), .11f, .73f);
            }
            for (int i = 0; i < 24; ++i) {
                float lane = (float)(i & 1);
                float phase = i * 1.618034f + lane * 3.14159f + u * 4.5f;
                float radius = .20f + ease * (.30f + (i % 4)*.035f);
                Vec3 at = burst.pos + Vec3{std::cos(phase)*radius,
                    -.35f + u*(1.85f + (i%5)*.10f), std::sin(phase)*radius};
                float size = .055f + (i%3)*.015f;
                drawParticle(vp, eye, at, size, size*1.45f,
                             (i&1) ? 1.0f : .18f, 1.0f, (i&1) ? .20f : .38f,
                             alpha*.95f, .28f);
            }
            continue;
        }
        bool ice = burst.kind == 2;
        float flash = clampf(1.0f - u*3.0f, 0.0f, 1.0f);
        drawParticle(vp, eye, burst.pos, (ice ? .82f : 1.55f) + ease*(ice ? .90f : 2.35f),
                     (ice ? .82f : 1.55f) + ease*(ice ? .90f : 2.35f),
                     ice ? .52f : 1.0f, ice ? .88f : .46f, ice ? 1.0f : .04f,
                     alpha*.72f + flash*.80f, .44f);
        float ringSize = .40f + ease * (ice ? 3.2f : 6.2f);
        drawParticle(vp, eye, burst.pos, ringSize, ringSize,
                     ice ? .38f : 1.0f, ice ? .83f : .28f, ice ? 1.0f : .02f,
                     alpha, .085f, .80f);
        if (!ice) {
            float second = clampf((u - .08f) / .92f, 0.0f, 1.0f);
            float secondAlpha = (1.0f-second)*(1.0f-second);
            float secondRing = .25f + second * 5.25f;
            drawParticle(vp, eye, burst.pos + Vec3{0,.10f,0}, secondRing, secondRing,
                         1.0f,.72f,.10f,secondAlpha*.84f,.075f,.88f);
            drawParticle(vp, eye, burst.pos, 1.10f + ease*2.4f, 1.10f + ease*2.4f,
                         1.0f,.92f,.48f,flash*.92f + alpha*.24f,.48f);
        }
        int particles = ice ? 42 : 72;
        for (int i = 0; i < particles; ++i) {
            float a = hash01(seed + (uint32_t)i*13u) * 6.2831853f;
            float y = hash01(seed + (uint32_t)i*31u) * 1.6f - .45f;
            Vec3 ray{std::cos(a), y, std::sin(a)};
            ray = ray.normalized();
            float speed = (ice ? .70f : 1.10f) + hash01(seed + (uint32_t)i*47u) * (ice ? 1.25f : 2.05f);
            Vec3 at = burst.pos + ray * (ease * speed) + Vec3{0, ice ? 0.0f : -.36f*u*u, 0};
            float size = (ice ? .065f : .060f) + hash01(seed + (uint32_t)i*71u) * .12f;
            drawParticle(vp, eye, at, size, size*(ice ? 2.8f : 1.55f),
                         ice ? .50f : 1.0f, ice ? .90f : (.24f + hash01(seed+i)*.48f),
                         ice ? 1.0f : .02f, alpha, .24f);
        }
        if (!ice) {
            for (int i = 0; i < 24; ++i) {
                float a = hash01(seed + (uint32_t)i*101u) * 6.2831853f;
                float radius = ease * (.35f + hash01(seed + (uint32_t)i*131u)*2.35f);
                Vec3 at = burst.pos + Vec3{std::cos(a)*radius, .12f + u*(.65f + (i%5)*.16f), std::sin(a)*radius};
                float size = .20f + u*.28f;
                drawParticle(vp, eye, at, size, size, .25f, .18f, .15f, alpha*.30f, .48f);
            }
            for (int i = 0; i < 20; ++i) {
                float a = hash01(seed + (uint32_t)i*163u) * 6.2831853f;
                float radius = ease * (.18f + hash01(seed + (uint32_t)i*181u)*2.2f);
                Vec3 at = burst.pos + Vec3{std::cos(a)*radius,
                    (hash01(seed + (uint32_t)i*197u)-.25f)*1.7f*ease,
                    std::sin(a)*radius};
                float size = .11f + hash01(seed + (uint32_t)i*211u)*.19f;
                drawParticle(vp, eye, at, size, size*1.25f, 1.0f,
                             .30f + hash01(seed+i)*.56f, .015f, alpha*.95f, .26f);
            }
        }
    }

    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    auto drawBurning = [&](Vec3 feet, uint32_t seed) {
        for (int i = 0; i < 20; ++i) {
            float rise = std::fmod(clock*(.72f + (i%4)*.08f) + seed*.037f + i*.173f, 1.0f);
            if (rise < 0.0f) rise += 1.0f;
            float angle = seed*.21f + i*2.39996f + clock*.55f;
            float radius = .16f + (i%5)*.035f;
            Vec3 at = feet + Vec3{std::cos(angle)*radius, .10f + rise*1.70f,
                                  std::sin(angle)*radius};
            float size = .09f + (1.0f-rise)*.09f;
            drawParticle(vp, eye, at, size, size*1.75f, 1.0f,
                         .10f + (1.0f-rise)*.62f, .01f, .88f-rise*.55f, .34f);
        }
    };
    auto drawFrozen = [&](Vec3 feet, uint32_t seed) {
        float pulse = .45f + .10f * std::sin(clock*3.5f + seed);
        drawOutlineAt(vp, eye, feet + Vec3{0,.9f,0}, {.72f,1.85f,.72f}, X,Y,Z,
                      .42f,.78f,1.0f,pulse);
        for (int i = 0; i < 16; ++i) {
            float angle = seed*.17f + i*.618034f + std::sin(clock*.7f+i)*.08f;
            float height = .08f + (i % 6) * .29f;
            float radius = .24f + (i % 3) * .065f;
            Vec3 at = feet + Vec3{std::cos(angle)*radius,height,std::sin(angle)*radius};
            float width = .045f + (i % 3) * .014f;
            drawParticle(vp, eye, at, width, .20f + (i%4)*.055f,
                         .58f,.91f,1.0f,.90f,.18f);
        }
    };
    auto drawHealing = [&](Vec3 feet, uint32_t seed) {
        drawParticle(vp, eye, feet + Vec3{0,.85f,0}, 1.05f, 1.85f,
                     .22f,1.0f,.36f,.18f,.48f);
        for (int i = 0; i < 18; ++i) {
            float phase = std::fmod(clock*.72f + seed*.13f + i*.113f, 1.0f);
            if (phase < 0.0f) phase += 1.0f;
            float angle = seed*.11f + i*.698132f + phase*4.2f;
            float radius = .24f + (i%3)*.055f;
            Vec3 at = feet + Vec3{std::cos(angle)*radius,.05f + phase*1.95f,std::sin(angle)*radius};
            float size = .055f + (1.0f-phase)*.045f;
            drawParticle(vp, eye, at, size, size*1.35f,
                         (i&1) ? 1.0f : .20f,1.0f,(i&1) ? .18f : .34f,
                         .88f-phase*.36f,.27f);
        }
    };
    auto drawStatuses = [&](Vec3 feet, uint32_t seed, uint8_t status) {
        if (status & 1u) drawBurning(feet, seed);
        if (status & 2u) drawFrozen(feet, seed);
        if (status & 4u) drawHealing(feet, seed);
    };
    if (!firstPerson && !ui.spectating && !ui.hideAvatar && !ui.playerDead)
        drawStatuses(player.pos, 1, ui.playerStatus);
    for (const RemoteAvatar& remote : ui.remotes)
        if (!remote.spectator && !remote.dead) drawStatuses(remote.pos, remote.id, remote.status);

    gl::DepthMask(GL_TRUE);
    gl::Enable(GL_CULL_FACE);
    gl::Disable(GL_BLEND);
}

void Renderer::drawCrackFace(const Mat4& vp, const Vec3& eye, const World& world,
                             int phys, int bx, int by, int bz, int face,
                             const float* steps, int nSteps) {
    if (!fallVAO || !steps || nSteps <= 0 || face < 0 || face > 5) return;
    uint8_t blk = (phys >= 0) ? world.getPhysBlock(phys, bx, by, bz) : world.getBlock(bx, by, bz);
    if (blk == AIR) return;

    std::vector<Vertex>& mesh = m_fallMesh;
    mesh.clear();
    const geo::FaceDef& F = geo::kFaces[face];
    const float lift = 0.008f;
    int i00 = 0, i10 = 0, i01 = 0;
    for (int c = 0; c < 4; c++) {
        if (F.t[c][0] < 0.5f && F.t[c][1] < 0.5f) i00 = c;
        if (F.t[c][0] > 0.5f && F.t[c][1] < 0.5f) i10 = c;
        if (F.t[c][0] < 0.5f && F.t[c][1] > 0.5f) i01 = c;
    }
    float uax[3], vax[3];
    for (int k = 0; k < 3; k++) {
        uax[k] = F.p[i10][k] - F.p[i00][k];
        vax[k] = F.p[i01][k] - F.p[i00][k];
    }
    Vertex vv[4];
    for (int c = 0; c < 4; c++) {
        float ue = F.t[c][0] * 3.0f - 1.0f;
        float ve = F.t[c][1] * 3.0f - 1.0f;
        vv[c] = {
            F.p[i00][0] + uax[0] * ue + vax[0] * ve - 0.5f + (float)F.n[0] * lift,
            F.p[i00][1] + uax[1] * ue + vax[1] * ve - 0.5f + (float)F.n[1] * lift,
            F.p[i00][2] + uax[2] * ue + vax[2] * ve - 0.5f + (float)F.n[2] * lift,
            ue, ve,
            (float)F.n[0], (float)F.n[1], (float)F.n[2],
            1.0f, 1.0f, 1.0f
        };
    }
    mesh.push_back(vv[0]); mesh.push_back(vv[1]); mesh.push_back(vv[2]);
    mesh.push_back(vv[0]); mesh.push_back(vv[2]); mesh.push_back(vv[3]);

    loot::CrackStyle ck = loot::blockCrack(blk);
    gl::Uniform1f(uCrackFolds, (float)ck.folds);
    gl::Uniform3f(uCrackColor, ck.r / 255.0f, ck.g / 255.0f, ck.b / 255.0f);
    gl::Uniform3f(uCrackSeed, (float)bx, (float)by, (float)bz + (float)face * 17.0f);
    gl::Uniform1f(uCrackShown, (float)nSteps);
    float maxD = loot::blockBreak(blk).durability;
    if (maxD < 1e-4f) maxD = 1.0f;
    float stepBuf[World::kMaxMineHits]{};
    int n = nSteps;
    if (n > World::kMaxMineHits) n = World::kMaxMineHits;
    for (int i = 0; i < n; i++) stepBuf[i] = steps[i] / maxD;
    gl::Uniform1fv(uCrackStep, World::kMaxMineHits, stepBuf);

    int du[3] = { 0, 0, 0 }, dv[3] = { 0, 0, 0 };
    crackFaceUVAxes(F, du, dv);
    auto backed = [&](int ox, int oy, int oz) {
        return crackBackSolid(world, phys, bx + ox, by + oy, bz + oz) ? 1.0f : 0.0f;
    };
    gl::Uniform4f(uCrackEdge,
        backed(-du[0], -du[1], -du[2]),
        backed(du[0], du[1], du[2]),
        backed(-dv[0], -dv[1], -dv[2]),
        backed(dv[0], dv[1], dv[2]));
    gl::Uniform4f(uCrackCorner,
        backed(-du[0] - dv[0], -du[1] - dv[1], -du[2] - dv[2]),
        backed(du[0] - dv[0], du[1] - dv[1], du[2] - dv[2]),
        backed(-du[0] + dv[0], -du[1] + dv[1], -du[2] + dv[2]),
        backed(du[0] + dv[0], du[1] + dv[1], du[2] + dv[2]));

    gl::BindVertexArray(fallVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, fallVBO);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh.size() * sizeof(Vertex)), mesh.data(), GL_STREAM_DRAW);
    setVertexAttribs();
    const float S = cfg::BLOCK_SCALE;
    Mat4 model;
    if (phys >= 0 && phys < (int)world.physicsIslands().size()) {
        const PhysicsIsland& t = world.physicsIslands()[(size_t)phys];
        Vec3 wc = tree_fall::worldOf(t, bx, by, bz);
        Mat4 R = Mat4::fromBasis(t.ax, t.ay, t.az);
        model = Mat4::translate(wc - eye) * R * Mat4::scale({ S, S, S });
        gl::Uniform3f(uChunkOffset, wc.x - eye.x, wc.y - eye.y, wc.z - eye.z);
    } else {
        Vec3 c{
            (bx + 0.5f) * S - eye.x,
            (by + 0.5f) * S - eye.y,
            (bz + 0.5f) * S - eye.z
        };
        model = Mat4::translate(c) * Mat4::scale({ S, S, S });
        gl::Uniform3f(uChunkOffset, c.x, c.y, c.z);
    }
    Mat4 mvp = vp * model;
    gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
    gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)mesh.size());
}

void Renderer::drawBreakOverlay(const Mat4& vp, const Vec3& eye, const UIState& ui, const World& world) {
    (void)ui;
    if (!fallVAO || world.mineStates().empty()) return;

    gl::UseProgram(progWorld);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, atlasTex);
    gl::Uniform1i(uAtlas, 0);
    gl::Uniform1f(uBreakProgress, 0.0f);
    gl::Uniform1f(uCrackReveal, 1.0f);
    gl::Uniform1f(uCrackLenMu0, loot::kCrackLenMu0);
    gl::Uniform1f(uCrackLenMu1, loot::kCrackLenMu1);
    gl::Uniform1f(uCrackLenSig, loot::kCrackLenSigma);
    gl::Uniform1f(uCrackWidMu0, loot::kCrackWidMu0);
    gl::Uniform1f(uCrackWidMu1, loot::kCrackWidMu1);
    gl::Uniform1f(uCrackWidSig, loot::kCrackWidSigma);
    gl::Uniform1f(uCrackGaussZ, loot::kCrackGaussZAbs);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Disable(GL_CULL_FACE);
    gl::DepthMask(GL_FALSE);

    for (const auto& kv : world.mineStates()) {
        int phys = -1, bx = 0, by = 0, bz = 0;
        World::decodeMineKey(kv.first, phys, bx, by, bz);
        if (phys >= (int)world.physicsIslands().size()) continue;
        const World::MineState& st = kv.second;
        if (st.nHits <= 0) continue;
        float byFace[6][World::kMaxMineHits]{};
        int nFace[6]{};
        int n = st.nHits;
        if (n > World::kMaxMineHits) n = World::kMaxMineHits;
        for (int i = 0; i < n; i++) {
            int f = (int)st.hits[i].face;
            if (f < 0 || f > 5) f = 0;
            int& nf = nFace[f];
            if (nf >= World::kMaxMineHits) continue;
            byFace[f][nf++] = st.hits[i].step;
        }
        for (int f = 0; f < 6; f++) {
            if (nFace[f] > 0)
                drawCrackFace(vp, eye, world, phys, bx, by, bz, f, byFace[f], nFace[f]);
        }
    }

    gl::BindVertexArray(0);
    gl::Uniform1f(uCrackReveal, 0.0f);
    gl::Uniform1f(uBreakProgress, 0.0f);
    gl::Uniform1f(uCrackShown, 0.0f);
    gl::DepthMask(GL_TRUE);
    gl::Enable(GL_CULL_FACE);
    gl::Disable(GL_BLEND);
}

void Renderer::drawOutlineOriented(const Mat4& vp, const Vec3& eye, const PhysicsIsland& t,
                                   int lx, int ly, int lz, float r, float g, float b, float a) {
    gl::UseProgram(progFlat);
    const float S = cfg::BLOCK_SCALE;
    Vec3 wc = tree_fall::worldOf(t, lx, ly, lz);
    Mat4 R = Mat4::fromBasis(t.ax, t.ay, t.az);
    Mat4 model = Mat4::translate(wc - eye) * R *
                 Mat4::scale({ 1.02f * S, 1.02f * S, 1.02f * S }) *
                 Mat4::translate({ -0.5f, -0.5f, -0.5f });
    Mat4 mvp = vp * model;
    gl::UniformMatrix4fv(uFlatMVP, 1, GL_FALSE, mvp.m);
    gl::Uniform4f(uFlatColor, r, g, b, a);
    gl::BindVertexArray(outlineVAO);
    gl::DrawArrays(GL_LINES, 0, 24);
    gl::BindVertexArray(0);
}

void Renderer::drawOutline(const Mat4& vp, const Vec3& eye, const IVec3& block,
                           float r, float g, float b, float a) {
    gl::UseProgram(progFlat);
    const float S = cfg::BLOCK_SCALE;
    double ox = (double)block.x * S - eye.x + 0.5 * S;
    double oy = (double)block.y * S - eye.y + 0.5 * S;
    double oz = (double)block.z * S - eye.z + 0.5 * S;
    Mat4 model = Mat4::translate({ (float)ox, (float)oy, (float)oz }) *
                 Mat4::scale({ 1.02f * S, 1.02f * S, 1.02f * S }) *
                 Mat4::translate({ -0.5f, -0.5f, -0.5f });
    Mat4 mvp = vp * model;
    gl::UniformMatrix4fv(uFlatMVP, 1, GL_FALSE, mvp.m);
    gl::Uniform4f(uFlatColor, r, g, b, a);
    gl::BindVertexArray(outlineVAO);
    gl::DrawArrays(GL_LINES, 0, 24);
    gl::BindVertexArray(0);
}

void Renderer::drawHumidity(const World& w, const Vec3& eye, const Mat4& vp) {
    const float S = cfg::BLOCK_SCALE;
    int cx = (int)std::floor(eye.x / S);
    int cy = (int)std::floor(eye.y / S);
    int cz = (int)std::floor(eye.z / S);
    const int R = 6;

    std::vector<float> verts;
    verts.reserve(8192);

    auto pushQuad = [&](float ax, float ay, float az, float bx, float by, float bz,
                        float cx2, float cy2, float cz2, float dx, float dy, float dz,
                        float r, float g, float b, float a) {
        // Two triangles (a,b,c) + (a,c,d), each vertex = pos(3) + color(4).
        const float v[6][7] = {
            { ax, ay, az, r, g, b, a }, { bx, by, bz, r, g, b, a }, { cx2, cy2, cz2, r, g, b, a },
            { ax, ay, az, r, g, b, a }, { cx2, cy2, cz2, r, g, b, a }, { dx, dy, dz, r, g, b, a },
        };
        for (auto& e : v) for (int i = 0; i < 7; i++) verts.push_back(e[i]);
    };

    for (int by = cy - R; by <= cy + R; by++) {
        for (int bz = cz - R; bz <= cz + R; bz++) {
            for (int bx = cx - R; bx <= cx + R; bx++) {
                if (w.getBlock(bx, by, bz) != AIR) continue;
                int h = w.humidityAt(bx, by, bz);
                float t;
                if (h < 0) t = (float)(-h) / 256.0f;
                else       t = (float)h / 255.0f;
                if (t > 1.0f) t = 1.0f;

                // Color: red (dry) / blue (humid), darker as |h| grows.
                float br = 1.0f - 0.45f * t;
                float r, g, b;
                if (h < 0) { r = br; g = 0.0f; b = 0.0f; }
                else       { r = 0.0f; g = 0.0f; b = br; }
                // Transparency: lower |h| -> more transparent (lower alpha).
                const float a = 0.08f + 0.22f * t;
                float x0 = (float)bx * S - eye.x, x1 = x0 + S;
                float y0 = (float)by * S - eye.y, y1 = y0 + S;
                float z0 = (float)bz * S - eye.z, z1 = z0 + S;
                // 6 faces, CCW from outside.
                pushQuad(x1,y0,z0, x1,y1,z0, x1,y1,z1, x1,y0,z1, r,g,b,a); // +X
                pushQuad(x0,y0,z1, x0,y1,z1, x0,y1,z0, x0,y0,z0, r,g,b,a); // -X
                pushQuad(x0,y1,z0, x0,y1,z1, x1,y1,z1, x1,y1,z0, r,g,b,a); // +Y
                pushQuad(x0,y0,z1, x1,y0,z1, x1,y0,z0, x0,y0,z0, r,g,b,a); // -Y
                pushQuad(x0,y0,z1, x1,y0,z1, x1,y1,z1, x0,y1,z1, r,g,b,a); // +Z
                pushQuad(x1,y0,z0, x0,y0,z0, x0,y1,z0, x1,y1,z0, r,g,b,a); // -Z
            }
        }
    }

    if (verts.empty()) return;

    gl::UseProgram(progHum);
    gl::Uniform1f(uHumLit, 0.0f);
    gl::UniformMatrix4fv(uHumMVP, 1, GL_FALSE, vp.m);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::DepthMask(GL_FALSE);
    gl::Enable(GL_DEPTH_TEST);
    gl::Disable(GL_CULL_FACE); // both faces so the volume reads clearly
    gl::BindVertexArray(humVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, humVBO);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STREAM_DRAW);
    gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 7));
    gl::BindVertexArray(0);
    gl::DepthMask(GL_TRUE);
    gl::Enable(GL_CULL_FACE);
    gl::Disable(GL_BLEND);
}

void Renderer::drawPlayerModel(const Vec3& pos, float bodyYaw, float headYaw, float pitch,
                               const Vec3& eye, const Mat4& vp, bool hideHead,
                               const anim::Clip* clip, float frame, uint8_t heldRight,
                               uint8_t heldLeft, uint8_t carried, const vitals::Vitals* tint,
                               const anim::Clip* strike, float strikeAt, const Sky* sun,
                               uint8_t wearUpper, uint8_t wearLower, uint8_t wearShoes, bool bare) {
    const anim::PlayerClips& lib = anim::playerClips();
    const bool hugging = hold::isCarrying(carried);
    std::vector<anim::BoneXform> pose;
    std::vector<pm::Part> parts;
    if (clip && !clip->bones.empty()) {
        const anim::Clip* layers[1] = {};
        int nLayers = 0;
        float layerFrame = 0.0f;
        if (hugging) {
            layers[nLayers++] = &lib.holdBlock;
        } else if (strike && !strike->tracks.empty()) {
            layers[nLayers++] = strike;
            layerFrame = strikeAt;
        }
        pose = (nLayers > 0)
            ? anim::evalPoseLayered(*clip, frame, layers, nLayers, layerFrame)
            : anim::evalPose(*clip, frame);
        if (strike && !strike->tracks.empty() && !hugging && heldRight != AIR) {
            hold::Spec spec = hold::resolveBlock(hold::playerHold(), heldRight, strike->name, "right");
            const anim::Clip* layers[1] = { strike };
            hold::applyToolAxis(*strike, pose, strikeAt, spec, [&](float f) {
                return anim::evalPoseLayered(*clip, frame, layers, 1, f);
            });
            hold::keepToolContact(*clip, pose, *strike, strikeAt, spec);
        }
        parts = anim::poseParts(lib.rest, *clip, pose);
    } else {
        parts = lib.rest;
    }
    if (tint) {
        for (pm::Part& p : parts) {
            int id = vitals::limbFromName(p.name);
            if (id < 0) id = vitals::limbFromName(p.kind);
            if (id >= 0) p.color = vitals::healthColor(tint->limb[id].health);
        }
    }
    if (bare) {
        const Vec3 gray{ 0.62f, 0.62f, 0.64f };
        std::vector<pm::Part> body;
        body.reserve(parts.size());
        for (pm::Part& p : parts) {
            if (pm::isHairPart(p) || pm::isHairCardPart(p)) continue;
            if (pm::isEyePart(p) || pm::isEyelidPart(p) || pm::isMouthPart(p)) continue;
            if (pm::isCutoutPart(p) || pm::isDecalPart(p)) continue;
            if (p.kind == "cloth") continue;
            int id = vitals::limbFromName(p.name);
            if (id < 0) id = vitals::limbFromName(p.kind);
            if (!(tint && id >= 0)) p.color = gray;
            p.tex.clear();
            p.kind.clear();
            body.push_back(std::move(p));
        }
        parts.swap(body);
    }
    uint8_t wornIds[3] = { wearUpper, wearLower, wearShoes };
    const float S = 1.0f; // model parts are in world units (player is ~1.8 world tall)

    auto xform = [&](const pm::Part& p, float lx, float ly, float lz) -> Vec3 {
        const bool isHead = (p.type == 0 || p.type == 4 || pm::isHairPart(p) || pm::isHairCardPart(p));
        if (isHead)
            pm::headPitchYZ(ly, lz, pitch, ly, lz);
        float rx, rz;
        pm::lookYawXZ(lx, lz, isHead ? headYaw : bodyYaw, rx, rz);
        return { pos.x + rx * S - eye.x, pos.y + ly * S - eye.y, pos.z + rz * S - eye.z };
    };
    auto namedTex = [&](const std::string& name) -> unsigned {
        if (name.empty()) return 0;
        auto it = overlayTex.find(name);
        if (it != overlayTex.end()) return it->second;
        auto e = extraMatTex.find(name);
        if (e != extraMatTex.end()) return e->second;
        return 0;
    };
    pdraw::Mesh mesh;
    pdraw::build(parts, xform, [&](const std::string& n) { return !bare && namedTex(n) != 0; },
                 !bare && skinTex != 0, hideHead, mesh, tint != nullptr);

    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_TRUE);
    gl::Disable(GL_CULL_FACE);
    auto applySun = [&](unsigned prog, int lit, int sunDir, int sunColor, int ambient, int fogColor, int fogDensity) {
        gl::UseProgram(prog);
        if (sun) {
            gl::Uniform1f(lit, 1.0f);
            gl::Uniform3f(sunDir, sun->sunDir.x, sun->sunDir.y, sun->sunDir.z);
            gl::Uniform3f(sunColor, sun->sunColor.x, sun->sunColor.y, sun->sunColor.z);
            gl::Uniform3f(ambient, sun->ambient.x, sun->ambient.y, sun->ambient.z);
            gl::Uniform3f(fogColor, sun->fogColor.x, sun->fogColor.y, sun->fogColor.z);
            gl::Uniform1f(fogDensity, cfg::FOG_DENSITY);
        } else {
            gl::Uniform1f(lit, 0.0f);
        }
    };
    if (!mesh.solid.empty()) {
        applySun(progHum, uHumLit, uHumSunDir, uHumSunColor, uHumAmbient, uHumFogColor, uHumFogDensity);
        gl::UniformMatrix4fv(uHumMVP, 1, GL_FALSE, vp.m);
        gl::BindVertexArray(humVAO);
        gl::BindBuffer(GL_ARRAY_BUFFER, humVBO);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh.solid.size() * sizeof(float)), mesh.solid.data(), GL_STREAM_DRAW);
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(mesh.solid.size() / 7));
        gl::BindVertexArray(0);
    }
    auto drawHumTex = [&](const std::vector<float>& tv, unsigned tex) {
        if (tv.empty() || !tex) return;
        applySun(progHumTex, uHumTexLit, uHumTexSunDir, uHumTexSunColor, uHumTexAmbient, uHumTexFogColor, uHumTexFogDensity);
        gl::UniformMatrix4fv(uHumTexMVP, 1, GL_FALSE, vp.m);
        gl::Uniform1i(uHumTexAtlas, 0);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, tex);
        gl::BindVertexArray(humTexVAO);
        gl::BindBuffer(GL_ARRAY_BUFFER, humTexVBO);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tv.size() * sizeof(float)), tv.data(), GL_STREAM_DRAW);
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(tv.size() / 9));
        gl::BindVertexArray(0);
        gl::UseProgram(0);
    };
    drawHumTex(mesh.skin, skinTex);
    drawHumTex(mesh.atlas, atlasTex);
    for (auto& kv : mesh.named) drawHumTex(kv.second, namedTex(kv.first));
    for (uint8_t wid : wornIds) {
        if (wid == AIR) continue;
        const wear::GarmentAsset& g = wear::garmentAsset(wid);
        if (g.parts.empty()) continue;
        std::vector<pm::Part> posed = (clip && !pose.empty())
            ? anim::poseParts(g.parts, *clip, pose) : g.parts;
        unsigned gtex = 0;
        auto git = garmentTex.find(wid);
        if (git != garmentTex.end()) gtex = git->second;
        pdraw::Mesh cloth;
        pdraw::build(posed, xform, [&](const std::string& n) { return namedTex(n) != 0; },
                     false, false, cloth, false, gtex ? g.sheetW : 0, gtex ? g.sheetH : 0);
        if (!cloth.solid.empty()) {
            applySun(progHum, uHumLit, uHumSunDir, uHumSunColor, uHumAmbient, uHumFogColor, uHumFogDensity);
            gl::UniformMatrix4fv(uHumMVP, 1, GL_FALSE, vp.m);
            gl::BindVertexArray(humVAO);
            gl::BindBuffer(GL_ARRAY_BUFFER, humVBO);
            gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cloth.solid.size() * sizeof(float)), cloth.solid.data(), GL_STREAM_DRAW);
            gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(cloth.solid.size() / 7));
            gl::BindVertexArray(0);
        }
        drawHumTex(cloth.sheet, gtex);
        drawHumTex(cloth.atlas, atlasTex);
        for (auto& kv : cloth.named) drawHumTex(kv.second, namedTex(kv.first));
    }

    const hold::File& holds = hold::playerHold();
    const std::string locClip = (clip && !clip->name.empty()) ? clip->name : std::string("idle");

    auto heldModel = [&](uint8_t block) -> const mat::Model& {
        const mat::Material* tm = mat::toolMaterial(block);
        if (tm) return tm->model;
        return mat::itemModel(block);
    };
    auto emitHeldMesh = [&](uint8_t block, std::vector<Vertex>& mesh, float alpha, auto&& xform) {
        const mat::Model& mdl = heldModel(block);
        bool custom = !mdl.quads.empty() || mat::modelHasSolidTex(mdl);
        if (custom && !mdl.cube) {
            mat::emitModelMesh(mdl, mesh, xform, blockOf(block).icon);
            return;
        }
        if (!mdl.cube && !mdl.solids.empty()) return;
        const BlockInfo& info = blockOf(block);
        for (int f = 0; f < 6; f++) {
            const geo::FaceDef& F = geo::kFaces[f];
            uint8_t tile = (f == 0) ? info.texTop : (f == 1 ? info.texBottom : info.texSide);
            float u0, v0, u1, v1;
            tex::tileUV(tile, u0, v0, u1, v1);
            Vec3 wp[4];
            for (int c = 0; c < 4; c++)
                wp[c] = xform(F.p[c][0], F.p[c][1], F.p[c][2]);
            Vec3 n = (wp[1] - wp[0]).cross(wp[2] - wp[0]);
            float nl = n.length();
            if (nl > 1e-8f) n = n * (1.0f / nl);
            else n = { 0.0f, 1.0f, 0.0f };
            Vertex vv[4];
            for (int c = 0; c < 4; c++) {
                vv[c] = { wp[c].x, wp[c].y, wp[c].z,
                          u0 + (u1 - u0) * F.t[c][0], v0 + (v1 - v0) * F.t[c][1],
                          n.x, n.y, n.z, F.shade, 1.0f, alpha };
            }
            mesh.push_back(vv[0]); mesh.push_back(vv[1]); mesh.push_back(vv[2]);
            mesh.push_back(vv[0]); mesh.push_back(vv[2]); mesh.push_back(vv[3]);
        }
        if (custom)
            mat::emitModelMesh(mdl, mesh, xform, blockOf(block).icon);
    };

    auto drawBound = [&](uint8_t block, const char* side, const std::string& clipName, float alpha = 1.0f) {
        if (block == AIR || !validBlock(block)) return;
        hold::Spec spec = hold::resolveBlock(holds, block, clipName, side);
        if (strike && !strike->name.empty() && strike->name == clipName) {
            spec.grip = anim::evalGrip(*strike, strikeAt, spec.grip);
            hold::composeFace(spec, anim::evalFace(*strike, strikeAt), anim::evalFaceOff(*strike, strikeAt));
        } else if (clip && clip->name == clipName) {
            spec.grip = anim::evalGrip(*clip, frame, spec.grip);
            hold::composeFace(spec, anim::evalFace(*clip, frame), anim::evalFaceOff(*clip, frame));
        }
        anim::BoneXform xf{};
        bool ok = clip && !pose.empty() && anim::boneXformOf(*clip, pose, spec.bone, xf);
        if (!ok && clip && !pose.empty()) {
            const char* fb = (std::strcmp(side, "left") == 0) ? "arm_l_hand" : "arm_r_hand";
            ok = anim::boneXformOf(*clip, pose, fb, xf);
        }
        if (!ok) {
            const char* hn = (std::strcmp(side, "left") == 0) ? "arm_l_hand" : "arm_r_hand";
            for (const pm::Part& p : parts) {
                if (p.name == hn) {
                    xf.pivot = p.center;
                    anim::ident9(xf.R);
                    ok = true;
                    break;
                }
            }
        }
        if (!ok) return;
        auto holdXform = [&](float x, float y, float z) {
            Vec3 m = hold::pointOnBone(spec, xf, x, y, z);
            float rx, rz;
            pm::lookYawXZ(m.x, m.z, bodyYaw, rx, rz);
            return Vec3{ pos.x + rx - eye.x, pos.y + m.y - eye.y, pos.z + rz - eye.z };
        };
        std::vector<Vertex> held;
        emitHeldMesh(block, held, alpha, holdXform);
        for (Vertex& v : held) v.alpha = alpha;
        if (blockEmission(block) > 0) {
            for (Vertex& v : held) v.blockLight = 1.0f;
        }
        std::vector<float> solid;
        const mat::Model& mdl = heldModel(block);
        if (!mdl.solids.empty())
            mat::emitSolidMesh(mdl.solids, solid, holdXform, false, alpha);
        if ((held.empty() && solid.empty()) || !fallVAO) return;
        gl::UseProgram(progWorld);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, atlasTex);
        gl::Uniform1i(uAtlas, 0);
        gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, vp.m);
        gl::Uniform3f(uChunkOffset, 0.0f, 0.0f, 0.0f);
        gl::Uniform1f(uBlockScale, 1.0f);
        gl::Enable(GL_DEPTH_TEST);
        const bool ghost = alpha < 0.999f;
        if (ghost) {
            gl::Enable(GL_BLEND);
            gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            gl::DepthMask(GL_FALSE);
        } else {
            gl::DepthMask(GL_TRUE);
        }
        // lookYawXZ is a reflection (det -1), which flips triangle winding.
        // Same as drops: draw both sides so no held face is culled.
        gl::Disable(GL_CULL_FACE);
        if (!held.empty()) {
            gl::BindVertexArray(fallVAO);
            gl::BindBuffer(GL_ARRAY_BUFFER, fallVBO);
            gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(held.size() * sizeof(Vertex)),
                           held.data(), GL_STREAM_DRAW);
            setVertexAttribs();
            gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)held.size());
            gl::BindVertexArray(0);
        }
        gl::Uniform1f(uBlockScale, cfg::BLOCK_SCALE);
        drawHumSolid(solid, vp, sun, cfg::FOG_DENSITY);
        if (ghost) {
            gl::DepthMask(GL_TRUE);
            gl::Disable(GL_BLEND);
        }
    };
    std::string rightClip = locClip;
    if (strike && !hugging && !strike->name.empty()) rightClip = strike->name;
    drawBound(heldRight, "right", rightClip);
    drawBound(heldLeft, "left", locClip);
    if (hugging) drawBound(carried, "right", "hold_block", 0.5f);
    gl::Enable(GL_CULL_FACE);
}

void Renderer::drawGuardians(const World& world, const Player& player, const UIState& ui,
                             const Vec3& eye, const Mat4& vp, const Sky* sun) {
    std::vector<structure::GuardianSpan> guards;
    structure::collectGuardians(world, guards);

    static std::vector<pm::Part> parts[ritual::RelicCount];
    static bool loaded[ritual::RelicCount]{};
    static anim::Clip attack;
    static bool attackReady = false;
    struct Swing { bool playing = false; bool armed = false; uint8_t seen = 0; float clock = 0.0f; };
    static Swing swing[ritual::RelicCount];
    if (!attackReady) {
        attackReady = true;
        attack = anim::load(pack::animationFile("guardian_attack").c_str());
    }

    float rdt = (ui.fps > 1.0f) ? (1.0f / ui.fps) : (1.0f / 60.0f);
    bool seen[ritual::RelicCount]{};
    if (guards.empty()) {
        for (int i = 0; i < ritual::RelicCount; ++i) {
            swing[i].armed = false;
            swing[i].playing = false;
            swing[i].clock = 0.0f;
            swing[i].seen = 0;
        }
        return;
    }
    (void)player;

    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_TRUE);
    gl::Disable(GL_CULL_FACE);
    for (const structure::GuardianSpan& g : guards) {
        if (g.relic < 0 || g.relic >= ritual::RelicCount) continue;
        seen[g.relic] = true;
        if (!loaded[g.relic]) {
            loaded[g.relic] = true;
            const char* stem = structure::guardianAppearance(g.relic);
            if (stem && stem[0]) {
                std::string path = pack::entityModel(std::string("guardians/") + stem);
                parts[g.relic] = pm::loadEntity(path.c_str()).parts;
            }
        }
        if (parts[g.relic].empty()) continue;
        float yaw = 0.0f;
        uint8_t swingId = 0;
        float poseX = 0.0f, poseY = 0.0f, poseZ = 0.0f;
        bool hasPose = structure::guardianPose(g.relic, poseX, poseY, poseZ, yaw, swingId);
        Swing& sw = swing[g.relic];
        if (!hasPose) {
            sw.armed = false;
        } else if (!sw.armed) {
            sw.armed = true;
            sw.seen = swingId;
        } else if (swingId != sw.seen) {
            sw.seen = swingId;
            if (swingId && !attack.bones.empty()) {
                sw.playing = true;
                sw.clock = 0.0f;
            }
        }
        if (sw.playing) {
            float fps = (attack.fps > 0.1f) ? attack.fps : 20.0f;
            sw.clock += rdt * fps;
            float end = (attack.length > 1) ? (float)(attack.length - 1) : 0.0f;
            if (sw.clock >= end) {
                sw.playing = false;
                sw.clock = 0.0f;
            }
        }
        const std::vector<pm::Part>* drawParts = &parts[g.relic];
        std::vector<pm::Part> posed;
        if (sw.playing) {
            anim::Clip local = attack;
            anim::rebindPivots(local, parts[g.relic]);
            posed = anim::poseParts(parts[g.relic], local, sw.clock);
            drawParts = &posed;
        }
        Vec3 feet{ g.feetX, g.feetY, g.feetZ };
        auto xform = [&](const pm::Part& p, float lx, float ly, float lz) -> Vec3 {
            (void)p;
            float rx, rz;
            pm::lookYawXZ(lx, lz, yaw, rx, rz);
            return { feet.x + rx - eye.x, feet.y + ly - eye.y, feet.z + rz - eye.z };
        };
        pdraw::Mesh mesh;
        pdraw::build(*drawParts, xform, [](const std::string&) { return false; }, false, false, mesh);
        if (mesh.solid.empty()) continue;
        gl::UseProgram(progHum);
        if (sun) {
            gl::Uniform1f(uHumLit, 1.0f);
            gl::Uniform3f(uHumSunDir, sun->sunDir.x, sun->sunDir.y, sun->sunDir.z);
            gl::Uniform3f(uHumSunColor, sun->sunColor.x, sun->sunColor.y, sun->sunColor.z);
            gl::Uniform3f(uHumAmbient, sun->ambient.x, sun->ambient.y, sun->ambient.z);
            gl::Uniform3f(uHumFogColor, sun->fogColor.x, sun->fogColor.y, sun->fogColor.z);
            gl::Uniform1f(uHumFogDensity, cfg::FOG_DENSITY);
        } else {
            gl::Uniform1f(uHumLit, 0.0f);
        }
        gl::UniformMatrix4fv(uHumMVP, 1, GL_FALSE, vp.m);
        gl::BindVertexArray(humVAO);
        gl::BindBuffer(GL_ARRAY_BUFFER, humVBO);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh.solid.size() * sizeof(float)), mesh.solid.data(), GL_STREAM_DRAW);
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(mesh.solid.size() / 7));
        gl::BindVertexArray(0);
    }
    for (int i = 0; i < ritual::RelicCount; ++i) {
        if (seen[i]) continue;
        swing[i].armed = false;
        swing[i].playing = false;
        swing[i].clock = 0.0f;
        swing[i].seen = 0;
    }
    gl::Enable(GL_CULL_FACE);
}

void Renderer::quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    float r, float g, float b, float a) {
    float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    float verts[6][8] = {
        { x0, y0, u0, v0, r, g, b, a },
        { x1, y0, u1, v0, r, g, b, a },
        { x1, y1, u1, v1, r, g, b, a },
        { x0, y0, u0, v0, r, g, b, a },
        { x1, y1, u1, v1, r, g, b, a },
        { x0, y1, u0, v1, r, g, b, a },
    };
    for (auto& v : verts)
        for (int i = 0; i < 8; i++) m_batch.data.push_back(v[i]);
}

void Renderer::scrimFade(float w, float h) {
    const float r = 0.04f, g = 0.05f, b = 0.07f;
    const float aL = 0.78f, aR = 0.08f;
    float verts[6][8] = {
        { 0, 0, 0, 0, r, g, b, aL },
        { w, 0, 0, 0, r, g, b, aR },
        { w, h, 0, 0, r, g, b, aR },
        { 0, 0, 0, 0, r, g, b, aL },
        { w, h, 0, 0, r, g, b, aR },
        { 0, h, 0, 0, r, g, b, aL },
    };
    for (auto& v : verts)
        for (int i = 0; i < 8; i++) m_batch.data.push_back(v[i]);
}

void Renderer::tri(float x0, float y0, float x1, float y1, float x2, float y2,
                   float r, float g, float b, float a) {
    float verts[3][8] = {
        { x0, y0, 0, 0, r, g, b, a },
        { x1, y1, 0, 0, r, g, b, a },
        { x2, y2, 0, 0, r, g, b, a },
    };
    for (auto& v : verts)
        for (int i = 0; i < 8; i++) m_batch.data.push_back(v[i]);
}

void Renderer::drawFlag(float x, float y, float w, float h, float r, float g, float b) {
    float rectH = h * 0.68f;
    quad(x - 1.0f, y - 1.0f, w + 2.0f, rectH + 1.0f, 0, 0, 0, 0, r * 0.35f, g * 0.35f, b * 0.35f, 1.0f);
    quad(x, y, w, rectH, 0, 0, 0, 0, r, g, b, 1.0f);
    float base = y + rectH;
    tri(x, base, x + w, base, x + w * 0.5f, y + h, r, g, b, 1.0f);
}

void Renderer::flushUI(unsigned int prog, unsigned int tex) {
    if (m_batch.data.empty()) return;
    gl::UseProgram(prog);
    if (prog == progUIText) {
        gl::Uniform2f(uUITextScreen, (float)scrW, (float)scrH);
        gl::Uniform1i(uUITextTex, 0);
    } else {
        gl::Uniform2f(uUIScreen, (float)scrW, (float)scrH);
        gl::Uniform1i(uUITex, 0);
    }
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex);
    gl::BindVertexArray(m_batch.vao);
    gl::BindBuffer(GL_ARRAY_BUFFER, m_batch.vbo);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m_batch.data.size() * sizeof(float)),
                   m_batch.data.data(), GL_STREAM_DRAW);
    gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(m_batch.data.size() / 8));
    gl::BindVertexArray(0);
    m_batch.data.clear();
}

void Renderer::stringSize(const std::string& s, int& w, int& h) {
    auto it = m_textCache.find(s);
    if (it != m_textCache.end()) { w = it->second.w; h = it->second.h; return; }
    if (m_textCache.size() >= 512) {
        for (auto& kv : m_textCache) gl::DeleteTextures(1, &kv.second.tex);
        m_textCache.clear();
    }
    int tw, th;
    unsigned int tex = renderTextTexture(s, tw, th);
    m_textCache[s] = { tex, tw, th };
    w = tw; h = th;
}

void Renderer::drawString(const std::string& s, float x, float y, float scale,
                          float r, float g, float b, float a) {
    if (s.empty()) return;
    auto it = m_textCache.find(s);
    if (it == m_textCache.end()) {
        if (m_textCache.size() >= 512) {
            for (auto& kv : m_textCache) gl::DeleteTextures(1, &kv.second.tex);
            m_textCache.clear();
        }
        int tw, th;
        unsigned int tex = renderTextTexture(s, tw, th);
        m_textCache[s] = { tex, tw, th };
        it = m_textCache.find(s);
    }
    const TextTex& t = it->second;
    quad(x, y, (float)t.w * scale, (float)t.h * scale, 0, 0, 1, 1, r, g, b, a);
    flushUI(progUIText, t.tex);
}

void Renderer::text(float x, float y, float scale, float r, float g, float b, float a,
                    const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    drawString(std::string(buf), x, y, scale, r, g, b, a);
}

void Renderer::centeredText(const std::string& s, float cx, float cy, float scale,
                            float r, float g, float b, float a) {
    int w, h;
    stringSize(s, w, h);
    drawString(s, cx - (float)w * scale * 0.5f, cy - (float)h * scale * 0.5f, scale, r, g, b, a);
}

void Renderer::layoutCrosshairPrompts(const CrosshairPrompt* items, int count, float originX, float crossY,
                                      std::vector<CrosshairPromptBox>& out) {
    out.clear();
    if (!items || count <= 0) return;
    const float boxH = 34.0f;
    const float pad = 16.0f;
    const float keyScale = 1.05f;
    const float actScale = 0.55f;
    const float top = crossY - 16.0f;
    int sw = 0, sh = 0;
    stringSize("/", sw, sh);
    const float sep = (float)sw * keyScale + 16.0f;
    float x = originX;
    out.reserve((size_t)count);
    for (int i = 0; i < count; i++) {
        const char* key = items[i].key ? items[i].key : "";
        const char* action = items[i].action ? items[i].action : "";
        int kw = 0, kh = 0, aw = 0, ah = 0;
        if (items[i].kind == CrosshairKeyKind::Text) stringSize(key, kw, kh);
        stringSize(action, aw, ah);
        float need = std::max((float)kw * keyScale, (float)aw * actScale) + pad;
        float boxW = std::max(34.0f, need);
        CrosshairPromptBox box;
        box.x = x;
        box.y = top;
        box.w = boxW;
        box.h = boxH;
        box.kind = items[i].kind;
        box.key = key;
        box.action = action;
        out.push_back(std::move(box));
        x += boxW + sep;
    }
}

void Renderer::drawCrosshairPromptChrome(const std::vector<CrosshairPromptBox>& boxes) {
    for (const CrosshairPromptBox& b : boxes) {
        quad(b.x, b.y, b.w, b.h, 0, 0, 0, 0, 0.06f, 0.06f, 0.07f, 0.92f);
        quad(b.x + 3, b.y + 3, b.w - 6, b.h - 6, 0, 0, 0, 0, 0.20f, 0.20f, 0.22f, 1.0f);
        quad(b.x + 3, b.y + 3, b.w - 6, 4, 0, 0, 0, 0, 0.50f, 0.50f, 0.54f, 0.85f);
        if (b.kind != CrosshairKeyKind::MouseRight) continue;
        const float iconW = 18.0f, iconH = 22.0f, split = 1.5f;
        float ix = b.x + (b.w - iconW) * 0.5f;
        float iy = b.y + (b.h - iconH) * 0.5f;
        float topH = (iconH - split) * 0.5f;
        float colW = (iconW - split) * 0.5f;
        float botY = iy + topH + split;
        const float dimR = 0.38f, dimG = 0.38f, dimB = 0.42f;
        quad(ix, iy, colW, topH, 0, 0, 0, 0, dimR, dimG, dimB, 1.0f);
        quad(ix + colW + split, iy, colW, topH, 0, 0, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f);
        quad(ix, botY, iconW, topH, 0, 0, 0, 0, dimR, dimG, dimB, 1.0f);
    }
}

void Renderer::drawCrosshairPromptText(const std::vector<CrosshairPromptBox>& boxes) {
    const float keyScale = 1.05f;
    const float actScale = 0.55f;
    for (size_t i = 0; i < boxes.size(); i++) {
        const CrosshairPromptBox& b = boxes[i];
        float cx = b.x + b.w * 0.5f;
        if (b.kind == CrosshairKeyKind::Text && !b.key.empty())
            centeredText(b.key, cx, b.y + b.h * 0.5f, keyScale, 1, 1, 1, 1);
        centeredText(b.action, cx, b.y + b.h + 10.0f, actScale, 0.90f, 0.90f, 0.90f, 0.95f);
        if (i + 1 >= boxes.size()) continue;
        const CrosshairPromptBox& n = boxes[i + 1];
        float slashX = (b.x + b.w + n.x) * 0.5f;
        float slashY = b.y + b.h * 0.5f;
        centeredText("/", slashX, slashY, keyScale, 0.90f, 0.90f, 0.90f, 0.95f);
    }
}

void Renderer::drawBlockIcon(uint8_t block, float x, float y, float size) {
    flushUI(progUI, atlasTex);
    const std::vector<pm::Part>& garment = wear::garment(block);
    if (!garment.empty() && fallVAO && size >= 2.0f) {
        Vec3 mn{ 1e9f, 1e9f, 1e9f }, mx{ -1e9f, -1e9f, -1e9f };
        for (const pm::Part& p : garment) {
            Vec3 a = p.center - p.half, b = p.center + p.half;
            mn.x = std::min(mn.x, a.x); mn.y = std::min(mn.y, a.y); mn.z = std::min(mn.z, a.z);
            mx.x = std::max(mx.x, b.x); mx.y = std::max(mx.y, b.y); mx.z = std::max(mx.z, b.z);
        }
        Vec3 mid = (mn + mx) * 0.5f;
        Vec3 ext = mx - mn;
        float span = std::max(ext.x, std::max(ext.y, ext.z));
        if (span < 1e-4f) span = 1.0f;
        float fit = 0.85f / span;
        std::vector<pm::Part> fitted = garment;
        for (pm::Part& p : fitted) {
            p.center = (p.center - mid) * fit + Vec3{ 0.5f, 0.45f, 0.5f };
            p.half = p.half * fit;
        }
        const wear::GarmentAsset& gasset = wear::garmentAsset(block);
        unsigned gtex = 0;
        auto git = garmentTex.find(block);
        if (git != garmentTex.end()) gtex = git->second;
        pdraw::Mesh icon;
        pdraw::build(fitted, pdraw::identXform, [](const std::string&) { return false; },
                     false, false, icon, false, gtex ? gasset.sheetW : 0, gtex ? gasset.sheetH : 0);
        if (!icon.solid.empty() || !icon.sheet.empty()) {
            int vx = (int)std::floor(x);
            int vy = scrH - (int)std::floor(y + size);
            int vw = std::max(1, (int)std::floor(size));
            int vh = std::max(1, (int)std::floor(size));
            Mat4 proj = Mat4::perspective(32.0f, 1.0f, 0.05f, 8.0f);
            Mat4 view = Mat4::lookAt({ 1.55f, 1.25f, 1.55f }, { 0.50f, 0.45f, 0.50f }, { 0.0f, 1.0f, 0.0f });
            Mat4 mvp = proj * view;
            gl::Enable(GL_SCISSOR_TEST);
            gl::Scissor(vx, vy, vw, vh);
            gl::Viewport(vx, vy, vw, vh);
            gl::Enable(GL_DEPTH_TEST);
            gl::DepthMask(GL_TRUE);
            gl::Clear(GL_DEPTH_BUFFER_BIT);
            gl::Disable(GL_CULL_FACE);
            gl::Disable(GL_BLEND);
            if (!icon.solid.empty()) {
                gl::UseProgram(progHum);
                gl::Uniform1f(uHumLit, 1.0f);
                gl::UniformMatrix4fv(uHumMVP, 1, GL_FALSE, mvp.m);
                gl::Uniform3f(uHumSunDir, -0.35f, 0.85f, 0.40f);
                gl::Uniform3f(uHumSunColor, 0.55f, 0.55f, 0.52f);
                gl::Uniform3f(uHumAmbient, 0.55f, 0.55f, 0.55f);
                gl::Uniform3f(uHumFogColor, 0.0f, 0.0f, 0.0f);
                gl::Uniform1f(uHumFogDensity, 0.0f);
                gl::BindVertexArray(humVAO);
                gl::BindBuffer(GL_ARRAY_BUFFER, humVBO);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(icon.solid.size() * sizeof(float)), icon.solid.data(), GL_STREAM_DRAW);
                gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(icon.solid.size() / 7));
                gl::BindVertexArray(0);
            }
            if (!icon.sheet.empty() && gtex) {
                gl::UseProgram(progHumTex);
                gl::Uniform1f(uHumTexLit, 1.0f);
                gl::UniformMatrix4fv(uHumTexMVP, 1, GL_FALSE, mvp.m);
                gl::Uniform1i(uHumTexAtlas, 0);
                gl::Uniform3f(uHumTexSunDir, -0.35f, 0.85f, 0.40f);
                gl::Uniform3f(uHumTexSunColor, 0.55f, 0.55f, 0.52f);
                gl::Uniform3f(uHumTexAmbient, 0.55f, 0.55f, 0.55f);
                gl::Uniform3f(uHumTexFogColor, 0.0f, 0.0f, 0.0f);
                gl::Uniform1f(uHumTexFogDensity, 0.0f);
                gl::ActiveTexture(GL_TEXTURE0);
                gl::BindTexture(GL_TEXTURE_2D, gtex);
                gl::BindVertexArray(humTexVAO);
                gl::BindBuffer(GL_ARRAY_BUFFER, humTexVBO);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(icon.sheet.size() * sizeof(float)), icon.sheet.data(), GL_STREAM_DRAW);
                gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(icon.sheet.size() / 9));
                gl::BindVertexArray(0);
            }
            gl::Disable(GL_SCISSOR_TEST);
            gl::Viewport(0, 0, scrW, scrH);
            gl::Disable(GL_DEPTH_TEST);
            gl::Enable(GL_BLEND);
            gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            gl::UseProgram(0);
            return;
        }
    }
    drawModelItemIcon(block, mat::itemModel(block), x, y, size);
}

void Renderer::drawHumSolid(const std::vector<float>& solid, const Mat4& mvp, const Sky* sun, float fogDensity) {
    if (solid.empty() || !humVAO) return;
    gl::UseProgram(progHum);
    gl::UniformMatrix4fv(uHumMVP, 1, GL_FALSE, mvp.m);
    if (sun) {
        gl::Uniform1f(uHumLit, 1.0f);
        gl::Uniform3f(uHumSunDir, sun->sunDir.x, sun->sunDir.y, sun->sunDir.z);
        gl::Uniform3f(uHumSunColor, sun->sunColor.x, sun->sunColor.y, sun->sunColor.z);
        gl::Uniform3f(uHumAmbient, sun->ambient.x, sun->ambient.y, sun->ambient.z);
        gl::Uniform3f(uHumFogColor, sun->fogColor.x, sun->fogColor.y, sun->fogColor.z);
        gl::Uniform1f(uHumFogDensity, fogDensity);
    } else {
        gl::Uniform1f(uHumLit, 1.0f);
        gl::Uniform3f(uHumSunDir, -0.35f, 0.85f, 0.40f);
        gl::Uniform3f(uHumSunColor, 0.55f, 0.55f, 0.52f);
        gl::Uniform3f(uHumAmbient, 0.55f, 0.55f, 0.55f);
        gl::Uniform3f(uHumFogColor, 0.0f, 0.0f, 0.0f);
        gl::Uniform1f(uHumFogDensity, 0.0f);
    }
    gl::Disable(GL_CULL_FACE);
    gl::BindVertexArray(humVAO);
    gl::BindBuffer(GL_ARRAY_BUFFER, humVBO);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(solid.size() * sizeof(float)), solid.data(), GL_STREAM_DRAW);
    gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(solid.size() / 7));
    gl::BindVertexArray(0);
}

void Renderer::drawModelItemIcon(uint8_t block, const mat::Model& model, float x, float y, float size) {
    if (!fallVAO || size < 2.0f) return;
    std::vector<Vertex> mesh;
    mat::buildItemDisplayMesh(block, model, mesh);
    std::vector<float> solid;
    if (!model.solids.empty()) {
        mat::emitSolidMesh(model.solids, solid, [](float x, float y, float z) {
            return Vec3{ x, y, z };
        }, false);
    }
    if (mesh.empty() && solid.empty()) return;

    int vx = (int)std::floor(x);
    int vy = scrH - (int)std::floor(y + size);
    int vw = std::max(1, (int)std::floor(size));
    int vh = std::max(1, (int)std::floor(size));

    Mat4 proj = Mat4::perspective(32.0f, 1.0f, 0.05f, 8.0f);
    Mat4 view = Mat4::lookAt({ 1.55f, 1.25f, 1.55f }, { 0.50f, 0.48f, 0.50f }, { 0.0f, 1.0f, 0.0f });
    Mat4 mvp = proj * view;

    gl::Enable(GL_SCISSOR_TEST);
    gl::Scissor(vx, vy, vw, vh);
    gl::Viewport(vx, vy, vw, vh);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_TRUE);
    gl::Clear(GL_DEPTH_BUFFER_BIT);
    gl::Disable(GL_CULL_FACE);
    gl::Disable(GL_BLEND);

    gl::UseProgram(progWorld);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, atlasTex);
    gl::Uniform1i(uAtlas, 0);
    gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
    gl::Uniform3f(uChunkOffset, 0.0f, 0.0f, 0.0f);
    gl::Uniform1f(uBlockScale, 1.0f);
    gl::Uniform1f(uFogDensity, 0.0f);
    gl::Uniform3f(uSunDir, -0.35f, 0.85f, 0.40f);
    gl::Uniform3f(uSunColor, 0.55f, 0.55f, 0.52f);
    gl::Uniform3f(uAmbient, 0.55f, 0.55f, 0.55f);

    if (!mesh.empty()) {
        gl::BindVertexArray(fallVAO);
        gl::BindBuffer(GL_ARRAY_BUFFER, fallVBO);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh.size() * sizeof(Vertex)), mesh.data(), GL_STREAM_DRAW);
        setVertexAttribs();
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)mesh.size());
        gl::BindVertexArray(0);
    }
    drawHumSolid(solid, mvp, nullptr, 0.0f);

    gl::Uniform1f(uBlockScale, cfg::BLOCK_SCALE);
    gl::Uniform1f(uFogDensity, cfg::FOG_DENSITY);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Viewport(0, 0, scrW, scrH);
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_CULL_FACE);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::UseProgram(0);
}

void Renderer::buttonChrome(float x, float y, float w, float h, bool hovered,
                            float fr, float fg, float fb) {
    quad(x, y, w, h, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);              // outer border
    float k = hovered ? 1.32f : 1.0f;
    float r = std::min(1.0f, fr * k);
    float g = std::min(1.0f, fg * k);
    float b = std::min(1.0f, fb * k);
    quad(x + 2, y + 2, w - 4, h - 4, 0, 0, 0, 0, r, g, b, 1.0f);          // fill
    quad(x + 2, y + 2, w - 4, 2, 0, 0, 0, 0, 0.85f, 0.85f, 0.85f, 1.0f); // top bevel
    quad(x + 2, y + 2, 2, h - 4, 0, 0, 0, 0, 0.85f, 0.85f, 0.85f, 1.0f); // left bevel
    quad(x + 2, y + h - 4, w - 4, 2, 0, 0, 0, 0, 0.22f, 0.22f, 0.22f, 1.0f);
    quad(x + w - 4, y + 2, 2, h - 4, 0, 0, 0, 0, 0.22f, 0.22f, 0.22f, 1.0f);
}

void Renderer::drawMenu(UIState& ui) {
    const float bw = 340.0f, bh = 56.0f, gap = 14.0f;
    const float bx = (scrW - bw) * 0.5f;
    const float by0 = scrH * 0.5f - 70.0f;
    const float titleY = scrH * 0.5f - 190.0f;
    const float ts = 1.3f;

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.55f);
    const int nBtn = ui.roomSession ? 3 : 4;
    ui.timeSliderW = 0.0f;
    if (ui.structureEdit) {
        const float sliderW = 380.0f, sliderH = 14.0f;
        const float sliderX = (scrW - sliderW) * 0.5f;
        const float timeY = scrH * 0.5f - 122.0f;
        float frac = clampf(ui.timeOfDay / (float)cfg::TICKS_PER_DAY, 0.0f, 1.0f);
        quad(sliderX - 3, timeY - 3, sliderW + 6, sliderH + 6, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
        quad(sliderX, timeY, sliderW, sliderH, 0, 0, 0, 0, 0.22f, 0.22f, 0.22f, 1.0f);
        quad(sliderX, timeY, sliderW * frac, sliderH, 0, 0, 0, 0, 0.55f, 0.75f, 0.35f, 1.0f);
        float hx = sliderX + sliderW * frac;
        quad(hx - 8, timeY - 8, 16, sliderH + 16, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
        quad(hx - 6, timeY - 6, 12, sliderH + 12, 0, 0, 0, 0, 0.92f, 0.92f, 0.92f, 1.0f);
        ui.timeSliderX = sliderX;
        ui.timeSliderY = timeY;
        ui.timeSliderW = sliderW;
        ui.timeSliderH = sliderH;
    }
    ui.menuHover = -1;
    for (int i = 0; i < nBtn; i++) {
        float y = by0 + i * (bh + gap);
        bool hover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= y && ui.mouseY < y + bh);
        if (hover) ui.menuHover = i;
        buttonChrome(bx, y, bw, bh, hover);
    }
    flushUI(progUI, whiteTex);

    int tw, th;
    stringSize("VOXEL LEGEND", tw, th);
    drawBlockIcon(DIRT, scrW * 0.5f - (float)tw * ts * 0.5f - 44.0f, titleY - 16.0f, 32.0f);
    float rowX = scrW * 0.5f - 60.0f;
    float rowY = (float)scrH - 76.0f;
    drawBlockIcon(DIRT, rowX, rowY, 36.0f);
    drawBlockIcon(STONE, rowX + 42.0f, rowY, 36.0f);
    drawBlockIcon(PLANKS, rowX + 84.0f, rowY, 36.0f);
    flushUI(progUI, atlasTex);

    centeredText("VOXEL LEGEND", scrW * 0.5f, titleY, ts, 1, 1, 1, 1);
    if (ui.structureEdit && ui.timeSliderW > 1.0f) {
        centeredText("时间段（0:00 = 午夜）", scrW * 0.5f, ui.timeSliderY - 28.0f, 1.0f, 1, 1, 1, 1);
        int ticks = ((int)ui.timeOfDay % cfg::TICKS_PER_DAY + cfg::TICKS_PER_DAY) % cfg::TICKS_PER_DAY;
        int hours = ticks / 1000;
        int mins = (ticks % 1000) * 60 / 1000;
        char clock[16];
        snprintf(clock, sizeof(clock), "%02d:%02d", hours, mins);
        drawString(clock, ui.timeSliderX + ui.timeSliderW + 18.0f, ui.timeSliderY - 4.0f, 0.9f, 0.78f, 0.78f, 0.78f, 1.0f);
    }
    const char* labelsFree[4] = { "继续游戏", "设置", "调试菜单", "返回菜单" };
    const char* labelsRoom[3] = { "继续游戏", "设置", "返回菜单" };
    for (int i = 0; i < nBtn; i++) {
        float y = by0 + i * (bh + gap);
        const char* label = ui.roomSession ? labelsRoom[i] : labelsFree[i];
        centeredText(label, bx + bw * 0.5f, y + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    }
}

void Renderer::drawStartMenu(UIState& ui) {
    const float bw = 300.0f, bh = 52.0f, gap = 12.0f;
    const float bx = 72.0f;
    const float by0 = scrH * 0.5f - 70.0f;
    const float titleY = by0 - 92.0f;
    const char* labels[5] = { "创建房间", "自由探索", "加入房间", "游戏设置", "关于我们" };
    const bool enabled[5] = { true, true, true, false, false };
    float split = ui.portraitX;
    if (split < 64.0f) split = std::max(420.0f, scrW * 0.42f);

    scrimFade(split, (float)scrH);
    ui.startHover = -1;
    ui.portraitHover = false;
    for (int i = 0; i < 5; i++) {
        float y = by0 + i * (bh + gap);
        bool hover = enabled[i] && ui.mouseX >= bx && ui.mouseX < bx + bw &&
                     ui.mouseY >= y && ui.mouseY < y + bh;
        if (hover) ui.startHover = i;
        if (enabled[i]) buttonChrome(bx, y, bw, bh, hover);
        else buttonChrome(bx, y, bw, bh, false, 0.22f, 0.22f, 0.24f);
    }
    if (ui.mouseX >= split && ui.mouseX < (float)scrW && ui.mouseY >= 0.0f && ui.mouseY < (float)scrH)
        ui.portraitHover = true;
    flushUI(progUI, whiteTex);

    drawString("VOXEL LEGEND", bx, titleY, 1.5f, 1, 1, 1, 1);
    for (int i = 0; i < 5; i++) {
        float y = by0 + i * (bh + gap);
        float a = enabled[i] ? 1.0f : 0.38f;
        centeredText(labels[i], bx + bw * 0.5f, y + bh * 0.5f, 1.0f, a, a, a, 1);
    }
    std::string who = ui.playerName.empty() ? "玩家" : ui.playerName;
    centeredText(who, split + ((float)scrW - split) * 0.5f, (float)scrH - 48.0f, 1.0f, 1, 1, 1, 1);
    centeredText("点击角色设置", split + ((float)scrW - split) * 0.5f, (float)scrH - 24.0f, 0.7f, 0.75f, 0.78f, 0.82f, 1);
    if (!ui.menuMessage.empty())
        drawString(ui.menuMessage, bx, (float)scrH - 48.0f, 0.85f, 0.85f, 0.95f, 0.55f, 1);
}

void Renderer::drawPlayerProfile(UIState& ui) {
    const float bw = 360.0f, bh = 48.0f;
    const float bx = 72.0f;
    const float fieldY = scrH * 0.5f - 36.0f;
    const float importY = fieldY + 78.0f;
    const float backY = importY + bh + 14.0f;
    float split = ui.portraitX;
    if (split < 64.0f) split = std::max(420.0f, scrW * 0.42f);

    scrimFade(split, (float)scrH);
    ui.profileHover = -1;

    bool fieldHover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= fieldY && ui.mouseY < fieldY + bh;
    if (fieldHover) ui.profileHover = 0;
    quad(bx, fieldY, bw, bh, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
    float ff = (ui.nameFieldActive || fieldHover) ? 0.22f : 0.16f;
    quad(bx + 2, fieldY + 2, bw - 4, bh - 4, 0, 0, 0, 0, ff, ff, ff, 1.0f);
    if (ui.nameFieldActive)
        quad(bx, fieldY, bw, 2, 0, 0, 0, 0, 0.55f, 0.75f, 0.35f, 1.0f);

    bool importHover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= importY && ui.mouseY < importY + bh;
    bool backHover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= backY && ui.mouseY < backY + bh;
    if (importHover) ui.profileHover = 1;
    if (backHover) ui.profileHover = 2;
    buttonChrome(bx, importY, bw, bh, importHover);
    buttonChrome(bx, backY, bw, bh, backHover);
    flushUI(progUI, whiteTex);

    drawString("玩家信息", bx, fieldY - 88.0f, 1.3f, 1, 1, 1, 1);
    drawString("玩家名称", bx, fieldY - 28.0f, 0.85f, 0.8f, 0.8f, 0.8f, 1);
    std::string shown = ui.playerName;
    bool caret = ((int)(ui.timeOfDay / 200.0f) % 2) == 0;
    if (ui.nameFieldActive && caret) shown += "|";
    if (shown.empty())
        centeredText("点击输入", bx + bw * 0.5f, fieldY + bh * 0.5f, 0.95f, 0.5f, 0.5f, 0.5f, 1);
    else
        centeredText(shown, bx + bw * 0.5f, fieldY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("导入模型", bx + bw * 0.5f, importY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("返回", bx + bw * 0.5f, backY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    if (!ui.menuMessage.empty())
        drawString(ui.menuMessage, bx, (float)scrH - 40.0f, 0.85f, 0.95f, 0.75f, 0.45f, 1);
}

void Renderer::drawRoomLobby(UIState& ui) {
    const float sideW = 240.0f;
    const float margin = 16.0f;
    const float leftX = margin;
    const float rightX = (float)scrW - sideW - margin;
    const float midX = leftX + sideW + margin;
    const float midW = rightX - midX - margin;
    const float top = 72.0f;
    const float rowH = 48.0f;
    const float rowGap = 8.0f;
    const float btnH = 44.0f;
    const float newY = (float)scrH - 16.0f - btnH;
    const int nTeams = (int)ui.roomTeams.size();
    const int nPlayers = (int)ui.roomPlayers.size();
    const bool enoughPlayers = nPlayers >= ui.roomMinPlayers;
    const bool canStart = ui.roomHost && enoughPlayers;
    const bool canAddTeam = ui.roomHost && nTeams < 1 + matchmap::kCombatTeams;

    int lw = 0, lh = 0;
    stringSize("开放端口", lw, lh);
    const float portBoxW = 96.0f, portBoxH = 30.0f;
    const float portBoxX = rightX - portBoxW;
    const float portLabelX = portBoxX - (float)lw * 0.85f - 8.0f;
    const float portBoxY = 14.0f;
    ui.portFieldX = portBoxX;
    ui.portFieldY = portBoxY;
    ui.portFieldW = portBoxW;
    ui.portFieldH = portBoxH;
    ui.portFieldHover = ui.roomHost &&
                        ui.mouseX >= portBoxX && ui.mouseX < portBoxX + portBoxW &&
                        ui.mouseY >= portBoxY && ui.mouseY < portBoxY + portBoxH;

    int localTeam = -1;
    for (const RoomPlayerView& p : ui.roomPlayers)
        if (p.local) localTeam = p.team;

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.06f, 0.07f, 0.09f, 1.0f);
    quad(leftX, top, sideW, newY - top - 10.0f, 0, 0, 0, 0, 0.10f, 0.11f, 0.13f, 1.0f);
    quad(rightX, top, sideW, (float)scrH - top - margin, 0, 0, 0, 0, 0.10f, 0.11f, 0.13f, 1.0f);
    quad(midX, top, midW, (float)scrH - top - 78.0f, 0, 0, 0, 0, 0.09f, 0.10f, 0.12f, 1.0f);

    ui.lobbyJoinHover = -1;
    ui.lobbyBtnHover = -1;

    std::vector<float> camX, camY, camS;

    const float flagW = 16.0f, flagH = 26.0f;
    for (int i = 0; i < nTeams; i++) {
        const RoomTeamView& t = ui.roomTeams[i];
        float y = top + 36.0f + i * (rowH + rowGap);
        float iconX = leftX + 14.0f;
        float iconY = y + (rowH - flagH) * 0.5f;
        if (t.spectator) {
            camX.push_back(iconX);
            camY.push_back(iconY);
            camS.push_back(flagH);
        } else {
            drawFlag(iconX + 2.0f, iconY, flagW, flagH, t.r, t.g, t.b);
        }

        float cy = top + 36.0f + i * (rowH + rowGap);
        float cIconX = midX + 16.0f;
        float cIconY = cy + (rowH - flagH) * 0.5f;
        if (localTeam == i)
            quad(midX + 4.0f, cy, 4.0f, rowH, 0, 0, 0, 0, t.r, t.g, t.b, 1.0f);
        if (t.spectator) {
            camX.push_back(cIconX);
            camY.push_back(cIconY);
            camS.push_back(flagH);
        } else {
            drawFlag(cIconX + 2.0f, cIconY, flagW, flagH, t.r, t.g, t.b);
        }
        float plus = 28.0f;
        float plusX = cIconX + flagW + 16.0f;
        float plusY = cy + (rowH - plus) * 0.5f;
        bool plusHover = ui.mouseX >= plusX && ui.mouseX < plusX + plus &&
                         ui.mouseY >= plusY && ui.mouseY < plusY + plus;
        if (plusHover) ui.lobbyJoinHover = i;
        buttonChrome(plusX, plusY, plus, plus, plusHover, t.spectator ? 0.45f : t.r,
                     t.spectator ? 0.45f : t.g, t.spectator ? 0.48f : t.b);
    }

    bool newHover = canAddTeam && ui.mouseX >= leftX && ui.mouseX < leftX + sideW &&
                    ui.mouseY >= newY && ui.mouseY < newY + btnH;
    if (newHover) ui.lobbyBtnHover = 0;
    if (canAddTeam) buttonChrome(leftX, newY, sideW, btnH, newHover);
    else buttonChrome(leftX, newY, sideW, btnH, false, 0.22f, 0.22f, 0.24f);

    float startW = 200.0f;
    float startX = midX + (midW - startW) * 0.5f;
    float backW = 140.0f;
    float backX = midX;
    float barY = (float)scrH - 16.0f - btnH;
    bool startHover = canStart && ui.mouseX >= startX && ui.mouseX < startX + startW &&
                      ui.mouseY >= barY && ui.mouseY < barY + btnH;
    bool backHover = ui.mouseX >= backX && ui.mouseX < backX + backW &&
                     ui.mouseY >= barY && ui.mouseY < barY + btnH;
    if (startHover) ui.lobbyBtnHover = 1;
    if (backHover) ui.lobbyBtnHover = 2;
    if (canStart) buttonChrome(startX, barY, startW, btnH, startHover, 0.28f, 0.48f, 0.24f);
    else buttonChrome(startX, barY, startW, btnH, false, 0.22f, 0.22f, 0.24f);
    buttonChrome(backX, barY, backW, btnH, backHover);
    {
        float br = ui.portFieldActive ? 0.22f : 0.14f;
        if (ui.portFieldHover) br = 0.28f;
        quad(portBoxX, portBoxY, portBoxW, portBoxH, 0, 0, 0, 0, br, br, br + 0.02f, 1.0f);
        if (ui.portFieldActive)
            quad(portBoxX, portBoxY, portBoxW, 2, 0, 0, 0, 0, 0.55f, 0.75f, 0.35f, 1.0f);
    }
    flushUI(progUI, whiteTex);

    if (cameraIconTex && !camX.empty()) {
        for (size_t i = 0; i < camX.size(); i++)
            quad(camX[i], camY[i], camS[i], camS[i], 0, 0, 1, 1, 1, 1, 1, 1);
        flushUI(progUI, cameraIconTex);
    }

    std::string title = ui.roomHost ? "等待开始  ·  主机" : "等待开始";
    drawString(title, 24.0f, 22.0f, 1.2f, 1, 1, 1, 1);
    drawString("开放端口", portLabelX, 20.0f, 0.85f, 0.8f, 0.82f, 0.78f, 1);
    std::string portShown = ui.roomPortText.empty() ? std::to_string(ui.roomPort) : ui.roomPortText;
    bool caret = ((int)(ui.timeOfDay / 200.0f) % 2) == 0;
    if (ui.portFieldActive && caret) portShown += "|";
    centeredText(portShown, portBoxX + portBoxW * 0.5f, portBoxY + portBoxH * 0.5f, 0.9f, 1, 1, 1, 1);
    char countBuf[64];
    snprintf(countBuf, sizeof(countBuf), "房间人数 %d / %d", nPlayers, ui.roomMinPlayers);
    drawString(countBuf, midX, 28.0f, 0.85f, enoughPlayers ? 0.75f : 0.95f, enoughPlayers ? 0.9f : 0.55f, enoughPlayers ? 0.55f : 0.4f, 1);

    drawString("队伍", leftX + 14.0f, top + 8.0f, 0.9f, 0.85f, 0.85f, 0.85f, 1);
    drawString("房间内玩家", rightX + 14.0f, top + 8.0f, 0.9f, 0.85f, 0.85f, 0.85f, 1);

    for (int i = 0; i < nTeams; i++) {
        const RoomTeamView& t = ui.roomTeams[i];
        float y = top + 36.0f + i * (rowH + rowGap);
        drawString(t.name, leftX + 48.0f, y + 12.0f, 0.85f, 1, 1, 1, 1);
        float plus = 28.0f;
        float plusX = midX + 16.0f + flagW + 16.0f;
        centeredText("+", plusX + plus * 0.5f, y + rowH * 0.5f, 1.1f, 1, 1, 1, 1);
        float nameX = plusX + plus + 12.0f;
        bool any = false;
        for (const RoomPlayerView& p : ui.roomPlayers) {
            if (p.team != i) continue;
            any = true;
            drawString(p.name, nameX, y + 12.0f, 0.85f, 1, 1, 1, 1);
            int tw = 0, th = 0;
            stringSize(p.name, tw, th);
            nameX += (float)tw * 0.85f + 8.0f;
            if (p.host) {
                drawString("主机", nameX, y + 12.0f, 0.75f, 0.95f, 0.78f, 0.35f, 1);
                stringSize("主机", tw, th);
                nameX += (float)tw * 0.75f + 16.0f;
            } else {
                nameX += 8.0f;
            }
        }
        if (!any)
            drawString("空", nameX, y + 14.0f, 0.75f, 0.45f, 0.45f, 0.48f, 1);
    }

    float py = top + 40.0f;
    for (const RoomPlayerView& p : ui.roomPlayers) {
        float cr = 0.7f, cg = 0.7f, cb = 0.72f;
        if (p.team >= 0 && p.team < nTeams) {
            cr = ui.roomTeams[p.team].r;
            cg = ui.roomTeams[p.team].g;
            cb = ui.roomTeams[p.team].b;
        }
        quad(rightX + 14.0f, py + 4.0f, 8.0f, 18.0f, 0, 0, 0, 0, cr, cg, cb, 1);
        flushUI(progUI, whiteTex);
        std::string label = p.name;
        if (p.host) label += "  主机";
        if (p.team < 0) label += "  未入队";
        else if (p.team < nTeams) label += "  " + ui.roomTeams[p.team].name;
        drawString(label, rightX + 30.0f, py, 0.8f, 1, 1, 1, 1);
        py += 28.0f;
    }

    const char* newLabel = canAddTeam ? "新建队伍" : (ui.roomHost ? "队伍已满" : "仅主机可新建");
    centeredText(newLabel, leftX + sideW * 0.5f, newY + btnH * 0.5f,
                 0.9f, canAddTeam ? 1.0f : 0.4f, canAddTeam ? 1.0f : 0.4f, canAddTeam ? 1.0f : 0.4f, 1);
    centeredText("返回", backX + backW * 0.5f, barY + btnH * 0.5f, 0.95f, 1, 1, 1, 1);
    const char* startLabel = "等待主机";
    if (ui.roomHost) startLabel = canStart ? "开始对局" : "人数不足";
    float sa = canStart ? 1.0f : 0.4f;
    centeredText(startLabel, startX + startW * 0.5f, barY + btnH * 0.5f, 0.95f, sa, sa, sa, 1);
    if (!ui.menuMessage.empty())
        drawString(ui.menuMessage, midX, barY - 28.0f, 0.8f, 0.95f, 0.75f, 0.45f, 1);
}

void Renderer::drawJoinRoom(UIState& ui) {
    const float bw = 360.0f, bh = 48.0f;
    const float bx = 72.0f;
    const float addrY = scrH * 0.5f - 70.0f;
    const float portY = addrY + 78.0f;
    const float goY = portY + 78.0f;
    const float backY = goY + bh + 14.0f;

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.06f, 0.07f, 0.09f, 1.0f);
    ui.joinHover = -1;

    auto field = [&](float y, bool active, int id) {
        bool hover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= y && ui.mouseY < y + bh;
        if (hover) ui.joinHover = id;
        quad(bx, y, bw, bh, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
        float f = (active || hover) ? 0.22f : 0.16f;
        quad(bx + 2, y + 2, bw - 4, bh - 4, 0, 0, 0, 0, f, f, f, 1.0f);
        if (active) quad(bx, y, bw, 2, 0, 0, 0, 0, 0.55f, 0.75f, 0.35f, 1.0f);
    };
    field(addrY, ui.joinAddrActive, 0);
    field(portY, ui.joinPortActive, 1);

    bool goHover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= goY && ui.mouseY < goY + bh;
    bool backHover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= backY && ui.mouseY < backY + bh;
    if (goHover) ui.joinHover = 2;
    if (backHover) ui.joinHover = 3;
    buttonChrome(bx, goY, bw, bh, goHover, 0.28f, 0.48f, 0.24f);
    buttonChrome(bx, backY, bw, bh, backHover);
    flushUI(progUI, whiteTex);

    drawString("加入房间", bx, addrY - 88.0f, 1.3f, 1, 1, 1, 1);
    drawString("主机地址", bx, addrY - 28.0f, 0.85f, 0.8f, 0.8f, 0.8f, 1);
    drawString("端口", bx, portY - 28.0f, 0.85f, 0.8f, 0.8f, 0.8f, 1);
    bool caret = ((int)(ui.timeOfDay / 200.0f) % 2) == 0;
    auto show = [&](const std::string& text, bool active, float y, const char* empty) {
        std::string s = text;
        if (active && caret) s += "|";
        if (s.empty())
            centeredText(empty, bx + bw * 0.5f, y + bh * 0.5f, 0.95f, 0.5f, 0.5f, 0.5f, 1);
        else
            centeredText(s, bx + bw * 0.5f, y + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    };
    show(ui.joinHost, ui.joinAddrActive, addrY, "127.0.0.1");
    show(ui.joinPortText, ui.joinPortActive, portY, "35535");
    centeredText("连接", bx + bw * 0.5f, goY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("返回", bx + bw * 0.5f, backY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    if (!ui.menuMessage.empty())
        drawString(ui.menuMessage, bx, (float)scrH - 40.0f, 0.85f, 0.95f, 0.75f, 0.45f, 1);
}

void Renderer::drawRoomLoading(UIState& ui) {
    const float bw = 220.0f, bh = 48.0f;
    const float bx = ((float)scrW - bw) * 0.5f;
    const float by = (float)scrH * 0.5f + 48.0f;

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.05f, 0.06f, 0.08f, 1.0f);
    ui.loadHover = -1;
    bool hover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= by && ui.mouseY < by + bh;
    if (hover) ui.loadHover = 0;
    buttonChrome(bx, by, bw, bh, hover);
    flushUI(progUI, whiteTex);

    centeredText("正在进入房间", (float)scrW * 0.5f, (float)scrH * 0.5f - 36.0f, 1.3f, 1, 1, 1, 1);
    std::string status = ui.loadStatus.empty() ? "正在加载" : ui.loadStatus;
    centeredText(status, (float)scrW * 0.5f, (float)scrH * 0.5f + 4.0f, 0.95f, 0.8f, 0.84f, 0.78f, 1);
    centeredText("取消", bx + bw * 0.5f, by + bh * 0.5f, 1.0f, 1, 1, 1, 1);
}

void Renderer::drawWorldsMenu(UIState& ui) {
    const float bw = 460.0f, bh = 48.0f, gap = 8.0f;
    const float bx = (scrW - bw) * 0.5f;
    const float titleY = 56.0f;
    const int vis = 6;
    const float listY = 110.0f;
    const float btnH = 52.0f;
    const float btnW = 200.0f;
    const float delW = 96.0f;
    const float nameW = bw - delW - 10.0f;

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.28f);
    ui.worldItemHover = -1;
    ui.worldDeleteHover = -1;
    ui.worldsBtnHover = -1;

    int n = (int)ui.worldNames.size();
    int maxScroll = std::max(0, n - vis);
    if (ui.worldScroll < 0) ui.worldScroll = 0;
    if (ui.worldScroll > maxScroll) ui.worldScroll = maxScroll;

    for (int i = 0; i < vis; i++) {
        int idx = ui.worldScroll + i;
        float y = listY + i * (bh + gap);
        if (idx >= n) {
            quad(bx, y, bw, bh, 0, 0, 0, 0, 0.10f, 0.10f, 0.10f, 0.35f);
            continue;
        }
        float delX = bx + nameW + 10.0f;
        bool delHover = (ui.mouseX >= delX && ui.mouseX < delX + delW &&
                         ui.mouseY >= y && ui.mouseY < y + bh);
        bool nameHover = !delHover && (ui.mouseX >= bx && ui.mouseX < bx + nameW &&
                                       ui.mouseY >= y && ui.mouseY < y + bh);
        if (delHover) ui.worldDeleteHover = idx;
        if (nameHover) ui.worldItemHover = idx;
        buttonChrome(bx, y, nameW, bh, nameHover);
        bool armed = (ui.pendingDelete == ui.worldNames[idx]);
        buttonChrome(delX, y, delW, bh, delHover,
                     armed ? 0.62f : 0.50f,
                     armed ? 0.16f : 0.22f,
                     armed ? 0.14f : 0.18f);
    }

    float btnY = listY + vis * (bh + gap) + 18.0f;
    float createX = scrW * 0.5f - btnW - 12.0f;
    float backX = scrW * 0.5f + 12.0f;
    bool createHover = (ui.mouseX >= createX && ui.mouseX < createX + btnW && ui.mouseY >= btnY && ui.mouseY < btnY + btnH);
    bool backHover = (ui.mouseX >= backX && ui.mouseX < backX + btnW && ui.mouseY >= btnY && ui.mouseY < btnY + btnH);
    if (createHover) ui.worldsBtnHover = 0;
    if (backHover) ui.worldsBtnHover = 1;
    buttonChrome(createX, btnY, btnW, btnH, createHover);
    buttonChrome(backX, btnY, btnW, btnH, backHover);
    flushUI(progUI, whiteTex);

    centeredText("选择世界", scrW * 0.5f, titleY, 1.3f, 1, 1, 1, 1);
    if (n == 0)
        centeredText("还没有世界，点击下方创建", scrW * 0.5f, listY + vis * (bh + gap) * 0.4f, 0.9f, 0.75f, 0.75f, 0.75f, 1);
    for (int i = 0; i < vis; i++) {
        int idx = ui.worldScroll + i;
        if (idx >= n) break;
        float y = listY + i * (bh + gap);
        float delX = bx + nameW + 10.0f;
        bool armed = (ui.pendingDelete == ui.worldNames[idx]);
        centeredText(ui.worldNames[idx], bx + nameW * 0.5f, y + bh * 0.5f, 1.0f, 1, 1, 1, 1);
        centeredText(armed ? "确认" : "删除", delX + delW * 0.5f, y + bh * 0.5f, 0.95f, 1.0f, 0.82f, 0.78f, 1);
    }
    centeredText("创建世界", createX + btnW * 0.5f, btnY + btnH * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("返回", backX + btnW * 0.5f, btnY + btnH * 0.5f, 1.0f, 1, 1, 1, 1);
    if (n > vis)
        centeredText("滚轮翻页", scrW * 0.5f, btnY + btnH + 22.0f, 0.75f, 0.7f, 0.7f, 0.7f, 1);
    if (!ui.menuMessage.empty())
        centeredText(ui.menuMessage, scrW * 0.5f, (float)scrH - 28.0f, 0.85f, 0.85f, 0.95f, 0.55f, 1);
}

void Renderer::drawStructurePicker(UIState& ui) {
    ui.structureItemHover = -1;
    ui.structureDeleteHover = -1;
    ui.structureBtnHover = -1;
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.05f, 0.05f, 0.06f, 0.94f);

    if (ui.structureNaming) {
        const float bw = 420.0f, bh = 52.0f;
        const float bx = (scrW - bw) * 0.5f;
        const float fieldY = scrH * 0.5f - 40.0f;
        const float fieldH = 48.0f;
        const float createY = fieldY + 80.0f;
        const float backY = createY + bh + 14.0f;

        bool fieldHover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= fieldY && ui.mouseY < fieldY + fieldH);
        if (fieldHover) ui.structureBtnHover = 0;
        quad(bx, fieldY, bw, fieldH, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
        float ff = (ui.nameFieldActive || fieldHover) ? 0.22f : 0.16f;
        quad(bx + 2, fieldY + 2, bw - 4, fieldH - 4, 0, 0, 0, 0, ff, ff, ff, 1.0f);
        if (ui.nameFieldActive)
            quad(bx, fieldY, bw, 2, 0, 0, 0, 0, 0.55f, 0.75f, 0.35f, 1.0f);

        bool createHover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= createY && ui.mouseY < createY + bh);
        bool backHover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= backY && ui.mouseY < backY + bh);
        if (createHover) ui.structureBtnHover = 1;
        if (backHover) ui.structureBtnHover = 2;
        buttonChrome(bx, createY, bw, bh, createHover, 0.28f, 0.48f, 0.24f);
        buttonChrome(bx, backY, bw, bh, backHover);
        flushUI(progUI, whiteTex);

        centeredText("新建建筑", scrW * 0.5f, scrH * 0.5f - 130.0f, 1.3f, 1, 1, 1, 1);
        centeredText("建筑名称", scrW * 0.5f, fieldY - 22.0f, 0.9f, 0.85f, 0.85f, 0.85f, 1);
        std::string shown = ui.structureNewName;
        bool caret = ((int)(ui.timeOfDay / 10.0f) % 2) == 0;
        if (ui.nameFieldActive && caret) shown += "|";
        if (shown.empty())
            centeredText("点击输入", bx + bw * 0.5f, fieldY + fieldH * 0.5f, 0.95f, 0.5f, 0.5f, 0.5f, 1);
        else
            centeredText(shown, bx + bw * 0.5f, fieldY + fieldH * 0.5f, 1.0f, 1, 1, 1, 1);
        centeredText("创建", bx + bw * 0.5f, createY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
        centeredText("返回", bx + bw * 0.5f, backY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
        if (!ui.menuMessage.empty())
            centeredText(ui.menuMessage, scrW * 0.5f, (float)scrH - 28.0f, 0.85f, 0.85f, 0.95f, 0.55f, 1);
        return;
    }

    const float bw = 460.0f, bh = 48.0f, gap = 8.0f;
    const float bx = (scrW - bw) * 0.5f;
    const float titleY = 56.0f;
    const int vis = 6;
    const float listY = 118.0f;
    const float btnH = 52.0f;
    const float btnW = 200.0f;
    const float delW = 96.0f;
    const float nameW = bw - delW - 10.0f;

    int n = (int)ui.structureNames.size();
    int maxScroll = std::max(0, n - vis);
    if (ui.structureScroll < 0) ui.structureScroll = 0;
    if (ui.structureScroll > maxScroll) ui.structureScroll = maxScroll;

    for (int i = 0; i < vis; i++) {
        int idx = ui.structureScroll + i;
        float y = listY + i * (bh + gap);
        if (idx >= n) {
            quad(bx, y, bw, bh, 0, 0, 0, 0, 0.10f, 0.10f, 0.10f, 0.35f);
            continue;
        }
        float delX = bx + nameW + 10.0f;
        bool delHover = (ui.mouseX >= delX && ui.mouseX < delX + delW &&
                         ui.mouseY >= y && ui.mouseY < y + bh);
        bool nameHover = !delHover && (ui.mouseX >= bx && ui.mouseX < bx + nameW &&
                                       ui.mouseY >= y && ui.mouseY < y + bh);
        if (delHover) ui.structureDeleteHover = idx;
        if (nameHover) ui.structureItemHover = idx;
        buttonChrome(bx, y, nameW, bh, nameHover);
        bool armed = (ui.structurePendingDelete == ui.structureNames[idx]);
        buttonChrome(delX, y, delW, bh, delHover,
                     armed ? 0.62f : 0.50f,
                     armed ? 0.16f : 0.22f,
                     armed ? 0.14f : 0.18f);
    }

    float btnY = listY + vis * (bh + gap) + 18.0f;
    float createX = ui.structureCanReturn ? (scrW * 0.5f - btnW - 12.0f) : ((scrW - btnW) * 0.5f);
    float backX = scrW * 0.5f + 12.0f;
    bool createHover = (ui.mouseX >= createX && ui.mouseX < createX + btnW && ui.mouseY >= btnY && ui.mouseY < btnY + btnH);
    bool backHover = ui.structureCanReturn &&
        (ui.mouseX >= backX && ui.mouseX < backX + btnW && ui.mouseY >= btnY && ui.mouseY < btnY + btnH);
    if (createHover) ui.structureBtnHover = 0;
    if (backHover) ui.structureBtnHover = 1;
    buttonChrome(createX, btnY, btnW, btnH, createHover, 0.28f, 0.48f, 0.24f);
    if (ui.structureCanReturn) buttonChrome(backX, btnY, btnW, btnH, backHover);
    flushUI(progUI, whiteTex);

    centeredText("选择建筑", scrW * 0.5f, titleY, 1.3f, 1, 1, 1, 1);
    if (ui.structureCanReturn)
        centeredText("载入后会替换当前建筑，未保存的修改会丢失", scrW * 0.5f, titleY + 28.0f, 0.75f, 0.75f, 0.75f, 0.7f, 1);
    if (n == 0)
        centeredText("还没有建筑，点击下方新建", scrW * 0.5f, listY + vis * (bh + gap) * 0.4f, 0.9f, 0.75f, 0.75f, 0.75f, 1);
    for (int i = 0; i < vis; i++) {
        int idx = ui.structureScroll + i;
        if (idx >= n) break;
        float y = listY + i * (bh + gap);
        float delX = bx + nameW + 10.0f;
        bool armed = (ui.structurePendingDelete == ui.structureNames[idx]);
        centeredText(ui.structureNames[idx], bx + nameW * 0.5f, y + bh * 0.5f, 1.0f, 1, 1, 1, 1);
        centeredText(armed ? "确认" : "删除", delX + delW * 0.5f, y + bh * 0.5f, 0.95f, 1.0f, 0.82f, 0.78f, 1);
    }
    centeredText("新建", createX + btnW * 0.5f, btnY + btnH * 0.5f, 1.0f, 1, 1, 1, 1);
    if (ui.structureCanReturn)
        centeredText("返回", backX + btnW * 0.5f, btnY + btnH * 0.5f, 1.0f, 1, 1, 1, 1);
    if (n > vis)
        centeredText("滚轮翻页", scrW * 0.5f, btnY + btnH + 22.0f, 0.75f, 0.7f, 0.7f, 0.7f, 1);
    if (!ui.menuMessage.empty())
        centeredText(ui.menuMessage, scrW * 0.5f, (float)scrH - 28.0f, 0.85f, 0.85f, 0.95f, 0.55f, 1);
}

void Renderer::drawWorldDetail(UIState& ui) {
    const float bw = 400.0f, bh = 46.0f, gap = 8.0f;
    const float bx = (scrW - bw) * 0.5f;
    const float titleY = 48.0f;
    const float btn0 = 88.0f;
    const int vis = 5;
    const float rowH = 40.0f, rowGap = 6.0f;
    const int nBtn = 5;

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.28f);
    ui.detailBtnHover = -1;
    ui.backupItemHover = -1;

    const char* labels[5] = { "进入世界", "手动备份", "使用备份启动", "删除世界", "返回" };
    if (ui.deleteArmed) labels[3] = "再点一次确认删除";
    for (int i = 0; i < nBtn; i++) {
        float y = btn0 + i * (bh + gap);
        bool hover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= y && ui.mouseY < y + bh);
        if (hover) ui.detailBtnHover = i;
        bool dim = (i == 2 && ui.selectedBackup < 0);
        if (i == 3)
            buttonChrome(bx, y, bw, bh, hover,
                         ui.deleteArmed ? 0.62f : 0.50f,
                         ui.deleteArmed ? 0.16f : 0.22f,
                         ui.deleteArmed ? 0.14f : 0.18f);
        else
            buttonChrome(bx, y, bw, bh, hover && !dim);
    }

    float listY = btn0 + nBtn * (bh + gap) + 24.0f;
    int n = (int)ui.backupNames.size();
    int maxScroll = std::max(0, n - vis);
    if (ui.backupScroll < 0) ui.backupScroll = 0;
    if (ui.backupScroll > maxScroll) ui.backupScroll = maxScroll;
    for (int i = 0; i < vis; i++) {
        int idx = ui.backupScroll + i;
        float y = listY + i * (rowH + rowGap);
        if (idx >= n) {
            quad(bx, y, bw, rowH, 0, 0, 0, 0, 0.10f, 0.10f, 0.10f, 0.30f);
            continue;
        }
        bool hover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= y && ui.mouseY < y + rowH);
        if (hover) ui.backupItemHover = idx;
        bool sel = (idx == ui.selectedBackup);
        buttonChrome(bx, y, bw, rowH, hover || sel);
        if (sel) {
            quad(bx, y, 6, rowH, 0, 0, 0, 0, 0.45f, 0.75f, 0.30f, 1.0f);
        }
    }
    flushUI(progUI, whiteTex);

    centeredText(ui.selectedWorld.empty() ? "世界" : ui.selectedWorld, scrW * 0.5f, titleY, 1.25f, 1, 1, 1, 1);
    for (int i = 0; i < nBtn; i++) {
        float y = btn0 + i * (bh + gap);
        float dim = (i == 2 && ui.selectedBackup < 0) ? 0.55f : 1.0f;
        if (i == 3)
            centeredText(labels[i], bx + bw * 0.5f, y + bh * 0.5f, 1.0f, 1.0f, 0.82f, 0.78f, 1);
        else
            centeredText(labels[i], bx + bw * 0.5f, y + bh * 0.5f, 1.0f, dim, dim, dim, 1);
    }
    centeredText("备份列表（点击选中）", scrW * 0.5f, listY - 18.0f, 0.85f, 0.8f, 0.8f, 0.8f, 1);
    for (int i = 0; i < vis; i++) {
        int idx = ui.backupScroll + i;
        if (idx >= n) break;
        float y = listY + i * (rowH + rowGap);
        centeredText(ui.backupNames[idx], bx + bw * 0.5f, y + rowH * 0.5f, 0.9f, 1, 1, 1, 1);
    }
    if (n == 0)
        centeredText("暂无备份", scrW * 0.5f, listY + vis * (rowH + rowGap) * 0.35f, 0.85f, 0.7f, 0.7f, 0.7f, 1);
    if (!ui.menuMessage.empty())
        centeredText(ui.menuMessage, scrW * 0.5f, (float)scrH - 24.0f, 0.85f, 0.85f, 0.95f, 0.55f, 1);
}

void Renderer::drawCreateWorld(UIState& ui) {
    const float bw = 420.0f, bh = 52.0f;
    const float bx = (scrW - bw) * 0.5f;
    const float fieldY = scrH * 0.5f - 40.0f;
    const float fieldH = 48.0f;
    const float createY = fieldY + 80.0f;
    const float backY = createY + bh + 14.0f;

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.28f);
    ui.createBtnHover = -1;

    bool fieldHover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= fieldY && ui.mouseY < fieldY + fieldH);
    if (fieldHover) ui.createBtnHover = 0;
    quad(bx, fieldY, bw, fieldH, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
    float ff = (ui.nameFieldActive || fieldHover) ? 0.22f : 0.16f;
    quad(bx + 2, fieldY + 2, bw - 4, fieldH - 4, 0, 0, 0, 0, ff, ff, ff, 1.0f);
    if (ui.nameFieldActive)
        quad(bx, fieldY, bw, 2, 0, 0, 0, 0, 0.55f, 0.75f, 0.35f, 1.0f);

    bool createHover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= createY && ui.mouseY < createY + bh);
    bool backHover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= backY && ui.mouseY < backY + bh);
    if (createHover) ui.createBtnHover = 1;
    if (backHover) ui.createBtnHover = 2;
    buttonChrome(bx, createY, bw, bh, createHover);
    buttonChrome(bx, backY, bw, bh, backHover);
    flushUI(progUI, whiteTex);

    centeredText("创建世界", scrW * 0.5f, scrH * 0.5f - 130.0f, 1.3f, 1, 1, 1, 1);
    centeredText("世界名称", scrW * 0.5f, fieldY - 22.0f, 0.9f, 0.85f, 0.85f, 0.85f, 1);

    std::string shown = ui.newWorldName;
    bool caret = ((int)(ui.timeOfDay / 200.0f) % 2) == 0;
    if (ui.nameFieldActive && caret) shown += "|";
    if (shown.empty())
        centeredText("点击输入", bx + bw * 0.5f, fieldY + fieldH * 0.5f, 0.95f, 0.5f, 0.5f, 0.5f, 1);
    else
        centeredText(shown, bx + bw * 0.5f, fieldY + fieldH * 0.5f, 1.0f, 1, 1, 1, 1);

    centeredText("创建", bx + bw * 0.5f, createY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("返回", bx + bw * 0.5f, backY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    if (!ui.menuMessage.empty())
        centeredText(ui.menuMessage, scrW * 0.5f, (float)scrH - 32.0f, 0.85f, 0.95f, 0.55f, 0.45f, 1);
}

void Renderer::drawSettings(UIState& ui) {
    const float bw = 340.0f, bh = 56.0f;
    const float bx = (scrW - bw) * 0.5f;
    const float titleY = scrH * 0.5f - 170.0f;
    const float sliderW = 380.0f, sliderH = 14.0f;
    const float sliderX = (scrW - sliderW) * 0.5f;
    const float sliderY = scrH * 0.5f - 8.0f;
    const float invYBtn = scrH * 0.5f + 80.0f;
    const float backY = scrH * 0.5f + 150.0f;
    const float ts = 1.3f;

    ui.settingsHover = -1;
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.6f);

    // Slider.
    quad(sliderX - 3, sliderY - 3, sliderW + 6, sliderH + 6, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
    quad(sliderX, sliderY, sliderW, sliderH, 0, 0, 0, 0, 0.22f, 0.22f, 0.22f, 1.0f);
    float t = (ui.mouseSens - cfg::SENS_MIN) / (cfg::SENS_MAX - cfg::SENS_MIN);
    t = clampf(t, 0.0f, 1.0f);
    quad(sliderX, sliderY, sliderW * t, sliderH, 0, 0, 0, 0, 0.55f, 0.75f, 0.35f, 1.0f);
    float hx = sliderX + sliderW * t;
    quad(hx - 8, sliderY - 8, 16, sliderH + 16, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
    quad(hx - 6, sliderY - 6, 12, sliderH + 12, 0, 0, 0, 0, 0.92f, 0.92f, 0.92f, 1.0f);

    bool invHover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= invYBtn && ui.mouseY < invYBtn + bh);
    if (invHover) ui.settingsHover = 1;
    buttonChrome(bx, invYBtn, bw, bh, invHover);

    bool backHover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= backY && ui.mouseY < backY + bh);
    if (backHover) ui.settingsHover = 0;
    buttonChrome(bx, backY, bw, bh, backHover);

    ui.sliderX = sliderX; ui.sliderY = sliderY; ui.sliderW = sliderW; ui.sliderH = sliderH;
    flushUI(progUI, whiteTex);

    int tw, th;
    stringSize("设置", tw, th);
    drawBlockIcon(STONE, scrW * 0.5f - (float)tw * ts * 0.5f - 44.0f, titleY - 16.0f, 32.0f);
    flushUI(progUI, atlasTex);

    centeredText("设置", scrW * 0.5f, titleY, ts, 1, 1, 1, 1);
    centeredText("鼠标灵敏度", scrW * 0.5f, scrH * 0.5f - 62.0f, 1.0f, 1, 1, 1, 1);
    char val[32];
    snprintf(val, sizeof(val), "%.4f", ui.mouseSens);
    centeredText(val, scrW * 0.5f, scrH * 0.5f + 30.0f, 0.9f, 0.7f, 0.9f, 0.5f, 1.0f);
    std::string invLabel = std::string("反转垂直视角：") + (ui.invertY ? "开" : "关");
    centeredText(invLabel, bx + bw * 0.5f, invYBtn + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("返回", bx + bw * 0.5f, backY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
}

void Renderer::drawDebugMenu(UIState& ui) {
    const float bw = 340.0f, bh = 42.0f;
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.6f);

    if (ui.trialPick) {
        ui.trialHover = -1;
        ui.railHover = -1;
        const float rowH = 34.0f;
        const float listTop = 108.0f;
        const float listBot = (float)scrH - 96.0f;
        int visible = (int)((listBot - listTop) / rowH);
        if (visible < 1) visible = 1;
        int maxScroll = ritual::RelicCount - visible;
        if (maxScroll < 0) maxScroll = 0;
        if (ui.trialScroll < 0) ui.trialScroll = 0;
        if (ui.trialScroll > maxScroll) ui.trialScroll = maxScroll;
        const float bx = (scrW - bw) * 0.5f;
        for (int i = 0; i < visible; i++) {
            int idx = ui.trialScroll + i;
            if (idx < 0 || idx >= ritual::RelicCount) break;
            float y = listTop + i * rowH;
            bool hover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= y && ui.mouseY < y + rowH - 4.0f;
            if (hover) ui.trialHover = idx;
            buttonChrome(bx, y, bw, rowH - 4.0f, hover);
        }
        float backY = (float)scrH - 78.0f;
        bool backHover = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= backY && ui.mouseY < backY + bh;
        if (backHover) ui.trialHover = -2;
        buttonChrome(bx, backY, bw, bh, backHover);
        flushUI(progUI, whiteTex);
        centeredText("选择守护者", scrW * 0.5f, 56.0f, 1.3f, 1, 1, 1, 1);
        for (int i = 0; i < visible; i++) {
            int idx = ui.trialScroll + i;
            if (idx < 0 || idx >= ritual::RelicCount) break;
            float y = listTop + i * rowH;
            centeredText(ritual::relicName(idx), bx + bw * 0.5f, y + (rowH - 4.0f) * 0.5f, 1.0f, 1, 1, 1, 1);
        }
        centeredText("返回", bx + bw * 0.5f, backY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
        return;
    }

    const float bx = (scrW - bw) * 0.5f;
    const float titleY = scrH * 0.5f - 236.0f;
    const float sliderW = 380.0f, sliderH = 14.0f;
    const float sliderX = (scrW - sliderW) * 0.5f;
    const float ts = 1.3f;
    const float gap = 6.0f;

    auto slider = [&](float y, float frac, float* rx, float* ry, float* rw, float* rh) {
        float f = clampf(frac, 0.0f, 1.0f);
        quad(sliderX - 3, y - 3, sliderW + 6, sliderH + 6, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
        quad(sliderX, y, sliderW, sliderH, 0, 0, 0, 0, 0.22f, 0.22f, 0.22f, 1.0f);
        quad(sliderX, y, sliderW * f, sliderH, 0, 0, 0, 0, 0.55f, 0.75f, 0.35f, 1.0f);
        float hx = sliderX + sliderW * f;
        quad(hx - 8, y - 8, 16, sliderH + 16, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
        quad(hx - 6, y - 6, 12, sliderH + 12, 0, 0, 0, 0, 0.92f, 0.92f, 0.92f, 1.0f);
        if (rx) { *rx = sliderX; *ry = y; *rw = sliderW; *rh = sliderH; }
    };

    float tickY = scrH * 0.5f - 132.0f;
    slider(tickY, ui.tickSpeed / 20.0f, &ui.tickSliderX, &ui.tickSliderY, &ui.tickSliderW, &ui.tickSliderH);

    float timeY = scrH * 0.5f - 48.0f;
    slider(timeY, ui.timeOfDay / (float)cfg::TICKS_PER_DAY, &ui.timeSliderX, &ui.timeSliderY, &ui.timeSliderW, &ui.timeSliderH);

    const float permY = scrH * 0.5f + 12.0f;
    const float humY = permY + bh + gap;
    const float matY = humY + bh + gap;
    const float modY = matY + bh + gap;
    const float dumY = modY + bh + gap;
    const float backY = dumY + bh + gap;

    auto hitBtn = [&](float y) {
        return ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= y && ui.mouseY < y + bh;
    };

    ui.debugHover = -1;
    ui.railHover = -1;
    bool permHover = hitBtn(permY);
    if (permHover) ui.debugHover = 5;
    buttonChrome(bx, permY, bw, bh, permHover);

    bool humHover = hitBtn(humY);
    if (humHover) ui.debugHover = 1;
    buttonChrome(bx, humY, bw, bh, humHover);

    bool matHover = hitBtn(matY);
    if (matHover) ui.debugHover = 2;
    buttonChrome(bx, matY, bw, bh, matHover);

    bool modHover = hitBtn(modY);
    if (modHover) ui.debugHover = 3;
    buttonChrome(bx, modY, bw, bh, modHover);

    bool dumHover = hitBtn(dumY);
    if (dumHover) ui.debugHover = 4;
    buttonChrome(bx, dumY, bw, bh, dumHover);

    bool backHover = hitBtn(backY);
    if (backHover) ui.debugHover = 0;
    buttonChrome(bx, backY, bw, bh, backHover);

    const bool showRail = (ui.privilegeMode || ui.inTrial) && !ui.roomSession;
    const bool flightOnly = ui.structureEdit;
    const float aw = 46.0f;
    const float ax = bx + bw + 12.0f;
    const float ay = permY;
    const float colW = 280.0f;
    const float colX = ax + aw + 12.0f;
    const float spaceY = ay;
    const float flyBtnY = flightOnly ? spaceY : (spaceY + bh + gap);
    const float breakBtnY = flyBtnY + bh + gap;
    const bool showQuick = ui.privilegeMode && !flightOnly;
    const float railBottom = showQuick ? (breakBtnY + bh) : (flyBtnY + bh);
    const float ah = ui.railOpen ? (railBottom - ay) : bh;
    if (showRail) {
        bool arrowHover = ui.mouseX >= ax && ui.mouseX < ax + aw && ui.mouseY >= ay && ui.mouseY < ay + ah;
        if (arrowHover) ui.railHover = 0;
        buttonChrome(ax, ay, aw, ah, arrowHover);
        if (ui.railOpen) {
            bool flyHover = ui.mouseX >= colX && ui.mouseX < colX + colW && ui.mouseY >= flyBtnY && ui.mouseY < flyBtnY + bh;
            if (flyHover) ui.railHover = 2;
            buttonChrome(colX, flyBtnY, colW, bh, flyHover);
            if (!flightOnly) {
                bool spaceHover = ui.mouseX >= colX && ui.mouseX < colX + colW && ui.mouseY >= spaceY && ui.mouseY < spaceY + bh;
                if (spaceHover) ui.railHover = 1;
                buttonChrome(colX, spaceY, colW, bh, spaceHover);
            }
            if (showQuick) {
                bool breakHover = ui.mouseX >= colX && ui.mouseX < colX + colW && ui.mouseY >= breakBtnY && ui.mouseY < breakBtnY + bh;
                if (breakHover) ui.railHover = 3;
                buttonChrome(colX, breakBtnY, colW, bh, breakHover);
            }
        }
    }
    flushUI(progUI, whiteTex);

    centeredText("调试菜单", scrW * 0.5f, titleY, ts, 1, 1, 1, 1);
    centeredText("游戏刻速度", scrW * 0.5f, tickY - 28.0f, 1.0f, 1, 1, 1, 1);
    centeredText("时间段（0:00 = 午夜）", scrW * 0.5f, timeY - 28.0f, 1.0f, 1, 1, 1, 1);
    char val[32];
    snprintf(val, sizeof(val), "%.1fx", ui.tickSpeed);
    drawString(val, sliderX + sliderW + 18.0f, tickY - 4.0f, 0.9f, 0.78f, 0.78f, 0.78f, 1.0f);
    int ticks = ((int)ui.timeOfDay % cfg::TICKS_PER_DAY + cfg::TICKS_PER_DAY) % cfg::TICKS_PER_DAY;
    int hours = ticks / 1000;
    int mins = (ticks % 1000) * 60 / 1000;
    snprintf(val, sizeof(val), "%02d:%02d", hours, mins);
    drawString(val, sliderX + sliderW + 18.0f, timeY - 4.0f, 0.9f, 0.78f, 0.78f, 0.78f, 1.0f);
    centeredText(ui.privilegeMode ? "权限模式：开" : "权限模式：关", bx + bw * 0.5f, permY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    if (showRail) {
        centeredText(ui.railOpen ? "▶" : "◀", ax + aw * 0.5f, ay + ah * 0.5f, 1.15f, 1, 1, 1, 1);
        if (ui.railOpen) {
            if (!flightOnly) {
                const char* spaceLabel = ui.inTrial ? "退出守护者空间" : "进入守护者空间";
                centeredText(spaceLabel, colX + colW * 0.5f, spaceY + bh * 0.5f, 0.95f, 1, 1, 1, 1);
            }
            centeredText(ui.flying ? "关闭飞行" : "开启飞行", colX + colW * 0.5f, flyBtnY + bh * 0.5f, 0.95f, 1, 1, 1, 1);
            if (showQuick)
                centeredText(ui.quickBreak ? "快速破坏：开" : "快速破坏：关", colX + colW * 0.5f, breakBtnY + bh * 0.5f, 0.95f, 1, 1, 1, 1);
        }
    }
    std::string humLabel = std::string("湿度显示：") + (ui.humidityMode ? "开" : "关");
    centeredText(humLabel, bx + bw * 0.5f, humY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("材质编辑器", bx + bw * 0.5f, matY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("模型编辑器", bx + bw * 0.5f, modY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText(ui.dummyActive ? "观察模型：开" : "观察模型：关", bx + bw * 0.5f, dumY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
    centeredText("返回", bx + bw * 0.5f, backY + bh * 0.5f, 1.0f, 1, 1, 1, 1);
}

void Renderer::drawMaterialEditor(UIState& ui) {
    mat::Image& img = mat::g_tileImages[ui.matEditorTile];
    const int T = tex::TILE;
    const float ts = std::min((float)scrW, (float)scrH) * 0.60f;
    const float x0 = ((float)scrW - ts) * 0.5f;
    const float y0 = 64.0f;
    const float px = ts / (float)T;
    ui.matEditorTileX = x0;
    ui.matEditorTileY = y0;
    ui.matEditorTileSize = ts;

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.05f, 0.05f, 0.08f, 0.96f);

    // Draw pixels (with a checkerboard behind transparent ones).
    for (int py = 0; py < T; py++) {
        for (int pxx = 0; pxx < T; pxx++) {
            size_t i = ((size_t)py * T + pxx) * 4;
            float r = img.rgba[i + 0] / 255.0f, g = img.rgba[i + 1] / 255.0f;
            float b = img.rgba[i + 2] / 255.0f, a = img.rgba[i + 3] / 255.0f;
            float cx = x0 + pxx * px, cy = y0 + py * px;
            bool check = (((pxx / 4) + (py / 4)) & 1) != 0;
            if (a < 0.99f) quad(cx, cy, px, px, 0, 0, 0, 0, check ? 0.4f : 0.6f, check ? 0.4f : 0.6f, check ? 0.4f : 0.6f, 1.0f);
            quad(cx, cy, px, px, 0, 0, 0, 0, r, g, b, a);
        }
    }

    // Hover cursor.
    if (ui.matEditorHoverX >= 0 && ui.matEditorHoverX < T && ui.matEditorHoverY >= 0 && ui.matEditorHoverY < T) {
        float hx = x0 + ui.matEditorHoverX * px, hy = y0 + ui.matEditorHoverY * px;
        quad(hx, hy, px, 2, 0, 0, 0, 0, 1, 1, 0, 1);
        quad(hx, hy, 2, px, 0, 0, 0, 0, 1, 1, 0, 1);
        quad(hx + px - 2, hy, 2, px, 0, 0, 0, 0, 1, 1, 0, 1);
        quad(hx, hy + px - 2, px, 2, 0, 0, 0, 0, 1, 1, 0, 1);
    }
    flushUI(progUI, whiteTex);

    // Color palette.
    static const float kPal[8][3] = {
        { 0.0f, 1.0f, 0.0f }, { 0.55f, 0.30f, 0.10f }, { 0.5f, 0.5f, 0.5f }, { 1.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.3f, 1.0f }, { 1.0f, 0.85f, 0.0f },
    };
    const float pc = 34.0f, py0 = y0 + ts + 26.0f;
    ui.matEditorPaletteHover = -1;
    for (int i = 0; i < 8; i++) {
        float cx = x0 + i * (pc + 8.0f);
        bool hov = ui.mouseX >= cx && ui.mouseX < cx + pc && ui.mouseY >= py0 && ui.mouseY < py0 + pc;
        if (hov) ui.matEditorPaletteHover = i;
        quad(cx, py0, pc, pc, 0, 0, 0, 0, kPal[i][0], kPal[i][1], kPal[i][2], 1.0f);
        if (hov) quad(cx - 3, py0 - 3, pc + 6, pc + 6, 0, 0, 0, 0, 1, 1, 1, 1);
    }
    {
        float cx = x0 + 8 * (pc + 8.0f);
        bool hov = ui.mouseX >= cx && ui.mouseX < cx + pc && ui.mouseY >= py0 && ui.mouseY < py0 + pc;
        if (hov) ui.matEditorPaletteHover = 8;
        quad(cx, py0, pc, pc, 0, 0, 0, 0, 0.3f, 0.3f, 0.3f, 1.0f);
        quad(cx + 6, py0 + pc * 0.5f - 1, pc - 12, 2, 0, 0, 0, 0, 1, 0, 0, 1);
        if (hov) quad(cx - 3, py0 - 3, pc + 6, pc + 6, 0, 0, 0, 0, 1, 1, 1, 1);
    }
    flushUI(progUI, whiteTex);

    centeredText("材质编辑器", scrW * 0.5f, 22.0f, 1.5f, 1, 1, 1, 1);
    char buf[160];
    snprintf(buf, sizeof(buf), "贴图: %s   [ ] 切换  F5 保存  ESC 返回  左键绘制/右键取色", mat::tileName(ui.matEditorTile));
    centeredText(buf, scrW * 0.5f, y0 + ts + 88.0f, 0.95f, 0.85f, 0.85f, 0.85f, 1);
    if (ui.matEditorDirty) centeredText("未保存", scrW * 0.5f, y0 + ts + 116.0f, 1.0f, 1, 0.6f, 0.2f, 1);
}

void Renderer::saveMaterialTile(int tile) {
    if (tile < 0 || tile >= TEX_COUNT) return;
    mat::Image& img = mat::g_tileImages[tile];
    if (!img.ok()) return;
    std::string path = pack::tilePng(mat::tileName(tile));
    mat::savePNG(path.c_str(), img.w, img.h, img.rgba.data());

    // Rebuild + re-upload the atlas so the world reflects the edit immediately.
    std::vector<uint8_t> atlas;
    tex::generateAtlas(atlas);
    for (int t = 0; t < TEX_COUNT; t++) {
        if (mat::g_tileImages[t].ok()) tex::overwriteTile(t, mat::g_tileImages[t].rgba.data(), atlas);
    }
    gl::BindTexture(GL_TEXTURE_2D, atlasTex);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex::ATLAS_W, tex::ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());
    gl::GenerateMipmap(GL_TEXTURE_2D);
    gl::BindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::paletteLayout(int& x0, int& y0, int& cell, int& cols, int& rows) const {
    cell = 46;
    cols = 10;
    int count = (int)plugin::creativePalette().size();
    rows = (count + cols - 1) / cols;
    int w = cols * cell + 20;
    int h = rows * cell + 20;
    x0 = (scrW - w) / 2 + 10;
    y0 = (scrH - h) / 2 + 10;
}

void Renderer::drawDeploy(UIState& ui) {
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.04f, 0.05f, 0.07f, 1.0f);
    flushUI(progUI, whiteTex);

    float side = std::min((float)scrW, (float)scrH) * 0.64f;
    float x = ((float)scrW - side) * 0.5f;
    float y = ((float)scrH - side) * 0.5f + 18.0f;
    ui.deployMapX = x;
    ui.deployMapY = y;
    ui.deployMapS = side;

    if (ui.deployPixels && ui.deploySpan > 0 && ui.deployStamp != deployTexStamp) {
        if (deployTex) gl::DeleteTextures(1, &deployTex);
        deployTex = makeTexture(ui.deployPixels->data(), ui.deploySpan, ui.deploySpan, false, false);
        deployTexStamp = ui.deployStamp;
    }
    if (deployTex) {
        quad(x, y, side, side, 0, 0, 1, 1, 1, 1, 1, 1);
        flushUI(progUI, deployTex);
    }
    const float bw = 2.0f;
    quad(x - bw, y - bw, side + bw * 2.0f, bw, 0, 0, 0, 0, 1, 1, 1, 1);
    quad(x - bw, y + side, side + bw * 2.0f, bw, 0, 0, 0, 0, 1, 1, 1, 1);
    quad(x - bw, y, bw, side, 0, 0, 0, 0, 1, 1, 1, 1);
    quad(x + side, y, bw, side, 0, 0, 0, 0, 1, 1, 1, 1);
    flushUI(progUI, whiteTex);

    float cell = (ui.deploySpan > 0) ? side / (float)ui.deploySpan : 1.0f;
    for (const DeployPinView& pin : ui.deployPins) {
        float px = x + ((float)(pin.bx - ui.deployOx) + 0.5f) * cell;
        float py = y + ((float)(pin.bz - ui.deployOz) + 0.5f) * cell;
        if (pin.phase == 2) {
            float a = pin.t;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            float d = 5.0f;
            quad(px - 2.0f, py - d, 4.0f, d * 2.0f, 0, 0, 0, 0, ui.deployR, ui.deployG, ui.deployB, a);
            quad(px - d, py - 2.0f, d * 2.0f, 4.0f, 0, 0, 0, 0, ui.deployR, ui.deployG, ui.deployB, a);
        } else {
            float s = 8.0f;
            for (int i = 0; i < 7; i++) {
                float t = (float)i / 6.0f;
                float dx = -s + t * s * 2.0f;
                float yA = -s + t * s * 2.0f;
                float yB = s - t * s * 2.0f;
                quad(px + dx - 2.0f, py + yA - 2.0f, 4.0f, 4.0f, 0, 0, 0, 0, 0, 0, 0, 1);
                quad(px + dx - 2.0f, py + yB - 2.0f, 4.0f, 4.0f, 0, 0, 0, 0, 0, 0, 0, 1);
                quad(px + dx - 1.5f, py + yA - 1.5f, 3.0f, 3.0f, 0, 0, 0, 0, ui.deployR, ui.deployG, ui.deployB, 1);
                quad(px + dx - 1.5f, py + yB - 1.5f, 3.0f, 3.0f, 0, 0, 0, 0, ui.deployR, ui.deployG, ui.deployB, 1);
            }
        }
    }
    flushUI(progUI, whiteTex);
    for (const DeployPinView& pin : ui.deployPins) {
        if (pin.phase == 2) continue;
        float px = x + ((float)(pin.bx - ui.deployOx) + 0.5f) * cell;
        float py = y + ((float)(pin.bz - ui.deployOz) + 0.5f) * cell;
        centeredText(pin.name, px, py + 14.0f, 1.0f, ui.deployR, ui.deployG, ui.deployB, 1);
    }
    if (ui.deploySeconds >= 0)
        centeredText(std::to_string(ui.deploySeconds), (float)scrW * 0.5f, y - 78.0f, 2.4f, 0.96f, 0.96f, 0.96f, 1);
    centeredText("选择部署位置", (float)scrW * 0.5f, y - 36.0f, 1.4f, 0.92f, 0.93f, 0.95f, 1);
    centeredText("左键选点并倒计时，结束前可改点，右键取消", (float)scrW * 0.5f, y + side + 22.0f, 1.15f,
                 0.82f, 0.84f, 0.88f, 1);
}

void Renderer::drawUI(const World& w, const Player& p, float timeOfDay, UIState& ui) {
    (void)w;
    (void)p;
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_CULL_FACE);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ui.menuHover = -1;
    ui.settingsHover = -1;
    ui.debugHover = -1;
    ui.noteHover = false;
    ui.noteBackHover = false;
    if (ui.appScreen != AppScreen::Playing) {
        if (ui.appScreen == AppScreen::Start) drawStartMenu(ui);
        else if (ui.appScreen == AppScreen::PlayerProfile) drawPlayerProfile(ui);
        else if (ui.appScreen == AppScreen::Worlds) drawWorldsMenu(ui);
        else if (ui.appScreen == AppScreen::WorldDetail) drawWorldDetail(ui);
        else if (ui.appScreen == AppScreen::CreateWorld) drawCreateWorld(ui);
        else if (ui.appScreen == AppScreen::RoomLobby) drawRoomLobby(ui);
        else if (ui.appScreen == AppScreen::JoinRoom) drawJoinRoom(ui);
        else if (ui.appScreen == AppScreen::RoomLoading) drawRoomLoading(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    auto drawCoordinates = [&]() {
        const float invBlock = 1.0f / cfg::BLOCK_SCALE;
        int bx = (int)std::floor(ui.playerPos.x * invBlock);
        int by = (int)std::floor(ui.playerPos.y * invBlock);
        int bz = (int)std::floor(ui.playerPos.z * invBlock);
        auto floorChunk = [](int block, int span) {
            int q = block / span;
            if (block < 0 && block % span != 0) --q;
            return q;
        };
        int cx = floorChunk(bx, cfg::CHUNK_X);
        int cz = floorChunk(bz, cfg::CHUNK_Z);
        quad(8, 8, 430, 28, 0, 0, 0, 0, 0.02f, 0.025f, 0.03f, 0.68f);
        flushUI(progUI, whiteTex);
        text(14, 12, 0.78f, 0.96f, 0.96f, 0.92f, 1.0f,
             "坐标 X %d  Y %d  Z %d   |   区块 %d,%d", bx, by, bz, cx, cz);
    };
    if (ui.storyOpen) {
        quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 1.0f);
        flushUI(progUI, whiteTex);
        if (!ui.storySentence.empty())
            centeredText(ui.storySentence, (float)scrW * 0.5f, (float)scrH * 0.5f, 1.35f,
                         0.93f, 0.93f, 0.91f, ui.storyFade);
        if (ui.storyHold)
            centeredText("单击或按任意键", (float)scrW * 0.5f, (float)scrH - 56.0f, 0.85f, 0.62f, 0.62f, 0.62f, 0.9f);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.deploying) {
        drawDeploy(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.guideOpen) {
        drawGuide(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.clueQuizOpen) {
        drawClueQuiz(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.clueOpen) {
        drawClue(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.matEditorOpen) {
        drawMaterialEditor(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.menuOpen) {
        if (ui.settingsOpen) drawSettings(ui);
        else if (ui.debugMenuOpen) drawDebugMenu(ui);
        else drawMenu(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.inventoryOpen) {
        drawInventory(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.targetPanel >= 0) {
        drawTargetPanel(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.playerDead) {
        drawDeath(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.spectating) {
        const float cx = scrW * 0.5f;
        const float cy = scrH * 0.5f;
        quad(cx - 9, cy - 1, 18, 2, 0, 0, 0, 0, 1, 1, 1, 0.9f);
        quad(cx - 1, cy - 9, 2, 18, 0, 0, 0, 0, 1, 1, 1, 0.9f);
        flushUI(progUI, whiteTex);
        centeredText("观战", cx, 28.0f, 1.0f, 0.9f, 0.9f, 0.95f, 1);
        drawCoordinates();
        gl::Enable(GL_DEPTH_TEST);
        return;
    }
    if (ui.structureEdit && ui.structurePicker) {
        drawStructurePicker(ui);
        gl::Enable(GL_DEPTH_TEST);
        return;
    }

    const float cx = scrW * 0.5f;
    const float cy = scrH * 0.5f;
    if (ui.bossNear && ui.bossMaxHp > 0 && !ui.bossName.empty()) {
        const float barW = 420.0f;
        const float barH = 16.0f;
        const float barX = cx - barW * 0.5f;
        const float barY = 52.0f;
        float frac = clampf((float)ui.bossHp / (float)ui.bossMaxHp, 0.0f, 1.0f);
        centeredText("守护者", cx, 16.0f, 0.72f, 0.86f, 0.70f, 0.36f, 1.0f);
        centeredText(ui.bossName, cx, 36.0f, 1.05f, 0.96f, 0.93f, 0.84f, 1.0f);
        quad(barX - 3.0f, barY - 3.0f, barW + 6.0f, barH + 6.0f, 0, 0, 0, 0, 0.04f, 0.03f, 0.03f, 0.88f);
        quad(barX, barY, barW, barH, 0, 0, 0, 0, 0.22f, 0.08f, 0.08f, 0.95f);
        if (frac > 0.0f)
            quad(barX, barY, barW * frac, barH, 0, 0, 0, 0, 0.78f, 0.16f, 0.13f, 1.0f);
        flushUI(progUI, whiteTex);
        char hpBuf[48];
        snprintf(hpBuf, sizeof(hpBuf), "%d / %d", ui.bossHp, ui.bossMaxHp);
        centeredText(hpBuf, cx, barY + barH * 0.5f, 0.62f, 1.0f, 0.96f, 0.92f, 1.0f);
    }
    const int n = cfg::HOTBAR_SLOTS;
    const float slot = 46.0f, gap = 4.0f;
    const float carryS = 56.0f;
    const float hbY = (float)scrH - slot - 8.0f;
    float leftX = 0, rightX = 0;
    splitHotbarHudX((float)scrW, slot, gap, 16.0f, leftX, rightX);
    const float carryX = 16.0f;
    const float carryY = hbY + slot - carryS;
    leftX = carryX + carryS + 12.0f;
    const bool carrying = ui.carrySlot && !ui.carrySlot->empty();
    const bool leftSealed = ui.vitals && ui.vitals->limb[vitals::HandL].health <= vitals::kDeadEps;
    const bool rightSealed = ui.vitals && ui.vitals->limb[vitals::HandR].health <= vitals::kDeadEps;
    const bool fPrompt = !ui.structureEdit &&
        (carrying || ui.targetDrop >= 0 || ui.targetAim >= 0 || ui.processLogReady);

    CrosshairPrompt promptItems[4];
    int promptCount = 0;
    if (fPrompt) {
        const char* action = "拾取";
        if (ui.targetAim >= 0) action = "状态";
        else if (carrying) action = "放下";
        else if (ui.targetDrop >= 0)
            action = ui.targetDrop < (int)w.drops().size() &&
                     w.drops()[(size_t)ui.targetDrop].item == ITEM_CLUE ? "答题" : "拾取";
        else if (ui.processLogReady) action = "加工";
        promptItems[promptCount++] = { CrosshairKeyKind::Text, "F", action };
    }
    if (ui.hasPlacePreview || ui.targetPlaceReady)
        promptItems[promptCount++] = { CrosshairKeyKind::MouseRight, "", "放置" };
    std::vector<CrosshairPromptBox> promptBoxes;
    layoutCrosshairPrompts(promptItems, promptCount, cx + 36.0f, cy, promptBoxes);

    // ---- solid-color pass (white texture) ----
    if (ui.borderFog > 0.001f)
        quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.78f, 0.79f, 0.80f, ui.borderFog);
    if (ui.damageFlash > 0.0f)
        quad(0, 0, (float)scrW, (float)scrH, 0,0,0,0, .75f,.03f,.01f,
             .18f * clampf(ui.damageFlash / .20f, 0.0f, 1.0f));
    quad(cx - 9, cy - 1, 18, 2, 0, 0, 0, 0, 1, 1, 1, 0.9f);
    quad(cx - 1, cy - 9, 2, 18, 0, 0, 0, 0, 1, 1, 1, 0.9f);
    if (ui.hitMarker > 0.0f) {
        float a = clampf(ui.hitMarker / .22f, 0.0f, 1.0f);
        const float o = 13.0f, n = 7.0f, t = 2.0f;
        quad(cx-o-n, cy-o, n, t, 0,0,0,0, 1,.25f,.18f,a);
        quad(cx+o, cy-o, n, t, 0,0,0,0, 1,.25f,.18f,a);
        quad(cx-o-n, cy+o, n, t, 0,0,0,0, 1,.25f,.18f,a);
        quad(cx+o, cy+o, n, t, 0,0,0,0, 1,.25f,.18f,a);
    }
    drawCrosshairPromptChrome(promptBoxes);

    if (!ui.structureEdit) {
        float bg = carrying ? 0.50f : 0.22f;
        quad(carryX, carryY, carryS, carryS, 0, 0, 0, 0, 0.10f, 0.08f, 0.05f, bg + 0.15f);
        const float bw = 3.0f;
        float br = carrying ? 0.92f : 0.62f;
        float bgc = carrying ? 0.72f : 0.50f;
        float bb = carrying ? 0.38f : 0.32f;
        quad(carryX - bw, carryY - bw, carryS + 2 * bw, bw, 0, 0, 0, 0, br, bgc, bb, 0.95f);
        quad(carryX - bw, carryY + carryS, carryS + 2 * bw, bw, 0, 0, 0, 0, br, bgc, bb, 0.95f);
        quad(carryX - bw, carryY, bw, carryS, 0, 0, 0, 0, br, bgc, bb, 0.95f);
        quad(carryX + carryS, carryY, bw, carryS, 0, 0, 0, 0, br, bgc, bb, 0.95f);

        for (int i = 0; i < n; i++) {
            float sx = splitHotbarSlotX(i, leftX, rightX, slot, gap);
            bool sealed = i < cfg::HAND_SLOTS ? leftSealed : rightSealed;
            float slotBg = sealed ? 0.08f :
                (splitHotbarSelected(i, ui) ? (carrying ? 0.40f : 0.55f) : (carrying ? 0.18f : 0.28f));
            quad(sx, hbY, slot, slot, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, slotBg);
            if (sealed) {
                quad(sx + 5, hbY + slot*.5f - 1, slot - 10, 2, 0,0,0,0, .65f,.08f,.06f,.9f);
                quad(sx + slot*.5f - 1, hbY + 5, 2, slot - 10, 0,0,0,0, .65f,.08f,.06f,.9f);
            }
        }
        for (int i = 0; i < n; i++) {
            if (!splitHotbarSelected(i, ui)) continue;
            float sx = splitHotbarSlotX(i, leftX, rightX, slot, gap);
            const float border = 3.0f;
            float a = carrying ? 0.45f : 0.95f;
            quad(sx - border, hbY - border, slot + 2 * border, border, 0, 0, 0, 0, 1, 1, 1, a);
            quad(sx - border, hbY + slot, slot + 2 * border, border, 0, 0, 0, 0, 1, 1, 1, a);
            quad(sx - border, hbY, border, slot, 0, 0, 0, 0, 1, 1, 1, a);
            quad(sx + slot, hbY, border, slot, 0, 0, 0, 0, 1, 1, 1, a);
        }
        flushUI(progUI, whiteTex);

        if (carrying) {
            uint8_t b = ui.carrySlot->block;
            if (b != AIR && b < liveBlockCount())
                drawBlockIcon(b, carryX + 4, carryY + 4, carryS - 8);
        }
        for (int i = 0; i < n; i++) {
            uint8_t b = ui.inventory ? ui.inventory[i].block : (uint8_t)AIR;
            if (b == AIR || b >= liveBlockCount()) continue;
            float sx = splitHotbarSlotX(i, leftX, rightX, slot, gap);
            drawBlockIcon(b, sx + 3, hbY + 3, slot - 6);
        }
        flushUI(progUI, atlasTex);

        float barW = cfg::HAND_SLOTS * slot + (cfg::HAND_SLOTS - 1) * gap;
        centeredText("搬运", carryX + carryS * 0.5f, carryY - 14.0f, 0.55f, 0.90f, 0.78f, 0.50f, 0.95f);
        centeredText("左手  1-3", leftX + barW * 0.5f, hbY - 14.0f, 0.55f, 0.85f, 0.85f, 0.85f, carrying ? 0.55f : 0.9f);
        centeredText("右手  4-6", rightX + barW * 0.5f, hbY - 14.0f, 0.55f, 0.85f, 0.85f, 0.85f, carrying ? 0.55f : 0.9f);
    }
    drawCrosshairPromptText(promptBoxes);
    if (ui.inventory && !ui.structureEdit) {
        for (int i = 0; i < n; i++) {
            const ItemSlot& s = ui.inventory[i];
            if (s.empty() || s.count <= 1) continue;
            float sx = splitHotbarSlotX(i, leftX, rightX, slot, gap);
            text(sx + slot - 18, hbY + slot - 22, 0.6f, 1, 1, 1, 1, "%d", s.count);
        }
    }

    if (!ui.showDebug) {
        drawCoordinates();
    } else {
        int ticks = ((int)timeOfDay % cfg::TICKS_PER_DAY + cfg::TICKS_PER_DAY) % cfg::TICKS_PER_DAY;
        int hours = ticks / 1000;
        int mins = (ticks % 1000) * 60 / 1000;
        text(10, 10, 0.9f, 1, 1, 1, 1, "VOXEL LEGEND  |  OpenGL 3.3  |  seed %u", ui.seed);
        text(10, 32, 0.9f, 1, 1, 1, 1, "FPS: %.0f  |  time %02d:%02d", ui.fps, hours, mins);
        text(10, 54, 0.9f, 1, 1, 1, 1, "pos: %.2f %.2f %.2f", ui.playerPos.x, ui.playerPos.y, ui.playerPos.z);
        text(10, 76, 0.9f, 1, 1, 1, 1, "vel: %.2f %.2f %.2f", ui.playerVel.x, ui.playerVel.y, ui.playerVel.z);
        text(10, 98, 0.9f, 1, 1, 1, 1, "yaw %.1f  pitch %.1f  fly %d  ground %d",
             ui.yaw * 57.2958f, ui.pitch * 57.2958f, (int)ui.flying, (int)ui.onGround);
        text(10, 120, 0.9f, 1, 1, 1, 1, "loaded chunks: %d", ui.loadedChunks);
        text(10, 142, 0.9f, 1, 1, 1, 1, "tick speed: %.1fx", ui.tickSpeed);
    }

    if (ui.humidityMode && ui.hasHumidityBlock) {
        char hb[32];
        snprintf(hb, sizeof(hb), "%d", ui.humidityValue);
        centeredText(hb, ui.humidityScreenX, ui.humidityScreenY - 24.0f, 0.9f, 1, 1, 1, 1);
    }

    if (!ui.goalText.empty())
        centeredText(ui.goalText.c_str(), scrW * 0.5f, 28.0f, 1.0f, 0.95f, 0.92f, 0.78f, 1.0f);

    ui.structureOpHover = -1;
    if (ui.structureEdit && ui.blockBarOpen) {
        std::vector<uint8_t> blocks;
        structure::collectBuildBlocks(blocks);
        const float cell = 44.0f, gap = 4.0f;
        const int cols = 2;
        const float x0 = 10.0f, y0 = 48.0f;
        int rowsFit = (int)(((float)scrH - y0 - 12.0f) / (cell + gap));
        if (rowsFit < 1) rowsFit = 1;
        int rows = ((int)blocks.size() + cols - 1) / cols;
        int maxScroll = rows - rowsFit;
        if (maxScroll < 0) maxScroll = 0;
        if (ui.blockBarScroll > maxScroll) ui.blockBarScroll = maxScroll;
        if (ui.blockBarScroll < 0) ui.blockBarScroll = 0;
        float panelH = (float)rowsFit * (cell + gap) + 8.0f;
        quad(4, y0 - 8, cols * (cell + gap) + 12, panelH, 0, 0, 0, 0, 0.08f, 0.08f, 0.10f, 0.92f);
        ui.blockBarHover = -1;
        flushUI(progUI, whiteTex);
        for (int i = 0; i < (int)blocks.size(); i++) {
            int row = i / cols;
            int col = i % cols;
            float y = y0 + (float)(row - ui.blockBarScroll) * (cell + gap);
            float x = x0 + (float)col * (cell + gap);
            if (y + cell < y0 || y > y0 + panelH - 8.0f) continue;
            bool hov = ui.mouseX >= x && ui.mouseX < x + cell && ui.mouseY >= y && ui.mouseY < y + cell;
            if (hov) ui.blockBarHover = i;
            bool on = blocks[(size_t)i] == ui.structureBlock;
            if (on || hov) {
                quad(x - 2, y - 2, cell + 4, cell + 4, 0, 0, 0, 0, on ? 0.95f : 0.55f, on ? 0.82f : 0.55f, 0.28f, 1);
                flushUI(progUI, whiteTex);
            }
            drawBlockIcon(blocks[(size_t)i], x + 4, y + 4, cell - 8);
        }
        centeredText("方块", x0 + cell, y0 - 22.0f, 0.8f, 1, 1, 1, 1);

        const float opW = 248.0f;
        const float opX = (float)scrW - opW - 16.0f;
        const float opY = 40.0f;
        const float opBtnH = 46.0f;
        const float opBtnW = opW - 28.0f;
        const float opBtnX = opX + 14.0f;
        const float saveY = opY + 78.0f;
        const float switchY = saveY + opBtnH + 10.0f;
        const float opH = (switchY + opBtnH + 16.0f) - opY;
        quad(opX, opY, opW, opH, 0, 0, 0, 0, 0.08f, 0.08f, 0.10f, 0.92f);
        bool saveHover = ui.mouseX >= opBtnX && ui.mouseX < opBtnX + opBtnW
            && ui.mouseY >= saveY && ui.mouseY < saveY + opBtnH;
        bool switchHover = ui.mouseX >= opBtnX && ui.mouseX < opBtnX + opBtnW
            && ui.mouseY >= switchY && ui.mouseY < switchY + opBtnH;
        if (saveHover) ui.structureOpHover = 0;
        if (switchHover) ui.structureOpHover = 1;
        buttonChrome(opBtnX, saveY, opBtnW, opBtnH, saveHover, 0.28f, 0.48f, 0.24f);
        buttonChrome(opBtnX, switchY, opBtnW, opBtnH, switchHover);
        flushUI(progUI, whiteTex);
        centeredText("操作", opX + opW * 0.5f, opY + 22.0f, 0.9f, 1, 1, 1, 1);
        centeredText(ui.structureFile.empty() ? "未命名" : ui.structureFile,
                     opX + opW * 0.5f, opY + 48.0f, 0.72f, 0.85f, 0.85f, 0.8f, 1);
        centeredText("保存建筑", opBtnX + opBtnW * 0.5f, saveY + opBtnH * 0.5f, 0.95f, 1, 1, 1, 1);
        centeredText("切换建筑文件", opBtnX + opBtnW * 0.5f, switchY + opBtnH * 0.5f, 0.9f, 1, 1, 1, 1);
    }

    gl::Enable(GL_DEPTH_TEST);
}

void Renderer::drawDeath(UIState& ui) {
    ui.deathHover = -1;
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.12f, 0.02f, 0.02f, 0.72f);

    const float bw = 280.0f, bh = 52.0f;
    const float bx = (scrW - bw) * 0.5f;
    const float by = scrH * 0.5f + 40.0f;
    bool hover = (ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= by && ui.mouseY < by + bh);
    if (hover) ui.deathHover = 0;
    buttonChrome(bx, by, bw, bh, hover);
    flushUI(progUI, whiteTex);

    centeredText("你已死亡", scrW * 0.5f, scrH * 0.5f - 40.0f, 1.6f, 0.95f, 0.25f, 0.22f, 1.0f);
    centeredText("头 / 上胸 / 腰腹核心 完全失能", scrW * 0.5f, scrH * 0.5f, 0.85f, 0.80f, 0.80f, 0.80f, 1.0f);
    centeredText("重生", bx + bw * 0.5f, by + bh * 0.5f, 1.0f, 1, 1, 1, 1);
}

void Renderer::drawTargetPanel(UIState& ui) {
    ui.targetBtnHover = -1;
    if (!ui.targets || ui.targetPanel < 0 || ui.targetPanel >= (int)ui.targets->size()) return;
    const TrainingTarget& target = (*ui.targets)[(size_t)ui.targetPanel];

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.45f);

    const float dollH = std::min(520.0f, (float)scrH - 96.0f);
    const float dollW = dollH * (248.0f / 470.0f);
    const float btnW = 220.0f;
    const float btnH = 52.0f;
    const float btnGap = 14.0f;
    const float midGap = 28.0f;
    const float groupW = dollW + midGap + btnW;
    const float groupH = dollH;
    const float x = ((float)scrW - groupW) * 0.5f;
    const float y = ((float)scrH - groupH) * 0.5f;
    drawInventoryDoll(ui, x, y, dollW, dollH, &target.vitals, false);

    const float bx = x + dollW + midGap;
    const float stackH = btnH * 2.0f + btnGap;
    const float by0 = y + (dollH - stackH) * 0.5f;
    const float by1 = by0 + btnH + btnGap;
    bool dismantle = ui.mouseX >= bx && ui.mouseX < bx + btnW && ui.mouseY >= by0 && ui.mouseY < by0 + btnH;
    bool reset = ui.mouseX >= bx && ui.mouseX < bx + btnW && ui.mouseY >= by1 && ui.mouseY < by1 + btnH;
    if (dismantle) ui.targetBtnHover = 0;
    if (reset) ui.targetBtnHover = 1;
    buttonChrome(bx, by0, btnW, btnH, dismantle, 0.55f, 0.28f, 0.24f);
    buttonChrome(bx, by1, btnW, btnH, reset, 0.28f, 0.48f, 0.24f);
    flushUI(progUI, whiteTex);
    centeredText("标靶", x + dollW * 0.5f, y - 18.0f, 1.05f, 0.95f, 0.95f, 0.93f, 1.0f);
    centeredText("拆除标靶", bx + btnW * 0.5f, by0 + btnH * 0.5f, 0.95f, 1, 1, 1, 1);
    centeredText("重置血量", bx + btnW * 0.5f, by1 + btnH * 0.5f, 0.95f, 1, 1, 1, 1);
}

void Renderer::drawInventoryDoll(UIState& ui, float x, float y, float w, float h,
                                 const vitals::Vitals* health, bool showStamina) {
    if (w < 8.0f || h < 8.0f) return;

    quad(x - 4, y - 4, w + 8, h + 8, 0, 0, 0, 0, 0.08f, 0.08f, 0.09f, 1.0f);
    quad(x, y, w, h, 0, 0, 0, 0, 0.14f, 0.14f, 0.16f, 1.0f);

    const bool survival = health ? showStamina : (!ui.privilegeMode && ui.vitals);
    const vitals::Vitals* vp = health ? health : (survival ? ui.vitals : nullptr);
    const Vec3 paper{ 0.56f, 0.55f, 0.53f };
    const Vec3 ink = vitals::outlineColor();

    auto fillOf = [&](int limb) -> Vec3 {
        if (!vp) return paper;
        return vitals::healthColor(vp->limb[limb].health);
    };
    auto stamOf = [&](int limb) -> float {
        if (!vp) return 1.0f;
        return vp->limb[limb].stamina;
    };

    const float pad = 24.0f;
    const float gap = std::max(18.0f, h * 0.032f);
    const float cx = x + w * 0.5f;
    const float innerH = h - pad * 2.0f;

    const float headH = innerH * 0.145f;
    const float headW = headH * 0.90f;
    const float chestH = innerH * 0.155f;
    const float chestW = innerH * 0.175f;
    const float coreH = innerH * 0.175f;
    const float coreW = chestW * 0.86f; // 腰腹核心略窄于上胸
    const float armW = innerH * 0.078f;
    const float armH = chestH + gap + coreH * 0.62f;
    const float legW = innerH * 0.088f;
    const float legH = innerH * 0.30f;
    const float armGap = gap * 1.35f;
    const float legGap = gap * 1.45f;

    const float headX = cx - headW * 0.5f;
    const float headY = y + pad;
    const float chestX = cx - chestW * 0.5f;
    const float chestY = headY + headH + gap;
    const float coreX = cx - coreW * 0.5f;
    const float coreY = chestY + chestH + gap;
    const float armY = chestY;
    const float handLX = cx - chestW * 0.5f - armGap - armW;
    const float handRX = cx + chestW * 0.5f + armGap;
    const float footY = coreY + coreH + gap;
    const float footLX = cx - legGap * 0.5f - legW;
    const float footRX = cx + legGap * 0.5f;

    const float ol = 3.0f; // contour thickness
    auto sil = [&](float px, float py, float pw, float ph, int limb) {
        Vec3 f = fillOf(limb);
        quad(px, py, pw, ph, 0, 0, 0, 0, ink.x, ink.y, ink.z, 1.0f);
        float ix = px + ol, iy = py + ol, iw = pw - ol * 2.0f, ih = ph - ol * 2.0f;
        if (iw > 1.0f && ih > 1.0f)
            quad(ix, iy, iw, ih, 0, 0, 0, 0, f.x, f.y, f.z, 1.0f);
    };

    sil(headX, headY, headW, headH, vitals::Head);
    sil(chestX, chestY, chestW, chestH, vitals::Chest);
    sil(coreX, coreY, coreW, coreH, vitals::Core);
    sil(handLX, armY, armW, armH, vitals::HandL);
    sil(handRX, armY, armW, armH, vitals::HandR);
    sil(footLX, footY, legW, legH, vitals::FootL);
    sil(footRX, footY, legW, legH, vitals::FootR);

    const float pipW = 6.0f, pipH = 5.0f, pipGap = 1.5f;
    const int nPip = vitals::kStaminaPips;
    const float barW = nPip * pipW + (nPip - 1) * pipGap;
    auto stam = [&](float bx, float by, int limb) {
        if (!survival) return;
        float frac = stamOf(limb);
        quad(bx - 1, by - 1, barW + 2, pipH + 2, 0, 0, 0, 0, 0.05f, 0.05f, 0.06f, 0.9f);
        for (int i = 0; i < nPip; i++) {
            float px = bx + i * (pipW + pipGap);
            float fill = clampf(frac * (float)nPip - (float)i, 0.0f, 1.0f);
            quad(px, by, pipW, pipH, 0, 0, 0, 0, 0.18f, 0.18f, 0.20f, 0.95f);
            if (fill > 0.02f)
                quad(px, by, pipW * fill, pipH, 0, 0, 0, 0, 0.30f, 0.62f, 0.82f, 0.95f);
        }
    };

    // Slots sit in the gaps, not on the silhouette.
    stam(headX + (headW - barW) * 0.5f, headY - pipH - 6.0f, vitals::Head);
    stam(chestX + (chestW - barW) * 0.5f, chestY + chestH + 4.0f, vitals::Chest);
    stam(coreX + (coreW - barW) * 0.5f, coreY + coreH + 5.0f, vitals::Core);
    stam(handLX + (armW - barW) * 0.5f, armY + armH + 6.0f, vitals::HandL);
    stam(handRX + (armW - barW) * 0.5f, armY + armH + 6.0f, vitals::HandR);
    stam(footLX + (legW - barW) * 0.5f, footY + legH + 6.0f, vitals::FootL);
    stam(footRX + (legW - barW) * 0.5f, footY + legH + 6.0f, vitals::FootR);

    flushUI(progUI, whiteTex);

    auto lab = [&](float cxp, float cy, int limb) {
        centeredText(vitals::limbLabel(limb), cxp, cy, 0.55f, 0.78f, 0.78f, 0.76f, 1.0f);
    };
    lab(cx, headY + headH * 0.58f, vitals::Head);
    lab(cx, chestY + chestH * 0.5f, vitals::Chest);
    lab(cx, coreY + coreH * 0.5f, vitals::Core);
    lab(handLX + armW * 0.5f, armY + armH * 0.45f, vitals::HandL);
    lab(handRX + armW * 0.5f, armY + armH * 0.45f, vitals::HandR);
    lab(footLX + legW * 0.5f, footY + legH * 0.45f, vitals::FootL);
    lab(footRX + legW * 0.5f, footY + legH * 0.45f, vitals::FootR);
}

void Renderer::drawNote(UIState& ui) {
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.55f);
    float w = std::min(760.0f, (float)scrW - 48.0f);
    float h = std::min(560.0f, (float)scrH - 48.0f);
    float x = ((float)scrW - w) * 0.5f;
    float y = ((float)scrH - h) * 0.5f;
    quad(x, y, w, h, 0, 0, 0, 0, 0.08f, 0.07f, 0.05f, 0.96f);
    flushUI(progUI, whiteTex);
    centeredText(ui.noteTitle.empty() ? "笔记" : ui.noteTitle, x + w * 0.5f, y + 28.0f, 1.4f, 0.95f, 0.9f, 0.75f, 1);
    for (int i = 0; i < ui.noteLineCount && i < 8; i++)
        drawString(ui.noteLines[i], x + 36.0f, y + 56.0f + (float)i * 26.0f, 1.0f, 0.9f, 0.88f, 0.8f, 1);
    float iy = y + 56.0f + (float)ui.noteLineCount * 26.0f + 18.0f;
    for (int i = 0; i < 3; i++) {
        if (ui.noteItems[i].empty()) continue;
        float row = iy + (float)i * 48.0f;
        quad(x + 36.0f, row, 40.0f, 40.0f, 0, 0, 0, 0, 0.16f, 0.14f, 0.1f, 1);
        flushUI(progUI, whiteTex);
        if (ui.noteItemId[i] != 0)
            drawBlockIcon(ui.noteItemId[i], x + 40.0f, row + 4.0f, 32.0f);
        flushUI(progUI, atlasTex);
        drawString(ui.noteItems[i], x + 88.0f, row + 8.0f, 1.05f, 0.95f, 0.93f, 0.86f, 1);
        const char* mark = "尚未找到";
        float mr = 0.75f, mg = 0.55f, mb = 0.5f;
        if (ui.notePlaced[i]) { mark = "已放上祭坛"; mr = 0.55f; mg = 0.85f; mb = 0.55f; }
        else if (ui.noteHeld[i]) { mark = "已携带"; mr = 0.55f; mg = 0.85f; mb = 0.55f; }
        drawString(mark, x + w - 160.0f, row + 8.0f, 0.9f, mr, mg, mb, 1);
    }
    if (ui.noteDone)
        centeredText("仪式已完成", x + w * 0.5f, y + h - 78.0f, 1.1f, 0.85f, 0.78f, 0.45f, 1);
    else
        centeredText("将三件物品放到祭坛的三角区域", x + w * 0.5f, y + h - 78.0f, 1.0f, 0.75f, 0.72f, 0.62f, 1);
    float bw = 120.0f, bh = 34.0f;
    float bx = x + w - bw - 28.0f;
    float by = y + h - 52.0f;
    ui.noteBackX = bx; ui.noteBackY = by; ui.noteBackW = bw; ui.noteBackH = bh;
    bool hov = ui.mouseX >= bx && ui.mouseX < bx + bw && ui.mouseY >= by && ui.mouseY < by + bh;
    ui.noteBackHover = hov;
    quad(bx, by, bw, bh, 0, 0, 0, 0, hov ? 0.32f : 0.18f, hov ? 0.28f : 0.16f, hov ? 0.18f : 0.1f, 1);
    flushUI(progUI, whiteTex);
    centeredText("返回", bx + bw * 0.5f, by + bh * 0.5f, 1.0f, 1, 1, 1, 1);
}

void Renderer::drawGuide(UIState& ui) {
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.62f);
    float w = std::min(820.0f, (float)scrW - 48.0f);
    float h = std::min(570.0f, (float)scrH - 48.0f);
    float x = ((float)scrW - w) * 0.5f;
    float y = ((float)scrH - h) * 0.5f;
    quad(x, y, w, h, 0, 0, 0, 0, 0.07f, 0.09f, 0.08f, 0.98f);
    quad(x + 12.0f, y + 12.0f, w - 24.0f, h - 24.0f, 0, 0, 0, 0,
         0.14f, 0.13f, 0.10f, 0.98f);
    flushUI(progUI, whiteTex);
    centeredText("新手指南", x + w * 0.5f, y + 30.0f, 1.45f, 0.96f, 0.90f, 0.67f, 1.0f);
    centeredText(ui.guideTitle, x + w * 0.5f, y + 76.0f, 1.20f, 0.86f, 0.91f, 0.82f, 1.0f);
    for (int i = 0; i < ui.guideLineCount && i < 7; ++i)
        drawString(ui.guideLines[i], x + 52.0f, y + 122.0f + (float)i * 42.0f,
                   1.0f, 0.91f, 0.89f, 0.82f, 1.0f);
    centeredText(std::to_string(ui.guidePage + 1) + " / " + std::to_string(ui.guidePageCount),
                 x + w * 0.5f, y + h - 48.0f, 0.9f, 0.70f, 0.72f, 0.68f, 1.0f);

    float bw = 116.0f, bh = 38.0f, by = y + h - 68.0f;
    float prevX = x + 28.0f, nextX = x + w - bw - 28.0f;
    float closeX = x + (w - bw) * 0.5f;
    ui.guidePrevHover = ui.mouseX >= prevX && ui.mouseX < prevX + bw &&
        ui.mouseY >= by && ui.mouseY < by + bh && ui.guidePage > 0;
    ui.guideNextHover = ui.mouseX >= nextX && ui.mouseX < nextX + bw &&
        ui.mouseY >= by && ui.mouseY < by + bh && ui.guidePage + 1 < ui.guidePageCount;
    ui.guideCloseHover = ui.mouseX >= closeX && ui.mouseX < closeX + bw &&
        ui.mouseY >= by && ui.mouseY < by + bh;
    buttonChrome(prevX, by, bw, bh, ui.guidePrevHover, ui.guidePage > 0 ? .47f : .24f,
                 ui.guidePage > 0 ? .47f : .24f, ui.guidePage > 0 ? .47f : .24f);
    buttonChrome(closeX, by, bw, bh, ui.guideCloseHover);
    buttonChrome(nextX, by, bw, bh, ui.guideNextHover,
                 ui.guidePage + 1 < ui.guidePageCount ? .47f : .24f,
                 ui.guidePage + 1 < ui.guidePageCount ? .47f : .24f,
                 ui.guidePage + 1 < ui.guidePageCount ? .47f : .24f);
    flushUI(progUI, whiteTex);
    centeredText("上一页", prevX + bw * .5f, by + bh * .5f, .95f, 1, 1, 1, 1);
    centeredText("关闭", closeX + bw * .5f, by + bh * .5f, .95f, 1, 1, 1, 1);
    centeredText("下一页", nextX + bw * .5f, by + bh * .5f, .95f, 1, 1, 1, 1);
}

void Renderer::drawClue(UIState& ui) {
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.60f);
    float w = std::min(700.0f, (float)scrW - 48.0f);
    float h = std::min(430.0f, (float)scrH - 48.0f);
    float x = ((float)scrW - w) * 0.5f;
    float y = ((float)scrH - h) * 0.5f;
    quad(x, y, w, h, 0, 0, 0, 0, 0.10f, 0.085f, 0.055f, 0.98f);
    flushUI(progUI, whiteTex);
    centeredText("建筑线索", x + w * 0.5f, y + 34.0f, 1.45f, 0.95f, 0.83f, 0.55f, 1.0f);
    if (!ui.clueTargetActive) {
        centeredText("这张线索尚未绑定目标", x + w * 0.5f, y + 150.0f,
                     1.15f, 0.82f, 0.75f, 0.62f, 1.0f);
        centeredText("请在建筑中拾取服务器登记的下一阶段线索", x + w * 0.5f, y + 198.0f,
                     .92f, 0.70f, 0.68f, 0.62f, 1.0f);
    } else {
        centeredText("阶段 " + std::to_string(ui.clueStage) + " · " + ui.clueDestination,
                     x + w * 0.5f, y + 106.0f, 1.15f, 0.88f, 0.84f, 0.70f, 1.0f);
        const float invBlock = cfg::BLOCK_SCALE > 0 ? 1.0f / cfg::BLOCK_SCALE : 1.0f;
        int bx = (int)std::floor(ui.cluePosition.x * invBlock);
        int by = (int)std::floor(ui.cluePosition.y * invBlock);
        int bz = (int)std::floor(ui.cluePosition.z * invBlock);
        centeredText("目标方块坐标", x + w * 0.5f, y + 164.0f,
                     .90f, 0.70f, 0.72f, 0.68f, 1.0f);
        centeredText("X " + std::to_string(bx) + "    Y " + std::to_string(by) +
                     "    Z " + std::to_string(bz), x + w * 0.5f, y + 207.0f,
                     1.35f, 0.96f, 0.90f, 0.68f, 1.0f);
        if (!ui.clueReward.empty())
            centeredText("目标奖励：" + ui.clueReward, x + w * 0.5f, y + 260.0f,
                         .95f, 0.80f, 0.84f, 0.76f, 1.0f);
        if (ui.clueBossRewardClaimed)
            centeredText("Boss 圣物已掉落", x + w * 0.5f, y + 300.0f,
                         .95f, 0.55f, 0.88f, 0.55f, 1.0f);
    }
    float bw = 120.0f, bh = 38.0f;
    float bx = x + (w - bw) * .5f, by = y + h - 62.0f;
    ui.clueCloseHover = ui.mouseX >= bx && ui.mouseX < bx + bw &&
        ui.mouseY >= by && ui.mouseY < by + bh;
    buttonChrome(bx, by, bw, bh, ui.clueCloseHover);
    flushUI(progUI, whiteTex);
    centeredText("关闭", bx + bw * .5f, by + bh * .5f, .95f, 1, 1, 1, 1);
}

void Renderer::drawClueQuiz(UIState& ui) {
    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0, 0, 0, .68f);
    float w = std::min(850.0f, (float)scrW - 48.0f);
    float h = std::min(550.0f, (float)scrH - 48.0f);
    float x = ((float)scrW - w) * .5f, y = ((float)scrH - h) * .5f;
    quad(x, y, w, h, 0, 0, 0, 0, .065f, .10f, .115f, .98f);
    quad(x + 10, y + 10, w - 20, h - 20, 0, 0, 0, 0, .12f, .14f, .14f, .96f);
    flushUI(progUI, whiteTex);
    centeredText("解读建筑线索", x + w * .5f, y + 35, 1.45f, .94f, .82f, .55f, 1);
    ui.clueQuizOptionHover.fill(false);
    if (ui.clueQuizStatus == 1) {
        centeredText("学科：" + ui.clueQuizSubject, x + w * .5f, y + 82,
                     1.0f, .58f, .83f, .88f, 1);
        drawString(ui.clueQuizPrompt, x + 42, y + 125, 1.02f, .97f, .95f, .85f, 1);
        for (int i = 0; i < 4; ++i) {
            float bx = x + 42, by = y + 182 + i * 65.0f, bw = w - 84, bh = 49;
            bool hover = !ui.clueQuizSubmitting && ui.mouseX >= bx && ui.mouseX < bx + bw &&
                ui.mouseY >= by && ui.mouseY < by + bh;
            ui.clueQuizOptionHover[(size_t)i] = hover;
            buttonChrome(bx, by, bw, bh, hover, hover ? .35f : .21f,
                         hover ? .42f : .29f, hover ? .43f : .31f);
            flushUI(progUI, whiteTex);
            drawString(std::string(1, (char)('A' + i)) + ".  " + ui.clueQuizOptions[(size_t)i],
                       bx + 18, by + 14, .96f, .95f, .94f, .86f, 1);
        }
        if (ui.clueQuizSubmitting)
            centeredText("正在由服务器核对答案...", x + w * .5f, y + h - 43,
                         .85f, .78f, .83f, .84f, 1);
        else
            centeredText("单选一项；答错后 20 秒换题", x + w * .5f, y + h - 43,
                         .85f, .70f, .78f, .77f, 1);
    } else {
        const char* message = ui.clueQuizStatus == 4 && !ui.clueQuizPrompt.empty()
            ? ui.clueQuizPrompt.c_str() :
            ui.clueQuizStatus == 2 ? "回答错误，线索仍留在原处" :
            ui.clueQuizStatus == 3 ? "回答正确，线索已收入背包" :
            "线索已被队友解开、失效，或你离它太远";
        centeredText(message, x + w * .5f, y + 186, 1.20f, .93f, .88f, .72f, 1);
        if (ui.clueQuizStatus == 2)
            centeredText("约 " + std::to_string(ui.clueQuizRetrySeconds) + " 秒后再按 F，将出现另一道题",
                         x + w * .5f, y + 244, .95f, .70f, .82f, .88f, 1);
        if (ui.clueQuizStatus == 3)
            centeredText("选中线索道具并右键，可查看下一目标坐标",
                         x + w * .5f, y + 244, .95f, .70f, .82f, .88f, 1);
    }
    float bw = 120, bh = 40, bx = x + (w - bw) * .5f, by = y + h - 95;
    ui.clueQuizCloseHover = ui.mouseX >= bx && ui.mouseX < bx + bw &&
        ui.mouseY >= by && ui.mouseY < by + bh;
    buttonChrome(bx, by, bw, bh, ui.clueQuizCloseHover);
    flushUI(progUI, whiteTex);
    centeredText("关闭", bx + bw * .5f, by + bh * .5f, .95f, 1, 1, 1, 1);
}

void Renderer::drawInventory(UIState& ui) {
    ui.pointerInInventory = false;
    if (!ui.inventory) return;
    if (ui.noteRitual >= 0) {
        ui.noteX = 24.0f; ui.noteY = 16.0f; ui.noteW = 88.0f; ui.noteH = 32.0f;
        bool hov = ui.mouseX >= ui.noteX && ui.mouseX < ui.noteX + ui.noteW
            && ui.mouseY >= ui.noteY && ui.mouseY < ui.noteY + ui.noteH;
        ui.noteHover = hov;
        quad(ui.noteX, ui.noteY, ui.noteW, ui.noteH, 0, 0, 0, 0,
             hov || ui.noteOpen ? 0.32f : 0.16f, hov || ui.noteOpen ? 0.26f : 0.14f, 0.1f, 1);
        flushUI(progUI, whiteTex);
        centeredText("笔记", ui.noteX + ui.noteW * 0.5f, ui.noteY + ui.noteH * 0.5f, 1.0f, 0.95f, 0.9f, 0.75f, 1);
    }
    if (ui.noteOpen && ui.noteRitual >= 0) {
        ui.pointerInInventory = true;
        drawNote(ui);
        return;
    }

    const bool survival = !ui.privilegeMode;
    const auto& pal = plugin::creativePalette();
    const int placeable = ui.privilegeMode ? (int)pal.size() : 0;
    const bool lockBag = ui.bagLocked;
    ui.hoveredSlot = -1;
    ui.hoveredWear = -1;
    ui.hoveredBlock = -1;

    const float margin = 20.0f;
    const float helpY = (float)scrH - 28.0f;
    const float availL = margin;
    const float availT = 36.0f;
    const float availR = (float)scrW - margin;
    const float availW = std::max(240.0f, availR - availL);
    const float availH = std::max(180.0f, helpY - 16.0f - availT);

    const int cols = 9;
    const int rows = (cfg::MAIN_SLOTS + cols - 1) / cols;
    const int nBars = 6;
    const float dollAspect = 248.0f / 470.0f;
    const float wearCol = 76.0f;

    auto gridWof = [](float s, float g) { return 9.0f * s + 8.0f * g; };
    auto groupWof = [](float s, float g) {
        return (float)cfg::HAND_SLOTS * s + (float)(cfg::HAND_SLOTS - 1) * g;
    };

    float slot = 44.0f, gap = 4.0f, carryExtra = 10.0f;
    float pairGap = 36.0f, carryGap = 14.0f;
    float carryS = slot + carryExtra;
    auto hotWof = [&]() {
        return carryS + carryGap + groupWof(slot, gap) + pairGap + groupWof(slot, gap);
    };
    float invW = std::max(gridWof(slot, gap), hotWof());

    float dollH = std::min(470.0f, availH);
    float dollW = dollH * dollAspect;
    float barW = survival ? 200.0f : 0.0f;
    float barGap = 38.0f;
    float leftW = wearCol + dollW + (survival ? 16.0f + barW : 0.0f);
    bool twoCol = (leftW + 24.0f + invW <= availW);
    if (!twoCol) {
        for (float s = 42.0f; s >= 30.0f; s -= 2.0f) {
            float k = s / 44.0f;
            slot = s;
            gap = std::max(3.0f, 4.0f * k);
            carryS = slot + carryExtra * k;
            pairGap = std::max(16.0f, 36.0f * k);
            carryGap = std::max(8.0f, 14.0f * k);
            invW = std::max(gridWof(slot, gap), hotWof());
            float dw = std::min(dollW, 180.0f);
            float lw = wearCol + dw + (survival ? 16.0f + 168.0f : 0.0f);
            if (lw + 20.0f + invW <= availW) { twoCol = true; dollW = dw; dollH = dw / dollAspect; leftW = lw; barW = survival ? 168.0f : 0.0f; break; }
        }
    }

    float colW = twoCol ? availW - leftW - 24.0f : availW;
    if (invW > colW) {
        float k = colW / invW;
        slot = std::max(28.0f, slot * k);
        gap = std::max(2.0f, gap * k);
        carryS = std::max(slot + 6.0f, carryS * k);
        pairGap = std::max(12.0f, pairGap * k);
        carryGap = std::max(6.0f, carryGap * k);
        invW = std::max(gridWof(slot, gap), hotWof());
    }

    const float gridW = gridWof(slot, gap);
    const float gridH = (float)rows * slot + (float)(rows - 1) * gap;
    const float groupW = groupWof(slot, gap);
    const float hotW = hotWof();
    const float titleH = 34.0f;
    const float sec = 16.0f;
    const float labelH = 18.0f;
    const float invCoreH = titleH + gridH + sec + labelH + std::max(slot, carryS);

    bool barsBeside = false;
    float barsH = survival ? (float)nBars * barGap : 0.0f;
    float leftH = 0.0f;
    if (twoCol) {
        barsBeside = survival && (dollW + 16.0f + 160.0f <= leftW - wearCol + 1.0f);
        if (survival && !barsBeside) {
            barW = std::min(barW, leftW);
            leftH = dollH + 12.0f + barsH;
            if (leftH > availH) {
                dollH = std::max(160.0f, availH - 12.0f - barsH);
                dollW = dollH * dollAspect;
                leftW = wearCol + std::max(dollW, barW);
                leftH = dollH + 12.0f + barsH;
            }
        } else {
            leftH = std::max(dollH, survival ? barsH + 28.0f : dollH);
            if (leftH > availH) {
                dollH = availH;
                dollW = dollH * dollAspect;
                leftW = wearCol + dollW + (survival ? 16.0f + barW : 0.0f);
                leftH = dollH;
            }
        }
    } else {
        dollW = std::min(200.0f, std::max(80.0f, (availW - wearCol) * 0.42f));
        dollH = dollW / dollAspect;
        barsBeside = survival && (wearCol + dollW + 16.0f + 160.0f <= availW);
        barW = survival ? (barsBeside ? std::min(200.0f, availW - wearCol - dollW - 16.0f) : std::min(200.0f, availW - wearCol)) : 0.0f;
        leftW = wearCol + (barsBeside ? dollW + 16.0f + barW : std::max(dollW, barW));
        leftH = barsBeside ? std::max(dollH, barsH + 28.0f) : dollH + (survival ? 12.0f + barsH : 0.0f);
    }

    float pgap = 4.0f, pcell = 40.0f;
    int pcols = 1, palRows = 0;
    float palW = 0.0f, palH = 0.0f;
    float palAvailW = twoCol ? std::max(invW, colW) : availW;
    float palAvailH = twoCol ? std::max(40.0f, availH - invCoreH - 8.0f)
                             : std::max(40.0f, availH - invCoreH - sec - leftH);
    if (placeable > 0) {
        pcols = std::max(1, (int)((palAvailW + pgap) / (24.0f + pgap)));
        pcols = std::min(pcols, placeable);
        palRows = (placeable + pcols - 1) / pcols;
        pcell = std::min(40.0f, (palAvailW - (float)(pcols - 1) * pgap) / (float)pcols);
        palH = (float)palRows * pcell + (float)(palRows - 1) * pgap;
        if (palH > palAvailH) {
            pcell = std::max(18.0f, (palAvailH - (float)(palRows - 1) * pgap) / (float)palRows);
            palH = (float)palRows * pcell + (float)(palRows - 1) * pgap;
        }
        int fit = std::max(1, (int)((palAvailW + pgap) / (pcell + pgap)));
        if (fit > pcols) {
            pcols = std::min(fit, placeable);
            palRows = (placeable + pcols - 1) / pcols;
            pcell = std::min(40.0f, (palAvailW - (float)(pcols - 1) * pgap) / (float)pcols);
            palH = (float)palRows * pcell + (float)(palRows - 1) * pgap;
            if (palH > palAvailH)
                pcell = std::max(18.0f, (palAvailH - (float)(palRows - 1) * pgap) / (float)palRows);
            palH = (float)palRows * pcell + (float)(palRows - 1) * pgap;
        }
        palW = (float)pcols * pcell + (float)(pcols - 1) * pgap;
    }

    float invH = invCoreH + (placeable > 0 ? 16.0f + palH : 0.0f);
    float gxCol = availL, gy0 = availT;
    float dollX = availL, dollY = availT, barX = availL, barY = availT + 28.0f;
    if (twoCol) {
        float extra = std::max(0.0f, availW - (leftW + 24.0f + std::max(invW, std::max(gridW, palW))));
        dollX = availL + extra * 0.15f;
        gxCol = dollX + leftW + 24.0f;
        colW = availR - gxCol;
        gy0 = availT;
        dollY = availT;
        if (barsBeside) { barX = dollX + wearCol + dollW + 16.0f; barY = dollY + 28.0f; }
        else { barX = dollX + wearCol; barY = dollY + dollH + 28.0f; }
    } else {
        gxCol = availL;
        colW = availW;
        gy0 = availT;
        dollX = availL;
        dollY = gy0 + invH + sec;
        float room = availT + availH - dollY;
        if (leftH > room && room > 90.0f) {
            float k = room / leftH;
            dollH = std::max(80.0f, dollH * k);
            dollW = dollH * dollAspect;
            if (survival && !barsBeside) barGap = std::max(22.0f, barGap * k);
            barsH = survival ? (float)nBars * barGap : 0.0f;
            leftW = wearCol + (barsBeside ? dollW + 16.0f + barW : std::max(dollW, barW));
            leftH = barsBeside ? std::max(dollH, barsH + 28.0f) : dollH + (survival ? 12.0f + barsH : 0.0f);
        }
        if (barsBeside) { barX = dollX + wearCol + dollW + 16.0f; barY = dollY + 28.0f; }
        else { barX = dollX + wearCol; barY = dollY + dollH + 28.0f; }
    }

    const float bodyX = dollX + wearCol;
    float wearGap = 8.0f;
    float wearS = 64.0f;
    if (wearS > wearCol - 8.0f) wearS = wearCol - 8.0f;
    float wearTotal = (float)wear::kOpenCount * wearS + (float)(wear::kOpenCount - 1) * wearGap;
    if (wearTotal > dollH && dollH > 1.0f) {
        float k = dollH / wearTotal;
        wearS = std::max(48.0f, wearS * k);
        wearGap = std::max(4.0f, wearGap * k);
        wearTotal = (float)wear::kOpenCount * wearS + (float)(wear::kOpenCount - 1) * wearGap;
    }
    const float wearX = dollX + (wearCol - wearS) * 0.5f;
    const float wearY0 = dollY + std::max(0.0f, (dollH - wearTotal) * 0.5f);
    float wearY[wear::kOpenCount];
    for (int i = 0; i < wear::kOpenCount; i++)
        wearY[i] = wearY0 + (float)i * (wearS + wearGap);

    const float gx0 = gxCol + std::max(0.0f, (colW - gridW) * 0.5f);
    const float mainY = gy0 + titleH;
    const float hotY = mainY + gridH + sec + labelH;
    const float carryY = hotY + slot - carryS;
    const float hotX0 = gxCol + std::max(0.0f, (colW - hotW) * 0.5f);
    const float carryInvX = hotX0;
    const float carryInvS = carryS;
    const float carryInvY = carryY;
    const float leftHX = carryInvX + carryS + carryGap;
    const float rightHX = leftHX + groupW + pairGap;
    const float px0 = gxCol + std::max(0.0f, (colW - palW) * 0.5f);
    const float py0 = hotY + slot + 16.0f;
    const float hotLabelY = carryY - 14.0f;

    auto cover = [&](float x, float y, float w, float h, float pad = 8.0f) {
        if (w <= 1.0f || h <= 1.0f) return;
        if (ui.mouseX >= x - pad && ui.mouseX < x + w + pad
            && ui.mouseY >= y - pad && ui.mouseY < y + h + pad)
            ui.pointerInInventory = true;
    };
    cover(gx0, gy0, gridW, titleH + gridH);
    cover(carryInvX, hotLabelY, (rightHX + groupW) - carryInvX, (hotY + slot) - hotLabelY);
    cover(wearX, wearY0, wearS, wearTotal);
    cover(bodyX, dollY, dollW, dollH);
    if (survival) cover(barX, barY - 20.0f, barW, (float)nBars * barGap);
    if (placeable > 0) cover(px0, py0, palW, palH);
    if (ui.noteRitual >= 0) cover(ui.noteX, ui.noteY, ui.noteW, ui.noteH, 0.0f);

    quad(0, 0, (float)scrW, (float)scrH, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.55f);

    for (int i = 0; i < cfg::MAIN_SLOTS; i++) {
        int r = i / cols, c = i % cols;
        float x = gx0 + c * (slot + gap);
        float y = mainY + r * (slot + gap);
        if (!lockBag && ui.mouseX >= x && ui.mouseX < x + slot && ui.mouseY >= y && ui.mouseY < y + slot)
            ui.hoveredSlot = cfg::HOTBAR_SLOTS + i;
        float dim = lockBag ? 0.45f : 1.0f;
        quad(x - 2, y - 2, slot + 4, slot + 4, 0, 0, 0, 0, 0.12f * dim, 0.12f * dim, 0.12f * dim, 1.0f);
        quad(x, y, slot, slot, 0, 0, 0, 0, 0.20f * dim, 0.20f * dim, 0.22f * dim, 1.0f);
    }
    for (int i = 0; i < cfg::HOTBAR_SLOTS; i++) {
        float x = splitHotbarSlotX(i, leftHX, rightHX, slot, gap);
        float y = hotY;
        if (ui.mouseX >= x && ui.mouseX < x + slot && ui.mouseY >= y && ui.mouseY < y + slot)
            ui.hoveredSlot = i;
        bool sel = splitHotbarSelected(i, ui);
        float bg = sel ? 0.38f : 0.26f;
        quad(x - 2, y - 2, slot + 4, slot + 4, 0, 0, 0, 0, 0.12f, 0.12f, 0.12f, 1.0f);
        quad(x, y, slot, slot, 0, 0, 0, 0, bg, bg, bg + 0.02f, 1.0f);
        if (sel) {
            const float bw = 2.0f;
            quad(x - bw, y - bw, slot + 2 * bw, bw, 0, 0, 0, 0, 1, 1, 1, 0.9f);
            quad(x - bw, y + slot, slot + 2 * bw, bw, 0, 0, 0, 0, 1, 1, 1, 0.9f);
            quad(x - bw, y, bw, slot, 0, 0, 0, 0, 1, 1, 1, 0.9f);
            quad(x + slot, y, bw, slot, 0, 0, 0, 0, 1, 1, 1, 0.9f);
        }
    }
    for (int i = 0; i < wear::kOpenCount; i++) {
        int slotId = wear::kOpenSlots[i];
        float x = wearX;
        float y = wearY[i];
        if (!lockBag && ui.mouseX >= x && ui.mouseX < x + wearS && ui.mouseY >= y && ui.mouseY < y + wearS)
            ui.hoveredWear = slotId;
        bool hot = ui.hoveredWear == slotId;
        float dim = lockBag ? 0.45f : 1.0f;
        float bg = hot ? 0.34f : 0.22f;
        quad(x - 2, y - 2, wearS + 4, wearS + 4, 0, 0, 0, 0, 0.10f * dim, 0.10f * dim, 0.12f * dim, 1.0f);
        quad(x, y, wearS, wearS, 0, 0, 0, 0, bg * dim, bg * dim, (bg + 0.03f) * dim, 1.0f);
    }
    {
        bool filled = ui.carrySlot && !ui.carrySlot->empty();
        float x = carryInvX, y = carryInvY;
        quad(x - 2, y - 2, carryInvS + 4, carryInvS + 4, 0, 0, 0, 0, 0.16f, 0.12f, 0.08f, 1.0f);
        quad(x, y, carryInvS, carryInvS, 0, 0, 0, 0, 0.28f, 0.22f, 0.14f, 1.0f);
        if (filled) {
            const float bw = 2.0f;
            quad(x - bw, y - bw, carryInvS + 2 * bw, bw, 0, 0, 0, 0, 0.92f, 0.72f, 0.38f, 0.9f);
            quad(x - bw, y + carryInvS, carryInvS + 2 * bw, bw, 0, 0, 0, 0, 0.92f, 0.72f, 0.38f, 0.9f);
            quad(x - bw, y, bw, carryInvS, 0, 0, 0, 0, 0.92f, 0.72f, 0.38f, 0.9f);
            quad(x + carryInvS, y, bw, carryInvS, 0, 0, 0, 0, 0.92f, 0.72f, 0.38f, 0.9f);
        }
    }
    for (int i = 0; i < placeable; i++) {
        float x = px0 + (i % pcols) * (pcell + pgap);
        float y = py0 + (i / pcols) * (pcell + pgap);
        if (!lockBag && ui.mouseX >= x && ui.mouseX < x + pcell && ui.mouseY >= y && ui.mouseY < y + pcell)
            ui.hoveredBlock = pal[i];
        float dim = lockBag ? 0.45f : 1.0f;
        quad(x, y, pcell, pcell, 0, 0, 0, 0, 0.15f * dim, 0.15f * dim, 0.18f * dim, 0.9f);
    }

    if (survival && ui.vitals) {
        const float bx = barX;
        const float by0 = barY;
        const float bw = barW, bh = 16.0f, bgap = barGap;
        const vitals::Vitals& v = *ui.vitals;
        struct Bar { const char* name; float val; float r, g, b; } bars[6] = {
            { "肢体健康", vitals::meanLimbHealth(v), 0.70f, 0.66f, 0.52f },
            { "肢体耐力", vitals::meanLimbStamina(v), 0.28f, 0.72f, 0.92f },
            { "饥饿值",   v.hunger, 0.90f, 0.62f, 0.18f },
            { "口渴值",   v.thirst, 0.28f, 0.62f, 0.95f },
            { "心肺功能", v.cardio, 0.90f, 0.28f, 0.32f },
            { "灵感",     v.inspire, 0.72f, 0.42f, 0.92f },
        };
        for (int i = 0; i < 6; i++) {
            float by = by0 + i * bgap;
            quad(bx - 4, by - 18, bw + 8, bh + 28, 0, 0, 0, 0, 0.08f, 0.08f, 0.10f, 0.85f);
            quad(bx, by, bw, bh, 0, 0, 0, 0, 0.16f, 0.16f, 0.18f, 1.0f);
            float f = clampf(bars[i].val, 0.0f, 1.0f);
            if (f > 0.001f)
                quad(bx, by, bw * f, bh, 0, 0, 0, 0, bars[i].r, bars[i].g, bars[i].b, 1.0f);
        }
    }
    flushUI(progUI, whiteTex);

    drawInventoryDoll(ui, bodyX, dollY, dollW, dollH);

    for (int i = 0; i < cfg::INVENTORY_SLOTS; i++) {
        const ItemSlot& s = ui.inventory[i];
        if (s.empty()) continue;
        float x, y;
        if (i < cfg::HOTBAR_SLOTS) {
            x = splitHotbarSlotX(i, leftHX, rightHX, slot, gap);
            y = hotY;
        } else {
            int k = i - cfg::HOTBAR_SLOTS;
            x = gx0 + (k % cols) * (slot + gap);
            y = mainY + (k / cols) * (slot + gap);
        }
        drawBlockIcon(s.block, x + 4, y + 4, slot - 8);
    }
    if (ui.wear) {
        for (int i = 0; i < wear::kOpenCount; i++) {
            int slotId = wear::kOpenSlots[i];
            const ItemSlot& s = ui.wear[slotId];
            if (s.empty()) continue;
            drawBlockIcon(s.block, wearX + 4, wearY[i] + 4, wearS - 8);
        }
    }
    if (ui.carrySlot && !ui.carrySlot->empty()) {
        drawBlockIcon(ui.carrySlot->block, carryInvX + 5, carryInvY + 5, carryInvS - 10);
    }
    for (int i = 0; i < placeable; i++) {
        float x = px0 + (i % pcols) * (pcell + pgap);
        float y = py0 + (i / pcols) * (pcell + pgap);
        drawBlockIcon(pal[i], x + std::max(2.0f, pcell * 0.12f), y + std::max(2.0f, pcell * 0.12f),
                      std::max(8.0f, pcell - std::max(4.0f, pcell * 0.24f)));
    }
    if (!lockBag && !ui.held.empty()) {
        drawBlockIcon(ui.held.block, ui.mouseX - slot * 0.5f, ui.mouseY - slot * 0.5f, slot);
    }
    flushUI(progUI, atlasTex);

    if (lockBag) {
        quad(dollX - 8.0f, dollY - 24.0f, leftW + 16.0f, leftH + 32.0f, 0, 0, 0, 0, 0.02f, 0.02f, 0.02f, 0.58f);
        quad(gx0 - 8.0f, mainY - 10.0f, gridW + 16.0f, gridH + 12.0f, 0, 0, 0, 0, 0.02f, 0.02f, 0.02f, 0.58f);
        if (placeable > 0)
            quad(px0 - 8.0f, py0 - 10.0f, palW + 16.0f, palH + 12.0f, 0, 0, 0, 0, 0.02f, 0.02f, 0.02f, 0.58f);
        flushUI(progUI, whiteTex);
    }
    if (lockBag && !ui.held.empty()) {
        drawBlockIcon(ui.held.block, ui.mouseX - slot * 0.5f, ui.mouseY - slot * 0.5f, slot);
        flushUI(progUI, atlasTex);
    }

    const float titleA = lockBag ? 0.40f : 1.0f;
    centeredText("物品栏", gx0 + gridW * 0.5f, gy0 + 14.0f, 1.2f, 1, 1, 1, titleA);
    centeredText("搬运", carryInvX + carryInvS * 0.5f, hotLabelY, 0.65f, 0.92f, 0.78f, 0.50f, titleA);
    centeredText("左手", leftHX + groupW * 0.5f, hotLabelY, 0.65f, 0.90f, 0.90f, 0.90f, titleA);
    centeredText("右手", rightHX + groupW * 0.5f, hotLabelY, 0.65f, 0.90f, 0.90f, 0.90f, titleA);
    if (survival) {
        centeredText("身体", bodyX + dollW * 0.5f, dollY - 16.0f, 0.9f, 0.85f, 0.85f, 0.85f, lockBag ? 0.35f : 1.0f);
        if (ui.vitals) {
            const vitals::Vitals& v = *ui.vitals;
            const char* names[6] = { "肢体健康", "肢体耐力", "饥饿值", "口渴值", "心肺功能", "灵感" };
            float vals[6] = {
                vitals::meanLimbHealth(v), vitals::meanLimbStamina(v),
                v.hunger, v.thirst, v.cardio, v.inspire
            };
            const float ta = lockBag ? 0.35f : 1.0f;
            for (int i = 0; i < 6; i++) {
                float by = barY + i * barGap;
                text(barX, by - 16.0f, 0.7f, 0.90f, 0.90f, 0.90f, ta, "%s", names[i]);
                text(barX + barW - 52.0f, by - 16.0f, 0.65f, 0.75f, 0.75f, 0.75f, ta, "%d%%", (int)(vals[i] * 100.0f + 0.5f));
            }
        }
    } else {
        centeredText("权限模式", bodyX + dollW * 0.5f, dollY - 16.0f, 0.85f, 0.85f, 0.75f, 0.40f, lockBag ? 0.35f : 1.0f);
    }

    for (int i = 0; i < cfg::INVENTORY_SLOTS; i++) {
        const ItemSlot& s = ui.inventory[i];
        if (s.empty() || s.count <= 1) continue;
        float x, y;
        if (i < cfg::HOTBAR_SLOTS) { x = splitHotbarSlotX(i, leftHX, rightHX, slot, gap); y = hotY; }
        else { int k = i - cfg::HOTBAR_SLOTS; x = gx0 + (k % cols) * (slot + gap); y = mainY + (k / cols) * (slot + gap); }
        float ca = (lockBag && i >= cfg::HOTBAR_SLOTS) ? 0.35f : 1.0f;
        text(x + slot - 18, y + slot - 22, 0.6f, 1, 1, 1, ca, "%d", s.count);
    }
    for (int i = 0; i < wear::kOpenCount; i++) {
        int slotId = wear::kOpenSlots[i];
        const ItemSlot* s = (ui.wear && slotId >= 0 && slotId < wear::Count) ? &ui.wear[slotId] : nullptr;
        float ca = lockBag ? 0.35f : 1.0f;
        if (!s || s->empty()) {
            centeredText(wear::label(slotId), wearX + wearS * 0.5f, wearY[i] + wearS * 0.5f, 0.75f,
                         0.72f, 0.72f, 0.70f, ca);
        } else if (s->count > 1) {
            text(wearX + wearS - 18, wearY[i] + wearS - 22, 0.6f, 1, 1, 1, ca, "%d", s->count);
        }
    }
    if (!ui.held.empty() && ui.held.count > 1) {
        text(ui.mouseX + 10, ui.mouseY + 10, 0.6f, 1, 1, 1, 1, "%d", ui.held.count);
    }
    if (lockBag) {
        text(10, helpY, 0.7f, 0.90f, 0.72f, 0.35f, 0.95f,
             "移动中背包锁定  |  仅可操作左右手栏  |  停下后可整理背包");
    } else {
        text(10, helpY, 0.7f, 0.85f, 0.85f, 0.85f, 0.9f,
             survival ? "拖动=移动单个  |  Shift+拖动=移动整组  |  拖到左侧穿戴  |  拖出界面=丢弃  |  肢体颜色=健康"
                      : "拖动=移动单个  |  Shift+拖动=移动整组  |  拖出界面=丢弃  |  点击下方方块=取一组");
    }
}
