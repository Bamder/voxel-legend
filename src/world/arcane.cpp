#include "arcane.hpp"
#include "../core/config.hpp"
#include <algorithm>
#include <cmath>

namespace arcane {
namespace {
bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool newer(uint32_t value, uint32_t previous) {
    return value != previous && reached(value, previous);
}
}

bool reached(uint32_t now, uint32_t then) {
    return uint32_t(now - then) < 0x80000000u;
}

uint32_t ticks(float seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0f) return 0;
    return (uint32_t)std::ceil(seconds * (float)cfg::TICKS_PER_SECOND);
}

bool beginFireball(CastState& state, uint32_t sequence, uint32_t tick,
                   const combat::Actor& actor, const vitals::Vitals& body,
                   combat::Hand hand, uint8_t ownedItem, bool blocked) {
    if (!newer(sequence, state.lastSequence)) return false;
    state.lastSequence = sequence;
    if (!actor.active || !actor.id || actor.category != combat::EntityCategory::Player ||
        actor.team < 1 || actor.team > 4 || ownedItem != ITEM_ARCANE_FIREBALL || blocked ||
        !combat::handUsable(body, hand) || !reached(tick, state.readyAt)) return false;
    state.readyAt = tick + ticks(kFireballCooldownSeconds);
    return true;
}

Projectile makeFireball(uint32_t id, uint32_t owner, uint32_t action, int team,
                        Vec3 origin, Vec3 direction, uint32_t tick) {
    Projectile out;
    float lengthSq = direction.lengthSq();
    if (!id || !owner || team < 1 || team > 4 || !finite(origin) || !finite(direction) ||
        !std::isfinite(lengthSq) || lengthSq < 1e-10f) return out;
    out.id = id;
    out.owner = owner;
    out.action = action;
    out.team = team;
    out.pos = origin;
    out.vel = direction.normalized() * kFireballSpeed;
    out.expireAt = tick + ticks(kFireballTtlSeconds);
    return out;
}

float explosionDamage(float distance) {
    if (!std::isfinite(distance) || distance < 0.0f || distance > kFireballExplosionRadius) return 0.0f;
    float u = distance / kFireballExplosionRadius;
    return kFireballCenterDamage * (1.0f - (1.0f - kFireballEdgeScale) * u);
}

void ignite(Burning& state, uint32_t source, uint32_t action, uint32_t tick) {
    if (!source) return;
    if (!state.active || state.source != source) {
        state.source = source;
        state.nextDamage = tick + ticks(1.0f);
    }
    state.action = action;
    state.until = tick + ticks(kBurnSeconds);
    state.active = true;
}

bool takeBurnDamage(Burning& state, uint32_t tick) {
    if (!state.active || !state.source || !reached(tick, state.nextDamage) ||
        !reached(state.until, state.nextDamage)) return false;
    state.nextDamage += ticks(1.0f);
    if (reached(tick, state.until)) state.active = false;
    return true;
}

bool burning(const Burning& state, uint32_t tick) {
    return state.active && state.source != 0 && !reached(tick, state.until);
}

} // namespace arcane
