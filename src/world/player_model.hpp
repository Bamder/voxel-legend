#pragma once
#include "asset_pack.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "../core/math.hpp"

// Appearance protocol for a *registered* entity type. Geometry is 9 numbers;
// everything after is optional `key value` fields. Unknown keys are kept but
// not interpreted. A .model file never registers a new entity type — that
// requires a plugin::EntityModule (strategy + identity). The default player is
// plugin::BasicConstruction. See src/plugin/plugin.hpp.
namespace pm {

struct Part {
    Vec3 center; // center of the cuboid
    Vec3 half;   // half extents of the cuboid
    Vec3 color;  // RGB (0..1)
    int type = 0; // 0=head (pitch), 1=torso, 2=arm, 3=leg, 4=hair (pitch, material in tex)
    int side = 0; // -1 left, 0 center, 1 right (for limb swing phase)
    std::string name; // identity from the .model file (eye_l, mouth, hair, …)
    std::string tex;  // material / overlay file stem; empty = body skin
    std::string kind; // role from the file: eye, eyelid, mouth, leaf, cutout, hair, …
    std::vector<std::pair<std::string, std::string>> extra; // unknown fields, round-tripped
    int bind = -1;   // >=0: selected together in the entity editor (Bind tool)
    int uvFlip = 0;  // bit0 = mirror U, bit1 = mirror V
    int boxX = -1, boxY = -1, boxW = 0, boxH = 0, boxD = 0; // own unwrap island, texels
    std::string support{}; // host part name for a decal; empty = resolve by overlap
    Vec3 rot{};      // Euler XYZ radians; local ±half is rotated around center
};

inline Vec3 rotateAxis(const Vec3& v, const Vec3& axis, float ang) {
    Vec3 u = axis.normalized();
    if (u.lengthSq() < 1e-12f) return v;
    float c = std::cos(ang), s = std::sin(ang);
    return v * c + u.cross(v) * s + u * (u.dot(v) * (1.0f - c));
}

// Local offset -> rotated offset. Order: Rx, then Ry, then Rz.
inline Vec3 rotateEuler(const Vec3& v, const Vec3& e) {
    if (e.lengthSq() < 1e-12f) return v;
    float cx = std::cos(e.x), sx = std::sin(e.x);
    float cy = std::cos(e.y), sy = std::sin(e.y);
    float cz = std::cos(e.z), sz = std::sin(e.z);
    Vec3 p{ v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx };
    p = { p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy };
    return { p.x * cz - p.y * sz, p.x * sz + p.y * cz, p.z };
}

inline Vec3 unrotateEuler(const Vec3& v, const Vec3& e) {
    if (e.lengthSq() < 1e-12f) return v;
    float cx = std::cos(e.x), sx = std::sin(e.x);
    float cy = std::cos(e.y), sy = std::sin(e.y);
    float cz = std::cos(e.z), sz = std::sin(e.z);
    Vec3 p{ v.x * cz + v.y * sz, -v.x * sz + v.y * cz, v.z };
    p = { p.x * cy - p.z * sy, p.y, p.x * sy + p.z * cy };
    return { p.x, p.y * cx + p.z * sx, -p.y * sx + p.z * cx };
}

inline Vec3 partWorldOffset(const Part& p, const Vec3& localOff) {
    return p.center + rotateEuler(localOff, p.rot);
}

inline Vec3 partWorldFromUnrot(const Part& p, const Vec3& unrot) {
    return partWorldOffset(p, unrot - p.center);
}

inline Vec3 partLocalOffset(const Part& p, const Vec3& world) {
    return unrotateEuler(world - p.center, p.rot);
}

inline void partWorldCorners(const Part& p, Vec3 c[8]) {
    for (int i = 0; i < 8; i++) {
        Vec3 o{
            (i & 1) ? p.half.x : -p.half.x,
            ((i >> 1) & 1) ? p.half.y : -p.half.y,
            ((i >> 2) & 1) ? p.half.z : -p.half.z
        };
        c[i] = partWorldOffset(p, o);
    }
}

inline void eulerToMat(const Vec3& e, float R[9]) {
    Vec3 c0 = rotateEuler({ 1, 0, 0 }, e);
    Vec3 c1 = rotateEuler({ 0, 1, 0 }, e);
    Vec3 c2 = rotateEuler({ 0, 0, 1 }, e);
    R[0] = c0.x; R[1] = c0.y; R[2] = c0.z;
    R[3] = c1.x; R[4] = c1.y; R[5] = c1.z;
    R[6] = c2.x; R[7] = c2.y; R[8] = c2.z;
}

inline void axisAngleMat(const Vec3& axis, float ang, float R[9]) {
    Vec3 u = axis.normalized();
    float c = std::cos(ang), s = std::sin(ang), t = 1.0f - c;
    float x = u.x, y = u.y, z = u.z;
    R[0] = t * x * x + c;     R[3] = t * x * y - s * z; R[6] = t * x * z + s * y;
    R[1] = t * x * y + s * z; R[4] = t * y * y + c;     R[7] = t * y * z - s * x;
    R[2] = t * x * z - s * y; R[5] = t * y * z + s * x; R[8] = t * z * z + c;
}

inline void mat3Mul(const float A[9], const float B[9], float C[9]) {
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            C[col * 3 + row] =
                A[0 * 3 + row] * B[col * 3 + 0] +
                A[1 * 3 + row] * B[col * 3 + 1] +
                A[2 * 3 + row] * B[col * 3 + 2];
        }
    }
}

