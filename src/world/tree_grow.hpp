#pragma once
#include "../core/math.hpp"
#include "../core/noise.hpp"
#include "tree_canopy.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Shared force-field tree growth used by both the game and tree_lab.
// Grows in tree-local coordinates: trunk at (0.5, 1.5, 0.5), +Y up.
struct TreeParams {
    uint32_t seed = 1337;

    float K = 0.45f;
    float v0 = 3.5f;
    float woodStr = 0.50f;

    float sourceS = 1.0f;
    int sourceN = 5;
    float srcOffX = 0.0f;
    float srcOffZ = 0.0f;
    float srcYMin = 8.0f;
    float srcYMax = 16.0f;
    float srcRMin = 8.0f;
    float srcRMax = 14.0f;
    float srcAzi0 = 0.0f;
    float srcAziSpan = 360.0f;

    int trunkH = 10;
    int trunkW = 1; // 1 = single column, 2 = 2x2 at the base (trunk only)
    float trunkWFrac = 0.50f; // fraction of trunkH that stays thick, then 1x1
    float energyAlpha = 3.5f;

    int trunkWidth() const { return std::max(1, std::min(4, trunkW)); }
    float originXZ() const { return 0.5f * (float)trunkWidth(); }
    int sliceWidth(int depth, int placed) const {
        if (depth != 0) return 1;
        int thick = trunkWidth();
        if (thick <= 1) return 1;
        float u = std::clamp(trunkWFrac, 0.05f, 1.0f);
        int n = std::max(1, (int)std::lround((float)std::max(1, trunkH) * u));
        return (placed < n) ? thick : 1;
    }
    float maxE = 8.0f;
    float forkAngle = 30.0f;
    float maxTilt = 28.0f;
    float maxTiltFork = 48.0f;
    float lean = 0.18f;

    float upS = 0.70f;
    float upY = 36.0f;

    int maxWood = 240;
    int maxDepth = 2;
    int maxPrimary = 0; // 0 = no cap on 子枝; 1 = at most one off the trunk
    int bound = 18;
    int branchProtect = 2;
    int maxBranch = 10;

    tree_canopy::Params leaf;
};

enum class TreeKind : uint8_t { Thin = 0, Thick = 1 };

// All trees are 1x1. Thick/Thin only vary height in world gen; both use
// the former thick-trunk fork rules (primary + secondary branches).
inline void applyTreeKind(TreeParams& p, TreeKind kind) {
    (void)kind;
    p.trunkW = 1;
    p.trunkWFrac = 1.0f;
    p.maxDepth = 2;
    p.maxPrimary = 0;
    p.maxWood = 280;
}

struct TreeSource {
    float x = 0, y = 0, z = 0;
    float strength = 1.0f;
};

struct TreeShoot {
    float x = 0, y = 0, z = 0;
    float vx = 0, vy = 1, vz = 0;
    int remaining = 0, maxLen = 0, depth = 0, age = 0;
    int lastIx = -999, lastIy = -999, lastIz = -999;
    int stuck = 0;
    float forkP = 0.0f;
    int placed = 0;
    int branchIndex = 0;
    int aimSource = -1;
    bool attract = false;
    bool decide = true;
};

