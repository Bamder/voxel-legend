#include "match_content.hpp"
#include "building_loot.hpp"
#include "blocks.hpp"
#include "matchmap.hpp"
#include "ritual.hpp"
#include "structure.hpp"
#include "world.hpp"
#include <algorithm>
#include <array>
#include <cmath>
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

struct PlacedRoom {
    int type = 0;
    int x = 0;
    int z = 0;
    int owner = 0;
    std::vector<Vec3> spots;
};

std::vector<PlacedRoom> g_rooms;
const World* g_roomsWorld = nullptr;
uint32_t g_roomsSeed = 0;

bool separated(int x, int z, int ox, int oz, int minDist) {
    long long dx = (long long)x - ox;
    long long dz = (long long)z - oz;
    return dx * dx + dz * dz >= (long long)minDist * minDist;
}

bool clearOfSites(int x, int z, int skipRitual = -1) {
    for (int relic = 0; relic < ritual::RelicCount; ++relic) {
        int sx = 0, sz = 0;
        if (!structure::relicAnchor(relic, sx, sz)) continue;
        if (!separated(x, z, sx, sz, 80)) return false;
    }
    for (int ritualId = 0; ritualId < ritual::kRitualCount; ++ritualId) {
        if (ritualId == skipRitual) continue;
        int sx = 0, sz = 0;
        if (!structure::ritualAnchor(ritualId, sx, sz)) continue;
        if (!separated(x, z, sx, sz, 80)) return false;
    }
    return true;
}

int claimRoom(int type, int team, int minSpots) {
    int reuse = -1;
    for (int i = 0; i < (int)g_rooms.size(); ++i) {
        if (g_rooms[(size_t)i].type != type) continue;
        if ((int)g_rooms[(size_t)i].spots.size() < minSpots) continue;
        if (g_rooms[(size_t)i].owner == 0) {
            g_rooms[(size_t)i].owner = team;
            return i;
        }
        if (reuse < 0) reuse = i;
    }
    return reuse;
}

enum class PlaceKind : uint8_t { Relic, Altar, Building };

struct PlaceJob {
    PlaceKind kind = PlaceKind::Relic;
    int a = 0;
    int b = 0;
    int ax = 0;
    int az = 0;
    int slot = -1;
};

std::vector<PlaceJob> g_jobs;
int g_job = 0;
bool g_planned = false;
uint32_t g_placeRng = 1;
int g_placeX0 = 0;
int g_placeSpan = 1;

bool plannedFor(const World& world, uint32_t seed) {
    return g_planned && g_roomsWorld == &world && g_roomsSeed == seed;
}

uint32_t nextPlaceRng() {
    g_placeRng = g_placeRng * 1664525u + 1013904223u;
    return g_placeRng;
}

bool inPlaceField(int x, int z) {
    return x >= g_placeX0 && z >= g_placeX0 &&
           x < g_placeX0 + g_placeSpan && z < g_placeX0 + g_placeSpan;
}

void commitBuilding(World& world, building_loot::Spawner& loot, uint32_t seed,
                    int type, int n, int x, int z) {
    int h = world.surfaceHeight(x, z);
    int y = std::min(h, cfg::WORLD_H - 26);
    for (int cz = floorDiv(z - 21, cfg::CHUNK_Z); cz <= floorDiv(z + 21, cfg::CHUNK_Z); ++cz)
        for (int cx = floorDiv(x - 21, cfg::CHUNK_X); cx <= floorDiv(x + 21, cfg::CHUNK_X); ++cx)
            world.ensureColumn(cx, cz);
    structure::paintRoomBuilding(world, x, y, z, type);
    PlacedRoom room;
    room.type = type;
    room.x = x;
    room.z = z;
    room.spots = roomSpots(world, x, y, z, seed ^ uint32_t(type * 131 + n * 17));
    const building_loot::Entry stock[] = {
        {PLANKS, 2, 6, 3}, {COBBLE, 2, 6, 3}, {STONE, 1, 4, 1},
    };
    if (!room.spots.empty())
        loot.spawnRandomLootAt(room.spots[0], stock);
    g_rooms.push_back(std::move(room));
}

bool buildingSpaced(int x, int z) {
    for (const PlacedRoom& room : g_rooms)
        if (!separated(x, z, room.x, room.z, 48)) return false;
    return true;
}

