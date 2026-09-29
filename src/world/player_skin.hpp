#pragma once
#include "player_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

// Player texture spec (64x68 player.png). Cuboids use a VOXEL LEGEND box unwrap.
// Islands are clustered by body region (torso / arms / legs) with left and
// right of the same part sitting next to each other.
//
//          0            16           32           48           64
//     0    [ HEAD 8x8x8 unwrap (32x16)           ]
//    16    [ BODY 26x20: +X | front | -X | back  ]
//    36    [ L ARM inner ][ L extra ][ R ARM inner ][ R extra ]
//    52    [ L LEG inner ][ L foot  ][ R LEG inner ][ R foot  ]
//
// Body island is 26x20 (2d+2w by d+h). Front 9x16 is lengthened from 8x12.
// Side faces share one row: [+X 4][+Z 9][-X 4][-Z 9].
// Extra strip (same order on L and R):
//   arm  [xsect 4x4][hand +Y 4x4][hand -Y 4x4]
//   foot [foot +Y 8x10][foot -Y 8x10]
// Arm inner +Y = shoulder top, -Y = forearm bottom.
// Leg inner +Y/-Y = joint cross-section.
// Eyes / mouth / hair are separate overlays, not on this sheet.
namespace pm {

inline constexpr int kSkinW = 64;
inline constexpr int kSkinH = 68;
inline constexpr int kSkinSize = 64;
inline constexpr int kEyeSize = 8;
inline constexpr int kEyelidSize = 8;
inline constexpr int kMouthW = 16;
inline constexpr int kMouthH = 8;
inline const char* kSkinPath = "assets/entities/player.png";
inline const char* kEyePath = "assets/entities/eye.png";
inline const char* kEyelidPath = "assets/entities/eyelid.png";
inline const char* kMouthClosedPath = "assets/entities/mouth_closed.png";
inline const char* kMouthOpenPath = "assets/entities/mouth_open.png";
inline const char* kMouthSmilePath = "assets/entities/mouth_smile.png";
inline constexpr int kMouthVariantCount = 3;
inline const char* kMouthNames[kMouthVariantCount] = { "mouth_closed", "mouth_open", "mouth_smile" };
inline const char* kMouthPaths[kMouthVariantCount] = { kMouthClosedPath, kMouthOpenPath, kMouthSmilePath };
inline const char* kMouthLabs[kMouthVariantCount] = { "Closed", "Open", "Smile" };

inline bool isEyeTex(const std::string& n) {
    return n == "eye" || n == "eye_l" || n == "eye_r" || n == "eye.png";
}
inline bool isEyelidTex(const std::string& n) {
    return n == "eyelid" || n == "eyelid_l" || n == "eyelid_r" || n == "eyelid.png";
}
inline bool isMouthTex(const std::string& n) { return n.rfind("mouth_", 0) == 0 || n == "mouth"; }
inline bool isCutoutOverlay(const std::string& n) {
    return isLeafFileTex(n) || isEyeTex(n) || isEyelidTex(n) || isMouthTex(n);
}
inline bool isHairPart(const Part& p) { return p.type == 4 || p.kind == "hair"; }
inline bool isHairCardPart(const Part& p) { return p.kind == "haircard"; }
inline std::string effectiveKind(const Part& p) {
    if (!p.kind.empty()) return p.kind;
    if (p.type == 4) return "hair";
    if (isEyeTex(partName(p)) || isEyeTex(p.tex)) return "eye";
    if (isEyelidTex(partName(p)) || isEyelidTex(p.tex)) return "eyelid";
    if (partName(p) == "mouth" || isMouthTex(partName(p)) || isMouthTex(p.tex)) return "mouth";
    if (isLeafFileTex(p.tex) || isLeafFileTex(partName(p))) return "leaf";
    return {};
}
inline bool isEyePart(const Part& p) { return effectiveKind(p) == "eye"; }
inline bool isEyelidPart(const Part& p) { return effectiveKind(p) == "eyelid"; }
inline bool isMouthPart(const Part& p) { return effectiveKind(p) == "mouth"; }
inline bool isCutoutPart(const Part& p) {
    const std::string k = effectiveKind(p);
    return k == "eye" || k == "eyelid" || k == "mouth" || k == "leaf" || k == "cutout"
        || k == "haircard" || isCutoutOverlay(p.tex);
}
// Thin overlay quad stuck on a cuboid (eyes, leaf, user-added Tex).
// Hair cards are standalone textured quads, not host-bound decals.
inline bool isDecalPart(const Part& p) {
    if (isHairPart(p) || isHairCardPart(p)) return false;
    return isCutoutPart(p) || !p.support.empty() || p.kind == "decal";
}
inline float axisComp(const Vec3& v, int a) {
    return a == 0 ? v.x : (a == 1 ? v.y : v.z);
}
// Fraction of `face` that lies on `host` (samples in host-local AABB).
inline float decalFaceOnHost(const Part& d, int face, const Part& host) {
    int axis = face / 2;
    int ua = (axis + 1) % 3, va = (axis + 2) % 3;
    float sgn = (face % 2 == 0) ? 1.0f : -1.0f;
    const int N = 4;
    int hit = 0, n = 0;
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            float tu = ((float)i + 0.5f) / (float)N * 2.0f - 1.0f;
            float tv = ((float)j + 0.5f) / (float)N * 2.0f - 1.0f;
            Vec3 loff{};
            float* c[3] = { &loff.x, &loff.y, &loff.z };
            *c[axis] = sgn * axisComp(d.half, axis);
            *c[ua] = tu * axisComp(d.half, ua);
            *c[va] = tv * axisComp(d.half, va);
            Vec3 loc = partLocalOffset(host, partWorldOffset(d, loff));
            if (std::fabs(loc.x) <= host.half.x + 0.03f &&
                std::fabs(loc.y) <= host.half.y + 0.03f &&
                std::fabs(loc.z) <= host.half.z + 0.03f)
                hit++;
            n++;
        }
    }
    return n ? (float)hit / (float)n : 0.0f;
}
inline float decalOnHost(const Part& d, const Part& host) {
    float best = 0.0f;
    for (int f = 0; f < 6; f++) best = std::max(best, decalFaceOnHost(d, f, host));
    return best;
}
inline constexpr float kDecalMinCover = 0.75f;
inline int mouthIndex(const std::string& n) {
    for (int i = 0; i < kMouthVariantCount; i++)
        if (n == kMouthNames[i]) return i;
    return 0;
}
inline std::string entityPngPath(const std::string& stem) {
    return pack::entityPng(stem);
}
inline std::string skinPath() { return pack::entityPng("player"); }
inline std::string mouthPathFor(const std::string& n) {
    return pack::resolvePng(n);
}
// 0=leaf, 1=eye, 2+mouth index, 5=eyelid, -1=not a standalone overlay file
inline int overlayFileSlotOf(const std::string& n, const std::vector<std::string>& mouths) {
    if (isLeafFileTex(n)) return 0;
    if (isEyeTex(n)) return 1;
    for (int i = 0; i < (int)mouths.size() && i < kMouthVariantCount; i++)
        if (mouths[i] == n) return 2 + i;
    if (isMouthTex(n)) return 2 + mouthIndex(n);
    if (isEyelidTex(n)) return 5;
    return -1;
}
inline int overlayFileSlot(const std::string& n) {
    return overlayFileSlotOf(n, {});
}
inline constexpr int kOverlaySlotCount = 6;
// Eyes / eyelids: overlay quads that can be moved (结构可动贴图).
inline bool isStructOverlay(const Part& p) { return isEyePart(p) || isEyelidPart(p); }
// Mouth: switchable paintable materials (材质可动贴图).
inline bool isMatOverlay(const Part& p) { return isMouthPart(p); }
inline bool canGizmoPart(const Part& p) { (void)p; return true; }
inline bool canCreateDestroyPart(const Part& p) { (void)p; return true; }

