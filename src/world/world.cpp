#include "world.hpp"
#include "drop_geom.hpp"
#include "saves.hpp"
#include "tree_canopy.hpp"
#include "tree_grow.hpp"
#include "log_appear.hpp"
#include "matchmap.hpp"
#include "structure.hpp"
#include "../core/noise.hpp"
#include "../render/textures.hpp"
#include "../render/block_geo.hpp"
#include "../material/registry.hpp"
#include "../material/blocks/grass_tuft_mat.hpp"
#include "../plugin/plugin.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <queue>
#include <unordered_set>
#include <utility>

namespace {

static_assert(LOG_AXIS_X == (uint8_t)(cfg::WATER_MAX_LEVEL + 1), "log axis overlaps water");
static_assert(LOG_AXIS_Z == (uint8_t)(cfg::WATER_MAX_LEVEL + 2), "log axis overlaps water");

uint32_t fnv1a(const uint8_t* d, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

// Sod (草皮) grows on the top face and the four side faces of dirt — never the bottom.
constexpr int SOD_FACE_COUNT = 5;
constexpr int SOD_FACES[SOD_FACE_COUNT] = { 0, 2, 3, 4, 5 };

// "Surface" dirt = dirt with a see-through block above (air/water/leaves/etc.).
// Only surface dirt grows sod; deep cave/cliff dirt stays bare even when its side
// face happens to touch air.
static bool isSurfaceDirt(const World::Chunk& ch, int x, int y, int z, int aboveBlock) {
    if (ch.get(x, y, z) != DIRT) return false;
    return !isOpaque((uint8_t)aboveBlock);
}

struct CellLoc {
    int cx = 0, cy = 0, cz = 0, lx = 0, ly = 0, lz = 0;
};

static bool cellLoc(int x, int y, int z, CellLoc& o) {
    if (y < 0 || y >= cfg::WORLD_H) return false;
    o.cx = floorDiv(x, cfg::CHUNK_X);
    o.cy = floorDiv(y, cfg::CHUNK_Y);
    o.cz = floorDiv(z, cfg::CHUNK_Z);
    o.lx = x - o.cx * cfg::CHUNK_X;
    o.ly = y - o.cy * cfg::CHUNK_Y;
    o.lz = z - o.cz * cfg::CHUNK_Z;
    return true;
}

// Face id whose normal points along (axis, sign): +Y=0, -Y=1, +X=2, -X=3, +Z=4, -Z=5.
static int faceForNormal(int axis, int sign) {
    if (axis == 1) return sign > 0 ? 0 : 1;
    if (axis == 0) return sign > 0 ? 2 : 3;
    return sign > 0 ? 4 : 5;
}

bool trialColumnOverlaps(int cx, int cz) {
    int x0 = cx * cfg::CHUNK_X;
    int z0 = cz * cfg::CHUNK_Z;
    int x1 = x0 + cfg::CHUNK_X;
    int z1 = z0 + cfg::CHUNK_Z;
    return x1 > structure::kTrialX0 && x0 < structure::kTrialX0 + structure::kTrialSpan
        && z1 > structure::kTrialZ0 && z0 < structure::kTrialZ0 + structure::kTrialSpan;
}

} // namespace

uint8_t World::getBlock(int x, int y, int z) const {
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return AIR;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return AIR;
    return it->second.get(c.lx, c.ly, c.lz);
}

uint8_t World::getFlags(int x, int y, int z) const {
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return 0;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return 0;
    if (it->second.flags.size() != (size_t)cfg::CHUNK_VOLUME) return 0;
    return it->second.flagAt(c.lx, c.ly, c.lz);
}

bool World::isAlive(int x, int y, int z) const {
    return (getFlags(x, y, z) & FLAG_ALIVE) != 0;
}

uint32_t World::getTreeId(int x, int y, int z) const {
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return 0;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return 0;
    return it->second.treeIdAt(c.lx, c.ly, c.lz);
}

uint8_t World::getWaterLevel(int x, int y, int z) const {
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return 0;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return 0;
    if (it->second.get(c.lx, c.ly, c.lz) != WATER) return 0;
    return it->second.levelAt(c.lx, c.ly, c.lz);
}

int World::logAxisAt(int x, int y, int z) const {
    uint8_t b = getBlock(x, y, z);
    if (!isOrientedWood(b)) return 1;
    uint8_t fl = getFlags(x, y, z);
    if (b == LOG && (fl & (FLAG_ALIVE | FLAG_SETTLED)) != 0) return 1;
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return 1;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return 1;
    return logAxisFromLevel(it->second.levelAt(c.lx, c.ly, c.lz));
}

int World::humidityAt(int x, int y, int z) const {
    return airHumidity(x, y, z);
}

World::Chunk* World::getChunk(int cx, int cy, int cz) {
    auto it = m_chunks.find(chunkKey(cx, cy, cz));
    return it == m_chunks.end() ? nullptr : &it->second;
}

namespace {

bool chunkEmits(World::Chunk& ch) {
    if (ch.emissionKnown) return ch.hasEmission;
    ch.emissionKnown = true;
    ch.hasEmission = false;
    for (uint8_t b : ch.blocks) {
        if (blockEmission(b) > 0) {
            ch.hasEmission = true;
            break;
        }
    }
    return ch.hasEmission;
}

// True when a light source sits within BLOCK_LIGHT_MAX of the edited cell,
// so the edit can change the flood (the source itself, or a solid that blocks it).
bool emissionNear(World& world, int x, int y, int z) {
    constexpr int R = BLOCK_LIGHT_MAX;
    const int cx0 = floorDiv(x - R, cfg::CHUNK_X);
    const int cx1 = floorDiv(x + R, cfg::CHUNK_X);
    const int cy0 = floorDiv(y - R, cfg::CHUNK_Y);
    const int cy1 = floorDiv(y + R, cfg::CHUNK_Y);
    const int cz0 = floorDiv(z - R, cfg::CHUNK_Z);
    const int cz1 = floorDiv(z + R, cfg::CHUNK_Z);
    const int ncx = cx1 - cx0 + 1;
    const int ncy = cy1 - cy0 + 1;
    const int ncz = cz1 - cz0 + 1;
    std::vector<World::Chunk*> grid((size_t)ncx * ncy * ncz, nullptr);
    auto slot = [&](int cx, int cy, int cz) -> World::Chunk*& {
        return grid[(size_t)((cy - cy0) * ncz + (cz - cz0)) * ncx + (cx - cx0)];
    };
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cx = cx0; cx <= cx1; ++cx)
                slot(cx, cy, cz) = world.getChunk(cx, cy, cz);

    for (int wy = y - R; wy <= y + R; ++wy) {
        if (wy < 0 || wy >= cfg::WORLD_H) continue;
        int cy = floorDiv(wy, cfg::CHUNK_Y);
        int ly = wy - cy * cfg::CHUNK_Y;
        for (int wz = z - R; wz <= z + R; ++wz) {
            int cz = floorDiv(wz, cfg::CHUNK_Z);
            int lz = wz - cz * cfg::CHUNK_Z;
            for (int wx = x - R; wx <= x + R; ++wx) {
                int cx = floorDiv(wx, cfg::CHUNK_X);
                World::Chunk* ch = slot(cx, cy, cz);
                if (!ch) continue;
                int lx = wx - cx * cfg::CHUNK_X;
                if (blockEmission(ch->get(lx, ly, lz)) > 0) return true;
            }
        }
    }
    return false;
}

// Flood block light through air in all six directions, then bake it onto the mesh.
// Each step loses 1, the same along +X -X +Y -Y +Z -Z, so the glow is round
// rather than aimed at one face. Opaque cells stop the flood.
void bakeBlockLight(World& world, World::Chunk& ch, int cx, int cy, int cz) {
    bool any = chunkEmits(ch);
    if (!any) {
        for (int dy = -1; dy <= 1 && !any; ++dy)
            for (int dz = -1; dz <= 1 && !any; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    World::Chunk* n = world.getChunk(cx + dx, cy + dy, cz + dz);
                    if (n && chunkEmits(*n)) { any = true; break; }
                }
    }
    if (!any) return;

    constexpr int R = BLOCK_LIGHT_MAX;
    const int ox = cx * cfg::CHUNK_X - R;
    const int oy = cy * cfg::CHUNK_Y - R;
    const int oz = cz * cfg::CHUNK_Z - R;
    const int sx = cfg::CHUNK_X + 2 * R;
    const int sy = cfg::CHUNK_Y + 2 * R;
    const int sz = cfg::CHUNK_Z + 2 * R;
    const int gx0 = floorDiv(ox, cfg::CHUNK_X);
    const int gy0 = floorDiv(oy, cfg::CHUNK_Y);
    const int gz0 = floorDiv(oz, cfg::CHUNK_Z);
    const int gx1 = floorDiv(ox + sx - 1, cfg::CHUNK_X);
    const int gy1 = floorDiv(oy + sy - 1, cfg::CHUNK_Y);
    const int gz1 = floorDiv(oz + sz - 1, cfg::CHUNK_Z);
    const int ncx = gx1 - gx0 + 1;
    const int ncy = gy1 - gy0 + 1;
    const int ncz = gz1 - gz0 + 1;
    std::vector<World::Chunk*> grid((size_t)ncx * ncy * ncz, nullptr);
    auto slot = [&](int ccx, int ccy, int ccz) -> World::Chunk* {
        return grid[(size_t)((ccy - gy0) * ncz + (ccz - gz0)) * ncx + (ccx - gx0)];
    };
    for (int ccy = gy0; ccy <= gy1; ++ccy)
        for (int ccz = gz0; ccz <= gz1; ++ccz)
            for (int ccx = gx0; ccx <= gx1; ++ccx)
                grid[(size_t)((ccy - gy0) * ncz + (ccz - gz0)) * ncx + (ccx - gx0)] =
                    world.getChunk(ccx, ccy, ccz);

    auto at = [&](int wx, int wy, int wz) -> uint8_t {
        if (wy < 0 || wy >= cfg::WORLD_H) return AIR;
        int ccx = floorDiv(wx, cfg::CHUNK_X);
        int ccy = floorDiv(wy, cfg::CHUNK_Y);
        int ccz = floorDiv(wz, cfg::CHUNK_Z);
        World::Chunk* src = slot(ccx, ccy, ccz);
        if (!src) return AIR;
        return src->get(wx - ccx * cfg::CHUNK_X, wy - ccy * cfg::CHUNK_Y, wz - ccz * cfg::CHUNK_Z);
    };

    std::vector<uint8_t> light((size_t)sx * sy * sz, 0);
    auto idx = [&](int ix, int iy, int iz) {
        return (iy * sz + iz) * sx + ix;
    };
    std::vector<int> q;
    q.reserve(1024);
    for (int iy = 0; iy < sy; ++iy) {
        int wy = oy + iy;
        for (int iz = 0; iz < sz; ++iz) {
            int wz = oz + iz;
            for (int ix = 0; ix < sx; ++ix) {
                int e = blockEmission(at(ox + ix, wy, wz));
                if (e <= 0) continue;
                int i = idx(ix, iy, iz);
                light[(size_t)i] = (uint8_t)e;
                q.push_back(i);
            }
        }
    }
    if (q.empty()) return;

    static const int kDir[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
    };
    size_t qh = 0;
    while (qh < q.size()) {
        int i = q[qh++];
        int ix = i % sx;
        int t = i / sx;
        int iz = t % sz;
        int iy = t / sz;
        int lv = light[(size_t)i];
        if (lv <= 1) continue;
        for (int d = 0; d < 6; ++d) {
            int nx = ix + kDir[d][0];
            int ny = iy + kDir[d][1];
            int nz = iz + kDir[d][2];
            if ((unsigned)nx >= (unsigned)sx || (unsigned)ny >= (unsigned)sy || (unsigned)nz >= (unsigned)sz)
                continue;
            if (isOpaque(at(ox + nx, oy + ny, oz + nz))) continue;
            int ni = idx(nx, ny, nz);
            int nl = lv - 1;
            if (nl > (int)light[(size_t)ni]) {
                light[(size_t)ni] = (uint8_t)nl;
                q.push_back(ni);
            }
        }
    }

    auto curve = [](int lv) {
        if (lv <= 0) return 0.0f;
        float t = (float)lv / (float)BLOCK_LIGHT_MAX;
        return std::pow(t, 1.2f);
    };
    auto levelAt = [&](float wx, float wy, float wz) {
        int ix = (int)std::floor(wx) - ox;
        int iy = (int)std::floor(wy) - oy;
        int iz = (int)std::floor(wz) - oz;
        if ((unsigned)ix >= (unsigned)sx || (unsigned)iy >= (unsigned)sy || (unsigned)iz >= (unsigned)sz)
            return 0;
        return (int)light[(size_t)idx(ix, iy, iz)];
    };
    auto paint = [&](Vertex& v) {
        float wx = (float)(cx * cfg::CHUNK_X) + v.px;
        float wy = (float)(cy * cfg::CHUNK_Y) + v.py;
        float wz = (float)(cz * cfg::CHUNK_Z) + v.pz;
        int a = levelAt(wx, wy, wz);
        int b = levelAt(wx + v.nx * 0.51f, wy + v.ny * 0.51f, wz + v.nz * 0.51f);
        v.blockLight = curve(a > b ? a : b);
    };
    for (Vertex& v : ch.meshOpaque) paint(v);
    for (Vertex& v : ch.meshTransparent) paint(v);
}

} // namespace

bool World::chunkExists(int cx, int cy, int cz) const {
    return m_chunks.find(chunkKey(cx, cy, cz)) != m_chunks.end();
}

bool World::columnLoaded(int cx, int cz) const {
    for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++)
        if (chunkExists(cx, cy, cz)) return true;
    return false;
}

World::Chunk* World::ensureLoadedSlice(int cx, int cy, int cz) {
    if (cy < 0 || cy >= cfg::CHUNK_LAYERS) return nullptr;
    int64_t key = chunkKey(cx, cy, cz);
    auto it = m_chunks.find(key);
    if (it != m_chunks.end()) return &it->second;
    if (!columnLoaded(cx, cz)) return nullptr;
    Chunk slice;
    slice.generated = true;
    auto [ins, ok] = m_chunks.emplace(key, std::move(slice));
    (void)ok;
    return &ins->second;
}

static bool lanternAttachBlock(uint8_t b) {
    return b != AIR && !isLiquid(b);
}

// Face 0 is the clicked top (sit). Face 1 is the clicked underside (hang).
// Side faces, and an unknown face, sit when a block is below and hang only
// when the cell below is empty and a block is above.
static bool lanternHangs(const World& world, int x, int y, int z, int placeFace) {
    if (placeFace == 0) return false;
    if (placeFace == 1) return true;
    if (lanternAttachBlock(world.getBlock(x, y - 1, z))) return false;
    if (lanternAttachBlock(world.getBlock(x, y + 1, z))) return true;
    return false;
}

static bool torchSupportBlock(uint8_t b) {
    return blocksMotion(b);
}

static int tryTorchAttach(const World& world, int x, int y, int z, int attachFace) {
    // Ceiling mounts (+Y) are not allowed. Floor = 1, walls = 2..5.
    if (attachFace < 1 || attachFace > 5) return -1;
    const geo::FaceDef& F = geo::kFaces[attachFace];
    if (!torchSupportBlock(world.getBlock(x + F.n[0], y + F.n[1], z + F.n[2])))
        return -1;
    return attachFace;
}

int World::resolveTorchAttach(int x, int y, int z, int placeFace) const {
    // Clicked face of the support block → attach face on the torch cell.
    if (placeFace == 0) return tryTorchAttach(*this, x, y, z, 1);
    if (placeFace >= 2 && placeFace <= 5)
        return tryTorchAttach(*this, x, y, z, oppositeFace(placeFace));
    if (placeFace == 1) return -1;
    if (tryTorchAttach(*this, x, y, z, 1) >= 0) return 1;
    for (int f = 2; f <= 5; f++) {
        int a = tryTorchAttach(*this, x, y, z, f);
        if (a >= 0) return a;
    }
    return -1;
}

int World::torchAttachAt(int x, int y, int z) const {
    if (getBlock(x, y, z) != TORCH) return -1;
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return -1;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return -1;
    uint8_t lv = it->second.levelAt(c.lx, c.ly, c.lz);
    if (lv >= 1 && lv <= 5) return (int)lv;
    return 1;
}

bool World::setBlock(int x, int y, int z, uint8_t b, bool markModified, bool updateMesh,
                     int placeFace, int cellFlags) {
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return false;
    int cx = c.cx, cy = c.cy, cz = c.cz, lx = c.lx, ly = c.ly, lz = c.lz;
    int64_t key = chunkKey(cx, cy, cz);
    auto it = m_chunks.find(key);
    if (it == m_chunks.end()) {
        if (!ensureLoadedSlice(cx, cy, cz)) return false;
        it = m_chunks.find(key);
    }

    int torchAttach = -1;
    if (b == TORCH) {
        // Structure load passes the stored attach face in placeFace together with
        // cellFlags >= 0 (even when flags are zero). Trust the blueprint; support
        // may be painted later in the same pass.
        if (cellFlags >= 0 && placeFace >= 1 && placeFace <= 5)
            torchAttach = placeFace;
        else
            torchAttach = resolveTorchAttach(x, y, z, placeFace);
        if (torchAttach < 0) return false;
    }

    uint8_t prev = it->second.get(lx, ly, lz);
    uint8_t prevFlags = it->second.flagAt(lx, ly, lz);
    uint32_t prevBind = it->second.treeIdAt(lx, ly, lz);
    bool cutAliveWood = isTreeWood(prev) && (prevFlags & FLAG_ALIVE) && b != prev;
    it->second.set(lx, ly, lz, b);
    it->second.setFlag(lx, ly, lz, 0); // player / other placement is death
    if (cellFlags >= 0)
        it->second.setFlag(lx, ly, lz, (uint8_t)cellFlags);
    else if (b == LANTERN && lanternHangs(*this, x, y, z, placeFace))
        it->second.setFlag(lx, ly, lz, FLAG_LANTERN_HANG);
    it->second.setTreeId(lx, ly, lz, 0);
    if (prev != b) clearBlockDur(-1, x, y, z);

    // Cut-face marks are for felling living trees (rings / stripped sides).
    // Placed logs in the structure editor must not inherit them from neighbors.
    if (cutAliveWood) {
        for (int f = 0; f < 6; f++) {
            const geo::FaceDef& F = geo::kFaces[f];
            int nx = x + F.n[0], ny = y + F.n[1], nz = z + F.n[2];
            if (!isTreeWood(getBlock(nx, ny, nz))) continue;
            CellLoc nc;
            if (!cellLoc(nx, ny, nz, nc)) continue;
            auto nit = m_chunks.find(chunkKey(nc.cx, nc.cy, nc.cz));
            if (nit == m_chunks.end()) continue;
            uint8_t nf = nit->second.flagAt(nc.lx, nc.ly, nc.lz);
            nit->second.setFlag(nc.lx, nc.ly, nc.lz, nf | flagCutFace(oppositeFace(f)));
            nit->second.dirty = true;
            if (markModified) nit->second.modified = true;
            touchAuth(nx, ny, nz);
        }
    }

    // Water: placing water creates a temporary source (level 16); any other block
    // clears the dynamic-water level. Generated ocean water keeps level 0 (static).
    // Log and stripped wood reuse that byte for the placement axis (17 = X, 18 = Z).
    // Torch stores its support attach face (1..5) in the same byte.
    if (b == WATER) {
        it->second.setLevel(lx, ly, lz, cfg::WATER_SOURCE_LEVEL);
        it->second.hasWater = true;
    } else if (b == TORCH) {
        it->second.setLevel(lx, ly, lz, (uint8_t)torchAttach);
    } else if (isOrientedWood(b) &&
               (cellFlags < 0 || (cellFlags & (FLAG_ALIVE | FLAG_SETTLED)) == 0)) {
        int axis = (placeFace >= 0 && placeFace < 6) ? faceAxis(placeFace) : 1;
        it->second.setLevel(lx, ly, lz, logAxisLevel(axis));
    } else {
        it->second.setLevel(lx, ly, lz, 0);
    }

    // Sod is attached to its dirt block: breaking or replacing that block removes
    // its sod immediately. Freshly exposed dirt stays bare (no instant regrowth) —
    // sod is only created at world generation (rebuildSod).
    removeSodAt(it->second, lx, ly, lz);
    removeBarkAt(it->second, lx, ly, lz);

    if (markModified && !(m_arena && trialColumnOverlaps(cx, cz))) {
        it->second.modified = true;
        rememberEdited(cx, cz);
    }
    touchAuth(x, y, z);
    touchAuthSod(key);
    touchAuthBark(key);

    if (blockEmission(b) > 0) {
        it->second.hasEmission = true;
        it->second.emissionKnown = true;
    } else if (blockEmission(prev) > 0) {
        it->second.emissionKnown = false;
    }
    const bool lightTouch = blockEmission(prev) > 0 || blockEmission(b) > 0
        || (isOpaque(prev) != isOpaque(b) && emissionNear(*this, x, y, z));

    auto rebuildNow = [&](int ncx, int ncy, int ncz) {
        auto nit = m_chunks.find(chunkKey(ncx, ncy, ncz));
        if (nit != m_chunks.end()) {
            buildMeshFor(nit->second, ncx, ncy, ncz);
            nit->second.dirty = false;
        }
    };
    auto touchBox = [&](bool immediate) {
        constexpr int R = BLOCK_LIGHT_MAX;
        int x0 = floorDiv(x - R, cfg::CHUNK_X);
        int x1 = floorDiv(x + R, cfg::CHUNK_X);
        int y0 = floorDiv(y - R, cfg::CHUNK_Y);
        int y1 = floorDiv(y + R, cfg::CHUNK_Y);
        int z0 = floorDiv(z - R, cfg::CHUNK_Z);
        int z1 = floorDiv(z + R, cfg::CHUNK_Z);
        for (int tz = z0; tz <= z1; ++tz)
            for (int ty = y0; ty <= y1; ++ty)
                for (int tx = x0; tx <= x1; ++tx) {
                    if (immediate) {
                        rebuildNow(tx, ty, tz);
                    } else {
                        auto nit = m_chunks.find(chunkKey(tx, ty, tz));
                        if (nit == m_chunks.end()) continue;
                        nit->second.dirty = true;
                        m_meshQueue.push_back(nit->first);
                    }
                }
    };
    if (updateMesh) {
        if (lightTouch) {
            touchBox(true);
        } else {
            rebuildNow(cx, cy, cz);
            if (lx == 0) rebuildNow(cx - 1, cy, cz);
            if (lx == cfg::CHUNK_X - 1) rebuildNow(cx + 1, cy, cz);
            if (lz == 0) rebuildNow(cx, cy, cz - 1);
            if (lz == cfg::CHUNK_Z - 1) rebuildNow(cx, cy, cz + 1);
            if (ly == 0) rebuildNow(cx, cy - 1, cz);
            if (ly == cfg::CHUNK_Y - 1) rebuildNow(cx, cy + 1, cz);
        }
    } else if (lightTouch) {
        touchBox(false);
    } else {
        it->second.dirty = true;
        m_meshQueue.push_back(key);
    }

    if (prev != b) {
        plugin::BlockEvent ev{ this, x, y, z, b, prev, -1, markModified, updateMesh };
        if (prev != AIR) plugin::blockStrategy(prev)->onBreak(ev);
        if (b != AIR) plugin::blockStrategy(b)->onPlace(ev);
        // Breaking / replacing a cell: each of the six neighbors self-checks.
        if (prev != AIR) {
            for (int f = 0; f < 6; f++) {
                const geo::FaceDef& F = geo::kFaces[f];
                int nx = x + F.n[0], ny = y + F.n[1], nz = z + F.n[2];
                uint8_t nb = getBlock(nx, ny, nz);
                if (nb == AIR) continue;
                // Neighbor's face toward this cell is the opposite of f.
                plugin::BlockEvent nev{ this, nx, ny, nz, nb, nb, f ^ 1, markModified, updateMesh };
                plugin::blockStrategy(nb)->onSelfCheck(nev);
            }
        }
    }

    if (cutAliveWood) detachAliveTree(x, y, z, prevBind);
    return true;
}

