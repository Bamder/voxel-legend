#pragma once
#include "blocks.hpp"
#include "../core/math.hpp"
#include "../render/block_geo.hpp"
#include "../render/textures.hpp"

// Cut-face bits live in flags[2..7], matching geo::kFaces 0..5.
constexpr int FLAG_CUT_SHIFT = 2;
inline uint8_t flagCutFace(int face) {
    return (uint8_t)(1u << (FLAG_CUT_SHIFT + (face & 7)));
}
inline bool hasCutFace(uint8_t flags, int face) {
    return (flags & flagCutFace(face)) != 0;
}
inline int oppositeFace(int face) { return face ^ 1; }

// 0 = X, 1 = Y, 2 = Z. Faces 0/1 are Y, 2/3 are X, 4/5 are Z.
inline int faceAxis(int face) {
    return (face < 2) ? 1 : (face < 4) ? 0 : 2;
}

// Placement axis for log and stripped wood, stored in the cell's water byte.
// Real water only uses 1..WATER_MAX_LEVEL (16). 0 means upright (Y), which is
// also every log saved before placement had a direction.
constexpr uint8_t LOG_AXIS_X = 17;
constexpr uint8_t LOG_AXIS_Z = 18;

inline bool isOrientedWood(uint8_t b) { return b == LOG || b == WOOD; }

inline uint8_t logAxisLevel(int axis) {
    if (axis == 0) return LOG_AXIS_X;
    if (axis == 2) return LOG_AXIS_Z;
    return 0;
}

inline int logAxisFromLevel(uint8_t level) {
    if (level == LOG_AXIS_X) return 0;
    if (level == LOG_AXIS_Z) return 2;
    return 1;
}

// Bark and stripped grain run along the trunk. Upright logs already do that
// with the face's own UV, so those stay put.
inline void orientLogSideUV(int face, int trunkAxis, float px, float py, float pz,
                            float& tu, float& tv) {
    if (trunkAxis == 1 || faceAxis(face) == trunkAxis) return;
    int around = 3 - faceAxis(face) - trunkAxis;
    float p[3] = { px, py, pz };
    tu = p[around];
    tv = p[trunkAxis];
}

inline int faceFromDir(const Vec3& n) {
    float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ay >= ax && ay >= az) return n.y >= 0.0f ? 0 : 1;
    if (ax >= az) return n.x >= 0.0f ? 2 : 3;
    return n.z >= 0.0f ? 4 : 5;
}

inline uint8_t remapFaceCut(uint8_t src, int f, const Vec3& ax, const Vec3& ay, const Vec3& az) {
    if (!hasCutFace(src, f)) return 0;
    const int* n = geo::kFaces[f].n;
    Vec3 wn = ax * (float)n[0] + ay * (float)n[1] + az * (float)n[2];
    return flagCutFace(faceFromDir(wn));
}

// Rest-pose ±Y cuts are the faces that already showed rings. Map those (and
// only those) onto world faces after a fall. Side cuts stay off so they cannot
// turn into extra rings.
inline uint8_t remapRingCutFlags(uint8_t src, const Vec3& ax, const Vec3& ay, const Vec3& az) {
    return (uint8_t)(remapFaceCut(src, 0, ax, ay, az) | remapFaceCut(src, 1, ax, ay, az));
}

inline uint8_t remapCutFlags(uint8_t src, const Vec3& ax, const Vec3& ay, const Vec3& az) {
    uint8_t out = 0;
    for (int f = 0; f < 6; f++) out = (uint8_t)(out | remapFaceCut(src, f, ax, ay, az));
    return out;
}

template <typename Along>
bool thickQuadOrigin2(int a, int b, int& oa, int& ob, Along along) {
    int best = -1;
    for (int db = -1; db <= 0; db++) {
        for (int da = -1; da <= 0; da++) {
            int n = 0;
            if (along(a + da, b + db)) n++;
            if (along(a + da + 1, b + db)) n++;
            if (along(a + da, b + db + 1)) n++;
            if (along(a + da + 1, b + db + 1)) n++;
            if (n >= 3 && n > best) {
                best = n;
                oa = a + da;
                ob = b + db;
            }
        }
    }
    return best >= 0;
}

