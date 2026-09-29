#pragma once
#include "blocks.hpp"
#include "../core/math.hpp"
#include <vector>

// Extra per-cell state for compressible leaves inside a physics island.
struct PhysCellState {
    float compress = 0.0f;
    float elastic = 0.0f;
    Vec3 expandDir{ 0, 1, 0 };
    int crushTicks = 0;
};

// A fallen tree lives in its own volume, not the loaded world map.
// Blocks are stored in local chunk coordinates; a rigid transform maps them
// back to the visible world pose for drawing, collision, and interaction.
struct PhysicsIsland {
    int sx = 0, sy = 0, sz = 0;
    int originX = 0, originY = 0, originZ = 0; // rest-pose world cell of local (0,0,0)
    std::vector<uint8_t> blocks;
    std::vector<uint8_t> flags;
    std::vector<uint32_t> treeIds; // living-tree bind copied from the world
    std::vector<PhysCellState> extra;
    std::vector<IVec3> cells; // local coords of non-air voxels

    Vec3 restCom{ 0, 0, 0 };
    Vec3 com{ 0, 0, 0 };
    Vec3 vel{ 0, 0, 0 };
    Vec3 omega{ 0, 0, 0 };
    Vec3 ax{ 1, 0, 0 }, ay{ 0, 1, 0 }, az{ 0, 0, 1 };
    Vec3 pivotRest{ 0, 0, 0 }; // world-space cut; spherical joint until the crown hits
    bool holdPivot = false;
    float mass = 1.0f;
    float inertia = 1.0f; // (Ixx+Iyy+Izz)/3, leftover scalar
    // Rest-pose inertia about COM, and its inverse (world = R I R^T).
    float Ixx = 1, Iyy = 1, Izz = 1, Ixy = 0, Ixz = 0, Iyz = 0;
    float ixx = 1, iyy = 1, izz = 1, ixy = 0, ixz = 0, iyz = 0;
    float stillTime = 0.0f;
    bool meshDirty = true;
    std::vector<Vertex> meshOpaque;
    uint32_t netId = 0;
    uint32_t contentRev = 0;

    int index(int x, int y, int z) const { return (y * sz + z) * sx + x; }
    bool inBounds(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < sx && y < sy && z < sz;
    }
    uint8_t get(int x, int y, int z) const {
        if (!inBounds(x, y, z) || blocks.empty()) return AIR;
        return blocks[(size_t)index(x, y, z)];
    }
    uint8_t flagAt(int x, int y, int z) const {
        if (!inBounds(x, y, z) || flags.empty()) return 0;
        return flags[(size_t)index(x, y, z)];
    }
    uint32_t treeIdAt(int x, int y, int z) const {
        if (!inBounds(x, y, z) || treeIds.empty()) return 0;
        return treeIds[(size_t)index(x, y, z)];
    }
    bool hiddenAt(int x, int y, int z) const {
        return (flagAt(x, y, z) & FLAG_HIDDEN) != 0;
    }
    PhysCellState& stateAt(int x, int y, int z) { return extra[(size_t)index(x, y, z)]; }
    const PhysCellState& stateAt(int x, int y, int z) const { return extra[(size_t)index(x, y, z)]; }

    Vec3 restCenter(int lx, int ly, int lz) const {
        const float S = cfg::BLOCK_SCALE;
        return {
            ((float)(originX + lx) + 0.5f) * S,
            ((float)(originY + ly) + 0.5f) * S,
            ((float)(originZ + lz) + 0.5f) * S
        };
    }
};

class World;

namespace tree_fall {
inline Vec3 rotate(const PhysicsIsland& t, const Vec3& local) {
    return t.ax * local.x + t.ay * local.y + t.az * local.z;
}
inline Vec3 unrotate(const PhysicsIsland& t, const Vec3& world) {
    return { t.ax.dot(world), t.ay.dot(world), t.az.dot(world) };
}
inline Vec3 worldOf(const PhysicsIsland& t, int lx, int ly, int lz) {
    return t.com + rotate(t, t.restCenter(lx, ly, lz) - t.restCom);
}
inline Vec3 restOf(const PhysicsIsland& t, const Vec3& world) {
    return t.restCom + unrotate(t, world - t.com);
}

void allocate(PhysicsIsland& t, int sx, int sy, int sz);
void setCell(PhysicsIsland& t, int x, int y, int z, uint8_t b, uint8_t fl = 0, uint32_t bind = 0);
void recomputeMass(PhysicsIsland& t);
void hingeAt(PhysicsIsland& t, const Vec3& worldCut);
void rebuildOccupied(PhysicsIsland& t);
void buildMesh(PhysicsIsland& t);
void step(PhysicsIsland& t, World& w, float dt);
bool gameTick(PhysicsIsland& t); // true if any cell was destroyed
PhysicsIsland extract(const PhysicsIsland& src, const std::vector<IVec3>& localCells);
// Separate trees by trunk bases (2x2 thick = one tree). Canopy wood/leaves that
// only touch another tree stay with the nearer trunk. Leaf-only clumps stay apart.
// Callers should already filter by living-tree bind id when one is available.
std::vector<std::vector<IVec3>> partitionByWoodCore(
    const std::vector<IVec3>& cells, const std::vector<uint8_t>& kinds);
}