inline Vec3 matToEuler(const float R[9]) {
    float sy = -R[2];
    if (sy > 1.0f) sy = 1.0f;
    if (sy < -1.0f) sy = -1.0f;
    Vec3 e;
    e.y = std::asin(sy);
    float cy = std::sqrt(std::max(0.0f, 1.0f - sy * sy));
    if (cy > 1e-6f) {
        e.x = std::atan2(R[5], R[8]);
        e.z = std::atan2(R[1], R[0]);
    } else {
        e.x = std::atan2(-R[7], R[4]);
        e.z = 0.0f;
    }
    return e;
}

inline void composePartRot(Vec3& euler, const Vec3& axis, float ang) {
    float Rw[9], Ro[9], Rn[9];
    axisAngleMat(axis, ang, Rw);
    eulerToMat(euler, Ro);
    mat3Mul(Rw, Ro, Rn);
    euler = matToEuler(Rn);
}

inline void mirrorPartRot(Vec3& euler, int axis) {
    if (axis < 0 || axis > 2 || euler.lengthSq() < 1e-12f) return;
    float R[9], S[9] = {}, T[9], Rn[9];
    eulerToMat(euler, R);
    S[0] = S[4] = S[8] = 1.0f;
    S[axis * 3 + axis] = -1.0f;
    mat3Mul(S, R, T);
    mat3Mul(T, S, Rn);
    euler = matToEuler(Rn);
}

inline void applyPartUvFlip(const Part& p, float& u0, float& v0, float& u1, float& v1) {
    if (p.uvFlip & 1) { float t = u0; u0 = u1; u1 = t; }
    if (p.uvFlip & 2) { float t = v0; v0 = v1; v1 = t; }
}

inline const std::string& partName(const Part& p) {
    return !p.name.empty() ? p.name : p.tex;
}

struct OverlaySpec {
    std::string name; // PNG stem (also the tex name parts refer to)
    int w = 0, h = 0; // 0 = use the image's size
    std::string kind; // eye, eyelid, mouth, leaf, cutout, …
    std::vector<std::pair<std::string, std::string>> extra;
};

struct EntityFile {
    std::string skin;                    // body sheet stem; empty = "player"
    int skinW = 0, skinH = 0;
    std::vector<OverlaySpec> overlays;   // from `overlay` lines
    std::vector<std::string> faceNames;  // from a `face` line (struct overlay parts)
    std::vector<Part> parts;
    std::vector<std::string> mouthTex;   // from a `mouth` line; empty = use defaults
    std::vector<std::string> extraLines; // unknown record types, round-tripped
};

inline std::string playerModelPath() { return pack::entityModel("player"); }
inline const char* kPlayerModelPath = "assets/entities/player.model"; // default pack
inline std::string leafTexPath() { return pack::resolvePng("leaf"); }
inline const char* kLeafTexPath = "assets/entities/leaf.png";

inline bool isLeafFileTex(const std::string& name) {
    return name == "leaf" || name == "leaf.png";
}

inline int thinAxis(const Part& p) {
    if (p.half.x <= p.half.y && p.half.x <= p.half.z) return 0;
    if (p.half.y <= p.half.z) return 1;
    return 2;
}