// 0+X 1-X 2+Y 3-Y 4+Z 5-Z. Corners BL, BR, TR, TL from outside.
inline void cuboidFaceCorners(const Part& p, int face, Vec3 q[4]) {
    float x0 = p.center.x - p.half.x, x1 = p.center.x + p.half.x;
    float y0 = p.center.y - p.half.y, y1 = p.center.y + p.half.y;
    float z0 = p.center.z - p.half.z, z1 = p.center.z + p.half.z;
    switch (face) {
        case 0: q[0] = { x1, y0, z1 }; q[1] = { x1, y0, z0 }; q[2] = { x1, y1, z0 }; q[3] = { x1, y1, z1 }; break;
        case 1: q[0] = { x0, y0, z0 }; q[1] = { x0, y0, z1 }; q[2] = { x0, y1, z1 }; q[3] = { x0, y1, z0 }; break;
        case 2: q[0] = { x0, y1, z0 }; q[1] = { x1, y1, z0 }; q[2] = { x1, y1, z1 }; q[3] = { x0, y1, z1 }; break;
        case 3: q[0] = { x0, y0, z1 }; q[1] = { x1, y0, z1 }; q[2] = { x1, y0, z0 }; q[3] = { x0, y0, z0 }; break;
        case 4: q[0] = { x0, y0, z1 }; q[1] = { x1, y0, z1 }; q[2] = { x1, y1, z1 }; q[3] = { x0, y1, z1 }; break;
        default:q[0] = { x1, y0, z0 }; q[1] = { x0, y0, z0 }; q[2] = { x0, y1, z0 }; q[3] = { x1, y1, z0 }; break;
    }
    for (int i = 0; i < 4; i++) q[i] = partWorldFromUnrot(p, q[i]);
}