template <typename WoodThis, typename WoodFwd, typename CutAB>
int alignedDestroyedRing(int a, int b, int& oa, int& ob, int& qa, int& qb, int& longAxis,
                         WoodThis woodThis, WoodFwd woodFwd, CutAB cutThis) {
    qa = 0;
    qb = 0;
    longAxis = 0;
    if (!thickQuadOrigin2(a, b, oa, ob, woodThis)) return 0;
    qa = a - oa;
    qb = b - ob;
    int nFwd = 0, nCut = 0;
    int fwdI[4], fwdJ[4], nF = 0;
    int cutI[4], cutJ[4], nC = 0;
    for (int j = 0; j <= 1; j++) {
        for (int i = 0; i <= 1; i++) {
            int ia = oa + i, ib = ob + j;
            if (!woodThis(ia, ib)) continue;
            if (woodFwd(ia, ib)) {
                nFwd++;
                fwdI[nF] = i;
                fwdJ[nF] = j;
                nF++;
            }
            if (cutThis(ia, ib)) {
                nCut++;
                cutI[nC] = i;
                cutJ[nC] = j;
                nC++;
            }
        }
    }
    auto edgePair = [&](int n, const int* pi, const int* pj, int& la) {
        if (n != 2) return false;
        if (pi[0] == pi[1] && std::abs(pj[0] - pj[1]) == 1) {
            la = 1;
            return true;
        }
        if (pj[0] == pj[1] && std::abs(pi[0] - pi[1]) == 1) {
            la = 0;
            return true;
        }
        return false;
    };

    // Aligned 2x2 (overlap 3–4): splice a full ring after the plane is opened.
    if (nCut >= 3 || (nFwd >= 3 && nCut >= 1)) return 1;

    // Edge overlap of exactly 2: stretch the ring to 2x1, then split in half.
    int la = 0;
    bool strip = false;
    if (nFwd == 2 && edgePair(nF, fwdI, fwdJ, la)) strip = true;
    else if (nFwd == 0 && nCut == 2 && edgePair(nC, cutI, cutJ, la)) strip = true;
    else if (nFwd == 1 && nCut == 1 && nF == 1 && nC == 1) {
        int pi[2] = { fwdI[0], cutI[0] };
        int pj[2] = { fwdJ[0], cutJ[0] };
        if (edgePair(2, pi, pj, la)) strip = true;
    }
    if (strip) {
        longAxis = la;
        return 2;
    }
    return 0;
}

template <typename IsWood>
int logRunLen(int x, int y, int z, int dx, int dy, int dz, IsWood isWood) {
    int n = 0;
    int cx = x + dx, cy = y + dy, cz = z + dz;
    while (n < 64 && isWood(cx, cy, cz)) {
        n++;
        cx += dx;
        cy += dy;
        cz += dz;
    }
    return n;
}

// Trunk axis from geometry, with cut faces breaking ties so a short 2x2 stump
// still treats its chop as an end (rings) instead of a long side.
// Settled logs: the remapped ring-cut axis is authoritative so splice follows
// the mapped chop, not the voxelized bounding box.
template <typename IsWood>
int logTrunkAxis(int x, int y, int z, uint8_t flags, IsWood isWood) {
    int cutN[3] = { 0, 0, 0 };
    for (int f = 0; f < 6; f++) {
        if (hasCutFace(flags, f)) cutN[faceAxis(f)]++;
    }
    int fromCut = -1;
    int bestCut = 0;
    int nCutAxes = 0;
    for (int a = 0; a < 3; a++) {
        if (cutN[a] > 0) nCutAxes++;
        if (cutN[a] > bestCut || (cutN[a] == bestCut && a == 1 && cutN[a] > 0)) {
            bestCut = cutN[a];
            fromCut = a;
        }
    }
    if ((flags & FLAG_SETTLED) != 0 && nCutAxes == 1) return fromCut;

    int run[3] = {
        1 + logRunLen(x, y, z, 1, 0, 0, isWood) + logRunLen(x, y, z, -1, 0, 0, isWood),
        1 + logRunLen(x, y, z, 0, 1, 0, isWood) + logRunLen(x, y, z, 0, -1, 0, isWood),
        1 + logRunLen(x, y, z, 0, 0, 1, isWood) + logRunLen(x, y, z, 0, 0, -1, isWood)
    };
    int geom = 1;
    if (run[0] > run[geom]) geom = 0;
    if (run[2] > run[geom]) geom = 2;
    if (fromCut < 0) return geom;
    if (run[fromCut] * 2 >= run[geom]) return fromCut;
    return geom;
}

