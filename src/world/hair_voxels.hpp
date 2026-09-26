#pragma once
#include "player_model.hpp"
#include "player_skin.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pm {

inline constexpr float kHairGrid = 0.04f;
inline constexpr float kHairCard = kHairGrid * 0.5f; // 0.02 square, 2x2 per voxel face
inline constexpr float kHairCardThin = 0.0015f;
inline constexpr int kHairVoxelMax = 400;
inline constexpr int kHairCardMax = 1600;

struct HairCell {
    int x = 0, y = 0, z = 0;
    bool operator<(const HairCell& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
    bool operator==(const HairCell& o) const { return x == o.x && y == o.y && z == o.z; }
};

inline HairCell worldToHairCell(const Vec3& p) {
    return {
        (int)std::floor(p.x / kHairGrid),
        (int)std::floor(p.y / kHairGrid),
        (int)std::floor(p.z / kHairGrid)
    };
}

inline int hairDomAxis(const Vec3& n) {
    float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ay >= ax && ay >= az) return 1;
    if (az >= ax && az >= ay) return 2;
    return 0;
}

inline float hairComp(const Vec3& v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

// Voxel fully outside the hit surface (avoids sinking into the body).
inline HairCell hairCellOutside(const Vec3& hit, const Vec3& n) {
    HairCell c = worldToHairCell(hit);
    int a = hairDomAxis(n);
    float h = hairComp(hit, a);
    float s = hairComp(n, a);
    const float e = 1e-4f;
    int idx = (s >= 0.0f)
        ? (int)std::ceil(h / kHairGrid - e)
        : (int)std::floor(h / kHairGrid + e) - 1;
    if (a == 0) c.x = idx;
    else if (a == 1) c.y = idx;
    else c.z = idx;
    return c;
}

// Voxel just inside the hit surface (the cell being painted over / erased).
inline HairCell hairCellInside(const Vec3& hit, const Vec3& n) {
    HairCell c = worldToHairCell(hit);
    int a = hairDomAxis(n);
    float h = hairComp(hit, a);
    float s = hairComp(n, a);
    const float e = 1e-4f;
    int idx = (s >= 0.0f)
        ? (int)std::floor((h - e) / kHairGrid)
        : (int)std::floor((h + e) / kHairGrid);
    if (a == 0) c.x = idx;
    else if (a == 1) c.y = idx;
    else c.z = idx;
    return c;
}

inline void hairSet(Vec3& v, int a, float x) {
    if (a == 0) v.x = x;
    else if (a == 1) v.y = x;
    else v.z = x;
}

// Exact AABB face the voxel should rest on (unrotated host), else the hit point.
inline float hairHostPlane(const Part& host, const Vec3& hit, const Vec3& n) {
    int a = hairDomAxis(n);
    float s = hairComp(n, a);
    if (host.rot.lengthSq() < 1e-6f)
        return hairComp(host.center, a) + ((s >= 0.0f) ? 1.0f : -1.0f) * hairComp(host.half, a);
    return hairComp(hit, a);
}

struct HairAtom {
    HairCell id{};
    Vec3 mn{};
    Vec3 mx{};
};

inline HairCell hairIdFromAabb(const Vec3& mn, const Vec3& mx) {
    (void)mx;
    const float e = 1e-4f;
    return {
        (int)std::floor(mn.x / kHairGrid + e),
        (int)std::floor(mn.y / kHairGrid + e),
        (int)std::floor(mn.z / kHairGrid + e)
    };
}

inline HairCell hairIdFromCenter(const Vec3& c) { return worldToHairCell(c); }

// One G cube: tangent grid starts at the host AABB min so model edges line up.
// A hit on the far edge snaps a full G cell flush to that edge (size stays 0.04).
inline HairAtom hairAtomOnSurface(const Part& host, const Vec3& hit, const Vec3& n, float G = kHairGrid) {
    const float lift = 1.5e-4f;
    int a = hairDomAxis(n);
    float s = hairComp(n, a);
    float plane = hairHostPlane(host, hit, n);

    auto snapTan = [&](int ax, float hitC, float& lo, float& hi) {
        if (host.rot.lengthSq() > 1e-6f) {
            int i = (int)std::floor(hitC / G);
            lo = (float)i * G;
            hi = lo + G;
            return;
        }
        float umin = hairComp(host.center, ax) - hairComp(host.half, ax);
        float umax = hairComp(host.center, ax) + hairComp(host.half, ax);
        int i = (int)std::floor((hitC - umin) / G + 1e-6f);
        if (i < 0) i = 0;
        lo = umin + (float)i * G;
        hi = lo + G;
        float dMin = hitC - umin, dMax = umax - hitC;
        const float edge = G * 0.5f + 1e-4f;
        // Keep 0.04; if the stroke is on a silhouette, sit that face on the host edge.
        if (dMax <= edge && dMax <= dMin) {
            hi = umax;
            lo = umax - G;
        } else if (dMin <= edge) {
            lo = umin;
            hi = umin + G;
        } else if (hi > umax + 1e-4f) {
            hi = umax;
            lo = umax - G;
        } else if (lo < umin - 1e-4f) {
            lo = umin;
            hi = umin + G;
        }
    };

    float x0, x1, y0, y1, z0, z1;
    snapTan(0, hit.x, x0, x1);
    snapTan(1, hit.y, y0, y1);
    snapTan(2, hit.z, z0, z1);
    Vec3 mn{ x0, y0, z0 };
    Vec3 mx{ x1, y1, z1 };
    if (s >= 0.0f) {
        hairSet(mn, a, plane + lift);
        hairSet(mx, a, plane + lift + G);
    } else {
        hairSet(mx, a, plane - lift);
        hairSet(mn, a, plane - lift - G);
    }
    HairAtom atom;
    atom.mn = mn;
    atom.mx = mx;
    atom.id = {
        (int)std::floor(mn.x / G + 1e-4f),
        (int)std::floor(mn.y / G + 1e-4f),
        (int)std::floor(mn.z / G + 1e-4f)
    };
    return atom;
}

inline void hairShiftAtom(HairAtom& a, int axis, float s, float dist) {
    if (s >= 0.0f) {
        hairSet(a.mn, axis, hairComp(a.mn, axis) + dist);
        hairSet(a.mx, axis, hairComp(a.mx, axis) + dist);
    } else {
        hairSet(a.mn, axis, hairComp(a.mn, axis) - dist);
        hairSet(a.mx, axis, hairComp(a.mx, axis) - dist);
    }
    float G = dist > 1e-8f ? dist : kHairGrid;
    a.id = {
        (int)std::floor(a.mn.x / G + 1e-4f),
        (int)std::floor(a.mn.y / G + 1e-4f),
        (int)std::floor(a.mn.z / G + 1e-4f)
    };
}

inline void hairForcePlane(HairAtom& a, int axis, float s, float plane, float G = kHairGrid) {
    if (s >= 0.0f) {
        hairSet(a.mn, axis, plane);
        hairSet(a.mx, axis, plane + G);
    } else {
        hairSet(a.mx, axis, plane);
        hairSet(a.mn, axis, plane - G);
    }
    a.id = {
        (int)std::floor(a.mn.x / G + 1e-4f),
        (int)std::floor(a.mn.y / G + 1e-4f),
        (int)std::floor(a.mn.z / G + 1e-4f)
    };
}

inline bool hairColorSame(const Vec3& a, const Vec3& b) {
    return std::fabs(a.x - b.x) < 0.04f && std::fabs(a.y - b.y) < 0.04f && std::fabs(a.z - b.z) < 0.04f;
}

inline bool hairTexCompat(const std::string& a, const std::string& b) {
    return a.empty() || b.empty() || a == b;
}

// Unrotated hair cuboids participate in paint/erase merge, including stretched
// (non-0.04) and L/T clumps. Rotated hair stays a free-form part.
// One grid thick, and only the faces that define the garment:
//   torso / legs: front, back, left, right (open at top and bottom)
//   sleeve: the same four faces; the cuff is the open end, not the side
//   shoe: those four sides plus the sole (open at the ankle)
//   neck is not its own ring; the collar is the open top of the torso
inline void shellClothParts(std::vector<Part>& parts) {
    const float g = kHairGrid;
    const float eps = 1e-4f;
    struct Seg {
        Vec3 mn{ 1e9f, 1e9f, 1e9f }, mx{ -1e9f, -1e9f, -1e9f };
        Part tmpl{};
        std::vector<Part> srcs;
        bool any = false;
    };
    std::vector<Part> passthrough;
    std::map<std::string, Seg> segs;
    for (const Part& src : parts) {
        if (src.kind != "cloth" || src.rot.lengthSq() > 1e-6f) {
            passthrough.push_back(src);
            continue;
        }
        if (src.name.find("neck") != std::string::npos) continue;
        std::string key = src.name.empty() ? ("_" + std::to_string((int)segs.size())) : src.name;
        if (src.name.empty()) key = "__anon_" + std::to_string((int)passthrough.size() + (int)segs.size());
        else if (key.find("arm_l") != std::string::npos) key = "arm_l";
        else if (key.find("arm_r") != std::string::npos) key = "arm_r";
        Seg& s = segs[key];
        Vec3 a = src.center - src.half, b = src.center + src.half;
        if (!s.any) { s.tmpl = src; s.any = true; }
        s.srcs.push_back(src);
        s.mn.x = std::min(s.mn.x, a.x); s.mn.y = std::min(s.mn.y, a.y); s.mn.z = std::min(s.mn.z, a.z);
        s.mx.x = std::max(s.mx.x, b.x); s.mx.y = std::max(s.mx.y, b.y); s.mx.z = std::max(s.mx.z, b.z);
    }
    std::vector<Part> out = std::move(passthrough);
    out.reserve(out.size() + segs.size() * 5);
    for (auto& kv : segs) {
        const std::string& key = kv.first;
        Seg& s = kv.second;
        float sx = s.mx.x - s.mn.x, sy = s.mx.y - s.mn.y, sz = s.mx.z - s.mn.z;
        if (sx <= eps || sy <= eps || sz <= eps) continue;
        // Part-tool blocks are named cloth / cloth_N and must stay the size that was saved.
        // Any block already at or under one paint cell on an axis is kept too: rebuilding
        // it as a paint-cell wall would inflate that axis up to the paint grid.
        bool authored = key.rfind("cloth", 0) == 0;
        if (authored || sx <= g + eps || sy <= g + eps || sz <= g + eps) {
            for (Part& p : s.srcs) out.push_back(std::move(p));
            continue;
        }
        float gx = std::min(g, sx);
        float gy = std::min(g, sy);
        float gz = std::min(g, sz);
        bool arm = key.find("arm") != std::string::npos;
        bool foot = key.find("foot") != std::string::npos;
        auto emit = [&](const Vec3& a, const Vec3& b) {
            if (b.x - a.x <= eps || b.y - a.y <= eps || b.z - a.z <= eps) return;
            Part p = s.tmpl;
            p.center = (a + b) * 0.5f;
            p.half = (b - a) * 0.5f;
            p.rot = {};
            p.boxX = p.boxY = -1;
            p.boxW = p.boxH = p.boxD = 0;
            p.bind = -1;
            if (p.name.empty()) p.name = key;
            out.push_back(std::move(p));
        };
        auto wallsLR = [&]() {
            emit({ s.mn.x, s.mn.y, s.mn.z }, { s.mn.x + gx, s.mx.y, s.mx.z });
            emit({ s.mx.x - gx, s.mn.y, s.mn.z }, { s.mx.x, s.mx.y, s.mx.z });
        };
        auto wallsFB = [&]() {
            emit({ s.mn.x, s.mn.y, s.mn.z }, { s.mx.x, s.mx.y, s.mn.z + gz });
            emit({ s.mn.x, s.mn.y, s.mx.z - gz }, { s.mx.x, s.mx.y, s.mx.z });
        };
        if (arm) {
            wallsLR();
            wallsFB();
        } else if (foot) {
            wallsLR();
            wallsFB();
            emit({ s.mn.x, s.mn.y, s.mn.z }, { s.mx.x, s.mn.y + gy, s.mx.z });
        } else {
            wallsLR();
            wallsFB();
        }
    }
    parts.swap(out);
}

// Remove the overlap of box [cmn,cmx] from one unrotated cuboid. Remaining volume
// is up to six slabs. Shared faces between slabs are hidden at draw time.
inline bool subtractPartBox(std::vector<Part>& parts, int index, const Vec3& cmn, const Vec3& cmx) {
    if (index < 0 || index >= (int)parts.size()) return false;
    Part src = parts[index];
    if (src.rot.lengthSq() > 1e-6f) return false;
    if (isDecalPart(src) || isHairCardPart(src)) return false;
    Vec3 mn = src.center - src.half, mx = src.center + src.half;
    Vec3 imn{ std::max(mn.x, cmn.x), std::max(mn.y, cmn.y), std::max(mn.z, cmn.z) };
    Vec3 imx{ std::min(mx.x, cmx.x), std::min(mx.y, cmx.y), std::min(mx.z, cmx.z) };
    const float eps = 1e-4f;
    if (imx.x - imn.x <= eps || imx.y - imn.y <= eps || imx.z - imn.z <= eps) return false;
    parts.erase(parts.begin() + index);
    auto emit = [&](const Vec3& a, const Vec3& b) {
        if (b.x - a.x <= eps || b.y - a.y <= eps || b.z - a.z <= eps) return;
        Part p = src;
        p.center = (a + b) * 0.5f;
        p.half = (b - a) * 0.5f;
        p.rot = {};
        p.boxX = p.boxY = -1;
        p.boxW = p.boxH = p.boxD = 0;
        p.bind = -1;
        parts.push_back(std::move(p));
    };
    emit({ mn.x, mn.y, mn.z }, { imn.x, mx.y, mx.z });
    emit({ imx.x, mn.y, mn.z }, { mx.x, mx.y, mx.z });
    emit({ imn.x, mn.y, mn.z }, { imx.x, imn.y, mx.z });
    emit({ imn.x, imx.y, mn.z }, { imx.x, mx.y, mx.z });
    emit({ imn.x, imn.y, mn.z }, { imx.x, imx.y, imn.z });
    emit({ imn.x, imn.y, imx.z }, { imx.x, imx.y, mx.z });
    return true;
}

inline bool isGridHairPart(const Part& p) {
    bool brush = isHairPart(p) || p.kind == "cloth";
    if (!brush || isDecalPart(p) || isHairCardPart(p)) return false;
    if (p.rot.lengthSq() > 1e-6f) return false;
    return p.half.x > 1e-4f && p.half.y > 1e-4f && p.half.z > 1e-4f;
}

struct HairCardSlot {
    int axis = 0;
    float sign = 1.0f;
    int iu = 0, iv = 0;
    Vec3 center{};
    Vec3 half{};
};

inline int hairCardCount(const std::vector<Part>& parts) {
    int n = 0;
    for (const Part& p : parts) if (isHairCardPart(p)) n++;
    return n;
}

// Snap a 0.02 square onto the hit cuboid face (host-local, 2x2 per 0.04 cell).
inline bool hairCardSlotOnHost(const Part& host, const Vec3& hit, const Vec3& n, HairCardSlot& out) {
    const float C = kHairCard;
    Vec3 nL = unrotateEuler(n, host.rot);
    if (nL.lengthSq() > 1e-12f) nL = nL.normalized();
    else nL = n;
    int axis = hairDomAxis(nL);
    float s = hairComp(nL, axis);
    if (std::fabs(s) < 1e-6f) return false;
    float sgn = (s >= 0.0f) ? 1.0f : -1.0f;
    int ua = (axis + 1) % 3, va = (axis + 2) % 3;
    float halfU = hairComp(host.half, ua);
    float halfV = hairComp(host.half, va);
    if (halfU < C * 0.49f || halfV < C * 0.49f) return false;
    int nu = (int)std::floor(halfU * 2.0f / C + 1e-4f);
    int nv = (int)std::floor(halfV * 2.0f / C + 1e-4f);
    if (nu < 1 || nv < 1) return false;
    Vec3 h = partLocalOffset(host, hit);
    float umin = -halfU, vmin = -halfV;
    int iu = (int)std::floor((hairComp(h, ua) - umin) / C + 1e-5f);
    int iv = (int)std::floor((hairComp(h, va) - vmin) / C + 1e-5f);
    if (iu < 0) iu = 0;
    if (iv < 0) iv = 0;
    if (iu >= nu) iu = nu - 1;
    if (iv >= nv) iv = nv - 1;
    float ulo = umin + (float)iu * C;
    float vlo = vmin + (float)iv * C;
    Vec3 local{};
    hairSet(local, ua, ulo + C * 0.5f);
    hairSet(local, va, vlo + C * 0.5f);
    hairSet(local, axis, sgn * (hairComp(host.half, axis) + kHairCardThin));
    out.axis = axis;
    out.sign = sgn;
    out.iu = iu;
    out.iv = iv;
    out.center = partWorldOffset(host, local);
    out.half = {};
    hairSet(out.half, ua, C * 0.5f);
    hairSet(out.half, va, C * 0.5f);
    hairSet(out.half, axis, kHairCardThin);
    return true;
}

inline int findHairCard(const std::vector<Part>& parts, const HairCardSlot& sl) {
    const float eps = kHairCard * 0.35f;
    float eps2 = eps * eps;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isHairCardPart(parts[i])) continue;
        Vec3 d = parts[i].center - sl.center;
        if (d.lengthSq() < eps2) return i;
    }
    return -1;
}

