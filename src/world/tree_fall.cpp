#include "world.hpp"
#include "../core/config.hpp"
#include "../render/textures.hpp"
#include "../render/block_geo.hpp"
#include "log_appear.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tree_fall {
namespace {

int floorToInt(float v) {
    return (int)std::floor(v);
}

float voxelMass(uint8_t b) {
    return blockWeight(b);
}

struct IVec3Hash {
    size_t operator()(const IVec3& v) const noexcept {
        size_t h = (size_t)(uint32_t)v.x;
        h = h * 1973u + (size_t)(uint32_t)v.y;
        h = h * 1973u + (size_t)(uint32_t)v.z;
        return h;
    }
};

struct Contact {
    Vec3 n{ 0, 1, 0 };
    float pen = 0.0f;
};

void closestOnAabb(const Vec3& c, int bx, int by, int bz, float S, Vec3& closest, bool& inside) {
    float minx = (float)bx * S, maxx = (float)(bx + 1) * S;
    float miny = (float)by * S, maxy = (float)(by + 1) * S;
    float minz = (float)bz * S, maxz = (float)(bz + 1) * S;
    closest.x = std::max(minx, std::min(c.x, maxx));
    closest.y = std::max(miny, std::min(c.y, maxy));
    closest.z = std::max(minz, std::min(c.z, maxz));
    inside = (c.x > minx && c.x < maxx && c.y > miny && c.y < maxy && c.z > minz && c.z < maxz);
}

Vec3 exitNormal(const Vec3& c, int bx, int by, int bz, float S) {
    float minx = (float)bx * S, maxx = (float)(bx + 1) * S;
    float miny = (float)by * S, maxy = (float)(by + 1) * S;
    float minz = (float)bz * S, maxz = (float)(bz + 1) * S;
    float dx = std::min(c.x - minx, maxx - c.x);
    float dy = std::min(c.y - miny, maxy - c.y);
    float dz = std::min(c.z - minz, maxz - c.z);
    if (dx <= dy && dx <= dz) return { (c.x - minx < maxx - c.x) ? -1.0f : 1.0f, 0, 0 };
    if (dy <= dz) return { 0, (c.y - miny < maxy - c.y) ? -1.0f : 1.0f, 0 };
    return { 0, 0, (c.z - minz < maxz - c.z) ? -1.0f : 1.0f };
}

void gatherContacts(World& w, const Vec3& center, float radius, std::vector<Contact>& out) {
    const float S = cfg::BLOCK_SCALE;
    int bx = floorToInt(center.x / S);
    int by = floorToInt(center.y / S);
    int bz = floorToInt(center.z / S);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                int x = bx + dx, y = by + dy, z = bz + dz;
                uint8_t b = w.getBlock(x, y, z);
                if (!isSolid(b) || b == LOG || isFoliage(b)) continue;
                Vec3 closest;
                bool inside = false;
                closestOnAabb(center, x, y, z, S, closest, inside);
                Contact ct;
                if (inside) {
                    ct.n = exitNormal(center, x, y, z, S);
                    float minx = (float)x * S, maxx = (float)(x + 1) * S;
                    float miny = (float)y * S, maxy = (float)(y + 1) * S;
                    float minz = (float)z * S, maxz = (float)(z + 1) * S;
                    if (std::fabs(ct.n.x) > 0.5f)
                        ct.pen = radius + ((ct.n.x > 0) ? (maxx - center.x) : (center.x - minx));
                    else if (std::fabs(ct.n.y) > 0.5f)
                        ct.pen = radius + ((ct.n.y > 0) ? (maxy - center.y) : (center.y - miny));
                    else
                        ct.pen = radius + ((ct.n.z > 0) ? (maxz - center.z) : (center.z - minz));
                    out.push_back(ct);
                } else {
                    Vec3 d = center - closest;
                    float dist = d.length();
                    if (dist < radius && dist > 1e-6f) {
                        ct.n = d / dist;
                        ct.pen = radius - dist;
                        out.push_back(ct);
                    }
                }
            }
        }
    }
}

} // namespace

void orthonormalize(PhysicsIsland& t) {
    t.ax = t.ax.normalized();
    if (t.ax.lengthSq() < 1e-8f) t.ax = { 1, 0, 0 };
    t.az = t.ax.cross(t.ay);
    if (t.az.lengthSq() < 1e-8f) {
        t.ay = (std::fabs(t.ax.y) < 0.9f) ? Vec3{ 0, 1, 0 } : Vec3{ 1, 0, 0 };
        t.az = t.ax.cross(t.ay);
    }
    t.az = t.az.normalized();
    t.ay = t.az.cross(t.ax).normalized();
}

