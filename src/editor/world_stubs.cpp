#include "../world/world.hpp"
#include "../world/blocks.hpp"

// editor.exe links BasicConstruction strategies but not world.cpp.
// Those strategies may call a few World methods from onSelfCheck; provide
// no-op stubs so the material/model editor still links.

uint8_t World::getBlock(int, int, int) const { return AIR; }

bool World::setBlock(int, int, int, uint8_t, bool, bool, int, int) { return false; }

int World::torchAttachAt(int, int, int) const { return -1; }

uint32_t World::spawnDrop(const Vec3&, uint8_t, int, bool, Vec3) { return 0; }
