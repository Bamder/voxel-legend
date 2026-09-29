#include "building_loot.hpp"
#include "loot.hpp"
#include "world.hpp"
#include "matchmap.hpp"
#include <cmath>

namespace building_loot {
namespace {
bool validStack(uint8_t item, uint8_t count) {
    return item != AIR && validBlock(item) && count > 0 &&
        count <= loot::maxStack(item) && count <= cfg::MAX_STACK;
}
}

bool Spawner::validPosition(Vec3 p) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    // Bound floats before conversion to voxel indices.
    const float edge = float((matchmap::rimMax() + 1) * cfg::CHUNK_X) * cfg::BLOCK_SCALE;
    if (std::fabs(p.x) >= edge || std::fabs(p.z) >= edge || p.y < 0 ||
        p.y >= cfg::WORLD_H * cfg::BLOCK_SCALE) return false;
    int x = (int)std::floor(p.x / cfg::BLOCK_SCALE);
    int y = (int)std::floor(p.y / cfg::BLOCK_SCALE);
    int z = (int)std::floor(p.z / cfg::BLOCK_SCALE);
    if (!world_.columnLoaded(floorDiv(x, cfg::CHUNK_X), floorDiv(z, cfg::CHUNK_Z))) return false;
    return !isSolid(world_.getBlock(x, y, z));
}

bool Spawner::spawnItemAt(Vec3 position, ItemSlot item) {
    if (!validPosition(position) || !validStack(item.block, item.count)) return false;
    world_.spawnDrop(position, item.block, item.count, true);
    return true;
}

bool Spawner::spawnRandomLootAt(Vec3 position, std::span<const Entry> table) {
    if (!validPosition(position) || table.empty() || table.size() > kMaxTableEntries) return false;
    uint32_t total = 0;
    // Reject the entire malformed table before drawing or spawning anything.
    for (const auto& entry : table) {
        if (!entry.weight || entry.minCount > entry.maxCount ||
            !validStack(entry.item, entry.minCount) || !validStack(entry.item, entry.maxCount)) return false;
        total += entry.weight;
    }
    uint32_t choice = std::uniform_int_distribution<uint32_t>(0, total - 1)(rng_);
    for (const auto& entry : table) {
        if (choice < entry.weight) {
            uint8_t count = (uint8_t)std::uniform_int_distribution<int>(entry.minCount, entry.maxCount)(rng_);
            return spawnItemAt(position, {entry.item, count});
        }
        choice -= entry.weight;
    }
    return false;
}
}