// Local corners of a textured overlay: bl, br, tr, tl.
inline void texQuadLocal(const Part& p, Vec3 q[4]) {
    float x0 = p.center.x - p.half.x, x1 = p.center.x + p.half.x;
    float y0 = p.center.y - p.half.y, y1 = p.center.y + p.half.y;
    float z0 = p.center.z - p.half.z, z1 = p.center.z + p.half.z;
    int a = thinAxis(p);
    if (a == 2) {
        float z = p.center.z;
        q[0] = { x0, y0, z }; q[1] = { x1, y0, z }; q[2] = { x1, y1, z }; q[3] = { x0, y1, z };
    } else if (a == 1) {
        float y = p.center.y;
        q[0] = { x0, y, z0 }; q[1] = { x1, y, z0 }; q[2] = { x1, y, z1 }; q[3] = { x0, y, z1 };
    } else {
        float x = p.center.x;
        q[0] = { x, y0, z0 }; q[1] = { x, y0, z1 }; q[2] = { x, y1, z1 }; q[3] = { x, y1, z0 };
    }
    for (int i = 0; i < 4; i++) q[i] = partWorldFromUnrot(p, q[i]);
}
// Head-pitch pivot: head-neck joint (head bottom). 5-head canon, height 1.80.
inline constexpr float kNeckY = 1.44f;
inline constexpr float kHeadCenterY = 1.62f; // default head cuboid center; matches cfg::EYE_HEIGHT
inline constexpr float kBodyFollowRad = 5.0f * kPi / 180.0f;

// Model +Z is the face and follows lookDir (sin yaw, -cos yaw).
// The rig's right hand (arm_r, editor Tool R) sits on model -X. Flip that
// axis onto the character's right (cos yaw, sin yaw). Det +1, so the body
// yaws with the mouse instead of mirroring the turn.
inline void lookYawXZ(float lx, float lz, float lookYaw, float& ox, float& oz) {
    float cy = std::cos(lookYaw), sy = std::sin(lookYaw);
    ox = -lx * cy + lz * sy;
    oz = -lx * sy - lz * cy;
}

// Nod around the neck so model +Z (face) follows look pitch (positive = look up).
inline void headPitchYZ(float ly, float lz, float pitch, float& oy, float& oz) {
    float dy = ly - kNeckY;
    float cp = std::cos(pitch), sp = std::sin(pitch);
    oy = kNeckY + dy * cp + lz * sp;
    oz = -dy * sp + lz * cp;
}

inline Vec3 headCameraOffset(float pitch, float lookYaw) {
    float ly, lz;
    headPitchYZ(kHeadCenterY, 0.0f, pitch, ly, lz);
    float ox, oz;
    lookYawXZ(0.0f, lz, lookYaw, ox, oz);
    return { ox, ly, oz };
}

