#pragma once
#include "loot.hpp"
#include "vitals.hpp"
#include "wear.hpp"
#include <string>
#include <vector>

// Gameplay data packs under assets/data/<pack>/.
//   pack.def
//   block/<id>.def
//   item/<id>.def
//   entity/<id>.def
// Later custom packs overlay vanilla by string id (higher priority wins).

namespace data {

inline constexpr const char* kHandId = "_hand";
inline constexpr const char* kSodId = "_sod";
inline constexpr const char* kVanilla = "vanilla";

struct HarvestDrop {
    std::string itemId;
    uint8_t count = 1;
};

struct ObjectDef {
    std::string id;
    std::string display;
    std::string pack = kVanilla;
    loot::Kind kind = loot::Kind::Block;
    uint8_t maxStack = 1;
    int wear = -1; // wear::Slot, or -1
    float hardness = 0.0f;
    float durability = 1.0f;
    float efficiency = 0.0f;
    float chargeSec = 0.5f;
    float cooldownSec = 0.5f;
    float weight = 1.0f;
    float wrongResist = loot::kDefaultWrongResist;
    uint32_t tags = 0;
    bool harvest = false;
    uint32_t harvestNeed = 0;
    HarvestDrop drops[4]{};
    int nDrops = 0;
    int crackFolds = loot::kDefaultCrackFolds;
    uint8_t crackR = loot::kDefaultCrackR;
    uint8_t crackG = loot::kDefaultCrackG;
    uint8_t crackB = loot::kDefaultCrackB;
};

struct EntityDef {
    std::string id = "player";
    std::string display = "Player";
    std::string pack = kVanilla;
    vitals::Rates rates{};
};

struct PackInfo {
    std::string name;
    std::string title;
    std::string description;
    int priority = 0;
};

struct TagInfo {
    const char* id;
    uint32_t bit;
    const wchar_t* label;
};

void init();
void reload();
bool saveAll();
bool saveObject(const ObjectDef& o);
bool saveEntity(const EntityDef& e);

std::vector<ObjectDef>& objects();
std::vector<EntityDef>& entities();
const std::vector<PackInfo>& packs();

ObjectDef* findObject(const std::string& id);
EntityDef* findEntity(const std::string& id);

int resolveId(const std::string& id); // -1 if unknown / _hand
const char* folderFor(const ObjectDef& o); // "block" or "item"

uint32_t parseTags(const std::string& s);
std::string formatTags(uint32_t bits);
const char* kindId(loot::Kind k);
loot::Kind parseKind(const std::string& s);
const TagInfo* tagList(int* count);

void apply();

} // namespace data