namespace tree_grow_detail {

inline void steerToward(float& vx, float& vy, float& vz,
                        float dx, float dy, float dz, float deg) {
    float tlen = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (tlen < 1e-5f) return;
    dx /= tlen; dy /= tlen; dz /= tlen;
    float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (speed < 1e-5f) {
        vx = 0.0f; vy = 1.0f; vz = 0.0f;
        speed = 1.0f;
    }
    float inv = 1.0f / speed;
    float vhx = vx * inv, vhy = vy * inv, vhz = vz * inv;
    float dot = vhx * dx + vhy * dy + vhz * dz;
    float pxp = dx - vhx * dot;
    float pyp = dy - vhy * dot;
    float pzp = dz - vhz * dot;
    float pl = std::sqrt(pxp * pxp + pyp * pyp + pzp * pzp);
    if (pl < 1e-4f) return;
    pxp /= pl; pyp /= pl; pzp /= pl;
    float th = std::clamp(deg, 0.0f, 80.0f) * kDeg2Rad;
    float c = std::cos(th), s = std::sin(th);
    vx = (vhx * c + pxp * s) * speed;
    vy = (vhy * c + pyp * s) * speed;
    vz = (vhz * c + pzp * s) * speed;
}

inline float primaryStartP(int i, int maxBranch) {
    return std::min(0.20f, std::max(0.0f, (float)(maxBranch - i) * 0.02f));
}

inline void clampTiltAndScale(float& vx, float& vy, float& vz, float speed, float maxTiltDeg) {
    float L = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (L < 1e-5f) {
        vx = 0.0f; vy = 1.0f; vz = 0.0f;
        L = 1.0f;
    }
    float ix = vx / L, iy = vy / L, iz = vz / L;
    float maxT = std::clamp(maxTiltDeg, 0.0f, 85.0f) * kDeg2Rad;
    float cmin = std::cos(maxT);
    if (iy < cmin) {
        float h = std::sqrt(ix * ix + iz * iz);
        float s = std::sin(maxT);
        if (h < 1e-5f) {
            ix = s; iy = cmin; iz = 0.0f;
        } else {
            ix = ix / h * s;
            iz = iz / h * s;
            iy = cmin;
        }
    }
    float sp = std::max(0.2f, speed);
    vx = ix * sp; vy = iy * sp; vz = iz * sp;
}

inline float tiltAtHeight(const TreeParams& p, bool attract, float y) {
    float t0 = attract ? p.maxTiltFork : p.maxTilt;
    float y0 = std::max(4.0f, (float)p.trunkH * 0.55f);
    float y1 = std::max(y0 + 4.0f, std::max(p.srcYMax, (float)p.trunkH + 6.0f));
    float u = std::clamp((y - y0) / (y1 - y0), 0.0f, 1.0f);
    float t = t0 * (1.0f - 0.52f * u);
    float tmin = p.maxTilt * 0.72f;
    if (t < tmin) t = tmin;
    return t;
}

inline void integrateDir(float& vx, float& vy, float& vz,
                         float fx, float fy, float fz, float speed, float maxTiltDeg) {
    float L = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (L < 1e-5f) {
        vx = 0.0f; vy = 1.0f; vz = 0.0f;
        L = 1.0f;
    }
    vx = vx / L + fx;
    vy = vy / L + fy;
    vz = vz / L + fz;
    clampTiltAndScale(vx, vy, vz, speed, maxTiltDeg);
}

inline bool inBound(const TreeParams& p, int x, int y, int z) {
    if (y < 1 || y >= p.bound + 10) return false;
    if (x < -p.bound || x > p.bound) return false;
    if (z < -p.bound || z > p.bound) return false;
    return true;
}

inline void addForce(Vec3& F, float px, float py, float pz,
                     const TreeSource& e, float K, float energy, bool attract) {
    float dx = px - e.x, dy = py - e.y, dz = pz - e.z;
    float r2 = dx * dx + dy * dy + dz * dz;
    if (r2 < 1.0f) r2 = 1.0f;
    float r = std::sqrt(r2);
    float mag = K * energy * e.strength / r2;
    float s = attract ? -1.0f : 1.0f;
    F.x += s * (dx / r) * mag;
    F.y += s * (dy / r) * mag;
    F.z += s * (dz / r) * mag;
}

} // namespace tree_grow_detail

class TreeGrow {
public:
    TreeParams params;
    std::vector<IVec3> woods;
    std::vector<uint8_t> woodS; // 1 = this voxel emits woodStr; 0 = filler (2x2 extras)
    std::vector<IVec3> leaves;
    std::vector<TreeSource> sources;
    std::vector<TreeShoot> tips;
    int tick = 0;
    bool finished = false;
    bool leavesPlaced = false;

    void reset() {
        spawnField();
        startTrunk();
    }
    void resetKeepSources() { startTrunk(); }
    void scatterSources() { spawnField(); }

