#include "guardian_ai.hpp"
#include <algorithm>
#include <cmath>

namespace guardian_ai {
namespace {

float nonneg(float value) {
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

uint32_t nextRng(Brain& brain) {
    uint32_t x = brain.rng ? brain.rng : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (!x) x = 1u;
    brain.rng = x;
    return x;
}

void setCast(Brain& brain, SkillSlot slot) {
    if (aggroSkill(slot)) return;
    if (brain.cast == SkillSlot::None) brain.cast = slot;
}

bool selectable(const Sense& sense) {
    return sense.id && sense.alive && std::isfinite(sense.distance) &&
           sense.distance >= 0.0f && sense.distance <= kLeash;
}

bool danger(float hp, float controlled, const Sense& target) {
    if (hp < kLowHp) return true;
    if (controlled > kControlBreak) return true;
    return std::isfinite(target.windup) && target.windup > kWindup;
}

bool empowerWanted(const Brain& brain, float hp, float combat) {
    if (brain.enraged) return false;
    int bucket = (int)(combat / kEmpowerEvery);
    if (combat > kEmpowerAt && !brain.timeEmpowered) return true;
    if (hp < kEmpowerHp && !brain.hpEmpowered) return true;
    return bucket > brain.empowerBucket && bucket > 0;
}

void markEmpower(Brain& brain, float hp, float combat) {
    if (combat > kEmpowerAt) brain.timeEmpowered = true;
    if (hp < kEmpowerHp) brain.hpEmpowered = true;
    int bucket = (int)(combat / kEmpowerEvery);
    if (bucket > brain.empowerBucket) brain.empowerBucket = bucket;
}

bool go(Brain& brain, State next, float hp, bool swing) {
    if (brain.state == next) return false;
    brain.state = next;
    brain.stateTime = 0.0f;
    brain.smokeUsed = false;
    brain.branched = false;
    brain.waitingSuppress = false;
    switch (next) {
    case State::Idle:
        brain.target = 0;
        break;
    case State::Charge:
        brain.cdCharge = kChargeCd;
        setCast(brain, SkillSlot::Charge);
        break;
    case State::Attack:
        brain.comboLeft = 3 + (int)(nextRng(brain) & 1u);
        brain.swing = kSwing;
        if (swing) {
            SkillSlot swingSkill = (nextRng(brain) % 5u == 0u) ? SkillSlot::SmallAoe : SkillSlot::Melee;
            setCast(brain, swingSkill);
            --brain.comboLeft;
        }
        break;
    case State::Evade:
        brain.cdEvade = kEvadeCd;
        brain.dodgeLeft = 2 + (int)(nextRng(brain) & 1u);
        --brain.dodgeLeft;
        brain.action = kDodgeStep;
        brain.evadeCycle = 0;
        setCast(brain, SkillSlot::Dodge);
        break;
    case State::Wander:
        brain.probe = 0.0f;
        brain.trap = 0.0f;
        brain.strafe = (nextRng(brain) & 1u) ? 1.0f : -1.0f;
        break;
    case State::Suppress:
        brain.cdSuppress = kSuppressCd;
        brain.action = kSuppressCast;
        brain.suppressHit = -1;
        brain.waitingSuppress = true;
        setCast(brain, SkillSlot::Suppress);
        break;
    case State::Empower:
        brain.selfBuff = hp < kEmpowerHp || (nextRng(brain) & 1u) == 0u;
        brain.empowerLeft = brain.selfBuff ? kSelfSeconds : kMarkSeconds;
        brain.cdEmpower = brain.empowerLeft;
        setCast(brain, brain.selfBuff ? SkillSlot::SelfBuff : SkillSlot::FragileMark);
        break;
    case State::Acquire:
        break;
    }
    return true;
}

const Sense* findTarget(const Sense* targets, int count, uint32_t id) {
    if (!id || !targets) return nullptr;
    for (int i = 0; i < count; ++i)
        if (targets[i].id == id && selectable(targets[i])) return &targets[i];
    return nullptr;
}

Order publish(const Brain& brain, float threatValue, bool group) {
    Order order;
    order.state = brain.state;
    order.target = brain.target;
    order.threat = threatValue;
    order.skill = brain.cast;
    order.groupSuppress = group;
    order.enraged = brain.enraged;
    order.permanentBuff = brain.enraged;
    order.controlImmune = brain.enraged || brain.state == State::Empower;
    order.invulnerable = brain.cast == SkillSlot::Dodge ? kIFrame : 0.0f;
    if (brain.enraged || (brain.state == State::Empower && brain.selfBuff)) {
        order.attackBonus = kAttackUp;
        order.speedBonus = kSpeedUp;
    }
    if (brain.state == State::Empower && !brain.selfBuff) order.fragile = kFragile;
    if (brain.state == State::Empower) order.buffSeconds = brain.empowerLeft > 0.0f ? brain.empowerLeft : 0.0f;
    else if (brain.enraged) order.buffSeconds = kSelfSeconds;
    switch (brain.state) {
    case State::Charge:
        order.move = Move::Approach;
        order.preferDistance = 2.0f;
        break;
    case State::Attack:
        order.move = Move::Approach;
        order.preferDistance = 2.0f;
        break;
    case State::Evade:
        order.move = Move::Retreat;
        order.preferDistance = kOrbit;
        break;
    case State::Wander:
        order.move = Move::Orbit;
        order.strafe = brain.strafe;
        order.preferDistance = kOrbit;
        break;
    default:
        order.move = Move::Hold;
        break;
    }
    return order;
}

bool tickCharge(Brain& brain, const Sense& target, float hp, float controlled) {
    if (!brain.enraged && controlled > 0.0f) return go(brain, State::Evade, hp, true);
    if (target.distance <= kMeleeRange) {
        setCast(brain, SkillSlot::Melee);
        return go(brain, State::Attack, hp, false);
    }
    return false;
}

bool tickAttack(Brain& brain, const Sense& target, float threatValue, float high, float hp, float step) {
    if (target.distance > kChargeRange) {
        if (threatValue > high && brain.cdCharge <= 0.0f) return go(brain, State::Charge, hp, true);
        return go(brain, State::Wander, hp, true);
    }
    brain.swing -= step;
    if (brain.comboLeft > 0 && brain.swing <= 0.0f) {
        SkillSlot swingSkill = (nextRng(brain) % 5u == 0u) ? SkillSlot::SmallAoe : SkillSlot::Melee;
        setCast(brain, swingSkill);
        --brain.comboLeft;
        brain.swing += kSwing;
        return false;
    }
    if (brain.comboLeft <= 0 && brain.swing <= 0.0f) {
        if (brain.cdSuppress <= 0.0f && (nextRng(brain) & 1u)) return go(brain, State::Suppress, hp, true);
        return go(brain, State::Wander, hp, true);
    }
    return false;
}

bool tickEvade(Brain& brain, const Sense& target, float threatValue, float high, float hp, float step) {
    brain.action -= step;
    if (brain.dodgeLeft > 0) {
        if (brain.action <= 0.0f) {
            setCast(brain, SkillSlot::Dodge);
            --brain.dodgeLeft;
            brain.action = kDodgeStep;
        }
        return false;
    }
    if (brain.action > 0.0f) return false;
    if (!brain.smokeUsed && target.distance < kChargeRange) {
        brain.smokeUsed = true;
        setCast(brain, SkillSlot::Smoke);
        brain.action = kDodgeStep;
        return false;
    }
    if (target.distance >= kChargeRange && threatValue <= high)
        return go(brain, State::Wander, hp, true);
    if (brain.evadeCycle < 1) {
        ++brain.evadeCycle;
        brain.dodgeLeft = 1;
        brain.action = kDodgeStep;
        setCast(brain, SkillSlot::Dodge);
        return false;
    }
    return go(brain, State::Wander, hp, true);
}

bool tickWander(Brain& brain, const Sense& target, float threatValue, float low, float high,
                float hp, float step, bool wanted) {
    if (threatValue > high && target.distance > kChargeRange && brain.cdCharge <= 0.0f)
        return go(brain, State::Charge, hp, true);
    if (target.distance < kMeleeRange) {
        if (!brain.enraged && brain.cdEvade <= 0.0f) return go(brain, State::Evade, hp, true);
        if (threatValue >= low) return go(brain, State::Attack, hp, true);
    }
    if (!brain.branched && brain.stateTime > 1.0f) {
        bool suppressOk = brain.cdSuppress <= 0.0f && (target.mobile || hp > kHealthy);
        bool empowerOk = wanted && brain.cdEmpower <= 0.0f;
        if (suppressOk || empowerOk) {
            brain.branched = true;
            bool suppress = suppressOk && (!empowerOk || (nextRng(brain) & 1u) == 0u);
            if (suppress) return go(brain, State::Suppress, hp, true);
            if (empowerOk) return go(brain, State::Empower, hp, true);
        }
    }
    brain.probe += step;
    brain.trap += step;
    if (brain.probe >= kProbe) {
        brain.probe = 0.0f;
        setCast(brain, SkillSlot::Ranged);
    } else if (brain.trap >= kTrap) {
        brain.trap = 0.0f;
        setCast(brain, SkillSlot::Trap);
    }
    return false;
}

bool tickSuppress(Brain& brain, float hp, float controlled, float step) {
    if (!brain.enraged && controlled > 0.0f) return go(brain, State::Evade, hp, true);
    if (brain.suppressHit == 1) {
        setCast(brain, SkillSlot::Followup);
        brain.suppressHit = -1;
        brain.waitingSuppress = false;
        return go(brain, State::Attack, hp, false);
    }
    brain.action -= step;
    if (brain.suppressHit == 0 || brain.action <= 0.0f) {
        brain.suppressHit = -1;
        brain.waitingSuppress = false;
        return go(brain, State::Wander, hp, true);
    }
    return false;
}

bool tickEmpower(Brain& brain, float hp, bool focused, float step) {
    if (brain.enraged) return false;
    brain.empowerLeft -= step;
    if (focused) return go(brain, State::Evade, hp, true);
    if (brain.empowerLeft <= 0.0f) return go(brain, State::Attack, hp, true);
    return false;
}

} // namespace

float threat(const Sense& sense) {
    float aggro = kAggroSkills ? 1.0f : 0.0f;
    float dps = nonneg(sense.damage10s) / kThreatWindow * kWeightDps;
    float burst = nonneg(sense.burst10s) * kWeightBurst;
    float control = nonneg(sense.controlSeconds10s) * kWeightControl;
    float heal = nonneg(sense.healing10s) * kWeightHeal * aggro;
    float taunt = (kAggroSkills && sense.taunt) ? kTauntBonus : 0.0f;
    float distance = 0.0f;
    if (std::isfinite(sense.distance) && sense.distance >= 0.0f && sense.distance <= kLeash)
        distance = ((kLeash - sense.distance) / kLeash) * kWeightDistance * 100.0f;
    float sum = dps + burst + control + heal + taunt + distance;
    return std::isfinite(sum) && sum > 0.0f ? sum : 0.0f;
}

void reportSuppress(Brain& brain, bool hit) {
    if (brain.state != State::Suppress || !brain.waitingSuppress || brain.suppressHit >= 0) return;
    brain.suppressHit = hit ? 1 : 0;
}

Order think(Brain& brain, const Body& body, const Sense* targets, int count, float dt) {
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    if (dt > 1.0f) dt = 1.0f;
    brain.cast = SkillSlot::None;

    float hp = body.hp;
    if (!std::isfinite(hp) || hp > 1.0f) hp = 1.0f;
    if (hp <= 0.0f) {
        uint32_t rng = brain.rng ? brain.rng : 1u;
        brain = {};
        brain.rng = rng;
        return {};
    }
    float combat = std::isfinite(body.combatSeconds) && body.combatSeconds > 0.0f ? body.combatSeconds : 0.0f;
    float controlled = std::isfinite(body.controlledSeconds) && body.controlledSeconds > 0.0f
                           ? body.controlledSeconds : 0.0f;

    brain.cdCharge = std::max(0.0f, brain.cdCharge - dt);
    brain.cdEvade = std::max(0.0f, brain.cdEvade - dt);
    brain.cdSuppress = std::max(0.0f, brain.cdSuppress - dt);
    brain.cdEmpower = std::max(0.0f, brain.cdEmpower - dt);
    brain.cdGroup = std::max(0.0f, brain.cdGroup - dt);
    brain.stateTime += dt;

    if (combat >= kEnrageAt) {
        brain.enraged = true;
        brain.selfBuff = true;
    }
    if (hp >= kEmpowerHp) brain.hpEmpowered = false;

    if (!targets || count <= 0) count = 0;
    if (count > kMaxTargets) count = kMaxTargets;

    const Sense* current = findTarget(targets, count, brain.target);
    if (brain.target && !current) brain.target = 0;

    float scale = brain.stateTime > kLinger ? kLingerScale : 1.0f;
    float low = kBandLow * scale;
    float high = kBandHigh * scale;
    float groupLine = kGroupThreat * scale;

    const Sense* best = nullptr;
    float bestThreat = -1.0f;
    int hot = 0;
    for (int i = 0; i < count; ++i) {
        if (!selectable(targets[i])) continue;
        float score = threat(targets[i]);
        if (score > groupLine) ++hot;
        if (!best || score > bestThreat || (score == bestThreat && targets[i].id < best->id)) {
            best = &targets[i];
            bestThreat = score;
        }
    }

    bool group = false;
    if (hot >= 2 && brain.cdGroup <= 0.0f) {
        group = true;
        brain.cdGroup = kGroupCd;
    }
    bool wanted = empowerWanted(brain, hp, combat);

    auto finish = [&]() {
        const Sense* locked = findTarget(targets, count, brain.target);
        return publish(brain, locked ? threat(*locked) : 0.0f, group);
    };

    if (!current || brain.state == State::Idle || brain.state == State::Acquire) {
        if (!best) {
            go(brain, State::Idle, hp, true);
            return finish();
        }
        brain.target = best->id;
        current = best;
        float score = threat(*current);
        if (!brain.enraged && brain.cdEvade <= 0.0f && danger(hp, controlled, *current)) {
            go(brain, State::Evade, hp, true);
            return finish();
        }
        if (wanted && brain.cdEmpower <= 0.0f) {
            markEmpower(brain, hp, combat);
            go(brain, State::Empower, hp, true);
            return finish();
        }
        if (score < low) go(brain, State::Wander, hp, true);
        else if (current->distance > kChargeRange && score > high && brain.cdCharge <= 0.0f)
            go(brain, State::Charge, hp, true);
        else if (current->distance < kMeleeRange) go(brain, State::Attack, hp, true);
        else if (brain.cdSuppress <= 0.0f && (current->mobile || hp > kHealthy))
            go(brain, State::Suppress, hp, true);
        else go(brain, State::Wander, hp, true);
        return finish();
    }

    if (brain.state == State::Empower) {
        tickEmpower(brain, hp, body.focused, dt);
        return finish();
    }
    if (!brain.enraged && brain.cdEvade <= 0.0f && danger(hp, controlled, *current)) {
        go(brain, State::Evade, hp, true);
        return finish();
    }
    if (wanted && brain.state != State::Evade && brain.cdEmpower <= 0.0f) {
        markEmpower(brain, hp, combat);
        go(brain, State::Empower, hp, true);
        return finish();
    }

    switch (brain.state) {
    case State::Charge: tickCharge(brain, *current, hp, controlled); break;
    case State::Attack: tickAttack(brain, *current, threat(*current), high, hp, dt); break;
    case State::Evade: tickEvade(brain, *current, threat(*current), high, hp, dt); break;
    case State::Wander: tickWander(brain, *current, threat(*current), low, high, hp, dt, wanted); break;
    case State::Suppress: tickSuppress(brain, hp, controlled, dt); break;
    default: break;
    }
    return finish();
}

} // namespace guardian_ai
