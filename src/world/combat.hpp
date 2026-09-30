#pragma once
#include "blocks.hpp"
#include "vitals.hpp"
#include <cstdint>
#include <optional>

// Player-versus-player rules. This module owns no parallel HP and performs no
// network, rendering, inventory mutation or AI. The room server supplies
// validated actors. Trial guardians keep a separate integer pool.
namespace combat {

enum class EntityCategory : uint8_t { Player, Boss };
enum class DamageCategory : uint8_t { Physical, Fire, Frost };
enum class Hand : uint8_t { Left, Right };

struct DamageSource {
    uint32_t attacker = 0;
    uint32_t action = 0;
    uint8_t item = AIR;
    DamageCategory category = DamageCategory::Physical;
};

struct DamageSpec {
    float base = 0.0f; // fraction of a limb's maximum health
    float bossMultiplier = 1.0f;
    bool wholeBody = false;
};

struct Weapon {
    uint8_t item = AIR;
    DamageSpec damage;
    float reach = 3.0f;
    float windup = 0.5f;
    float recovery = 0.5f;
};

std::optional<Weapon> weapon(uint8_t item);

struct Actor {
    uint32_t id = 0;
    int team = 0;
    bool active = false; // alive, deployed and past opening protection
    EntityCategory category = EntityCategory::Player;
};

bool hostile(const Actor& attacker, const Actor& target);
bool handUsable(const vitals::Vitals& body, Hand hand);
bool slotUsable(const vitals::Vitals& body, int slot);

struct Mobility {
    bool canMove = false;
    bool canJump = false;
    bool canSprint = false;
    bool hopOnly = false;
    float speedMultiplier = 0.0f;
};

// The server must also zero existing horizontal velocity when canMove is false.
// One leg permits horizontal movement only while jumping/airborne, never swimming.
Mobility mobility(const vitals::Vitals& body, bool grounded, bool jump,
                  bool swimming, bool frozen = false);

struct DamageResult {
    bool applied = false;
    bool killed = false;
    int limb = -1;
    float amount = 0.0f; // total actual normalized limb health removed
};

// EntityCategory::Boss scales a limb fraction for a future room boss.
// It is not the trial guardian's hit-point pool.
float damageFor(const DamageSpec& spec, EntityCategory target);
DamageResult damagePlayer(vitals::Vitals& body, const DamageSource& source,
                          const DamageSpec& spec, int limb);
bool healPlayer(vitals::Vitals& body, float fraction = 0.25f);
bool canHealPlayer(const vitals::Vitals& body);

struct LimbHit { int limb = -1; float distance = 0.0f; };
// Fixed gameplay hit volumes, independent of editable clothing/hair/model assets.
// Directions are normalized internally; obstructionDistance is world-units.
std::optional<LimbHit> rayPlayer(const Vec3& origin, const Vec3& direction,
    const Vec3& feet, float bodyYaw, float reach, float obstructionDistance);
// Sweeps a sphere along the same authoritative ray. Projectile visuals have
// real width, so their hit volume must not collapse to an infinitely thin line.
std::optional<LimbHit> sweepPlayer(const Vec3& origin, const Vec3& direction,
    const Vec3& feet, float bodyYaw, float reach, float obstructionDistance,
    float radius);

struct MeleeState {
    uint32_t lastSequence = 0;
    uint32_t action = 0;
    uint32_t hitAt = 0;
    uint32_t readyAt = 0;
    uint8_t item = AIR;
    Hand hand = Hand::Right;
    bool pending = false;
};

// All timestamps are server ticks (20Hz), never supplied client timestamps.
// ownedItem must come from server inventory, not the client's held-item field.
// At impact the caller must revalidate life, hand, equipment, target and LOS;
// takeMeleeHit only consumes the scheduled impact, it does not authorize damage.
// Consume each new sequence even on rejection, so it cannot be replayed later.
bool beginMelee(MeleeState& state, uint32_t sequence, uint32_t tick,
    const Actor& actor, const vitals::Vitals& body, Hand hand,
    uint8_t ownedItem, bool actionBlocked = false);
bool takeMeleeHit(MeleeState& state, uint32_t tick);
void cancelMelee(MeleeState& state); // preserves cooldown and replay protection

} // namespace combat
