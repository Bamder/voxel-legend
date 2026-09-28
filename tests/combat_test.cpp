#include "../src/world/combat.hpp"
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
    check(!mobility(body, false, true, false, true).canMove, "frozen movement blocked");
    body.limb[vitals::Head].health = 0;
    check(!healPlayer(body) && body.limb[vitals::Head].health == 0, "heal cannot resurrect");

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
    std::cout << "combat: " << checks << " checks passed\n";
}