int World::computeHeight(int wx, int wz) const {
    // Half-size blocks: use lower noise frequencies so terrain features are larger
    // and gentler (smoother hills at the finer block resolution).
    float n1 = noise::fbm2((float)wx * 0.0022f, (float)wz * 0.0022f, 4, m_seed);
    float n2 = noise::fbm2((float)wx * 0.008f, (float)wz * 0.008f, 3, m_seed + 777u);
    float e = n1 * 0.70f + n2 * 0.30f;
    e = std::pow(e, 1.35f);
    float h = (float)(cfg::SEA_LEVEL - 10) + e * 72.0f;
    int hi = (int)h;
    if (hi < 1) hi = 1;
    if (hi > cfg::WORLD_H - 3) hi = cfg::WORLD_H - 3;
    return hi;
}

int World::surfaceHeight(int x, int z) { return computeHeight(x, z); }

void World::reset(uint32_t seed) {
    m_seed = seed;
    m_chunks.clear();
    m_meshQueue.clear();
    m_sodCursor = 0;
    m_phys.clear();
    m_drops.clear();
    m_nextDropId = 1;
    m_blockDur.clear();
    m_originTrees.clear();
    m_authCells.clear();
    m_authSod.clear();
    m_authBark.clear();
    m_editedCols.clear();
    m_nextTree = 1;
    m_mineEpoch = 1;
    m_arena = false;
    if (m_matchBounds) structure::roll(m_seed);
}

struct ColumnBuf {
    std::vector<uint8_t> blocks;
    std::vector<uint8_t> flags;
    std::vector<uint32_t> treeId;
    ColumnBuf()
        : blocks((size_t)cfg::CHUNK_X * cfg::WORLD_H * cfg::CHUNK_Z, (uint8_t)AIR),
          flags((size_t)cfg::CHUNK_X * cfg::WORLD_H * cfg::CHUNK_Z, 0),
          treeId((size_t)cfg::CHUNK_X * cfg::WORLD_H * cfg::CHUNK_Z, 0) {}
    int idx(int lx, int y, int lz) const {
        return (y * cfg::CHUNK_Z + lz) * cfg::CHUNK_X + lx;
    }
    uint8_t get(int lx, int y, int lz) const { return blocks[(size_t)idx(lx, y, lz)]; }
    void set(int lx, int y, int lz, uint8_t b) { blocks[(size_t)idx(lx, y, lz)] = b; }
    void markAlive(int lx, int y, int lz) { flags[(size_t)idx(lx, y, lz)] |= FLAG_ALIVE; }
    void bindAlive(int lx, int y, int lz, uint32_t id) {
        markAlive(lx, y, lz);
        treeId[(size_t)idx(lx, y, lz)] = id;
    }
};

void World::noteBlankColumn(int cx, int cz) {
    if (columnLoaded(cx, cz)) return;
    Chunk slice;
    slice.generated = true;
    slice.dirty = true;
    m_chunks.emplace(chunkKey(cx, 0, cz), std::move(slice));
}

void World::setMatchBounds(bool on) {
    m_matchBounds = on;
    m_buildCanvas = false;
    if (on) structure::roll(m_seed);
}

void World::setBuildCanvas(bool on) {
    m_buildCanvas = on;
    if (on) m_matchBounds = false;
}

void World::generateColumn(int cx, int cz) {
    auto commit = [&](ColumnBuf& col) {
        for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) {
            bool any = false;
            int base = cy * cfg::CHUNK_Y;
            for (int ly = 0; ly < cfg::CHUNK_Y && !any; ly++) {
                for (int lz = 0; lz < cfg::CHUNK_Z && !any; lz++) {
                    for (int lx = 0; lx < cfg::CHUNK_X; lx++) {
                        if (col.get(lx, base + ly, lz) != AIR) { any = true; break; }
                    }
                }
            }
            if (!any) continue;
            Chunk slice;
            slice.generated = true;
            slice.dirty = true;
            for (int ly = 0; ly < cfg::CHUNK_Y; ly++) {
                for (int lz = 0; lz < cfg::CHUNK_Z; lz++) {
                    for (int lx = 0; lx < cfg::CHUNK_X; lx++) {
                        int src = col.idx(lx, base + ly, lz);
                        int dst = (ly * cfg::CHUNK_Z + lz) * cfg::CHUNK_X + lx;
                        slice.blocks[(size_t)dst] = col.blocks[(size_t)src];
                        slice.flags[(size_t)dst] = col.flags[(size_t)src];
                        if (col.treeId[(size_t)src] != 0) slice.setTreeId(lx, ly, lz, col.treeId[(size_t)src]);
                        if (slice.blocks[(size_t)dst] == WATER) slice.hasWater = true;
                    }
                }
            }
            int64_t key = chunkKey(cx, cy, cz);
            if (m_chunks.find(key) == m_chunks.end())
                m_chunks.emplace(key, std::move(slice));
        }
    };

    if (m_arena && trialColumnOverlaps(cx, cz)) {
        ColumnBuf room;
        const int ax0 = structure::kTrialX0;
        const int az0 = structure::kTrialZ0;
        const int ax1 = ax0 + structure::kTrialSpan;
        const int az1 = az0 + structure::kTrialSpan;
        const int ccx = structure::kTrialCX;
        const int ccz = structure::kTrialCZ;
        const uint8_t shell = (uint8_t)ARENA_SHELL;
        for (int lz = 0; lz < cfg::CHUNK_Z; lz++) {
            for (int lx = 0; lx < cfg::CHUNK_X; lx++) {
                int wx = cx * cfg::CHUNK_X + lx;
                int wz = cz * cfg::CHUNK_Z + lz;
                bool inside = wx >= ax0 && wx < ax1 && wz >= az0 && wz < az1;
                int dx = wx - ccx;
                int dz = wz - ccz;
                bool pillar = std::abs(dx) == 8 && std::abs(dz) == 8;
                bool core = wx == ccx && wz == ccz;
                for (int y = 0; y < cfg::WORLD_H; y++) {
                    uint8_t b = AIR;
                    if (!inside) b = shell;
                    else if (y == 0 || y == cfg::WORLD_H - 1) b = shell;
                    else if (wx == ax0 || wx == ax1 - 1 || wz == az0 || wz == az1 - 1) b = shell;
                    else if (pillar && y >= 1 && y <= 4) b = shell;
                    if (core && y == 0) b = (uint8_t)GUARDIAN_CORE;
                    room.set(lx, y, lz, b);
                }
            }
        }
        commit(room);
        return;
    }

    const bool canvas = m_buildCanvas;
    const bool match = m_matchBounds;
    if (canvas && (cx < -1 || cx > 1 || cz < -1 || cz > 1)) {
        noteBlankColumn(cx, cz);
        return;
    }
    if (match && !matchmap::columnInside(cx, cz)) {
        noteBlankColumn(cx, cz);
        return;
    }
    const bool plain = canvas;
    if (!plain) {
        for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) {
            Chunk loaded;
            if (!loadChunkFile(cx, cy, cz, loaded)) continue;
            m_chunks.emplace(chunkKey(cx, cy, cz), std::move(loaded));
        }
    }

    ColumnBuf ch;
    if (plain) {
        int floorY = canvas ? (structure::kEditY - 1) : cfg::SEA_LEVEL;
        if (floorY < 1) floorY = 1;
        if (floorY >= cfg::WORLD_H) floorY = cfg::WORLD_H - 1;
        for (int lz = 0; lz < cfg::CHUNK_Z; lz++) {
            for (int lx = 0; lx < cfg::CHUNK_X; lx++) {
                ch.set(lx, 0, lz, BEDROCK);
                for (int y = 1; y <= floorY; y++) ch.set(lx, y, lz, STONE);
            }
        }
    } else {

    // Base terrain (solid fill + water).
    for (int lz = 0; lz < cfg::CHUNK_Z; lz++) {
        for (int lx = 0; lx < cfg::CHUNK_X; lx++) {
            int wx = cx * cfg::CHUNK_X + lx;
            int wz = cz * cfg::CHUNK_Z + lz;
            int h = computeHeight(wx, wz);
            bool beach = (h <= cfg::SEA_LEVEL + 1);
            for (int y = 0; y < cfg::WORLD_H; y++) {
                uint8_t b;
                if (y == 0) b = BEDROCK;
                else if (y == h) b = beach ? SAND : DIRT;
                else if (y > h - 4 && y < h) b = beach ? SAND : DIRT;
                else if (y < h) b = STONE;
                else if (y <= cfg::SEA_LEVEL) b = WATER;
                else b = AIR;
                ch.set(lx, y, lz, b);
            }
        }
    }

    // Caves (carve stone below the surface).
    for (int lz = 0; lz < cfg::CHUNK_Z; lz++) {
        for (int lx = 0; lx < cfg::CHUNK_X; lx++) {
            int wx = cx * cfg::CHUNK_X + lx;
            int wz = cz * cfg::CHUNK_Z + lz;
            int h = computeHeight(wx, wz);
            for (int y = 2; y < h - 1; y++) {
                float cv = noise::fbm3((float)wx * 0.055f, (float)y * 0.055f, (float)wz * 0.055f, 2, m_seed + 555u);
                if (cv > 0.62f && ch.get(lx, y, lz) == STONE) ch.set(lx, y, lz, AIR);
            }
        }
    }

    // Ore deposits in remaining stone.
    for (int lz = 0; lz < cfg::CHUNK_Z; lz++) {
        for (int lx = 0; lx < cfg::CHUNK_X; lx++) {
            int wx = cx * cfg::CHUNK_X + lx;
            int wz = cz * cfg::CHUNK_Z + lz;
            int h = computeHeight(wx, wz);
            for (int y = 1; y < h - 1; y++) {
                if (ch.get(lx, y, lz) != STONE) continue;
                float c1 = noise::noise3((float)wx * 0.10f, (float)y * 0.10f, (float)wz * 0.10f, m_seed + 111u);
                float c2 = noise::noise3((float)wx * 0.09f, (float)y * 0.09f, (float)wz * 0.09f, m_seed + 222u);
                float c3 = noise::noise3((float)wx * 0.08f, (float)y * 0.08f, (float)wz * 0.08f, m_seed + 333u);
                float c4 = noise::noise3((float)wx * 0.07f, (float)y * 0.07f, (float)wz * 0.07f, m_seed + 444u);
                if (c1 > 0.74f) ch.set(lx, y, lz, COAL_ORE);
                else if (y < 44 && c2 > 0.80f) ch.set(lx, y, lz, IRON_ORE);
                else if (y < 26 && c3 > 0.84f) ch.set(lx, y, lz, GOLD_ORE);
                else if (y < 14 && c4 > 0.88f) ch.set(lx, y, lz, DIAMOND_ORE);
            }
        }
    }

    // Trees can spill ~18 blocks into neighboring columns. Stamp this chunk's
    // trees plus overflow from a 2-chunk neighborhood so canopies are not
    // sliced at chunk borders.
    // 0.1 per 16-block chunk; scale so a 32-wide chunk keeps the same biome size.
    float forest = noise::noise2((float)cx * 0.2f, (float)cz * 0.2f, m_seed + 8888u);
    auto putTree = [&](int wx, int wy, int wz, uint8_t b, uint32_t bind) {
        if (wy < 0 || wy >= cfg::WORLD_H) return;
        if (floorDiv(wx, cfg::CHUNK_X) != cx || floorDiv(wz, cfg::CHUNK_Z) != cz) return;
        int lx = wx - cx * cfg::CHUNK_X;
        int lz = wz - cz * cfg::CHUNK_Z;
        if (ch.get(lx, wy, lz) != AIR) return;
        ch.set(lx, wy, lz, b);
        ch.bindAlive(lx, wy, lz, bind);
    };
    for (int ncz = cz - 2; ncz <= cz + 2; ncz++) {
        for (int ncx = cx - 2; ncx <= cx + 2; ncx++) {
            cacheOriginTrees(ncx, ncz);
            auto it = m_originTrees.find(columnKey(ncx, ncz));
            if (it == m_originTrees.end()) continue;
            for (const OriginTree& t : it->second) {
                for (const IVec3& w : t.woods) putTree(w.x, w.y, w.z, LOG, t.bindId);
                for (const IVec3& L : t.leaves) putTree(L.x, L.y, L.z, LEAVES, t.bindId);
            }
        }
    }

    // Shrubs: low bushes (shrub-leaf wrapping shrub-stem) as small clumps or
    // elongated strips, only inside the forest biome.
    int shrubCount = 0;
    if (forest > 0.40f) shrubCount = 2 + (int)((forest - 0.40f) * 6.0f);
    if (shrubCount > 7) shrubCount = 7;

    for (int i = 0; i < shrubCount; i++) {
        uint32_t sh = noise::hash2(cx * 911 + i * 31, cz * 733 + i * 53, m_seed + 4321u);
        int span = cfg::CHUNK_X - 4;
        int sx = 2 + (int)(noise::hash01(sh) * (float)span);
        int sz = 2 + (int)(noise::hash01(sh ^ 0x5A5Au) * (float)span);
        int wx = cx * cfg::CHUNK_X + sx;
        int wz = cz * cfg::CHUNK_Z + sz;
        int h = computeHeight(wx, wz);
        if (h <= cfg::SEA_LEVEL + 1 || h >= cfg::WORLD_H - 8) continue;
        if (ch.get(sx, h, sz) != DIRT) continue;

        auto inC = [&](int x, int y, int z) {
            return x >= 0 && x < cfg::CHUNK_X && y >= 0 && y < cfg::WORLD_H && z >= 0 && z < cfg::CHUNK_Z;
        };
        auto leaf = [&](int x, int y, int z) {
            if (inC(x, y, z) && ch.get(x, y, z) == AIR) {
                ch.set(x, y, z, SHRUB_LEAF);
                ch.markAlive(x, y, z);
            }
        };
        auto stem = [&](int x, int y, int z) {
            if (inC(x, y, z) && (ch.get(x, y, z) == AIR || ch.get(x, y, z) == SHRUB_LEAF)) {
                ch.set(x, y, z, SHRUB_STEM);
                ch.markAlive(x, y, z);
            }
        };

        bool strip = noise::hash01(sh ^ 0x77u) > 0.5f;
        int len = strip ? 2 + (int)(noise::hash01(sh ^ 0x99u) * 2.0f) : 1; // 1..3
        int dir = (int)(noise::hash01(sh ^ 0xB1u) * 4.0f);
        static const int BDX4[4] = { 1, -1, 0, 0 };
        static const int BDZ4[4] = { 0, 0, 1, -1 };

        for (int k = 0; k < len; k++) {
            int bx = sx + BDX4[dir] * k;
            int bz = sz + BDZ4[dir] * k;
            if (!inC(bx, h, bz) || ch.get(bx, h, bz) != DIRT) break;
            bool tall = noise::hash01(sh ^ (uint32_t)(k * 7 + 3)) > 0.5f;
            stem(bx, h + 1, bz);
            if (tall) stem(bx, h + 2, bz);
            for (int d = 0; d < 4; d++) {
                leaf(bx + BDX4[d], h + 1, bz + BDZ4[d]);
                leaf(bx + BDX4[d], h + 2, bz + BDZ4[d]);
            }
            leaf(bx, h + (tall ? 3 : 2), bz); // top cap
            if (noise::hash01(sh ^ (uint32_t)(k * 11 + 1)) > 0.45f) leaf(bx + 1, h + 1, bz + 1);
            if (noise::hash01(sh ^ (uint32_t)(k * 11 + 2)) > 0.45f) leaf(bx - 1, h + 1, bz - 1);
        }
    }

    // Grass tufts: density mapped from air humidity. Each column's low-space air
    // humidity (clamped to 0..255) maps to a 20%..80% tuft density, so wetter
    // areas grow denser grass and drier areas grow sparser grass.
    // Naturally generated tufts occupy one cell, or two stacked cells (75%).
    for (int gz = 0; gz < cfg::CHUNK_Z; gz++) {
        for (int gx = 0; gx < cfg::CHUNK_X; gx++) {
            int gh = computeHeight(cx * cfg::CHUNK_X + gx, cz * cfg::CHUNK_Z + gz);
            if (gh <= cfg::SEA_LEVEL + 1 || gh >= cfg::WORLD_H - 2) continue;
            if (ch.get(gx, gh, gz) != DIRT || ch.get(gx, gh + 1, gz) != AIR) continue;
            int h = airHumidity(cx * cfg::CHUNK_X + gx, gh + 1, cz * cfg::CHUNK_Z + gz);
            if (h < 0) h = 0;
            if (h > 255) h = 255;
            float density = 0.20f + ((float)h / 255.0f) * 0.60f; // [0.20, 0.80]
            int wx = cx * cfg::CHUNK_X + gx;
            int wz = cz * cfg::CHUNK_Z + gz;
            if (noise::hash01(noise::hash2(wx, wz, m_seed + 987u)) < density) {
                ch.set(gx, gh + 1, gz, GRASS_TUFT);
                bool room = (gh + 2 < cfg::WORLD_H) && ch.get(gx, gh + 2, gz) == AIR;
                if (room && noise::hash01(noise::hash2(wx, wz, m_seed + 211u)) < 0.75f)
                    ch.set(gx, gh + 2, gz, GRASS_TUFT);
            }
        }
    }
    }

    if (match && matchmap::columnPlayable(cx, cz)) {
        structure::stampColumn(cx, cz,
            [&](int x, int z) { return surfaceHeight(x, z); },
            [&](int x, int y, int z, uint8_t b) {
                int lx = x - cx * cfg::CHUNK_X;
                int lz = z - cz * cfg::CHUNK_Z;
                if (lx < 0 || lx >= cfg::CHUNK_X || lz < 0 || lz >= cfg::CHUNK_Z) return;
                if (y < 0 || y >= cfg::WORLD_H) return;
                int i = ch.idx(lx, y, lz);
                ch.blocks[(size_t)i] = b;
                ch.flags[(size_t)i] = 0;
                ch.treeId[(size_t)i] = 0;
            });
    }

    commit(ch);
}

