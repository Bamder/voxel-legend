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

bool reachable(const World& world, const loot::Drop& drop, Vec3 eye, Vec3 feet) {
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z) ||
        !std::isfinite(feet.x) || !std::isfinite(feet.y) || !std::isfinite(feet.z)) return false;
    Vec3 delta = drop.pos - eye;
    float distance = delta.length();
    if (!std::isfinite(distance) || distance > cfg::REACH ||
        std::fabs(drop.pos.x - feet.x) > cfg::REACH ||
        std::fabs(drop.pos.z - feet.z) > cfg::REACH) return false;
    if (distance <= 1e-6f) return true;

    Vec3 direction = delta / distance;
    float traveled = 0.0f;
    // World::raycast selects decorative grass/water too. They can cover a
    // floor drop visually but must not count as a solid pickup wall.
    for (int i = 0; i < 32 && traveled < distance; ++i) {
        IVec3 hit{}, prev{}; Vec3 normal{};
        int phys = -1;
        float wall = distance - traveled + 1.0f;
        if (!world.raycast(eye + direction * traveled, direction, distance - traveled,
                           hit, prev, normal, &phys, &wall) ||
            traveled + wall + .05f >= distance) return true;
        if (phys >= 0 || blocksMotion(world.getBlock(hit.x, hit.y, hit.z))) return false;
        traveled += wall + .002f;
    }
    return false;
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

int nearbyDrop(const World& world, Vec3 eye, Vec3 feet, float radius) {
    if (!std::isfinite(radius) || radius <= 0.0f || radius > cfg::REACH) return -1;
    int best = -1;
    float bestDistSq = radius * radius;
    const auto& drops = world.drops();
    for (int i = 0; i < (int)drops.size(); ++i) {
        const loot::Drop& drop = drops[(size_t)i];
        if (!drop.netId || drop.item == AIR || !drop.count) continue;
        float dx = drop.pos.x - feet.x, dz = drop.pos.z - feet.z;
        float distSq = dx * dx + dz * dz;
        if (distSq >= bestDistSq || std::fabs(drop.pos.y - feet.y) > 2.2f ||
            !reachable(world, drop, eye, feet)) continue;
        best = i;
        bestDistSq = distSq;
    }
    return best;
}

bool pickup(State& state, uint32_t sequence, World& world, uint32_t dropId,
            Vec3 eye, Vec3 feet) {
    if (!newer(sequence, state.lastPickupSequence)) return false;
    state.lastPickupSequence = sequence;
    if (!canPickup(state, world, dropId, eye, feet)) return false;
    const loot::Drop* found = world.dropById(dropId);
    loot::Drop drop = *found;
    State next = state;
    int left = add(next, drop.item, drop.count);
    int taken = (int)drop.count - left;
    if (taken <= 0 || !world.takeDropCountById(dropId, (uint8_t)taken)) return false;
    state.slots = next.slots;
    state.revision = next.revision;
    return true;
}

bool canPickup(const State& state, const World& world, uint32_t dropId,
               Vec3 eye, Vec3 feet) {
    const loot::Drop* drop = world.dropById(dropId);
    if (!drop || !drop->count || !reachable(world, *drop, eye, feet)) return false;
    State next = state;
    return add(next, drop->item, drop->count) < drop->count;
}
}
