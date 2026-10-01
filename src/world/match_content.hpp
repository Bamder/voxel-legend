#pragma once
#include <cstdint>

class World;
namespace building_loot { class Spawner; }
namespace clue { class Director; }

namespace match_content {

// Inclusive total of the three resource-building types.
// lower = max(15, ceil(players * 0.6 + teams * 0.4)), then uniform in [lower, 64].
int resourceBuildingTotal(int combatTeams, int combatPlayers, uint32_t pick);
// Splits total across the three types so the counts differ by at most one.
void splitResourceBuildings(int total, uint32_t pick, int outTypes[3]);

// Placement is shared by every team. The server can paint one site per tick
// so the welcome handshake is not stuck behind the whole map.
void beginMatchContent(World& world, building_loot::Spawner& loot, uint32_t seed);
bool advanceMatchContent(World& world, building_loot::Spawner& loot, uint32_t seed);
bool matchContentReady();
void placementProgress(int& done, int& total);

// Called once for each team that actually enters the match. The server owns
// the spawned drops and clue bindings; no other team needs to be present.
bool populateTeam(World& world, building_loot::Spawner& loot,
                  clue::Director& clues, uint32_t seed, int team);

} // namespace match_content