void World::cacheOriginTrees(int ocx, int ocz) {
    int64_t key = columnKey(ocx, ocz);
    if (m_originTrees.find(key) != m_originTrees.end()) return;
    if (m_matchBounds && !matchmap::columnInside(ocx, ocz)) {
        m_originTrees.emplace(key, std::vector<OriginTree>{});
        return;
    }

    std::vector<OriginTree> grown;
    float forest = noise::noise2((float)ocx * 0.2f, (float)ocz * 0.2f, m_seed + 8888u);
    int treeCount = 0;
    if (forest > 0.52f) treeCount = 1 + (int)((forest - 0.52f) * 6.0f);
    if (treeCount > 4) treeCount = 4;

    std::vector<std::pair<int, int>> planted;
    for (int i = 0; i < treeCount; i++) {
        uint32_t hh = noise::hash2(ocx * 733 + i * 137, ocz * 977 + i * 251, m_seed + 1234u);
        TreeKind kind = (noise::hash01(hh ^ 0x71EEu) < 0.40f) ? TreeKind::Thick : TreeKind::Thin;
        int tx = -1, tz = -1, h = 0;
        bool placed = false;
        for (int attempt = 0; attempt < 16; attempt++) {
            uint32_t ha = hh ^ (0x9E3779B9u * (uint32_t)(attempt + 1));
            int span = cfg::CHUNK_X - 3;
            if (span < 1) break;
            int ax = 1 + (int)(noise::hash01(ha) * (float)span);
            int az = 1 + (int)(noise::hash01(ha ^ 0xABCDu) * (float)span);
            bool far = true;
            for (const auto& p : planted) {
                int dx = std::abs(ax - p.first);
                int dz = std::abs(az - p.second);
                if (std::max(dx, dz) < 5) { far = false; break; }
            }
            if (!far) continue;
            int wx = ocx * cfg::CHUNK_X + ax;
            int wz = ocz * cfg::CHUNK_Z + az;
            h = computeHeight(wx, wz);
            if (h <= cfg::SEA_LEVEL + 1 || h >= cfg::WORLD_H - 28) continue;
            tx = ax;
            tz = az;
            placed = true;
            break;
        }
        if (!placed) continue;
        planted.push_back({ tx, tz });

        TreeGrow g;
        applyTreeKind(g.params, kind);
        g.params.seed = hh;
        g.params.trunkH = (kind == TreeKind::Thick)
            ? (10 + (int)(noise::hash01(hh ^ 0x5A5Au) * 4.0f))
            : (8 + (int)(noise::hash01(hh ^ 0x5A5Au) * 4.0f));
        g.params.leaf = tree_canopy::forTree(hh);
        g.reset();
        g.growAll();

        OriginTree t;
        t.bindId = noise::hash3(ocx, ocz, (int)grown.size() + 1, m_seed ^ 0x71EE71EEu);
        if (t.bindId == 0) t.bindId = 1;
        int ox = ocx * cfg::CHUNK_X + tx;
        int oz = ocz * cfg::CHUNK_Z + tz;
        t.woods.reserve(g.woods.size());
        t.leaves.reserve(g.leaves.size());
        for (const IVec3& w : g.woods)
            t.woods.push_back(IVec3(ox + w.x, h + w.y, oz + w.z));
        for (const IVec3& L : g.leaves)
            t.leaves.push_back(IVec3(ox + L.x, h + L.y, oz + L.z));
        grown.push_back(std::move(t));
    }
    m_originTrees.emplace(key, std::move(grown));
}

void World::update(const Vec3& playerPos, int meshBudget) {
    updateAnchors(&playerPos, 1, meshBudget);
}

void World::updateAnchors(const Vec3* pos, int count, int meshBudget) {
    std::vector<std::pair<int, int>> anchors;
    if (pos && count > 0) {
        anchors.reserve((size_t)count);
        for (int i = 0; i < count; i++) {
            int pcx = floorDiv((int)std::floor(pos[i].x / cfg::BLOCK_SCALE), cfg::CHUNK_X);
            int pcz = floorDiv((int)std::floor(pos[i].z / cfg::BLOCK_SCALE), cfg::CHUNK_Z);
            anchors.emplace_back(pcx, pcz);
        }
    }
    if (anchors.empty()) anchors.emplace_back(0, 0);

    auto colDist = [&](int cx, int cz) {
        int best = 1 << 30;
        for (const auto& a : anchors) {
            int d = (cx - a.first) * (cx - a.first) + (cz - a.second) * (cz - a.second);
            if (d < best) best = d;
        }
        return best;
    };
    auto nearAny = [&](int cx, int cz, int radius) {
        for (const auto& a : anchors) {
            int dist = std::max(std::abs(cx - a.first), std::abs(cz - a.second));
            if (dist <= radius) return true;
        }
        return false;
    };

    // Collect missing chunk columns, nearest first, so a player's own column
    // is generated immediately (prevents falling through ungenerated terrain).
    std::vector<std::pair<int, int>> missing;
    std::unordered_set<int64_t> seenCol;
    for (const auto& a : anchors) {
        for (int dz = -cfg::LOAD_RADIUS; dz <= cfg::LOAD_RADIUS; dz++) {
            for (int dx = -cfg::LOAD_RADIUS; dx <= cfg::LOAD_RADIUS; dx++) {
                int cx = a.first + dx, cz = a.second + dz;
                if (m_matchBounds && !matchmap::columnInside(cx, cz)) continue;
                if (m_buildCanvas && (cx < -1 || cx > 1 || cz < -1 || cz > 1)) continue;
                if (!seenCol.insert(columnKey(cx, cz)).second) continue;
                if (!columnLoaded(cx, cz)) missing.emplace_back(cx, cz);
            }
        }
    }
    std::sort(missing.begin(), missing.end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        return colDist(a.first, a.second) < colDist(b.first, b.second);
    });

    int genBudget = 4 * (int)anchors.size();
    if (genBudget > 12) genBudget = 12;
    for (const auto& [cx, cz] : missing) {
        if (genBudget-- <= 0) break;
        generateColumn(cx, cz);
        for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) {
            auto it = m_chunks.find(chunkKey(cx, cy, cz));
            if (it == m_chunks.end()) continue;
            if (!it->second.sodValid) rebuildSod(it->second, cx, cy, cz);
            addBorderSod(cx, cy, cz);
            m_meshQueue.push_back(it->first);
        }
        // Remesh neighbors so AO / face culling / block light match across the
        // new border. Push front so seams clear before deeper mesh work.
        for (int nz = -1; nz <= 1; nz++) {
            for (int nx = -1; nx <= 1; nx++) {
                if (nx == 0 && nz == 0) continue;
                for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) {
                    auto nit = m_chunks.find(chunkKey(cx + nx, cy, cz + nz));
                    if (nit == m_chunks.end()) continue;
                    nit->second.dirty = true;
                    m_meshQueue.push_front(nit->first);
                }
            }
        }
    }

    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        int cx = chunkCX(it->first), cz = chunkCZ(it->first);
        bool keep = nearAny(cx, cz, cfg::UNLOAD_RADIUS);
        if (!keep && m_arena && trialColumnOverlaps(cx, cz)) keep = true;
        if (!keep && m_keepEdited && m_editedCols.count(columnKey(cx, cz))) keep = true;
        if (!keep) {
            bool arenaCol = m_arena && trialColumnOverlaps(cx, cz);
            if (it->second.modified && m_saveEnabled && !arenaCol)
                saveChunkFile(cx, chunkCY(it->first), cz, it->second);
            it = m_chunks.erase(it);
        } else {
            ++it;
        }
    }

    int treeKeep = cfg::UNLOAD_RADIUS + 2;
    for (auto it = m_originTrees.begin(); it != m_originTrees.end();) {
        int ocx = columnCX(it->first), ocz = columnCZ(it->first);
        if (!nearAny(ocx, ocz, treeKeep)) it = m_originTrees.erase(it);
        else ++it;
    }

    // Hold mesh builds until orthogonal neighbor columns that should already be
    // loaded are present. Meshing against missing neighbors treats them as air,
    // which bakes a bright AO rim exactly on the chunk border.
    auto columnWanted = [&](int cx, int cz) {
        if (m_matchBounds && !matchmap::columnInside(cx, cz)) return false;
        if (m_buildCanvas && (cx < -1 || cx > 1 || cz < -1 || cz > 1)) return false;
        return nearAny(cx, cz, cfg::LOAD_RADIUS);
    };
    auto meshNeighborsReady = [&](int cx, int cz) {
        static const int kOrth[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (int i = 0; i < 4; i++) {
            int nx = cx + kOrth[i][0], nz = cz + kOrth[i][1];
            if (!columnWanted(nx, nz)) continue;
            if (!columnLoaded(nx, nz)) return false;
        }
        return true;
    };

    int budget = meshBudget;
    size_t pass = m_meshQueue.size();
    while (budget > 0 && pass-- > 0 && !m_meshQueue.empty()) {
        int64_t key = m_meshQueue.front();
        m_meshQueue.pop_front();
        auto it = m_chunks.find(key);
        if (it == m_chunks.end() || !it->second.dirty) continue;
        int cx = chunkCX(key), cz = chunkCZ(key);
        if (!meshNeighborsReady(cx, cz)) {
            m_meshQueue.push_back(key);
            continue;
        }
        buildMeshFor(it->second, cx, chunkCY(key), cz);
        it->second.dirty = false;
        --budget;
    }
}

float World::vertexAO(int wx, int wy, int wz, int nx, int ny, int nz, int ox, int oy, int oz) const {
    // Sample from the empty cell the face looks into, then step ±1 on the two
    // tangent axes. Using the vertex cell (wx+ox, ...) wrongly pulls in coplanar
    // solid neighbors on -X/-Y/-Z faces, which paints a fake grid on flat walls.
    int bx = wx + nx, by = wy + ny, bz = wz + nz;
    int ta[2]; int n = 0;
    if (nx == 0) ta[n++] = 0;
    if (ny == 0) ta[n++] = 1;
    if (nz == 0) ta[n++] = 2;
    int off[3] = { ox, oy, oz };
    int d[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) { int a = ta[i]; d[a] = (off[a] == 0) ? -1 : 1; }
    int a0 = ta[0], a1 = ta[1];
    int s1[3] = { bx, by, bz }; s1[a0] += d[a0];
    int s2[3] = { bx, by, bz }; s2[a1] += d[a1];
    int s3[3] = { bx, by, bz }; s3[a0] += d[a0]; s3[a1] += d[a1];
    bool o1 = isOpaque(getBlock(s1[0], s1[1], s1[2]));
    bool o2 = isOpaque(getBlock(s2[0], s2[1], s2[2]));
    bool o3 = isOpaque(getBlock(s3[0], s3[1], s3[2]));
    int ao = (o1 && o2) ? 3 : ((o1 ? 1 : 0) + (o2 ? 1 : 0) + (o3 ? 1 : 0));
    // Softer than the classic 0.25 step so adjacent blocks blend instead of
    // reading as a hard brightness cut at every edge (and chunk border).
    return 1.0f - 0.16f * (float)ao; // 1.0, 0.84, 0.68, 0.52
}

void World::buildMeshFor(Chunk& ch, int cx, int cy, int cz) {
    ch.meshOpaque.clear();
    ch.meshTransparent.clear();
    ch.meshOpaque.reserve(8192);
    ch.meshTransparent.reserve(2048);

    // Sod mask first so dirt faces under sod are never culled (holes in thinning
    // sod would otherwise show void).
    std::vector<uint32_t> sodMask((size_t)cfg::CHUNK_VOLUME, 0u);
    for (const Chunk::SodFace& sf : ch.sodFaces) {
        if (sf.face >= 6 || sf.stage >= cfg::SOD_STAGES) continue;
        size_t idx = ((size_t)sf.y * cfg::CHUNK_Z + sf.z) * cfg::CHUNK_X + sf.x;
        sodMask[idx] |= (1u << sf.face);
    }

    for (int y = 0; y < cfg::CHUNK_Y; y++) {
        int wy = cy * cfg::CHUNK_Y + y;
        for (int z = 0; z < cfg::CHUNK_Z; z++) {
            for (int x = 0; x < cfg::CHUNK_X; x++) {
                uint8_t b = ch.get(x, y, z);
                if (b == AIR) continue;

                int wx = cx * cfg::CHUNK_X + x;
                int wz = cz * cfg::CHUNK_Z + z;
                if (structure::isGuardianToken(wx, wy, wz, b)) continue;
                uint8_t below = (wy > 0) ? getBlock(wx, wy - 1, wz) : (uint8_t)AIR;
                uint8_t above = (wy + 1 < cfg::WORLD_H) ? getBlock(wx, wy + 1, wz) : (uint8_t)AIR;
                if (plugin::blockStrategy(b)->emitMesh(ch, x, y, z, wx, wz, below, above))
                    continue;

                const BlockInfo& info = blockOf(b);
                bool isTrans = info.transparent;
                uint8_t wlvl = (b == WATER) ? ch.levelAt(x, y, z) : 0;
                bool dynWater = (b == WATER && wlvl > 0);
                float wh = (float)wlvl / (float)cfg::WATER_MAX_LEVEL;

                uint8_t fl = ch.flagAt(x, y, z);
                int trunk = 1;
                auto isWood = [&](int ix, int iy, int iz) { return isTreeWood(getBlock(ix, iy, iz)); };
                bool grownLog = b == LOG && (fl & (FLAG_ALIVE | FLAG_SETTLED)) != 0;
                if (grownLog) trunk = logTrunkAxis(wx, wy, wz, fl, isWood);
                else if (isOrientedWood(b)) trunk = logAxisFromLevel(ch.levelAt(x, y, z));

                for (int f = 0; f < 6; f++) {
                    const geo::FaceDef& F = geo::kFaces[f];
                    int nx = wx + F.n[0], ny = wy + F.n[1], nz = wz + F.n[2];
                    uint8_t nb = getBlock(nx, ny, nz);
                    if (structure::isGuardianToken(nx, ny, nz, nb)) nb = AIR;
                    // Render a face when the neighbor is see-through (air, water,
                    // glass, leaves). Same-block faces are culled except for passable
                    // cutout blocks (leaves) so adjacent leaves stay visible when the
                    // camera is inside them.
                    bool vis = !blockOf(nb).opaque;
                    if (nb == b && !isPassableCutout(b)) vis = false;
                    if (b == DIRT) {
                        size_t sidx = ((size_t)y * cfg::CHUNK_Z + z) * cfg::CHUNK_X + x;
                        if (sodMask[sidx] & (1u << f)) vis = true;
                    }
                    if (!vis) continue;

                    int qa = 0, qb = 0, longAxis = 0;
                    int spliceKind = 0;
                    if (b == LOG) {
                        auto hasCut = [&](int ix, int iy, int iz, int fc) {
                            return hasCutFace(getFlags(ix, iy, iz), fc);
                        };
                        spliceKind = spliceEndRing(wx, wy, wz, f, trunk, qa, qb, longAxis, isWood, hasCut);
                    }
                    LogFaceTex lf;
                    if (b == LOG) {
                        lf = logFaceTex(fl, f, trunk, spliceKind, qa, qb, longAxis, TEX_LOG_SIDE);
                    } else if (b == WOOD) {
                        uint8_t wf = (uint8_t)(fl & (uint8_t)~(FLAG_ALIVE | FLAG_SETTLED));
                        lf = logFaceTex(wf, f, trunk, 0, 0, 0, 0, TEX_WOOD_SIDE);
                    } else {
                        lf.tile = (f == 0) ? info.texTop : (f == 1 ? info.texBottom : info.texSide);
                    }
                    float alpha = (b == WATER) ? 0.55f : (b == GLASS ? 0.4f : 1.0f);
                    Vertex vv[4];
                    for (int c = 0; c < 4; c++) {
                        float px = (float)x + F.p[c][0];
                        float py = (float)y + F.p[c][1];
                        float pz = (float)z + F.p[c][2];
                        if (dynWater) {
                            // Dynamic water occupies the bottom wh of the cell.
                            py = (float)y + (F.p[c][1] == 1.0f ? wh : 0.0f);
                        } else if (b == WATER && f == 0) {
                            py -= 0.14f; // recessed ocean surface
                        }
                        float u, v;
                        if (b == LOG || b == WOOD) {
                            float tu = F.t[c][0], tv = F.t[c][1];
                            if (!grownLog && lf.tile != TEX_LOG_TOP)
                                orientLogSideUV(f, trunk, F.p[c][0], F.p[c][1], F.p[c][2], tu, tv);
                            logCornerUV(lf, tu, tv, F.p[c][0], F.p[c][1], F.p[c][2], u, v);
                        } else {
                            float u0, v0, u1, v1;
                            tex::tileUV(lf.tile, u0, v0, u1, v1);
                            u = u0 + (u1 - u0) * F.t[c][0];
                            v = v0 + (v1 - v0) * F.t[c][1];
                        }
                        float aof = vertexAO(wx, wy, wz, F.n[0], F.n[1], F.n[2],
                                            (int)F.p[c][0], (int)F.p[c][1], (int)F.p[c][2]);
                        vv[c] = { px, py, pz, u, v, (float)F.n[0], (float)F.n[1], (float)F.n[2],
                                  F.shade, aof, alpha };
                    }

                    std::vector<Vertex>& dst = isTrans ? ch.meshTransparent : ch.meshOpaque;
                    // Flip the split when the other diagonal is brighter so AO
                    // does not leave a dark crease across an otherwise flat face.
                    if (vv[0].ao + vv[2].ao > vv[1].ao + vv[3].ao) {
                        dst.push_back(vv[1]); dst.push_back(vv[2]); dst.push_back(vv[3]);
                        dst.push_back(vv[1]); dst.push_back(vv[3]); dst.push_back(vv[0]);
                    } else {
                        dst.push_back(vv[0]); dst.push_back(vv[1]); dst.push_back(vv[2]);
                        dst.push_back(vv[0]); dst.push_back(vv[2]); dst.push_back(vv[3]);
                    }

                    // Leaf frills: on air-facing faces, probabilistically add a
                    // crossed-quad "X" frill to smooth the crown/shrub silhouette.
                    if ((b == LEAVES || b == SHRUB_LEAF) && nb == AIR) {
                        uint32_t fh = noise::hash2(wx * 6 + f, wz * 6 + f, 0x1EA0u + (uint32_t)b);
                        if (noise::hash01(fh) < 0.30f) {
                            float fcx = (float)x + 0.5f + 0.5f * (float)F.n[0];
                            float fcy = (float)y + 0.5f + 0.5f * (float)F.n[1];
                            float fcz = (float)z + 0.5f + 0.5f * (float)F.n[2];
                            const float len = 0.25f; // 25% of the block edge
                            const float halfW = 0.68f; // ~96% of the face diagonal (sqrt2)
                            float t1x = 0.0f, t1y = 0.0f, t1z = 0.0f;
                            float t2x = 0.0f, t2y = 0.0f, t2z = 0.0f;
                            if (F.n[0] != 0) { t1y = 1.0f; t2z = 1.0f; }
                            else if (F.n[1] != 0) { t1x = 1.0f; t2z = 1.0f; }
                            else { t1x = 1.0f; t2y = 1.0f; }
                            // X arms lie along the face diagonals so the root spans the
                            // full diagonal of the block face.
                            const float INV_SQRT2 = 0.70710678f;
                            float d1x = (t1x + t2x) * INV_SQRT2, d1y = (t1y + t2y) * INV_SQRT2, d1z = (t1z + t2z) * INV_SQRT2;
                            float d2x = (t1x - t2x) * INV_SQRT2, d2y = (t1y - t2y) * INV_SQRT2, d2z = (t1z - t2z) * INV_SQRT2;

                            float lu0, lv0, lu1, lv1;
                            tex::tileUV((b == SHRUB_LEAF) ? TEX_SHRUB_LEAF_X : TEX_LEAF_X, lu0, lv0, lu1, lv1);
                            auto emitFrill = [&](float tx, float ty2, float tz) {
                                float bx0 = fcx - tx * halfW, by0 = fcy - ty2 * halfW, bz0 = fcz - tz * halfW;
                                float bx1 = fcx + tx * halfW, by1 = fcy + ty2 * halfW, bz1 = fcz + tz * halfW;
                                float ox0 = bx0 + (float)F.n[0] * len, oy0 = by0 + (float)F.n[1] * len, oz0 = bz0 + (float)F.n[2] * len;
                                float ox1 = bx1 + (float)F.n[0] * len, oy1 = by1 + (float)F.n[1] * len, oz1 = bz1 + (float)F.n[2] * len;
                                Vertex q[4] = {
                                    { bx0, by0, bz0, lu0, lv1, (float)F.n[0], (float)F.n[1], (float)F.n[2], F.shade, 1.0f, 1.0f },
                                    { bx1, by1, bz1, lu1, lv1, (float)F.n[0], (float)F.n[1], (float)F.n[2], F.shade, 1.0f, 1.0f },
                                    { ox1, oy1, oz1, lu1, lv0, (float)F.n[0], (float)F.n[1], (float)F.n[2], F.shade, 1.0f, 1.0f },
                                    { ox0, oy0, oz0, lu0, lv0, (float)F.n[0], (float)F.n[1], (float)F.n[2], F.shade, 1.0f, 1.0f },
                                };
                                ch.meshOpaque.push_back(q[0]); ch.meshOpaque.push_back(q[1]); ch.meshOpaque.push_back(q[2]);
                                ch.meshOpaque.push_back(q[0]); ch.meshOpaque.push_back(q[2]); ch.meshOpaque.push_back(q[3]);
                                ch.meshOpaque.push_back(q[0]); ch.meshOpaque.push_back(q[2]); ch.meshOpaque.push_back(q[1]);
                                ch.meshOpaque.push_back(q[0]); ch.meshOpaque.push_back(q[3]); ch.meshOpaque.push_back(q[2]);
                            };
                            emitFrill(d1x, d1y, d1z);
                            emitFrill(d2x, d2y, d2z);
                        }
                    }
                }
            }
        }
    }

    // Grass sod (草皮): thin single-face overlays on exposed dirt faces, drawn in
    // the opaque pass. Texture index follows the wither stage (green -> brown).
    // sodMask was built before the dirt faces so the dirt under sod stays visible.
    for (const Chunk::SodFace& sf : ch.sodFaces) {
        if (sf.face >= 6 || sf.stage >= cfg::SOD_STAGES) continue;
        if (ch.get(sf.x, sf.y, sf.z) != DIRT) continue; // dirt was replaced
        const geo::FaceDef& F = geo::kFaces[sf.face];
        size_t idx = ((size_t)sf.y * cfg::CHUNK_Z + sf.z) * cfg::CHUNK_X + sf.x;
        uint32_t mask = sodMask[idx];
        uint8_t tile = TEX_SOD_0 + sf.stage;
        float u0, v0, u1, v1;
        tex::tileUV(tile, u0, v0, u1, v1);
        const float off = 0.02f; // push slightly proud of the dirt face (no z-fight)
        const float pad = 0.03f; // in-plane overhang where a perpendicular sod meets
        float maxD = loot::sodBreak().durability;
        float rem = (sf.rem < 0.0f) ? maxD : sf.rem;
        float keep = (maxD > 0.001f) ? clampf(rem / maxD, 0.0f, 1.0f) : 1.0f;
        Vertex vv[4];
        for (int c = 0; c < 4; c++) {
            float px = (float)sf.x + F.p[c][0] + (float)F.n[0] * off;
            float py = (float)sf.y + F.p[c][1] + (float)F.n[1] * off;
            float pz = (float)sf.z + F.p[c][2] + (float)F.n[2] * off;
            // Extend each corner outward only along an edge whose perpendicular
            // face has sod. This closes top/side corner seams without overlapping
            // coplanar neighbors (which would z-fight).
            for (int a = 0; a < 3; a++) {
                if (F.n[a] != 0) continue; // skip the normal axis
                float corner = (a == 0) ? F.p[c][0] : (a == 1 ? F.p[c][1] : F.p[c][2]);
                int sign = (corner < 0.5f) ? -1 : 1;
                int perp = faceForNormal(a, sign);
                if (!(mask & (1u << perp))) continue; // no perpendicular sod here
                float d = (float)sign * pad;
                if (a == 0) px += d;
                else if (a == 1) py += d;
                else pz += d;
            }
            float u = u0 + (u1 - u0) * F.t[c][0];
            float v = v0 + (v1 - v0) * F.t[c][1];
            vv[c] = { px, py, pz, u, v, (float)F.n[0], (float)F.n[1], (float)F.n[2],
                      F.shade, keep, 1.0f };
        }
        ch.meshOpaque.push_back(vv[0]); ch.meshOpaque.push_back(vv[1]); ch.meshOpaque.push_back(vv[2]);
        ch.meshOpaque.push_back(vv[0]); ch.meshOpaque.push_back(vv[2]); ch.meshOpaque.push_back(vv[3]);
    }

    float bu0, bv0, bu1, bv1;
    tex::tileUV(TEX_BARK, bu0, bv0, bu1, bv1);
    const float barkOff = 0.03f;
    for (const Chunk::BarkFace& bf : ch.barkFaces) {
        if (bf.face >= 6) continue;
        if (!isSolid(ch.get(bf.x, bf.y, bf.z))) continue;
        const geo::FaceDef& F = geo::kFaces[bf.face];
        Vertex vv[4];
        for (int c = 0; c < 4; c++) {
            float px = (float)bf.x + F.p[c][0] + (float)F.n[0] * barkOff;
            float py = (float)bf.y + F.p[c][1] + (float)F.n[1] * barkOff;
            float pz = (float)bf.z + F.p[c][2] + (float)F.n[2] * barkOff;
            float u = bu0 + (bu1 - bu0) * F.t[c][0];
            float v = bv0 + (bv1 - bv0) * F.t[c][1];
            vv[c] = { px, py, pz, u, v, (float)F.n[0], (float)F.n[1], (float)F.n[2],
                      F.shade, 1.0f, 1.0f };
        }
        ch.meshOpaque.push_back(vv[0]); ch.meshOpaque.push_back(vv[1]); ch.meshOpaque.push_back(vv[2]);
        ch.meshOpaque.push_back(vv[0]); ch.meshOpaque.push_back(vv[2]); ch.meshOpaque.push_back(vv[3]);
    }

    bakeBlockLight(*this, ch, cx, cy, cz);

    ch.hasMesh = true;
    ch.uploaded = false;
}