inline Part makeHairCard(const Part& host, const HairCardSlot& sl, const Vec3& color,
                         const std::string& tex, const std::string& name) {
    Part np;
    np.center = sl.center;
    np.half = sl.half;
    np.color = color;
    if (host.type == 1 || host.type == 2 || host.type == 3) {
        np.type = host.type;
        np.side = host.side;
    } else {
        np.type = 0;
        np.side = 0;
    }
    np.kind = "haircard";
    np.name = name;
    np.tex = tex;
    np.rot = host.rot;
    return np;
}

inline bool hairMergeCompat(const Part& p, const Part& tmpl) {
    if (!isGridHairPart(p)) return false;
    if (!hairColorSame(p.color, tmpl.color)) return false;
    if (!hairTexCompat(p.tex, tmpl.tex)) return false;
    return p.type == tmpl.type;
}

struct HairRect { float u0 = 0, v0 = 0, u1 = 0, v1 = 0; };

inline void hairRectSub(std::vector<HairRect>& rs, const HairRect& cut) {
    std::vector<HairRect> next;
    next.reserve(rs.size() * 4);
    for (const HairRect& r : rs) {
        float x0 = std::max(r.u0, cut.u0), y0 = std::max(r.v0, cut.v0);
        float x1 = std::min(r.u1, cut.u1), y1 = std::min(r.v1, cut.v1);
        if (x0 >= x1 - 1e-6f || y0 >= y1 - 1e-6f) { next.push_back(r); continue; }
        if (r.v0 < y0 - 1e-6f) next.push_back({ r.u0, r.v0, r.u1, y0 });
        if (y1 < r.v1 - 1e-6f) next.push_back({ r.u0, y1, r.u1, r.v1 });
        if (r.u0 < x0 - 1e-6f) next.push_back({ r.u0, y0, x0, y1 });
        if (x1 < r.u1 - 1e-6f) next.push_back({ x1, y0, r.u1, y1 });
    }
    rs.swap(next);
}