// Cuboid box at pixel origin (bx,by) with texel size w,h,d. y grows down.
// Side faces share one row: [+X][+Z][-X][-Z]. Top/bottom sit above that row.
inline void boxFacePx(int bx, int by, int w, int h, int d, int face,
                      int& x, int& y, int& fw, int& fh) {
    switch (face) {
        case 2: x = bx + d;     y = by;     fw = w; fh = d; break; // +Y top
        case 3: x = bx + d + w; y = by;     fw = w; fh = d; break; // -Y bottom
        case 0: x = bx;         y = by + d; fw = d; fh = h; break; // +X
        case 4: x = bx + d;     y = by + d; fw = w; fh = h; break; // +Z front
        case 1: x = bx + d + w; y = by + d; fw = d; fh = h; break; // -X
        default:x = bx + d + w + d; y = by + d; fw = w; fh = h; break; // -Z back
    }
}

inline void limbVRange(const Part& p, float& t0, float& t1) {
    t0 = 0.0f; t1 = 1.0f;
    const std::string& n = partName(p);
    if (p.type == 1) {
        if (n == "neck") { t0 = 0.0f; t1 = 1.0f / 16.0f; }       // 1 texel of body island
        else if (n == "chest") { t0 = 1.0f / 16.0f; t1 = 0.36f; }
        else if (n == "abs") { t0 = 0.36f; t1 = 0.66f; }
        else if (n == "hip") { t0 = 0.66f; t1 = 1.00f; }
        else if (p.center.y > 1.35f) { t0 = 0.0f; t1 = 1.0f / 16.0f; }
        else if (p.center.y > 1.17f) { t0 = 1.0f / 16.0f; t1 = 0.36f; }
        else if (p.center.y > 0.97f) { t0 = 0.36f; t1 = 0.66f; }
        else { t0 = 0.66f; t1 = 1.00f; }
    } else if (p.type == 2) {
        if (n.find("shoulder") != std::string::npos) { t0 = 0.00f; t1 = 0.18f; }
        else if (n.find("upper") != std::string::npos) { t0 = 0.18f; t1 = 0.50f; }
        else if (n.find("fore") != std::string::npos) { t0 = 0.50f; t1 = 0.82f; }
        else if (n.find("hand") != std::string::npos) { t0 = 0.82f; t1 = 1.00f; }
        else if (p.center.y > 1.27f) { t0 = 0.00f; t1 = 0.18f; }
        else if (p.center.y > 1.07f) { t0 = 0.18f; t1 = 0.50f; }
        else if (p.center.y > 0.88f) { t0 = 0.50f; t1 = 0.82f; }
        else { t0 = 0.82f; t1 = 1.00f; }
    } else if (p.type == 3) {
        if (n.find("thigh") != std::string::npos) { t0 = 0.00f; t1 = 0.50f; }
        else if (n.find("shin") != std::string::npos) { t0 = 0.50f; t1 = 0.82f; }
        else if (n.find("foot") != std::string::npos) { t0 = 0.82f; t1 = 1.00f; }
        else if (p.center.y > 0.42f) { t0 = 0.00f; t1 = 0.50f; }
        else if (p.center.y > 0.14f) { t0 = 0.50f; t1 = 0.82f; }
        else { t0 = 0.82f; t1 = 1.00f; }
    }
}

