#include "basic_construction.hpp"
#include "../../plugin.hpp"
#include "../../../material/registry.hpp"
#include "../../../material/blocks/grass_tuft_mat.hpp"
#include "../../../render/textures.hpp"
#include "../../../world/player.hpp"
#include "../../../world/animation.hpp"
#include "../../../core/config.hpp"

namespace plugin {
namespace BasicConstruction {
namespace {

constexpr BlockInfo kBlocks[BLOCK_COUNT] = {
    // name, opaque, transp, collid, liquid, top, side, bottom, icon, weight, friction, passable, dragH, dragV
    // drag is a damping rate in 1/s: velocity *= exp(-drag * dt). Horizontal and vertical are independent.
    { "Air",          false, false, false, false, TEX_DIRT,     TEX_DIRT,     TEX_DIRT,    TEX_DIRT,       0.00f, 0.00f, false, 0.0f, 0.0f },
    { "Grass",        true,  false, true,  false, TEX_GRASS_TOP,TEX_GRASS_SIDE,TEX_DIRT,   TEX_GRASS_TOP,  9.00f, 0.55f, false, 0.0f, 0.0f },
    { "Dirt",         true,  false, true,  false, TEX_DIRT,     TEX_DIRT,     TEX_DIRT,    TEX_DIRT,      10.00f, 0.62f, false, 0.0f, 0.0f },
    { "Stone",        true,  false, true,  false, TEX_STONE,    TEX_STONE,    TEX_STONE,   TEX_STONE,     18.00f, 0.75f, false, 0.0f, 0.0f },
    { "Sand",         true,  false, true,  false, TEX_SAND,     TEX_SAND,     TEX_SAND,    TEX_SAND,      12.00f, 0.38f, false, 0.0f, 0.0f },
    { "Water",        false, true,  false, true,  TEX_WATER,    TEX_WATER,    TEX_WATER,   TEX_WATER,      8.00f, 0.02f, true,  3.0f, 4.5f },
    { "Log",          true,  false, true,  false, TEX_LOG_TOP,  TEX_LOG_SIDE, TEX_LOG_TOP, TEX_LOG_SIDE,  12.00f, 0.48f, false, 0.0f, 0.0f },
    { "Leaves",       false, false, false, false, TEX_LEAVES,   TEX_LEAVES,   TEX_LEAVES,  TEX_LEAVES,     0.15f, 0.28f, true,  8.0f, 4.0f },
    { "Gravel",       true,  false, true,  false, TEX_GRAVEL,   TEX_GRAVEL,   TEX_GRAVEL,  TEX_GRAVEL,    14.00f, 0.52f, false, 0.0f, 0.0f },
    { "Snow",         true,  false, true,  false, TEX_SNOW,     TEX_SNOW,     TEX_DIRT,    TEX_SNOW,       3.00f, 0.08f, false, 0.0f, 0.0f },
    { "Glass",        false, true,  true,  false, TEX_GLASS,    TEX_GLASS,    TEX_GLASS,   TEX_GLASS,      6.00f, 0.12f, false, 0.0f, 0.0f },
    { "Bedrock",      true,  false, true,  false, TEX_BEDROCK,  TEX_BEDROCK,  TEX_BEDROCK, TEX_BEDROCK,   40.00f, 0.85f, false, 0.0f, 0.0f },
    { "Coal Ore",     true,  false, true,  false, TEX_COAL,     TEX_COAL,     TEX_COAL,    TEX_COAL,      18.00f, 0.72f, false, 0.0f, 0.0f },
    { "Iron Ore",     true,  false, true,  false, TEX_IRON,     TEX_IRON,     TEX_IRON,    TEX_IRON,      22.00f, 0.74f, false, 0.0f, 0.0f },
    { "Gold Ore",     true,  false, true,  false, TEX_GOLD,     TEX_GOLD,     TEX_GOLD,    TEX_GOLD,      24.00f, 0.70f, false, 0.0f, 0.0f },
    { "Diamond Ore",  true,  false, true,  false, TEX_DIAMOND,  TEX_DIAMOND,  TEX_DIAMOND, TEX_DIAMOND,   20.00f, 0.73f, false, 0.0f, 0.0f },
    { "Planks",       true,  false, true,  false, TEX_PLANKS,   TEX_PLANKS,   TEX_PLANKS,  TEX_PLANKS,     6.00f, 0.42f, false, 0.0f, 0.0f },
    { "Cobblestone",  true,  false, true,  false, TEX_COBBLE,   TEX_COBBLE,   TEX_COBBLE,  TEX_COBBLE,    16.00f, 0.80f, false, 0.0f, 0.0f },
    { "Bricks",       true,  false, true,  false, TEX_BRICK,    TEX_BRICK,    TEX_BRICK,   TEX_BRICK,     15.00f, 0.70f, false, 0.0f, 0.0f },
    { "Sandstone",    true,  false, true,  false, TEX_SANDSTONE, TEX_SANDSTONE, TEX_SANDSTONE, TEX_SANDSTONE, 14.00f, 0.58f, false, 0.0f, 0.0f },
    { "Shrub Stem",   false, false, false, false, TEX_SHRUB_STEM, TEX_SHRUB_STEM, TEX_SHRUB_STEM, TEX_SHRUB_STEM, 0.40f, 0.35f, true, 5.0f, 2.2f },
    { "Shrub Leaf",   false, false, false, false, TEX_SHRUB_LEAF, TEX_SHRUB_LEAF, TEX_SHRUB_LEAF, TEX_SHRUB_LEAF, 0.12f, 0.30f, true, 3.5f, 1.2f },
    { "Stick",        false, false, false, false, TEX_SHRUB_STEM, TEX_SHRUB_STEM, TEX_SHRUB_STEM, TEX_SHRUB_STEM, 0.20f, 0.40f, false, 0.0f, 0.0f },
    { "Grass Tuft",   false, false, false, false, TEX_GRASS_TUFT, TEX_GRASS_TUFT, TEX_GRASS_TUFT, TEX_GRASS_TUFT, 0.08f, 0.45f, true, 1.8f, 0.25f },
    { "Wood",         true,  false, true,  false, TEX_LOG_TOP,  TEX_WOOD_SIDE, TEX_LOG_TOP,  TEX_WOOD_SIDE,  8.00f, 0.46f, false, 0.0f, 0.0f },
    { "Bark",         false, false, false, false, TEX_BARK,     TEX_BARK,     TEX_BARK,     TEX_BARK,     0.10f, 0.50f, false, 0.0f, 0.0f },
    { "Hand Axe",     false, false, false, false, TEX_HAND_AXE, TEX_HAND_AXE, TEX_HAND_AXE, TEX_HAND_AXE, 1.20f, 0.55f, false, 0.0f, 0.0f },
    { "Shears",        false, false, false, false, TEX_SHEARS,   TEX_SHEARS,   TEX_SHEARS,   TEX_SHEARS,   0.80f, 0.35f, false, 0.0f, 0.0f },
    { "Hand Pick",     false, false, false, false, TEX_HAND_PICK, TEX_HAND_PICK, TEX_HAND_PICK, TEX_HAND_PICK, 1.40f, 0.55f, false, 0.0f, 0.0f },
    { "Hand Shovel",   false, false, false, false, TEX_HAND_SHOVEL, TEX_HAND_SHOVEL, TEX_HAND_SHOVEL, TEX_HAND_SHOVEL, 1.10f, 0.55f, false, 0.0f, 0.0f },
    { "Grass Item",    false, false, false, false, TEX_GRASS_TUFT, TEX_GRASS_TUFT, TEX_GRASS_TUFT, TEX_GRASS_TUFT, 0.04f, 0.40f, false, 0.0f, 0.0f },
    { "Shirt",         false, false, false, false, TEX_PLANKS, TEX_PLANKS, TEX_PLANKS, TEX_PLANKS, 0.40f, 0.50f, false, 0.0f, 0.0f },
    { "Shorts",        false, false, false, false, TEX_BARK, TEX_BARK, TEX_BARK, TEX_BARK, 0.35f, 0.50f, false, 0.0f, 0.0f },
    { "Shoes",         false, false, false, false, TEX_LOG_SIDE, TEX_LOG_SIDE, TEX_LOG_SIDE, TEX_LOG_SIDE, 0.50f, 0.60f, false, 0.0f, 0.0f },
    { "元素核心", true, false, true, false, TEX_DIAMOND, TEX_DIAMOND, TEX_DIAMOND, TEX_DIAMOND, 0.40f, 0.45f, false, 0.0f, 0.0f },
    { "原始之火", true, false, true, false, TEX_COAL, TEX_COAL, TEX_COAL, TEX_COAL, 0.40f, 0.45f, false, 0.0f, 0.0f },
    { "静滞之水", true, false, true, false, TEX_WATER, TEX_WATER, TEX_WATER, TEX_WATER, 0.40f, 0.20f, false, 0.0f, 0.0f },
    { "生命嫩枝", true, false, true, false, TEX_LEAVES, TEX_LEAVES, TEX_LEAVES, TEX_LEAVES, 0.30f, 0.40f, false, 0.0f, 0.0f },
    { "根须织毯", true, false, true, false, TEX_BARK, TEX_BARK, TEX_BARK, TEX_BARK, 0.30f, 0.55f, false, 0.0f, 0.0f },
    { "裁决天平", true, false, true, false, TEX_GOLD, TEX_GOLD, TEX_GOLD, TEX_GOLD, 0.50f, 0.40f, false, 0.0f, 0.0f },
    { "黄金冠冕", true, false, true, false, TEX_GOLD, TEX_GOLD, TEX_GOLD, TEX_GOLD, 0.50f, 0.42f, false, 0.0f, 0.0f },
    { "审判之书", true, false, true, false, TEX_PLANKS, TEX_PLANKS, TEX_PLANKS, TEX_PLANKS, 0.35f, 0.48f, false, 0.0f, 0.0f },
    { "鳞片沙漏", true, false, true, false, TEX_SAND, TEX_SAND, TEX_SAND, TEX_SAND, 0.40f, 0.36f, false, 0.0f, 0.0f },
    { "循环刻印", true, false, true, false, TEX_STONE, TEX_STONE, TEX_STONE, TEX_STONE, 0.45f, 0.65f, false, 0.0f, 0.0f },
    { "深渊棱镜", true, false, true, false, TEX_GLASS, TEX_GLASS, TEX_GLASS, TEX_GLASS, 0.40f, 0.18f, false, 0.0f, 0.0f },
    { "上古图腾", true, false, true, false, TEX_LOG_SIDE, TEX_LOG_SIDE, TEX_LOG_SIDE, TEX_LOG_SIDE, 0.45f, 0.60f, false, 0.0f, 0.0f },
    { "残响符石", true, false, true, false, TEX_COBBLE, TEX_COBBLE, TEX_COBBLE, TEX_COBBLE, 0.45f, 0.72f, false, 0.0f, 0.0f },
    { "旧神骸骨", true, false, true, false, TEX_SNOW, TEX_SNOW, TEX_SNOW, TEX_SNOW, 0.35f, 0.30f, false, 0.0f, 0.0f },
    { "无目雕像", true, false, true, false, TEX_SANDSTONE, TEX_SANDSTONE, TEX_SANDSTONE, TEX_SANDSTONE, 0.55f, 0.58f, false, 0.0f, 0.0f },
    { "火球术卷轴", false, false, false, false, TEX_COAL, TEX_COAL, TEX_COAL, TEX_COAL, 0.20f, 0.25f, false, 0.0f, 0.0f },
};
static_assert(sizeof(kBlocks) / sizeof(kBlocks[0]) == BLOCK_COUNT, "BasicConstruction block table size mismatch");

constexpr const char* kIds[BLOCK_COUNT] = {
    "air", "grass", "dirt", "stone", "sand", "water", "log", "leaves",
    "gravel", "snow", "glass", "bedrock", "coal_ore", "iron_ore",
    "gold_ore", "diamond_ore", "planks", "cobble", "brick", "sandstone",
    "shrub_stem", "shrub_leaf", "stick", "grass_tuft",
    "wood", "bark", "hand_axe",
    "shears", "hand_pick", "hand_shovel", "grass_item",
    "shirt", "shorts", "shoes",
    "elem_core", "prim_fire", "still_water", "life_sprout", "root_weave",
    "judge_scale", "gold_crown", "judge_tome", "scale_glass", "cycle_mark",
    "abyss_prism", "ancient_totem", "echo_rune", "old_bones", "eyeless", "arcane_fireball",
};
static_assert(sizeof(kIds) / sizeof(kIds[0]) == BLOCK_COUNT, "BasicConstruction id table size mismatch");

bool inCreative(uint8_t id) {
    if (id >= ITEM_ELEM_CORE) return false;
    switch (id) {
        case AIR:
        case GRASS:
        case SHRUB_STEM:
        case SHRUB_LEAF:
        case STICK:
        case GRASS_ITEM:
            return false;
        default:
            return true;
    }
}

struct BedrockStrategy : BlockStrategy {
    bool canBreak(uint8_t) const override { return false; }
};
struct UnplaceableStrategy : BlockStrategy {
    bool canPlace(uint8_t) const override { return false; }
};
struct LogStrategy : BlockStrategy {
    uint8_t dropItem(uint8_t) const override { return WOOD; }
    int extraDrops(uint8_t, uint8_t* out, int max) const override {
        if (!out || max < 1) return 0;
        out[0] = BARK;
        return 1;
    }
};
struct ShrubStemStrategy : BlockStrategy {
    uint8_t dropItem(uint8_t) const override { return STICK; }
};
struct GrassTuftStrategy : BlockStrategy {
    bool emitMesh(World::Chunk& ch, int lx, int y, int lz, int wx, int wz) override {
        // Upper half of a 2-tall tuft is drawn by the lower cell.
        if (y > 0 && ch.get(lx, y - 1, lz) == GRASS_TUFT) return true;
        float u0, v0, u1, v1;
        tex::tileUV(TEX_GRASS_TUFT, u0, v0, u1, v1);
        bool twoHigh = (y + 1 < cfg::CHUNK_Y && ch.get(lx, y + 1, lz) == GRASS_TUFT);
        // Two-cell tufts must reach 75% into the second cell (world height 1.75).
        float unitMax = mat::g_grassTuft.rand.get("tall_base", 0.375f)
                      + mat::g_grassTuft.rand.get("tall_range", 0.375f);
        if (unitMax < 0.01f) unitMax = 0.75f;
        float hScale = twoHigh ? (1.75f / unitMax) : 1.0f;
        mat::buildGrassTuftMesh(mat::g_grassTuft, ch.meshOpaque, lx, y, lz, wx, wz,
                                u0, v0, u1, v1, hScale);
        return true;
    }
};
struct PlayerStrategy : EntityStrategy {
    // Physics stays in Player::update. onTick picks idle / walk / run and
    // advances the clock; head look / body yaw stay runtime overlays.
    void onTick(EntityEvent& ev) override {
        if (!ev.player) return;
        Player& p = *ev.player;
        const anim::PlayerClips& lib = anim::playerClips();
        float hs = std::hypot(p.vel.x, p.vel.z);
        bool moving = !p.flying && hs > 0.35f;
        bool running = moving && p.sprinting;
        const anim::Clip& clip = !moving ? lib.idle : (running ? lib.run : lib.walk);
        float scale = 1.0f;
        if (moving) {
            float ref = running ? cfg::SPRINT_SPEED : cfg::WALK_SPEED;
            if (ref > 0.1f)
                scale = clampf(hs / ref, 0.45f, 1.35f);
        }
        anim::Playback pb{ p.animName, p.animClock };
        anim::advance(pb, clip, ev.dt, scale);
        p.animName = pb.name;
        p.animClock = pb.clock;
    }
};

BedrockStrategy g_bedrock;
UnplaceableStrategy g_unplaceable;
LogStrategy g_log;
ShrubStemStrategy g_shrubStem;
GrassTuftStrategy g_grassTuft;
PlayerStrategy g_player;

BlockStrategy* strategyFor(int id) {
    if (id >= ITEM_ELEM_CORE) return &g_unplaceable;
    switch (id) {
        case BEDROCK:    return &g_bedrock;
        case STICK:
        case HAND_AXE:
        case SHEARS:
        case HAND_PICK:
        case HAND_SHOVEL:
        case GRASS_ITEM:
        case SHIRT:
        case SHORTS:
        case SHOES: return &g_unplaceable;
        case LOG:        return &g_log;
        case SHRUB_STEM: return &g_shrubStem;
        case GRASS_TUFT: return &g_grassTuft;
        default:         return nullptr; // host default
    }
}

} // namespace

void registerModule() {
    for (int i = 0; i < BLOCK_COUNT; i++) {
        int got = registerBlock(kIds[i], kBlocks[i], strategyFor(i), inCreative((uint8_t)i));
        if (got != i) return; // already registered, or id contract broken
    }
    registerEntity({ "player", "Player", "player", &g_player });
}

} // namespace BasicConstruction
} // namespace plugin
