#include "room_body.hpp"
#include "../world/combat.hpp"
#include "../world/matchmap.hpp"
#include <cmath>

namespace room_body {
InputState movement(uint8_t b) {
    InputState in;
    in.forward = (b & kMoveForward) != 0; in.back = (b & kMoveBack) != 0;
    in.left = (b & kMoveLeft) != 0; in.right = (b & kMoveRight) != 0;
    in.jump = (b & kMoveJump) != 0; in.sneak = (b & kMoveSneak) != 0;
    in.sprint = (b & kMoveSprint) != 0;
    return in;
}

MovementLimits limits(const Player& p, const InputState& in, bool frozen) {
    auto m = combat::mobility(p.vitals, p.onGround, in.jump, p.inWater, frozen);
    return {m.canMove, m.canJump, m.canSprint, m.speedMultiplier};
}

bool accept(State& s, const PlayInputNet& in, uint32_t tick) {
    if (!std::isfinite(in.yaw) || !std::isfinite(in.pitch) ||
        std::fabs(in.yaw) > 10000 || std::fabs(in.pitch) > 1.570797f || (in.movement & 128)) return false;
    s.input = movement(in.movement);
    s.player.yaw = wrapPi(in.yaw);
    s.player.pitch = in.pitch;
    s.lastInputTick = tick;
    s.hasInput = true;
    return true;
}

void spawn(State& s, Vec3 position) {
    float yaw = s.player.yaw, pitch = s.player.pitch;
    s = State{};
    s.player.setSpawn(position);
    s.player.yaw = yaw; s.player.pitch = pitch;
    s.player.bodyYaw = yaw;
}

void tick(State& s, const World& world, uint32_t tick, bool active, bool mining) {
    Player& p = s.player;
    p.flying = p.noclip = p.privilegeMode = false;
    if (!active || vitals::isDead(p.vitals)) {
        p.vel = {}; p.sprinting = false;
        p.dead = vitals::isDead(p.vitals);
        return;
    }
    // A paused/disconnected client cannot keep walking indefinitely. Packet rate
    // cannot accelerate simulation: only the server's 20Hz tick calls this.
    bool fresh = s.hasInput && uint32_t(tick - s.lastInputTick) <= 10;
    InputState in = fresh ? s.input : InputState{};
    int bx = (int)std::floor(p.pos.x / cfg::BLOCK_SCALE);
    int bz = (int)std::floor(p.pos.z / cfg::BLOCK_SCALE);
    if (!world.columnLoaded(floorDiv(bx, cfg::CHUNK_X), floorDiv(bz, cfg::CHUNK_Z))) return;
    for (int sub = 0; sub < 6; ++sub) {
        p.update(world, in, cfg::FIXED_DT, limits(p, in));
        matchmap::clampOutside(p.pos, p.vel);
        vitals::TickInput vin;
        vin.moving = p.vel.x * p.vel.x + p.vel.z * p.vel.z > .001f;
        vin.sprint = p.sprinting;
        vin.jumpImpulse = p.jumpedThisUpdate; vin.landed = p.landedThisUpdate;
        vin.swim = p.inWater; vin.mining = mining && fresh; vin.landImpact = p.landImpact;
        vin.borderDrain = matchmap::vitalRate(matchmap::outwardT(p.pos.x, p.pos.z));
        vitals::tick(p.vitals, p.fatigue, vin, cfg::FIXED_DT);
        if (vitals::isDead(p.vitals)) { p.dead = true; p.vel = {}; break; }
    }
}

BodyStateNet snapshot(const State& s, bool landed) {
    const Player& p = s.player;
    BodyStateNet result;
    result.x = p.pos.x; result.y = p.pos.y; result.z = p.pos.z;
    result.vx = p.vel.x; result.vy = p.vel.y; result.vz = p.vel.z;
    result.vitals = p.vitals;
    result.fatigue = p.fatigue;
    result.flags = (landed ? 1 : 0) | (p.onGround ? 2 : 0) | (p.inWater ? 4 : 0);
    return result;
}
}
