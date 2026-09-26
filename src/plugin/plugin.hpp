#pragma once
#include "../world/blocks.hpp"
#include "../world/world.hpp"
#include <string>
#include <vector>

class Player;

// Host contract for complete functional-module plugins.
//
// This header is the engine side: Strategy bases, events, and the registry.
// Concrete types live in a plugin package under src/plugin/modules/.
// Built-in blocks + the player entity are plugin::BasicConstruction.
//
// Adding a type is NOT dropping a file. Appearance files (.model / .png / .rand)
// never register a new block or entity. Extra keys in those files are allowed
// and ignored.
//
// Block plugin (one complete module):
//   1. Subclass BlockStrategy; override only the events you need.
//   2. plugin::registerBlock({ "marble", info, &myStrategy });
//   3. Optional appearance: assets/materials/blocks/marble.model + tiles.
//
// Entity plugin (one complete module):
//   1. Subclass EntityStrategy (onTick, think/AI, onInteract, …).
//   2. plugin::registerEntity({ "wolf", "Wolf", "wolf", &myStrategy });
//   3. Optional appearance: assets/entities/wolf.model (skin only).
//   A lone wolf.model does not create a wolf entity type.
namespace plugin {

struct BlockEvent {
    World* world = nullptr;
    int x = 0, y = 0, z = 0;
    uint8_t id = 0;
    uint8_t previous = 0;
};

struct EntityEvent {
    World* world = nullptr;
    Player* player = nullptr; // set when the local player is the subject
    float dt = 0.0f;
};

// Strategy: plugin authors override the events they care about.
struct BlockStrategy {
    virtual ~BlockStrategy() = default;
    virtual void onPlace(BlockEvent&) {}
    virtual void onBreak(BlockEvent&) {}
    virtual void onRandomTick(BlockEvent&) {}
    virtual void onNeighborChanged(BlockEvent&) {}
    // Return true if the click was consumed (do not place a held block).
    virtual bool onInteract(BlockEvent&) { return false; }
    virtual bool canBreak(uint8_t) const { return true; }
    virtual bool canPlace(uint8_t) const { return true; }
    virtual uint8_t dropItem(uint8_t id) const { return id; }
    // Extra items besides dropItem. Returns how many were written to `out`.
    virtual int extraDrops(uint8_t /*id*/, uint8_t* /*out*/, int /*max*/) const { return 0; }
    // Return true if this strategy filled the chunk mesh for this cell.
    virtual bool emitMesh(World::Chunk&, int /*lx*/, int /*y*/, int /*lz*/, int /*wx*/, int /*wz*/) {
        return false;
    }
};

struct EntityStrategy {
    virtual ~EntityStrategy() = default;
    virtual void onSpawn(EntityEvent&) {}
    virtual void onDespawn(EntityEvent&) {}
    virtual void onTick(EntityEvent&) {}
    virtual void think(EntityEvent&) {} // AI
    virtual void onHurt(EntityEvent&, float /*amount*/) {}
    virtual bool onInteract(EntityEvent&) { return false; }
};

struct BlockModule {
    std::string id;          // type key / appearance stem; not inferred from files
    BlockInfo info{};
    BlockStrategy* strategy = nullptr;
    bool creative = true;    // appear in the inventory palette
};

struct EntityModule {
    std::string id;          // type key; this is the identity, not the .model name
    std::string title;       // UI
    std::string appearance;  // entities/<appearance>.model — skin only
    EntityStrategy* strategy = nullptr;
};

void init();

int blockCount();
const char* blockId(uint8_t id);
BlockStrategy* blockStrategy(uint8_t id);
const std::vector<uint8_t>& creativePalette();
// Register a new block type. Returns the numeric id, or -1 on failure.
// Strategy must outlive the process (static storage is fine). Display name is copied.
int registerBlock(BlockModule mod);
int registerBlock(const char* id, const BlockInfo& info, BlockStrategy* strategy, bool creative = true);

const EntityModule* findEntity(const char* id);
const std::vector<EntityModule>& entities();
// Register a new entity type. Fails if `mod.id` is already taken. Strategy must outlive the process.
bool registerEntity(EntityModule mod);

} // namespace plugin