// Arms: 0 shoulder, 1 upper, 2 forearm, 3 hand. Legs: 0 thigh, 1 shin, 2 foot.
inline int limbSeg(const Part& p) {
    const std::string& n = partName(p);
    if (p.type == 2) {
        if (n.find("hand") != std::string::npos) return 3;
        if (n.find("fore") != std::string::npos) return 2;
        if (n.find("upper") != std::string::npos) return 1;
        if (n.find("shoulder") != std::string::npos) return 0;
        if (p.center.y > 1.27f) return 0;
        if (p.center.y > 1.07f) return 1;
        if (p.center.y > 0.88f) return 2;
        return 3;
    }
    if (p.type == 3) {
        if (n.find("foot") != std::string::npos) return 2;
        if (n.find("shin") != std::string::npos) return 1;
        if (n.find("thigh") != std::string::npos) return 0;
        if (p.center.y > 0.42f) return 0;
        if (p.center.y > 0.14f) return 1;
        return 2;
    }
    return -1;
}

inline constexpr int kHeadX = 0, kHeadY = 0;
inline constexpr int kBodyX = 0, kBodyY = 16;
inline constexpr int kBodyW = 9, kBodyH = 16, kBodyD = 4;
inline constexpr int kBodyIslandW = 26, kBodyIslandH = 20;
static_assert(2 * kBodyD + 2 * kBodyW == kBodyIslandW, "body island width");
static_assert(kBodyD + kBodyH == kBodyIslandH, "body island height");

// One slot per limb: inner unwrap + extra-cap cell immediately to its right.
// Row y=36 = both arms (L then R). Row y=52 = both legs (L then R).
struct SkinLimbSlot {
    int type, side;
    int bx, by; // inner unwrap
    int ox, oy; // extra-cap cell (16x16), right of inner
    int w, h, d;
};
inline constexpr SkinLimbSlot kSkinLimbs[] = {
    { 2, -1,  0, 36, 16, 36, 4, 12, 4 }, // L arm
    { 2,  1, 32, 36, 48, 36, 4, 12, 4 }, // R arm
    { 3, -1,  0, 52, 16, 52, 4, 12, 4 }, // L leg
    { 3,  1, 32, 52, 48, 52, 4, 12, 4 }, // R leg
};
inline constexpr int kSkinLimbCount = (int)(sizeof(kSkinLimbs) / sizeof(kSkinLimbs[0]));
inline constexpr int kArmCap = 4;
inline constexpr int kFootCapW = 8;
inline constexpr int kFootCapH = 10;

