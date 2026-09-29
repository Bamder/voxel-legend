#include "loot.hpp"
#include "wear.hpp"
#include "../core/config.hpp"
#include <algorithm>
#include <cmath>

namespace {

uint32_t g_itemTagBits[256]{};

} // namespace

uint32_t itemTags(uint8_t b) { return g_itemTagBits[b]; }

namespace loot {
namespace {

ItemDef g_item[256]{};
BreakStats g_block[256]{};
BreakStats g_tool[256]{};
HarvestRule g_harvest[256]{};
CrackStyle g_crack[256]{};
BreakStats g_hand{ 2.5f, 0.0f, 1.0f, 0.5f, 0.5f };
BreakStats g_sod{ 0.5f, 5.0f, 0.0f }; // 铲 1 次完全破坏；空手约 3 次
uint32_t g_sodNeed = TAG_SHOVEL;
float g_sodWrong = kDefaultWrongResist;
bool g_ready = false;

void fillBuiltin() {
    for (int i = 0; i < 256; i++) {
        g_item[i] = { Kind::Block, 1 };
        g_block[i] = { 1.6f, 9.0f, 0.0f };
        g_tool[i] = { 2.5f, 0.0f, 1.0f, 1.0f, 1.0f };
        g_harvest[i] = {};
        g_crack[i] = { kDefaultCrackFolds, kDefaultCrackR, kDefaultCrackG, kDefaultCrackB };
        g_itemTagBits[i] = 0;
    }

    auto kind = [](uint8_t id, Kind k, uint8_t stack) {
        g_item[id] = { k, stack };
    };
    kind(AIR, Kind::Block, 1);
    kind(HAND_AXE, Kind::Tool, 1);
    kind(SHEARS, Kind::Tool, 1);
    kind(HAND_PICK, Kind::Tool, 1);
    kind(HAND_SHOVEL, Kind::Tool, 1);
    kind(STICK, Kind::Item, cfg::MAX_STACK);
    kind(BARK, Kind::Item, cfg::MAX_STACK);
    kind(GRASS_ITEM, Kind::Item, cfg::MAX_STACK);
    g_item[SHIRT] = { Kind::Item, 1, (int8_t)wear::Upper };
    g_item[SHORTS] = { Kind::Item, 1, (int8_t)wear::Lower };
    g_item[SHOES] = { Kind::Item, 1, (int8_t)wear::Shoes };
    for (int i = ITEM_ELEM_CORE; i <= ITEM_EYELESS; i++)
        kind((uint8_t)i, Kind::Item, 1);

    auto blk = [](uint8_t id, float h, float d) {
        g_block[id] = { h, d, 0.0f };
    };
    blk(AIR, 0.0f, 0.0f);
    blk(WATER, 0.0f, 1.0f);
    blk(GRASS_TUFT, 0.2f, 2.0f); // 空手一次打掉（效率*硬度差 = 2.3）
    blk(LEAVES, 0.3f, 6.6f);
    blk(SHRUB_LEAF, 0.3f, 6.6f);
    blk(SNOW, 0.4f, 6.3f);
    blk(SAND, 1.4f, 8.8f);
    blk(GRASS, 1.6f, 9.0f);   // 空手公式约 10s（前摇+后摇各 0.5s）
    blk(DIRT, 1.6f, 9.0f);    // 空手公式约 10s（前摇+后摇各 0.5s）
    blk(GRAVEL, 1.7f, 8.8f);
    blk(GLASS, 1.5f, 3.0f);
    blk(SHRUB_STEM, 2.2f, 4.5f);
    blk(PLANKS, 2.4f, 4.0f);  // 空手公式约 40s（前摇+后摇各 0.5s）
    blk(WOOD, 2.4f, 4.0f);
    blk(LOG, 2.4f, 4.0f);
    blk(SANDSTONE, 3.8f, 36.0f);
    blk(BRICK, 3.8f, 36.0f);
    blk(COBBLE, 3.5f, 42.0f);
    blk(STONE, 4.0f, 36.0f);
    blk(COAL_ORE, 4.5f, 37.5f);
    blk(IRON_ORE, 5.0f, 45.0f);
    blk(GOLD_ORE, 5.0f, 36.0f);
    blk(DIAMOND_ORE, 6.0f, 45.0f);
    blk(BEDROCK, 100.0f, 10000.0f);
    for (int i = ITEM_ELEM_CORE; i <= ITEM_EYELESS; i++) {
        blk((uint8_t)i, 0.4f, 2.0f);
        HarvestRule relic;
        relic.used = true;
        relic.needTags = 0;
        relic.nDrops = 1;
        relic.drops[0] = { (uint8_t)i, 1 };
        relic.wrongResist = 1.0f;
        g_harvest[i] = relic;
    }

    auto tool = [](uint8_t id, float h, float e, float ch = 1.0f, float cd = 1.0f) {
        g_tool[id] = { h, 0.0f, e, ch, cd };
    };
    g_hand = { 2.5f, 0.0f, 1.0f, 0.5f, 0.5f };
    g_sod = { 0.5f, 5.0f, 0.0f };
    g_sodNeed = TAG_SHOVEL;
    g_sodWrong = kDefaultWrongResist;
    tool(SHEARS, 1.0f, 2.0f, 1.0f, 1.0f);
    tool(HAND_SHOVEL, 3.0f, 2.0f, 1.0f, 1.0f);
    tool(HAND_AXE, 3.4f, 2.5f, 1.0f, 1.0f);
    tool(HAND_PICK, 7.0f, 1.5f, 1.0f, 1.0f);

    g_itemTagBits[HAND_AXE] = TAG_AXE | TAG_WOODWORKING | TAG_ONE_HAND;
    g_itemTagBits[SHEARS] = TAG_SHEARS | TAG_ONE_HAND;
    g_itemTagBits[HAND_PICK] = TAG_PICK | TAG_ONE_HAND;
    g_itemTagBits[HAND_SHOVEL] = TAG_SHOVEL | TAG_ONE_HAND;

    auto rule = [](uint8_t block, uint32_t tags, uint8_t item, uint8_t count = 1) {
        HarvestRule r;
        r.used = true;
        r.needTags = tags;
        r.drops[0] = { item, count };
        r.nDrops = 1;
        g_harvest[block] = r;
    };
    rule(DIRT, TAG_SHOVEL, DIRT);
    rule(GRASS, TAG_SHOVEL, DIRT);
    rule(SAND, TAG_SHOVEL, SAND);
    rule(GRAVEL, TAG_SHOVEL, GRAVEL);
    rule(SNOW, TAG_SHOVEL, SNOW);
    rule(STONE, TAG_PICK, COBBLE);
    rule(COBBLE, TAG_PICK, COBBLE);
    rule(COAL_ORE, TAG_PICK, COAL_ORE);
    rule(IRON_ORE, TAG_PICK, IRON_ORE);
    rule(GOLD_ORE, TAG_PICK, GOLD_ORE);
    rule(DIAMOND_ORE, TAG_PICK, DIAMOND_ORE);
    rule(BRICK, TAG_PICK, BRICK);
    rule(SANDSTONE, TAG_PICK, SANDSTONE);
    rule(GLASS, TAG_PICK, GLASS);
    rule(LOG, TAG_AXE, WOOD);
    rule(WOOD, TAG_AXE, WOOD);
    rule(PLANKS, TAG_AXE, PLANKS);
    rule(SHRUB_STEM, TAG_AXE, STICK);
    rule(GRASS_TUFT, TAG_SHEARS, GRASS_ITEM);
    rule(LEAVES, TAG_SHEARS, LEAVES);
    rule(SHRUB_LEAF, TAG_SHEARS, GRASS_ITEM);
}

void ensure() {
    if (g_ready) return;
    fillBuiltin();
    g_ready = true;
}

} // namespace

void resetBuiltin() {
    fillBuiltin();
    g_ready = true;
}

void setItemDef(uint8_t id, ItemDef def) {
    ensure();
    if (def.maxStack < 1) def.maxStack = 1;
    if (def.wear < -1 || def.wear >= wear::Count) def.wear = -1;
    g_item[id] = def;
}

void setBlockBreak(uint8_t id, BreakStats s) {
    ensure();
    g_block[id] = s;
}

void setToolBreak(uint8_t id, BreakStats s) {
    ensure();
    g_tool[id] = s;
}

void setHand(BreakStats s) {
    ensure();
    g_hand = s;
}

void setSod(BreakStats s, uint32_t needTags, float wrongResist) {
    ensure();
    g_sod = s;
    g_sodNeed = needTags;
    if (wrongResist < 0.0f) wrongResist = 0.0f;
    if (wrongResist > 1.0f) wrongResist = 1.0f;
    g_sodWrong = wrongResist;
}

void setTags(uint8_t id, uint32_t tags) {
    ensure();
    g_itemTagBits[id] = tags;
}

void setHarvest(uint8_t block, const HarvestRule& rule) {
    ensure();
    HarvestRule r = rule;
    r.used = true;
    if (r.nDrops < 0) r.nDrops = 0;
    if (r.nDrops > 4) r.nDrops = 4;
    g_harvest[block] = r;
}

void clearHarvest(uint8_t block) {
    ensure();
    g_harvest[block] = {};
}

void setBlockCrack(uint8_t id, CrackStyle s) {
    ensure();
    s.folds = clampCrackFolds(s.folds);
    g_crack[id] = s;
}

CrackStyle blockCrack(uint8_t block) {
    ensure();
    return g_crack[block];
}

HarvestRule harvestRule(uint8_t block) {
    ensure();
    return g_harvest[block];
}

const ItemDef& itemDef(uint8_t id) {
    ensure();
    return g_item[id];
}

bool isHarvestBreak(uint8_t block, uint8_t heldTool) {
    ensure();
    if (block == AIR) return false;
    const HarvestRule& r = g_harvest[block];
    if (!r.used) return false;
    if (r.needTags == 0) return true;
    return hasItemTags(heldTool, r.needTags);
}

int harvestDrops(uint8_t block, uint8_t heldTool, DropSpec* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;
    if (!isHarvestBreak(block, heldTool)) return 0;
    const HarvestRule& r = g_harvest[block];
    int n = std::min(r.nDrops, maxOut);
    for (int i = 0; i < n; i++) out[i] = r.drops[i];
    return n;
}

BreakStats blockBreak(uint8_t block) {
    ensure();
    return g_block[block];
}

BreakStats toolBreak(uint8_t held) {
    ensure();
    if (isTool(held)) return g_tool[held];
    return g_hand;
}

BreakStats handBreak() {
    ensure();
    return g_hand;
}

float mineResist(uint8_t block, uint8_t held) {
    ensure();
    const HarvestRule& r = g_harvest[block];
    if (!r.used || r.needTags == 0) return 1.0f;
    if (hasItemTags(held, r.needTags)) return 1.0f;
    if (!isTool(held)) return 1.0f;
    float w = r.wrongResist;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    return w;
}

float mineDamage(uint8_t block, uint8_t held) {
    BreakStats b = blockBreak(block);
    BreakStats t = toolBreak(held);
    float adv = t.hardness - b.hardness;
    if (adv < 0.0f) adv = 0.0f;
    return t.efficiency * adv * mineResist(block, held);
}

BreakStats sodBreak() {
    ensure();
    return g_sod;
}

float sodMineResist(uint8_t held) {
    ensure();
    if (g_sodNeed == 0) return 1.0f;
    if (hasItemTags(held, g_sodNeed)) return 1.0f;
    if (!isTool(held)) return 1.0f;
    float w = g_sodWrong;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    return w;
}

float sodMineDamage(uint8_t held) {
    BreakStats t = toolBreak(held);
    BreakStats s = sodBreak();
    float adv = t.hardness - s.hardness;
    if (adv < 0.0f) adv = 0.0f;
    return t.efficiency * adv * sodMineResist(held);
}

float mineChargeSec(uint8_t held) {
    return clampMineTiming(toolBreak(held).chargeSec);
}

float mineCooldownSec(uint8_t held) {
    return clampMineTiming(toolBreak(held).cooldownSec);
}

int mineHits(uint8_t block, uint8_t held) {
    float dmg = mineDamage(block, held);
    float D = blockBreak(block).durability;
    if (dmg <= 1e-8f || D <= 0.0f) return 0;
    return (int)std::ceil(D / dmg - 1e-4f);
}

bool rayAabb(const Vec3& origin, const Vec3& dir, const Vec3& mn, const Vec3& mx, float& tHit) {
    float tmin = 0.0f, tmax = 1e30f;
    const float d[3] = { dir.x, dir.y, dir.z };
    const float o[3] = { origin.x, origin.y, origin.z };
    const float a[3] = { mn.x, mn.y, mn.z };
    const float b[3] = { mx.x, mx.y, mx.z };
    for (int i = 0; i < 3; i++) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < a[i] || o[i] > b[i]) return false;
            continue;
        }
        float inv = 1.0f / d[i];
        float t0 = (a[i] - o[i]) * inv;
        float t1 = (b[i] - o[i]) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmax < tmin) return false;
    }
    tHit = tmin;
    return tmax >= 0.0f;
}

} // namespace loot
