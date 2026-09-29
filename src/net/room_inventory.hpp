#pragma once
#include "../world/blocks.hpp"
#include "../world/vitals.hpp"
#include "../core/config.hpp"
#include <array>
#include <cstdint>

class World;

namespace room_inventory {
using Slots = std::array<ItemSlot, cfg::INVENTORY_SLOTS>;

struct State {
    Slots slots{};
    uint32_t revision = 1;
    uint32_t lastLayoutSequence = 0;
    uint32_t lastPickupSequence = 0;
};

bool validSlot(ItemSlot slot);
int add(State& state, uint8_t item, int count);
bool consume(State& state, int slot, uint8_t expectedItem, int count = 1);
bool acceptLayout(State& state, uint32_t sequence, uint32_t baseRevision,
                  const Slots& requested);
uint8_t held(const State& state, const vitals::Vitals& body, int slot);
bool pickup(State& state, uint32_t sequence, World& world, uint32_t dropId,
            Vec3 eye, Vec3 feet);
}
