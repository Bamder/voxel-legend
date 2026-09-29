#pragma once
#include "guardian_fight.hpp"
#include "structure.hpp"
#include "world.hpp"
#include "blocks.hpp"
#include "../core/config.hpp"
#include <cmath>
#include <limits>

// Floor directly under a candidate stand height, at most two blocks down.
// Returns NaN when that column has no support, so a step cannot float.
// The relic token is not a floor; the core is.
inline float guardianFeetY(void* ctx, float x, float y, float z) {
    World* world = static_cast<World*>(ctx);
    if (!world || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return std::numeric_limits<float>::quiet_NaN();
    const float scale = cfg::BLOCK_SCALE;
    int ix = (int)std::floor(x / scale);
    int iz = (int)std::floor(z / scale);
    int top = (int)std::floor((y - 1e-4f) / scale);
    if (top >= cfg::WORLD_H) top = cfg::WORLD_H - 1;
    int bottom = top - 2;
    if (bottom < 0) bottom = 0;
    for (int iy = top; iy >= bottom; --iy) {
        uint8_t block = world->getBlock(ix, iy, iz);
        if (structure::isGuardianToken(ix, iy, iz, block)) continue;
        if (!blocksMotion(block)) continue;
        return (float)(iy + 1) * scale;
    }
    return std::numeric_limits<float>::quiet_NaN();
}

inline bool guardianBlocked(void* ctx, float x, float y, float z) {
    World* world = static_cast<World*>(ctx);
    if (!world || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return true;
    const float scale = cfg::BLOCK_SCALE;
    int ix = (int)std::floor(x / scale);
    int iz = (int)std::floor(z / scale);
    auto solid = [&](float yy) {
        int iy = (int)std::floor(yy / scale);
        uint8_t block = world->getBlock(ix, iy, iz);
        if (block == (uint8_t)GUARDIAN_CORE) return false;
        if (structure::isGuardianToken(ix, iy, iz, block)) return false;
        return blocksMotion(block);
    };
    return solid(y + 0.3f) || solid(y + 1.1f);
}

inline guardian_fight::Ground guardianGround(World& world) {
    return { &world, guardianFeetY, guardianBlocked };
}