void World::removeSodAt(Chunk& ch, int lx, int y, int lz) {
    for (size_t i = 0; i < ch.sodFaces.size();) {
        const Chunk::SodFace& sf = ch.sodFaces[i];
        if ((int)sf.x == lx && (int)sf.y == y && (int)sf.z == lz) {
            ch.sodFaces[i] = ch.sodFaces.back();
            ch.sodFaces.pop_back();
        } else {
            i++;
        }
    }
}

void World::removeBarkAt(Chunk& ch, int lx, int y, int lz) {
    for (size_t i = 0; i < ch.barkFaces.size();) {
        const Chunk::BarkFace& bf = ch.barkFaces[i];
        if ((int)bf.x == lx && (int)bf.y == y && (int)bf.z == lz) {
            ch.barkFaces[i] = ch.barkFaces.back();
            ch.barkFaces.pop_back();
        } else {
            i++;
        }
    }
}

int World::faceFromHitNormal(const Vec3& n) const {
    return faceFromDir(n);
}

bool World::hasBarkFace(int x, int y, int z, int face) const {
    CellLoc c;
    if (!cellLoc(x, y, z, c) || face < 0 || face > 5) return false;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return false;
    for (const Chunk::BarkFace& bf : it->second.barkFaces) {
        if ((int)bf.x == c.lx && (int)bf.y == c.ly && (int)bf.z == c.lz && (int)bf.face == face)
            return true;
    }
    return false;
}

bool World::addBarkFace(int x, int y, int z, int face) {
    CellLoc c;
    if (!cellLoc(x, y, z, c) || face < 0 || face > 5) return false;
    if (!isSolid(getBlock(x, y, z))) return false;
    if (hasBarkFace(x, y, z, face)) return false;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return false;
    it->second.barkFaces.push_back({ (uint8_t)c.lx, (uint8_t)c.lz, (uint8_t)c.ly, (uint8_t)face });
    it->second.dirty = true;
    it->second.modified = true;
    it->second.uploaded = false;
    rememberEdited(c.cx, c.cz);
    touchAuthBark(chunkKey(c.cx, c.cy, c.cz));
    m_meshQueue.push_back(chunkKey(c.cx, c.cy, c.cz));
    return true;
}

bool World::takeBarkFace(int x, int y, int z, int face) {
    CellLoc c;
    if (!cellLoc(x, y, z, c) || face < 0 || face > 5) return false;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return false;
    Chunk& ch = it->second;
    for (size_t i = 0; i < ch.barkFaces.size(); i++) {
        const Chunk::BarkFace& bf = ch.barkFaces[i];
        if ((int)bf.x == c.lx && (int)bf.y == c.ly && (int)bf.z == c.lz && (int)bf.face == face) {
            ch.barkFaces[i] = ch.barkFaces.back();
            ch.barkFaces.pop_back();
            ch.dirty = true;
            ch.modified = true;
            ch.uploaded = false;
            rememberEdited(c.cx, c.cz);
            touchAuthBark(chunkKey(c.cx, c.cy, c.cz));
            m_meshQueue.push_back(chunkKey(c.cx, c.cy, c.cz));
            return true;
        }
    }
    return false;
}

int World::takeAllBarkAt(int x, int y, int z) {
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return 0;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return 0;
    Chunk& ch = it->second;
    int n = 0;
    for (size_t i = 0; i < ch.barkFaces.size();) {
        const Chunk::BarkFace& bf = ch.barkFaces[i];
        if ((int)bf.x == c.lx && (int)bf.y == c.ly && (int)bf.z == c.lz) {
            ch.barkFaces[i] = ch.barkFaces.back();
            ch.barkFaces.pop_back();
            n++;
        } else {
            i++;
        }
    }
    if (n > 0) {
        ch.dirty = true;
        ch.modified = true;
        ch.uploaded = false;
        rememberEdited(c.cx, c.cz);
        touchAuthBark(chunkKey(c.cx, c.cy, c.cz));
        m_meshQueue.push_back(chunkKey(c.cx, c.cy, c.cz));
    }
    return n;
}

void World::rebuildSod(Chunk& ch, int cx, int cy, int cz) {
    ch.sodFaces.clear();
    for (int y = 0; y < cfg::CHUNK_Y; y++) {
        int wy = cy * cfg::CHUNK_Y + y;
        for (int z = 0; z < cfg::CHUNK_Z; z++) {
            for (int x = 0; x < cfg::CHUNK_X; x++) {
                int wx = cx * cfg::CHUNK_X + x;
                int wz = cz * cfg::CHUNK_Z + z;
                if (!isSurfaceDirt(ch, x, y, z, getBlock(wx, wy + 1, wz))) continue;
                for (int i = 0; i < SOD_FACE_COUNT; i++) {
                    int f = SOD_FACES[i];
                    const geo::FaceDef& F = geo::kFaces[f];
                    int nx = wx + F.n[0], ny = wy + F.n[1], nz = wz + F.n[2];
                    if (F.n[1] == 0) {
                        int ncx = floorDiv(nx, cfg::CHUNK_X);
                        int ncy = floorDiv(ny, cfg::CHUNK_Y);
                        int ncz = floorDiv(nz, cfg::CHUNK_Z);
                        if (!chunkExists(ncx, ncy, ncz)) continue;
                    }
                    if (isSodAir(getBlock(nx, ny, nz))) {
                        ch.sodFaces.push_back({ (uint8_t)x, (uint8_t)z, (uint8_t)y, (uint8_t)f, 0 });
                    }
                }
            }
        }
    }
    ch.sodValid = true;
}

void World::addBorderSod(int cx, int cy, int cz) {
    // A chunk's border side faces could not be decided until this chunk existed
    // (rebuildSod defers them). Now that (cx,cz) is loaded, fill in the side sod
    // of its four neighbors whose surface-dirt border faces touch air in here.
    struct Dir { int ncx, ncz; int fixAxis; int fixVal; int face; };
    const Dir dirs[4] = {
        { cx - 1, cz,     0, cfg::CHUNK_X - 1, 2 }, // west neighbor, lx=15, +X face
        { cx + 1, cz,     0, 0,                  3 }, // east neighbor, lx=0,  -X face
        { cx,     cz - 1, 2, cfg::CHUNK_Z - 1, 4 }, // north neighbor, lz=15, +Z face
        { cx,     cz + 1, 2, 0,                  5 }, // south neighbor, lz=0,  -Z face
    };
    for (const Dir& d : dirs) {
        Chunk* nch = getChunk(d.ncx, cy, d.ncz);
        if (!nch) continue;
        bool changed = false;
        const geo::FaceDef& F = geo::kFaces[d.face];
        for (int y = 0; y < cfg::CHUNK_Y; y++) {
            int wy = cy * cfg::CHUNK_Y + y;
            for (int v = 0; v < cfg::CHUNK_X; v++) {
                int lx = (d.fixAxis == 0) ? d.fixVal : v;
                int lz = (d.fixAxis == 2) ? d.fixVal : v;
                int wx = d.ncx * cfg::CHUNK_X + lx;
                int wz = d.ncz * cfg::CHUNK_Z + lz;
                if (!isSurfaceDirt(*nch, lx, y, lz, getBlock(wx, wy + 1, wz))) continue;
                if (!isSodAir(getBlock(wx + F.n[0], wy + F.n[1], wz + F.n[2]))) continue;
                bool present = false;
                for (const auto& sf : nch->sodFaces) {
                    if ((int)sf.x == lx && (int)sf.y == y && (int)sf.z == lz && (int)sf.face == d.face) {
                        present = true;
                        break;
                    }
                }
                if (present) continue;
                nch->sodFaces.push_back({ (uint8_t)lx, (uint8_t)lz, (uint8_t)y, (uint8_t)d.face, 0 });
                changed = true;
            }
        }
        if (changed) {
            nch->dirty = true;
            m_meshQueue.push_back(chunkKey(d.ncx, cy, d.ncz));
        }
    }
}

void World::sodTick(int budget, uint64_t tick) {
    if (budget <= 0 || m_chunks.empty()) return;
    std::vector<int64_t> keys;
    keys.reserve(m_chunks.size());
    for (const auto& kv : m_chunks) keys.push_back(kv.first);
    if (m_sodCursor >= keys.size()) m_sodCursor = 0;

    int processed = 0;
    size_t guard = keys.size(); // one full round at most, even if budget remains
    while (processed < budget && guard-- > 0) {
        int64_t key = keys[m_sodCursor];
        m_sodCursor = (m_sodCursor + 1) % keys.size();
        auto it = m_chunks.find(key);
        if (it == m_chunks.end() || it->second.sodFaces.empty()) continue;
        Chunk& ch = it->second;
        int cx = chunkCX(key), cy = chunkCY(key), cz = chunkCZ(key);

        size_t i = 0;
        while (i < ch.sodFaces.size() && processed < budget) {
            Chunk::SodFace& sf = ch.sodFaces[i];
            processed++;
            int wx = cx * cfg::CHUNK_X + sf.x;
            int wz = cz * cfg::CHUNK_Z + sf.z;
            const geo::FaceDef& F = geo::kFaces[sf.face];
            int wy = cy * cfg::CHUNK_Y + (int)sf.y;
            uint8_t nb = getBlock(wx + F.n[0], wy + F.n[1], wz + F.n[2]);

            if (isSodAir(nb)) {
                // Re-exposed: regrow green and forget any wither progress.
                if (sf.stage != 0) {
                    sf.stage = 0;
                    ch.dirty = true;
                    ch.modified = true;
                    rememberEdited(cx, cz);
                    touchAuthSod(key);
                    m_meshQueue.push_back(key);
                }
                sf.coveredTick = UINT64_MAX;
                i++;
            } else {
                // Covered: advance one wither stage per WITHER_STAGE_TICKS.
                if (sf.coveredTick == UINT64_MAX) sf.coveredTick = tick;
                if (tick - sf.coveredTick >= (uint64_t)cfg::WITHER_STAGE_TICKS) {
                    sf.coveredTick = tick;
                    if (sf.stage + 1 >= cfg::SOD_STAGES) {
                        // Last stage reached: remove the sod entirely.
                        ch.sodFaces[i] = ch.sodFaces.back();
                        ch.sodFaces.pop_back();
                        ch.dirty = true;
                        ch.modified = true;
                        rememberEdited(cx, cz);
                        touchAuthSod(key);
                        m_meshQueue.push_back(key);
                        continue; // do not advance i: a different face moved into this slot
                    } else {
                        sf.stage++;
                        ch.dirty = true;
                        ch.modified = true;
                        rememberEdited(cx, cz);
                        touchAuthSod(key);
                        m_meshQueue.push_back(key);
                    }
                }
                i++;
            }
        }
    }
}

int World::airHumidity(int x, int y, int z) const {
    // Deterministic air-humidity field in [-256, 255] (negative = dry air).
    // Very low base frequency distributes dry/humid regions at biome scale
    // (hundreds of blocks across), with multi-octave detail for smooth internal
    // variation and gentle transitions between biomes.
    float n = noise::fbm3((float)x * 0.002f, (float)y * 0.002f, (float)z * 0.002f, 4, m_seed + 12345u);
    int h = (int)(n * 511.0f) - 300;
    if (h < cfg::HUMIDITY_MIN) h = cfg::HUMIDITY_MIN;
    if (h > cfg::HUMIDITY_MAX) h = cfg::HUMIDITY_MAX;
    return h;
}

void World::setWaterCell(int x, int y, int z, uint8_t level, bool markModified) {
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return;
    if (!ensureLoadedSlice(c.cx, c.cy, c.cz)) return;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return;
    int64_t key = chunkKey(c.cx, c.cy, c.cz);
    if (level == 0) {
        it->second.set(c.lx, c.ly, c.lz, AIR);
        it->second.setLevel(c.lx, c.ly, c.lz, 0);
    } else {
        if (level > cfg::WATER_MAX_LEVEL) level = cfg::WATER_MAX_LEVEL;
        it->second.set(c.lx, c.ly, c.lz, WATER);
        it->second.setLevel(c.lx, c.ly, c.lz, level);
        it->second.hasWater = true;
    }
    it->second.dirty = true;
    if (markModified) {
        it->second.modified = true;
        rememberEdited(c.cx, c.cz);
    }
    touchAuth(x, y, z);
    m_meshQueue.push_back(key);
}

int World::downhillSteps(int x, int y, int z, int dx, int dz) const {
    // Explore the horizontal plane in direction (dx,dz) using "forward" (along d)
    // and "translate" (perpendicular) steps, looking for the nearest column whose
    // ground has dropped below the water's support level (y-1). Fewer steps means
    // the terrain is more downhill, i.e. a higher-priority spread direction.
    int tx = dz, tz = -dx; // one perpendicular direction
    for (int s = 1; s <= 3; s++) {
        for (int f = 0; f <= s; f++) {
            int t = s - f;
            for (int sign = -1; sign <= 1; sign++) {
                if (t == 0 && sign != 0) continue;
                int cx = x + f * dx + t * sign * tx;
                int cz = z + f * dz + t * sign * tz;
                if (getBlock(cx, y, cz) == AIR && getBlock(cx, y - 1, cz) == AIR) return s;
            }
        }
    }
    return 4; // no lower plane within the exploration budget
}

void World::processWaterCell(int x, int y, int z, uint8_t level, uint64_t tick) {
    if (level == 0 || level > cfg::WATER_MAX_LEVEL) return;
    if (getBlock(x, y, z) != WATER) return;

    // Evaporation: only when the water touches air and the local air is dry.
    {
        bool touchesAir = (getBlock(x + 1, y, z) == AIR || getBlock(x - 1, y, z) == AIR ||
                           getBlock(x, y + 1, z) == AIR || getBlock(x, y - 1, z) == AIR ||
                           getBlock(x, y, z + 1) == AIR || getBlock(x, y, z - 1) == AIR);
        int h = airHumidity(x, y, z);
        if (touchesAir && h < 0) {
            int t = 8 + h / 32;               // linear positive correlation with h (h<0)
            if (t < 1) t = 1;
            uint32_t off = noise::hash3(x, y, z, m_seed) % (uint32_t)t;
            if ((tick + off) % (uint32_t)t == 0) {
                float u = (float)h / 160.0f;  // [-1.6, 0)
                float p = u * u;              // convex (下凸), negative correlation
                if (p > 1.0f) p = 1.0f;
                float r = noise::hash01(noise::hash3(x, y, z, m_seed ^ 0xABCDEFu ^ (uint32_t)tick));
                if (r < p) {
                    if (level <= 1) { setWaterCell(x, y, z, 0, true); return; }
                    setWaterCell(x, y, z, (uint8_t)(level - 1), true);
                    level--;
                }
            }
        }
    }

    // Flow: move down when below is air, or fall & merge when below is liquid
    // (prevents water from stacking on top of water).
    uint8_t below = getBlock(x, y - 1, z);
    if (below == AIR) {
        setWaterCell(x, y, z, 0, true);
        setWaterCell(x, y - 1, z, level, true);
        return;
    } else if (below == WATER) {
        uint8_t belowLevel = getWaterLevel(x, y - 1, z);
        if (belowLevel == 0) {
            // Static ocean water is already a full block: the water is absorbed.
            setWaterCell(x, y, z, 0, true);
        } else {
            int merged = (int)belowLevel + level;
            if (merged > cfg::WATER_MAX_LEVEL) merged = cfg::WATER_MAX_LEVEL;
            setWaterCell(x, y, z, 0, true);
            setWaterCell(x, y - 1, z, (uint8_t)merged, true);
        }
        return;
    }

    // Diffusion: a grounded water block (level > 1) transfers
    //   take = n * floor(level / (n + 1))
    // to its n horizontal targets (air or lower-level water), each receiving
    // floor(level / (n + 1)). When that yields zero, a level >= 3 block still tries
    // to pass 1 level toward the most-downhill direction(s).
    if (level > 1) {
        int tx[4], tz[4], dxs[4], dzs[4], n = 0;
        const int DX[4] = { 1, -1, 0, 0 };
        const int DZ[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], nz = z + DZ[d];
            uint8_t nb = getBlock(nx, y, nz);
            if (nb == AIR) { tx[n] = nx; tz[n] = nz; dxs[n] = DX[d]; dzs[n] = DZ[d]; n++; }
            else if (nb == WATER) {
                uint8_t nl = getWaterLevel(nx, y, nz);
                if (nl > 0 && nl < level) { tx[n] = nx; tz[n] = nz; dxs[n] = DX[d]; dzs[n] = DZ[d]; n++; }
            }
        }
        if (n > 0) {
            int take = n * (level / (n + 1));
            if (take > 0) {
                int perChild = level / (n + 1);
                for (int i = 0; i < n; i++) {
                    uint8_t nl = getWaterLevel(tx[i], y, tz[i]);
                    uint8_t nl2 = (uint8_t)(nl + perChild);
                    if (nl2 > cfg::WATER_MAX_LEVEL) nl2 = cfg::WATER_MAX_LEVEL;
                    setWaterCell(tx[i], y, tz[i], nl2, true);
                }
                int nm = level - take;
                if (nm < 1) nm = 1;
                if (nm < (int)level) setWaterCell(x, y, z, (uint8_t)nm, true);
            } else if (level >= 3) {
                // Stall fallback: pass 1 level to the most-downhill direction(s).
                int best = 1000;
                for (int i = 0; i < n; i++) {
                    int s = downhillSteps(x, y, z, dxs[i], dzs[i]);
                    if (s < best) best = s;
                }
                int pick[4], m = 0;
                for (int i = 0; i < n; i++) {
                    if (downhillSteps(x, y, z, dxs[i], dzs[i]) == best) pick[m++] = i;
                }
                if (m > 0) {
                    if ((int)level - m >= 1) {
                        for (int j = 0; j < m; j++) {
                            int i = pick[j];
                            uint8_t nl = getWaterLevel(tx[i], y, tz[i]);
                            uint8_t nl2 = (uint8_t)(nl + 1);
                            if (nl2 > cfg::WATER_MAX_LEVEL) nl2 = cfg::WATER_MAX_LEVEL;
                            setWaterCell(tx[i], y, tz[i], nl2, true);
                        }
                        setWaterCell(x, y, z, (uint8_t)(level - m), true);
                    } else {
                        int r = (int)(noise::hash01(noise::hash3(x, y, z, m_seed ^ 0x5A5Au ^ (uint32_t)tick)) * (float)m);
                        if (r >= m) r = m - 1;
                        int i = pick[r];
                        uint8_t nl = getWaterLevel(tx[i], y, tz[i]);
                        uint8_t nl2 = (uint8_t)(nl + 1);
                        if (nl2 > cfg::WATER_MAX_LEVEL) nl2 = cfg::WATER_MAX_LEVEL;
                        setWaterCell(tx[i], y, tz[i], nl2, true);
                        setWaterCell(x, y, z, (uint8_t)(level - 1), true);
                    }
                }
            }
        }
    }
}