    void resizeSources(int n) {
        n = std::max(0, std::min(48, n));
        params.sourceN = n;
        while ((int)sources.size() > n)
            sources.pop_back();
        while ((int)sources.size() < n)
            sources.push_back(sampleSource((int)sources.size(), n));
    }

    void growAll() {
        while (step()) {}
    }

    void buildLeaves() {
        leavesPlaced = true;
        auto ok = [&](int x, int y, int z) {
            return tree_grow_detail::inBound(params, x, y, z);
        };
        int ox = (int)std::floor(params.originXZ());
        tree_canopy::build(woods, leaves, ox, ox, 0, params.bound + 9, params.seed, ok, params.leaf);
    }

    void rebuildLeaves() {
        if (woods.empty()) {
            leaves.clear();
            return;
        }
        leavesPlaced = false;
        buildLeaves();
    }

    bool step() {
        using namespace tree_grow_detail;
        if (finished) return false;
        if (tips.empty() || (int)woods.size() >= params.maxWood) {
            if (!leavesPlaced) buildLeaves();
            finished = true;
            return false;
        }

        std::vector<TreeShoot> next;
        next.reserve(tips.size() + 4);

        for (TreeShoot s : tips) {
            int ix = (int)std::floor(s.x);
            int iy = (int)std::floor(s.y);
            int iz = (int)std::floor(s.z);
            if (!inBound(params, ix, iy, iz) || iy < 1) continue;

            if (ix == s.lastIx && iy == s.lastIy && iz == s.lastIz) {
                if (++s.stuck > 2) continue;
            } else {
                s.stuck = 0;
                addWoodFacePath(s.lastIx, s.lastIy, s.lastIz, ix, iy, iz, s.depth, s.placed);
                s.lastIx = ix; s.lastIy = iy; s.lastIz = iz;
                s.placed++;

                bool tryFork = false;
                if (s.decide && s.depth == 0 && params.maxDepth >= 1) {
                    if (params.maxPrimary <= 0 || s.branchIndex < params.maxPrimary) {
                        s.forkP = std::min(1.0f, s.forkP + 0.05f);
                        tryFork = true;
                    }
                } else if (s.decide && s.depth == 1 && params.maxDepth >= 2) {
                    int prot = std::max(0, params.branchProtect);
                    if (s.placed > prot) {
                        if (s.placed == prot + 1)
                            s.forkP = primaryStartP(s.branchIndex, params.maxBranch);
                        else
                            s.forkP = std::min(1.0f, s.forkP + 0.05f);
                        tryFork = true;
                    }
                }
                if (tryFork) {
                    float roll = noise::hash01(noise::hash3(ix, iy, iz, params.seed + 5555u + (uint32_t)s.depth));
                    if (roll < s.forkP) {
                        uint32_t fh = noise::hash3(ix, iy + 17, iz, params.seed + 6666u + (uint32_t)s.age);
                        int nd = s.depth + 1;
                        int flen = (nd == 1) ? 5 + (int)(noise::hash01(fh ^ 0x77u) * 4.0f)
                                 : (nd == 2) ? 4 + (int)(noise::hash01(fh ^ 0x77u) * 3.0f)
                                             : 3 + (int)(noise::hash01(fh ^ 0x77u) * 2.0f);
                        TreeShoot child = s;
                        child.remaining = flen;
                        child.maxLen = flen;
                        child.age = 0;
                        child.depth = nd;
                        child.stuck = 0;
                        child.forkP = 0.0f;
                        child.placed = 0;
                        child.lastIx = ix; child.lastIy = iy; child.lastIz = iz;
                        if (nd == 1) {
                            child.branchIndex = s.branchIndex++;
                            child.attract = true;
                            child.decide = params.maxDepth >= 2;
                            if (!sources.empty()) {
                                int nsrc = (int)sources.size();
                                int si = (int)(noise::hash01(fh ^ 0x51u) * (float)nsrc);
                                if (si >= nsrc) si = nsrc - 1;
                                child.aimSource = si;
                                const TreeSource& e = sources[(size_t)si];
                                steerToward(child.vx, child.vy, child.vz,
                                            e.x - child.x, e.y - child.y, e.z - child.z,
                                            params.forkAngle);
                                clampTiltAndScale(child.vx, child.vy, child.vz, params.v0, params.maxTiltFork);
                            }
                        } else {
                            child.attract = false;
                            child.decide = false;
                            child.branchIndex = 0;
                            if (!sources.empty()) {
                                int nsrc = (int)sources.size();
                                int si = s.aimSource;
                                if (si < 0 || si >= nsrc) {
                                    si = (int)(noise::hash01(fh ^ 0x51u) * (float)nsrc);
                                    if (si >= nsrc) si = nsrc - 1;
                                }
                                const TreeSource& e = sources[(size_t)si];
                                steerToward(child.vx, child.vy, child.vz,
                                            e.x - child.x, e.y - child.y, e.z - child.z,
                                            params.forkAngle);
                                child.vx = -child.vx;
                                child.vy = -child.vy;
                                child.vz = -child.vz;
                            }
                            clampTiltAndScale(child.vx, child.vy, child.vz, params.v0, params.maxTilt);
                        }
                        next.push_back(child);
                        s.forkP *= 0.25f;
                        if (s.depth == 0) {
                            s.remaining += 2;
                            s.maxLen += 2;
                        }
                    }
                }
            }

            float st = (float)std::max(0, s.age);
            float a = std::max(0.0f, params.energyAlpha);
            float energy = std::min(a * std::log(st + 1.0f), std::max(0.0f, params.maxE));
            Vec3 F{ 0, 0, 0 };
            for (const TreeSource& e : sources)
                addForce(F, s.x, s.y, s.z, e, params.K, energy, s.attract);
            if (params.upS > 1e-5f) {
                TreeSource up{
                    params.originXZ() + params.srcOffX,
                    params.upY,
                    params.originXZ() + params.srcOffZ,
                    params.upS
                };
                addForce(F, s.x, s.y, s.z, up, params.K, energy, true);
            }
            if (params.woodStr > 1e-5f) {
                int nEmit = 0;
                for (uint8_t sbit : woodS) if (sbit) nEmit++;
                int stride = nEmit > 24 ? (nEmit / 16) : 1;
                int seen = 0;
                int nWood = (int)woods.size();
                for (int wi = 0; wi < nWood; wi++) {
                    if (wi >= (int)woodS.size() || woodS[(size_t)wi] == 0) continue;
                    if ((seen++ % stride) != 0) continue;
                    const IVec3& w = woods[wi];
                    if (w.x == ix && w.y == iy && w.z == iz) continue;
                    TreeSource ws{ (float)w.x + 0.5f, (float)w.y + 0.5f, (float)w.z + 0.5f, params.woodStr };
                    addForce(F, s.x, s.y, s.z, ws, params.K, energy, false);
                }
            }
            float tilt = tiltAtHeight(params, s.attract, s.y);
            integrateDir(s.vx, s.vy, s.vz, F.x, F.y, F.z, params.v0, tilt);
            float vlen = std::sqrt(s.vx * s.vx + s.vy * s.vy + s.vz * s.vz);
            if (vlen < 1e-5f) { s.vx = 0.0f; s.vy = 1.0f; s.vz = 0.0f; vlen = 1.0f; }
            s.x += s.vx / vlen;
            s.y += s.vy / vlen;
            s.z += s.vz / vlen;
            s.age++;
            s.remaining--;
            if (s.remaining > 0 && (int)woods.size() < params.maxWood)
                next.push_back(s);
        }

        tips.swap(next);
        tick++;
        if (tips.empty() || (int)woods.size() >= params.maxWood) {
            if (!leavesPlaced) buildLeaves();
            finished = true;
            return false;
        }
        return true;
    }

