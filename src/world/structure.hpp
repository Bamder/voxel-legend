#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class World;

// Building blueprints (assets/structures/*.vlstruct) and the in-game structure editor volume.
namespace structure {

inline constexpr int kEditX = 8;
inline constexpr int kEditY = 8;
inline constexpr int kEditZ = 8;
inline constexpr int kEditW = 48;
inline constexpr int kEditH = 32;
inline constexpr int kEditD = 48;

void ensureLibrary();
std::string defaultPath();

// Loads every blueprint and rolls which ritual props this seed places.
void roll(uint32_t seed);

void stampColumn(int cx, int cz,
                 const std::function<int(int, int)>& height,
                 const std::function<void(int, int, int, uint8_t)>& put);

bool nearRitual(int ritual, float x, float z);

// Air cell directly above the brick triangle on that ritual's altar.
bool isOfferingCell(World& world, int ritual, int x, int y, int z);
// The team's three relics are all sitting in that triangle.
bool offeringReady(World& world, int ritual);
bool offeringPlaced(World& world, int ritual, int relic);

bool inVolume(int x, int y, int z);
bool paintFile(World& world, const std::string& path);
bool saveFile(const World& world, const std::string& path);
void clearVolume(World& world);

bool validName(const std::string& name);
std::string pathFor(const std::string& name);
void listFiles(std::vector<std::string>& names);
bool createFile(const std::string& name);
bool deleteFile(const std::string& name);

void collectBuildBlocks(std::vector<uint8_t>& out);

} // namespace structure
