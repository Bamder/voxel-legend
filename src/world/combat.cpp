#include "combat.hpp"
#include "../core/config.hpp"
#include "matchmap.hpp"
#include <algorithm>
#include <cmath>

namespace combat {
namespace {
bool finite(Vec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool validBody(const vitals::Vitals& v) {
    for (const auto& limb : v.limb)
        if (!std::isfinite(limb.health) || limb.health < 0 || limb.health > 1)
            return false;
    return true;
}
bool alive(const vitals::Vitals& v) { return validBody(v) && !vitals::isDead(v); }
bool reached(uint32_t now, uint32_t then) {
    return uint32_t(now - then) < 0x80000000u;
}
bool newer(uint32_t value, uint32_t previous) {
    return value != previous && reached(value, previous);
}
uint32_t ticks(float seconds) {
    return (uint32_t)std::ceil(seconds * cfg::TICKS_PER_SECOND);
}
bool rayBox(Vec3 o, Vec3 d, Vec3 lo, Vec3 hi, float maxT, float& hit) {
    float enter = 0, leave = maxT;
    const float origin[] = {o.x, o.y, o.z};
    const float dir[] = {d.x, d.y, d.z};
    const float mn[] = {lo.x, lo.y, lo.z}, mx[] = {hi.x, hi.y, hi.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-7f) {
            if (origin[i] < mn[i] || origin[i] > mx[i]) return false;
        } else {
            float a = (mn[i] - origin[i]) / dir[i];
            float b = (mx[i] - origin[i]) / dir[i];
            if (a > b) std::swap(a, b);
            enter = std::max(enter, a);
            leave = std::min(leave, b);
            if (enter > leave) return false;
        }
    }
    hit = enter;
    return true;
}

std::optional<LimbHit> hitPlayer(const Vec3& origin, const Vec3& direction,
    const Vec3& feet, float bodyYaw, float reach, float obstructionDistance,
    float radius) {
    if (!finite(origin) || !finite(direction) || !finite(feet) || !std::isfinite(bodyYaw) ||
        !std::isfinite(reach) || !std::isfinite(obstructionDistance) || !std::isfinite(radius) ||
        reach <= 0 || obstructionDistance < 0 || radius < 0 || radius > 1.0f ||
        !std::isfinite(direction.lengthSq()) || direction.lengthSq() < 1e-10f)
        return std::nullopt;
    float c = std::cos(bodyYaw), s = std::sin(bodyYaw);
    // Inverse of pm::lookYawXZ.
    auto local = [&](Vec3 p) { return Vec3{-p.x*c - p.z*s, p.y, p.x*s - p.z*c}; };
    Vec3 o = local(origin - feet), d = local(direction.normalized());
    struct Box { Vec3 lo, hi; int limb; };
    const Box boxes[] = {
        {{-.18f,1.44f,-.18f},{.18f,1.80f,.18f},vitals::Head},
        {{-.20f,1.08f,-.16f},{.20f,1.44f,.16f},vitals::Chest},
        {{-.20f,.72f,-.16f},{.20f,1.08f,.16f},vitals::Core},
        // arm_r is model -X (editor Tool R); arm_l is model +X.
        {{-.36f,.80f,-.16f},{-.20f,1.44f,.16f},vitals::HandR},
        {{.20f,.80f,-.16f},{.36f,1.44f,.16f},vitals::HandL},
        {{-.20f,0,-.16f},{0,.72f,.16f},vitals::FootL},
        {{0,0,-.16f},{.20f,.72f,.16f},vitals::FootR}
    };
    const Vec3 inflate{radius, radius, radius};
    std::optional<LimbHit> result;
    float best = std::min(reach, obstructionDistance);
    for (const auto& box : boxes) {
        float t = 0;
        if (rayBox(o, d, box.lo - inflate, box.hi + inflate, best, t) &&
            t < obstructionDistance && (!result || t < best)) {
            result = LimbHit{box.limb, t};
            best = t;
        }
    }
    return result;
}
} // namespace

std::optional<Weapon> weapon(uint8_t item) {
    // Combat timings are deliberately separate from the existing mining table.
    if (item == HAND_PICK) return Weapon{item, {0.14f, 0.35f, false}, 4.5f, 0.55f, 0.55f};
    if (item == HAND_AXE) return Weapon{item, {0.10f, 2.5f, false}, 4.5f, 0.85f, 0.85f};
    return std::nullopt;
}

bool hostile(const Actor& a, const Actor& b) {
    if (!a.id || !b.id || a.id == b.id || !a.active || !b.active) return false;
    if (a.category != EntityCategory::Player || a.team < 1 || a.team > matchmap::kCombatTeams) return false;
    if (b.category == EntityCategory::Boss) return true;
    return b.category == EntityCategory::Player && b.team >= 1 && b.team <= matchmap::kCombatTeams && a.team != b.team;
}

bool handUsable(const vitals::Vitals& v, Hand hand) {
    if (hand != Hand::Left && hand != Hand::Right) return false;
    return alive(v) && v.limb[hand == Hand::Left ? vitals::HandL : vitals::HandR].health > vitals::kDeadEps;
}

bool slotUsable(const vitals::Vitals& v, int slot) {
    if (slot < 0 || slot >= cfg::HOTBAR_SLOTS) return false;
    return handUsable(v, slot < cfg::HAND_SLOTS ? Hand::Left : Hand::Right);
}

Mobility mobility(const vitals::Vitals& v, bool grounded, bool jump, bool swimming, bool frozen) {
    if (!alive(v) || frozen) return {};
    int legs = (v.limb[vitals::FootL].health > vitals::kDeadEps ? 1 : 0)
             + (v.limb[vitals::FootR].health > vitals::kDeadEps ? 1 : 0);
    if (!legs) return {};
    if (legs == 1) {
        bool canJump = !swimming && vitals::canJump(v);
        return { !swimming && (!grounded || (jump && canJump)), canJump, false, true, 0.4f };
    }
    return {true, vitals::canJump(v), vitals::canSprint(v), false, 1.0f};
}

float damageFor(const DamageSpec& spec, EntityCategory target) {
    if (!std::isfinite(spec.base) || spec.base <= 0 || spec.base > 1 ||
        !std::isfinite(spec.bossMultiplier) || spec.bossMultiplier < 0 ||
        (target != EntityCategory::Player && target != EntityCategory::Boss)) return 0;
    float value = spec.base * (target == EntityCategory::Boss ? spec.bossMultiplier : 1.0f);
    return std::isfinite(value) ? value : 0;
}

DamageResult damagePlayer(vitals::Vitals& v, const DamageSource& source, const DamageSpec& spec, int limb) {
    DamageResult out;
    if (!source.attacker || !alive(v) ||
        (source.category != DamageCategory::Physical && source.category != DamageCategory::Fire &&
         source.category != DamageCategory::Frost)) return out;
    float amount = damageFor(spec, EntityCategory::Player);
    if (amount <= 0 || (!spec.wholeBody && (limb < 0 || limb >= vitals::Count))) return out;
    for (int i = 0; i < vitals::Count; ++i) {
        if (!spec.wholeBody && i != limb) continue;
        float before = v.limb[i].health;
        vitals::hurt(v, i, amount);
        out.amount += before - v.limb[i].health;
    }
    out.applied = out.amount > 0;
    out.killed = vitals::isDead(v);
    out.limb = spec.wholeBody ? -1 : limb;
    return out;
}

bool healPlayer(vitals::Vitals& v, float fraction) {
    if (!alive(v) || !std::isfinite(fraction) || fraction <= 0 || fraction > 1) return false;
    bool changed = false;
    for (auto& limb : v.limb) {
        float after = std::min(1.0f, limb.health + fraction);
        changed |= after > limb.health;
        limb.health = after;
    }
    return changed;
}

bool canHealPlayer(const vitals::Vitals& v) {
    if (!alive(v)) return false;
    for (const auto& limb : v.limb)
        if (limb.health < 1.0f) return true;
    return false;
}

std::optional<LimbHit> rayPlayer(const Vec3& origin, const Vec3& direction,
    const Vec3& feet, float bodyYaw, float reach, float obstructionDistance) {
    return hitPlayer(origin, direction, feet, bodyYaw, reach, obstructionDistance, 0.0f);
}

std::optional<LimbHit> sweepPlayer(const Vec3& origin, const Vec3& direction,
    const Vec3& feet, float bodyYaw, float reach, float obstructionDistance,
    float radius) {
    return hitPlayer(origin, direction, feet, bodyYaw, reach, obstructionDistance, radius);
}

bool beginMelee(MeleeState& state, uint32_t sequence, uint32_t tick,
    const Actor& actor, const vitals::Vitals& body, Hand hand, uint8_t ownedItem, bool blocked) {
    if (!newer(sequence, state.lastSequence)) return false;
    state.lastSequence = sequence;
    auto def = weapon(ownedItem);
    if (!actor.active || !actor.id || actor.category != EntityCategory::Player || actor.team < 1 || actor.team > matchmap::kCombatTeams ||
        !handUsable(body, hand) || !def || blocked || state.pending || !reached(tick, state.readyAt)) return false;
    state.action = sequence;
    state.item = ownedItem;
    state.hand = hand;
    state.hitAt = tick + ticks(def->windup);
    state.readyAt = state.hitAt + ticks(def->recovery);
    state.pending = true;
    return true;
}

bool takeMeleeHit(MeleeState& state, uint32_t tick) {
    if (!state.pending || !reached(tick, state.hitAt)) return false;
    state.pending = false;
    return true;
}

void cancelMelee(MeleeState& state) { state.pending = false; }
} // namespace combat