// 0 = no splice (1x1 ring), 1 = full 2x2 ring, 2 = 2x1 stretched ring split in half.
template <typename IsWood, typename HasCut>
int spliceEndRing(int x, int y, int z, int face, int trunkAxis, int& qa, int& qb, int& longAxis,
                  IsWood isWood, HasCut hasCut) {
    qa = 0;
    qb = 0;
    longAxis = 0;
    if (faceAxis(face) != trunkAxis) return 0;
    int oa = 0, ob = 0;
    int dn = geo::kFaces[face].n[trunkAxis == 1 ? 1 : (trunkAxis == 0 ? 0 : 2)];
    if (dn == 0) return 0;
    if (trunkAxis == 1) {
        auto woodThis = [&](int ix, int iz) { return isWood(ix, y, iz); };
        auto woodFwd = [&](int ix, int iz) { return isWood(ix, y + dn, iz); };
        auto cutThis = [&](int ix, int iz) { return hasCut(ix, y, iz, face); };
        return alignedDestroyedRing(x, z, oa, ob, qa, qb, longAxis, woodThis, woodFwd, cutThis);
    }
    if (trunkAxis == 0) {
        auto woodThis = [&](int iz, int iy) { return isWood(x, iy, iz); };
        auto woodFwd = [&](int iz, int iy) { return isWood(x + dn, iy, iz); };
        auto cutThis = [&](int iz, int iy) { return hasCut(x, iy, iz, face); };
        return alignedDestroyedRing(z, y, oa, ob, qa, qb, longAxis, woodThis, woodFwd, cutThis);
    }
    auto woodThis = [&](int ix, int iy) { return isWood(ix, iy, z); };
    auto woodFwd = [&](int ix, int iy) { return isWood(ix, iy, z + dn); };
    auto cutThis = [&](int ix, int iy) { return hasCut(ix, iy, z, face); };
    return alignedDestroyedRing(x, y, oa, ob, qa, qb, longAxis, woodThis, woodFwd, cutThis);
}

struct LogFaceTex {
    uint8_t tile = TEX_LOG_SIDE;
    int spliceKind = 0; // 0 none, 1 full 2x2, 2 stretched 2x1 split
    int trunkAxis = 1;
    int longAxis = 0;
    int qa = 0, qb = 0;
};

// Living and settled logs keep bark on uncut faces; rings show where a neighbor
// was opened. A placed log or stripped block puts rings on the trunk ends, so
// the ring axis is the normal of the face it was set against.
// Aligned 2x2: one ring. Edge overlap of 2: ring stretched to 2x1 then halved.
// 1-cell junction: small ring.
inline LogFaceTex logFaceTex(uint8_t flags, int face, int trunkAxis, int spliceKind,
                            int qa, int qb, int longAxis, uint8_t sideTile = TEX_LOG_SIDE) {
    LogFaceTex r;
    r.qa = qa;
    r.qb = qb;
    r.trunkAxis = trunkAxis;
    r.longAxis = longAxis;
    r.spliceKind = 0;
    bool alive = (flags & FLAG_ALIVE) != 0;
    bool settled = (flags & FLAG_SETTLED) != 0;
    bool cut = hasCutFace(flags, face);
    bool end = faceAxis(face) == trunkAxis;
    if (!cut) {
        if (alive || settled || !end) {
            r.tile = sideTile;
            return r;
        }
        r.tile = TEX_LOG_TOP;
        return r;
    }
    if (settled || end) {
        r.tile = TEX_LOG_TOP;
        r.spliceKind = spliceKind;
        return r;
    }
    r.tile = TEX_WOOD_SIDE;
    return r;
}

inline void logCornerUV(const LogFaceTex& lf, float tu, float tv,
                        float px, float py, float pz, float& u, float& v) {
    if (lf.spliceKind != 0) {
        int tx = TEX_LOG_TOP % tex::COLS;
        int ty = TEX_LOG_TOP / tex::COLS;
        float ru0 = (float)tx / (float)tex::COLS;
        float ru1 = (float)(tx + 1) / (float)tex::COLS;
        float rv0 = (float)ty / (float)tex::ROWS;
        float rv1 = (float)(ty + 1) / (float)tex::ROWS;
        float pa, pb;
        if (lf.trunkAxis == 1) {
            pa = px;
            pb = pz;
        } else if (lf.trunkAxis == 0) {
            pa = pz;
            pb = py;
        } else {
            pa = px;
            pb = py;
        }
        float su, sv;
        if (lf.spliceKind == 2) {
            if (lf.longAxis == 0) {
                su = 0.5f * ((float)lf.qa + pa);
                sv = pb;
            } else {
                su = pa;
                sv = 0.5f * ((float)lf.qb + pb);
            }
        } else {
            su = 0.5f * ((float)lf.qa + pa);
            sv = 0.5f * ((float)lf.qb + pb);
        }
        u = ru0 + (ru1 - ru0) * su;
        v = rv0 + (rv1 - rv0) * sv;
        return;
    }
    float u0, v0, u1, v1;
    tex::tileUV(lf.tile, u0, v0, u1, v1);
    u = u0 + (u1 - u0) * tu;
    v = v0 + (v1 - v0) * tv;
}