// Default procedural model (bootstrap; overwritten by the entity file).
// 5 heads tall at 1.80. Every major extent is a multiple of the 0.04 hair grid
// so Paint tiles faces with no leftover. Torso chest:abs:hip = 6:5:4 cells.
// Legs 10:8:2 cells (~2.2 heads). Neck buried; 0.02 collar (1 texel on the body sheet). Hands match arms.
inline std::vector<Part> proceduralParts() {
    std::vector<Part> p;
    const Vec3 skin{ 0.90f, 0.72f, 0.58f };
    const Vec3 skinLo{ 0.82f, 0.64f, 0.50f };
    const Vec3 white{ 1.0f, 1.0f, 1.0f };
    const float G = 0.04f;

    const float yTop = 1.80f;
    const float headH = 9 * G;                   // 0.36
    const float headBot = yTop - headH;          // 1.44
    const float neckShow = 0.02f;                // visible collar
    const float neckH = neckShow + G;            // 0.06: 0.04 buried in chest, 0.02 shown
    const float chestTop = headBot - neckShow;   // 1.42
    const float absH = 5 * G;                    // 0.20
    const float hipH = 4 * G;                    // 0.16
    const float hipBot = 0.80f;                  // crotch (legs stay put)
    const float absBot = hipBot + hipH;          // 0.96
    const float chestBot = absBot + absH;        // 1.16
    const float chestH = chestTop - chestBot;    // 0.26
    const float thighH = 10 * G;                 // 0.40
    const float shinH = 8 * G;                   // 0.32
    const float footH = 2 * G;                   // 0.08
    const float armR = 2 * G;                    // 0.08 half → 0.16 thick
    const float chestY = chestBot + chestH * 0.5f;
    const float absY = absBot + absH * 0.5f;
    const float hipY = hipBot + hipH * 0.5f;

    // ---- head (type 0): blank skin; eyes + mouth are overlays ----
    p.push_back({ { 0.0f, headBot + headH * 0.5f, 0.0f }, { headH * 0.5f, headH * 0.5f, headH * 0.5f }, skin, 0, 0, "head", {}, {}, {} });
    auto overlay = [&](float x, float y, float z, float hx, float hy, const char* id, const char* mat, const char* kind, int type) {
        Part q;
        q.center = { x, y, z };
        q.half = { hx, hy, 0.004f };
        q.color = white;
        q.type = type;
        q.name = id;
        q.tex = mat;
        q.kind = kind;
        p.push_back(q);
    };
    overlay(-0.0623f, 1.6319f, 0.1840f, 0.0388f, 0.0249f, "eye_l", "eye", "eye", 0);
    p.back().side = -1;
    overlay( 0.0623f, 1.6319f, 0.1840f, 0.0388f, 0.0249f, "eye_r", "eye", "eye", 0);
    p.back().side = 1;
    overlay(-0.0623f, 1.6782f, 0.1860f, 0.0388f, 0.0111f, "eyelid_l", "eyelid", "eyelid", 0);
    p.back().side = -1;
    overlay( 0.0623f, 1.6782f, 0.1860f, 0.0388f, 0.0111f, "eyelid_r", "eyelid", "eyelid", 0);
    p.back().side = 1;
    overlay( 0.0000f, 1.5409f, 0.1840f, 0.0582f, 0.0194f, "mouth", "mouth_closed", "mouth", 0);

    // Neck is buried in the chest; only `neckShow` sits in the collar gap.
    const float neckY = headBot - neckH * 0.5f;
    p.push_back({ { 0.0f, neckY, 0.0f }, { 0.06f, neckH * 0.5f, 0.06f }, skinLo, 1, 0, "neck", {}, {}, {} });

    // ---- torso: chest / abdomen / hip ----
    p.push_back({ { 0.0f, chestY, 0.0f }, { 0.20f, chestH * 0.5f, 0.12f }, skin, 1, 0, "chest", {}, {}, {} });
    p.push_back({ { 0.0f, absY, 0.0f }, { 0.18f, absH * 0.5f, 0.12f }, skin, 1, 0, "abs", {}, {}, {} });
    p.push_back({ { 0.0f, hipY, 0.0f }, { 0.20f, hipH * 0.5f, 0.12f }, skin, 1, 0, "hip", {}, {}, {} });

    auto leaf = [&](float x, float y, float z, float hx, float hy, int side, const char* id) {
        Part q;
        q.center = { x, y, z };
        q.half = { hx, hy, 0.004f };
        q.color = white;
        q.type = 1;
        q.side = side;
        q.name = id;
        q.tex = "leaf";
        q.kind = "leaf";
        p.push_back(q);
    };
    leaf( 0.00f, hipY,  0.124f, 0.12f, 0.12f,  0, "leaf_f");
    leaf(-0.06f, hipY + 0.02f,  0.122f, 0.10f, 0.12f, -1, "leaf_f_l");
    leaf( 0.06f, hipY + 0.02f,  0.122f, 0.10f, 0.12f,  1, "leaf_f_r");
    leaf( 0.00f, hipY, -0.124f, 0.12f, 0.12f,  0, "leaf_b");
    leaf(-0.06f, hipY + 0.02f, -0.122f, 0.10f, 0.12f, -1, "leaf_b_l");
    leaf( 0.06f, hipY + 0.02f, -0.122f, 0.10f, 0.12f,  1, "leaf_b_r");

    const float armX = 0.20f + armR;
    const float shH = 3 * G;
    const float upH = 6 * G;
    const float foH = 5 * G;
    const float haH = 3 * G;
    const float shTop = chestTop;
    const float shBot = shTop - shH;
    const float upBot = shBot - upH;
    const float foBot = upBot - foH;
    const float haBot = foBot - haH;
    for (float sx : { -armX, armX }) {
        int side = sx < 0 ? -1 : 1;
        const char* pre = side < 0 ? "arm_l_" : "arm_r_";
        p.push_back({ { sx, shBot + shH * 0.5f, 0.0f }, { armR, shH * 0.5f, armR }, skin, 2, side, std::string(pre) + "shoulder", {}, {}, {} });
        p.push_back({ { sx, upBot + upH * 0.5f, 0.0f }, { armR, upH * 0.5f, armR }, skin, 2, side, std::string(pre) + "upper", {}, {}, {} });
        p.push_back({ { sx, foBot + foH * 0.5f, 0.0f }, { armR, foH * 0.5f, armR }, skin, 2, side, std::string(pre) + "fore", {}, {}, {} });
        p.push_back({ { sx, haBot + haH * 0.5f, 0.0f }, { armR, haH * 0.5f, armR }, skin, 2, side, std::string(pre) + "hand", {}, {}, {} });
    }

    const float thighBot = hipBot - thighH;
    const float shinBot = thighBot - shinH;
    const float legX = 0.12f;
    const float legR = 2 * G;
    for (float sx : { -legX, legX }) {
        int side = sx < 0 ? -1 : 1;
        const char* pre = side < 0 ? "leg_l_" : "leg_r_";
        p.push_back({ { sx, thighBot + thighH * 0.5f, 0.0f }, { legR, thighH * 0.5f, legR }, skin, 3, side, std::string(pre) + "thigh", {}, {}, {} });
        p.push_back({ { sx, shinBot + shinH * 0.5f, 0.0f }, { legR, shinH * 0.5f, legR }, skin, 3, side, std::string(pre) + "shin", {}, {}, {} });
        p.push_back({ { sx, footH * 0.5f, 0.04f }, { legR, footH * 0.5f, 0.12f }, skinLo, 3, side, std::string(pre) + "foot", {}, {}, {} });
    }
    return p;
}