void integrateRotation(PhysicsIsland& t, float dt) {
    t.ax += t.omega.cross(t.ax) * dt;
    t.ay += t.omega.cross(t.ay) * dt;
    orthonormalize(t);
}

void allocate(PhysicsIsland& t, int sx, int sy, int sz) {
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    if (sz < 1) sz = 1;
    t.sx = sx; t.sy = sy; t.sz = sz;
    size_t n = (size_t)sx * (size_t)sy * (size_t)sz;
    t.blocks.assign(n, AIR);
    t.flags.assign(n, 0);
    t.treeIds.assign(n, 0);
    t.extra.assign(n, PhysCellState{});
    t.cells.clear();
    t.meshOpaque.clear();
    t.meshDirty = true;
}

void rebuildOccupied(PhysicsIsland& t) {
    t.cells.clear();
    for (int y = 0; y < t.sy; y++)
        for (int z = 0; z < t.sz; z++)
            for (int x = 0; x < t.sx; x++)
                if (t.get(x, y, z) != AIR) t.cells.push_back({ x, y, z });
}

void setCell(PhysicsIsland& t, int x, int y, int z, uint8_t b, uint8_t fl, uint32_t bind) {
    if (!t.inBounds(x, y, z)) return;
    int i = t.index(x, y, z);
    uint8_t prev = t.blocks[(size_t)i];
    t.blocks[(size_t)i] = b;
    t.flags[(size_t)i] = (b == AIR) ? 0 : fl;
    if (!t.treeIds.empty()) t.treeIds[(size_t)i] = (b == AIR) ? 0 : bind;
    if (prev == AIR && b != AIR) t.cells.push_back({ x, y, z });
    if (prev != AIR && b == AIR) {
        for (size_t k = 0; k < t.cells.size(); k++) {
            if (t.cells[k].x == x && t.cells[k].y == y && t.cells[k].z == z) {
                t.cells[k] = t.cells.back();
                t.cells.pop_back();
                break;
            }
        }
    }
    t.meshDirty = true;
}

struct Sym3 {
    float xx = 1, yy = 1, zz = 1, xy = 0, xz = 0, yz = 0;
};

Vec3 mulSym(const Sym3& A, const Vec3& v) {
    return {
        A.xx * v.x + A.xy * v.y + A.xz * v.z,
        A.xy * v.x + A.yy * v.y + A.yz * v.z,
        A.xz * v.x + A.yz * v.y + A.zz * v.z
    };
}

bool invertSym(const Sym3& A, Sym3& out) {
    float c00 = A.yy * A.zz - A.yz * A.yz;
    float c01 = A.xz * A.yz - A.xy * A.zz;
    float c02 = A.xy * A.yz - A.xz * A.yy;
    float det = A.xx * c00 + A.xy * c01 + A.xz * c02;
    if (std::fabs(det) < 1e-12f) return false;
    float s = 1.0f / det;
    out.xx = c00 * s;
    out.xy = c01 * s;
    out.xz = c02 * s;
    out.yy = (A.xx * A.zz - A.xz * A.xz) * s;
    out.yz = (A.xy * A.xz - A.xx * A.yz) * s;
    out.zz = (A.xx * A.yy - A.xy * A.xy) * s;
    return true;
}

