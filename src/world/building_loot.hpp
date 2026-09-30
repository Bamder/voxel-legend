#pragma once
#include "blocks.hpp"
#include "clue.hpp"
#include "../core/math.hpp"
#include <cstdint>
#include <random>
#include <span>

class World;

namespace building_loot {
// A weighted item choice at a position supplied by the building module.
// This is a server API, not a client message or a data-pack format.
struct Entry {
    uint8_t item = AIR;
    uint8_t minCount = 1;
    uint8_t maxCount = 1;
    uint16_t weight = 1;
};
inline constexpr size_t kMaxTableEntries = 64;

// Owned by the Dedicated Server for one match. Building code needs only this
// capability and registered item IDs; it does not depend on combat internals.
// Call once after a building is placed, not every time its chunks are loaded.
// No default world scatter, startup loadout, or client-side RNG.
class Spawner {
public:
    explicit Spawner(World& world, uint32_t seed) : world_(world), rng_(seed) {}
    Spawner(const Spawner&) = delete;
    Spawner& operator=(const Spawner&) = delete;
    bool spawnItemAt(Vec3 position, ItemSlot item);
    bool spawnRandomLootAt(Vec3 position, std::span<const Entry> table);
    // Clues must use this overload so their drop id remains bound to the
    // server-owned route. Generic loot tables deliberately reject ITEM_CLUE.
    uint32_t spawnClueAt(Vec3 position, clue::Director& director, const clue::Link& link);

private:
    bool validPosition(Vec3 position) const;
    World& world_;
    std::mt19937 rng_;
};
}