inline std::vector<std::string> splitWs(const char* s) {
    std::vector<std::string> t;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (!*s) break;
        const char* a = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
        t.emplace_back(a, (size_t)(s - a));
    }
    return t;
}

inline bool isNumberTok(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end && end != s.c_str() && *end == '\0';
}

inline bool isKnownPartKey(const std::string& k) {
    return k == "type" || k == "side" || k == "name" || k == "tex" || k == "kind"
        || k == "bind" || k == "uvflip" || k == "support" || k == "box"
        || k == "rx" || k == "ry" || k == "rz";
}

inline void applyPartField(Part& p, const std::string& k, const std::string& v) {
    if (k == "type") p.type = std::atoi(v.c_str());
    else if (k == "side") p.side = std::atoi(v.c_str());
    else if (k == "name") p.name = v;
    else if (k == "tex") p.tex = v;
    else if (k == "kind") p.kind = v;
    else if (k == "bind") p.bind = std::atoi(v.c_str());
    else if (k == "uvflip") p.uvFlip = std::atoi(v.c_str());
    else if (k == "support") p.support = v;
    else if (k == "rx") p.rot.x = (float)std::atof(v.c_str());
    else if (k == "ry") p.rot.y = (float)std::atof(v.c_str());
    else if (k == "rz") p.rot.z = (float)std::atof(v.c_str());
    else p.extra.push_back({ k, v });
}

inline Part parsePartToks(const std::vector<std::string>& t) {
    Part p;
    if (t.size() < 10) return p;
    p.center.x = (float)std::atof(t[1].c_str());
    p.center.y = (float)std::atof(t[2].c_str());
    p.center.z = (float)std::atof(t[3].c_str());
    p.half.x = (float)std::atof(t[4].c_str());
    p.half.y = (float)std::atof(t[5].c_str());
    p.half.z = (float)std::atof(t[6].c_str());
    p.color.x = (float)std::atof(t[7].c_str());
    p.color.y = (float)std::atof(t[8].c_str());
    p.color.z = (float)std::atof(t[9].c_str());
    size_t i = 10;
    if (i < t.size() && isNumberTok(t[i])) {
        p.type = std::atoi(t[i++].c_str());
        if (i < t.size() && isNumberTok(t[i])) p.side = std::atoi(t[i++].c_str());
        if (i < t.size() && !isKnownPartKey(t[i]) && !isNumberTok(t[i])) p.tex = t[i++];
    }
    while (i < t.size()) {
        const std::string& k = t[i];
        if (k == "box" && i + 5 < t.size()) {
            p.boxX = std::atoi(t[i + 1].c_str());
            p.boxY = std::atoi(t[i + 2].c_str());
            p.boxW = std::atoi(t[i + 3].c_str());
            p.boxH = std::atoi(t[i + 4].c_str());
            p.boxD = std::atoi(t[i + 5].c_str());
            i += 6;
            continue;
        }
        if (isKnownPartKey(k) && i + 1 < t.size()) {
            applyPartField(p, k, t[i + 1]);
            i += 2;
            continue;
        }
        std::string v;
        if (i + 1 < t.size() && !isKnownPartKey(t[i + 1])) {
            v = t[i + 1];
            i += 2;
        } else {
            i += 1;
        }
        p.extra.push_back({ k, v });
    }
    return p;
}

