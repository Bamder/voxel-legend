#include "player.hpp"
#include "player_model.hpp"
#include "structure.hpp"
#include <algorithm>
#include <cmath>

static float approach(float cur, float target, float maxDelta) {
    if (cur < target) return std::min(cur + maxDelta, target);
    return std::max(cur - maxDelta, target);
}

Vec3 Player::eye() const {
    Vec3 o = pm::headCameraOffset(pitch, yaw);
    return { pos.x + o.x, pos.y + o.y, pos.z + o.z };
}

void Player::syncBodyYaw() {
    float d = wrapPi(yaw - bodyYaw);
    if (std::fabs(d) > pm::kBodyFollowRad)
        bodyYaw = yaw;
}

void Player::update(const World& world, const InputState& requested, float dt, MovementLimits limits) {
    InputState in = requested;
    in.jump = in.jump && limits.jump;
    in.sprint = in.sprint && limits.sprint;
    if (!limits.horizontal) {
        in.forward = in.back = in.left = in.right = false;
        vel.x = vel.z = 0;
    }
    const float S = cfg::BLOCK_SCALE;
    int ex = (int)std::floor(pos.x / S);
    int ez = (int)std::floor(pos.z / S);
    int headY = (int)std::floor((pos.y + cfg::EYE_HEIGHT) / S);
    int feetY = (int)std::floor((pos.y + 0.25f * S) / S);
    inWater = isLiquid(world.getBlock(ex, headY, ez)) || isLiquid(world.getBlock(ex, feetY, ez));
    float dragH = 0.0f, dragV = 0.0f;
    {
        const float HW = cfg::PLAYER_HALF_WIDTH;
        const float HGT = cfg::PLAYER_HEIGHT;
        int x0 = (int)std::floor((pos.x - HW) / S);
        int x1 = (int)std::floor((pos.x + HW - 1e-6f) / S);
        int y0 = (int)std::floor(pos.y / S);
        int y1 = (int)std::floor((pos.y + HGT - 1e-6f) / S);
        int z0 = (int)std::floor((pos.z - HW) / S);
        int z1 = (int)std::floor((pos.z + HW - 1e-6f) / S);
        for (int bx = x0; bx <= x1; bx++)
            for (int by = y0; by <= y1; by++)
                for (int bz = z0; bz <= z1; bz++)
                    accumPassableDrag(world.getBlock(bx, by, bz), dragH, dragV);
    }

    Vec3 wish{ 0, 0, 0 };
    if (in.forward) wish += forward();
    if (in.back) wish -= forward();
    if (in.right) wish += right();
    if (in.left) wish -= right();
    float wl = wish.length();
    if (wl > 1e-4f) wish = wish / wl;

    float speed = flying ? (in.sprint ? cfg::FLY_SPRINT_SPEED : cfg::FLY_SPEED)
                 : inWater ? cfg::SWIM_SPEED
                 : (in.sprint ? cfg::SPRINT_SPEED : cfg::WALK_SPEED);
    if (!privilegeMode && !flying) {
        speed *= vitals::moveSpeedMul(vitals, fatigue);
        if (in.sprint && !vitals::canSprint(vitals))
            speed = cfg::WALK_SPEED * vitals::moveSpeedMul(vitals, fatigue);
    }
    sprinting = in.sprint && !flying && !inWater
        && (privilegeMode || vitals::canSprint(vitals));
    // Ground acceleration would otherwise erase viscous drag, so the wish speed
    // itself yields. Water keeps its swim speed; drag still bleeds extra velocity.
    if (!flying && !inWater && dragH > 0.0f)
        speed /= (1.0f + dragH * 0.25f);
    speed *= clampf(limits.speed, 0.0f, 1.0f);
    float accel = flying ? 60.0f : (onGround ? 80.0f : 18.0f);
    if (inWater) accel = 20.0f;

    vel.x = approach(vel.x, wish.x * speed, accel * dt);
    vel.z = approach(vel.z, wish.z * speed, accel * dt);

    jumpedThisUpdate = false;
    bool wasGrounded = onGround;

    if (flying) {
        vel.y = 0.0f;
        if (in.jump) vel.y += cfg::FLY_SPEED;
        if (in.sneak) vel.y -= cfg::FLY_SPEED;
    } else if (inWater) {
        vel.y -= cfg::WATER_GRAVITY * dt;
        if (in.jump) vel.y = cfg::SWIM_SPEED * 1.7f;
        vel.y = clampf(vel.y, -8.0f, 8.0f);
    } else {
        vel.y -= cfg::GRAVITY * dt;
        if (in.jump && onGround && (privilegeMode || vitals::canJump(vitals))) {
            float jh = privilegeMode ? 1.0f : vitals::jumpHeightMul(vitals, fatigue);
            vel.y = cfg::JUMP_SPEED * jh;
            jumpedThisUpdate = true;
        }
        vel.y = clampf(vel.y, -70.0f, 70.0f);
    }

    if (!flying) {
        if (dragH > 0.0f) {
            float k = std::exp(-dragH * dt);
            vel.x *= k;
            vel.z *= k;
        }
        if (dragV > 0.0f)
            vel.y *= std::exp(-dragV * dt);
    }

    onGround = false;
    float vy0 = vel.y;
    moveAxis(world, 0, vel.x * dt);
    moveAxis(world, 1, vel.y * dt);
    moveAxis(world, 2, vel.z * dt);
    landImpact = (!flying && onGround && vy0 < 0.0f) ? -vy0 : 0.0f;
    landedThisUpdate = !flying && !wasGrounded && onGround;

    if (flying) onGround = false;
    syncBodyYaw();
}