void World::waterTick(uint64_t tick) {
    // Snapshot the dynamic water cells first so each cell advances at most once per
    // tick (no cascade from flow/diffusion within a single tick).
    struct Cell { int x, y, z; };
    std::vector<Cell> cells;
    for (auto& [key, ch] : m_chunks) {
        if (!ch.hasWater) continue;
        int cx = chunkCX(key), cy = chunkCY(key), cz = chunkCZ(key);
        bool any = false;
        for (int y = 0; y < cfg::CHUNK_Y; y++) {
            int wy = cy * cfg::CHUNK_Y + y;
            for (int z = 0; z < cfg::CHUNK_Z; z++) {
                for (int x = 0; x < cfg::CHUNK_X; x++) {
                    int idx = (y * cfg::CHUNK_Z + z) * cfg::CHUNK_X + x;
                    if (ch.blocks[idx] == WATER && ch.waterLevel[idx] > 0) {
                        cells.push_back({ cx * cfg::CHUNK_X + x, wy, cz * cfg::CHUNK_Z + z });
                        any = true;
                    }
                }
            }
        }
        if (!any) ch.hasWater = false; // stale flag cleanup
    }
    for (const Cell& c : cells) {
        uint8_t lvl = getWaterLevel(c.x, c.y, c.z);
        if (lvl == 0) continue;
        processWaterCell(c.x, c.y, c.z, lvl, tick);
    }
}

bool World::raycast(const Vec3& origin, const Vec3& dir, float maxDist,
                    IVec3& hitBlock, IVec3& prevBlock, Vec3& hitNormal,
                    int* physIsland, float* hitT) const {
    if (physIsland) *physIsland = -1;
    if (hitT) *hitT = maxDist + 1.0f;
    const float S = cfg::BLOCK_SCALE;
    int x = (int)std::floor(origin.x / S), y = (int)std::floor(origin.y / S), z = (int)std::floor(origin.z / S);
    int stepX = dir.x > 0 ? 1 : -1;
    int stepY = dir.y > 0 ? 1 : -1;
    int stepZ = dir.z > 0 ? 1 : -1;

    const float INF = 1e30f;
    float tMaxX = dir.x != 0.0f ? ((stepX > 0 ? ((x + 1) * S - origin.x) : (origin.x - x * S)) / std::fabs(dir.x)) : INF;
    float tMaxY = dir.y != 0.0f ? ((stepY > 0 ? ((y + 1) * S - origin.y) : (origin.y - y * S)) / std::fabs(dir.y)) : INF;
    float tMaxZ = dir.z != 0.0f ? ((stepZ > 0 ? ((z + 1) * S - origin.z) : (origin.z - z * S)) / std::fabs(dir.z)) : INF;
    float tDeltaX = dir.x != 0.0f ? std::fabs(S / dir.x) : INF;
    float tDeltaY = dir.y != 0.0f ? std::fabs(S / dir.y) : INF;
    float tDeltaZ = dir.z != 0.0f ? std::fabs(S / dir.z) : INF;

    IVec3 prev{ x, y, z };
    int steps = (int)(maxDist / S * 3.0f) + 8;
    bool worldHit = false;
    float bestT = maxDist + 1.0f;
    IVec3 bestHit, bestPrev;
    Vec3 bestN;
    for (int i = 0; i < steps; i++) {
        float t;
        Vec3 normal;
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX; t = tMaxX; tMaxX += tDeltaX; normal = { (float)-stepX, 0, 0 };
        } else if (tMaxY < tMaxZ) {
            y += stepY; t = tMaxY; tMaxY += tDeltaY; normal = { 0, (float)-stepY, 0 };
        } else {
            z += stepZ; t = tMaxZ; tMaxZ += tDeltaZ; normal = { 0, 0, (float)-stepZ };
        }
        if (t > maxDist) break;
        uint8_t b = getBlock(x, y, z);
        if (structure::isGuardianToken(x, y, z, b)) b = AIR;
        if (b != AIR) {
            worldHit = true;
            bestT = t;
            bestHit = { x, y, z };
            bestPrev = prev;
            bestN = normal;
            break;
        }
        prev = { x, y, z };
    }

    if (physIsland) {
        for (int pi = 0; pi < (int)m_phys.size(); pi++) {
            const PhysicsIsland& isl = m_phys[(size_t)pi];
            if (isl.cells.empty()) continue;
            Vec3 restO = tree_fall::restOf(isl, origin);
            Vec3 restD = tree_fall::unrotate(isl, dir);
            int px = (int)std::floor(restO.x / S);
            int py = (int)std::floor(restO.y / S);
            int pz = (int)std::floor(restO.z / S);
            int sx = restD.x > 0 ? 1 : -1;
            int sy = restD.y > 0 ? 1 : -1;
            int sz = restD.z > 0 ? 1 : -1;
            float pMaxX = restD.x != 0.0f ? ((sx > 0 ? ((px + 1) * S - restO.x) : (restO.x - px * S)) / std::fabs(restD.x)) : INF;
            float pMaxY = restD.y != 0.0f ? ((sy > 0 ? ((py + 1) * S - restO.y) : (restO.y - py * S)) / std::fabs(restD.y)) : INF;
            float pMaxZ = restD.z != 0.0f ? ((sz > 0 ? ((pz + 1) * S - restO.z) : (restO.z - pz * S)) / std::fabs(restD.z)) : INF;
            float pDelX = restD.x != 0.0f ? std::fabs(S / restD.x) : INF;
            float pDelY = restD.y != 0.0f ? std::fabs(S / restD.y) : INF;
            float pDelZ = restD.z != 0.0f ? std::fabs(S / restD.z) : INF;
            IVec3 pprev{ px - isl.originX, py - isl.originY, pz - isl.originZ };
            int psteps = (int)(maxDist / S * 3.0f) + 8;
            for (int i = 0; i < psteps; i++) {
                float t;
                Vec3 nrest;
                if (pMaxX < pMaxY && pMaxX < pMaxZ) {
                    px += sx; t = pMaxX; pMaxX += pDelX; nrest = { (float)-sx, 0, 0 };
                } else if (pMaxY < pMaxZ) {
                    py += sy; t = pMaxY; pMaxY += pDelY; nrest = { 0, (float)-sy, 0 };
                } else {
                    pz += sz; t = pMaxZ; pMaxZ += pDelZ; nrest = { 0, 0, (float)-sz };
                }
                if (t > maxDist || t >= bestT) break;
                int lx = px - isl.originX, ly = py - isl.originY, lz = pz - isl.originZ;
                uint8_t b = isl.get(lx, ly, lz);
                if (b != AIR && !isl.hiddenAt(lx, ly, lz)) {
                    bestT = t;
                    worldHit = true;
                    bestHit = { lx, ly, lz };
                    bestPrev = pprev;
                    bestN = tree_fall::rotate(isl, nrest);
                    *physIsland = pi;
                    break;
                }
                pprev = { lx, ly, lz };
            }
        }
    }

    if (!worldHit) return false;
    hitBlock = bestHit;
    prevBlock = bestPrev;
    hitNormal = bestN;
    if (hitT) *hitT = bestT;
    return true;
}

uint8_t World::getPhysBlock(int island, int x, int y, int z) const {
    if (island < 0 || island >= (int)m_phys.size()) return AIR;
    return m_phys[(size_t)island].get(x, y, z);
}

bool World::breakPhysBlock(int island, int x, int y, int z, uint8_t& dropped) {
    dropped = AIR;
    if (island < 0 || island >= (int)m_phys.size()) return false;
    PhysicsIsland& t = m_phys[(size_t)island];
    uint8_t b = t.get(x, y, z);
    if (b == AIR || t.hiddenAt(x, y, z)) return false;
    dropped = b;
    clearBlockDur(island, x, y, z);
    for (int f = 0; f < 6; f++) {
        const geo::FaceDef& F = geo::kFaces[f];
        int nx = x + F.n[0], ny = y + F.n[1], nz = z + F.n[2];
        if (!t.inBounds(nx, ny, nz) || !isTreeWood(t.get(nx, ny, nz))) continue;
        int i = t.index(nx, ny, nz);
        t.flags[(size_t)i] = (uint8_t)(t.flags[(size_t)i] | flagCutFace(oppositeFace(f)));
    }
    tree_fall::setCell(t, x, y, z, AIR, 0);
    t.contentRev++;
    tree_fall::recomputeMass(t);
    t.meshDirty = true;
    tree_fall::buildMesh(t);
    if (t.cells.empty()) {
        forgetIslandMines((size_t)island);
        m_phys[(size_t)island] = std::move(m_phys.back());
        m_phys.pop_back();
        return true;
    }
    splitIsland((size_t)island);
    return true;
}

bool World::placePhysBlock(int island, int x, int y, int z, uint8_t b) {
    if (island < 0 || island >= (int)m_phys.size()) return false;
    PhysicsIsland& t = m_phys[(size_t)island];
    if (!t.inBounds(x, y, z)) return false;
    if (t.get(x, y, z) != AIR) return false;
    tree_fall::setCell(t, x, y, z, b, 0);
    tree_fall::recomputeMass(t);
    t.meshDirty = true;
    tree_fall::buildMesh(t);
    return true;
}

void World::resolvePhysPlayer(Vec3& pos, Vec3& vel, int axis, float delta,
                              float hw, float hgt, bool& onGround) const {
    if (delta == 0.0f || m_phys.empty()) return;
    const float S = cfg::BLOCK_SCALE;
    Vec3 mn{ pos.x - hw, pos.y, pos.z - hw };
    Vec3 mx{ pos.x + hw, pos.y + hgt, pos.z + hw };
    Vec3 corners[8] = {
        { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mn.x, mx.y, mn.z }, { mx.x, mx.y, mn.z },
        { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z }, { mn.x, mx.y, mx.z }, { mx.x, mx.y, mx.z }
    };
    for (const PhysicsIsland& isl : m_phys) {
        if (isl.cells.empty()) continue;
        Vec3 rmin = tree_fall::restOf(isl, corners[0]);
        Vec3 rmax = rmin;
        for (int i = 1; i < 8; i++) {
            Vec3 r = tree_fall::restOf(isl, corners[i]);
            rmin.x = std::min(rmin.x, r.x); rmax.x = std::max(rmax.x, r.x);
            rmin.y = std::min(rmin.y, r.y); rmax.y = std::max(rmax.y, r.y);
            rmin.z = std::min(rmin.z, r.z); rmax.z = std::max(rmax.z, r.z);
        }
        int x0 = (int)std::floor(rmin.x / S) - isl.originX - 1;
        int y0 = (int)std::floor(rmin.y / S) - isl.originY - 1;
        int z0 = (int)std::floor(rmin.z / S) - isl.originZ - 1;
        int x1 = (int)std::floor(rmax.x / S) - isl.originX + 1;
        int y1 = (int)std::floor(rmax.y / S) - isl.originY + 1;
        int z1 = (int)std::floor(rmax.z / S) - isl.originZ + 1;
        for (int lx = x0; lx <= x1; lx++)
            for (int ly = y0; ly <= y1; ly++)
                for (int lz = z0; lz <= z1; lz++) {
                    uint8_t b = isl.get(lx, ly, lz);
                    if (!isSolid(b) || isl.hiddenAt(lx, ly, lz)) continue;
                    Vec3 wmin{ 1e9f, 1e9f, 1e9f }, wmax{ -1e9f, -1e9f, -1e9f };
                    for (int cy = 0; cy <= 1; cy++)
                        for (int cz = 0; cz <= 1; cz++)
                            for (int cx = 0; cx <= 1; cx++) {
                                Vec3 rest{
                                    (float)(isl.originX + lx + cx) * S,
                                    (float)(isl.originY + ly + cy) * S,
                                    (float)(isl.originZ + lz + cz) * S
                                };
                                Vec3 w = isl.com + tree_fall::rotate(isl, rest - isl.restCom);
                                wmin.x = std::min(wmin.x, w.x); wmax.x = std::max(wmax.x, w.x);
                                wmin.y = std::min(wmin.y, w.y); wmax.y = std::max(wmax.y, w.y);
                                wmin.z = std::min(wmin.z, w.z); wmax.z = std::max(wmax.z, w.z);
                            }
                    if (mx.x <= wmin.x || mn.x >= wmax.x) continue;
                    if (mx.y <= wmin.y || mn.y >= wmax.y) continue;
                    if (mx.z <= wmin.z || mn.z >= wmax.z) continue;
                    if (axis == 0) {
                        pos.x = (delta > 0.0f) ? (wmin.x - hw - 1e-4f) : (wmax.x + hw + 1e-4f);
                        vel.x = 0.0f;
                    } else if (axis == 1) {
                        if (delta > 0.0f) pos.y = wmin.y - hgt - 1e-4f;
                        else { pos.y = wmax.y + 1e-4f; onGround = true; }
                        vel.y = 0.0f;
                    } else {
                        pos.z = (delta > 0.0f) ? (wmin.z - hw - 1e-4f) : (wmax.z + hw + 1e-4f);
                        vel.z = 0.0f;
                    }
                    return;
                }
    }
}

namespace {

struct IVec3Hash {
    size_t operator()(const IVec3& v) const noexcept {
        size_t h = (size_t)(uint32_t)v.x;
        h = h * 1973u + (size_t)(uint32_t)v.y;
        h = h * 1973u + (size_t)(uint32_t)v.z;
        return h;
    }
};

} // namespace

void World::writeCell(int x, int y, int z, uint8_t b, uint8_t flags, bool markModified) {
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return;
    Chunk* ch = ensureLoadedSlice(c.cx, c.cy, c.cz);
    if (!ch) return;
    ch->set(c.lx, c.ly, c.lz, b);
    ch->setFlag(c.lx, c.ly, c.lz, flags);
    ch->setTreeId(c.lx, c.ly, c.lz, 0);
    if (b == WATER) {
        ch->setLevel(c.lx, c.ly, c.lz, cfg::WATER_SOURCE_LEVEL);
        ch->hasWater = true;
    } else {
        ch->setLevel(c.lx, c.ly, c.lz, 0);
    }
    if (b != DIRT) removeSodAt(*ch, c.lx, c.ly, c.lz);
    removeBarkAt(*ch, c.lx, c.ly, c.lz);
    ch->dirty = true;
    if (markModified) {
        ch->modified = true;
        rememberEdited(c.cx, c.cz);
    }
    touchAuth(x, y, z);
    touchAuthSod(chunkKey(c.cx, c.cy, c.cz));
    touchAuthBark(chunkKey(c.cx, c.cy, c.cz));
}

void World::rebuildTouched(const std::vector<IVec3>& cells) {
    std::unordered_set<int64_t> keys;
    auto add = [&](int cx, int cy, int cz) {
        if (cy < 0 || cy >= cfg::CHUNK_LAYERS) return;
        keys.insert(chunkKey(cx, cy, cz));
    };
    for (const IVec3& c : cells) {
        CellLoc loc;
        if (!cellLoc(c.x, c.y, c.z, loc)) continue;
        add(loc.cx, loc.cy, loc.cz);
        if (loc.lx == 0) add(loc.cx - 1, loc.cy, loc.cz);
        if (loc.lx == cfg::CHUNK_X - 1) add(loc.cx + 1, loc.cy, loc.cz);
        if (loc.lz == 0) add(loc.cx, loc.cy, loc.cz - 1);
        if (loc.lz == cfg::CHUNK_Z - 1) add(loc.cx, loc.cy, loc.cz + 1);
        if (loc.ly == 0) add(loc.cx, loc.cy - 1, loc.cz);
        if (loc.ly == cfg::CHUNK_Y - 1) add(loc.cx, loc.cy + 1, loc.cz);
    }
    for (int64_t key : keys) {
        auto it = m_chunks.find(key);
        if (it == m_chunks.end()) continue;
        buildMeshFor(it->second, chunkCX(key), chunkCY(key), chunkCZ(key));
        it->second.dirty = false;
    }
}

void World::spawnFallingTree(const std::vector<IVec3>& cells, int cutX, int cutY, int cutZ) {
    if (cells.empty()) return;
    const float S = cfg::BLOCK_SCALE;
    std::vector<IVec3> kept;
    kept.reserve(cells.size());
    for (const IVec3& c : cells) {
        uint8_t b = getBlock(c.x, c.y, c.z);
        if (isTreePart(b)) kept.push_back(c);
    }
    if (kept.empty()) return;

    int minx = kept[0].x, maxx = minx;
    int miny = kept[0].y, maxy = miny;
    int minz = kept[0].z, maxz = minz;
    for (const IVec3& c : kept) {
        minx = std::min(minx, c.x); maxx = std::max(maxx, c.x);
        miny = std::min(miny, c.y); maxy = std::max(maxy, c.y);
        minz = std::min(minz, c.z); maxz = std::max(maxz, c.z);
    }
    const int kMax = 64;
    if (maxx - minx + 1 > kMax) maxx = minx + kMax - 1;
    if (maxy - miny + 1 > kMax) maxy = miny + kMax - 1;
    if (maxz - minz + 1 > kMax) maxz = minz + kMax - 1;

    PhysicsIsland t;
    tree_fall::allocate(t, maxx - minx + 1, maxy - miny + 1, maxz - minz + 1);
    t.originX = minx; t.originY = miny; t.originZ = minz;
    for (const IVec3& c : kept) {
        if (c.x < minx || c.x > maxx || c.y < miny || c.y > maxy || c.z < minz || c.z > maxz) continue;
        uint8_t b = getBlock(c.x, c.y, c.z);
        uint8_t fl = (uint8_t)(getFlags(c.x, c.y, c.z) | FLAG_ALIVE);
        uint32_t bind = getTreeId(c.x, c.y, c.z);
        for (int f = 0; f < 6; f++) {
            const geo::FaceDef& F = geo::kFaces[f];
            if (c.x + F.n[0] == cutX && c.y + F.n[1] == cutY && c.z + F.n[2] == cutZ)
                fl = (uint8_t)(fl | flagCutFace(f));
        }
        tree_fall::setCell(t, c.x - minx, c.y - miny, c.z - minz, b, fl, bind);
    }
    if (t.cells.empty()) return;
    tree_fall::recomputeMass(t);
    t.com = t.restCom;
    Vec3 cut{ ((float)cutX + 0.5f) * S, ((float)cutY + 0.5f) * S, ((float)cutZ + 0.5f) * S };
    tree_fall::hingeAt(t, cut);
    t.meshDirty = true;
    tree_fall::buildMesh(t);

    for (const IVec3& c : kept) writeCell(c.x, c.y, c.z, AIR, 0, true);
    rebuildTouched(kept);
    m_phys.push_back(std::move(t));
    tagIsland(m_phys.back());
    splitIsland(m_phys.size() - 1);
}