struct Mat3 {
    float a[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    Vec3 mul(const Vec3& v) const {
        return {
            a[0] * v.x + a[3] * v.y + a[6] * v.z,
            a[1] * v.x + a[4] * v.y + a[7] * v.z,
            a[2] * v.x + a[5] * v.y + a[8] * v.z
        };
    }
};

bool invertMat3(Mat3 m, Mat3& out) {
    float* a = m.a;
    float c0 = a[4] * a[8] - a[7] * a[5];
    float c1 = a[7] * a[2] - a[1] * a[8];
    float c2 = a[1] * a[5] - a[4] * a[2];
    float det = a[0] * c0 + a[3] * c1 + a[6] * c2;
    if (std::fabs(det) < 1e-12f) return false;
    float s = 1.0f / det;
    out.a[0] = c0 * s;
    out.a[1] = c1 * s;
    out.a[2] = c2 * s;
    out.a[3] = (a[6] * a[5] - a[3] * a[8]) * s;
    out.a[4] = (a[0] * a[8] - a[6] * a[2]) * s;
    out.a[5] = (a[3] * a[2] - a[0] * a[5]) * s;
    out.a[6] = (a[3] * a[7] - a[6] * a[4]) * s;
    out.a[7] = (a[6] * a[1] - a[0] * a[7]) * s;
    out.a[8] = (a[0] * a[4] - a[3] * a[1]) * s;
    return true;
}

Sym3 restI(const PhysicsIsland& t) {
    return { t.Ixx, t.Iyy, t.Izz, t.Ixy, t.Ixz, t.Iyz };
}
Sym3 restInvI(const PhysicsIsland& t) {
    return { t.ixx, t.iyy, t.izz, t.ixy, t.ixz, t.iyz };
}

Vec3 worldIinv(const PhysicsIsland& t, const Vec3& v) {
    return rotate(t, mulSym(restInvI(t), unrotate(t, v)));
}
Vec3 worldI(const PhysicsIsland& t, const Vec3& v) {
    return rotate(t, mulSym(restI(t), unrotate(t, v)));
}

void applyImpulse(PhysicsIsland& t, const Vec3& r, const Vec3& J) {
    float invM = 1.0f / std::max(t.mass, 1e-4f);
    t.vel += J * invM;
    t.omega += worldIinv(t, r.cross(J));
}

float keffAlong(const PhysicsIsland& t, const Vec3& r, const Vec3& n) {
    Vec3 rxn = r.cross(n);
    return 1.0f / std::max(t.mass, 1e-4f) + rxn.dot(worldIinv(t, rxn));
}

void applyDeltaSpin(PhysicsIsland& t, const Vec3& dTheta) {
    t.ax += dTheta.cross(t.ax);
    t.ay += dTheta.cross(t.ay);
    orthonormalize(t);
}

void solvePoint(PhysicsIsland& t, const Vec3& r, const Vec3& wantVp, bool doVel, float posW) {
    Mat3 K;
    float invM = 1.0f / std::max(t.mass, 1e-4f);
    Vec3 e[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int c = 0; c < 3; c++) {
        Vec3 dw = worldIinv(t, r.cross(e[c]));
        Vec3 col = e[c] * invM + dw.cross(r);
        K.a[c * 3 + 0] = col.x;
        K.a[c * 3 + 1] = col.y;
        K.a[c * 3 + 2] = col.z;
    }
    Mat3 invK;
    if (!invertMat3(K, invK)) return;
    if (doVel) {
        Vec3 vp = t.vel + t.omega.cross(r);
        applyImpulse(t, r, invK.mul(wantVp - vp));
    }
    if (posW > 0.0f) {
        Vec3 p = t.com + r;
        Vec3 err = t.pivotRest - p;
        Vec3 lam = invK.mul(err * posW);
        t.com += lam * invM;
        applyDeltaSpin(t, worldIinv(t, r.cross(lam)));
    }
}

void recomputeMass(PhysicsIsland& t) {
    const float S = cfg::BLOCK_SCALE;
    Vec3 sum{ 0, 0, 0 };
    float mass = 0.0f;
    for (const IVec3& c : t.cells) {
        uint8_t b = t.get(c.x, c.y, c.z);
        if (b == AIR) continue;
        float m = voxelMass(b);
        sum += t.restCenter(c.x, c.y, c.z) * m;
        mass += m;
    }
    if (mass < 1e-4f) {
        t.mass = 0.0f;
        t.inertia = 0.18f;
        t.Ixx = t.Iyy = t.Izz = 0.18f;
        t.Ixy = t.Ixz = t.Iyz = 0;
        t.ixx = t.iyy = t.izz = 1.0f / 0.18f;
        t.ixy = t.ixz = t.iyz = 0;
        return;
    }
    Vec3 newRest = sum / mass;
    t.com = t.com + rotate(t, newRest - t.restCom);
    t.restCom = newRest;
    t.mass = mass;
    Sym3 I{};
    const float cube = S * S / 6.0f;
    for (const IVec3& c : t.cells) {
        uint8_t b = t.get(c.x, c.y, c.z);
        if (b == AIR) continue;
        float m = voxelMass(b);
        Vec3 d = t.restCenter(c.x, c.y, c.z) - t.restCom;
        I.xx += m * (d.y * d.y + d.z * d.z + cube);
        I.yy += m * (d.x * d.x + d.z * d.z + cube);
        I.zz += m * (d.x * d.x + d.y * d.y + cube);
        I.xy -= m * d.x * d.y;
        I.xz -= m * d.x * d.z;
        I.yz -= m * d.y * d.z;
    }
    I.xx = std::max(I.xx, 0.05f);
    I.yy = std::max(I.yy, 0.05f);
    I.zz = std::max(I.zz, 0.05f);
    t.Ixx = I.xx; t.Iyy = I.yy; t.Izz = I.zz;
    t.Ixy = I.xy; t.Ixz = I.xz; t.Iyz = I.yz;
    t.inertia = (I.xx + I.yy + I.zz) / 3.0f;
    Sym3 inv;
    if (!invertSym(I, inv)) {
        inv.xx = 1.0f / I.xx;
        inv.yy = 1.0f / I.yy;
        inv.zz = 1.0f / I.zz;
        inv.xy = inv.xz = inv.yz = 0;
    }
    t.ixx = inv.xx; t.iyy = inv.yy; t.izz = inv.zz;
    t.ixy = inv.xy; t.ixz = inv.xz; t.iyz = inv.yz;
}

void hingeAt(PhysicsIsland& t, const Vec3& worldCut) {
    t.pivotRest = worldCut;
    t.holdPivot = true;
    t.vel = { 0, 0, 0 };
    t.omega = { 0, 0, 0 };
    Vec3 away{ t.restCom.x - worldCut.x, 0.0f, t.restCom.z - worldCut.z };
    if (away.lengthSq() < 1e-6f) away = { 1, 0, 0 };
    away = away.normalized();
    Vec3 axis = away.cross(Vec3{ 0, 1, 0 });
    if (axis.lengthSq() < 1e-8f) axis = { 1, 0, 0 };
    axis = axis.normalized();
    const float lean = 3.0f * kDeg2Rad;
    float c = std::cos(lean), s = std::sin(lean);
    auto spin = [&](const Vec3& v) {
        return v * c + axis.cross(v) * s + axis * (axis.dot(v) * (1.0f - c));
    };
    t.ax = spin(t.ax);
    t.ay = spin(t.ay);
    t.az = spin(t.az);
    orthonormalize(t);
    Vec3 r = rotate(t, t.restCom - t.pivotRest);
    t.com = t.pivotRest + r;
}

std::vector<std::vector<IVec3>> partitionByWoodCore(
    const std::vector<IVec3>& cells, const std::vector<uint8_t>& kinds)
{
    std::vector<std::vector<IVec3>> out;
    std::unordered_map<IVec3, uint8_t, IVec3Hash> kind;
    kind.reserve(cells.size());
    for (size_t i = 0; i < cells.size(); i++) {
        uint8_t b = (i < kinds.size()) ? kinds[i] : (uint8_t)AIR;
        if (b == AIR) continue;
        kind[cells[i]] = b;
    }
    if (kind.empty()) return out;

    auto isCore = [](uint8_t b) { return b != AIR && !isCompressible(b); };

    std::vector<IVec3> cores;
    std::vector<IVec3> leafs;
    cores.reserve(kind.size());
    leafs.reserve(kind.size());
    for (const auto& kv : kind) {
        if (isCore(kv.second)) cores.push_back(kv.first);
        else leafs.push_back(kv.first);
    }

    std::unordered_map<IVec3, int, IVec3Hash> owner;
    std::vector<std::vector<IVec3>> comps;

    struct Stem {
        int x = 0, z = 0, y = 0;
    };
    std::vector<Stem> bases;
    if (!cores.empty()) {
        int minY = cores[0].y;
        for (const IVec3& c : cores) minY = std::min(minY, c.y);
        std::unordered_map<int, Stem> col;
        auto colKey = [](int x, int z) { return (x + 4096) * 8192 + (z + 4096); };
        for (const IVec3& c : cores) {
            if (c.y > minY + 1) continue;
            int k = colKey(c.x, c.z);
            auto it = col.find(k);
            if (it == col.end() || c.y < it->second.y)
                col[k] = Stem{ c.x, c.z, c.y };
        }
        bases.reserve(col.size());
        for (const auto& kv : col) bases.push_back(kv.second);

        std::vector<int> uf((int)bases.size());
        for (int i = 0; i < (int)bases.size(); i++) uf[(size_t)i] = i;
        auto find = [&](int i) {
            while (uf[(size_t)i] != i) {
                uf[(size_t)i] = uf[(size_t)uf[(size_t)i]];
                i = uf[(size_t)i];
            }
            return i;
        };
        auto unite = [&](int a, int b) {
            a = find(a); b = find(b);
            if (a != b) uf[(size_t)a] = b;
        };
        auto idxAt = [&](int x, int z) {
            for (int i = 0; i < (int)bases.size(); i++)
                if (bases[(size_t)i].x == x && bases[(size_t)i].z == z) return i;
            return -1;
        };
        for (const Stem& s : bases) {
            for (int z0 = s.z - 1; z0 <= s.z; z0++) {
                for (int x0 = s.x - 1; x0 <= s.x; x0++) {
                    int ids[4], n = 0;
                    for (int dz = 0; dz <= 1; dz++)
                        for (int dx = 0; dx <= 1; dx++) {
                            int id = idxAt(x0 + dx, z0 + dz);
                            if (id >= 0) ids[n++] = id;
                        }
                    if (n >= 3) {
                        for (int i = 1; i < n; i++) unite(ids[0], ids[i]);
                    }
                }
            }
        }
        std::unordered_map<int, int> rootId;
        for (int i = 0; i < (int)bases.size(); i++) {
            int r = find(i);
            if (!rootId.count(r)) {
                int nid = (int)comps.size();
                rootId[r] = nid;
                comps.push_back({});
            }
        }
        for (const IVec3& c : cores) {
            int best = 0;
            int bestD = 1 << 30;
            for (int i = 0; i < (int)bases.size(); i++) {
                int dx = c.x - bases[(size_t)i].x;
                int dz = c.z - bases[(size_t)i].z;
                int dy = c.y - bases[(size_t)i].y;
                int d = dx * dx * 4 + dz * dz * 4 + dy * dy;
                if (d < bestD) {
                    bestD = d;
                    best = i;
                }
            }
            int id = rootId[find(best)];
            owner[c] = id;
            comps[(size_t)id].push_back(c);
        }
    }

    std::queue<IVec3> lq;
    std::unordered_set<IVec3, IVec3Hash> seenLeaf;
    for (const IVec3& c : cores) {
        lq.push(c);
        seenLeaf.insert(c);
    }
    while (!lq.empty()) {
        IVec3 p = lq.front();
        lq.pop();
        auto ow = owner.find(p);
        if (ow == owner.end()) continue;
        int id = ow->second;
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    IVec3 n{ p.x + dx, p.y + dy, p.z + dz };
                    if (seenLeaf.count(n)) continue;
                    auto it = kind.find(n);
                    if (it == kind.end() || isCore(it->second)) continue;
                    seenLeaf.insert(n);
                    owner[n] = id;
                    comps[(size_t)id].push_back(n);
                    lq.push(n);
                }
    }