void Player::moveAxis(const World& world, int axis, float delta) {
    if (delta == 0.0f) return;
    if (axis == 0) pos.x += delta;
    else if (axis == 1) pos.y += delta;
    else pos.z += delta;
    if (noclip) {
        onGround = false;
        return;
    }

    const float HW = cfg::PLAYER_HALF_WIDTH;
    const float HGT = cfg::PLAYER_HEIGHT;
    const float S = cfg::BLOCK_SCALE;
    const float STEP = 1.1f * S; // auto-step height: 1.1 blocks
    Vec3 mn = { pos.x - HW, pos.y, pos.z - HW };
    Vec3 mx = { pos.x + HW, pos.y + HGT, pos.z + HW };

    int x0 = (int)std::floor(mn.x / S), x1 = (int)std::floor(mx.x / S - 1e-6f);
    int y0 = (int)std::floor(mn.y / S), y1 = (int)std::floor(mx.y / S - 1e-6f);
    int z0 = (int)std::floor(mn.z / S), z1 = (int)std::floor(mx.z / S - 1e-6f);

    int hitX = 0, hitY = 0, hitZ = 0;
    bool hit = false;
    for (int bx = x0; bx <= x1 && !hit; bx++)
        for (int by = y0; by <= y1 && !hit; by++)
            for (int bz = z0; bz <= z1 && !hit; bz++)
                if (blocksMotion(world.getBlock(bx, by, bz)) &&
                    !structure::isGuardianToken(bx, by, bz, world.getBlock(bx, by, bz))) {
                    hitX = bx; hitY = by; hitZ = bz; hit = true;
                }

    if (!hit) {
        world.resolvePhysPlayer(pos, vel, axis, delta, HW, HGT, onGround);
        return;
    }

    // Horizontal auto-step: if the collided block's top is within one block above our
    // feet, step up onto it (provided our body fits above it).
    if (axis != 1) {
        float blockTop = (hitY + 1) * S;
        float rise = blockTop - pos.y;
        if (rise > 0.0f && rise <= STEP + 1e-3f) {
            float origY = pos.y;
            pos.y = blockTop;
            bool clear = true;
            int ny0 = (int)std::floor(pos.y / S);
            int ny1 = (int)std::floor((pos.y + HGT - 1e-6f) / S);
            for (int bx = x0; bx <= x1 && clear; bx++)
                for (int by = ny0; by <= ny1 && clear; by++)
                    for (int bz = z0; bz <= z1 && clear; bz++)
                        if (blocksMotion(world.getBlock(bx, by, bz)) &&
                            !structure::isGuardianToken(bx, by, bz, world.getBlock(bx, by, bz)))
                            clear = false;
            if (clear) {
                onGround = true;
                vel.y = 0.0f;
                world.resolvePhysPlayer(pos, vel, axis, delta, HW, HGT, onGround);
                return; // stepped up
            }
            pos.y = origY; // revert the step
        }
    }

    // Normal collision resolution (clamp + zero velocity).
    if (hit) {
        if (axis == 0) {
            pos.x = (delta > 0.0f) ? (hitX * S - HW - 1e-4f) : (hitX * S + S + HW + 1e-4f);
            vel.x = 0.0f;
        } else if (axis == 1) {
            if (delta > 0.0f) pos.y = hitY * S - HGT - 1e-4f;
            else { pos.y = hitY * S + S + 1e-4f; onGround = true; }
            vel.y = 0.0f;
        } else {
            pos.z = (delta > 0.0f) ? (hitZ * S - HW - 1e-4f) : (hitZ * S + S + HW + 1e-4f);
            vel.z = 0.0f;
        }
    }

    world.resolvePhysPlayer(pos, vel, axis, delta, HW, HGT, onGround);
}
