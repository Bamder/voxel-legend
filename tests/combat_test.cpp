#include "../src/world/combat.hpp"
#include "../src/world/arcane.hpp"
#include "../src/world/guardian_ai.hpp"
#include "../src/world/guardian_fight.hpp"
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

float g_lip = 0.0f;
float lipStand(void*, float x, float y, float) {
    float top = (x >= 0.25f) ? g_lip : 0.0f;
    if (std::fabs(y - top) <= 1e-3f) return top;
    return std::numeric_limits<float>::quiet_NaN();
}
bool lipBlocked(void*, float x, float y, float) {
    float top = (x >= 0.25f) ? g_lip : 0.0f;
    return y + 1e-3f < top;
}

float gateStand(void*, float x, float y, float z) {
    bool wall = x >= 1.0f && x < 1.5f && std::fabs(z) < 1.5f;
    float top = wall ? 2.0f : 0.0f;
    if (std::fabs(y - top) <= 1e-3f) return top;
    return std::numeric_limits<float>::quiet_NaN();
}
bool gateBlocked(void*, float x, float y, float z) {
    bool wall = x >= 1.0f && x < 1.5f && std::fabs(z) < 1.5f;
    return y + 1e-3f < (wall ? 2.0f : 0.0f);
}
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

    auto projectile = arcane::makeFireball(7, 1, 3, 1, {1,2,3}, {0,0,2}, 10);
    check(projectile.id == 7 && near(projectile.vel.length(), arcane::kFireballSpeed) &&
          projectile.expireAt == 110, "authoritative fireball spawn normalizes direction and sets ttl");
    check(!arcane::makeFireball(0, 1, 1, 1, {}, {0,0,1}, 0).id,
          "invalid projectile identity rejected");
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
    using namespace guardian_ai;
    auto foe = [](uint32_t id, float dist, float damage, float burst = 0.0f, float control = 0.0f) {
        Sense s;
        s.id = id;
        s.alive = true;
        s.distance = dist;
        s.damage10s = damage;
        s.burst10s = burst;
        s.controlSeconds10s = control;
        return s;
    };
    Body live;
    Sense example = foe(1, 5.0f, 15000.0f, 8000.0f, 2.0f);
    float exampleThreat = threat(example);
    float distTerm = (15.0f / 20.0f) * 0.1f * 100.0f;
    check(near(exampleThreat, 600.0f + 1600.0f + 0.3f + distTerm), "worked threat example");
    check(exampleThreat > kBandHigh, "worked example is in the press band");
    example.healing10s = 50000.0f;
    example.taunt = true;
    check(threat(example) == exampleThreat, "heal and taunt slots add nothing");
    check(aggroSkill(SkillSlot::Taunt) && aggroSkill(SkillSlot::AggroTransfer) &&
          !aggroSkill(SkillSlot::Melee), "aggro skills stay vacant slots");
    check(threat(foe(1, 20.0f, 0.0f)) == 0.0f, "leash edge has no distance score");
    Sense bad = foe(1, 5.0f, std::numeric_limits<float>::quiet_NaN(), -4.0f, -1.0f);
    check(threat(bad) > 0.0f && threat(bad) < 20.0f, "invalid threat samples fail closed");

    Brain brain;
    check(think(brain, live, nullptr, 4, 0.05f).state == State::Idle, "no scan stays idle");
    Sense far = foe(1, 25.0f, 999999.0f);
    check(think(brain, live, &far, 1, 0.05f).state == State::Idle, "beyond leash is not a target");

    brain = {};
    Sense melee = foe(1, 3.0f, 50000.0f, 8000.0f);
    Order order = think(brain, live, &melee, 1, 0.05f);
    check(order.state == State::Attack && order.target == 1 && order.move == Move::Approach &&
          (order.skill == SkillSlot::Melee || order.skill == SkillSlot::SmallAoe) &&
          !aggroSkill(order.skill), "melee high threat opens on a swing");
    brain.cdSuppress = 100.0f;
    bool leftCombo = false;
    for (int i = 0; i < 20; ++i) {
        order = think(brain, live, &melee, 1, 0.25f);
        if (order.state != State::Attack) { leftCombo = true; break; }
    }
    check(leftCombo && order.state == State::Wander, "combo ends in wander while suppress is down");

    int aoe = 0;
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        Brain roll;
        roll.rng = seed;
        Order swing = think(roll, live, &melee, 1, 0.05f);
        if (swing.skill == SkillSlot::SmallAoe) ++aoe;
    }
    check(aoe >= 20 && aoe <= 60, "about one swing in five is a small aoe");

    brain = {};
    Sense charging = foe(1, 12.0f, 50000.0f, 8000.0f);
    order = think(brain, live, &charging, 1, 0.05f);
    check(order.state == State::Charge && order.skill == SkillSlot::Charge &&
          order.move == Move::Approach && !order.groupSuppress, "far high threat charges");
    charging.distance = 3.0f;
    order = think(brain, live, &charging, 1, 0.05f);
    check(order.state == State::Attack && order.skill == SkillSlot::Melee, "charge connects into a melee");
    charging.distance = 12.0f;
    order = think(brain, live, &charging, 1, 0.05f);
    check(order.state == State::Wander, "charge cooldown blocks an immediate recast");

    brain = {};
    order = think(brain, live, &charging, 1, 0.05f);
    check(order.state == State::Charge, "charge can start once the cooldown is clear");
    live.controlledSeconds = 0.2f;
    order = think(brain, live, &charging, 1, 0.05f);
    check(order.state == State::Evade && order.skill == SkillSlot::Dodge &&
          order.move == Move::Retreat && near(order.invulnerable, kIFrame),
          "control breaks a charge into a dodge");
    live.controlledSeconds = 0.0f;

    brain = {};
    live.hp = 0.20f;
    order = think(brain, live, &melee, 1, 0.05f);
    check(order.state == State::Evade && order.skill == SkillSlot::Dodge, "low health opens on evade");
    bool smoked = false;
    for (int i = 0; i < 15 && !smoked; ++i) {
        order = think(brain, live, &melee, 1, 0.40f);
        if (order.skill == SkillSlot::Smoke) smoked = true;
    }
    check(smoked, "a chase after the dodge asks for smoke");
    live.hp = 1.0f;

    brain = {};
    melee.windup = 0.60f;
    order = think(brain, live, &melee, 1, 0.05f);
    check(order.state == State::Evade, "a long windup is evaded");
    melee.windup = 0.0f;

    brain = {};
    live.hp = 0.40f;
    order = think(brain, live, &melee, 1, 0.05f);
    check(order.state == State::Empower && order.skill == SkillSlot::SelfBuff &&
          order.controlImmune && near(order.attackBonus, kAttackUp) &&
          near(order.speedBonus, kSpeedUp), "half health empowers attack and speed");
    live.hp = 1.0f;
    melee.windup = 1.0f;
    order = think(brain, live, &melee, 1, 1.0f);
    check(order.state == State::Empower && order.controlImmune && order.skill == SkillSlot::None,
          "empower ignores a windup");
    live.focused = true;
    order = think(brain, live, &melee, 1, 0.05f);
    check(order.state == State::Evade && !order.controlImmune, "focus fire ends empower early");
    live.focused = false;
    melee.windup = 0.0f;

    brain = {};
    live.hp = 0.20f;
    live.combatSeconds = 300.0f;
    melee.windup = 1.0f;
    order = think(brain, live, &melee, 1, 0.05f);
    check(order.enraged && order.permanentBuff && order.controlImmune &&
          order.state == State::Attack && near(order.attackBonus, kAttackUp),
          "enrage keeps the buff and stops dodging");
    order = think(brain, live, &melee, 1, 0.05f);
    check(order.state != State::Evade && order.enraged, "enrage stays on the target");
    live.hp = 1.0f;
    live.combatSeconds = 0.0f;
    melee.windup = 0.0f;

    brain = {};
    Sense pacing = foe(1, 10.0f, 35000.0f);
    order = think(brain, {0.60f, 1.0f, 0.0f, false}, &pacing, 1, 0.05f);
    check(order.state == State::Wander && !order.groupSuppress, "mid threat paces instead of charging");
    for (int i = 0; i < 5; ++i)
        order = think(brain, {0.60f, 1.0f, 0.0f, false}, &pacing, 1, 1.0f);
    check(order.state == State::Wander, "five seconds of pacing does not cross the band yet");
    order = think(brain, {0.60f, 1.0f, 0.0f, false}, &pacing, 1, 0.05f);
    check(order.state == State::Charge, "lingering lowers the charge band");

    brain = {};
    Sense poke = foe(1, 10.0f, 2500.0f);
    order = think(brain, {0.60f, 0.0f, 0.0f, false}, &poke, 1, 0.05f);
    check(order.state == State::Wander, "low threat wanders");
    bool poked = false;
    for (int i = 0; i < 10 && !poked; ++i) {
        order = think(brain, {0.60f, 0.0f, 0.0f, false}, &poke, 1, 0.50f);
        if (order.skill == SkillSlot::Ranged) poked = true;
        check(order.state == State::Wander, "a probe does not leave the orbit");
    }
    check(poked && (order.strafe == 1.0f || order.strafe == -1.0f), "orbit tries a ranged probe");

    brain = {};
    Sense grip = foe(1, 6.0f, 20000.0f);
    order = think(brain, live, &grip, 1, 0.05f);
    check(order.state == State::Suppress && order.skill == SkillSlot::Suppress, "healthy mid range suppresses");
    reportSuppress(brain, true);
    order = think(brain, live, &grip, 1, 0.05f);
    check(order.state == State::Attack && order.skill == SkillSlot::Followup, "a landed suppress leads a follow-up");

    brain = {};
    think(brain, live, &grip, 1, 0.05f);
    reportSuppress(brain, false);
    order = think(brain, live, &grip, 1, 0.05f);
    check(order.state == State::Wander, "a missed suppress returns to pacing");

    brain = {};
    think(brain, live, &grip, 1, 0.05f);
    order = think(brain, live, &grip, 1, 0.50f);
    check(order.state == State::Suppress, "suppress holds through its cast");
    order = think(brain, live, &grip, 1, 0.50f);
    check(order.state == State::Suppress, "suppress is still casting");
    order = think(brain, live, &grip, 1, 0.50f);
    check(order.state == State::Wander, "an unanswered suppress times out");

    brain = {};
    think(brain, live, &grip, 1, 0.05f);
    live.controlledSeconds = 0.20f;
    order = think(brain, live, &grip, 1, 0.05f);
    check(order.state == State::Evade, "control breaks suppress");
    live.controlledSeconds = 0.0f;

    brain = {};
    Sense pair[2] = { foe(1, 5.0f, 15000.0f, 8000.0f), foe(2, 6.0f, 80000.0f) };
    order = think(brain, live, pair, 2, 0.05f);
    check(order.target == 2 && order.groupSuppress, "highest threat is locked and a pair asks for group control");
    pair[0].damage10s = 900000.0f;
    order = think(brain, live, pair, 2, 0.05f);
    check(order.target == 2 && !order.groupSuppress, "the lock holds and group control does not repeat");
    pair[1].alive = false;
    order = think(brain, live, pair, 2, 0.05f);
    check(order.target == 1 && order.state != State::Idle, "a dead target is replaced");
    pair[0].alive = false;
    order = think(brain, live, pair, 2, 0.05f);
    check(order.state == State::Idle && order.target == 0, "no living target returns to idle");

    brain = {};
    Sense decoy[2] = { foe(1, 4.0f, 10000.0f), foe(2, 40.0f, 999999.0f) };
    decoy[0].healing10s = 1000000.0f;
    decoy[0].taunt = true;
    order = think(brain, live, decoy, 2, 0.05f);
    check(order.target == 1, "out of range damage and vacant aggro do not steal the target");

    live.hp = 0.0f;
    order = think(brain, live, &melee, 1, 0.05f);
    check(order.state == State::Idle && order.target == 0, "a dead guardian drops the fight");

    guardian_fight::reset();
    guardian_fight::Home home;
    home.relic = 0;
    home.feet = {};
    home.hp = 320.0f;
    home.maxHp = 320.0f;
    guardian_fight::Rival rival;
    rival.id = 7;
    rival.feet = { 12.0f, 0.0f, 0.0f };
    rival.alive = true;
    rival.active = true;
    guardian_fight::noteDamage(0, 7, 400);
    guardian_fight::Blow blows[4];
    int blowCount = guardian_fight::tick(&home, 1, &rival, 1, 0.5f, {}, blows, 4);
    guardian_fight::Pose pose;
    int poseCount = guardian_fight::poses(&pose, 1);
    check(blowCount == 0 && poseCount == 1 && pose.feet.x > 1.0f, "a far threat is charged toward");
    guardian_fight::reset();
    rival.feet = { 3.0f, 0.0f, 0.0f };
    guardian_fight::noteDamage(0, 7, 400);
    blowCount = guardian_fight::tick(&home, 1, &rival, 1, 0.05f, {}, blows, 4);
    check(blowCount >= 1 && blows[0].target == 7 && blows[0].amount > 0.03f && blows[0].relic == 0,
          "melee range connects a blow");
    guardian_fight::reset();
    home.hp = 64.0f;
    blowCount = guardian_fight::tick(&home, 1, &rival, 1, 0.05f, {}, blows, 4);
    check(blowCount == 0 && !guardian_fight::vulnerable(0), "low health dodges with an invulnerable window");
    check(guardian_fight::vulnerable(3), "an untouched guardian can still be hit");

    guardian_fight::Ground climb{ nullptr, lipStand, lipBlocked };
    guardian_fight::reset();
    home.hp = home.maxHp = 320.0f;
    home.feet = {};
    rival.feet = { 12.0f, 0.0f, 0.0f };
    rival.alive = rival.active = true;
    g_lip = 1.0f;
    guardian_fight::noteDamage(0, 7, 400);
    guardian_fight::tick(&home, 1, &rival, 1, 0.05f, climb, blows, 4);
    poseCount = guardian_fight::poses(&pose, 1);
    check(poseCount == 1 && pose.feet.x > 0.25f && std::fabs(pose.feet.y - 1.0f) < 1e-3f,
          "a two-block lip is stepped onto");
    guardian_fight::reset();
    g_lip = 1.5f;
    guardian_fight::noteDamage(0, 7, 400);
    guardian_fight::tick(&home, 1, &rival, 1, 0.05f, climb, blows, 4);
    poseCount = guardian_fight::poses(&pose, 1);
    check(poseCount == 1 && std::fabs(pose.feet.x) < 1e-3f && std::fabs(pose.feet.y) < 1e-3f,
          "a three-block wall is not climbed");

    guardian_fight::Ground gate{ nullptr, gateStand, gateBlocked };
    guardian_fight::reset();
    home.hp = home.maxHp = 320.0f;
    home.feet = {};
    rival.feet = { 12.0f, 0.0f, 0.0f };
    rival.alive = rival.active = true;
    guardian_fight::noteDamage(0, 7, 400);
    for (int step = 0; step < 30; ++step)
        guardian_fight::tick(&home, 1, &rival, 1, 0.05f, gate, blows, 4);
    poseCount = guardian_fight::poses(&pose, 1);
    check(poseCount == 1 && pose.feet.x > 1.6f && std::fabs(pose.feet.y) < 0.2f,
          "chase routes around a wall with A*");

    std::cout << "combat: " << checks << " checks passed\n";
}