    for (const IVec3& start : leafs) {
        if (owner.count(start)) continue;
        int id = (int)comps.size();
        std::vector<IVec3> comp;
        std::queue<IVec3> q;
        q.push(start);
        owner[start] = id;
        while (!q.empty()) {
            IVec3 p = q.front();
            q.pop();
            comp.push_back(p);
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0 && dz == 0) continue;
                        IVec3 n{ p.x + dx, p.y + dy, p.z + dz };
                        if (owner.count(n)) continue;
                        auto it = kind.find(n);
                        if (it == kind.end() || isCore(it->second)) continue;
                        owner[n] = id;
                        q.push(n);
                    }
        }
        comps.push_back(std::move(comp));
    }

    for (auto& c : comps) {
        if (!c.empty()) out.push_back(std::move(c));
    }
    return out;
}

void buildMesh(PhysicsIsland& t) {
    t.meshOpaque.clear();
    for (const IVec3& c : t.cells) {
        uint8_t b = t.get(c.x, c.y, c.z);
        if (b == AIR || t.hiddenAt(c.x, c.y, c.z)) continue;
        const BlockInfo& info = blockOf(b);
        uint8_t fl = t.flagAt(c.x, c.y, c.z);
        int trunk = 1;
        auto isWood = [&](int ix, int iy, int iz) {
            return t.inBounds(ix, iy, iz) && isTreeWood(t.get(ix, iy, iz));
        };
        if (b == LOG) trunk = logTrunkAxis(c.x, c.y, c.z, fl, isWood);
        for (int f = 0; f < 6; f++) {
            const geo::FaceDef& F = geo::kFaces[f];
            int nx = c.x + F.n[0], ny = c.y + F.n[1], nz = c.z + F.n[2];
            uint8_t nb = t.inBounds(nx, ny, nz) ? t.get(nx, ny, nz) : (uint8_t)AIR;
            bool neighHide = t.inBounds(nx, ny, nz) && t.hiddenAt(nx, ny, nz);
            bool vis = (nb == AIR) || neighHide || !blockOf(nb).opaque;
            if (nb == b && !isPassableCutout(b) && !neighHide) vis = false;
            if (!vis) continue;

            int qa = 0, qb = 0, longAxis = 0;
            int spliceKind = 0;
            if (b == LOG) {
                auto hasCut = [&](int ix, int iy, int iz, int fc) {
                    return t.inBounds(ix, iy, iz) && hasCutFace(t.flagAt(ix, iy, iz), fc);
                };
                spliceKind = spliceEndRing(c.x, c.y, c.z, f, trunk, qa, qb, longAxis, isWood, hasCut);
            }
            LogFaceTex lf = (b == LOG)
                ? logFaceTex(fl, f, trunk, spliceKind, qa, qb, longAxis)
                : LogFaceTex{};
            if (b != LOG)
                lf.tile = (f == 0) ? info.texTop : (f == 1 ? info.texBottom : info.texSide);
            Vertex vv[4];
            for (int k = 0; k < 4; k++) {
                float px = (float)c.x + F.p[k][0];
                float py = (float)c.y + F.p[k][1];
                float pz = (float)c.z + F.p[k][2];
                float u, v;
                if (b == LOG) logCornerUV(lf, F.t[k][0], F.t[k][1],
                                         F.p[k][0], F.p[k][1], F.p[k][2], u, v);
                else {
                    float u0, v0, u1, v1;
                    tex::tileUV(lf.tile, u0, v0, u1, v1);
                    u = u0 + (u1 - u0) * F.t[k][0];
                    v = v0 + (v1 - v0) * F.t[k][1];
                }
                vv[k] = { px, py, pz, u, v, (float)F.n[0], (float)F.n[1], (float)F.n[2],
                          F.shade, 1.0f, 1.0f };
            }
            t.meshOpaque.push_back(vv[0]); t.meshOpaque.push_back(vv[1]); t.meshOpaque.push_back(vv[2]);
            t.meshOpaque.push_back(vv[0]); t.meshOpaque.push_back(vv[2]); t.meshOpaque.push_back(vv[3]);
        }
    }
    t.meshDirty = false;
}