// Remaining pieces of `face` after subtracting coplanar overlaps with neighbors.
inline void hairFaceRemainders(const Part& p, int face, const std::vector<Part>& parts, std::vector<HairRect>& out) {
    out.clear();
    int axis = face / 2;
    int ua = (axis + 1) % 3, va = (axis + 2) % 3;
    bool pos = (face % 2) == 0;
    Vec3 pmn = p.center - p.half, pmx = p.center + p.half;
    float u0 = hairComp(pmn, ua), u1 = hairComp(pmx, ua);
    float v0 = hairComp(pmn, va), v1 = hairComp(pmx, va);
    out.push_back({ u0, v0, u1, v1 });
    if (!isGridHairPart(p)) return;
    float plane = pos ? hairComp(pmx, axis) : hairComp(pmn, axis);
    const float eps = 3.0e-3f;
    for (const Part& q : parts) {
        if (&q == &p || !isGridHairPart(q)) continue;
        if (q.type != p.type || !hairTexCompat(q.tex, p.tex) || !hairColorSame(q.color, p.color)) continue;
        Vec3 qmn = q.center - q.half, qmx = q.center + q.half;
        float qplane = pos ? hairComp(qmn, axis) : hairComp(qmx, axis);
        if (std::fabs(qplane - plane) > eps) continue;
        float cu0 = std::max(u0, hairComp(qmn, ua)), cu1 = std::min(u1, hairComp(qmx, ua));
        float cv0 = std::max(v0, hairComp(qmn, va)), cv1 = std::min(v1, hairComp(qmx, va));
        if (cu1 - cu0 > 1e-5f && cv1 - cv0 > 1e-5f)
            hairRectSub(out, { cu0, cv0, cu1, cv1 });
    }
}

inline void hairFaceCorners(const Part& p, int face, const HairRect& r, Vec3 q[4]) {
    Vec3 pmn = p.center - p.half, pmx = p.center + p.half;
    float x0 = pmn.x, x1 = pmx.x, y0 = pmn.y, y1 = pmx.y, z0 = pmn.z, z1 = pmx.z;
    switch (face) {
        case 0: q[0] = { x1, r.u0, r.v1 }; q[1] = { x1, r.u0, r.v0 }; q[2] = { x1, r.u1, r.v0 }; q[3] = { x1, r.u1, r.v1 }; break;
        case 1: q[0] = { x0, r.u0, r.v0 }; q[1] = { x0, r.u0, r.v1 }; q[2] = { x0, r.u1, r.v1 }; q[3] = { x0, r.u1, r.v0 }; break;
        case 2: q[0] = { r.v0, y1, r.u0 }; q[1] = { r.v1, y1, r.u0 }; q[2] = { r.v1, y1, r.u1 }; q[3] = { r.v0, y1, r.u1 }; break;
        case 3: q[0] = { r.v0, y0, r.u1 }; q[1] = { r.v1, y0, r.u1 }; q[2] = { r.v1, y0, r.u0 }; q[3] = { r.v0, y0, r.u0 }; break;
        case 4: q[0] = { r.u0, r.v0, z1 }; q[1] = { r.u1, r.v0, z1 }; q[2] = { r.u1, r.v1, z1 }; q[3] = { r.u0, r.v1, z1 }; break;
        default:q[0] = { r.u1, r.v0, z0 }; q[1] = { r.u0, r.v0, z0 }; q[2] = { r.u0, r.v1, z0 }; q[3] = { r.u1, r.v1, z0 }; break;
    }
    for (int i = 0; i < 4; i++) q[i] = partWorldFromUnrot(p, q[i]);
}

inline void hairSub1D(std::vector<std::pair<float, float>>& segs, float a, float b, float eps) {
    if (b < a) { float t = a; a = b; b = t; }
    a -= eps;
    b += eps;
    std::vector<std::pair<float, float>> next;
    next.reserve(segs.size() + 1);
    for (const auto& s : segs) {
        float lo = s.first, hi = s.second;
        if (b <= lo + eps || a >= hi - eps) { next.push_back(s); continue; }
        if (lo < a - eps) next.push_back({ lo, a });
        if (hi > b + eps) next.push_back({ b, hi });
    }
    segs.swap(next);
}

struct HairOutlineSeg { Vec3 a{}, b{}; };