void placeOneBuilding(World& world, building_loot::Spawner& loot, uint32_t seed,
                      int type, int n, int slot, int ax, int az) {
    const int needSpots[3] = {2, 6, 1};
    const bool beside = slot >= 0;
    bool accepted = false;
    int fallbackX = beside ? ax : g_placeX0 + g_placeSpan / 2;
    int fallbackZ = beside ? az : fallbackX;
    if (beside) {
        double ang = (slot * 120 + 20) * 0.017453292519943295;
        fallbackX = ax + (int)std::lround(std::cos(ang) * 88.0);
        fallbackZ = az + (int)std::lround(std::sin(ang) * 88.0);
    }
    if (fallbackX < g_placeX0) fallbackX = g_placeX0;
    if (fallbackZ < g_placeX0) fallbackZ = g_placeX0;
    if (fallbackX >= g_placeX0 + g_placeSpan) fallbackX = g_placeX0 + g_placeSpan - 1;
    if (fallbackZ >= g_placeX0 + g_placeSpan) fallbackZ = g_placeX0 + g_placeSpan - 1;
    for (int attempt = 0; attempt < 40 && !accepted; ++attempt) {
        int x = 0, z = 0;
        int skipRitual = -1;
        if (beside) {
            int radius = 56 + (int)(nextPlaceRng() % 57u);
            double ang = (nextPlaceRng() % 360u) * 0.017453292519943295;
            x = ax + (int)std::lround(std::cos(ang) * radius);
            z = az + (int)std::lround(std::sin(ang) * radius);
            long long dx = (long long)x - ax;
            long long dz = (long long)z - az;
            if (dx * dx + dz * dz < 56LL * 56LL || dx * dx + dz * dz > 112LL * 112LL) continue;
            for (int ritualId = 0; ritualId < ritual::kRitualCount; ++ritualId) {
                int sx = 0, sz = 0;
                if (!structure::ritualAnchor(ritualId, sx, sz)) continue;
                if (sx == ax && sz == az) { skipRitual = ritualId; break; }
            }
        } else {
            x = g_placeX0 + (int)(nextPlaceRng() % (uint32_t)g_placeSpan);
            z = g_placeX0 + (int)(nextPlaceRng() % (uint32_t)g_placeSpan);
        }
        if (!inPlaceField(x, z) || !clearOfSites(x, z, skipRitual) || !buildingSpaced(x, z)) continue;
        int h = world.surfaceHeight(x, z);
        if (h <= cfg::SEA_LEVEL + 1 || h >= cfg::WORLD_H - 28) continue;
        int y = std::min(h, cfg::WORLD_H - 26);
        for (int cz = floorDiv(z - 21, cfg::CHUNK_Z); cz <= floorDiv(z + 21, cfg::CHUNK_Z); ++cz)
            for (int cx = floorDiv(x - 21, cfg::CHUNK_X); cx <= floorDiv(x + 21, cfg::CHUNK_X); ++cx)
                world.ensureColumn(cx, cz);
        if (!structure::paintRoomBuilding(world, x, y, z, type)) continue;
        PlacedRoom room;
        room.type = type;
        room.x = x;
        room.z = z;
        room.spots = roomSpots(world, x, y, z, seed ^ uint32_t(type * 131 + n * 17));
        if ((int)room.spots.size() < needSpots[type] && attempt < 39) continue;
        const building_loot::Entry stock[] = {
            {PLANKS, 2, 6, 3}, {COBBLE, 2, 6, 3}, {STONE, 1, 4, 1},
        };
        if (!room.spots.empty())
            loot.spawnRandomLootAt(room.spots[0], stock);
        g_rooms.push_back(std::move(room));
        accepted = true;
    }
    if (!accepted)
        commitBuilding(world, loot, seed, type, n, fallbackX, fallbackZ);
}

void ensureMatchContent(World& world, building_loot::Spawner& loot, uint32_t seed) {
    beginMatchContent(world, loot, seed);
    while (!matchContentReady()) advanceMatchContent(world, loot, seed);
}

} // namespace