void World::detachAliveTree(int x, int y, int z, uint32_t bindId) {
    if (!m_localTrees) return;
    auto alivePart = [&](int px, int py, int pz) {
        if (py < 0 || py >= cfg::WORLD_H) return false;
        uint8_t b = getBlock(px, py, pz);
        return isTreePart(b) && isAlive(px, py, pz);
    };
    auto ownedPart = [&](int px, int py, int pz) {
        if (!alivePart(px, py, pz)) return false;
        uint32_t id = getTreeId(px, py, pz);
        if (bindId != 0) return id == bindId;
        return id == 0;
    };
    auto ownedWood = [&](int px, int py, int pz) {
        if (py < 0 || py >= cfg::WORLD_H) return false;
        if (!isTreeWood(getBlock(px, py, pz)) || !isAlive(px, py, pz)) return false;
        uint32_t id = getTreeId(px, py, pz);
        if (bindId != 0) return id == bindId;
        return id == 0;
    };
    auto grounded = [&](int px, int py, int pz) {
        if (py <= 0) return true;
        uint8_t b = getBlock(px, py - 1, pz);
        return b != AIR && !isLiquid(b) && !isPassableCutout(b);
    };

    const int k6[6][3] = {
        { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
    };
    const int kMax = 8000;

    auto colKey = [](int cx, int cz) {
        return ((cx + 4096) << 16) ^ (cz + 4096);
    };
    std::unordered_set<int> cols;
    cols.insert(colKey(x, z));
    for (int z0 = z - 1; z0 <= z; z0++) {
        for (int x0 = x - 1; x0 <= x; x0++) {
            int remain = 0, below = 0, above = 0;
            for (int dz = 0; dz <= 1; dz++) {
                for (int dx = 0; dx <= 1; dx++) {
                    int nx = x0 + dx, nz = z0 + dz;
                    if (!(nx == x && nz == z) && ownedWood(nx, y, nz)) remain++;
                    if (ownedWood(nx, y - 1, nz)) below++;
                    if (ownedWood(nx, y + 1, nz)) above++;
                }
            }
            bool thick = (remain + 1 >= 3) || below >= 3 || above >= 3;
            if (!thick) continue;
            for (int dz = 0; dz <= 1; dz++) {
                for (int dx = 0; dx <= 1; dx++) {
                    int nx = x0 + dx, nz = z0 + dz;
                    if (ownedWood(nx, y, nz) || ownedWood(nx, y - 1, nz) || (nx == x && nz == z))
                        cols.insert(colKey(nx, nz));
                }
            }
        }
    }
    auto inCols = [&](int cx, int cz) { return cols.count(colKey(cx, cz)) != 0; };

    std::unordered_set<IVec3, IVec3Hash> owned;
    std::queue<IVec3> oq;
    auto seedOwned = [&](int px, int py, int pz) {
        if (!ownedPart(px, py, pz)) return;
        IVec3 n{ px, py, pz };
        if (owned.insert(n).second) oq.push(n);
    };
    for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                seedOwned(x + dx, y + dy, z + dz);
            }
        }
    }
    while (!oq.empty() && (int)owned.size() < kMax) {
        IVec3 p = oq.front();
        oq.pop();
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    seedOwned(p.x + dx, p.y + dy, p.z + dz);
                }
            }
        }
    }
    if (bindId != 0 && !owned.empty()) {
        int minx = x, maxx = x, miny = y, maxy = y, minz = z, maxz = z;
        for (const IVec3& c : owned) {
            minx = std::min(minx, c.x); maxx = std::max(maxx, c.x);
            miny = std::min(miny, c.y); maxy = std::max(maxy, c.y);
            minz = std::min(minz, c.z); maxz = std::max(maxz, c.z);
        }
        minx--; maxx++; miny = std::max(0, miny - 1); maxy = std::min(cfg::WORLD_H - 1, maxy + 1); minz--; maxz++;
        for (int py = miny; py <= maxy; py++) {
            for (int pz = minz; pz <= maxz; pz++) {
                for (int px = minx; px <= maxx; px++) {
                    if (ownedPart(px, py, pz)) owned.insert({ px, py, pz });
                }
            }
        }
    }

    std::unordered_set<IVec3, IVec3Hash> standing;
    std::queue<IVec3> sq;
    auto seedStanding = [&](int px, int py, int pz) {
        if (!ownedWood(px, py, pz)) return;
        IVec3 n{ px, py, pz };
        if (standing.insert(n).second) sq.push(n);
    };
    for (int i = 0; i < 6; i++) {
        int px = x + k6[i][0], py = y + k6[i][1], pz = z + k6[i][2];
        if (py < y) seedStanding(px, py, pz);
    }
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dz == 0) continue;
            int px = x + dx, pz = z + dz;
            if (!inCols(px, pz) || !grounded(px, y, pz)) continue;
            seedStanding(px, y, pz);
        }
    }
    while (!sq.empty() && (int)standing.size() < kMax) {
        IVec3 p = sq.front();
        sq.pop();
        for (int i = 0; i < 6; i++) {
            IVec3 n{ p.x + k6[i][0], p.y + k6[i][1], p.z + k6[i][2] };
            if (!ownedWood(n.x, n.y, n.z)) continue;
            if (standing.insert(n).second) sq.push(n);
        }
    }

    std::unordered_set<IVec3, IVec3Hash> standingLeaf;
    std::queue<IVec3> lq;
    for (const IVec3& c : standing) {
        lq.push(c);
        standingLeaf.insert(c);
    }
    while (!lq.empty() && (int)standingLeaf.size() < kMax) {
        IVec3 p = lq.front();
        lq.pop();
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    IVec3 n{ p.x + dx, p.y + dy, p.z + dz };
                    if (!ownedPart(n.x, n.y, n.z)) continue;
                    if (isTreeWood(getBlock(n.x, n.y, n.z))) continue;
                    if (standingLeaf.insert(n).second) lq.push(n);
                }
            }
        }
    }

    std::unordered_set<IVec3, IVec3Hash> fallingWood;
    std::queue<IVec3> fq;
    auto seedFalling = [&](int px, int py, int pz) {
        if (standing.count({ px, py, pz })) return;
        if (!ownedWood(px, py, pz)) return;
        IVec3 n{ px, py, pz };
        if (fallingWood.insert(n).second) fq.push(n);
    };
    for (int dy = -1; dy <= 1; dy++) {
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                int px = x + dx, py = y + dy, pz = z + dz;
                if (py > y) seedFalling(px, py, pz);
                else if (py == y && inCols(px, pz) && !grounded(px, py, pz))
                    seedFalling(px, py, pz);
            }
        }
    }
    while (!fq.empty() && (int)fallingWood.size() < kMax) {
        IVec3 p = fq.front();
        fq.pop();
        for (int i = 0; i < 6; i++) {
            IVec3 n{ p.x + k6[i][0], p.y + k6[i][1], p.z + k6[i][2] };
            if (standing.count(n)) continue;
            if (!ownedWood(n.x, n.y, n.z)) continue;
            if (fallingWood.insert(n).second) fq.push(n);
        }
    }
    if (bindId != 0) {
        for (const IVec3& c : owned) {
            if (standing.count(c)) continue;
            if (isTreeWood(getBlock(c.x, c.y, c.z))) fallingWood.insert(c);
        }
    }

    std::unordered_set<IVec3, IVec3Hash> above = fallingWood;
    if (bindId != 0) {
        for (const IVec3& c : owned) {
            if (standing.count(c) || standingLeaf.count(c)) continue;
            if (!isTreeLeaf(getBlock(c.x, c.y, c.z))) continue;
            above.insert(c);
        }
    } else {
        std::queue<IVec3> q;
        for (const IVec3& c : fallingWood) q.push(c);
        while (!q.empty() && (int)above.size() < kMax) {
            IVec3 p = q.front();
            q.pop();
            for (int dy = -1; dy <= 1; dy++) {
                for (int dz = -1; dz <= 1; dz++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0 && dz == 0) continue;
                        IVec3 n{ p.x + dx, p.y + dy, p.z + dz };
                        if (standing.count(n) || standingLeaf.count(n)) continue;
                        if (!ownedPart(n.x, n.y, n.z)) continue;
                        if (isTreeWood(getBlock(n.x, n.y, n.z)) && !fallingWood.count(n))
                            continue;
                        if (above.insert(n).second) q.push(n);
                    }
                }
            }
        }
    }

    if (above.empty()) return;
    std::vector<IVec3> region(above.begin(), above.end());
    std::vector<uint8_t> kinds;
    kinds.reserve(region.size());
    for (const IVec3& c : region) kinds.push_back(getBlock(c.x, c.y, c.z));
    auto comps = tree_fall::partitionByWoodCore(region, kinds);
    for (const std::vector<IVec3>& comp : comps) {
        if (!comp.empty()) spawnFallingTree(comp, x, y, z);
    }
}

bool World::trySettleFalling(PhysicsIsland& t) {
    if (t.cells.empty() || t.mass < 1e-4f) return true;
    if (t.com.y < -4.0f) return true;
    if (t.stillTime < 0.40f) return false;
    bool flat = std::fabs(t.ay.y) <= 0.55f;
    if (!flat && t.stillTime < 2.2f) return false;

    const float S = cfg::BLOCK_SCALE;
    auto freeAt = [&](int bx, int by, int bz) {
        if (by < 0 || by >= cfg::WORLD_H) return false;
        uint8_t dest = getBlock(bx, by, bz);
        return dest == AIR || isLiquid(dest) || isPassableCutout(dest);
    };

    struct Put { int x, y, z; uint8_t b, fl; };
    std::vector<Put> plan;
    std::unordered_set<IVec3, IVec3Hash> claimed;
    int woodNeed = 0, woodGot = 0;

    auto claim = [&](int bx, int by, int bz) {
        IVec3 p{ bx, by, bz };
        if (claimed.count(p) || !freeAt(bx, by, bz)) return false;
        claimed.insert(p);
        return true;
    };
    auto findSpot = [&](int bx, int by, int bz, int& ox, int& oy, int& oz) {
        const int dy[3] = { 0, 1, 2 };
        for (int i = 0; i < 3; i++) {
            int y2 = by + dy[i];
            if (claim(bx, y2, bz)) { ox = bx; oy = y2; oz = bz; return true; }
        }
        const int d[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1} };
        for (int i = 0; i < 8; i++) {
            for (int k = 0; k < 2; k++) {
                int x2 = bx + d[i][0], z2 = bz + d[i][1], y2 = by + k;
                if (claim(x2, y2, z2)) { ox = x2; oy = y2; oz = z2; return true; }
            }
        }
        return false;
    };

    for (const IVec3& c : t.cells) {
        uint8_t b = t.get(c.x, c.y, c.z);
        if (b == AIR || t.hiddenAt(c.x, c.y, c.z)) continue;
        bool wood = isTreeWood(b);
        if (wood) woodNeed++;
        Vec3 wp = tree_fall::worldOf(t, c.x, c.y, c.z);
        int bx = (int)std::floor(wp.x / S);
        int by = (int)std::floor(wp.y / S);
        int bz = (int)std::floor(wp.z / S);
        int ox = 0, oy = 0, oz = 0;
        if (!findSpot(bx, by, bz, ox, oy, oz)) continue;
        uint8_t fl = 0;
        if (wood) {
            // Keep remapped cut faces so bark wrap / rings / stripped grain
            // still resolve; leave FLAG_ALIVE off so this log cannot fall again.
            fl = (uint8_t)(remapRingCutFlags(t.flagAt(c.x, c.y, c.z), t.ax, t.ay, t.az)
                           | FLAG_SETTLED);
            woodGot++;
        }
        plan.push_back({ ox, oy, oz, b, fl });
    }

    if (woodNeed > 0 && woodGot * 3 < woodNeed * 2 && t.stillTime < 4.0f)
        return false;

    std::vector<IVec3> placed;
    placed.reserve(plan.size());
    for (const Put& p : plan) {
        writeCell(p.x, p.y, p.z, p.b, p.fl, true);
        placed.push_back({ p.x, p.y, p.z });
    }
    if (!placed.empty()) rebuildTouched(placed);
    return !plan.empty() || t.stillTime > 5.0f;
}

void World::splitIsland(size_t index) {
    if (index >= m_phys.size()) return;
    PhysicsIsland& t = m_phys[index];
    if (t.cells.size() < 2) return;

    std::vector<IVec3> cells;
    std::vector<uint8_t> kinds;
    cells.reserve(t.cells.size());
    kinds.reserve(t.cells.size());
    for (const IVec3& c : t.cells) {
        uint8_t b = t.get(c.x, c.y, c.z);
        if (b == AIR) continue;
        cells.push_back(c);
        kinds.push_back(b);
    }
    auto comps = tree_fall::partitionByWoodCore(cells, kinds);
    if (comps.size() <= 1) return;
    std::sort(comps.begin(), comps.end(),
              [](const std::vector<IVec3>& a, const std::vector<IVec3>& b) { return a.size() > b.size(); });

    std::vector<PhysicsIsland> extra;
    extra.reserve(comps.size() - 1);
    for (size_t i = 1; i < comps.size(); i++) {
        extra.push_back(tree_fall::extract(t, comps[i]));
        for (const IVec3& c : comps[i]) tree_fall::setCell(t, c.x, c.y, c.z, AIR, 0);
    }
    tree_fall::recomputeMass(t);
    t.meshDirty = true;
    t.contentRev++;
    tree_fall::buildMesh(t);
    for (PhysicsIsland& e : extra) {
        if (e.cells.empty()) continue;
        tree_fall::buildMesh(e);
        m_phys.push_back(std::move(e));
        tagIsland(m_phys.back());
    }
}

void World::treeFallPhysics(float dt) {
    for (size_t i = 0; i < m_phys.size();) {
        tree_fall::step(m_phys[i], *this, dt);
        if (m_phys[i].meshDirty) tree_fall::buildMesh(m_phys[i]);
        if (trySettleFalling(m_phys[i])) {
            forgetIslandMines(i);
            m_phys[i] = std::move(m_phys.back());
            m_phys.pop_back();
        } else {
            i++;
        }
    }
}

void World::treeFallGameTick() {
    for (size_t i = 0; i < m_phys.size();) {
        bool crushed = tree_fall::gameTick(m_phys[i]);
        if (crushed) m_phys[i].contentRev++;
        if (m_phys[i].cells.empty()) {
            forgetIslandMines(i);
            m_phys[i] = std::move(m_phys.back());
            m_phys.pop_back();
            continue;
        }
        splitIsland(i);
        i++;
    }
}

bool World::saveChunkFile(int cx, int cy, int cz, const Chunk& ch) const {
    if (!m_saveEnabled) return true;
    std::error_code ec;
    auto dir = saves::utf8Path(m_saveDir);
    std::filesystem::create_directories(dir, ec);
    auto path = dir / (std::to_string(cx) + "." + std::to_string(cy) + "." + std::to_string(cz) + ".bin");
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    char magic[4] = { 'V', 'L', 'V', '2' };
    uint32_t seed = m_seed, len = cfg::CHUNK_VOLUME;
    int32_t scx = cx, scy = cy, scz = cz;
    uint32_t chk = fnv1a(ch.blocks.data(), ch.blocks.size());
    f.write(magic, 4);
    f.write((const char*)&seed, 4);
    f.write((const char*)&scx, 4);
    f.write((const char*)&scy, 4);
    f.write((const char*)&scz, 4);
    f.write((const char*)&len, 4);
    f.write((const char*)ch.blocks.data(), (std::streamsize)ch.blocks.size());
    f.write((const char*)&chk, 4);

    // Sod section (v2): persist the exact sod faces so dug-out pits stay bare
    // across reloads. Each face is 5 bytes (x, z, y, face, stage) + a checksum.
    std::vector<uint8_t> sod;
    sod.reserve(ch.sodFaces.size() * 5);
    for (const auto& sf : ch.sodFaces) {
        sod.push_back(sf.x);
        sod.push_back(sf.z);
        sod.push_back(sf.y);
        sod.push_back(sf.face);
        sod.push_back(sf.stage);
    }
    uint32_t sodCount = (uint32_t)ch.sodFaces.size();
    uint32_t sodChk = fnv1a(sod.data(), sod.size());
    f.write((const char*)&sodCount, 4);
    f.write((const char*)sod.data(), (std::streamsize)sod.size());
    f.write((const char*)&sodChk, 4);

    // Water-level section (v3): dynamic water levels (0 for non-dynamic cells).
    uint32_t wchk = fnv1a(ch.waterLevel.data(), ch.waterLevel.size());
    f.write((const char*)ch.waterLevel.data(), (std::streamsize)ch.waterLevel.size());
    f.write((const char*)&wchk, 4);

    uint32_t fchk = fnv1a(ch.flags.data(), ch.flags.size());
    f.write((const char*)ch.flags.data(), (std::streamsize)ch.flags.size());
    f.write((const char*)&fchk, 4);

    std::vector<uint8_t> bark;
    bark.reserve(ch.barkFaces.size() * 4);
    for (const auto& bf : ch.barkFaces) {
        bark.push_back(bf.x);
        bark.push_back(bf.z);
        bark.push_back(bf.y);
        bark.push_back(bf.face);
    }
    uint32_t barkCount = (uint32_t)ch.barkFaces.size();
    uint32_t barkChk = fnv1a(bark.data(), bark.size());
    f.write((const char*)&barkCount, 4);
    f.write((const char*)bark.data(), (std::streamsize)bark.size());
    f.write((const char*)&barkChk, 4);

    std::vector<uint8_t> binds;
    if (ch.treeId.size() == (size_t)cfg::CHUNK_VOLUME) {
        binds.reserve(256 * 7);
        for (int y = 0; y < cfg::CHUNK_Y; y++) {
            for (int lz = 0; lz < cfg::CHUNK_Z; lz++) {
                for (int lx = 0; lx < cfg::CHUNK_X; lx++) {
                    uint32_t id = ch.treeIdAt(lx, y, lz);
                    if (id == 0) continue;
                    uint8_t b = ch.get(lx, y, lz);
                    if (!isTreePart(b)) continue;
                    binds.push_back((uint8_t)lx);
                    binds.push_back((uint8_t)lz);
                    binds.push_back((uint8_t)y);
                    binds.push_back((uint8_t)(id & 0xFFu));
                    binds.push_back((uint8_t)((id >> 8) & 0xFFu));
                    binds.push_back((uint8_t)((id >> 16) & 0xFFu));
                    binds.push_back((uint8_t)((id >> 24) & 0xFFu));
                }
            }
        }
    }
    uint32_t bindCount = (uint32_t)(binds.size() / 7);
    uint32_t bindChk = fnv1a(binds.data(), binds.size());
    f.write((const char*)&bindCount, 4);
    f.write((const char*)binds.data(), (std::streamsize)binds.size());
    f.write((const char*)&bindChk, 4);
    f.flush();
    return (bool)f;
}

bool World::loadChunkFile(int cx, int cy, int cz, Chunk& ch) const {
    if (!m_saveEnabled) return false;
    auto path = saves::utf8Path(m_saveDir) / (std::to_string(cx) + "." + std::to_string(cy) + "." + std::to_string(cz) + ".bin");
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4] = { 0, 0, 0, 0 };
    uint32_t seed = 0, len = 0, chk = 0;
    int32_t scx = 0, scy = 0, scz = 0;
    f.read(magic, 4);
    f.read((char*)&seed, 4);
    f.read((char*)&scx, 4);
    f.read((char*)&scy, 4);
    f.read((char*)&scz, 4);
    f.read((char*)&len, 4);
    if (!f) return false;
    if (std::memcmp(magic, "VLV2", 4) != 0 || seed != m_seed || scx != cx || scy != cy || scz != cz || len != cfg::CHUNK_VOLUME) {
        return false;
    }
    f.read((char*)ch.blocks.data(), (std::streamsize)ch.blocks.size());
    f.read((char*)&chk, 4);
    if (!f) return false;
    if (fnv1a(ch.blocks.data(), ch.blocks.size()) != chk) return false;
    ch.generated = true;

    uint32_t sodCount = 0, sodChk = 0;
    f.read((char*)&sodCount, 4);
    if (!f || sodCount > 200000) return false; // sanity cap
    ch.sodFaces.clear();
    ch.sodFaces.reserve(sodCount);
    std::vector<uint8_t> sodBytes;
    sodBytes.reserve((size_t)sodCount * 5);
    for (uint32_t i = 0; i < sodCount; i++) {
        uint8_t b[5];
        f.read((char*)b, 5);
        if (!f) return false;
        sodBytes.insert(sodBytes.end(), b, b + 5);
        Chunk::SodFace sf;
        sf.x = b[0]; sf.z = b[1]; sf.y = b[2]; sf.face = b[3]; sf.stage = b[4];
        ch.sodFaces.push_back(sf);
    }
    f.read((char*)&sodChk, 4);
    if (!f) return false;
    if (fnv1a(sodBytes.data(), sodBytes.size()) != sodChk) return false;
    size_t w = 0;
    for (size_t r = 0; r < ch.sodFaces.size(); r++) {
        const Chunk::SodFace& sf = ch.sodFaces[r];
        int above = AIR;
        if ((int)sf.y + 1 < cfg::CHUNK_Y) above = ch.get(sf.x, sf.y + 1, sf.z);
        else {
            auto up = m_chunks.find(chunkKey(cx, cy + 1, cz));
            if (up != m_chunks.end()) above = up->second.get(sf.x, 0, sf.z);
        }
        if (isSurfaceDirt(ch, sf.x, sf.y, sf.z, above)) ch.sodFaces[w++] = sf;
    }
    ch.sodFaces.resize(w);
    ch.sodValid = true;

    uint32_t wchk = 0;
    f.read((char*)ch.waterLevel.data(), (std::streamsize)ch.waterLevel.size());
    f.read((char*)&wchk, 4);
    if (!f) return false;
    if (fnv1a(ch.waterLevel.data(), ch.waterLevel.size()) != wchk) return false;
    ch.hasWater = false;
    for (size_t i = 0; i < ch.waterLevel.size(); i++) {
        if (ch.blocks[i] == WATER && ch.waterLevel[i] > 0) { ch.hasWater = true; break; }
    }

    uint32_t fchk = 0;
    f.read((char*)ch.flags.data(), (std::streamsize)ch.flags.size());
    f.read((char*)&fchk, 4);
    if (!f) return false;
    if (fnv1a(ch.flags.data(), ch.flags.size()) != fchk) return false;

    ch.barkFaces.clear();
    uint32_t barkCount = 0, barkChk = 0;
    f.read((char*)&barkCount, 4);
    if (!f || barkCount > 200000) return false;
    std::vector<uint8_t> barkBytes;
    barkBytes.reserve((size_t)barkCount * 4);
    ch.barkFaces.reserve(barkCount);
    for (uint32_t i = 0; i < barkCount; i++) {
        uint8_t b[4];
        f.read((char*)b, 4);
        if (!f) return false;
        barkBytes.insert(barkBytes.end(), b, b + 4);
        Chunk::BarkFace bf;
        bf.x = b[0]; bf.z = b[1]; bf.y = b[2]; bf.face = b[3];
        if (bf.face < 6) ch.barkFaces.push_back(bf);
    }
    f.read((char*)&barkChk, 4);
    if (!f) return false;
    if (fnv1a(barkBytes.data(), barkBytes.size()) != barkChk) return false;

    uint32_t bindCount = 0, bindChk = 0;
    f.read((char*)&bindCount, 4);
    if (!f) return true; // older VLV1 chunks have no bind table
    if (bindCount > (uint32_t)cfg::CHUNK_VOLUME) return false;
    std::vector<uint8_t> bindBytes((size_t)bindCount * 7);
    if (bindCount > 0) {
        f.read((char*)bindBytes.data(), (std::streamsize)bindBytes.size());
        if (!f) return false;
    }
    f.read((char*)&bindChk, 4);
    if (!f) return false;
    if (fnv1a(bindBytes.data(), bindBytes.size()) != bindChk) return false;
    for (uint32_t i = 0; i < bindCount; i++) {
        const uint8_t* b = bindBytes.data() + (size_t)i * 7;
        int lx = b[0], lz = b[1], y = b[2];
        if (lx >= cfg::CHUNK_X || lz >= cfg::CHUNK_Z || y >= cfg::CHUNK_Y) continue;
        uint32_t id = (uint32_t)b[3] | ((uint32_t)b[4] << 8) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 24);
        if (id == 0 || !isTreePart(ch.get(lx, y, lz))) continue;
        ch.setTreeId(lx, y, lz, id);
        ch.markAlive(lx, y, lz);
    }
    return true;
}