inline const SkinLimbSlot* findLimbSlot(int type, int side) {
    int s = side < 0 ? -1 : 1;
    for (int i = 0; i < kSkinLimbCount; i++)
        if (kSkinLimbs[i].type == type && kSkinLimbs[i].side == s)
            return &kSkinLimbs[i];
    return nullptr;
}

// Extra caps in the overlay cell, same relative order for left and right:
//   arm  [xsect 4x4][hand +Y 4x4][hand -Y 4x4]
//   foot [foot +Y 8x10][foot -Y 8x10]
// role 0 = joint cross-section, 1 = hand/foot +Y, 2 = hand/foot -Y.
struct SkinExtraCap { int x, y, w, h; int type; int side; int role; };
inline constexpr int kSkinExtraCapCount = 10;

inline bool extraCapPx(int type, int side, int role, int& x, int& y, int& fw, int& fh) {
    const SkinLimbSlot* sl = findLimbSlot(type, side);
    if (!sl) return false;
    if (type == 2) {
        if (role < 0 || role > 2) return false;
        fw = kArmCap; fh = kArmCap;
        x = sl->ox + role * kArmCap;
        y = sl->oy;
        return true;
    }
    if (type == 3 && (role == 1 || role == 2)) {
        fw = kFootCapW; fh = kFootCapH;
        x = sl->ox + (role == 2 ? kFootCapW : 0);
        y = sl->oy;
        return true;
    }
    return false;
}

inline bool extraCapByIndex(int i, SkinExtraCap& c) {
    int n = 0;
    for (int li = 0; li < kSkinLimbCount; li++) {
        const SkinLimbSlot& sl = kSkinLimbs[li];
        int r0 = (sl.type == 2) ? 0 : 1;
        for (int role = r0; role <= 2; role++) {
            if (n++ != i) continue;
            if (!extraCapPx(sl.type, sl.side, role, c.x, c.y, c.w, c.h)) return false;
            c.type = sl.type;
            c.side = sl.side;
            c.role = role;
            return true;
        }
    }
    return false;
}

// Fit an integer island inside (slotW, slotH) so fw/fh matches worldU/worldV (no stretch).
inline void fitCapAspect(int slotW, int slotH, float worldU, float worldV, int& fw, int& fh) {
    worldU = std::fabs(worldU);
    worldV = std::fabs(worldV);
    if (worldU < 1e-8f) worldU = 1e-8f;
    if (worldV < 1e-8f) worldV = 1e-8f;
    float s = std::min((float)slotW / worldU, (float)slotH / worldV);
    fw = std::max(1, (int)std::lround(worldU * s));
    fh = std::max(1, (int)std::lround(worldV * s));
    if (fw > slotW) fw = slotW;
    if (fh > slotH) fh = slotH;
}

inline void limbCapPx(const Part& p, int face, int bx, int by, int w, int h, int d,
                      int& x, int& y, int& fw, int& fh) {
    int seg = limbSeg(p);
    int side = p.side < 0 ? -1 : 1;
    if (p.type == 2) {
        if (seg == 0 && face == 2) { boxFacePx(bx, by, w, h, d, 2, x, y, fw, fh); return; }
        if (seg == 2 && face == 3) { boxFacePx(bx, by, w, h, d, 3, x, y, fw, fh); return; }
        int role = (seg == 3) ? (face == 2 ? 1 : 2) : 0;
        if (extraCapPx(2, side, role, x, y, fw, fh)) return;
    } else if (p.type == 3) {
        if (seg == 2) {
            if (extraCapPx(3, side, face == 2 ? 1 : 2, x, y, fw, fh)) {
                int slotW = fw, slotH = fh;
                fitCapAspect(slotW, slotH, p.half.x, p.half.z, fw, fh);
                return;
            }
        } else {
            boxFacePx(bx, by, w, h, d, face, x, y, fw, fh);
            return;
        }
    }
    boxFacePx(bx, by, w, h, d, face, x, y, fw, fh);
}