    void startTrunk() {
        woods.clear();
        woodS.clear();
        leaves.clear();
        tips.clear();
        tick = 0;
        finished = false;
        leavesPlaced = false;

        const TreeParams& p = params;
        uint32_t hh = noise::hash2(1, 2, p.seed);
        int th = std::max(4, std::min(28, p.trunkH));

        TreeShoot trunk{};
        trunk.x = p.originXZ();
        trunk.y = 1.5f;
        trunk.z = p.originXZ();
        trunk.vx = (noise::hash01(hh ^ 0x111u) - 0.5f) * p.lean;
        trunk.vy = std::max(0.2f, p.v0);
        trunk.vz = (noise::hash01(hh ^ 0x222u) - 0.5f) * p.lean;
        tree_grow_detail::clampTiltAndScale(trunk.vx, trunk.vy, trunk.vz, p.v0, p.maxTilt);
        trunk.remaining = th;
        trunk.maxLen = th;
        trunk.depth = 0;
        trunk.attract = false;
        trunk.decide = true;
        trunk.forkP = 0.0f;
        trunk.placed = 0;
        trunk.branchIndex = 0;
        trunk.aimSource = -1;
        tips.push_back(trunk);
    }

    void addWoodAt(int x, int y, int z, bool emitS) {
        if (!tree_grow_detail::inBound(params, x, y, z)) return;
        if (tree_canopy::hasWood(woods, x, y, z)) return;
        woods.push_back(IVec3(x, y, z));
        woodS.push_back(emitS ? 1 : 0);
    }
    void addWoodSlice(int ix, int iy, int iz, int depth, int placed) {
        int w = params.sliceWidth(depth, placed);
        int x0 = ix - w / 2;
        int z0 = iz - w / 2;
        for (int dz = 0; dz < w; dz++) {
            for (int dx = 0; dx < w; dx++) {
                int x = x0 + dx, z = z0 + dz;
                bool emitS = (x == ix && z == iz);
                addWoodAt(x, iy, z, emitS);
            }
        }
    }
    // 6-connected walk so a leaning shoot never leaves an edge/corner-only joint.
    void addWoodFacePath(int ax, int ay, int az, int bx, int by, int bz, int depth, int placed) {
        if (ax < -900) {
            addWoodSlice(bx, by, bz, depth, placed);
            return;
        }
        int x = ax, y = ay, z = az;
        for (int guard = 0; guard < 16 && (x != bx || y != by || z != bz); guard++) {
            int dx = bx - x, dy = by - y, dz = bz - z;
            int adx = std::abs(dx), ady = std::abs(dy), adz = std::abs(dz);
            if (adx >= ady && adx >= adz) x += (dx > 0) ? 1 : -1;
            else if (ady >= adz) y += (dy > 0) ? 1 : -1;
            else z += (dz > 0) ? 1 : -1;
            addWoodSlice(x, y, z, depth, placed);
        }
    }

private:
    TreeSource sampleSource(int index, int count) const {
        const TreeParams& p = params;
        uint32_t hh = noise::hash2(1, 2, p.seed);
        int cn = std::max(1, count);
        const float ox = p.originXZ() + p.srcOffX;
        const float oz = p.originXZ() + p.srcOffZ;
        float y0 = p.srcYMin, y1 = p.srcYMax;
        if (y1 < y0) std::swap(y0, y1);
        float r0 = std::max(0.4f, std::min(p.srcRMin, p.srcRMax));
        float r1 = std::max(r0, std::max(p.srcRMin, p.srcRMax));
        float span = std::clamp(p.srcAziSpan, 8.0f, 360.0f) * kDeg2Rad;
        float a0 = p.srcAzi0 * kDeg2Rad;
        uint32_t chh = noise::hash3(index * 3, index * 17, index * 11, hh ^ 0xC20u);
        float jitter = noise::hash01(chh) - 0.5f;
        float t = ((float)index + jitter * 0.32f) / (float)cn;
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        float phi = a0 + t * span;
        float rr = r0 + (r1 - r0) * std::sqrt(noise::hash01(chh ^ 0x9Eu));
        float y = y0 + (y1 - y0) * noise::hash01(chh ^ 0x4Bu);
        if (y < 0.4f) y = 0.4f;
        return { ox + rr * std::cos(phi), y, oz + rr * std::sin(phi), p.sourceS };
    }

    void spawnField() {
        sources.clear();
        int cn = std::max(0, std::min(48, params.sourceN));
        sources.reserve((size_t)cn);
        for (int c = 0; c < cn; c++)
            sources.push_back(sampleSource(c, cn));
    }
};
