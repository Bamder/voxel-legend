#pragma once
#include <cstdint>

enum Block : uint8_t {
    AIR = 0,
    GRASS, DIRT, STONE, SAND, WATER, LOG, LEAVES, GRAVEL, SNOW, GLASS,
    BEDROCK, COAL_ORE, IRON_ORE, GOLD_ORE, DIAMOND_ORE,
    PLANKS, COBBLE, BRICK, SANDSTONE,
    SHRUB_STEM, SHRUB_LEAF, STICK, GRASS_TUFT,
    WOOD, BARK, HAND_AXE,
    SHEARS, HAND_PICK, HAND_SHOVEL, GRASS_ITEM,
    SHIRT, SHORTS, SHOES,
    ITEM_ELEM_CORE, ITEM_PRIM_FIRE, ITEM_STILL_WATER, ITEM_LIFE_SPROUT, ITEM_ROOT_WEAVE,
    ITEM_JUDGE_SCALE, ITEM_GOLD_CROWN, ITEM_JUDGE_TOME, ITEM_SCALE_GLASS, ITEM_CYCLE_MARK,
    ITEM_ABYSS_PRISM, ITEM_ANCIENT_TOTEM, ITEM_ECHO_RUNE, ITEM_OLD_BONES, ITEM_EYELESS,
    ITEM_ARCANE_FIREBALL,
    ARENA_SHELL,
    ITEM_TARGET,
    ITEM_ARCANE_FREEZE, ITEM_ARCANE_HEAL,
    ITEM_GUIDE_BOOK,
    ITEM_CLUE,
    GUARDIAN_CORE,
    // Medieval fabric. Appended so earlier save ids stay put.
    // tools/stl2vlstruct/medieval_rooms.py stamps these into the resource rooms.
    TIMBER, PLASTER, THATCH, CLAY_TILE, ASHLAR,
    // Placeable lights. Appended so earlier save ids stay put.
    TORCH, LANTERN,
    BLOCK_COUNT
};

enum TexId : uint8_t {
    TEX_DIRT = 0, TEX_GRASS_TOP, TEX_GRASS_SIDE, TEX_STONE, TEX_SAND, TEX_WATER,
    TEX_LOG_SIDE, TEX_LOG_TOP, TEX_LEAVES, TEX_GRAVEL, TEX_SNOW, TEX_GLASS,
    TEX_BEDROCK, TEX_COAL, TEX_IRON, TEX_GOLD, TEX_DIAMOND, TEX_PLANKS,
    TEX_COBBLE, TEX_BRICK, TEX_SANDSTONE,
    TEX_SOD_0, TEX_SOD_1, TEX_SOD_2, TEX_SOD_3, // grass-sod wither stages (green -> brown)
    TEX_SHRUB_STEM, TEX_SHRUB_LEAF, TEX_GRASS_TUFT, TEX_LEAF_X, TEX_SHRUB_LEAF_X,
    TEX_BARK, TEX_HAND_AXE, TEX_WOOD_SIDE,
    TEX_SHEARS, TEX_HAND_PICK, TEX_HAND_SHOVEL, TEX_CRACK, TEX_CORE,
    TEX_TIMBER, TEX_PLASTER, TEX_THATCH, TEX_CLAY_TILE, TEX_ASHLAR,
    TEX_TORCH_WOOD, TEX_FLAME, TEX_LANTERN, TEX_LANTERN_GLOW,
    TEX_COUNT
};

struct BlockInfo {
    const char* name;
    bool opaque;      // fully occludes neighbors + casts AO, drawn in opaque pass
    bool transparent; // drawn in blended pass
    bool collides;    // solid for physics
    bool liquid;
    uint8_t texTop, texSide, texBottom;
    uint8_t icon;     // atlas tile used for hotbar / palette icon
    float weight;     // mass of one voxel (leaves << wood)
    float friction;   // kinetic friction of this surface, 0 = slick, ~0.8 = rough
    bool passable;    // bodies move through; the block is a resisting medium
    float dragH;      // horizontal damping rate, 1/s (v *= exp(-drag * dt))
    float dragV;      // vertical damping rate, 1/s
    // Block light 0..15. Spreads one step weaker along each of the six axes,
    // so a source lights every direction the same way (not a single facing).
    uint8_t emission = 0;
};

constexpr int BLOCK_LIGHT_MAX = 15;

// enum Block values 0..BLOCK_COUNT-1 are the save/world-gen contract.
// Built-in BlockInfo, strategies, and the player entity are registered by
// plugin::BasicConstruction (not by appearance files).
extern BlockInfo g_blockInfo[256];
extern int g_blockCount;

