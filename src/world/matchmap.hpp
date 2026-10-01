#pragma once
#include <algorithm>
#include <cmath>
#include "../core/config.hpp"
#include "../core/math.hpp"

// Multiplayer field: a square of playable chunk columns, plus one outer ring.
// Gray fog starts on the first block of that ring, is opaque at half its depth,
// and the outer face stops travel. The side length follows the combat roster.
namespace matchmap {

inline constexpr int kFullSpan = 2048;
inline constexpr int kRim = 1;
inline int g_span = kFullSpan;

inline int span() { return g_span; }
inline void setSpan(int chunkColumns) {
    if (chunkColumns < 1) chunkColumns = 1;
    if (chunkColumns > kFullSpan) chunkColumns = kFullSpan;
    g_span = chunkColumns;
}

// Side length in chunk columns.
// n is combat players, clamped to [2, 64]. t is combat teams, clamped to [1, 4].
// Full roster (4 teams, 64 players) stays at kFullSpan.
inline int playableSpan(int combatTeams, int combatPlayers) {
    int t = combatTeams;
    int n = combatPlayers;
    if (t < 1) t = 1;
    if (t > 4) t = 4;
    if (n < 2) n = 2;
    if (n > 64) n = 64;
    const double side = (double)kFullSpan * std::pow(n / 64.0, 0.65) * std::pow(t / 4.0, 0.25);
    int columns = (int)std::lround(side);
    if (columns < 1) columns = 1;
    if (columns > kFullSpan) columns = kFullSpan;
    return columns;
}

inline int playMin() { return -span() / 2; }
inline int playMax() { return playMin() + span() - 1; }
inline int rimMin() { return playMin() - kRim; }
inline int rimMax() { return playMax() + kRim; }

inline bool columnPlayable(int cx, int cz) {
    return cx >= playMin() && cx <= playMax() && cz >= playMin() && cz <= playMax();
}
inline bool columnInside(int cx, int cz) {
    return cx >= rimMin() && cx <= rimMax() && cz >= rimMin() && cz <= rimMax();
}
inline bool columnRim(int cx, int cz) {
    return columnInside(cx, cz) && !columnPlayable(cx, cz);
}

// Six combat teams. Each owns one 4×4 chunk-column corner of the playable field.
inline constexpr int kCombatTeams = 6;
inline constexpr int kZoneChunks = 4;
inline constexpr float kDeploySeconds = 10.0f;
inline constexpr float kDeployDeathSeconds = 20.0f;
inline constexpr float kDeployFade = 1.6f;

struct Zone {
    int cx0 = 0;
    int cz0 = 0;
};

inline int blockToCol(int block, int chunk) {
    int q = block / chunk;
    int r = block % chunk;
    if (r != 0 && block < 0) q--;
    return q;
}

inline Zone combatZone(int index) {
    if (index < 0) index = 0;
    if (index >= kCombatTeams) index = kCombatTeams - 1;
    // 6-team layout: 3 columns x 2 rows
    // 2 | 4 | 3
    //---+---+---
    // 0 | 5 | 1
    int cols = 3;  // number of columns
    int col = index % cols;
    int row = index / cols;
    // Calculate x side: 0=left, 1=right, 2=center
    int xSide;
    if (col == 0) xSide = playMin();
    else if (col == 2) xSide = playMax() - (kZoneChunks - 1);
    else xSide = (playMin() + playMax() + 1 - kZoneChunks) / 2;
    // Calculate z side: 0=bottom, 1=top
    int zSide = row == 0 ? playMin() : (playMax() - (kZoneChunks - 1));
    return { xSide, zSide };
}

inline bool blockInZone(int combatIndex, int bx, int bz) {
    if (combatIndex < 0 || combatIndex >= kCombatTeams) return false;
    Zone z = combatZone(combatIndex);
    int cx = blockToCol(bx, cfg::CHUNK_X);
    int cz = blockToCol(bz, cfg::CHUNK_Z);
    return cx >= z.cx0 && cx < z.cx0 + kZoneChunks && cz >= z.cz0 && cz < z.cz0 + kZoneChunks;
}

// 0 on the playable side of the ring, 1 on the outer face.
inline float outwardT(float x, float z) {
    const float S = cfg::BLOCK_SCALE;
    const float play0 = (float)(playMin() * cfg::CHUNK_X) * S;
    const float play1 = (float)((playMax() + 1) * cfg::CHUNK_X) * S;
    const float rimW = (float)(kRim * cfg::CHUNK_X) * S;
    float ox = 0.0f, oz = 0.0f;
    if (x < play0) ox = play0 - x;
    else if (x > play1) ox = x - play1;
    if (z < play0) oz = play0 - z;
    else if (z > play1) oz = z - play1;
    float out = std::max(ox, oz);
    if (out <= 0.0f || rimW <= 0.0f) return 0.0f;
    float t = out / rimW;
    if (t > 1.0f) t = 1.0f;
    return t;
}

// t is 0 on the inner face of the ring and 1 on the outer face.
// Density rises linearly and is fully opaque at half the ring's depth.
inline float visualFog(float t) {
    if (t <= 0.0f) return 0.0f;
    float f = t / 0.5f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

inline float shaderDensity(float t) {
    (void)t;
    return cfg::FOG_DENSITY;
}

// Cardiopulmonary and inspiration loss per second. Follows the fog, saturated at half depth.
inline float vitalRate(float t) {
    float f = visualFog(t);
    if (f <= 0.0f) return 0.0f;
    return 0.012f * std::exp(3.4f * f);
}

inline void clampOutside(Vec3& pos, Vec3& vel) {
    const float S = cfg::BLOCK_SCALE;
    const float hw = cfg::PLAYER_HALF_WIDTH;
    const float minW = (float)(rimMin() * cfg::CHUNK_X) * S + hw;
    const float maxW = (float)((rimMax() + 1) * cfg::CHUNK_X) * S - hw;
    if (pos.x < minW) { pos.x = minW; if (vel.x < 0.0f) vel.x = 0.0f; }
    if (pos.x > maxW) { pos.x = maxW; if (vel.x > 0.0f) vel.x = 0.0f; }
    if (pos.z < minW) { pos.z = minW; if (vel.z < 0.0f) vel.z = 0.0f; }
    if (pos.z > maxW) { pos.z = maxW; if (vel.z > 0.0f) vel.z = 0.0f; }
}

} // namespace matchmap
