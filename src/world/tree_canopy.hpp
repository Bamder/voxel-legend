#pragma once
#include "../core/math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Shade-aware clusters: N rounds of face spawn, occlude check, then outward puffs.
namespace tree_canopy {

struct Params {
    int rounds = 3;          // spawn + occlude rounds
    int tStep = 2;           // look-up shortens by t*(i-1)
    int unsatPct = 40;       // skin blocked ≥ this % → expand
    float topP0 = 0.16f;     // top-face spawn p = p0 + p1 * u²
    float topP1 = 0.64f;
    float sideP0 = 0.05f;    // side-face spawn
    float sideP1 = 0.34f;
    int rMin = 2;
    int rMax = 4;
    int expMin = 1;          // unsatisfied cluster expands this many times
    int expMax = 2;
    float expStep = 2.2f;    // horizontal step of an expansion puff
    int punchDiv = 14;       // hole brushes ≈ nLeaves / punchDiv; 0 = off
    float cinchU = 0.42f;    // bottom cinch below this height fraction; 0 = off
    int trunkKeep = 2;       // top N trunk voxels may grow leaves
};

inline bool same(const IVec3& a, const IVec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

inline bool hasWood(const std::vector<IVec3>& woods, int x, int y, int z) {
    for (const IVec3& w : woods)
        if (w.x == x && w.y == y && w.z == z) return true;
    return false;
}

// A 2x2 trunk slice: this voxel sits in a full 2x2 of wood at the same Y.
inline bool inThickSlice(const std::vector<IVec3>& woods, const IVec3& w) {
    for (int dx = -1; dx <= 0; dx++) {
        for (int dz = -1; dz <= 0; dz++) {
            if (hasWood(woods, w.x + dx, w.y, w.z + dz) &&
                hasWood(woods, w.x + dx + 1, w.y, w.z + dz) &&
                hasWood(woods, w.x + dx, w.y, w.z + dz + 1) &&
                hasWood(woods, w.x + dx + 1, w.y, w.z + dz + 1))
                return true;
        }
    }
    return false;
}

inline bool hasLeaf(const std::vector<IVec3>& leaves, int x, int y, int z) {
    for (const IVec3& L : leaves)
        if (L.x == x && L.y == y && L.z == z) return true;
    return false;
}

inline int dist2xz(const IVec3& w, int tx, int tz) {
    int dx = w.x - tx, dz = w.z - tz;
    return dx * dx + dz * dz;
}

inline bool adj26(const IVec3& a, const IVec3& b) {
    int dx = std::abs(a.x - b.x), dy = std::abs(a.y - b.y), dz = std::abs(a.z - b.z);
    return dx <= 1 && dy <= 1 && dz <= 1 && (dx + dy + dz) > 0;
}

inline int countN6(const std::vector<IVec3>& woods, const IVec3& a) {
    int n = 0;
    for (const IVec3& w : woods) {
        if (same(w, a)) continue;
        if (std::abs(w.x - a.x) + std::abs(w.y - a.y) + std::abs(w.z - a.z) == 1)
            n++;
    }
    return n;
}

inline int countN26(const std::vector<IVec3>& woods, const IVec3& a) {
    int n = 0;
    for (const IVec3& w : woods) {
        if (same(w, a)) continue;
        if (adj26(a, w)) n++;
    }
    return n;
}

inline bool nearWood26(const std::vector<IVec3>& woods, int x, int y, int z) {
    for (const IVec3& w : woods) {
        int dx = std::abs(w.x - x), dy = std::abs(w.y - y), dz = std::abs(w.z - z);
        if (dx <= 1 && dy <= 1 && dz <= 1 && (dx + dy + dz) > 0) return true;
    }
    return false;
}

inline int cheb(const IVec3& a, const IVec3& b) {
    return std::max(std::abs(a.x - b.x), std::max(std::abs(a.y - b.y), std::abs(a.z - b.z)));
}

inline uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed ^ (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h;
}

// Per-tree roll for in-game canopy. Lab sliders stay on the defaults in Params.
inline Params forTree(uint32_t seed) {
    Params p;
    uint32_t h0 = hash3(3, 11, 19, seed ^ 0xA11Eu);
    uint32_t h1 = hash3(7, 23, 29, seed ^ 0xB22Fu);
    uint32_t h2 = hash3(13, 31, 37, seed ^ 0xC33u);
    uint32_t h3 = hash3(17, 41, 43, seed ^ 0xD44u);
    p.rounds = 2 + (int)(h0 % 4u);                         // 2..5
    p.tStep = 1 + (int)(h1 % 4u);                          // 1..4
    p.punchDiv = 10 + (int)(h2 % 15u);                     // 10..24
    p.cinchU = 0.28f + (float)(h3 % 1000u) * 0.00024f;     // 0.28..0.52
    return p;
}

inline int findU(std::vector<int>& p, int i) {
    while (p[(size_t)i] != i) {
        p[(size_t)i] = p[(size_t)p[(size_t)i]];
        i = p[(size_t)i];
    }
    return i;
}

inline void uniteU(std::vector<int>& p, int a, int b) {
    a = findU(p, a);
    b = findU(p, b);
    if (a != b) p[(size_t)a] = b;
}

inline int leafN6(const std::vector<IVec3>& leaves, int x, int y, int z) {
    int n = 0;
    static const int d[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    for (int k = 0; k < 6; k++)
        if (hasLeaf(leaves, x + d[k][0], y + d[k][1], z + d[k][2])) n++;
    return n;
}

template <typename InBound>
void addLeaf(std::vector<IVec3>& leaves, const std::vector<IVec3>& woods,
             InBound inBound, int floorY, int maxY, int x, int y, int z) {
    if (y <= floorY || y > maxY) return;
    if (!inBound(x, y, z)) return;
    if (hasWood(woods, x, y, z)) return;
    if (hasLeaf(leaves, x, y, z)) return;
    leaves.push_back(IVec3(x, y, z));
}

inline IVec3 towardTrunk(const std::vector<IVec3>& woods, const IVec3& a,
                         bool havePrev, const IVec3& prev, int tx, int tz) {
    IVec3 best(99999, 0, 0);
    int bestD = 1 << 30;
    int bestN = -1;
    for (const IVec3& w : woods) {
        if (same(w, a)) continue;
        if (havePrev && same(w, prev)) continue;
        if (!adj26(a, w)) continue;
        int d = dist2xz(w, tx, tz);
        int nn = countN26(woods, w);
        if (d < bestD || (d == bestD && nn > bestN)) {
            best = w;
            bestD = d;
            bestN = nn;
        }
    }
    return best;
}

struct Anchor {
    IVec3 p;
    int R = 2;
    int ox = 0, oz = 0;
};

inline void upsertAnchor(std::vector<Anchor>& a, const Anchor& add) {
    for (Anchor& t : a) {
        if (same(t.p, add.p)) {
            if (add.R > t.R) t = add;
            return;
        }
    }
    a.push_back(add);
}

inline int radiusFor(int s, bool twig, const Params& p) {
    float g = 1.0f - std::exp(-1.15f * (float)std::max(0, s));
    int lo = std::max(1, p.rMin);
    int hi = std::max(lo, p.rMax);
    int R = lo + (int)std::round((float)(hi - lo) * g);
    if (twig && R < lo + 1 && hi > lo) R = lo + 1;
    if (R < lo) R = lo;
    if (R > hi) R = hi;
    return R;
}

// Whole-tree: smaller blobs at the crown top and the lowest twigs.
inline int radiusAtHeight(int R, int y, int minY, int maxY) {
    int span = std::max(1, maxY - minY);
    float t = (float)(y - minY) / (float)span;
    float mid = 1.0f - 4.0f * (t - 0.5f) * (t - 0.5f); // 0 at ends, 1 at mid
    int r = (int)std::round((float)R * (0.72f + 0.28f * mid));
    if (r < 2) r = 2;
    return r;
}

template <typename InBound>
void splatBlob(std::vector<IVec3>& leaves, const std::vector<IVec3>& woods,
               InBound inBound, int floorY, int maxY, const Anchor& a) {
    // Spindle: sit on the wood, fat at mid-height, pinch at top and bottom.
    const float Rx = (float)a.R;
    const float Ry = std::max(1.2f, 0.80f * Rx);
    const float Rz = Rx;
    const float cx = (float)a.p.x + 0.35f * (float)a.ox;
    const float cy = (float)a.p.y + Ry;
    const float cz = (float)a.p.z + 0.35f * (float)a.oz;
    const int h = (int)std::ceil(2.0f * Ry);
    for (int dy = 0; dy <= h; dy++) {
        float u = (h > 0) ? (float)dy / (float)h : 0.5f;
        float mid = 1.0f - 4.0f * (u - 0.5f) * (u - 0.5f);
        float waist = 0.34f + 0.66f * mid;
        float rx = std::max(0.75f, Rx * waist);
        float rz = std::max(0.75f, Rz * waist);
        for (int dz = -a.R; dz <= a.R; dz++) {
            for (int dx = -a.R; dx <= a.R; dx++) {
                float nx = ((float)(a.p.x + dx) - cx) / rx;
                float ny = ((float)(a.p.y + dy) - cy) / Ry;
                float nz = ((float)(a.p.z + dz) - cz) / rz;
                if (nx * nx + ny * ny + nz * nz > 1.0f) continue;
                addLeaf(leaves, woods, inBound, floorY, maxY,
                        a.p.x + dx, a.p.y + dy, a.p.z + dz);
            }
        }
    }
}

inline bool onShell(const std::vector<IVec3>& leaves, const std::vector<IVec3>& woods,
                    int x, int y, int z) {
    static const int d[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    for (int k = 0; k < 6; k++) {
        int nx = x + d[k][0], ny = y + d[k][1], nz = z + d[k][2];
        if (!hasLeaf(leaves, nx, ny, nz) && !hasWood(woods, nx, ny, nz))
            return true;
    }
    return false;
}

inline uint64_t pack3(int x, int y, int z) {
    return ((uint64_t)(uint32_t)x << 42) ^ ((uint64_t)(uint32_t)y << 21) ^ (uint32_t)z;
}

inline bool tooMuchLeafLoss(size_t lost, size_t n0) {
    if (n0 == 0) return true;
    return lost * 5 >= n0 * 2; // ≥ 40%
}

inline void punchEllipsoidBrushes(std::vector<IVec3>& leaves,
                                  const std::vector<IVec3>& woods, uint32_t seed,
                                  int punchDiv) {
    if (punchDiv <= 0 || leaves.size() < 8) return;
    const size_t n0 = leaves.size();
    const size_t maxLose = (n0 * 2 - 1) / 5; // strictly < 40%
    if (maxLose == 0) return;

    std::vector<IVec3> sites;
    sites.reserve(n0);
    for (const IVec3& L : leaves) {
        bool wood = nearWood26(woods, L.x, L.y, L.z);
        bool shell = onShell(leaves, woods, L.x, L.y, L.z);
        if (wood || shell) sites.push_back(L);
    }
    if (sites.empty()) return;

    int nBrush = (int)n0 / punchDiv;
    if (nBrush < 5) nBrush = 5;
    if (nBrush > 22) nBrush = 22;

    struct Brush { float x, y, z, rx, ry, rz; };
    std::vector<Brush> brushes;
    brushes.reserve((size_t)nBrush);
    for (int k = 0; k < nBrush * 4 && (int)brushes.size() < nBrush; k++) {
        uint32_t h = hash3(k * 17, (int)sites.size(), nBrush, seed ^ 0x51u);
        const IVec3& s = sites[(size_t)(h % (uint32_t)sites.size())];
        bool tooClose = false;
        for (const Brush& b : brushes) {
            float dx = (float)s.x - b.x, dy = (float)s.y - b.y, dz = (float)s.z - b.z;
            if (dx * dx + dy * dy + dz * dz < 3.2f) { tooClose = true; break; }
        }
        if (tooClose) continue;
        float rx = 1.15f + (float)((h >> 8) % 5u) * 0.28f;
        float ry = 0.75f + (float)((h >> 12) % 4u) * 0.28f;
        float rz = 1.15f + (float)((h >> 16) % 5u) * 0.28f;
        brushes.push_back({ (float)s.x + 0.5f, (float)s.y + 0.5f, (float)s.z + 0.5f, rx, ry, rz });
    }
    if (brushes.empty()) return;

    std::vector<char> dug(n0, 0);
    std::vector<float> depth(n0, 2.0f);
    for (size_t i = 0; i < n0; i++) {
        const IVec3& L = leaves[i];
        float px = (float)L.x + 0.5f, py = (float)L.y + 0.5f, pz = (float)L.z + 0.5f;
        for (const Brush& b : brushes) {
            float nx = (px - b.x) / b.rx;
            float ny = (py - b.y) / b.ry;
            float nz = (pz - b.z) / b.rz;
            float r2 = nx * nx + ny * ny + nz * nz;
            if (r2 <= 1.0f) {
                dug[i] = 1;
                if (r2 < depth[i]) depth[i] = r2;
            }
        }
    }

    std::unordered_set<uint64_t> woodSet;
    woodSet.reserve(woods.size() * 2 + 8);
    for (const IVec3& w : woods)
        woodSet.insert(pack3(w.x, w.y, w.z));
    auto isWood = [&](int x, int y, int z) {
        return woodSet.find(pack3(x, y, z)) != woodSet.end();
    };
    auto faceWood = [&](int x, int y, int z) {
        static const int d[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
        for (int k = 0; k < 6; k++)
            if (isWood(x + d[k][0], y + d[k][1], z + d[k][2])) return true;
        return false;
    };

    std::unordered_map<uint64_t, size_t> at;
    at.reserve(n0 * 2 + 8);
    for (size_t i = 0; i < n0; i++)
        at[pack3(leaves[i].x, leaves[i].y, leaves[i].z)] = i;

    // Keep a floor under a kept leaf that is not itself anchored to wood.
    for (int pass = 0; pass < 4; pass++) {
        bool changed = false;
        for (size_t i = 0; i < n0; i++) {
            if (!dug[i]) continue;
            const IVec3& L = leaves[i];
            auto it = at.find(pack3(L.x, L.y + 1, L.z));
            if (it == at.end()) continue;
            size_t u = it->second;
            if (dug[u]) continue;
            if (faceWood(leaves[u].x, leaves[u].y, leaves[u].z)) continue;
            if (nearWood26(woods, leaves[u].x, leaves[u].y, leaves[u].z)) continue;
            dug[i] = 0;
            changed = true;
        }
        if (!changed) break;
    }

    auto countDug = [&]() {
        size_t n = 0;
        for (char d : dug) if (d) n++;
        return n;
    };
    if (countDug() > maxLose) {
        std::vector<std::pair<float, size_t>> rank;
        rank.reserve(n0);
        for (size_t i = 0; i < n0; i++)
            if (dug[i]) rank.push_back({ depth[i], i });
        std::sort(rank.begin(), rank.end()); // keep the most interior holes
        for (size_t k = maxLose; k < rank.size(); k++)
            dug[rank[k].second] = 0;
    }

    static const int d6[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

    auto collectKept = [&](std::vector<IVec3>& kept, std::unordered_map<uint64_t, int>& kAt) {
        kept.clear();
        kAt.clear();
        kept.reserve(n0);
        for (size_t i = 0; i < n0; i++) {
            if (dug[i]) continue;
            kAt[pack3(leaves[i].x, leaves[i].y, leaves[i].z)] = (int)kept.size();
            kept.push_back(leaves[i]);
        }
    };

    auto markAnchored = [&](const std::vector<IVec3>& kept,
                            const std::unordered_map<uint64_t, int>& kAt,
                            std::vector<char>& anc) {
        anc.assign(kept.size(), 0);
        std::queue<int> q;
        for (int i = 0; i < (int)kept.size(); i++) {
            if (!faceWood(kept[(size_t)i].x, kept[(size_t)i].y, kept[(size_t)i].z) &&
                !nearWood26(woods, kept[(size_t)i].x, kept[(size_t)i].y, kept[(size_t)i].z))
                continue;
            anc[(size_t)i] = 1;
            q.push(i);
        }
        while (!q.empty()) {
            int i = q.front(); q.pop();
            const IVec3& L = kept[(size_t)i];
            for (int k = 0; k < 6; k++) {
                auto it = kAt.find(pack3(L.x + d6[k][0], L.y + d6[k][1], L.z + d6[k][2]));
                if (it == kAt.end()) continue;
                int j = it->second;
                if (anc[(size_t)j]) continue;
                anc[(size_t)j] = 1;
                q.push(j);
            }
        }
    };

    std::vector<IVec3> kept;
    std::unordered_map<uint64_t, int> kAt;
    std::vector<char> anc;
    bool ok = false;
    for (int iter = 0; iter < 10; iter++) {
        collectKept(kept, kAt);
        markAnchored(kept, kAt, anc);
        size_t nFloat = 0;
        for (char a : anc) if (!a) nFloat++;
        size_t nDug = n0 - kept.size();
        if (nFloat == 0) {
            if (tooMuchLeafLoss(nDug, n0)) return;
            ok = true;
            break;
        }
        size_t lostDrop = nDug + nFloat;
        if (!tooMuchLeafLoss(lostDrop, n0)) {
            std::vector<IVec3> slim;
            slim.reserve(kept.size() - nFloat);
            for (size_t i = 0; i < kept.size(); i++)
                if (anc[i]) slim.push_back(kept[i]);
            kept.swap(slim);
            ok = true;
            break;
        }
        // Restore dug leaves that touch a floater so islands reattach instead of floating.
        std::unordered_set<uint64_t> floatPos;
        for (size_t i = 0; i < kept.size(); i++)
            if (!anc[i]) floatPos.insert(pack3(kept[i].x, kept[i].y, kept[i].z));
        int restored = 0;
        for (size_t i = 0; i < n0; i++) {
            if (!dug[i]) continue;
            const IVec3& L = leaves[i];
            bool touch = false;
            for (int k = 0; k < 6; k++) {
                if (floatPos.count(pack3(L.x + d6[k][0], L.y + d6[k][1], L.z + d6[k][2]))) {
                    touch = true; break;
                }
            }
            if (!touch) continue;
            dug[i] = 0;
            restored++;
        }
        if (restored == 0) return;
    }
    if (!ok) return;
    if (tooMuchLeafLoss(n0 - kept.size(), n0)) return;
    leaves.swap(kept);
}

inline void cinchBottom(std::vector<IVec3>& leaves, const std::vector<IVec3>& woods,
                        float cinchU) {
    if (cinchU <= 0.0f || leaves.empty() || woods.empty()) return;
    int y0 = leaves[0].y, y1 = leaves[0].y;
    for (const IVec3& L : leaves) {
        y0 = std::min(y0, L.y);
        y1 = std::max(y1, L.y);
    }
    int span = std::max(1, y1 - y0);
    size_t w = 0;
    for (size_t i = 0; i < leaves.size(); i++) {
        const IVec3& L = leaves[i];
        float u = (float)(L.y - y0) / (float)span;
        if (u >= cinchU) {
            leaves[w++] = L;
            continue;
        }
        int d2 = 1 << 30;
        for (const IVec3& wd : woods) {
            int dx = L.x - wd.x, dz = L.z - wd.z;
            int t = dx * dx + dz * dz;
            if (t < d2) d2 = t;
        }
        float allow = 1.0f + 6.0f * (u / cinchU);
        if ((float)d2 <= allow * allow) leaves[w++] = L;
    }
    leaves.resize(w);
}

template <typename InBound>
void connectCrowns(std::vector<IVec3>& leaves, const std::vector<IVec3>& woods,
                   InBound inBound, int floorY, int maxY) {
    const int kGap = 8;
    for (int pass = 0; pass < 16; pass++) {
        const int m = (int)leaves.size();
        if (m < 2) return;
        std::vector<int> p((size_t)m);
        for (int i = 0; i < m; i++) p[(size_t)i] = i;
        for (int i = 0; i < m; i++) {
            for (int j = i + 1; j < m; j++) {
                int ad = std::abs(leaves[(size_t)i].x - leaves[(size_t)j].x)
                       + std::abs(leaves[(size_t)i].y - leaves[(size_t)j].y)
                       + std::abs(leaves[(size_t)i].z - leaves[(size_t)j].z);
                if (ad == 1) uniteU(p, i, j);
            }
        }
        int best = kGap + 1, ia = -1, ib = -1;
        for (int i = 0; i < m; i++) {
            int ci = findU(p, i);
            for (int j = i + 1; j < m; j++) {
                if (findU(p, j) == ci) continue;
                int d = cheb(leaves[(size_t)i], leaves[(size_t)j]);
                if (std::abs(leaves[(size_t)i].y - leaves[(size_t)j].y) > 4) continue;
                if (d < best) { best = d; ia = i; ib = j; }
            }
        }
        if (ia < 0 || best > kGap) return;

        IVec3 a = leaves[(size_t)ia], b = leaves[(size_t)ib];
        int nBlob = std::max(1, (best + 1) / 3);
        if (nBlob > 3) nBlob = 3;
        for (int k = 1; k <= nBlob; k++) {
            float t = (float)k / (float)(nBlob + 1);
            int x = (int)std::round((1.0f - t) * (float)a.x + t * (float)b.x);
            int y = (int)std::round((1.0f - t) * (float)a.y + t * (float)b.y);
            int z = (int)std::round((1.0f - t) * (float)a.z + t * (float)b.z);
            int ox = (b.x > a.x) - (b.x < a.x);
            int oz = (b.z > a.z) - (b.z < a.z);
            int R = 2 + (best >= 5 ? 1 : 0);
            if (R > 3) R = 3;
            y = std::min(y, std::min(a.y, b.y));
            splatBlob(leaves, woods, inBound, floorY, maxY, Anchor{ IVec3(x, y, z), R, ox, oz });
        }
        if ((int)leaves.size() <= m) return;
    }
}

template <typename InBound>
void smoothRim(std::vector<IVec3>& leaves, const std::vector<IVec3>& woods,
               InBound inBound, int floorY, int maxY) {
    auto add = [&](int x, int y, int z) {
        if (nearWood26(woods, x, y, z)) return;
        addLeaf(leaves, woods, inBound, floorY, maxY, x, y, z);
    };
    for (int pass = 0; pass < 1; pass++) {
        std::vector<IVec3> fill;
        for (const IVec3& L : leaves) {
            const int nb[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
            for (int k = 0; k < 6; k++) {
                int x = L.x + nb[k][0], y = L.y + nb[k][1], z = L.z + nb[k][2];
                if (hasWood(woods, x, y, z) || hasLeaf(leaves, x, y, z)) continue;
                if (nearWood26(woods, x, y, z)) continue;
                int n6 = leafN6(leaves, x, y, z);
                if (n6 >= 5) fill.push_back(IVec3(x, y, z));
            }
        }
        for (const IVec3& c : fill)
            add(c.x, c.y, c.z);

        size_t w = 0;
        for (size_t i = 0; i < leaves.size(); i++) {
            const IVec3& L = leaves[i];
            if (leafN6(leaves, L.x, L.y, L.z) <= 1) continue;
            leaves[w++] = L;
        }
        leaves.resize(w);
    }
}

inline bool blockedAbove(const std::vector<IVec3>& woods, const std::vector<IVec3>& leaves,
                         int x, int y, int z, int nUp) {
    if (nUp <= 0) return false;
    for (int dy = 1; dy <= nUp; dy++) {
        int yy = y + dy;
        if (hasWood(woods, x, yy, z) || hasLeaf(leaves, x, yy, z)) return true;
    }
    return false;
}

inline bool inCluster(const std::vector<IVec3>& c, int x, int y, int z) {
    for (const IVec3& L : c)
        if (L.x == x && L.y == y && L.z == z) return true;
    return false;
}

inline float occludeFrac(const std::vector<IVec3>& cluster,
                         const std::vector<IVec3>& woods, const std::vector<IVec3>& leaves,
                         int treeH, int roundI, int tStep) {
    int skin = 0, hit = 0;
    for (const IVec3& L : cluster) {
        if (inCluster(cluster, L.x, L.y + 1, L.z)) continue;
        skin++;
        int nUp = (treeH - L.y) - tStep * (roundI - 1);
        if (blockedAbove(woods, leaves, L.x, L.y, L.z, nUp)) hit++;
    }
    if (skin <= 0) return 0.0f;
    return (float)hit / (float)skin;
}

inline void outwardXZ(const std::vector<IVec3>& woods, const std::vector<IVec3>& leaves,
                      int tx, int tz, float cx, float cz, uint32_t h, float& ox, float& oz) {
    float fx = 0.0f, fz = 0.0f;
    auto acc = [&](int x, int z) {
        float dx = cx - ((float)x + 0.5f);
        float dz = cz - ((float)z + 0.5f);
        float r2 = dx * dx + dz * dz;
        if (r2 < 1.0f) r2 = 1.0f;
        fx += dx / r2;
        fz += dz / r2;
    };
    for (const IVec3& w : woods) acc(w.x, w.z);
    for (const IVec3& L : leaves) acc(L.x, L.z);
    acc(tx, tz);
    float ang = ((float)(h % 71u) - 35.0f) * 0.017f;
    float c = std::cos(ang), s = std::sin(ang);
    float rx = fx * c - fz * s;
    float rz = fx * s + fz * c;
    float len = std::sqrt(rx * rx + rz * rz);
    if (len < 1e-4f) {
        rx = cx - ((float)tx + 0.5f);
        rz = cz - ((float)tz + 0.5f);
        len = std::sqrt(rx * rx + rz * rz);
    }
    if (len < 1e-4f) { ox = 1.0f; oz = 0.0f; return; }
    ox = rx / len;
    oz = rz / len;
}

template <typename InBound>
void build(const std::vector<IVec3>& woods, std::vector<IVec3>& leaves,
           int tx, int tz, int floorY, int maxY, uint32_t seed, InBound inBound,
           const Params& p = Params{}) {
    leaves.clear();
    if (woods.empty()) return;

    int minY = woods[0].y, treeH = woods[0].y;
    for (const IVec3& w : woods) {
        minY = std::min(minY, w.y);
        treeH = std::max(treeH, w.y);
    }
    const int ySpan = std::max(1, treeH - minY);
    const int kRounds = std::max(1, std::min(8, p.rounds));
    const int kT = std::max(0, p.tStep);
    const float kUnsat = std::clamp((float)p.unsatPct / 100.0f, 0.0f, 1.0f);
    int expLo = std::max(0, std::min(p.expMin, p.expMax));
    int expHi = std::max(0, std::max(p.expMin, p.expMax));
    const int fd[5][3] = { { 0, 1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

    auto faceCovered = [&](const IVec3& w, int fi) {
        return hasLeaf(leaves, w.x + fd[fi][0], w.y + fd[fi][1], w.z + fd[fi][2]);
    };

    auto plant = [&](int x, int y, int z, int R, int ox, int oz) -> std::vector<IVec3> {
        size_t before = leaves.size();
        splatBlob(leaves, woods, inBound, floorY, maxY, Anchor{ IVec3(x, y, z), R, ox, oz });
        std::vector<IVec3> cl;
        cl.reserve(leaves.size() - before);
        for (size_t i = before; i < leaves.size(); i++)
            cl.push_back(leaves[i]);
        return cl;
    };

    for (int round = 1; round <= kRounds; round++) {
        std::vector<std::vector<IVec3>> born;
        for (const IVec3& w : woods) {
            bool trunkCol = (w.x == tx && w.z == tz) || inThickSlice(woods, w);
            if (trunkCol) {
                if (p.trunkKeep <= 0 || w.y < treeH - (p.trunkKeep - 1)) continue;
            }
            float u = (float)(w.y - minY) / (float)ySpan;
            float u2 = u * u;
            for (int fi = 0; fi < 5; fi++) {
                if (hasWood(woods, w.x + fd[fi][0], w.y + fd[fi][1], w.z + fd[fi][2])) continue;
                if (faceCovered(w, fi)) continue;
                float pFace = (fi == 0)
                    ? (p.topP0 + p.topP1 * u2)
                    : (p.sideP0 + p.sideP1 * u2);
                uint32_t h = hash3(w.x, w.y + fi * 13, w.z, seed ^ (uint32_t)(round * 9176));
                if ((h % 1000u) / 1000.0f >= pFace) continue;
                int ox = fd[fi][0], oz = fd[fi][2];
                if (ox == 0 && oz == 0) {
                    ox = (w.x > tx) - (w.x < tx);
                    oz = (w.z > tz) - (w.z < tz);
                }
                bool nearTop = (treeH - w.y) <= 3;
                int R = radiusAtHeight(radiusFor(std::max(0, treeH - w.y), nearTop, p),
                                       w.y, minY, treeH);
                auto cl = plant(w.x, w.y, w.z, R, ox, oz);
                if (!cl.empty()) born.push_back(std::move(cl));
            }
        }

        for (const auto& cl : born) {
            float frac = occludeFrac(cl, woods, leaves, treeH, round, kT);
            if (frac < kUnsat) continue;
            float cx = 0, cy = 0, cz = 0;
            for (const IVec3& L : cl) {
                cx += (float)L.x; cy += (float)L.y; cz += (float)L.z;
            }
            float inv = 1.0f / (float)cl.size();
            cx *= inv; cy *= inv; cz *= inv;
            uint32_t h = hash3((int)cx, (int)cy, (int)cz, seed ^ (uint32_t)(round * 3331));
            int span = std::max(0, expHi - expLo);
            int nExp = expLo + (span > 0 ? (int)(h % (uint32_t)(span + 1)) : 0);
            for (int e = 0; e < nExp; e++) {
                uint32_t he = hash3((int)cx + e * 9, (int)cz, round, h);
                float ox, oz;
                outwardXZ(woods, leaves, tx, tz, cx, cz, he, ox, oz);
                float step = std::max(0.5f, p.expStep) + (float)(he % 3u);
                int nx = (int)std::round(cx + ox * step);
                int nz = (int)std::round(cz + oz * step);
                int ny = std::max(minY + 1, (int)std::round(cy) - e);
                int iox = (ox > 0.25f) - (ox < -0.25f);
                int ioz = (oz > 0.25f) - (oz < -0.25f);
                int midR = std::max(p.rMin, (p.rMin + p.rMax) / 2);
                int R = radiusAtHeight(midR, ny, minY, treeH);
                plant(nx, ny, nz, R, iox, ioz);
            }
        }
    }

    cinchBottom(leaves, woods, p.cinchU);
    punchEllipsoidBrushes(leaves, woods, seed, p.punchDiv);
    smoothRim(leaves, woods, inBound, floorY, maxY);
}

} // namespace tree_canopy