// Outer wire of a bound clump. All faces on the same axis-aligned plane (both
// windings) are clipped together so internal walls and coplanar seams vanish.
inline void hairClumpOutlines(const std::vector<Part>& parts, int bind,
                              std::vector<HairOutlineSeg>& out) {
    if (bind < 0) return;
    const float eps = 4.0e-3f;
    const float minLen = 3.0e-3f;
    for (int axis = 0; axis < 3; axis++) {
        int ua = (axis + 1) % 3, va = (axis + 2) % 3;
        struct Bucket { float plane = 0; std::vector<HairRect> rs; };
        std::vector<Bucket> buckets;
        auto addRect = [&](float plane, const HairRect& r) {
            if (r.u1 - r.u0 < 1e-5f || r.v1 - r.v0 < 1e-5f) return;
            for (Bucket& b : buckets) {
                if (std::fabs(b.plane - plane) <= eps) {
                    b.rs.push_back(r);
                    b.plane = (b.plane * (float)(b.rs.size() - 1) + plane) / (float)b.rs.size();
                    return;
                }
            }
            buckets.push_back({ plane, { r } });
        };
        for (const Part& p : parts) {
            if (!isGridHairPart(p) || p.bind != bind) continue;
            Vec3 pmn = p.center - p.half, pmx = p.center + p.half;
            HairRect r{
                hairComp(pmn, ua), hairComp(pmn, va),
                hairComp(pmx, ua), hairComp(pmx, va)
            };
            addRect(hairComp(pmn, axis), r);
            addRect(hairComp(pmx, axis), r);
        }
        for (const Bucket& bkt : buckets) {
            const std::vector<HairRect>& rs = bkt.rs;
            float plane = bkt.plane;
            auto emitH = [&](float v, float u0, float u1) {
                if (u1 - u0 < minLen) return;
                Vec3 a{}, b{};
                hairSet(a, axis, plane); hairSet(b, axis, plane);
                hairSet(a, ua, u0); hairSet(a, va, v);
                hairSet(b, ua, u1); hairSet(b, va, v);
                out.push_back({ a, b });
            };
            auto emitV = [&](float u, float v0, float v1) {
                if (v1 - v0 < minLen) return;
                Vec3 a{}, b{};
                hairSet(a, axis, plane); hairSet(b, axis, plane);
                hairSet(a, ua, u); hairSet(a, va, v0);
                hairSet(b, ua, u); hairSet(b, va, v1);
                out.push_back({ a, b });
            };
            auto coveredH = [&](size_t skip, float v, float u0, float u1) {
                std::vector<std::pair<float, float>> segs = { { u0, u1 } };
                for (size_t j = 0; j < rs.size(); j++) {
                    if (j == skip) continue;
                    const HairRect& o = rs[j];
                    if (v < o.v0 - eps || v > o.v1 + eps) continue;
                    hairSub1D(segs, o.u0, o.u1, eps);
                }
                for (const auto& s : segs) emitH(v, s.first, s.second);
            };
            auto coveredV = [&](size_t skip, float u, float v0, float v1) {
                std::vector<std::pair<float, float>> segs = { { v0, v1 } };
                for (size_t j = 0; j < rs.size(); j++) {
                    if (j == skip) continue;
                    const HairRect& o = rs[j];
                    if (u < o.u0 - eps || u > o.u1 + eps) continue;
                    hairSub1D(segs, o.v0, o.v1, eps);
                }
                for (const auto& s : segs) emitV(u, s.first, s.second);
            };
            for (size_t i = 0; i < rs.size(); i++) {
                const HairRect& r = rs[i];
                coveredH(i, r.v0, r.u0, r.u1);
                coveredH(i, r.v1, r.u0, r.u1);
                coveredV(i, r.u0, r.v0, r.v1);
                coveredV(i, r.u1, r.v0, r.v1);
            }
        }
    }
}

inline void voxelizeHairAtoms(const Part& p, std::vector<HairAtom>& out) {
    Vec3 mn = p.center - p.half, mx = p.center + p.half;
    auto nstep = [](float h) {
        int n = (int)std::lround(h * 2.0f / kHairGrid);
        return n < 1 ? 1 : n;
    };
    int nx = nstep(p.half.x), ny = nstep(p.half.y), nz = nstep(p.half.z);
    float dx = (mx.x - mn.x) / (float)nx;
    float dy = (mx.y - mn.y) / (float)ny;
    float dz = (mx.z - mn.z) / (float)nz;
    for (int z = 0; z < nz; z++)
        for (int y = 0; y < ny; y++)
            for (int x = 0; x < nx; x++) {
                HairAtom a;
                a.mn = { mn.x + x * dx, mn.y + y * dy, mn.z + z * dz };
                a.mx = { a.mn.x + dx, a.mn.y + dy, a.mn.z + dz };
                a.id = hairIdFromAabb(a.mn, a.mx);
                out.push_back(a);
            }
}

inline void voxelizeHairAtoms(const Part& p, std::map<HairCell, HairAtom>& out) {
    std::vector<HairAtom> atoms;
    voxelizeHairAtoms(p, atoms);
    for (const HairAtom& a : atoms) out[a.id] = a;
}

inline void voxelizeHairPart(const Part& p, std::set<HairCell>& out) {
    std::map<HairCell, HairAtom> atoms;
    voxelizeHairAtoms(p, atoms);
    for (const auto& kv : atoms) out.insert(kv.first);
}

inline Part hairCuboidFromAabb(const Vec3& mn, const Vec3& mx, const Part& tmpl) {
    Part p = tmpl;
    p.rot = {};
    if (p.kind != "cloth") p.kind = "hair";
    p.name.clear();
    p.support.clear();
    p.extra.clear();
    p.center = (mn + mx) * 0.5f;
    p.half = (mx - mn) * 0.5f;
    return p;
}

inline bool hairPointInAabb(const Vec3& p, const Vec3& mn, const Vec3& mx, float e) {
    return p.x >= mn.x - e && p.x <= mx.x + e
        && p.y >= mn.y - e && p.y <= mx.y + e
        && p.z >= mn.z - e && p.z <= mx.z + e;
}

inline bool hairAtomAt(const std::vector<Part>& parts, const Vec3& p, HairAtom& out) {
    const float e = 1e-3f;
    for (const Part& hp : parts) {
        if (!isGridHairPart(hp)) continue;
        Vec3 mn = hp.center - hp.half, mx = hp.center + hp.half;
        if (!hairPointInAabb(p, mn, mx, e)) continue;
        std::map<HairCell, HairAtom> atoms;
        voxelizeHairAtoms(hp, atoms);
        for (const auto& kv : atoms) {
            if (hairPointInAabb(p, kv.second.mn, kv.second.mx, e)) {
                out = kv.second;
                return true;
            }
        }
        HairAtom fallback;
        fallback.mn = mn;
        fallback.mx = mx;
        fallback.id = hairIdFromAabb(mn, mx);
        out = fallback;
        return true;
    }
    return false;
}

inline bool partOwnsHairCell(const Part& p, HairCell c) {
    std::set<HairCell> cells;
    voxelizeHairPart(p, cells);
    return cells.count(c) != 0;
}

inline int countGridHairCells(const std::vector<Part>& parts) {
    std::set<HairCell> all;
    for (const Part& p : parts)
        if (isGridHairPart(p)) voxelizeHairPart(p, all);
    return (int)all.size();
}

inline const Part* gridHairAt(const std::vector<Part>& parts, HairCell c) {
    for (const Part& p : parts)
        if (isGridHairPart(p) && partOwnsHairCell(p, c)) return &p;
    return nullptr;
}

inline bool hairAxisEqual(const HairAtom& a, const HairAtom& b, int ax, float eps) {
    return std::fabs(hairComp(a.mn, ax) - hairComp(b.mn, ax)) <= eps
        && std::fabs(hairComp(a.mx, ax) - hairComp(b.mx, ax)) <= eps;
}

inline float hairOverlap1(float a0, float a1, float b0, float b1) {
    return std::min(a1, b1) - std::max(a0, b0);
}

inline float hairAtomVolume(const HairAtom& a) {
    float x = a.mx.x - a.mn.x, y = a.mx.y - a.mn.y, z = a.mx.z - a.mn.z;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (z < 0.0f) z = 0.0f;
    return x * y * z;
}

inline bool hairAbuts(const HairAtom& a, const HairAtom& b, int ax, float eps) {
    return std::fabs(hairComp(a.mx, ax) - hairComp(b.mn, ax)) <= eps
        || std::fabs(hairComp(b.mx, ax) - hairComp(a.mn, ax)) <= eps;
}