inline int liveBlockCount() { return g_blockCount; }
inline bool validBlock(uint8_t b) { return b < liveBlockCount(); }
inline const BlockInfo& blockOf(uint8_t b) {
    if (g_blockCount > 0 && b < g_blockCount) return g_blockInfo[b];
    static const BlockInfo kMissing = {
        "?", false, false, false, false, 0, 0, 0, 0, 1.0f, 0.0f, false, 0.0f, 0.0f
    };
    return kMissing;
}
inline int blockEmission(uint8_t b) {
    if (!validBlock(b)) return 0;
    int e = (int)blockOf(b).emission;
    if (e < 0) return 0;
    if (e > BLOCK_LIGHT_MAX) return BLOCK_LIGHT_MAX;
    return e;
}
inline bool isOpaque(uint8_t b) { return validBlock(b) && blockOf(b).opaque; }
inline bool isTransparent(uint8_t b) { return validBlock(b) && blockOf(b).transparent; }
inline bool isSolid(uint8_t b) { return validBlock(b) && blockOf(b).collides; }
inline bool isPassable(uint8_t b) { return validBlock(b) && blockOf(b).passable; }
// A passable block is a medium, not a wall, even if collides was also set.
inline bool blocksMotion(uint8_t b) { return isSolid(b) && !isPassable(b); }
inline bool isLiquid(uint8_t b) { return validBlock(b) && blockOf(b).liquid; }
inline bool isPassableCutout(uint8_t b) {
    return validBlock(b) && !blockOf(b).collides && !blockOf(b).liquid && !blockOf(b).transparent;
}

// Tree and shrub foliage identity (crush, falling trees). Movement drag is BlockInfo::passable.
inline bool isFoliage(uint8_t b) {
    return b == LEAVES || b == SHRUB_STEM || b == SHRUB_LEAF;
}

// Strongest passable medium overlapping a body wins on each axis.
inline void accumPassableDrag(uint8_t b, float& dragH, float& dragV) {
    if (!isPassable(b)) return;
    const BlockInfo& info = blockOf(b);
    if (info.dragH > dragH) dragH = info.dragH;
    if (info.dragV > dragV) dragV = info.dragV;
}

inline bool isTreeWood(uint8_t b) { return b == LOG; }
inline bool isTreeLeaf(uint8_t b) { return b == LEAVES; }
inline bool isTreePart(uint8_t b) { return isTreeWood(b) || isTreeLeaf(b); }
inline bool isShrubPart(uint8_t b) { return b == SHRUB_STEM || b == SHRUB_LEAF; }
inline bool isCompressible(uint8_t b) { return b == LEAVES; }

// Item tags. Tools are ids that cannot be placed; harvest rules match these bits.
constexpr uint32_t TAG_AXE = 1u;
constexpr uint32_t TAG_WOODWORKING = 2u; // 木工
constexpr uint32_t TAG_ONE_HAND = 4u;    // 单手
constexpr uint32_t TAG_PICK = 8u;        // 镐
constexpr uint32_t TAG_SHOVEL = 16u;     // 铲
constexpr uint32_t TAG_SHEARS = 32u;     // 剪刀
// Filled by loot::resetBuiltin / data packs. Defined in loot.cpp.
uint32_t itemTags(uint8_t b);
inline bool hasItemTags(uint8_t b, uint32_t need) {
    return need != 0 && (itemTags(b) & need) == need;
}

inline float blockWeight(uint8_t b) {
    if (b == AIR || !validBlock(b)) return 0.0f;
    float w = blockOf(b).weight;
    return w > 0.0f ? w : 1.0f;
}

inline float blockFriction(uint8_t b) {
    if (b == AIR || !validBlock(b)) return 0.0f;
    float f = blockOf(b).friction;
    if (f < 0.0f) return 0.0f;
    if (f > 2.0f) return 2.0f;
    return f;
}

constexpr uint8_t FLAG_ALIVE = 1;   // grown living tree wood and leaves
constexpr uint8_t FLAG_HIDDEN = 2; // physics island: crushed leaf
constexpr uint8_t FLAG_SETTLED = 2; // world log: written back from a fall
// Lantern cells only. Same storage bit as FLAG_HIDDEN; read only when the block is LANTERN.
constexpr uint8_t FLAG_LANTERN_HANG = 2;
// bits 2..7 = cut faces (see log_appear.hpp flagCutFace)
// Living wood/leaves of one generated tree share a unique treeId (0 = unbound).

// One structure-file tag per flag bit. Save and load walk this table, so a new
// bit is persisted as soon as it has a name here. Do not rename an entry.
inline constexpr const char* kCellFlagTag[8] = {
    "alive",
    "hang",
    "cut0", "cut1", "cut2", "cut3", "cut4", "cut5",
};

// A sod face treats plant foliage (passable cutout blocks like leaves, shrubs,
// grass tufts) as air: plants sit on top of sod instead of withering it.
inline bool isSodAir(uint8_t b) {
    return b == AIR || isPassableCutout(b);
}

// A single inventory slot: a block type plus a quantity (count).
struct ItemSlot {
    uint8_t block = AIR;
    uint8_t count = 0;
    bool empty() const { return block == AIR || count == 0; }
    void clear() { block = AIR; count = 0; }
};

// Default hotbar contents (creative-style block palette selection).
inline constexpr uint8_t DEFAULT_HOTBAR[6] = {
    HAND_AXE, HAND_PICK, HAND_SHOVEL, SHEARS, PLANKS, STONE
};