inline EntityFile loadEntity(const char* path) {
    EntityFile ef;
    FILE* f = std::fopen(path, "rb");
    if (!f) return ef;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::vector<std::string> t = splitWs(line);
        if (t.empty() || t[0].empty() || t[0][0] == '#') continue;
        auto keepRaw = [&]() {
            std::string raw = line;
            while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
            if (!raw.empty()) ef.extraLines.push_back(raw);
        };
        if (t[0] == "part") {
            if (t.size() >= 10) ef.parts.push_back(parsePartToks(t));
            else keepRaw();
            continue;
        }
        if (t[0] == "mouth") {
            for (size_t i = 1; i < t.size(); i++) ef.mouthTex.push_back(t[i]);
            continue;
        }
        if (t[0] == "face") {
            for (size_t i = 1; i < t.size(); i++) ef.faceNames.push_back(t[i]);
            continue;
        }
        if (t[0] == "skin") {
            if (t.size() >= 2) ef.skin = t[1];
            if (t.size() >= 3 && isNumberTok(t[2])) ef.skinW = std::atoi(t[2].c_str());
            if (t.size() >= 4 && isNumberTok(t[3])) ef.skinH = std::atoi(t[3].c_str());
            continue;
        }
        if (t[0] == "overlay") {
            if (t.size() < 2) { keepRaw(); continue; }
            OverlaySpec o;
            o.name = t[1];
            size_t i = 2;
            if (i < t.size() && isNumberTok(t[i])) o.w = std::atoi(t[i++].c_str());
            if (i < t.size() && isNumberTok(t[i])) o.h = std::atoi(t[i++].c_str());
            while (i < t.size()) {
                const std::string& k = t[i];
                if (k == "kind" && i + 1 < t.size()) {
                    o.kind = t[i + 1];
                    i += 2;
                    continue;
                }
                std::string v;
                if (i + 1 < t.size() && t[i + 1] != "kind") {
                    v = t[i + 1];
                    o.extra.push_back({ k, v });
                    i += 2;
                } else {
                    o.extra.push_back({ k, {} });
                    i += 1;
                }
            }
            ef.overlays.push_back(std::move(o));
            continue;
        }
        keepRaw();
    }
    std::fclose(f);
    return ef;
}

// Load cuboid parts from an entity model file (one `part` per line).
inline std::vector<Part> loadParts(const char* path) {
    return loadEntity(path).parts;
}

inline void writePartLine(FILE* f, const Part& p) {
    std::fprintf(f, "part %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f %.3f type %d side %d",
                 p.center.x, p.center.y, p.center.z,
                 p.half.x, p.half.y, p.half.z,
                 p.color.x, p.color.y, p.color.z, p.type, p.side);
    if (!p.name.empty()) std::fprintf(f, " name %s", p.name.c_str());
    if (!p.tex.empty()) std::fprintf(f, " tex %s", p.tex.c_str());
    if (!p.kind.empty()) std::fprintf(f, " kind %s", p.kind.c_str());
    if (p.bind >= 0) std::fprintf(f, " bind %d", p.bind);
    if (p.uvFlip) std::fprintf(f, " uvflip %d", p.uvFlip);
    if (p.boxW > 0 && p.boxH > 0 && p.boxD > 0 && p.boxX >= 0)
        std::fprintf(f, " box %d %d %d %d %d", p.boxX, p.boxY, p.boxW, p.boxH, p.boxD);
    if (!p.support.empty()) std::fprintf(f, " support %s", p.support.c_str());
    if (std::fabs(p.rot.x) > 1e-5f) std::fprintf(f, " rx %.5f", p.rot.x);
    if (std::fabs(p.rot.y) > 1e-5f) std::fprintf(f, " ry %.5f", p.rot.y);
    if (std::fabs(p.rot.z) > 1e-5f) std::fprintf(f, " rz %.5f", p.rot.z);
    for (const auto& kv : p.extra) {
        if (kv.second.empty()) std::fprintf(f, " %s", kv.first.c_str());
        else std::fprintf(f, " %s %s", kv.first.c_str(), kv.second.c_str());
    }
    std::fprintf(f, "\n");
}

