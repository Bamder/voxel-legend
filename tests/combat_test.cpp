#include "../src/world/combat.hpp"
#include "../src/world/arcane.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
int checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a-b) < 0.00001f; }
}

int main() {
    using namespace combat;
    const auto pick = *weapon(HAND_PICK), axe = *weapon(HAND_AXE);
    DamageSource source{1, 1, HAND_PICK, DamageCategory::Physical};
    check(!weapon(AIR), "non-weapons rejected");
    check(near(damageFor(pick.damage, EntityCategory::Player), .14f), "pick player damage");
    check(near(damageFor(pick.damage, EntityCategory::Boss), .049f), "pick boss adapter");
    check(near(damageFor(axe.damage, EntityCategory::Boss), .25f), "axe boss adapter");
    check(near(pick.reach, 4.5f) && near(axe.reach, 4.5f), "melee weapons use expanded reach");
    for (int part = 0; part < vitals::Count; ++part) {
        vitals::Vitals body;
        auto result = damagePlayer(body, source, pick.damage, part);
        check(result.applied && !result.killed && result.limb == part, "localized damage result");
        for (int other = 0; other < vitals::Count; ++other)
            check(near(body.limb[other].health, other == part ? .86f : 1.f), "only hit limb loses health");
        body.limb[part].health = .1f;
        result = damagePlayer(body, source, pick.damage, part);
        check(result.killed == vitals::isVital(part), "only vital limb loss kills");
        check(near(body.limb[part].health, 0), "damage clamps at zero");
    }
    vitals::Vitals body;
    body.limb[vitals::HandL].health = 0;
    for (int slot = 0; slot < 6; ++slot)
        check(slotUsable(body, slot) == (slot >= 3), "left hand disables only left slots");
    body.limb[vitals::HandR].health = 0;
    for (int slot = 0; slot < 6; ++slot) check(!slotUsable(body, slot), "both hands disabled");
    check(!slotUsable(body, -1) && !slotUsable(body, 6), "invalid hand slots rejected");
    body.limb[vitals::FootL].health = 0;
    auto m = mobility(body, true, false, false);
    check(!m.canMove && m.hopOnly && !m.canSprint, "single foot cannot walk");
    m = mobility(body, true, true, false);
    check(m.canMove && m.canJump && near(m.speedMultiplier, .4f), "single foot hopping");
    check(mobility(body, false, false, false).canMove, "single foot airborne movement");
    check(!mobility(body, false, true, true).canMove, "swimming cannot bypass single foot restriction");
    body.limb[vitals::FootR].health = 0;
    m = mobility(body, false, true, false);
    check(!m.canMove && !m.canJump, "both feet disabled");
    check(healPlayer(body), "living player can heal disabled limbs");
    check(near(body.limb[vitals::HandL].health, .25f) && near(body.limb[vitals::FootR].health, .25f), "heal adds quarter maximum to every limb");
    check(near(body.limb[vitals::Head].health, 1.f), "heal capped at maximum");
    check(slotUsable(body, 0), "healing restores hand use");
    check(canHealPlayer(body), "partially injured living player is a valid heal target");
    check(!mobility(body, false, true, false, true).canMove, "frozen movement blocked");
    body.limb[vitals::Head].health = 0;
    check(!canHealPlayer(body) && !healPlayer(body) && body.limb[vitals::Head].health == 0,
          "heal cannot target or resurrect the dead");
    body = {};
    check(!canHealPlayer(body), "full-health player is not a heal target");

    body = {};
    auto result = damagePlayer(body, source, {.12f, 1.f, true}, -1);
    check(result.applied && near(result.amount, .84f), "whole body damage total");
    for (auto part : body.limb) check(near(part.health, .88f), "whole body damage distribution");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    check(!damagePlayer(body, source, {nan, 1, false}, 0).applied, "NaN damage rejected");
    check(!damagePlayer(body, source, pick.damage, 99).applied, "invalid limb rejected");
    check(!healPlayer(body, nan), "NaN healing rejected");
    body.limb[0].health = nan;
    check(!handUsable(body, Hand::Left) && !healPlayer(body), "invalid health fails closed");
    body = {};
    Actor attacker{1, 1, true}, enemy{2, 2, true}, friendActor{3, 1, true};
    check(hostile(attacker, enemy), "opposing teams valid");
    check(!hostile(attacker, friendActor) && !hostile(attacker, attacker), "friendly and self damage blocked");
    enemy.active = false;
    check(!hostile(attacker, enemy), "protected target rejected");
    MeleeState state;
    check(beginMelee(state, 1, 0, attacker, body, Hand::Right, HAND_PICK), "start melee");
    check(state.hitAt == 11 && state.readyAt == 22, "pick windup and recovery ticks");
    check(!takeMeleeHit(state, 10) && takeMeleeHit(state, 11), "hit only after windup");
    check(!takeMeleeHit(state, 11), "one impact per swing");
    check(!beginMelee(state, 2, 12, attacker, body, Hand::Right, HAND_PICK), "recovery blocks new attack");
    check(!beginMelee(state, 2, 22, attacker, body, Hand::Right, HAND_PICK), "rejected sequence cannot replay");
    check(beginMelee(state, 3, 22, attacker, body, Hand::Right, HAND_AXE), "axe starts after cooldown");
    check(state.hitAt == 39 && state.readyAt == 56, "axe heavier timing");
    cancelMelee(state);
    check(!takeMeleeHit(state, 39), "cancelled attack cannot hit");
    check(!beginMelee(state, 4, 40, attacker, body, Hand::Right, HAND_AXE), "cancel preserves cooldown");
    body.limb[vitals::HandR].health = 0;
    check(!beginMelee(state, 5, 56, attacker, body, Hand::Right, HAND_AXE), "disabled hand cannot start");
    state = {}; state.lastSequence = 0xfffffffeu; state.readyAt = 0xfffffff0u;
    body = {};
    check(beginMelee(state, 0xffffffffu, 0xfffffff8u, attacker, body, Hand::Left, HAND_PICK), "tick wrap attack starts");
    check(!takeMeleeHit(state, 2) && takeMeleeHit(state, 3), "windup survives tick wrap");
    check(beginMelee(state, 1, 14, attacker, body, Hand::Left, HAND_PICK), "sequence wrap accepted");

    const Vec3 feet{0,0,0};
    auto hit = rayPlayer({0,1.6f,-2}, {0,0,1}, feet, 0, 3, 4);
    check(hit && hit->limb == vitals::Head && near(hit->distance, 1.82f), "head hit volume");
    hit = rayPlayer({-.28f,1.2f,-2}, {0,0,2}, feet, 0, 3, 4);
    check(hit && hit->limb == vitals::HandL, "left hand and normalized ray");
    hit = rayPlayer({2,1.2f,-.28f}, {-1,0,0}, feet, 1.57079632679f, 3, 4);
    check(hit && hit->limb == vitals::HandL, "rotated body preserves limb identity");
    check(!rayPlayer({0,1.6f,-2}, {0,0,1}, feet, 0, 3, 1), "wall blocks player hit");
    check(!rayPlayer({0,1.6f,-2}, {0,0,1}, feet, 0, 1, 4), "reach limit enforced");
    check(!rayPlayer({0,1.6f,-2}, {}, feet, 0, 3, 4), "zero direction rejected");
    check(!rayPlayer({nan,1.6f,-2}, {0,0,1}, feet, 0, 3, 4), "invalid ray rejected");
    check(!rayPlayer({.60f,1.3f,-2}, {0,0,1}, feet, 0, 3, 4),
          "thin interaction ray can miss beside the body");
    auto swept = sweepPlayer({.60f,1.3f,-2}, {0,0,1}, feet, 0, 3, 4,
                             arcane::kFireballHitRadius);
    check(swept && swept->distance < 2.0f, "fireball volume catches a visible grazing hit");
    check(!sweepPlayer({.60f,1.3f,-2}, {0,0,1}, feet, 0, 3, 4, -0.1f) &&
          !sweepPlayer({.60f,1.3f,-2}, {0,0,1}, feet, 0, 3, 4, nan),
          "invalid projectile radii rejected");

    arcane::CastState cast;
    body = {};
    check(arcane::beginFireball(cast, 1, 0, attacker, body, Hand::Right, ITEM_ARCANE_FIREBALL),
          "fireball cast accepted from authoritative item");
    check(cast.readyAt == 60, "fireball cooldown is three seconds");
    check(!arcane::beginFireball(cast, 1, 60, attacker, body, Hand::Right, ITEM_ARCANE_FIREBALL),
          "cast sequence cannot replay");
    check(!arcane::beginFireball(cast, 2, 59, attacker, body, Hand::Right, ITEM_ARCANE_FIREBALL),
          "server cooldown blocks early cast");
    check(!arcane::beginFireball(cast, 2, 60, attacker, body, Hand::Right, ITEM_ARCANE_FIREBALL),
          "rejected cast sequence stays consumed");
    check(arcane::beginFireball(cast, 3, 60, attacker, body, Hand::Left, ITEM_ARCANE_FIREBALL),
          "cast resumes after cooldown");
    body.limb[vitals::HandL].health = 0;
    check(!arcane::beginFireball(cast, 4, 120, attacker, body, Hand::Left, ITEM_ARCANE_FIREBALL),
          "sealed hand cannot cast");
    check(!arcane::beginFireball(cast, 5, 120, attacker, body, Hand::Right, ITEM_PRIM_FIRE),
          "ritual fire is not a fireball item");

    arcane::CastState freezeCast;
    body = {};
    check(arcane::beginFreeze(freezeCast, 1, 0, attacker, body, Hand::Right, ITEM_ARCANE_FREEZE),
          "freeze cast accepted from authoritative item");
    check(freezeCast.readyAt == 200, "freeze cooldown is ten seconds");
    check(!arcane::beginFreeze(freezeCast, 1, 200, attacker, body, Hand::Right, ITEM_ARCANE_FREEZE),
          "freeze sequence cannot replay");
    check(!arcane::beginFreeze(freezeCast, 2, 199, attacker, body, Hand::Right, ITEM_ARCANE_FREEZE),
          "freeze cooldown blocks early cast");
    check(arcane::beginFreeze(freezeCast, 3, 200, attacker, body, Hand::Left, ITEM_ARCANE_FREEZE),
          "freeze resumes after cooldown");
    body.limb[vitals::HandL].health = 0;
    check(!arcane::beginFreeze(freezeCast, 4, 400, attacker, body, Hand::Left, ITEM_ARCANE_FREEZE),
          "sealed hand cannot cast freeze");

    arcane::CastState healCast;
    body = {};
    check(arcane::beginHeal(healCast, 1, 0, attacker, body, Hand::Right, ITEM_ARCANE_HEAL),
          "heal cast accepted from authoritative item");
    check(healCast.readyAt == 240, "heal cooldown is twelve seconds");
    check(!arcane::beginHeal(healCast, 2, 239, attacker, body, Hand::Right, ITEM_ARCANE_HEAL),
          "heal cooldown blocks early cast");
    check(!arcane::beginHeal(healCast, 3, 240, attacker, body, Hand::Right, ITEM_ARCANE_FREEZE),
          "another arcane item cannot be consumed as heal");
    check(arcane::beginHeal(healCast, 4, 240, attacker, body, Hand::Left, ITEM_ARCANE_HEAL),
          "heal resumes after cooldown");

    auto projectile = arcane::makeFireball(7, 1, 3, 1, {1,2,3}, {0,0,2}, 10);
    check(projectile.id == 7 && near(projectile.vel.length(), arcane::kFireballSpeed) &&
          projectile.expireAt == 110, "authoritative fireball spawn normalizes direction and sets ttl");
    check(!arcane::makeFireball(0, 1, 1, 1, {}, {0,0,1}, 0).id,
          "invalid projectile identity rejected");
    auto freezeProjectile = arcane::makeFreeze(8, 1, 3, 1, {1,2,3}, {0,0,3}, 10);
    check(freezeProjectile.id == 8 && freezeProjectile.kind == arcane::ProjectileKind::Freeze &&
          near(freezeProjectile.vel.length(), arcane::kFreezeSpeed) && freezeProjectile.expireAt == 110,
          "authoritative freeze projectile normalizes direction and sets ttl");
    check(near(arcane::explosionDamage(0), .12f), "fireball center damage");
    check(near(arcane::explosionDamage(1.5f), .084f), "fireball linear falloff");
    check(near(arcane::explosionDamage(3), .048f), "fireball edge minimum damage");
    check(arcane::explosionDamage(3.01f) == 0 && arcane::explosionDamage(nan) == 0,
          "fireball radius and invalid distance rejected");

    arcane::Burning burning;
    arcane::ignite(burning, 1, 10, 0);
    check(arcane::burning(burning, 0) && burning.until == 80 && burning.nextDamage == 20,
          "burning begins for four seconds");
    check(!arcane::takeBurnDamage(burning, 19) && arcane::takeBurnDamage(burning, 20),
          "burning ticks once per second");
    arcane::ignite(burning, 1, 11, 25);
    check(burning.until == 105 && burning.nextDamage == 40,
          "same source refreshes duration without adding an early stack");
    int burnTicks = 1;
    for (uint32_t tick : {40u,60u,80u,100u}) if (arcane::takeBurnDamage(burning, tick)) ++burnTicks;
    check(burnTicks == 5 && !arcane::burning(burning, 105) && !arcane::takeBurnDamage(burning, 120),
          "refreshed burning has one cadence and expires naturally");

    arcane::Frozen frozen;
    arcane::freeze(frozen, 2, 9, 0);
    check(arcane::frozen(frozen, 0) && frozen.until == 100 && frozen.source == 2,
          "frozen begins for five seconds");
    arcane::freeze(frozen, 3, 10, 50);
    check(frozen.until == 150 && frozen.source == 3 && frozen.action == 10,
          "repeat freeze refreshes one authoritative state");
    check(arcane::frozen(frozen, 149) && !arcane::frozen(frozen, 150),
          "refreshed freeze expires naturally");
    std::cout << "combat: " << checks << " checks passed\n";
}