inline void skinFaceUV(const Part& p, int face, float& u0, float& v0, float& u1, float& v1) {
    int bx = kBodyX, by = kBodyY, w = kBodyW, h = kBodyH, d = kBodyD;
    if (p.type == 0) { bx = kHeadX; by = kHeadY; w = 8; h = 8; d = 8; }
    else if (p.type == 1) { bx = kBodyX; by = kBodyY; w = kBodyW; h = kBodyH; d = kBodyD; }
    else if (const SkinLimbSlot* sl = findLimbSlot(p.type, p.side)) {
        bx = sl->bx; by = sl->by; w = sl->w; h = sl->h; d = sl->d;
    }
    int x, y, fw, fh;
    if ((face == 2 || face == 3) && (p.type == 2 || p.type == 3)) {
        limbCapPx(p, face, bx, by, w, h, d, x, y, fw, fh);
    } else {
        boxFacePx(bx, by, w, h, d, face, x, y, fw, fh);
        if (face != 2 && face != 3 && p.type != 0) {
            float t0, t1;
            limbVRange(p, t0, t1);
            int y0 = y + (int)std::round(t0 * (float)fh);
            int y1 = y + (int)std::round(t1 * (float)fh);
            if (y1 <= y0) y1 = y0 + 1;
            y = y0;
            fh = y1 - y0;
        }
    }
    const float SW = (float)kSkinW, SH = (float)kSkinH;
    u0 = (float)x / SW;
    v0 = (float)y / SH;
    u1 = (float)(x + fw) / SW;
    v1 = (float)(y + fh) / SH;
    applyPartUvFlip(p, u0, v0, u1, v1);
}

// Body-sheet unwrap islands used by the entity editor's planar view.
struct SkinBox { int bx, by, w, h, d; int type; int side; };
inline constexpr int kSkinBoxCount = 6;
inline constexpr SkinBox kSkinBoxes[kSkinBoxCount] = {
    { kHeadX, kHeadY, 8, 8, 8, 0, 0 },
    { kBodyX, kBodyY, kBodyW, kBodyH, kBodyD, 1, 0 },
    { kSkinLimbs[0].bx, kSkinLimbs[0].by, kSkinLimbs[0].w, kSkinLimbs[0].h, kSkinLimbs[0].d, kSkinLimbs[0].type, kSkinLimbs[0].side },
    { kSkinLimbs[1].bx, kSkinLimbs[1].by, kSkinLimbs[1].w, kSkinLimbs[1].h, kSkinLimbs[1].d, kSkinLimbs[1].type, kSkinLimbs[1].side },
    { kSkinLimbs[2].bx, kSkinLimbs[2].by, kSkinLimbs[2].w, kSkinLimbs[2].h, kSkinLimbs[2].d, kSkinLimbs[2].type, kSkinLimbs[2].side },
    { kSkinLimbs[3].bx, kSkinLimbs[3].by, kSkinLimbs[3].w, kSkinLimbs[3].h, kSkinLimbs[3].d, kSkinLimbs[3].type, kSkinLimbs[3].side },
};