void World::saveAll() {
    if (!m_saveEnabled) return;
    for (const auto& [key, ch] : m_chunks) {
        if (!ch.modified) continue;
        int cx = chunkCX(key), cz = chunkCZ(key);
        if (m_arena && trialColumnOverlaps(cx, cz)) continue;
        saveChunkFile(cx, chunkCY(key), cz, ch);
    }
}

void World::setGuardianArena(bool on) {
    if (m_arena && !on) discardGuardianArenaChunks();
    m_arena = on;
}

void World::discardGuardianArenaChunks() {
    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        int cx = chunkCX(it->first), cz = chunkCZ(it->first);
        if (!trialColumnOverlaps(cx, cz)) { ++it; continue; }
        m_editedCols.erase(columnKey(cx, cz));
        it = m_chunks.erase(it);
    }
    std::deque<int64_t> kept;
    while (!m_meshQueue.empty()) {
        int64_t key = m_meshQueue.front();
        m_meshQueue.pop_front();
        if (!trialColumnOverlaps(chunkCX(key), chunkCZ(key))) kept.push_back(key);
    }
    m_meshQueue.swap(kept);
    for (auto it = m_originTrees.begin(); it != m_originTrees.end();) {
        if (trialColumnOverlaps(columnCX(it->first), columnCZ(it->first)))
            it = m_originTrees.erase(it);
        else ++it;
    }
}

void World::loadGuardianArena() {
    if (!m_arena) return;
    int cx0 = floorDiv(structure::kTrialX0, cfg::CHUNK_X);
    int cz0 = floorDiv(structure::kTrialZ0, cfg::CHUNK_Z);
    int cx1 = floorDiv(structure::kTrialX0 + structure::kTrialSpan - 1, cfg::CHUNK_X);
    int cz1 = floorDiv(structure::kTrialZ0 + structure::kTrialSpan - 1, cfg::CHUNK_Z);
    for (int cz = cz0; cz <= cz1; cz++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            if (!trialColumnOverlaps(cx, cz)) continue;
            ensureColumn(cx, cz);
        }
    }
    while (!m_meshQueue.empty()) {
        int64_t key = m_meshQueue.front();
        m_meshQueue.pop_front();
        auto it = m_chunks.find(key);
        if (it == m_chunks.end() || !it->second.dirty) continue;
        buildMeshFor(it->second, chunkCX(key), chunkCY(key), chunkCZ(key));
        it->second.dirty = false;
    }
}

void World::clearTrialDrops() {
    const float S = cfg::BLOCK_SCALE;
    float x0 = structure::kTrialX0 * S - 2.0f;
    float z0 = structure::kTrialZ0 * S - 2.0f;
    float x1 = (structure::kTrialX0 + structure::kTrialSpan) * S + 2.0f;
    float z1 = (structure::kTrialZ0 + structure::kTrialSpan) * S + 2.0f;
    m_drops.erase(std::remove_if(m_drops.begin(), m_drops.end(), [&](const loot::Drop& d) {
        return d.pos.x >= x0 && d.pos.x <= x1 && d.pos.z >= z0 && d.pos.z <= z1;
    }), m_drops.end());
}

uint32_t World::spawnDrop(const Vec3& pos, uint8_t item, int count, bool inPlace, Vec3 vel) {
    if (item == AIR || count <= 0 || !validBlock(item)) return 0;
    uint32_t firstId = 0;
    int left = count;
    const int stack = (int)loot::maxStack(item);
    while (left > 0) {
        loot::Drop d;
        d.netId = m_nextDropId++;
        if (!m_nextDropId) m_nextDropId = 1;
        if (!firstId) firstId = d.netId;
        d.pos = pos;
        d.item = item;
        int n = std::min(left, stack);
        d.count = (uint8_t)n;
        left -= n;
        if (inPlace) {
            d.vel = { 0, 0, 0 };
            d.grounded = true;
        } else {
            d.vel = vel;
            d.grounded = false;
        }
        d.ax = { 1, 0, 0 };
        d.ay = { 0, 1, 0 };
        d.az = { 0, 0, 1 };
        d.angVel = { 0, 0, 0 };
        d.age = 0.0f;
        m_drops.push_back(d);
    }
    return firstId;
}

int World::raycastDrop(const Vec3& origin, const Vec3& dir, float maxDist, float& tHit) const {
    tHit = maxDist + 1.0f;
    int best = -1;
    Vec3 nd = dir;
    float len = nd.length();
    if (len < 1e-8f) return -1;
    nd = nd / len;
    for (int i = 0; i < (int)m_drops.size(); i++) {
        const loot::Drop& d = m_drops[(size_t)i];
        if (d.item == AIR || d.count == 0) continue;
        const dropgeom::Shape& sh = dropgeom::cached(d.item);
        Vec3 rel = origin - d.pos;
        Vec3 localO{ rel.dot(d.ax), rel.dot(d.ay), rel.dot(d.az) };
        Vec3 localD{ nd.dot(d.ax), nd.dot(d.ay), nd.dot(d.az) };
        Vec3 mn{ -sh.half.x, -sh.half.y, -sh.half.z };
        Vec3 mx{ sh.half.x, sh.half.y, sh.half.z };
        float t = 0.0f;
        if (!loot::rayAabb(localO, localD, mn, mx, t)) continue;
        if (t < 0.0f || t > maxDist || t >= tHit) continue;
        tHit = t;
        best = i;
    }
    return best;
}

bool World::takeDrop(int index, uint8_t& item, uint8_t& count) {
    item = AIR;
    count = 0;
    if (index < 0 || index >= (int)m_drops.size()) return false;
    loot::Drop& d = m_drops[(size_t)index];
    if (d.item == AIR || d.count == 0) return false;
    item = d.item;
    count = d.count;
    m_drops[(size_t)index] = std::move(m_drops.back());
    m_drops.pop_back();
    return true;
}

void World::setDropCount(int index, uint8_t count) {
    if (index < 0 || index >= (int)m_drops.size()) return;
    if (count == 0) {
        uint8_t item = AIR, n = 0;
        takeDrop(index, item, n);
        return;
    }
    m_drops[(size_t)index].count = count;
}

const loot::Drop* World::dropById(uint32_t id) const {
    if (!id) return nullptr;
    for (const auto& drop : m_drops) if (drop.netId == id) return &drop;
    return nullptr;
}

bool World::takeDropCountById(uint32_t id, uint8_t count) {
    if (!id || !count) return false;
    for (size_t i = 0; i < m_drops.size(); ++i) {
        loot::Drop& drop = m_drops[i];
        if (drop.netId != id || drop.count < count) continue;
        drop.count = (uint8_t)(drop.count - count);
        if (!drop.count) {
            m_drops[i] = std::move(m_drops.back());
            m_drops.pop_back();
        }
        return true;
    }
    return false;
}

void World::replaceNetworkDrops(const std::vector<loot::Drop>& drops) {
    m_drops = drops;
    uint32_t largest = 0;
    for (const auto& drop : m_drops) largest = std::max(largest, drop.netId);
    m_nextDropId = largest + 1;
    if (!m_nextDropId) m_nextDropId = 1;
}

static uint64_t makeDurKey(int phys, int x, int y, int z) {
    uint8_t p = (phys < 0) ? 0xFFu : (uint8_t)std::min(phys, 254);
    uint64_t y8 = (uint64_t)(uint8_t)clampi(y, 0, 255);
    uint64_t x24 = (uint64_t)((uint32_t)(x + 0x800000) & 0xFFFFFFu);
    uint64_t z24 = (uint64_t)((uint32_t)(z + 0x800000) & 0xFFFFFFu);
    return ((uint64_t)p << 56) | (y8 << 48) | (x24 << 24) | z24;
}

void World::decodeMineKey(uint64_t key, int& phys, int& x, int& y, int& z) {
    int p = (int)(key >> 56);
    phys = (p == 255) ? -1 : p;
    y = (int)((key >> 48) & 0xFFu);
    x = (int)((key >> 24) & 0xFFFFFFu) - 0x800000;
    z = (int)(key & 0xFFFFFFu) - 0x800000;
}

void World::clearBlockDur(int phys, int x, int y, int z) {
    m_blockDur.erase(makeDurKey(phys, x, y, z));
}

float World::blockDurRemaining(int phys, int x, int y, int z) const {
    uint8_t b = (phys >= 0) ? getPhysBlock(phys, x, y, z) : getBlock(x, y, z);
    if (b == AIR) return 0.0f;
    float maxD = loot::blockBreak(b).durability;
    auto it = m_blockDur.find(makeDurKey(phys, x, y, z));
    if (it == m_blockDur.end()) return maxD;
    return it->second.rem;
}

float World::blockDurProgress(int phys, int x, int y, int z) const {
    uint8_t b = (phys >= 0) ? getPhysBlock(phys, x, y, z) : getBlock(x, y, z);
    if (b == AIR) return 0.0f;
    float maxD = loot::blockBreak(b).durability;
    if (maxD <= 0.001f) return 0.0f;
    auto it = m_blockDur.find(makeDurKey(phys, x, y, z));
    if (it == m_blockDur.end()) return 0.0f;
    return clampf(1.0f - it->second.rem / maxD, 0.0f, 1.0f);
}

bool World::hasSodFace(int x, int y, int z, int face) const {
    CellLoc c;
    if (!cellLoc(x, y, z, c) || face < 0 || face > 5) return false;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return false;
    for (const Chunk::SodFace& sf : it->second.sodFaces) {
        if ((int)sf.x == c.lx && (int)sf.y == c.ly && (int)sf.z == c.lz && (int)sf.face == face)
            return true;
    }
    return false;
}

float World::sodDurProgress(int x, int y, int z, int face) const {
    CellLoc c;
    if (!cellLoc(x, y, z, c) || face < 0 || face > 5) return 0.0f;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return 0.0f;
    float maxD = loot::sodBreak().durability;
    if (maxD <= 0.001f) return 0.0f;
    for (const Chunk::SodFace& sf : it->second.sodFaces) {
        if ((int)sf.x != c.lx || (int)sf.y != c.ly || (int)sf.z != c.lz || (int)sf.face != face)
            continue;
        float rem = (sf.rem < 0.0f) ? maxD : sf.rem;
        return clampf(1.0f - rem / maxD, 0.0f, 1.0f);
    }
    return 0.0f;
}

static bool applySodHit(World::Chunk& ch, int lx, int y, int lz, int face, uint8_t heldTool) {
    float maxD = loot::sodBreak().durability;
    float dmg = loot::sodMineDamage(heldTool);
    if (dmg <= 0.0f) return false;
    for (size_t i = 0; i < ch.sodFaces.size(); i++) {
        World::Chunk::SodFace& sf = ch.sodFaces[i];
        if ((int)sf.x != lx || (int)sf.y != y || (int)sf.z != lz || (int)sf.face != face)
            continue;
        float rem = (sf.rem < 0.0f) ? maxD : sf.rem;
        rem -= dmg;
        if (rem > 0.0f) {
            sf.rem = rem;
            ch.dirty = true;
            ch.modified = true;
            return false;
        }
        ch.sodFaces[i] = ch.sodFaces.back();
        ch.sodFaces.pop_back();
        ch.dirty = true;
        ch.modified = true;
        return true;
    }
    return false;
}

bool World::applyMineHit(int phys, int x, int y, int z, uint8_t heldTool, int face) {
    if (phys < 0 && face >= 0 && hasSodFace(x, y, z, face)) {
        CellLoc c;
        if (cellLoc(x, y, z, c)) {
            auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
            if (it != m_chunks.end()) {
                applySodHit(it->second, c.lx, c.ly, c.lz, face, heldTool);
                touchAuthSod(chunkKey(c.cx, c.cy, c.cz));
                if (it->second.dirty) {
                    buildMeshFor(it->second, c.cx, c.cy, c.cz);
                    it->second.dirty = false;
                }
            }
        }
        return false; // sod is not the dirt block
    }
    uint8_t b = (phys >= 0) ? getPhysBlock(phys, x, y, z) : getBlock(x, y, z);
    if (b == AIR) return false;
    float maxD = loot::blockBreak(b).durability;
    if (maxD <= 0.001f) return true;
    float dmg = loot::mineDamage(b, heldTool);
    if (dmg <= 0.0f) return false;
    uint64_t key = makeDurKey(phys, x, y, z);
    auto it = m_blockDur.find(key);
    MineState st = (it == m_blockDur.end()) ? MineState{} : it->second;
    if (it == m_blockDur.end()) st.rem = maxD;
    st.rem -= dmg;
    if (st.rem <= 0.0f) {
        m_blockDur.erase(key);
        return true;
    }
    if (st.nHits < kMaxMineHits) {
        uint8_t f = (face >= 0 && face <= 5) ? (uint8_t)face : 0;
        st.hits[st.nHits] = { dmg, f };
        st.nHits++;
    }
    st.serial = m_mineEpoch++;
    if (m_mineEpoch == 0) m_mineEpoch = 1;
    m_blockDur[key] = st;
    return false;
}

void World::updateDrops(float dt) {
    const float S = cfg::BLOCK_SCALE;
    for (size_t i = 0; i < m_drops.size();) {
        loot::Drop& d = m_drops[i];
        d.age += dt;
        // Clues are progression-critical and may be placed long before a
        // player reaches their building. Ordinary dropped items still expire.
        if ((d.item != ITEM_CLUE && d.age > cfg::DROP_LIFETIME) ||
            d.pos.y < -12.0f || d.item == AIR || d.count == 0) {
            m_drops[i] = std::move(m_drops.back());
            m_drops.pop_back();
            continue;
        }
        if (!d.grounded) d.vel.y -= cfg::GRAVITY * dt;
        d.vel.y = clampf(d.vel.y, -28.0f, 28.0f);
        const dropgeom::Shape& sh = dropgeom::cached(d.item);
        float ex = 0.0f, ey = 0.0f, ez = 0.0f;
        dropgeom::worldHalf(sh, d.ax, d.ay, d.az, ex, ey, ez);
        {
            float dragH = 0.0f, dragV = 0.0f;
            const float pad = 1e-3f;
            Vec3 mn{ d.pos.x - ex + pad, d.pos.y - ey + pad, d.pos.z - ez + pad };
            Vec3 mx{ d.pos.x + ex - pad, d.pos.y + ey - pad, d.pos.z + ez - pad };
            int x0 = (int)std::floor(mn.x / S), x1 = (int)std::floor(mx.x / S);
            int y0 = (int)std::floor(mn.y / S), y1 = (int)std::floor(mx.y / S);
            int z0 = (int)std::floor(mn.z / S), z1 = (int)std::floor(mx.z / S);
            if (x1 < x0) x1 = x0;
            if (y1 < y0) y1 = y0;
            if (z1 < z0) z1 = z0;
            for (int bx = x0; bx <= x1; bx++)
                for (int by = y0; by <= y1; by++)
                    for (int bz = z0; bz <= z1; bz++)
                        accumPassableDrag(getBlock(bx, by, bz), dragH, dragV);
            if (dragH > 0.0f) {
                float k = std::exp(-dragH * dt);
                d.vel.x *= k;
                d.vel.z *= k;
            }
            if (dragV > 0.0f)
                d.vel.y *= std::exp(-dragV * dt);
        }
        if (d.grounded) {
            int gy = (int)std::floor((d.pos.y - ey - 1e-3f) / S);
            auto solidAt = [&](int x, int y, int z) -> uint8_t {
                uint8_t b = getBlock(x, y, z);
                if (structure::isGuardianToken(x, y, z, b)) return (uint8_t)AIR;
                return blocksMotion(b) ? b : (uint8_t)AIR;
            };
            int gx = (int)std::floor(d.pos.x / S);
            int gz = (int)std::floor(d.pos.z / S);
            uint8_t support = solidAt(gx, gy, gz);
            if (support == AIR) {
                int x0 = (int)std::floor((d.pos.x - ex) / S);
                int x1 = (int)std::floor((d.pos.x + ex) / S);
                int z0 = (int)std::floor((d.pos.z - ez) / S);
                int z1 = (int)std::floor((d.pos.z + ez) / S);
                support = solidAt(x0, gy, z0);
                if (support == AIR) support = solidAt(x1, gy, z0);
                if (support == AIR) support = solidAt(x0, gy, z1);
                if (support == AIR) support = solidAt(x1, gy, z1);
            }
            dropgeom::applyGroundContact(d.vel, d.angVel, sh, d.ax, d.ay, d.az,
                                         blockFriction(support), dt);
        }
        float bottom = d.pos.y - ey;
        dropgeom::spinBasis(d.ax, d.ay, d.az, d.angVel, dt);
        dropgeom::worldHalf(sh, d.ax, d.ay, d.az, ex, ey, ez);
        if (d.grounded) d.pos.y = bottom + ey;

        auto moveAxis = [&](int axis, float delta) {
            if (std::fabs(delta) < 1e-8f) return;
            float prev = (axis == 0) ? d.pos.x : (axis == 1) ? d.pos.y : d.pos.z;
            float he = (axis == 0) ? ex : (axis == 1) ? ey : ez;
            if (axis == 0) d.pos.x += delta;
            else if (axis == 1) d.pos.y += delta;
            else d.pos.z += delta;
            // Inset so a drop that exactly fills its cell does not count as
            // already inside the block it is resting on.
            const float pad = 1e-3f;
            Vec3 mn{ d.pos.x - ex + pad, d.pos.y - ey + pad, d.pos.z - ez + pad };
            Vec3 mx{ d.pos.x + ex - pad, d.pos.y + ey - pad, d.pos.z + ez - pad };
            int x0 = (int)std::floor(mn.x / S), x1 = (int)std::floor(mx.x / S);
            int y0 = (int)std::floor(mn.y / S), y1 = (int)std::floor(mx.y / S);
            int z0 = (int)std::floor(mn.z / S), z1 = (int)std::floor(mx.z / S);
            int hx = 0, hy = 0, hz = 0;
            bool hit = false;
            for (int bx = x0; bx <= x1 && !hit; bx++)
                for (int by = y0; by <= y1 && !hit; by++)
                    for (int bz = z0; bz <= z1 && !hit; bz++)
                        if (blocksMotion(getBlock(bx, by, bz)) &&
                            !structure::isGuardianToken(bx, by, bz, getBlock(bx, by, bz))) {
                            hx = bx; hy = by; hz = bz; hit = true;
                        }
            if (!hit) {
                if (axis == 1 && delta < 0.0f) d.grounded = false;
                return;
            }
            if (axis == 0) {
                d.pos.x = (prev < hx * S) ? (hx * S - he - 1e-4f) : (hx * S + S + he + 1e-4f);
                d.vel.x *= -0.18f;
            } else if (axis == 1) {
                // Resolve from where the drop was, not from the sign of velocity.
                // An upward pop while already touching the floor used to be treated
                // as a ceiling and pushed the drop under the ground.
                float blockMid = (hy + 0.5f) * S;
                if (prev >= blockMid) {
                    d.pos.y = hy * S + S + he + 1e-4f;
                    if (d.vel.y < -2.4f)
                        d.vel.y *= -0.28f;
                    else {
                        d.vel.y = 0.0f;
                        d.grounded = true;
                    }
                } else {
                    d.pos.y = hy * S - he - 1e-4f;
                    d.vel.y = 0.0f;
                }
            } else {
                d.pos.z = (prev < hz * S) ? (hz * S - he - 1e-4f) : (hz * S + S + he + 1e-4f);
                d.vel.z *= -0.18f;
            }
        };
        moveAxis(0, d.vel.x * dt);
        moveAxis(1, d.vel.y * dt);
        moveAxis(2, d.vel.z * dt);
        i++;
    }
}