inline bool saveEntity(const char* path, const EntityFile& ef) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "# entity model: skin / overlay / face / mouth records, then part lines.\n");
    std::fprintf(f, "# unknown records and part keys are ignored by the parser and written back as-is.\n");
    if (!ef.skin.empty()) {
        std::fprintf(f, "skin %s", ef.skin.c_str());
        if (ef.skinW > 0 && ef.skinH > 0) std::fprintf(f, " %d %d", ef.skinW, ef.skinH);
        std::fprintf(f, "\n");
    }
    for (const OverlaySpec& o : ef.overlays) {
        std::fprintf(f, "overlay %s", o.name.c_str());
        if (o.w > 0 && o.h > 0) std::fprintf(f, " %d %d", o.w, o.h);
        if (!o.kind.empty()) std::fprintf(f, " kind %s", o.kind.c_str());
        for (const auto& kv : o.extra) {
            if (kv.second.empty()) std::fprintf(f, " %s", kv.first.c_str());
            else std::fprintf(f, " %s %s", kv.first.c_str(), kv.second.c_str());
        }
        std::fprintf(f, "\n");
    }
    if (!ef.faceNames.empty()) {
        std::fprintf(f, "face");
        for (const std::string& n : ef.faceNames) std::fprintf(f, " %s", n.c_str());
        std::fprintf(f, "\n");
    }
    if (!ef.mouthTex.empty()) {
        std::fprintf(f, "mouth");
        for (const std::string& n : ef.mouthTex) std::fprintf(f, " %s", n.c_str());
        std::fprintf(f, "\n");
    }
    for (const std::string& s : ef.extraLines) std::fprintf(f, "%s\n", s.c_str());
    for (const Part& p : ef.parts) writePartLine(f, p);
    std::fclose(f);
    return true;
}

// Save cuboid parts to an entity model file.
inline bool saveParts(const char* path, const std::vector<Part>& parts) {
    EntityFile ef;
    ef.parts = parts;
    return saveEntity(path, ef);
}

inline bool fileExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

inline std::vector<std::string> defaultMouthTex() {
    return { "mouth_closed", "mouth_open", "mouth_smile" };
}

inline std::vector<std::string> defaultFaceNames() {
    return { "eye_l", "eye_r", "eyelid_l", "eyelid_r" };
}

inline std::vector<OverlaySpec> defaultOverlays() {
    return {
        { "eye", 8, 8, "eye", {} },
        { "eyelid", 8, 8, "eyelid", {} },
        { "mouth_closed", 16, 8, "mouth", {} },
        { "mouth_open", 16, 8, "mouth", {} },
        { "mouth_smile", 16, 8, "mouth", {} },
        { "leaf", 0, 0, "leaf", {} },
    };
}

inline void fillEntityDefaults(EntityFile& ef) {
    if (ef.parts.empty()) ef.parts = proceduralParts();
    if (ef.skin.empty()) ef.skin = "player";
    if (ef.mouthTex.empty()) ef.mouthTex = defaultMouthTex();
    if (ef.faceNames.empty()) ef.faceNames = defaultFaceNames();
    if (ef.overlays.empty()) ef.overlays = defaultOverlays();
}

// Build the player entity from the .model file (bootstrapping on first run).
inline EntityFile buildPlayerEntity() {
    std::string path = pack::entityModel("player");
    if (!fileExists(path.c_str())) {
        std::filesystem::create_directories(pack::entitiesDir());
        EntityFile ef;
        fillEntityDefaults(ef);
        saveEntity(path.c_str(), ef);
        return ef;
    }
    EntityFile ef = loadEntity(path.c_str());
    fillEntityDefaults(ef);
    return ef;
}

inline std::vector<Part> buildPlayerModel() {
    return buildPlayerEntity().parts;
}

} // namespace pm