// Shared internal face (partial overlap OK). Lengths need not match.
inline bool hairSharesFace(const HairAtom& a, const HairAtom& b) {
    const float eps = 2.5e-3f;
    for (int ax = 0; ax < 3; ax++) {
        if (!hairAbuts(a, b, ax, eps)) continue;
        int u = (ax + 1) % 3, v = (ax + 2) % 3;
        float ou = hairOverlap1(hairComp(a.mn, u), hairComp(a.mx, u), hairComp(b.mn, u), hairComp(b.mx, u));
        float ov = hairOverlap1(hairComp(a.mn, v), hairComp(a.mx, v), hairComp(b.mn, v), hairComp(b.mx, v));
        if (ou > eps && ov > eps) return true;
    }
    return false;
}

// Merge into one cuboid only when that cuboid equals the union of volumes (no hole-fill).
inline bool hairCanMerge(const HairAtom& a, const HairAtom& b, int ax) {
    const float eps = 2.5e-3f;
    int u = (ax + 1) % 3, v = (ax + 2) % 3;
    if (!hairAxisEqual(a, b, u, eps) || !hairAxisEqual(a, b, v, eps) || !hairAbuts(a, b, ax, eps))
        return false;
    HairAtom un;
    un.mn = { std::min(a.mn.x, b.mn.x), std::min(a.mn.y, b.mn.y), std::min(a.mn.z, b.mn.z) };
    un.mx = { std::max(a.mx.x, b.mx.x), std::max(a.mx.y, b.mx.y), std::max(a.mx.z, b.mx.z) };
    return hairAtomVolume(un) <= (hairAtomVolume(a) + hairAtomVolume(b)) * 1.02f;
}

inline bool hairAtomFilled(const HairAtom& a, const HairAtom& b) {
    float ox = hairOverlap1(a.mn.x, a.mx.x, b.mn.x, b.mx.x);
    float oy = hairOverlap1(a.mn.y, a.mx.y, b.mn.y, b.mx.y);
    float oz = hairOverlap1(a.mn.z, a.mx.z, b.mn.z, b.mx.z);
    if (ox <= 1e-4f || oy <= 1e-4f || oz <= 1e-4f) return false;
    float va = hairAtomVolume(a);
    if (va < 1e-10f) return true;
    return (ox * oy * oz) > va * 0.45f;
}

inline bool hairAtomOccupied(const std::vector<Part>& parts, const HairAtom& a, const Part& tmpl) {
    for (const Part& p : parts) {
        if (!hairMergeCompat(p, tmpl)) continue;
        std::vector<HairAtom> local;
        voxelizeHairAtoms(p, local);
        for (const HairAtom& b : local)
            if (hairAtomFilled(a, b)) return true;
    }
    return false;
}

// Any grid hair in this volume (any color) — used so paint cannot thicken a layer.
inline bool hairAtomBlocked(const std::vector<Part>& parts, const HairAtom& a) {
    for (const Part& p : parts) {
        if (!isGridHairPart(p)) continue;
        std::vector<HairAtom> local;
        voxelizeHairAtoms(p, local);
        for (const HairAtom& b : local)
            if (hairAtomFilled(a, b)) return true;
    }
    return false;
}

// Same tangent column, further along +n (a thicker layer, not a neighbor).
inline bool hairIsOutwardStack(const HairCell& from, const HairCell& to, const Vec3& n) {
    int ax = hairDomAxis(n);
    int d[3] = { to.x - from.x, to.y - from.y, to.z - from.z };
    int along = (ax == 0) ? d[0] : (ax == 1) ? d[1] : d[2];
    int s = (hairComp(n, ax) >= 0.0f) ? 1 : -1;
    if (along * s <= 0) return false;
    int t1 = d[(ax + 1) % 3], t2 = d[(ax + 2) % 3];
    return t1 == 0 && t2 == 0;
}

inline HairAtom hairUnionAtom(const HairAtom& a, const HairAtom& b) {
    HairAtom r;
    r.mn = { std::min(a.mn.x, b.mn.x), std::min(a.mn.y, b.mn.y), std::min(a.mn.z, b.mn.z) };
    r.mx = { std::max(a.mx.x, b.mx.x), std::max(a.mx.y, b.mx.y), std::max(a.mx.z, b.mx.z) };
    r.id = hairIdFromAabb(r.mn, r.mx);
    return r;
}

inline void floodHairAtoms(const std::vector<HairAtom>& atoms, int seed, std::vector<uint8_t>& in) {
    in.assign(atoms.size(), 0);
    if (seed < 0 || seed >= (int)atoms.size()) return;
    std::vector<int> st;
    st.push_back(seed);
    in[seed] = 1;
    while (!st.empty()) {
        int i = st.back(); st.pop_back();
        for (int j = 0; j < (int)atoms.size(); j++) {
            if (in[j]) continue;
            if (!hairSharesFace(atoms[i], atoms[j])) continue;
            in[j] = 1;
            st.push_back(j);
        }
    }
}

// Pairwise cuboid merge: aligned runs become bigger boxes; L/T leftovers stay separate.
inline void greedyHairCuboids(std::vector<HairAtom> cur, const Part& tmpl, std::vector<Part>& out) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < cur.size() && !changed; i++) {
            for (size_t j = i + 1; j < cur.size(); j++) {
                bool ok = false;
                for (int ax = 0; ax < 3; ax++)
                    if (hairCanMerge(cur[i], cur[j], ax)) { ok = true; break; }
                if (!ok) continue;
                cur[i] = hairUnionAtom(cur[i], cur[j]);
                cur.erase(cur.begin() + (int)j);
                changed = true;
                break;
            }
        }
    }
    out.reserve(out.size() + cur.size());
    for (const HairAtom& a : cur)
        out.push_back(hairCuboidFromAabb(a.mn, a.mx, tmpl));
}

inline HairAtom hairPartAtom(const Part& p) {
    HairAtom a;
    a.mn = p.center - p.half;
    a.mx = p.center + p.half;
    a.id = hairIdFromAabb(a.mn, a.mx);
    return a;
}

inline bool hairAtomEq(const HairAtom& a, const HairAtom& b) {
    if (a.id == b.id) return true;
    return hairAtomFilled(a, b) && hairAtomFilled(b, a);
}

inline bool hairAtomInSel(const std::vector<HairAtom>& sel, const HairAtom& a) {
    for (const HairAtom& s : sel)
        if (hairAtomEq(s, a)) return true;
    return false;
}

inline int hairAtomSelIndex(const std::vector<HairAtom>& sel, const HairAtom& a) {
    for (int i = 0; i < (int)sel.size(); i++)
        if (hairAtomEq(sel[i], a)) return i;
    return -1;
}

// Add one G-cube. Merge only with a fully aligned neighbor (cuboid, no L/T bind).
inline bool paintHairVoxel(std::vector<Part>& parts, HairAtom atom, Part tmpl) {
    tmpl.rot = {};
    if (tmpl.kind != "cloth") tmpl.kind = "hair";
    if (hairAtomOccupied(parts, atom, tmpl)) return false;
    int cap = (tmpl.kind == "cloth") ? 8000 : kHairVoxelMax;
    if (countGridHairCells(parts) >= cap) return false;

    int mergeI = -1;
    HairAtom merged = atom;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!hairMergeCompat(parts[i], tmpl)) continue;
        HairAtom b = hairPartAtom(parts[i]);
        for (int ax = 0; ax < 3; ax++) {
            if (!hairCanMerge(atom, b, ax)) continue;
            mergeI = i;
            merged = hairUnionAtom(atom, b);
            break;
        }
        if (mergeI >= 0) break;
    }
    if (mergeI >= 0) {
        std::string nm = parts[mergeI].name;
        int b = parts[mergeI].bind;
        parts[mergeI] = hairCuboidFromAabb(merged.mn, merged.mx, tmpl);
        parts[mergeI].name = std::move(nm);
        parts[mergeI].bind = b;
        return true;
    }
    Part np = hairCuboidFromAabb(atom.mn, atom.mx, tmpl);
    np.bind = -1;
    parts.push_back(std::move(np));
    return true;
}