void World::touchAuth(int x, int y, int z) {
    if (!m_authCapture) return;
    CellLoc c;
    if (!cellLoc(x, y, z, c)) return;
    auto it = m_chunks.find(chunkKey(c.cx, c.cy, c.cz));
    if (it == m_chunks.end()) return;
    AuthCell cell;
    cell.x = x;
    cell.y = y;
    cell.z = z;
    cell.block = it->second.get(c.lx, c.ly, c.lz);
    cell.water = it->second.levelAt(c.lx, c.ly, c.lz);
    cell.flags = it->second.flagAt(c.lx, c.ly, c.lz);
    m_authCells[AuthCellKey{ x, y, z }] = cell;
    rememberEdited(c.cx, c.cz);
}

void World::touchAuthSod(int64_t key) {
    if (!m_authCapture) return;
    m_authSod.insert(key);
    rememberEdited(chunkCX(key), chunkCZ(key));
}

void World::touchAuthBark(int64_t key) {
    if (!m_authCapture) return;
    m_authBark.insert(key);
    rememberEdited(chunkCX(key), chunkCZ(key));
}

void World::rememberEdited(int cx, int cz) {
    if (!m_keepEdited) return;
    m_editedCols.insert(columnKey(cx, cz));
}

void World::remeshChunk(int cx, int cy, int cz) {
    const int n[7][3] = {
        { 0, 0, 0 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, -1, 0 }, { 0, 1, 0 }
    };
    for (int i = 0; i < 7; i++) {
        int x = cx + n[i][0], y = cy + n[i][1], z = cz + n[i][2];
        auto it = m_chunks.find(chunkKey(x, y, z));
        if (it == m_chunks.end()) continue;
        buildMeshFor(it->second, x, y, z);
        it->second.dirty = false;
    }
}

uint32_t World::chunkAuthHash(const Chunk& ch) const {
    uint32_t h = 2166136261u;
    auto mix = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    };
    mix(ch.blocks.data(), ch.blocks.size());
    mix(ch.waterLevel.data(), ch.waterLevel.size());
    mix(ch.flags.data(), ch.flags.size());
    std::vector<AuthSod> sods;
    sods.reserve(ch.sodFaces.size());
    for (const Chunk::SodFace& sf : ch.sodFaces) {
        if (sf.face >= 6) continue;
        AuthSod face{ sf.x, sf.z, sf.y, sf.face, sf.stage, quantSodRem(sf.rem) };
        sods.push_back(face);
    }
    std::sort(sods.begin(), sods.end(), [](const AuthSod& a, const AuthSod& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        if (a.x != b.x) return a.x < b.x;
        return a.face < b.face;
    });
    for (const AuthSod& s : sods) {
        uint8_t b[6] = { s.x, s.z, s.y, s.face, s.stage, s.rem };
        mix(b, 6);
    }
    std::vector<AuthBark> barks;
    barks.reserve(ch.barkFaces.size());
    for (const Chunk::BarkFace& bf : ch.barkFaces)
        barks.push_back(AuthBark{ bf.x, bf.z, bf.y, bf.face });
    std::sort(barks.begin(), barks.end(), [](const AuthBark& a, const AuthBark& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        if (a.x != b.x) return a.x < b.x;
        return a.face < b.face;
    });
    for (const AuthBark& bk : barks) {
        uint8_t b[4] = { bk.x, bk.z, bk.y, bk.face };
        mix(b, 4);
    }
    return h;
}

std::vector<World::AuthSlice> World::flushAuth() {
    std::unordered_map<int64_t, AuthSlice> slices;
    for (const auto& kv : m_authCells) {
        const AuthCell& cell = kv.second;
        CellLoc c;
        if (!cellLoc(cell.x, cell.y, cell.z, c)) continue;
        int64_t key = chunkKey(c.cx, c.cy, c.cz);
        AuthSlice& s = slices[key];
        s.cx = c.cx;
        s.cy = c.cy;
        s.cz = c.cz;
        s.cells.push_back(cell);
    }
    for (int64_t key : m_authSod) {
        auto it = m_chunks.find(key);
        if (it == m_chunks.end()) continue;
        AuthSlice& s = slices[key];
        s.cx = chunkCX(key);
        s.cy = chunkCY(key);
        s.cz = chunkCZ(key);
        s.sod = true;
        s.sods.clear();
        s.sods.reserve(it->second.sodFaces.size());
        for (const Chunk::SodFace& sf : it->second.sodFaces) {
            if (sf.face >= 6) continue;
            s.sods.push_back(AuthSod{ sf.x, sf.z, sf.y, sf.face, sf.stage, quantSodRem(sf.rem) });
        }
    }
    for (int64_t key : m_authBark) {
        auto it = m_chunks.find(key);
        if (it == m_chunks.end()) continue;
        AuthSlice& s = slices[key];
        s.cx = chunkCX(key);
        s.cy = chunkCY(key);
        s.cz = chunkCZ(key);
        s.bark = true;
        s.barks.clear();
        s.barks.reserve(it->second.barkFaces.size());
        for (const Chunk::BarkFace& bf : it->second.barkFaces)
            s.barks.push_back(AuthBark{ bf.x, bf.z, bf.y, bf.face });
    }
    m_authCells.clear();
    m_authSod.clear();
    m_authBark.clear();
    std::vector<AuthSlice> out;
    out.reserve(slices.size());
    for (auto& kv : slices) out.push_back(std::move(kv.second));
    return out;
}

void World::ensureColumn(int cx, int cz) {
    if (columnLoaded(cx, cz)) return;
    generateColumn(cx, cz);
    for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) {
        auto it = m_chunks.find(chunkKey(cx, cy, cz));
        if (it == m_chunks.end()) continue;
        if (!it->second.sodValid) rebuildSod(it->second, cx, cy, cz);
        addBorderSod(cx, cy, cz);
        m_meshQueue.push_back(it->first);
    }
    for (int nz = -1; nz <= 1; nz++) {
        for (int nx = -1; nx <= 1; nx++) {
            if (nx == 0 && nz == 0) continue;
            for (int cy = 0; cy < cfg::CHUNK_LAYERS; cy++) {
                auto nit = m_chunks.find(chunkKey(cx + nx, cy, cz + nz));
                if (nit == m_chunks.end()) continue;
                nit->second.dirty = true;
                m_meshQueue.push_front(nit->first);
            }
        }
    }
}

void World::acceptSeedChunk(int cx, int cy, int cz) {
    int64_t key = chunkKey(cx, cy, cz);
    auto it = m_chunks.find(key);
    bool changed = false;
    if (it != m_chunks.end() && it->second.modified) {
        m_chunks.erase(it);
        changed = true;
    }
    if (!chunkExists(cx, cy, cz)) {
        generateColumn(cx, cz);
        changed = true;
    }
    it = m_chunks.find(key);
    if (it == m_chunks.end()) return;
    if (!it->second.sodValid) {
        rebuildSod(it->second, cx, cy, cz);
        addBorderSod(cx, cy, cz);
        changed = true;
    }
    if (changed) remeshChunk(cx, cy, cz);
}

void World::writeAuthChunk(int cx, int cy, int cz, const uint8_t* blocks, const uint8_t* water,
                           const uint8_t* flags, const std::vector<AuthSod>& sod,
                           const std::vector<AuthBark>& bark) {
    if (!blocks || !water || !flags) return;
    if (!columnLoaded(cx, cz)) ensureColumn(cx, cz);
    int64_t key = chunkKey(cx, cy, cz);
    auto it = m_chunks.find(key);
    if (it == m_chunks.end()) {
        Chunk slice;
        slice.generated = true;
        it = m_chunks.emplace(key, std::move(slice)).first;
    }
    Chunk& ch = it->second;
    ch.blocks.assign(blocks, blocks + cfg::CHUNK_VOLUME);
    ch.waterLevel.assign(water, water + cfg::CHUNK_VOLUME);
    ch.flags.assign(flags, flags + cfg::CHUNK_VOLUME);
    ch.treeId.clear();
    ch.barkFaces.clear();
    ch.barkFaces.reserve(bark.size());
    for (const AuthBark& bk : bark) {
        if (bk.face >= 6) continue;
        ch.barkFaces.push_back(Chunk::BarkFace{ bk.x, bk.z, bk.y, bk.face });
    }
    ch.sodFaces.clear();
    ch.sodFaces.reserve(sod.size());
    for (const AuthSod& s : sod) {
        if (s.face >= 6) continue;
        Chunk::SodFace sf;
        sf.x = s.x;
        sf.z = s.z;
        sf.y = s.y;
        sf.face = s.face;
        sf.stage = s.stage;
        sf.rem = dequantSodRem(s.rem);
        ch.sodFaces.push_back(sf);
    }
    ch.sodValid = true;
    ch.generated = true;
    ch.modified = true;
    ch.hasWater = false;
    for (size_t i = 0; i < ch.waterLevel.size() && i < ch.blocks.size(); i++) {
        if (ch.blocks[i] == WATER && ch.waterLevel[i] > 0) { ch.hasWater = true; break; }
    }
    rememberEdited(cx, cz);
    remeshChunk(cx, cy, cz);
}

bool World::writeAuthDelta(int cx, int cy, int cz, const std::vector<AuthCell>& cells,
                           bool replaceSod, const std::vector<AuthSod>& sod,
                           bool replaceBark, const std::vector<AuthBark>& bark) {
    auto it = m_chunks.find(chunkKey(cx, cy, cz));
    if (it == m_chunks.end()) return false;
    Chunk& ch = it->second;
    for (const AuthCell& cell : cells) {
        CellLoc c;
        if (!cellLoc(cell.x, cell.y, cell.z, c)) continue;
        if (c.cx != cx || c.cy != cy || c.cz != cz) continue;
        ch.set(c.lx, c.ly, c.lz, cell.block);
        ch.setLevel(c.lx, c.ly, c.lz, cell.water);
        ch.setFlag(c.lx, c.ly, c.lz, cell.flags);
        if (cell.block == WATER && cell.water > 0) ch.hasWater = true;
    }
    if (replaceSod) {
        std::vector<Chunk::SodFace> prev = ch.sodFaces;
        ch.sodFaces.clear();
        ch.sodFaces.reserve(sod.size());
        float maxD = loot::sodBreak().durability;
        for (const AuthSod& s : sod) {
            if (s.face >= 6) continue;
            Chunk::SodFace sf;
            sf.x = s.x;
            sf.z = s.z;
            sf.y = s.y;
            sf.face = s.face;
            sf.stage = s.stage;
            sf.rem = dequantSodRem(s.rem);
            for (const Chunk::SodFace& old : prev) {
                if (old.x != sf.x || old.z != sf.z || old.y != sf.y || old.face != sf.face) continue;
                if (old.rem < 0.0f) break;
                float incoming = (sf.rem < 0.0f) ? maxD : sf.rem;
                if (old.rem < incoming) sf.rem = old.rem;
                break;
            }
            ch.sodFaces.push_back(sf);
        }
        ch.sodValid = true;
    }
    if (replaceBark) {
        ch.barkFaces.clear();
        ch.barkFaces.reserve(bark.size());
        for (const AuthBark& bk : bark) {
            if (bk.face >= 6) continue;
            ch.barkFaces.push_back(Chunk::BarkFace{ bk.x, bk.z, bk.y, bk.face });
        }
    }
    ch.generated = true;
    ch.modified = true;
    ch.dirty = true;
    rememberEdited(cx, cz);
    remeshChunk(cx, cy, cz);
    return true;
}

void World::tagIsland(PhysicsIsland& t) {
    if (!m_tagTrees || t.netId != 0) return;
    t.netId = m_nextTree++;
    if (m_nextTree == 0) m_nextTree = 1;
    if (t.contentRev == 0) t.contentRev = 1;
}

static void mixByte(uint32_t& h, uint8_t v) { h ^= v; h *= 16777619u; }

static uint32_t hashTreeCells(uint32_t h, const PhysicsIsland& t) {
    mixByte(h, (uint8_t)t.netId);
    mixByte(h, (uint8_t)(t.netId >> 8));
    mixByte(h, (uint8_t)(t.netId >> 16));
    mixByte(h, (uint8_t)(t.netId >> 24));
    std::vector<IVec3> cells = t.cells;
    std::sort(cells.begin(), cells.end(), [](const IVec3& a, const IVec3& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.x < b.x;
    });
    for (const IVec3& c : cells) {
        if (c.x < 0 || c.y < 0 || c.z < 0 || c.x > 255 || c.y > 255 || c.z > 255) continue;
        mixByte(h, (uint8_t)c.x);
        mixByte(h, (uint8_t)c.y);
        mixByte(h, (uint8_t)c.z);
        mixByte(h, t.get(c.x, c.y, c.z));
        mixByte(h, t.flagAt(c.x, c.y, c.z));
    }
    return h;
}

uint32_t World::treeAuthHash() const {
    std::vector<const PhysicsIsland*> list;
    list.reserve(m_phys.size());
    for (const PhysicsIsland& t : m_phys)
        if (t.netId) list.push_back(&t);
    std::sort(list.begin(), list.end(), [](const PhysicsIsland* a, const PhysicsIsland* b) {
        return a->netId < b->netId;
    });
    uint32_t h = 2166136261u;
    for (const PhysicsIsland* t : list) h = hashTreeCells(h, *t);
    return h;
}

uint32_t World::treeAuthHashOf(const std::vector<uint32_t>& ids) const {
    std::vector<uint32_t> order = ids;
    std::sort(order.begin(), order.end());
    uint32_t h = 2166136261u;
    for (uint32_t id : order) {
        for (const PhysicsIsland& t : m_phys) {
            if (t.netId != id) continue;
            h = hashTreeCells(h, t);
            break;
        }
    }
    return h;
}

void World::exportNetTree(const PhysicsIsland& t, NetTree& out, bool withCells) const {
    out = NetTree{};
    out.id = t.netId;
    out.rev = t.contentRev;
    out.cells = withCells;
    out.ox = t.originX;
    out.oy = t.originY;
    out.oz = t.originZ;
    out.com = t.com;
    out.vel = t.vel;
    out.omega = t.omega;
    out.ax = t.ax;
    out.ay = t.ay;
    out.az = t.az;
    out.pivot = t.pivotRest;
    out.hold = t.holdPivot;
    out.still = t.stillTime;
    if (!withCells) return;
    out.body.reserve(t.cells.size());
    for (const IVec3& c : t.cells) {
        if (c.x < 0 || c.y < 0 || c.z < 0 || c.x > 255 || c.y > 255 || c.z > 255) continue;
        NetTreeCell cell;
        cell.x = (uint8_t)c.x;
        cell.y = (uint8_t)c.y;
        cell.z = (uint8_t)c.z;
        cell.block = t.get(c.x, c.y, c.z);
        cell.flags = t.flagAt(c.x, c.y, c.z);
        if (cell.block == AIR) continue;
        out.body.push_back(cell);
    }
}

static void copyTreePose(PhysicsIsland& t, const World::NetTree& n) {
    t.com = n.com;
    t.vel = n.vel;
    t.omega = n.omega;
    t.ax = n.ax;
    t.ay = n.ay;
    t.az = n.az;
    t.pivotRest = n.pivot;
    t.holdPivot = n.hold;
    t.stillTime = n.still;
    t.contentRev = n.rev;
}

void World::clearIslandMines(size_t index) {
    std::vector<uint64_t> drop;
    for (const auto& kv : m_blockDur) {
        int phys = -1, x = 0, y = 0, z = 0;
        decodeMineKey(kv.first, phys, x, y, z);
        if (phys >= 0 && (size_t)phys == index) drop.push_back(kv.first);
    }
    for (uint64_t k : drop) m_blockDur.erase(k);
}

void World::forgetIslandMines(size_t index) {
    if (index >= m_phys.size()) return;
    size_t last = m_phys.size() - 1;
    std::vector<uint64_t> drop;
    std::vector<std::pair<uint64_t, MineState>> slide;
    for (const auto& kv : m_blockDur) {
        int phys = -1, x = 0, y = 0, z = 0;
        decodeMineKey(kv.first, phys, x, y, z);
        if (phys < 0) continue;
        if ((size_t)phys == index) drop.push_back(kv.first);
        else if (index != last && (size_t)phys == last) {
            drop.push_back(kv.first);
            slide.push_back({ makeDurKey((int)index, x, y, z), kv.second });
        }
    }
    for (uint64_t k : drop) m_blockDur.erase(k);
    for (auto& p : slide) m_blockDur[p.first] = std::move(p.second);
}

bool World::applyNetTrees(const std::vector<NetTree>& trees, const std::vector<uint32_t>& gone) {
    bool ok = true;
    for (uint32_t id : gone) {
        for (size_t i = 0; i < m_phys.size();) {
            if (m_phys[i].netId == id) {
                forgetIslandMines(i);
                m_phys[i] = std::move(m_phys.back());
                m_phys.pop_back();
            } else {
                i++;
            }
        }
    }
    for (const NetTree& n : trees) {
        if (n.id == 0) continue;
        PhysicsIsland* found = nullptr;
        for (PhysicsIsland& t : m_phys) {
            if (t.netId == n.id) { found = &t; break; }
        }
        if (!n.cells) {
            if (!found) { ok = false; continue; }
            copyTreePose(*found, n);
            continue;
        }
        if (n.body.empty()) {
            if (found) {
                size_t idx = (size_t)(found - m_phys.data());
                forgetIslandMines(idx);
                if (idx + 1 != m_phys.size()) m_phys[idx] = std::move(m_phys.back());
                m_phys.pop_back();
            }
            continue;
        }
        int maxx = 1, maxy = 1, maxz = 1;
        for (const NetTreeCell& c : n.body) {
            maxx = std::max(maxx, (int)c.x + 1);
            maxy = std::max(maxy, (int)c.y + 1);
            maxz = std::max(maxz, (int)c.z + 1);
        }
        PhysicsIsland built;
        tree_fall::allocate(built, maxx, maxy, maxz);
        built.originX = n.ox;
        built.originY = n.oy;
        built.originZ = n.oz;
        for (const NetTreeCell& c : n.body)
            tree_fall::setCell(built, c.x, c.y, c.z, c.block, c.flags);
        tree_fall::recomputeMass(built);
        tree_fall::buildMesh(built);
        copyTreePose(built, n);
        built.netId = n.id;
        built.meshDirty = false;
        if (found) {
            clearIslandMines((size_t)(found - m_phys.data()));
            *found = std::move(built);
        }
        else m_phys.push_back(std::move(built));
    }
    return ok;
}

uint8_t World::quantSodRem(float rem) {
    float maxD = loot::sodBreak().durability;
    if (rem < 0.0f || maxD <= 0.001f) return 255;
    float t = rem / maxD;
    if (t >= 0.999f) return 255;
    int q = (int)(t * 254.0f + 0.5f);
    if (q < 0) q = 0;
    if (q > 254) q = 254;
    return (uint8_t)q;
}

float World::dequantSodRem(uint8_t q) {
    float maxD = loot::sodBreak().durability;
    if (q >= 255 || maxD <= 0.001f) return -1.0f;
    return maxD * ((float)q / 254.0f);
}

static int physOfTree(const std::vector<PhysicsIsland>& islands, uint32_t tree) {
    if (tree == 0) return -1;
    for (int i = 0; i < (int)islands.size(); i++)
        if (islands[(size_t)i].netId == tree) return i;
    return -2;
}

void World::collectMineViews(std::vector<MineView>& out) const {
    out.clear();
    for (const auto& kv : m_blockDur) {
        int phys = -1, x = 0, y = 0, z = 0;
        decodeMineKey(kv.first, phys, x, y, z);
        MineView v;
        v.x = x;
        v.y = y;
        v.z = z;
        v.rem = kv.second.rem;
        v.serial = kv.second.serial;
        v.nHits = kv.second.nHits;
        if (v.nHits > kMaxMineHits) v.nHits = kMaxMineHits;
        for (int i = 0; i < v.nHits; i++) v.hits[i] = kv.second.hits[i];
        if (phys < 0) {
            v.tree = 0;
        } else if (phys < (int)m_phys.size() && m_phys[(size_t)phys].netId != 0) {
            v.tree = m_phys[(size_t)phys].netId;
        } else {
            continue;
        }
        out.push_back(v);
    }
}

void World::applyMineView(uint32_t tree, int x, int y, int z, float rem, const MineHit* hits, int nHits) {
    int phys = physOfTree(m_phys, tree);
    if (phys < -1) return;
    if (nHits < 0) nHits = 0;
    if (nHits > kMaxMineHits) nHits = kMaxMineHits;
    uint64_t key = makeDurKey(phys, x, y, z);
    auto it = m_blockDur.find(key);
    if (it != m_blockDur.end() && it->second.nHits > nHits) return;
    MineState st;
    st.rem = rem;
    st.nHits = nHits;
    for (int i = 0; i < nHits; i++) st.hits[i] = hits ? hits[i] : MineHit{};
    m_blockDur[key] = st;
}

void World::clearMineView(uint32_t tree, int x, int y, int z) {
    int phys = physOfTree(m_phys, tree);
    if (phys < -1) return;
    m_blockDur.erase(makeDurKey(phys, x, y, z));
}

