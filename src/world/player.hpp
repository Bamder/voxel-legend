#pragma once
#include "../core/math.hpp"
#include "vitals.hpp"
#include "world.hpp"
#include <string>

struct InputState {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool jump = false;
    bool sneak = false;
    bool sprint = false;
};

class Player {
public:
    Vec3 pos;          // feet position
    Vec3 vel;
    float yaw = 0.0f;      // look / head yaw (crosshair)
    float pitch = 0.0f;    // look / head pitch
    float bodyYaw = 0.0f;  // torso yaw; catches up when |head-body| > 5°
    bool onGround = false;
    bool flying = false;
    bool noclip = false; // spectator camera: move through blocks
    bool sprinting = false; // this update used sprint speed; not saved
    bool inWater = false;
    bool privilegeMode = false; // 权限模式: no drain / no death
    bool dead = false;
    float landImpact = 0.0f;    // downward speed on landing this update (0 if none)
    bool jumpedThisUpdate = false;
    bool landedThisUpdate = false;
    vitals::Vitals vitals{};
    vitals::Fatigue fatigue{};
    std::string animName = "idle";
    float animClock = 0.0f;
    // Right-hand strike overlay while mining. "punch", "axe_chop", or "pick_mine".
    // A pick swing plays two_hand_ready, then pick_raise, then mine_down.
    // After the hit, mine_up lifts back to the raise. pickRaised means the next
    // held swing skips the ready and the raise and plays mine_down only.
    std::string strikeName;
    bool pickRaised = false;
    float strikeCharge = 0.5f;
    float strikeCool = 0.5f;
    // Player-owned attempt clock (windup + recovery). Cooldown always ticks down; click-spam cannot skip it.
    float mineCooldown = 0.0f;
    float mineCharge = 0.0f;

    Vec3 eye() const;
    Vec3 forward() const { return { std::sin(yaw), 0.0f, -std::cos(yaw) }; }
    Vec3 right() const { return { std::cos(yaw), 0.0f, std::sin(yaw) }; }
    Vec3 lookDir() const {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        return { std::sin(yaw) * cp, sp, -std::cos(yaw) * cp };
    }

    void setSpawn(const Vec3& p) { pos = p; vel = { 0, 0, 0 }; }
    void syncBodyYaw();

    void update(const World& world, const InputState& in, float dt);

private:
    void moveAxis(const World& world, int axis, float delta);
};