void step(PhysicsIsland& t, World& w, float dt) {
    if (t.cells.empty() || t.mass < 1e-4f || dt < 1e-6f) return;
    const float S = cfg::BLOCK_SCALE;
    const float rWood = 0.42f * S;
    const float rLeaf = 0.48f * S;
    const float pivotR2 = (1.8f * S) * (1.8f * S);

    t.vel.y -= cfg::GRAVITY * dt;
    Vec3 Iomega = worldI(t, t.omega);
    t.omega += worldIinv(t, Iomega.cross(t.omega) * -1.0f) * dt;

    auto nearPivot = [&](const Vec3& c) {
        if (!t.holdPivot) return false;
        float dx = c.x - t.pivotRest.x, dy = c.y - t.pivotRest.y, dz = c.z - t.pivotRest.z;
        return (dx * dx + dz * dz) < pivotR2 && std::fabs(dy) < 2.2f * S;
    };

    struct Hit {
        Vec3 r, n;
        float pen = 0.0f;
        float jn = 0.0f;
    };
    std::vector<Hit> woods;
    bool grounded = false;
    bool woodFar = false;
    bool hideChanged = false;
    float leafMass = 0.0f;

    for (const IVec3& cell : t.cells) {
        uint8_t b = t.get(cell.x, cell.y, cell.z);
        if (b == AIR) continue;
        Vec3 r = rotate(t, t.restCenter(cell.x, cell.y, cell.z) - t.restCom);
        Vec3 c = t.com + r;
        bool leaf = isCompressible(b);
        std::vector<Contact> raw;
        gatherContacts(w, c, leaf ? rLeaf : rWood, raw);

        if (leaf) {
            leafMass += voxelMass(b);
            PhysCellState& st = t.stateAt(cell.x, cell.y, cell.z);
            const float mLeaf = std::max(0.01f, blockWeight(b));
            float maxPen = 0.0f;
            Vec3 nSum{ 0, 0, 0 };
            for (const Contact& h : raw) {
                if (h.pen > maxPen) maxPen = h.pen;
                nSum += h.n * h.pen;
            }
            if (maxPen > 0.0f) {
                float comp = std::min(1.0f, maxPen / (rLeaf + 1e-5f));
                if (comp > st.compress) st.compress = comp;
                else st.compress = st.compress * 0.7f + comp * 0.3f;
                Vec3 n = nSum.lengthSq() > 1e-8f ? nSum.normalized() : Vec3{ 0, 1, 0 };
                st.expandDir = n;
                st.elastic = 0.0f;
                Vec3 vAt = t.vel + t.omega.cross(r);
                float vn = vAt.dot(n);
                float k = 90.0f * mLeaf;
                float Fn = k * maxPen - 6.0f * mLeaf * std::min(vn, 0.0f);
                float Fmax = 8.0f * mLeaf * cfg::GRAVITY;
                if (Fn > Fmax) Fn = Fmax;
                if (Fn > 0.0f) applyImpulse(t, r, n * (Fn * dt));
                if (n.y > 0.25f) grounded = true;
                bool wantHide = st.compress >= cfg::LEAF_HIDE_COMPRESS;
                int i = t.index(cell.x, cell.y, cell.z);
                bool wasHide = (t.flags[(size_t)i] & FLAG_HIDDEN) != 0;
                if (wantHide && !wasHide) {
                    t.flags[(size_t)i] |= FLAG_HIDDEN;
                    hideChanged = true;
                }
            } else {
                st.compress *= 0.82f;
                st.elastic = 0.0f;
                if (st.compress < cfg::LEAF_HIDE_COMPRESS * 0.55f) {
                    int i = t.index(cell.x, cell.y, cell.z);
                    if (t.flags[(size_t)i] & FLAG_HIDDEN) {
                        t.flags[(size_t)i] = (uint8_t)(t.flags[(size_t)i] & ~FLAG_HIDDEN);
                        hideChanged = true;
                    }
                }
                if (st.compress < 0.04f) st.compress = 0.0f;
            }
            continue;
        }

        if (t.hiddenAt(cell.x, cell.y, cell.z)) continue;
        for (const Contact& h : raw) {
            if (h.n.y > 0.25f) grounded = true;
            if (nearPivot(c)) continue;
            woodFar = true;
            woods.push_back(Hit{ r, h.n, h.pen, 0.0f });
        }
    }
    if (hideChanged) t.meshDirty = true;

    if (leafMass > 0.01f) {
        float sp = t.vel.length();
        if (sp > 1e-4f)
            t.vel -= t.vel * ((0.06f * leafMass * sp / t.mass) * dt);
        float w = t.omega.length();
        if (w > 1e-4f)
            t.omega -= t.omega * ((0.04f * leafMass / t.mass) * w * dt);
    }

    if (woodFar || t.ay.y < 0.62f) t.holdPivot = false;

    if (t.holdPivot) {
        Vec3 r = rotate(t, t.pivotRest - t.restCom);
        solvePoint(t, r, Vec3{ 0, 0, 0 }, true, 0.0f);
    }

    for (int it = 0; it < 10; it++) {
        for (Hit& h : woods) {
            Vec3 vAt = t.vel + t.omega.cross(h.r);
            float vn = vAt.dot(h.n);
            if (vn < 0.0f) {
                float ke = keffAlong(t, h.r, h.n);
                if (ke > 1e-8f) {
                    float jn = -vn / ke;
                    applyImpulse(t, h.r, h.n * jn);
                    h.jn += jn;
                }
            }
            vAt = t.vel + t.omega.cross(h.r);
            vn = vAt.dot(h.n);
            Vec3 vt = vAt - h.n * vn;
            float vtLen = vt.length();
            if (vtLen < 1e-6f) continue;
            Vec3 tang = vt * (1.0f / vtLen);
            float kt = keffAlong(t, h.r, tang);
            if (kt < 1e-8f) continue;
            float jt = -vtLen / kt;
            float lim = cfg::WOOD_FRICTION * std::max(h.jn, 0.0f);
            if (jt > lim) jt = lim;
            else if (jt < -lim) jt = -lim;
            applyImpulse(t, h.r, tang * jt);
        }
    }

    t.com += t.vel * dt;
    integrateRotation(t, dt);

    if (t.holdPivot) {
        Vec3 r = rotate(t, t.pivotRest - t.restCom);
        solvePoint(t, r, Vec3{ 0, 0, 0 }, true, 0.85f);
    }

    for (int it = 0; it < 2; it++) {
        for (const Hit& h : woods) {
            float slop = 0.003f;
            float pen = h.pen - slop;
            if (pen <= 0.0f) continue;
            float ke = keffAlong(t, h.r, h.n);
            if (ke < 1e-8f) continue;
            float lam = (pen * 0.18f) / ke;
            float invM = 1.0f / t.mass;
            t.com += h.n * (lam * invM);
            applyDeltaSpin(t, worldIinv(t, h.r.cross(h.n * lam)));
        }
    }

    const float maxW = 25.0f;
    if (t.omega.length() > maxW) t.omega *= maxW / t.omega.length();
    const float maxV = 40.0f;
    if (t.vel.length() > maxV) t.vel *= maxV / t.vel.length();

    bool lying = std::fabs(t.ay.y) < 0.52f;
    bool slow = t.vel.length() < 0.70f && t.omega.length() < 0.65f;
    if (grounded && slow) {
        t.stillTime += lying ? dt : dt * 0.45f;
    } else if (grounded) {
        t.stillTime += dt * 0.12f;
    } else {
        t.stillTime *= 0.72f;
    }
}

