#pragma once
#include "guardian_ai.hpp"
#include "vitals.hpp"
#include <cstdint>

// Runs the guardian brain against live fighters. The caller owns hit points
// and applies the returned blows. Taunt, smoke, traps and suppress skills
// still have no effect; charge, evade and melee do.
namespace guardian_fight {

inline constexpr int kSlots = 16;
inline constexpr float kThreatPerHp = 120.0f;
inline constexpr float kHitReach = 3.6f;
inline constexpr float kAoeReach = 3.5f;
inline constexpr float kMeleeHurt = 0.07f;
inline constexpr float kAoeHurt = 0.04f;
inline constexpr uint32_t kAttackerBase = 0x47550000u;

inline uint32_t attackerId(int relic) { return kAttackerBase + (uint32_t)relic; }

struct Home {
    int relic = -1;
    Vec3 feet{};
    float hp = 0.0f;
    float maxHp = 1.0f;
};

struct Rival {
    uint32_t id = 0;
    Vec3 feet{};
    bool alive = false;
    bool active = false;
    float windup = 0.0f;
};

struct Blow {
    uint32_t target = 0;
    int relic = -1;
    float amount = 0.0f;
    int limb = -1;
    bool wholeBody = false;
};

struct Pose {
    int relic = -1;
    Vec3 feet{};
    float yaw = 0.0f;
    uint8_t swing = 0;
};

struct Ground {
    void* ctx = nullptr;
    // y is the feet height before the snap. Return the floor to stand on.
    float (*feetY)(void* ctx, float x, float y, float z) = nullptr;
    bool (*blocked)(void* ctx, float x, float y, float z) = nullptr;
};

void reset();

// Integer hit points actually removed. Ignored while the guardian is dodging.
void noteDamage(int relic, uint32_t attacker, int hp);

// False during a dodge's invulnerable window.
bool vulnerable(int relic);

// dt is seconds. Returns how many blows were written.
int tick(const Home* homes, int homeCount, const Rival* rivals, int rivalCount,
         float dt, Ground ground, Blow* blows, int maxBlows);

int poses(Pose* out, int max);

} // namespace guardian_fight
