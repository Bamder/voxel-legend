#pragma once
#include "combat.hpp"
#include <cstdint>

// Pure, server-oriented Arcane rules. Networking, inventory mutation, world
// collision and rendering remain in their owning systems.
namespace arcane {

inline constexpr float kFireballSpeed = 14.0f;
inline constexpr float kFireballTtlSeconds = 5.0f;
inline constexpr float kFireballHitRadius = 0.42f;
inline constexpr float kFireballExplosionRadius = 3.0f;
inline constexpr float kFireballCenterDamage = 0.12f;
inline constexpr float kFireballEdgeScale = 0.40f;
inline constexpr float kFireballCooldownSeconds = 3.0f;
inline constexpr float kBurnDamage = 0.03f;
inline constexpr float kBurnSeconds = 4.0f;
inline constexpr float kFreezeSpeed = 18.0f;
inline constexpr float kFreezeTtlSeconds = 5.0f;
inline constexpr float kFreezeHitRadius = 0.30f;
inline constexpr float kFreezeDamage = 0.08f;
inline constexpr float kFreezeSeconds = 5.0f;
inline constexpr float kFreezeCooldownSeconds = 10.0f;
inline constexpr float kHealFraction = 0.25f;
inline constexpr float kHealCooldownSeconds = 12.0f;
inline constexpr float kHealRange = 8.0f;

enum class ProjectileKind : uint8_t { Fireball = 1, Freeze = 2 };

struct CastState {
    uint32_t lastSequence = 0;
    uint32_t readyAt = 0;
};

struct Projectile {
    uint32_t id = 0;
    uint32_t owner = 0;
    uint32_t action = 0;
    int team = 0;
    ProjectileKind kind = ProjectileKind::Fireball;
    Vec3 pos{};
    Vec3 vel{};
    uint32_t expireAt = 0;
};

// One entry per attacker on a victim. Re-ignition by the same attacker refreshes
// until but preserves the damage cadence, so packet spam cannot create stacks.
struct Burning {
    uint32_t source = 0;
    uint32_t action = 0;
    uint32_t until = 0;
    uint32_t nextDamage = 0;
    bool active = false;
};

// Frozen is one authoritative state per target. Any repeat hit refreshes the
// expiry and source metadata; it never creates another control layer.
struct Frozen {
    uint32_t source = 0;
    uint32_t action = 0;
    uint32_t until = 0;
    bool active = false;
};

bool reached(uint32_t now, uint32_t then);
uint32_t ticks(float seconds);
bool beginFireball(CastState& state, uint32_t sequence, uint32_t tick,
                   const combat::Actor& actor, const vitals::Vitals& body,
                   combat::Hand hand, uint8_t ownedItem, bool actionBlocked = false);
bool beginFreeze(CastState& state, uint32_t sequence, uint32_t tick,
                 const combat::Actor& actor, const vitals::Vitals& body,
                 combat::Hand hand, uint8_t ownedItem, bool actionBlocked = false);
bool beginHeal(CastState& state, uint32_t sequence, uint32_t tick,
               const combat::Actor& actor, const vitals::Vitals& body,
               combat::Hand hand, uint8_t ownedItem, bool actionBlocked = false);
Projectile makeFireball(uint32_t id, uint32_t owner, uint32_t action, int team,
                        Vec3 origin, Vec3 direction, uint32_t tick);
Projectile makeFreeze(uint32_t id, uint32_t owner, uint32_t action, int team,
                      Vec3 origin, Vec3 direction, uint32_t tick);
float explosionDamage(float distance);
void ignite(Burning& state, uint32_t source, uint32_t action, uint32_t tick);
bool takeBurnDamage(Burning& state, uint32_t tick);
bool burning(const Burning& state, uint32_t tick);
void freeze(Frozen& state, uint32_t source, uint32_t action, uint32_t tick);
bool frozen(const Frozen& state, uint32_t tick);

} // namespace arcane
