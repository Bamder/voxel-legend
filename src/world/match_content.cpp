#include "match_content.hpp"
#include "building_loot.hpp"
#include "matchmap.hpp"
#include "ritual.hpp"
#include "structure.hpp"
#include "world.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace match_content {
namespace {

std::vector<Vec3> roomSpots(World& world, int x, int y, int z, uint32_t seed) {
    struct Candidate { int x, y, z, rank; };
    std::vector<Candidate> candidates;
    for (int rise = 1; rise <= 5; ++rise) {
        for (int dz = -12; dz <= 12; dz += 3) {
            for (int dx = -12; dx <= 12; dx += 3) {
                int bx = x + dx, by = y + rise, bz = z + dz;
                if (by + 1 >= cfg::WORLD_H ||
                    !isSolid(world.getBlock(bx, by - 1, bz)) ||
                    isSolid(world.getBlock(bx, by, bz)) ||
                    isSolid(world.getBlock(bx, by + 1, bz))) continue;
                // Prefer usable floor in the center, then vary the order by seed.
                uint32_t hash = seed ^ uint32_t(bx * 73856093u) ^ uint32_t(bz * 19349663u);
                candidates.push_back({bx, by, bz, (dx * dx + dz * dz) * 256 + int(hash & 255u)});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.rank < b.rank;
    });
    std::vector<Vec3> spots;
    for (const Candidate& candidate : candidates) {
        bool separated = true;
        for (Vec3 used : spots) {
            float dx = used.x / cfg::BLOCK_SCALE - (candidate.x + 0.5f);
            float dz = used.z / cfg::BLOCK_SCALE - (candidate.z + 0.5f);
            if (dx * dx + dz * dz < 8.0f) { separated = false; break; }
        }
        if (!separated) continue;
        const float s = cfg::BLOCK_SCALE;
        spots.push_back({(candidate.x + 0.5f) * s, (candidate.y + 0.5f) * s,
                         (candidate.z + 0.5f) * s});
        if (spots.size() == 8) break;
    }
    return spots;
}

} // namespace

bool populateTeam(World& world, building_loot::Spawner& loot,
                  clue::Director& clues, uint32_t seed, int team) {
    if (team < 1 || team > matchmap::kCombatTeams) return false;
    const int assigned = ritual::assignedRitual(team);
    if (assigned < 0 || assigned >= ritual::kRitualCount) return false;
    int relics[3];
    ritual::recipeRelics(assigned, relics);
    matchmap::Zone zone = matchmap::combatZone(team - 1);
    const int spawnX = zone.cx0 * cfg::CHUNK_X + matchmap::kZoneChunks * cfg::CHUNK_X / 2;
    const int spawnZ = zone.cz0 * cfg::CHUNK_Z + matchmap::kZoneChunks * cfg::CHUNK_Z / 2;
    // The 32-40-block blueprints need enough separation to leave the spawn clear.
    const int offsets[3][2] = {{48, 0}, {0, 48}, {-48, 0}};
    std::array<std::vector<Vec3>, 3> spots;
    for (int room = 0; room < 3; ++room) {
        const int x = spawnX + offsets[room][0], z = spawnZ + offsets[room][1];
        int y = world.surfaceHeight(x, z);
        y = std::min(y, cfg::WORLD_H - 26);
        for (int cz = floorDiv(z - 21, cfg::CHUNK_Z); cz <= floorDiv(z + 21, cfg::CHUNK_Z); ++cz)
            for (int cx = floorDiv(x - 21, cfg::CHUNK_X); cx <= floorDiv(x + 21, cfg::CHUNK_X); ++cx)
                world.ensureColumn(cx, cz);
        if (!structure::paintRoomBuilding(world, x, y, z, room)) return false;
        spots[room] = roomSpots(world, x, y, z, seed ^ uint32_t(team * 101 + room * 17));
    }
    if (spots[0].size() < 2 || spots[1].size() < 6 || spots[2].empty()) {
        std::fprintf(stderr, "match content: team %d room spots %zu/%zu/%zu\n",
                     team, spots[0].size(), spots[1].size(), spots[2].size());
        return false;
    }

    std::array<Vec3, 3> bosses{};
    for (int i = 0; i < 3; ++i)
        if (!structure::guardianHome(world, relics[i], bosses[i])) {
            std::fprintf(stderr, "match content: guardian home missing for relic %d\n", relics[i]);
            return false;
        }
    if (!structure::paintMatchRitualAltar(world, assigned)) return false;

    // The weapon room has both melee tools and all three stackable Arcane items.
    const uint8_t weaponRoomItems[] = {HAND_PICK, HAND_AXE, ITEM_ARCANE_FIREBALL,
                                       ITEM_ARCANE_FREEZE, ITEM_ARCANE_HEAL};
    for (int i = 0; i < 5; ++i)
        if (!loot.spawnItemAt(spots[1][i], {weaponRoomItems[i], 1})) return false;
    if (spots[1].size() > 6) {
        const building_loot::Entry bonus[] = {
            {HAND_PICK, 1, 1, 1}, {HAND_AXE, 1, 1, 1},
            {ITEM_ARCANE_FIREBALL, 1, 1, 2}, {ITEM_ARCANE_FREEZE, 1, 1, 2},
            {ITEM_ARCANE_HEAL, 1, 1, 2},
        };
        if (!loot.spawnRandomLootAt(spots[1][6], bonus)) return false;
    }

    // Three Bosses supply the three relics assigned to this team's ritual.
    // The intervening clues are in buildings or at the previous Boss site.
    const struct Step { Vec3 drop, target; clue::Destination destination; uint8_t reward; } steps[] = {
        {spots[0][0], spots[1][5], clue::Destination::Arcane, ITEM_ARCANE_FIREBALL},
        {spots[1][5], bosses[0], clue::Destination::Boss, uint8_t(ritual::blockId(relics[0]))},
        {bosses[0], spots[2][0], clue::Destination::Clue, AIR},
        {spots[2][0], bosses[1], clue::Destination::Boss, uint8_t(ritual::blockId(relics[1]))},
        {bosses[1], spots[0][1], clue::Destination::Clue, AIR},
        {spots[0][1], bosses[2], clue::Destination::Boss, uint8_t(ritual::blockId(relics[2]))},
    };
    for (int i = 0; i < 6; ++i) {
        clue::Link link{uint8_t(team), uint8_t(i + 1), steps[i].target,
                        steps[i].destination, steps[i].reward};
        if (!loot.spawnClueAt(steps[i].drop, clues, link)) return false;
    }
    return true;
}

} // namespace match_content