inline bool eraseHairVoxel(std::vector<Part>& parts, const HairAtom& hit) {
    Vec3 c = (hit.mn + hit.mx) * 0.5f;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isGridHairPart(parts[i])) continue;
        Vec3 mn = parts[i].center - parts[i].half, mx = parts[i].center + parts[i].half;
        if (!hairPointInAabb(c, mn, mx, 1e-3f)) continue;
        std::vector<HairAtom> local;
        voxelizeHairAtoms(parts[i], local);
        std::vector<HairAtom> keep;
        keep.reserve(local.size());
        bool cut = false;
        for (const HairAtom& a : local) {
            if (hairAtomFilled(hit, a) || hairPointInAabb(c, a.mn, a.mx, 1e-3f)) {
                cut = true;
                continue;
            }
            keep.push_back(a);
        }
        if (!cut) continue;
        Part tmpl = parts[i];
        parts.erase(parts.begin() + i);
        std::vector<Part> built;
        greedyHairCuboids(std::move(keep), tmpl, built);
        for (Part& p : built) {
            p.bind = tmpl.bind;
            parts.push_back(std::move(p));
        }
        return true;
    }
    return false;
}

struct HairStrip {
    int idx = -1;
    std::vector<HairAtom> keep;
    std::vector<HairAtom> take;
};

inline void hairCollectStrips(const std::vector<Part>& parts, const std::vector<HairAtom>& sel,
                              std::vector<HairStrip>& strips, std::vector<HairAtom>& got) {
    strips.clear();
    got.clear();
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isGridHairPart(parts[i])) continue;
        std::vector<HairAtom> local;
        voxelizeHairAtoms(parts[i], local);
        HairStrip st;
        st.idx = i;
        for (const HairAtom& a : local) {
            if (hairAtomInSel(sel, a)) st.take.push_back(a);
            else st.keep.push_back(a);
        }
        if (st.take.empty()) continue;
        for (const HairAtom& t : st.take) got.push_back(t);
        strips.push_back(std::move(st));
    }
}

inline int hairNextBind(const std::vector<Part>& parts) {
    int b = 0;
    for (const Part& p : parts)
        if (p.bind >= b) b = p.bind + 1;
    return b;
}

// Voxel + whole-part mix: keep only cuboids that contain a partial voxel pick
// and expand those cuboids to every cell. Extra fully-selected parts are dropped.
inline bool hairResolveMergeSel(const std::vector<Part>& parts, std::vector<HairAtom>& sel) {
    if (sel.empty()) return false;
    std::vector<int> partial, full;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isGridHairPart(parts[i])) continue;
        std::vector<HairAtom> local;
        voxelizeHairAtoms(parts[i], local);
        if (local.empty()) continue;
        int hit = 0;
        for (const HairAtom& a : local)
            if (hairAtomInSel(sel, a)) hit++;
        if (hit == 0) continue;
        if (hit < (int)local.size()) partial.push_back(i);
        else full.push_back(i);
    }
    if (partial.empty() || full.empty()) return false;
    sel.clear();
    for (int i : partial)
        voxelizeHairAtoms(parts[i], sel);
    return true;
}

inline void hairAddUniqueAtom(std::vector<HairAtom>& sel, const HairAtom& a) {
    if (!hairAtomInSel(sel, a)) sel.push_back(a);
}

inline void hairVoxelizeClump(const std::vector<Part>& parts, int seed, std::vector<HairAtom>& out) {
    if (seed < 0 || seed >= (int)parts.size() || !isGridHairPart(parts[seed])) return;
    int b = parts[seed].bind;
    if (b < 0) {
        voxelizeHairAtoms(parts[seed], out);
        return;
    }
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isGridHairPart(parts[i]) || parts[i].bind != b) continue;
        voxelizeHairAtoms(parts[i], out);
    }
}

// Fully selected cuboids pull in the rest of their bind group (a merged 整体).
inline void hairExpandFullBind(const std::vector<Part>& parts, std::vector<HairAtom>& sel) {
    if (sel.empty()) return;
    std::vector<int> binds;
    auto addBind = [&](int b) {
        if (b < 0) return;
        for (int x : binds) if (x == b) return;
        binds.push_back(b);
    };
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isGridHairPart(parts[i]) || parts[i].bind < 0) continue;
        std::vector<HairAtom> local;
        voxelizeHairAtoms(parts[i], local);
        if (local.empty()) continue;
        int hit = 0;
        for (const HairAtom& a : local)
            if (hairAtomInSel(sel, a)) hit++;
        if (hit == (int)local.size()) addBind(parts[i].bind);
    }
    if (binds.empty()) return;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isGridHairPart(parts[i])) continue;
        bool want = false;
        for (int b : binds) if (parts[i].bind == b) { want = true; break; }
        if (!want) continue;
        std::vector<HairAtom> local;
        voxelizeHairAtoms(parts[i], local);
        for (const HairAtom& a : local) hairAddUniqueAtom(sel, a);
    }
}

inline bool hairEdgeOnClumpNeighbor(const Part& p, const Vec3& ea, const Vec3& eb,
                                    const std::vector<Part>& parts) {
    if (!isGridHairPart(p) || p.bind < 0) return false;
    const float e = 2.5e-3f;
    HairAtom pa = hairPartAtom(p);
    for (const Part& q : parts) {
        if (&q == &p || !isGridHairPart(q) || q.bind != p.bind) continue;
        if (!hairSharesFace(pa, hairPartAtom(q))) continue;
        Vec3 qn = q.center - q.half, qx = q.center + q.half;
        if (hairPointInAabb(ea, qn, qx, e) && hairPointInAabb(eb, qn, qx, e))
            return true;
    }
    return false;
}

inline void hairApplyStrips(std::vector<Part>& parts, std::vector<HairStrip> strips) {
    std::sort(strips.begin(), strips.end(), [](const HairStrip& a, const HairStrip& b) {
        return a.idx > b.idx;
    });
    for (HairStrip& st : strips) {
        if (st.idx < 0 || st.idx >= (int)parts.size()) continue;
        Part tmpl = parts[st.idx];
        parts.erase(parts.begin() + st.idx);
        if (st.keep.empty()) continue;
        std::vector<Part> rebuilt;
        greedyHairCuboids(std::move(st.keep), tmpl, rebuilt);
        for (size_t k = 0; k < rebuilt.size(); k++) {
            rebuilt[k].bind = tmpl.bind;
            if (k == 0) rebuilt[k].name = tmpl.name;
            else rebuilt[k].name.clear();
            parts.push_back(std::move(rebuilt[k]));
        }
    }
}

// Manual merge: selection-first. Aligned runs still union; leftovers stay cuboids
// but share one bind so L/T count as one clump (no hole-fill AABB).
inline bool mergeHairSelection(std::vector<Part>& parts, std::vector<HairAtom> sel,
                               std::vector<HairAtom>* outSel = nullptr) {
    bool mixed = hairResolveMergeSel(parts, sel);
    if (!mixed) hairExpandFullBind(parts, sel);
    std::vector<HairStrip> strips;
    std::vector<HairAtom> got;
    hairCollectStrips(parts, sel, strips, got);
    if (got.size() < 2 && strips.size() < 2) return false;

    struct Group { Part tmpl; std::vector<HairAtom> atoms; };
    std::vector<Group> groups;
    for (const HairStrip& st : strips) {
        const Part& t = parts[st.idx];
        int gi = -1;
        for (int g = 0; g < (int)groups.size(); g++) {
            if (hairMergeCompat(t, groups[g].tmpl)) { gi = g; break; }
        }
        if (gi < 0) {
            Group ng;
            ng.tmpl = t;
            ng.tmpl.rot = {};
            ng.tmpl.kind = "hair";
            ng.tmpl.bind = -1;
            ng.tmpl.name.clear();
            gi = (int)groups.size();
            groups.push_back(std::move(ng));
        }
        if (st.keep.empty())
            groups[gi].atoms.push_back(hairPartAtom(parts[st.idx]));
        else
            for (const HairAtom& a : st.take) groups[gi].atoms.push_back(a);
    }

    hairApplyStrips(parts, std::move(strips));
    if (outSel) outSel->clear();
    for (Group& g : groups) {
        std::vector<Part> built;
        greedyHairCuboids(std::move(g.atoms), g.tmpl, built);
        int b = -1;
        if ((int)built.size() > 1) b = hairNextBind(parts);
        for (Part& p : built) {
            p.bind = b;
            if (outSel) voxelizeHairAtoms(p, *outSel);
            parts.push_back(std::move(p));
        }
    }
    return true;
}