// Per-model unwrap. One box island per cloth cuboid, packed on that model's sheet.
// Not the human skin layout.
inline bool partHasBox(const Part& p) {
    return p.boxX >= 0 && p.boxW > 0 && p.boxH > 0 && p.boxD > 0;
}
inline bool partWantsModelSheet(const Part& p) {
    if (p.kind != "cloth") return false;
    if (isHairPart(p) || isHairCardPart(p) || isDecalPart(p) || isCutoutPart(p)) return false;
    if (!p.tex.empty()) return false;
    return true;
}
inline int sheetTexel(float fullExtent) {
    int n = (int)std::lround(std::fabs(fullExtent) / 0.025f);
    if (n < 2) n = 2;
    if (n > 48) n = 48;
    return n;
}
inline int sheetPow2(int n) {
    int p = 32;
    while (p < n && p < 512) p <<= 1;
    return p;
}
inline void modelBoxUV(const Part& p, int face, int sheetW, int sheetH,
                       float& u0, float& v0, float& u1, float& v1) {
    int x, y, fw, fh;
    boxFacePx(p.boxX, p.boxY, p.boxW, p.boxH, p.boxD, face, x, y, fw, fh);
    float SW = (float)std::max(1, sheetW), SH = (float)std::max(1, sheetH);
    u0 = (float)x / SW;
    v0 = (float)y / SH;
    u1 = (float)(x + fw) / SW;
    v1 = (float)(y + fh) / SH;
    applyPartUvFlip(p, u0, v0, u1, v1);
}
// Fill missing cloth islands. Existing boxes and a recorded sheet size are kept.
inline void assignModelSheet(std::vector<Part>& parts, int& sheetW, int& sheetH) {
    bool any = false, missing = false;
    int usedW = 0, usedH = 0;
    for (const Part& p : parts) {
        if (!partWantsModelSheet(p)) continue;
        any = true;
        if (!partHasBox(p)) missing = true;
        else {
            int r = p.boxX + 2 * p.boxD + 2 * p.boxW;
            int b = p.boxY + p.boxD + p.boxH;
            if (r > usedW) usedW = r;
            if (b > usedH) usedH = b;
        }
    }
    if (!any) { sheetW = 0; sheetH = 0; return; }
    if (!missing) {
        if (sheetW <= 0) sheetW = sheetPow2(usedW);
        if (sheetH <= 0) sheetH = sheetPow2(usedH);
        return;
    }
    const int kMax = 256;
    const int pad = 1;
    int cursorX = 0, cursorY = usedH > 0 ? usedH + pad : 0, rowH = 0;
    for (Part& p : parts) {
        if (!partWantsModelSheet(p) || partHasBox(p)) continue;
        int w = sheetTexel(p.half.x * 2.0f);
        int h = sheetTexel(p.half.y * 2.0f);
        int d = sheetTexel(p.half.z * 2.0f);
        int iw = 2 * d + 2 * w;
        int ih = d + h;
        if (cursorX > 0 && cursorX + iw > kMax) {
            cursorX = 0;
            cursorY += rowH + pad;
            rowH = 0;
        }
        p.boxX = cursorX;
        p.boxY = cursorY;
        p.boxW = w;
        p.boxH = h;
        p.boxD = d;
        cursorX += iw + pad;
        if (ih > rowH) rowH = ih;
        if (cursorX > usedW) usedW = cursorX;
    }
    usedH = cursorY + rowH;
    sheetW = sheetPow2(std::max(1, usedW));
    sheetH = sheetPow2(std::max(1, usedH));
}
inline void fillSheetRgba(std::vector<uint8_t>& rgba, int w, int h, const std::vector<Part>& parts) {
    rgba.assign((size_t)w * (size_t)h * 4, 255);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgba[i] = 48;
        rgba[i + 1] = 48;
        rgba[i + 2] = 52;
    }
    for (const Part& p : parts) {
        if (!partHasBox(p)) continue;
        uint8_t r = (uint8_t)std::lround(std::clamp(p.color.x, 0.0f, 1.0f) * 255.0f);
        uint8_t g = (uint8_t)std::lround(std::clamp(p.color.y, 0.0f, 1.0f) * 255.0f);
        uint8_t b = (uint8_t)std::lround(std::clamp(p.color.z, 0.0f, 1.0f) * 255.0f);
        for (int f = 0; f < 6; f++) {
            int x, y, fw, fh;
            boxFacePx(p.boxX, p.boxY, p.boxW, p.boxH, p.boxD, f, x, y, fw, fh);
            for (int yy = y; yy < y + fh; yy++) {
                for (int xx = x; xx < x + fw; xx++) {
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    size_t i = ((size_t)yy * (size_t)w + (size_t)xx) * 4;
                    rgba[i] = r;
                    rgba[i + 1] = g;
                    rgba[i + 2] = b;
                    rgba[i + 3] = 255;
                }
            }
        }
    }
}

} // namespace pm
