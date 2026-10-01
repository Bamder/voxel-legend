#pragma once
#include "../core/math.hpp"
#include <algorithm>
#include <string>

// Survival body stats. Default (non-privilege) mode drains these over time.
// Display regions: head / chest / core (腰腹核心) / L-R hands / L-R feet.
// Death: head / chest (上胸) / core (腰腹核心) health reaching 0 (drawn black).
namespace vitals {

enum Limb : int {
    Head = 0,  // 头 — vital
    Chest,     // 上胸 — vital
    Core,      // 腰腹核心 (胯 + 腹) — vital
    HandL,     // 左手
    HandR,     // 右手
    FootL,     // 左脚
    FootR,     // 右脚
    Count
};

inline constexpr int kStaminaPips = 5;
inline constexpr float kDeadEps = 0.0005f;

struct LimbStat {
    float health = 1.0f;
    float stamina = 1.0f;
};

struct Vitals {
    LimbStat limb[Count];
    float hunger = 1.0f;
    float thirst = 1.0f;
    float cardio = 1.0f;
    float inspire = 1.0f;
};

struct TickInput {
    bool moving = false;
    bool sprint = false;      // actually running (not flying)
    bool jumpImpulse = false; // took off this step
    bool landed = false;      // became grounded this step
    bool swim = false;
    bool mining = false;
    bool flying = false;
    float landImpact = 0.0f;
    float borderDrain = 0.0f; // cardiopulmonary + inspiration loss per second
};

// Runtime overuse / combo state (not saved).
struct Fatigue {
    bool emptied[Count]{};     // this limb hit 0 stamina; penalized until full
    float recoverDelay[Count]{}; // rest required before empty-recovery starts
    int jumpChain = 0;         // 1 = first jump, 2+ = consecutive
    float jumpChainTimer = 0.0f;
    bool jumpChainArmed = false;
};

// Defaults. Live values are rates() — overlaid by assets/data packs.
inline constexpr float kSprintStaminaPerSec = 0.0022f; // both feet; half of 0.0044 (2x endurance)
inline constexpr float kJumpStaminaBurst = 0.0010f;    // half of 0.0020 (2x endurance)
inline constexpr float kSwimStaminaPerSec = 0.0009f;   // half of 0.0018 (2x endurance)
inline constexpr float kMineStaminaPerSec = 0.09f;     // half of 0.18 (2x endurance)
inline constexpr float kHungerThirstDrainMul = 0.2f;   // hunger / thirst fall at 20% of the base curve
inline constexpr float kStamRegenFast = 0.52f;         // stopped before empty
inline constexpr float kStamRegenSlow = 0.075f;        // recovering from empty
inline constexpr float kEmptyRecoverDelay = 3.5f;      // seconds of rest before slow regen
inline constexpr float kRunCardioPerSec = 0.016f;      // slow drain while running
inline constexpr float kJumpChainWindow = 0.40f;       // land → next jump
inline constexpr float kJumpCardioBase = 0.022f;       // 2nd jump
inline constexpr float kJumpCardioGrowth = 1.55f;      // each next jump costs more
inline constexpr float kJumpCardioCap = 0.10f;         // per-jump deduction threshold
inline constexpr float kEmptySpeedMul = 0.55f;         // walk/run while a foot is emptied
inline constexpr float kEmptyJumpMul = 0.60f;

struct Rates {
    float sprintStaminaPerSec = kSprintStaminaPerSec;
    float jumpStaminaBurst = kJumpStaminaBurst;
    float swimStaminaPerSec = kSwimStaminaPerSec;
    float mineStaminaPerSec = kMineStaminaPerSec;
    float stamRegenFast = kStamRegenFast;
    float stamRegenSlow = kStamRegenSlow;
    float emptyRecoverDelay = kEmptyRecoverDelay;
    float runCardioPerSec = kRunCardioPerSec;
    float jumpChainWindow = kJumpChainWindow;
    float jumpCardioBase = kJumpCardioBase;
    float jumpCardioGrowth = kJumpCardioGrowth;
    float jumpCardioCap = kJumpCardioCap;
    float emptySpeedMul = kEmptySpeedMul;
    float emptyJumpMul = kEmptyJumpMul;
    float mineChargeSec = 0.50f;    // unused by mining; timings live on tools / _hand
    float mineCooldownSec = 0.50f;
};

inline Rates& rates() {
    static Rates r;
    return r;
}

inline void resetRates() { rates() = Rates{}; }

inline void clamp01(float& v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
}

inline void reset(Vitals& v) {
    v = Vitals{};
}

inline void sanitize(Vitals& v) {
    for (int i = 0; i < Count; i++) {
        if (!(v.limb[i].health >= 0.0f) || v.limb[i].health > 1.0f) v.limb[i].health = 1.0f;
        if (!(v.limb[i].stamina >= 0.0f) || v.limb[i].stamina > 1.0f) v.limb[i].stamina = 1.0f;
        clamp01(v.limb[i].health);
        clamp01(v.limb[i].stamina);
    }
    auto fix = [](float x) {
        if (!(x >= 0.0f) || x > 1.0f) return 1.0f;
        return x;
    };
    v.hunger = fix(v.hunger);
    v.thirst = fix(v.thirst);
    v.cardio = fix(v.cardio);
    v.inspire = fix(v.inspire);
}

inline float minOf(float a, float b) { return a < b ? a : b; }
inline float minOf(float a, float b, float c) { return minOf(a, minOf(b, c)); }
inline float minOf(float a, float b, float c, float d) { return minOf(minOf(a, b), minOf(c, d)); }

inline bool isVital(int limb) {
    return limb == Head || limb == Chest || limb == Core;
}

inline bool isDead(const Vitals& v) {
    return v.limb[Head].health <= kDeadEps
        || v.limb[Chest].health <= kDeadEps
        || v.limb[Core].health <= kDeadEps;
}

inline float meanLimbHealth(const Vitals& v) {
    float s = 0.0f;
    for (int i = 0; i < Count; i++) s += v.limb[i].health;
    return s / (float)Count;
}

inline float meanLimbStamina(const Vitals& v) {
    float s = 0.0f;
    for (int i = 0; i < Count; i++) s += v.limb[i].stamina;
    return s / (float)Count;
}

inline void hurt(Vitals& v, int limb, float amount) {
    if (limb < 0 || limb >= Count || amount <= 0.0f) return;
    v.limb[limb].health -= amount;
    clamp01(v.limb[limb].health);
}

inline void resetFatigue(Fatigue& f) { f = Fatigue{}; }

inline void drainStamina(Vitals& v, int limb, float amount) {
    if (limb < 0 || limb >= Count || amount <= 0.0f) return;
    v.limb[limb].stamina -= amount;
    clamp01(v.limb[limb].stamina);
}

inline void drainStamina(Vitals& v, Fatigue& f, int limb, float amount) {
    if (limb < 0 || limb >= Count || amount <= 0.0f) return;
    float before = v.limb[limb].stamina;
    v.limb[limb].stamina -= amount;
    clamp01(v.limb[limb].stamina);
    if (before > kDeadEps && v.limb[limb].stamina <= kDeadEps) {
        f.emptied[limb] = true;
        f.recoverDelay[limb] = rates().emptyRecoverDelay;
    }
}

inline bool limbEmptied(const Fatigue& f, int limb) {
    return limb >= 0 && limb < Count && f.emptied[limb];
}

inline void regenStamina(Vitals& v, int limb, float amount) {
    if (limb < 0 || limb >= Count || amount <= 0.0f) return;
    v.limb[limb].stamina += amount;
    clamp01(v.limb[limb].stamina);
}

// Silhouette fill: healthy is a mute paper-gray, not green. Black = fully disabled.
inline Vec3 healthColor(float h) {
    if (h <= kDeadEps) return { 0.04f, 0.04f, 0.045f };
    auto lerp3 = [](Vec3 a, Vec3 b, float t) {
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return Vec3{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    };
    const Vec3 black{ 0.06f, 0.06f, 0.07f };
    const Vec3 red{ 0.72f, 0.16f, 0.12f };
    const Vec3 amber{ 0.78f, 0.58f, 0.28f };
    const Vec3 paper{ 0.58f, 0.57f, 0.55f };
    if (h < 0.33f) return lerp3(black, red, h / 0.33f);
    if (h < 0.66f) return lerp3(red, amber, (h - 0.33f) / 0.33f);
    return lerp3(amber, paper, (h - 0.66f) / 0.34f);
}

inline Vec3 outlineColor() { return { 0.06f, 0.06f, 0.07f }; }

inline int limbFromName(const std::string& n) {
    if (n.empty()) return -1;
    if (n == "head" || n.starts_with("eye") || n.starts_with("eyelid") ||
        n.starts_with("mouth") || n.starts_with("hair") || n.starts_with("card"))
        return Head;
    if (n == "neck" || n == "chest" || n.starts_with("leaf")) return Chest;
    if (n == "abs" || n == "hip") return Core;
    if (n.starts_with("arm_l")) return HandL;
    if (n.starts_with("arm_r")) return HandR;
    if (n.starts_with("leg_l")) return FootL;
    if (n.starts_with("leg_r")) return FootR;
    return -1;
}

inline const char* limbLabel(int limb) {
    switch (limb) {
        case Head: return "头";
        case Chest: return "上胸";
        case Core: return "腰腹核心";
        case HandL: return "左手";
        case HandR: return "右手";
        case FootL: return "左脚";
        case FootR: return "右脚";
        default: return "";
    }
}

inline bool showsStaminaSlot(int limb) {
    return limb >= 0 && limb < Count;
}

inline float leftLegHealth(const Vitals& v) { return v.limb[FootL].health; }
inline float rightLegHealth(const Vitals& v) { return v.limb[FootR].health; }
inline float leftArmHealth(const Vitals& v) { return v.limb[HandL].health; }
inline float rightArmHealth(const Vitals& v) { return v.limb[HandR].health; }

inline bool canJump(const Vitals& v) {
    return leftLegHealth(v) > kDeadEps || rightLegHealth(v) > kDeadEps;
}

inline bool canSprint(const Vitals& v) {
    if (v.cardio < 0.12f) return false;
    if (leftLegHealth(v) <= kDeadEps && rightLegHealth(v) <= kDeadEps) return false;
    if (v.hunger < 0.05f) return false;
    return true;
}

inline float moveSpeedMul(const Vitals& v, const Fatigue& f) {
    float lh = leftLegHealth(v);
    float rh = rightLegHealth(v);
    float h = 0.5f * (lh + rh);
    float mul = 0.22f + 0.78f * h;
    if (lh <= kDeadEps && rh <= kDeadEps) mul = 0.12f;
    else if (lh <= kDeadEps || rh <= kDeadEps) mul *= 0.55f;
    // Overuse: emptied feet stay penalized until the bar is full again.
    if (limbEmptied(f, FootL) || limbEmptied(f, FootR)
        || minOf(v.limb[FootL].stamina, v.limb[FootR].stamina) <= kDeadEps)
        mul *= rates().emptySpeedMul;
    if (v.cardio < 0.12f) mul *= 0.55f + v.cardio / 0.12f * 0.45f;
    if (v.hunger < 0.08f) mul *= 0.70f;
    return mul;
}

inline float jumpHeightMul(const Vitals& v, const Fatigue& f) {
    if (!canJump(v)) return 0.0f;
    float h = 0.5f * (leftLegHealth(v) + rightLegHealth(v));
    float mul = 0.45f + 0.55f * h;
    if (limbEmptied(f, FootL) || limbEmptied(f, FootR)
        || minOf(v.limb[FootL].stamina, v.limb[FootR].stamina) <= kDeadEps)
        mul *= rates().emptyJumpMul;
    return mul;
}

inline void tick(Vitals& v, Fatigue& f, const TickInput& in, float dt) {
    if (dt <= 0.0f) return;

    float work = 0.0f;
    if (in.moving) work += 0.4f;
    if (in.sprint) work += 1.2f;
    if (in.jumpImpulse) work += 0.8f;
    if (in.swim) work += 0.7f;
    if (in.mining) work += 0.6f;
    v.hunger -= dt * (1.0f / 900.0f) * (0.55f + work) * kHungerThirstDrainMul;
    v.thirst -= dt * (1.0f / 620.0f) * (0.70f + work * 1.15f) * kHungerThirstDrainMul;

    // Running slowly taxes cardiopulmonary function.
    if (in.sprint && !in.flying)
        v.cardio -= rates().runCardioPerSec * dt;

    // Consecutive jumps: first jump is free; each follow-up inside the
    // landing window costs more than the last, capped per jump.
    if (in.jumpImpulse && !in.flying && !in.swim) {
        if (f.jumpChainTimer > 0.0f && f.jumpChain >= 1) {
            f.jumpChain += 1;
            float cost = rates().jumpCardioBase;
            for (int n = 2; n < f.jumpChain; n++) {
                cost *= rates().jumpCardioGrowth;
                if (cost >= rates().jumpCardioCap) { cost = rates().jumpCardioCap; break; }
            }
            if (cost > rates().jumpCardioCap) cost = rates().jumpCardioCap;
            v.cardio -= cost;
        } else {
            f.jumpChain = 1;
        }
        f.jumpChainArmed = true;
        f.jumpChainTimer = 0.0f;
    }
    if (in.landed && f.jumpChainArmed) {
        f.jumpChainTimer = rates().jumpChainWindow;
        f.jumpChainArmed = false;
    }
    if (f.jumpChainTimer > 0.0f) {
        f.jumpChainTimer -= dt;
        if (f.jumpChainTimer <= 0.0f) {
            f.jumpChainTimer = 0.0f;
            if (!f.jumpChainArmed) f.jumpChain = 0;
        }
    }

    if (in.borderDrain <= 0.0f && !(in.sprint && !in.flying) && !in.jumpImpulse)
        v.cardio += dt * 0.10f * (0.35f + 0.65f * v.thirst);
    if (v.thirst < 0.12f) v.cardio -= dt * 0.04f;

    float well = 0.5f * (v.hunger + v.thirst);
    if (in.borderDrain <= 0.0f) {
        v.inspire += dt * (well - 0.45f) * 0.05f;
        if (meanLimbHealth(v) < 0.5f) v.inspire -= dt * 0.03f;
        if (in.moving && well > 0.5f) v.inspire += dt * 0.01f;
    } else {
        v.cardio -= in.borderDrain * dt;
        v.inspire -= in.borderDrain * dt;
    }

    bool usingLegs = (in.sprint && !in.flying) || in.jumpImpulse || (in.swim && !in.flying);
    bool usingHandR = in.mining;
    if (in.sprint && !in.flying) {
        drainStamina(v, f, FootL, rates().sprintStaminaPerSec * dt);
        drainStamina(v, f, FootR, rates().sprintStaminaPerSec * dt);
    }
    if (in.jumpImpulse && !in.flying) {
        drainStamina(v, f, FootL, rates().jumpStaminaBurst);
        drainStamina(v, f, FootR, rates().jumpStaminaBurst);
    }
    if (in.swim && !in.flying) {
        drainStamina(v, f, FootL, rates().swimStaminaPerSec * dt);
        drainStamina(v, f, FootR, rates().swimStaminaPerSec * dt);
    }
    if (usingHandR)
        drainStamina(v, f, HandR, rates().mineStaminaPerSec * dt);

    for (int i = 0; i < Count; i++) {
        bool busy = false;
        if ((i == FootL || i == FootR) && usingLegs) busy = true;
        if (i == HandR && usingHandR) busy = true;
        if (busy) {
            if (f.emptied[i]) f.recoverDelay[i] = rates().emptyRecoverDelay;
            continue;
        }
        if (f.emptied[i]) {
            if (f.recoverDelay[i] > 0.0f) {
                f.recoverDelay[i] -= dt;
                if (f.recoverDelay[i] < 0.0f) f.recoverDelay[i] = 0.0f;
                continue;
            }
            regenStamina(v, i, rates().stamRegenSlow * dt);
            if (v.limb[i].stamina >= 1.0f - 1e-4f) {
                f.emptied[i] = false;
                f.recoverDelay[i] = 0.0f;
            }
        } else {
            regenStamina(v, i, rates().stamRegenFast * dt);
        }
    }

    if (in.landImpact > 9.5f) {
        float extra = (in.landImpact - 9.5f) / 18.0f;
        if (extra > 1.6f) extra = 1.6f;
        hurt(v, FootL, extra * 0.85f);
        hurt(v, FootR, extra * 0.85f);
        if (extra > 0.45f) hurt(v, Core, (extra - 0.45f) * 0.55f);
        if (extra > 0.90f) {
            hurt(v, Chest, (extra - 0.90f) * 0.55f);
            hurt(v, Head, (extra - 0.90f) * 0.25f);
        }
    }

    if (v.hunger <= kDeadEps) {
        for (int i = 0; i < Count; i++) hurt(v, i, dt * 0.008f);
    }
    if (v.thirst <= kDeadEps) {
        hurt(v, Head, dt * 0.012f);
        hurt(v, Chest, dt * 0.010f);
        v.cardio -= dt * 0.05f;
    }
    if (v.cardio <= kDeadEps) {
        hurt(v, Chest, dt * 0.06f);
        hurt(v, Core, dt * 0.04f);
        hurt(v, Head, dt * 0.03f);
    }

    if (v.hunger > 0.55f && v.thirst > 0.55f && v.cardio > 0.40f && in.landImpact <= 0.0f) {
        float rec = dt * 0.012f * v.hunger;
        for (int i = 0; i < Count; i++) {
            if (v.limb[i].health > kDeadEps && v.limb[i].health < 1.0f) {
                v.limb[i].health += rec;
                clamp01(v.limb[i].health);
            }
        }
    }

    clamp01(v.hunger);
    clamp01(v.thirst);
    clamp01(v.cardio);
    clamp01(v.inspire);
}

} // namespace vitals