// Extract selected voxels as individual G-cubes. Remainder of each owner is re-greedy'd.
inline bool splitHairSelection(std::vector<Part>& parts, const std::vector<HairAtom>& sel,
                               std::vector<HairAtom>* outSel = nullptr) {
    std::vector<HairStrip> strips;
    std::vector<HairAtom> got;
    hairCollectStrips(parts, sel, strips, got);
    if (got.empty()) return false;

    std::vector<Part> tmpls;
    tmpls.reserve(got.size());
    for (const HairStrip& st : strips) {
        Part t = parts[st.idx];
        t.rot = {};
        t.kind = "hair";
        t.bind = -1;
        t.name.clear();
        for (size_t k = 0; k < st.take.size(); k++) tmpls.push_back(t);
    }

    hairApplyStrips(parts, std::move(strips));
    if (outSel) outSel->clear();
    for (int i = 0; i < (int)got.size(); i++) {
        Part np = hairCuboidFromAabb(got[i].mn, got[i].mx, tmpls[i]);
        np.bind = -1;
        if (outSel) outSel->push_back(got[i]);
        parts.push_back(std::move(np));
    }
    return true;
}

// Paint: insert atom then remesh the face-connected clump (partial overlap counts).
// Erase: remove the hit atom from that clump; overlapping faces stay clipped at draw.
// Irregular (L/T/uneven) clumps stay multiple cuboids bound as one group.
inline bool remeshHairClump(std::vector<Part>& parts, HairCell seed, bool erase, const Part& tmpl,
                            const HairAtom* paintAtom = nullptr) {
    std::vector<HairAtom> atoms;
    std::vector<int> owner;
    atoms.reserve(64);
    owner.reserve(64);
    for (int i = 0; i < (int)parts.size(); i++) {
        const Part& p = parts[i];
        if (!hairMergeCompat(p, tmpl)) continue;
        std::vector<HairAtom> local;
        voxelizeHairAtoms(p, local);
        for (const HairAtom& a : local) {
            atoms.push_back(a);
            owner.push_back(i);
        }
    }
    int seedIdx = -1;
    if (erase) {
        for (int i = 0; i < (int)atoms.size(); i++) {
            if (atoms[i].id == seed) { seedIdx = i; break; }
        }
        if (seedIdx < 0 && paintAtom) {
            Vec3 c = (paintAtom->mn + paintAtom->mx) * 0.5f;
            for (int i = 0; i < (int)atoms.size(); i++) {
                if (hairPointInAabb(c, atoms[i].mn, atoms[i].mx, 1e-3f)) { seedIdx = i; break; }
            }
        }
        if (seedIdx < 0) return false;
    } else {
        if (!paintAtom) return false;
        for (const HairAtom& a : atoms)
            if (hairAtomFilled(*paintAtom, a)) return false;
        if (countGridHairCells(parts) >= kHairVoxelMax) return false;
        seedIdx = (int)atoms.size();
        atoms.push_back(*paintAtom);
        owner.push_back(-1);
    }
    std::vector<uint8_t> in;
    floodHairAtoms(atoms, seedIdx, in);
    bool any = false;
    for (uint8_t v : in) if (v) { any = true; break; }
    if (!any) return false;

    std::vector<uint8_t> kill(parts.size(), 0);
    int inheritBind = tmpl.bind;
    for (int i = 0; i < (int)atoms.size(); i++) {
        if (!in[i] || owner[i] < 0) continue;
        kill[owner[i]] = 1;
        int b = parts[owner[i]].bind;
        if (b >= 0) inheritBind = b;
    }
    std::vector<HairAtom> rebuild;
    rebuild.reserve(atoms.size());
    for (int i = 0; i < (int)atoms.size(); i++) {
        if (erase && i == seedIdx) continue;
        int ow = owner[i];
        if (ow < 0 || kill[ow])
            rebuild.push_back(atoms[i]);
    }
    std::vector<Part> kept;
    kept.reserve(parts.size());
    for (int i = 0; i < (int)parts.size(); i++)
        if (!kill[i]) kept.push_back(parts[i]);
    std::vector<Part> built;
    greedyHairCuboids(std::move(rebuild), tmpl, built);
    if (built.size() > 1) {
        int b = inheritBind;
        if (b < 0) {
            b = 0;
            for (const Part& p : kept)
                if (p.bind >= b) b = p.bind + 1;
        }
        for (Part& p : built) p.bind = b;
    }
    for (Part& p : built) kept.push_back(std::move(p));
    parts = std::move(kept);
    return true;
}

inline bool isGridHairCard(const Part& p) {
    if (!isHairCardPart(p)) return false;
    if (p.rot.lengthSq() > 1e-6f) return false;
    return p.half.x > 1e-5f && p.half.y > 1e-5f && p.half.z > 1e-5f;
}

inline HairCell hairCardIdFromAabb(const Vec3& mn, const Vec3& mx) {
    (void)mx;
    const float e = 1e-4f;
    return {
        (int)std::floor(mn.x / kHairCard + e),
        (int)std::floor(mn.y / kHairCard + e),
        (int)std::floor(mn.z / kHairCard + e)
    };
}

inline bool hairCardMergeCompat(const Part& p, const Part& tmpl) {
    if (!isGridHairCard(p)) return false;
    if (!hairColorSame(p.color, tmpl.color)) return false;
    if (!hairTexCompat(p.tex, tmpl.tex)) return false;
    if (p.type != tmpl.type) return false;
    int ax = thinAxis(p), bx = thinAxis(tmpl);
    if (ax != bx) return false;
    return std::fabs(hairComp(p.center, ax) - hairComp(tmpl.center, ax)) <= 2.5e-3f;
}

inline void voxelizeHairCardAtoms(const Part& p, std::vector<HairAtom>& out) {
    Vec3 mn = p.center - p.half, mx = p.center + p.half;
    if (p.rot.lengthSq() > 1e-6f) {
        HairAtom a;
        a.mn = mn; a.mx = mx;
        a.id = hairCardIdFromAabb(mn, mx);
        out.push_back(a);
        return;
    }
    auto nstep = [](float h) {
        int n = (int)std::lround(h * 2.0f / kHairCard);
        return n < 1 ? 1 : n;
    };
    int nx = nstep(p.half.x), ny = nstep(p.half.y), nz = nstep(p.half.z);
    float dx = (mx.x - mn.x) / (float)nx;
    float dy = (mx.y - mn.y) / (float)ny;
    float dz = (mx.z - mn.z) / (float)nz;
    for (int z = 0; z < nz; z++)
        for (int y = 0; y < ny; y++)
            for (int x = 0; x < nx; x++) {
                HairAtom a;
                a.mn = { mn.x + x * dx, mn.y + y * dy, mn.z + z * dz };
                a.mx = { a.mn.x + dx, a.mn.y + dy, a.mn.z + dz };
                a.id = hairCardIdFromAabb(a.mn, a.mx);
                out.push_back(a);
            }
}

inline Part hairCardFromAabb(const Vec3& mn, const Vec3& mx, const Part& tmpl) {
    Part p = tmpl;
    p.rot = {};
    p.kind = "haircard";
    p.name.clear();
    p.support.clear();
    p.extra.clear();
    p.center = (mn + mx) * 0.5f;
    p.half = (mx - mn) * 0.5f;
    return p;
}

inline HairAtom hairCardPartAtom(const Part& p) {
    HairAtom a;
    a.mn = p.center - p.half;
    a.mx = p.center + p.half;
    a.id = hairCardIdFromAabb(a.mn, a.mx);
    return a;
}

inline bool hairCardAtomAt(const std::vector<Part>& parts, const Vec3& p, HairAtom& out) {
    const float e = 1e-3f;
    for (const Part& hp : parts) {
        if (!isHairCardPart(hp)) continue;
        Vec3 mn = hp.center - hp.half, mx = hp.center + hp.half;
        if (!hairPointInAabb(p, mn, mx, e)) continue;
        std::vector<HairAtom> atoms;
        voxelizeHairCardAtoms(hp, atoms);
        for (const HairAtom& a : atoms) {
            if (hairPointInAabb(p, a.mn, a.mx, e)) {
                out = a;
                return true;
            }
        }
        out = hairCardPartAtom(hp);
        return true;
    }
    return false;
}