void beginMatchContent(World& world, building_loot::Spawner&, uint32_t seed) {
    if (plannedFor(world, seed)) return;
    g_rooms.clear();
    g_roomsWorld = &world;
    g_roomsSeed = seed;
    g_jobs.clear();
    g_job = 0;
    g_planned = true;
    for (int relic = 0; relic < ritual::RelicCount; ++relic) {
        int x = 0, z = 0;
        if (!structure::relicAnchor(relic, x, z)) continue;
        g_jobs.push_back({PlaceKind::Relic, relic, 0});
    }
    for (int team = 1; team <= matchmap::kCombatTeams; ++team) {
        if (!matchmap::teamInMatch(team)) continue;
        int assigned = ritual::assignedRitual(team);
        if (assigned >= 0) g_jobs.push_back({PlaceKind::Altar, assigned, team});
    }
    int altarX[matchmap::kCombatTeams];
    int altarZ[matchmap::kCombatTeams];
    int altarN = 0;
    for (const PlaceJob& job : g_jobs) {
        if (job.kind != PlaceKind::Altar || altarN >= matchmap::kCombatTeams) continue;
        int x = 0, z = 0;
        if (!structure::ritualAnchor(job.a, x, z)) continue;
        altarX[altarN] = x;
        altarZ[altarN] = z;
        ++altarN;
    }
    g_placeRng = seed ? seed : 1u;
    int counts[3] = {};
    int total = resourceBuildingTotal(matchmap::rosterTeams(), matchmap::rosterPlayers(), nextPlaceRng());
    int besideNeed = altarN * 3;
    if (total < besideNeed) total = besideNeed > 64 ? 64 : besideNeed;
    splitResourceBuildings(total, nextPlaceRng(), counts);
    g_placeX0 = matchmap::playMin() * cfg::CHUNK_X + 24;
    int x1 = (matchmap::playMax() + 1) * cfg::CHUNK_X - 24;
    if (x1 <= g_placeX0) {
        g_placeX0 = matchmap::playMin() * cfg::CHUNK_X;
        x1 = g_placeX0 + cfg::CHUNK_X;
    }
    g_placeSpan = std::max(1, x1 - g_placeX0);
    std::vector<int> types;
    types.reserve((size_t)total);
    int left[3] = { counts[0], counts[1], counts[2] };
    while ((int)types.size() < total) {
        bool any = false;
        for (int type = 0; type < 3; ++type) {
            if (left[type] <= 0) continue;
            types.push_back(type);
            --left[type];
            any = true;
            if ((int)types.size() == total) break;
        }
        if (!any) break;
    }
    int cursor = 0;
    int reserved = altarN * 3;
    if (reserved > (int)types.size()) reserved = (int)types.size();
    for (int altar = 0; altar < altarN; ++altar) {
        for (int slot = 0; slot < 3 && cursor < reserved; ++slot, ++cursor) {
            PlaceJob job;
            job.kind = PlaceKind::Building;
            job.a = types[(size_t)cursor];
            job.b = cursor;
            job.ax = altarX[altar];
            job.az = altarZ[altar];
            job.slot = slot;
            g_jobs.push_back(job);
        }
    }
    for (; cursor < (int)types.size(); ++cursor) {
        PlaceJob job;
        job.kind = PlaceKind::Building;
        job.a = types[(size_t)cursor];
        job.b = cursor;
        g_jobs.push_back(job);
    }
}

bool advanceMatchContent(World& world, building_loot::Spawner& loot, uint32_t seed) {
    if (!plannedFor(world, seed)) beginMatchContent(world, loot, seed);
    if (g_job >= (int)g_jobs.size()) return true;
    const PlaceJob job = g_jobs[(size_t)g_job++];
    if (job.kind == PlaceKind::Relic) structure::anchorRelicRuin(world, job.a);
    else if (job.kind == PlaceKind::Altar) structure::paintMatchRitualAltar(world, job.a);
    else placeOneBuilding(world, loot, seed, job.a, job.b, job.slot, job.ax, job.az);
    return g_job >= (int)g_jobs.size();
}

bool matchContentReady() {
    return g_planned && g_job >= (int)g_jobs.size();
}

int buildingsNear(int x, int z, int radius) {
    if (radius < 0) radius = 0;
    long long limit = (long long)radius * radius;
    int count = 0;
    for (const PlacedRoom& room : g_rooms) {
        long long dx = (long long)room.x - x;
        long long dz = (long long)room.z - z;
        if (dx * dx + dz * dz <= limit) ++count;
    }
    return count;
}

void placementProgress(int& done, int& total) {
    if (!g_planned) {
        done = 0;
        total = 0;
        return;
    }
    total = (int)g_jobs.size();
    done = g_job;
    if (done > total) done = total;
    if (total == 0) {
        done = 1;
        total = 1;
    }
}

int resourceBuildingTotal(int combatTeams, int combatPlayers, uint32_t pick) {
    if (combatTeams < 0) combatTeams = 0;
    if (combatPlayers < 0) combatPlayers = 0;
    int num = combatPlayers * 3 + combatTeams * 2;
    int lower = std::max(15, (num + 4) / 5);
    if (lower > 64) lower = 64;
    return lower + (int)(pick % (uint32_t)(64 - lower + 1));
}

