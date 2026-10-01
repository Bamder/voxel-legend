#pragma once
#include <cstdint>

// Guardian decision brain. The caller supplies the last-10-second threat
// samples and keeps Brain between ticks. think() does not move, damage,
// spawn, or touch the network.
//
// Taunt and aggro-transfer have no skills yet: those threat terms stay in
// the formula but contribute nothing, and think() never emits them.
// Other skills are requests only; combat does not execute them here.
//
// Threat uses the worked weights (about 2208 for the 15000/8000/2s/5m
// example). Bands are 500 and 1500. Distance gates are 5m, 8m and 20m.
// A charge or a closing attack commits from the low band. Evade stays shut
// until (maxHp - hp) / (maxHp * combatSeconds) reaches kEvadeRate.
namespace guardian_ai {

inline constexpr float kThreatWindow = 10.0f;
inline constexpr float kWeightDps = 0.40f;
inline constexpr float kWeightBurst = 0.20f;
inline constexpr float kWeightControl = 0.15f;
inline constexpr float kWeightHeal = 0.10f;      // vacant: no aggro transfer
inline constexpr float kWeightDistance = 0.10f;
inline constexpr float kTauntBonus = 20.0f;      // vacant: no taunt skill
inline constexpr bool kAggroSkills = false;

inline constexpr float kLeash = 20.0f;
inline constexpr float kMeleeRange = 5.0f;
inline constexpr float kChargeRange = 8.0f;
inline constexpr float kOrbit = 10.0f;
inline constexpr float kBandLow = 500.0f;
inline constexpr float kBandHigh = 1500.0f;
inline constexpr float kGroupThreat = 1200.0f;
inline constexpr float kLinger = 5.0f;
inline constexpr float kLingerScale = 0.90f;

inline constexpr float kLowHp = 0.30f;
inline constexpr float kEmpowerHp = 0.50f;
inline constexpr float kHealthy = 0.70f;
inline constexpr float kWindup = 0.50f;
inline constexpr float kControlBreak = 1.50f;
inline constexpr float kEmpowerAt = 30.0f;
inline constexpr float kEmpowerEvery = 90.0f;
inline constexpr float kEnrageAt = 300.0f;
inline constexpr float kSelfSeconds = 15.0f;
inline constexpr float kMarkSeconds = 10.0f;
inline constexpr float kAttackUp = 0.30f;
inline constexpr float kSpeedUp = 0.15f;
inline constexpr float kFragile = 0.25f;
inline constexpr float kIFrame = 0.30f;
inline constexpr float kSwing = 0.50f;
inline constexpr float kProbe = 3.50f;
inline constexpr float kTrap = 11.0f;
inline constexpr float kChargeCd = 8.0f;
inline constexpr float kEvadeCd = 4.0f;
// Fraction of max health lost per second of the fight. Body.hp is already
// that fraction, so the rate is (1 - hp) / combatSeconds.
inline constexpr float kEvadeRate = 0.02f;
inline constexpr float kSuppressCd = 12.0f;
inline constexpr float kGroupCd = 20.0f;
inline constexpr float kDodgeStep = 0.35f;
inline constexpr float kSuppressCast = 1.20f;
inline constexpr int kMaxTargets = 32;

enum class State : uint8_t {
    Idle,
    Acquire, // resolved inside think(); callers observe the destination
    Charge,
    Attack,
    Evade,
    Wander,
    Suppress,
    Empower,
};

enum class SkillSlot : uint8_t {
    None,
    Melee,
    SmallAoe,
    Charge,
    Dodge,
    Smoke,
    Ranged,
    Trap,
    Suppress,
    Followup,
    SelfBuff,
    FragileMark,
    Taunt,          // vacant
    AggroTransfer,  // vacant
};

enum class Move : uint8_t { Hold, Approach, Retreat, Orbit };

constexpr bool aggroSkill(SkillSlot slot) {
    return slot == SkillSlot::Taunt || slot == SkillSlot::AggroTransfer;
}

// One living candidate. Distances are metres. Damage, burst, control and
// healing cover the last kThreatWindow seconds. healing and taunt are stored
// for later; they do not affect threat while kAggroSkills is false.
struct Sense {
    uint32_t id = 0;
    bool alive = false;
    float distance = 0.0f;
    float damage10s = 0.0f;
    float burst10s = 0.0f;
    float controlSeconds10s = 0.0f;
    float healing10s = 0.0f;
    bool taunt = false;
    bool mobile = false;
    float windup = 0.0f;
};

struct Body {
    float hp = 1.0f;                 // 0..1, zero is dead
    float combatSeconds = 0.0f;
    float controlledSeconds = 0.0f;  // continuous control on the guardian
    bool focused = false;            // focused fire; cuts Empower short
};

struct Brain {
    State state = State::Idle;
    uint32_t target = 0;
    float stateTime = 0.0f;
    float cdCharge = 0.0f;
    float cdEvade = 0.0f;
    float cdSuppress = 0.0f;
    float cdEmpower = 0.0f;
    float cdGroup = 0.0f;
    float empowerLeft = 0.0f;
    float swing = 0.0f;
    float action = 0.0f;
    float probe = 0.0f;
    float trap = 0.0f;
    float strafe = 1.0f;
    int comboLeft = 0;
    int dodgeLeft = 0;
    int evadeCycle = 0;
    int suppressHit = -1; // -1 unknown, 0 miss, 1 hit
    int empowerBucket = 0;
    bool timeEmpowered = false;
    bool hpEmpowered = false;
    bool enraged = false;
    bool selfBuff = false;
    bool smokeUsed = false;
    bool branched = false;
    bool waitingSuppress = false;
    uint32_t rng = 1;
    SkillSlot cast = SkillSlot::None;
};

struct Order {
    State state = State::Idle;
    uint32_t target = 0;
    float threat = 0.0f;
    SkillSlot skill = SkillSlot::None;
    Move move = Move::Hold;
    float strafe = 0.0f;
    float preferDistance = 0.0f;
    bool groupSuppress = false; // extra control when two threats are both hot
    bool controlImmune = false;
    bool enraged = false;
    bool permanentBuff = false;
    float attackBonus = 0.0f;
    float speedBonus = 0.0f;
    float fragile = 0.0f;
    float buffSeconds = 0.0f;
    float invulnerable = 0.0f;
};

float threat(const Sense& sense);

// Hit or miss for the suppress request currently on the brain. Ignored unless
// that cast is still waiting.
void reportSuppress(Brain& brain, bool hit);

// dt is seconds, clamped to [0, 1]. At most one state change per call.
Order think(Brain& brain, const Body& body, const Sense* targets, int count, float dt);

} // namespace guardian_ai