inline void greedyHairCards(std::vector<HairAtom> cur, const Part& tmpl, std::vector<Part>& out) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < cur.size() && !changed; i++) {
            for (size_t j = i + 1; j < cur.size(); j++) {
                bool ok = false;
                for (int ax = 0; ax < 3; ax++)
                    if (hairCanMerge(cur[i], cur[j], ax)) { ok = true; break; }
                if (!ok) continue;
                cur[i] = hairUnionAtom(cur[i], cur[j]);
                cur[i].id = hairCardIdFromAabb(cur[i].mn, cur[i].mx);
                cur.erase(cur.begin() + (int)j);
                changed = true;
                break;
            }
        }
    }
    out.reserve(out.size() + cur.size());
    for (const HairAtom& a : cur)
        out.push_back(hairCardFromAabb(a.mn, a.mx, tmpl));
}

inline void hairCollectCardStrips(const std::vector<Part>& parts, const std::vector<HairAtom>& sel,
                                 std::vector<HairStrip>& strips, std::vector<HairAtom>& got) {
    strips.clear();
    got.clear();
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isHairCardPart(parts[i])) continue;
        std::vector<HairAtom> local;
        voxelizeHairCardAtoms(parts[i], local);
        HairStrip st;
        st.idx = i;
        for (const HairAtom& a : local) {
            if (hairAtomInSel(sel, a)) st.take.push_back(a);
            else st.keep.push_back(a);
        }
        if (st.take.empty()) continue;
        for (const HairAtom& t : st.take) got.push_back(t);
        strips.push_back(std::move(st));
    }
}

inline void hairApplyCardStrips(std::vector<Part>& parts, std::vector<HairStrip> strips) {
    std::sort(strips.begin(), strips.end(), [](const HairStrip& a, const HairStrip& b) {
        return a.idx > b.idx;
    });
    for (HairStrip& st : strips) {
        if (st.idx < 0 || st.idx >= (int)parts.size()) continue;
        Part tmpl = parts[st.idx];
        parts.erase(parts.begin() + st.idx);
        if (st.keep.empty()) continue;
        std::vector<Part> rebuilt;
        greedyHairCards(std::move(st.keep), tmpl, rebuilt);
        for (size_t k = 0; k < rebuilt.size(); k++) {
            rebuilt[k].bind = tmpl.bind;
            if (k == 0) rebuilt[k].name = tmpl.name;
            else rebuilt[k].name.clear();
            parts.push_back(std::move(rebuilt[k]));
        }
    }
}

inline bool hairResolveCardMergeSel(const std::vector<Part>& parts, std::vector<HairAtom>& sel) {
    if (sel.empty()) return false;
    std::vector<int> partial, full;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isHairCardPart(parts[i])) continue;
        std::vector<HairAtom> local;
        voxelizeHairCardAtoms(parts[i], local);
        if (local.empty()) continue;
        int hit = 0;
        for (const HairAtom& a : local)
            if (hairAtomInSel(sel, a)) hit++;
        if (hit == 0) continue;
        if (hit < (int)local.size()) partial.push_back(i);
        else full.push_back(i);
    }
    if (partial.empty() || full.empty()) return false;
    sel.clear();
    for (int i : partial)
        voxelizeHairCardAtoms(parts[i], sel);
    return true;
}

inline void hairCardVoxelizeClump(const std::vector<Part>& parts, int seed, std::vector<HairAtom>& out) {
    if (seed < 0 || seed >= (int)parts.size() || !isHairCardPart(parts[seed])) return;
    int b = parts[seed].bind;
    if (b < 0) {
        voxelizeHairCardAtoms(parts[seed], out);
        return;
    }
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isHairCardPart(parts[i]) || parts[i].bind != b) continue;
        voxelizeHairCardAtoms(parts[i], out);
    }
}

inline void hairExpandFullCardBind(const std::vector<Part>& parts, std::vector<HairAtom>& sel) {
    if (sel.empty()) return;
    std::vector<int> binds;
    auto addBind = [&](int b) {
        if (b < 0) return;
        for (int x : binds) if (x == b) return;
        binds.push_back(b);
    };
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isHairCardPart(parts[i]) || parts[i].bind < 0) continue;
        std::vector<HairAtom> local;
        voxelizeHairCardAtoms(parts[i], local);
        if (local.empty()) continue;
        int hit = 0;
        for (const HairAtom& a : local)
            if (hairAtomInSel(sel, a)) hit++;
        if (hit == (int)local.size()) addBind(parts[i].bind);
    }
    if (binds.empty()) return;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!isHairCardPart(parts[i])) continue;
        bool want = false;
        for (int b : binds) if (parts[i].bind == b) { want = true; break; }
        if (!want) continue;
        std::vector<HairAtom> local;
        voxelizeHairCardAtoms(parts[i], local);
        for (const HairAtom& a : local) hairAddUniqueAtom(sel, a);
    }
}

inline bool mergeHairCardSelection(std::vector<Part>& parts, std::vector<HairAtom> sel,
                                   std::vector<HairAtom>* outSel = nullptr) {
    bool mixed = hairResolveCardMergeSel(parts, sel);
    if (!mixed) hairExpandFullCardBind(parts, sel);
    std::vector<HairStrip> strips;
    std::vector<HairAtom> got;
    hairCollectCardStrips(parts, sel, strips, got);
    if (got.size() < 2 && strips.size() < 2) return false;

    struct Group { Part tmpl; std::vector<HairAtom> atoms; };
    std::vector<Group> groups;
    for (const HairStrip& st : strips) {
        const Part& t = parts[st.idx];
        int gi = -1;
        for (int g = 0; g < (int)groups.size(); g++) {
            if (hairCardMergeCompat(t, groups[g].tmpl)) { gi = g; break; }
        }
        if (gi < 0) {
            Group ng;
            ng.tmpl = t;
            ng.tmpl.rot = {};
            ng.tmpl.kind = "haircard";
            ng.tmpl.bind = -1;
            ng.tmpl.name.clear();
            gi = (int)groups.size();
            groups.push_back(std::move(ng));
        }
        if (st.keep.empty())
            groups[gi].atoms.push_back(hairCardPartAtom(parts[st.idx]));
        else
            for (const HairAtom& a : st.take) groups[gi].atoms.push_back(a);
    }
    if (groups.empty()) return false;

    hairApplyCardStrips(parts, std::move(strips));
    if (outSel) outSel->clear();
    for (Group& g : groups) {
        std::vector<Part> built;
        greedyHairCards(std::move(g.atoms), g.tmpl, built);
        int b = -1;
        if ((int)built.size() > 1) b = hairNextBind(parts);
        for (Part& p : built) {
            p.bind = b;
            if (outSel) voxelizeHairCardAtoms(p, *outSel);
            parts.push_back(std::move(p));
        }
    }
    return true;
}

inline bool splitHairCardSelection(std::vector<Part>& parts, const std::vector<HairAtom>& sel,
                                   std::vector<HairAtom>* outSel = nullptr) {
    std::vector<HairStrip> strips;
    std::vector<HairAtom> got;
    hairCollectCardStrips(parts, sel, strips, got);
    if (got.empty()) return false;

    std::vector<Part> tmpls;
    tmpls.reserve(got.size());
    for (const HairStrip& st : strips) {
        Part t = parts[st.idx];
        t.rot = {};
        t.kind = "haircard";
        t.bind = -1;
        t.name.clear();
        for (size_t k = 0; k < st.take.size(); k++) tmpls.push_back(t);
    }

    hairApplyCardStrips(parts, std::move(strips));
    if (outSel) outSel->clear();
    for (int i = 0; i < (int)got.size(); i++) {
        Part np = hairCardFromAabb(got[i].mn, got[i].mx, tmpls[i]);
        np.bind = -1;
        if (outSel) outSel->push_back(got[i]);
        parts.push_back(std::move(np));
    }
    return true;
}

} // namespace pm