void splitResourceBuildings(int total, uint32_t pick, int outTypes[3]) {
    if (!outTypes) return;
    if (total < 0) total = 0;
    int base = total / 3;
    int extra = total % 3;
    int order[3] = {0, 1, 2};
    for (int i = 2; i > 0; --i) {
        pick = pick * 1664525u + 1013904223u;
        int j = (int)((pick >> 16) % (uint32_t)(i + 1));
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    for (int i = 0; i < 3; ++i) outTypes[i] = 0;
    for (int i = 0; i < 3; ++i) outTypes[order[i]] = base + (i < extra ? 1 : 0);
}

bool populateTeam(World& world, building_loot::Spawner& loot,
                  clue::Director& clues, uint32_t seed, int team) {
    if (team < 1 || team > matchmap::kCombatTeams) return false;
    const int assigned = ritual::assignedRitual(team);
    if (assigned < 0 || assigned >= ritual::kRitualCount) return false;
    int relics[3];
    ritual::recipeRelics(assigned, relics);
    ensureMatchContent(world, loot, seed);
    const int minSpots[3] = {2, 6, 1};
    int claimed[3] = {-1, -1, -1};
    for (int type = 0; type < 3; ++type) {
        claimed[type] = claimRoom(type, team, minSpots[type]);
        if (claimed[type] < 0) claimed[type] = claimRoom(type, team, 1);
        if (claimed[type] < 0) {
            std::fprintf(stderr, "match content: team %d has no type %d resource building\n", team, type);
            return false;
        }
    }
    const std::vector<Vec3>& spots0 = g_rooms[(size_t)claimed[0]].spots;
    const std::vector<Vec3>& spots1 = g_rooms[(size_t)claimed[1]].spots;
    const std::vector<Vec3>& spots2 = g_rooms[(size_t)claimed[2]].spots;
    if (spots0.size() < 2 || spots1.size() < 6 || spots2.empty()) {
        std::fprintf(stderr, "match content: team %d room spots %zu/%zu/%zu\n",
                     team, spots0.size(), spots1.size(), spots2.size());
        return false;
    }

    std::array<Vec3, 3> bosses{};
    for (int i = 0; i < 3; ++i)
        if (!structure::guardianHome(world, relics[i], bosses[i])) {
            std::fprintf(stderr, "match content: guardian home missing for relic %d\n", relics[i]);
            return false;
        }

    // The weapon room has both melee tools and all three stackable Arcane items.
    const uint8_t weaponRoomItems[] = {HAND_PICK, HAND_AXE, ITEM_ARCANE_FIREBALL,
                                       ITEM_ARCANE_FREEZE, ITEM_ARCANE_HEAL};
    for (int i = 0; i < 5; ++i)
        if (!loot.spawnItemAt(spots1[i], {weaponRoomItems[i], 1})) return false;
    if (spots1.size() > 6) {
        const building_loot::Entry bonus[] = {
            {HAND_PICK, 1, 1, 1}, {HAND_AXE, 1, 1, 1},
            {ITEM_ARCANE_FIREBALL, 1, 1, 2}, {ITEM_ARCANE_FREEZE, 1, 1, 2},
            {ITEM_ARCANE_HEAL, 1, 1, 2},
        };
        if (!loot.spawnRandomLootAt(spots1[6], bonus)) return false;
    }

    // Three Bosses supply the three relics assigned to this team's ritual.
    // The intervening clues are in buildings or at the previous Boss site.
    const struct Step { Vec3 drop, target; clue::Destination destination; uint8_t reward; } steps[] = {
        {spots0[0], spots1[5], clue::Destination::Arcane, ITEM_ARCANE_FIREBALL},
        {spots1[5], bosses[0], clue::Destination::Boss, uint8_t(ritual::blockId(relics[0]))},
        {bosses[0], spots2[0], clue::Destination::Clue, AIR},
        {spots2[0], bosses[1], clue::Destination::Boss, uint8_t(ritual::blockId(relics[1]))},
        {bosses[1], spots0[1], clue::Destination::Clue, AIR},
        {spots0[1], bosses[2], clue::Destination::Boss, uint8_t(ritual::blockId(relics[2]))},
    };
    for (int i = 0; i < 6; ++i) {
        clue::Link link{uint8_t(team), uint8_t(i + 1), steps[i].target,
                        steps[i].destination, steps[i].reward};
        if (!loot.spawnClueAt(steps[i].drop, clues, link)) return false;
    }
    return true;
}

} // namespace match_content
