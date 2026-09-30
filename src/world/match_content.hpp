#pragma once
#include <cstdint>

class World;
namespace building_loot { class Spawner; }
namespace clue { class Director; }

namespace match_content {

// Called once for each team that actually enters the match. The server owns
// the spawned drops and clue bindings; no other team needs to be present.
bool populateTeam(World& world, building_loot::Spawner& loot,
                  clue::Director& clues, uint32_t seed, int team);

} // namespace match_content
