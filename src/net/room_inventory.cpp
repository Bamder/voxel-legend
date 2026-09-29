#include "room_inventory.hpp"
#include "../world/combat.hpp"
#include "../world/loot.hpp"
#include "../world/world.hpp"
#include <array>
#include <cmath>

namespace room_inventory {
namespace {
bool newer(uint32_t value, uint32_t previous) {
    return value != previous && uint32_t(value - previous) < 0x80000000u;
}
std::array<int,256> totals(const Slots& slots) {
    std::array<int,256> out{};
    for (auto slot : slots) if (!slot.empty()) out[slot.block] += slot.count;
    return out;
}
}

bool validSlot(ItemSlot slot) {
    if (slot.empty()) return slot.block == AIR && slot.count == 0;
    return validBlock(slot.block) && slot.count <= loot::maxStack(slot.block) &&
        slot.count <= cfg::MAX_STACK;
}

int add(State& state, uint8_t item, int count) {
    if (item == AIR || !validBlock(item) || count <= 0) return count;
    int before = count;
    const int max = loot::maxStack(item);
    for (auto& slot : state.slots) {
        if (slot.empty() || slot.block != item || slot.count >= max) continue;
        int n = std::min(count, max - (int)slot.count);
        slot.count = (uint8_t)(slot.count + n); count -= n;
        if (!count) break;
    }
    for (auto& slot : state.slots) {
        if (!count) break;
        if (!slot.empty()) continue;
        int n = std::min(count, max);
        slot = {item, (uint8_t)n}; count -= n;
    }
    if (count != before) ++state.revision;
    return count;
}

bool consume(State& state, int slot, uint8_t expectedItem, int count) {
    if (slot < 0 || slot >= (int)state.slots.size() || expectedItem == AIR ||
        !validBlock(expectedItem) || count <= 0) return false;
    ItemSlot& item = state.slots[(size_t)slot];
    if (item.empty() || item.block != expectedItem || item.count < count) return false;
    item.count = (uint8_t)(item.count - count);
    if (!item.count) item.clear();
    ++state.revision;
    return true;
}

bool acceptLayout(State& state, uint32_t sequence, uint32_t baseRevision,
                  const Slots& requested) {
    if (!newer(sequence, state.lastLayoutSequence)) return false;
    state.lastLayoutSequence = sequence; // rejected messages cannot be replayed later
    if (baseRevision != state.revision) return false;
    for (auto slot : requested) if (!validSlot(slot)) return false;
    if (totals(requested) != totals(state.slots)) return false;
    state.slots = requested;
    ++state.revision;
    return true;
}

uint8_t held(const State& state, const vitals::Vitals& body, int slot) {
    if (!combat::slotUsable(body, slot)) return AIR;
    const ItemSlot& item = state.slots[(size_t)slot];
    return item.empty() ? (uint8_t)AIR : item.block;
}

bool pickup(State& state, uint32_t sequence, World& world, uint32_t dropId,
            Vec3 eye, Vec3 feet) {
    if (!newer(sequence, state.lastPickupSequence)) return false;
    state.lastPickupSequence = sequence;
    const loot::Drop* found = world.dropById(dropId);
    if (!found || !std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z)) return false;
    loot::Drop drop = *found;
    Vec3 delta = drop.pos - eye;
    float distance = delta.length();
    if (!std::isfinite(distance) || distance > cfg::REACH ||
        std::fabs(drop.pos.x - feet.x) > cfg::REACH || std::fabs(drop.pos.z - feet.z) > cfg::REACH) return false;
    IVec3 hit{}, prev{}; Vec3 normal{}; float wall = cfg::REACH + 1;
    if (world.raycast(eye, delta, distance, hit, prev, normal, nullptr, &wall) && wall + .05f < distance) return false;
    State next = state;
    int left = add(next, drop.item, drop.count);
    int taken = (int)drop.count - left;
    if (taken <= 0 || !world.takeDropCountById(dropId, (uint8_t)taken)) return false;
    state.slots = next.slots;
    state.revision = next.revision;
    return true;
}
}