bool gameTick(PhysicsIsland& t) {
    bool destroyed = false;
    for (size_t i = 0; i < t.cells.size();) {
        IVec3 c = t.cells[i];
        uint8_t b = t.get(c.x, c.y, c.z);
        if (isCompressible(b) && t.hiddenAt(c.x, c.y, c.z)) {
            PhysCellState& st = t.stateAt(c.x, c.y, c.z);
            st.crushTicks++;
            if (st.crushTicks >= cfg::LEAF_CRUSH_TICKS) {
                setCell(t, c.x, c.y, c.z, AIR, 0);
                destroyed = true;
                continue;
            }
        } else if (t.inBounds(c.x, c.y, c.z)) {
            t.stateAt(c.x, c.y, c.z).crushTicks = 0;
        }
        i++;
    }
    if (destroyed) recomputeMass(t);
    return destroyed;
}

PhysicsIsland extract(const PhysicsIsland& src, const std::vector<IVec3>& localCells) {
    PhysicsIsland t;
    if (localCells.empty()) return t;
    int minx = localCells[0].x, maxx = minx;
    int miny = localCells[0].y, maxy = miny;
    int minz = localCells[0].z, maxz = minz;
    for (const IVec3& c : localCells) {
        minx = std::min(minx, c.x); maxx = std::max(maxx, c.x);
        miny = std::min(miny, c.y); maxy = std::max(maxy, c.y);
        minz = std::min(minz, c.z); maxz = std::max(maxz, c.z);
    }
    allocate(t, maxx - minx + 1, maxy - miny + 1, maxz - minz + 1);
    t.originX = src.originX + minx;
    t.originY = src.originY + miny;
    t.originZ = src.originZ + minz;
    t.ax = src.ax; t.ay = src.ay; t.az = src.az;
    t.omega = src.omega;
    t.vel = src.vel;
    t.com = src.com;
    t.restCom = src.restCom;
    for (const IVec3& c : localCells) {
        uint8_t b = src.get(c.x, c.y, c.z);
        if (b == AIR) continue;
        int lx = c.x - minx, ly = c.y - miny, lz = c.z - minz;
        setCell(t, lx, ly, lz, b, src.flagAt(c.x, c.y, c.z), src.treeIdAt(c.x, c.y, c.z));
        t.stateAt(lx, ly, lz) = src.stateAt(c.x, c.y, c.z);
    }
    recomputeMass(t);
    Vec3 r = rotate(src, t.restCom - src.restCom);
    t.vel = src.vel + src.omega.cross(r);
    t.stillTime = 0.0f;
    t.holdPivot = false;
    t.meshDirty = true;
    return t;
}

} // namespace tree_fall
