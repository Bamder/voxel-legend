#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "../core/math.hpp"

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

// The relic block on a guardian platform is a hidden token, not the pickup.
// Defeating that guardian removes the token and the caller spawns the drop.
struct GuardianSpan {
    int relic = -1;
    float feetX = 0, feetY = 0, feetZ = 0;
    float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    int hp = 0;
    int maxHp = 1;
};

bool isGuardianToken(int x, int y, int z, uint8_t block);
void collectGuardians(const World& world, std::vector<GuardianSpan>& out);
bool raycastGuardian(const World& world, const Vec3& origin, const Vec3& dir, float maxDist,
                     float& tHit, GuardianSpan& hit, float radius = 0.0f);
// True on the blow that drops the relic. Writes the token cell.
// amount is subtracted from the guardian's hit points. Zero deals no damage.
bool damageGuardian(const World& world, int relic, int amount, int& x, int& y, int& z);
// Hurt dealt by one swing, after armor, resistance, and multipliers.
int guardianStrikeHurt(uint8_t held, int relic);
// Converts Arcane percentage damage into this legacy guardian's integer HP.
// Fireball distance is measured from the explosion to the guardian bounds;
// Freeze is a direct hit. Static guardians do not receive timed AI statuses.
int guardianArcaneHurt(uint8_t item, int relic, float distance = 0.0f);

// Match-world guardians. The room server owns hp; clients only display it.
struct GuardianSync {
    int relic = -1;
    int x = 0, y = 0, z = 0; // token cell
    int hp = 0;
    int maxHp = 1;
};
void collectRoomGuardians(std::vector<GuardianSync>& out);
void applyRoomGuardian(int relic, int hp, float x, float y, float z, float yaw, uint8_t swing);
// Combat pose. When set, the hit volume and the drawn model leave the token.
void setGuardianPose(int relic, float x, float y, float z, float yaw, uint8_t swing);
void clearGuardianPose(int relic);
bool guardianPose(int relic, float& x, float& y, float& z, float& yaw, uint8_t& swing);
// Living guardian whose model is within reach of eye, and no foreign block stops the path.
bool roomGuardianHit(const World& world, const Vec3& eye, int relic, float reach,
                     int& x, int& y, int& z);
// Nearest living guardian within maxDist of pos (world units). 
bool nearestGuardian(const World& world, const Vec3& pos, float maxDist, GuardianSpan& out);
const char* guardianAppearance(int relic);

// Free-explore trial chamber: 255×128×255 blocks (the world is only 128 blocks tall).
// Origin is chunk-aligned and far from the explore spawn.
inline constexpr int kTrialX0 = 262144;
inline constexpr int kTrialZ0 = 262144;
inline constexpr int kTrialSpan = 255;
inline constexpr int kTrialCX = kTrialX0 + (kTrialSpan - 1) / 2;
inline constexpr int kTrialCZ = kTrialZ0 + (kTrialSpan - 1) / 2;
inline constexpr int kTrialFloor = 0;

// Replaces the site list with one guardian platform. Only for free explore.
void openTrial(int relic);
void closeTrial();
bool trialActive();
// Writes the trial core if generation left the anchor cell empty.
void ensureTrialCore(World& world);

bool inVolume(int x, int y, int z);
bool paintFile(World& world, const std::string& path);
// Centers the altar on worldX/worldZ. Writes the floor Y back through worldY:
// the highest terrain under the footprint, clamped so the roof stays in the world.
bool paintRitualAltar(World& world, int worldX, int& worldY, int worldZ, int altarIndex);
// Number of available ritual altar variants
inline constexpr int kRitualAltarCount = 6;
const char* ritualAltarName(int altarIndex);

// Room building count and names (for props room, weapon room, clue room)
inline constexpr int kRoomBuildingCount = 3;
const char* roomBuildingName(int roomIndex);
// Centers the room on worldX/worldZ and writes the floor Y back through worldY.
bool paintRoomBuilding(World& world, int worldX, int& worldY, int worldZ, int roomIndex);
bool saveFile(const World& world, const std::string& path);
void clearVolume(World& world);

bool validName(const std::string& name);
std::string pathFor(const std::string& name);
void listFiles(std::vector<std::string>& names);
bool createFile(const std::string& name);
bool deleteFile(const std::string& name);

void collectBuildBlocks(std::vector<uint8_t>& out);

// Stonehenge structure
inline constexpr int kStonehengeCount = 1;
const char* stonehengeName(int index);
bool paintStonehenge(World& world, int worldX, int& worldY, int worldZ, int stonehengeIndex);

} // namespace structure
