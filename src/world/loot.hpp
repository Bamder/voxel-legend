#pragma once
#include "blocks.hpp"
#include "../core/math.hpp"

// Data-driven item + harvest tables.
// Built-in defaults live in loot.cpp; assets/data/<pack>/ overlays them at
// startup (data::init). Edit with editor.exe → Data Pack Editor.

namespace loot {

enum class Kind : uint8_t {
    Block = 0, // placeable voxel; never stacks
    Tool  = 1, // never stacks, never places
    Item  = 2  // props; stack size comes from the item table
};

struct ItemDef {
    Kind kind = Kind::Block;
    uint8_t maxStack = 1;
    int8_t wear = -1; // wear::Slot, or -1 if it cannot be worn
};

struct DropSpec {
    uint8_t item = AIR;
    uint8_t count = 1;
};

const ItemDef& itemDef(uint8_t id);
inline uint8_t maxStack(uint8_t id) {
    uint8_t n = itemDef(id).maxStack;
    return n < 1 ? 1 : n;
}
inline int itemWear(uint8_t id) { return (int)itemDef(id).wear; }
inline bool isTool(uint8_t id) { return itemDef(id).kind == Kind::Tool; }
inline bool isStackableItem(uint8_t id) {
    return itemDef(id).kind == Kind::Item && itemDef(id).maxStack > 1;
}

// True = 采集型破坏 (correct tool). False = 完全破坏 (no drops).
bool isHarvestBreak(uint8_t block, uint8_t heldTool);
// Writes drop specs for a harvest break. Returns how many entries were written.
int harvestDrops(uint8_t block, uint8_t heldTool, DropSpec* out, int maxOut);

// 破坏进程（表驱动）。每次尝试落地时套用：
//   耐久 -= 效率 * max(工具硬度 - 方块硬度, 0) * 抵抗
// 抵抗：对口工具 / 空手 = 1；拿着不匹配的工具 = wrongResist（惩罚）。
// 硬度差 <= 0 则本次不掉耐久；耐久归 0 才完成破坏。
// 前后摇只约束尝试节奏（玩家状态，连点无法跳过），不改公式。
// 草皮与泥土分开结算，且只有完全破坏（无掉落）。
struct BreakStats {
    float hardness = 0.0f;
    float durability = 1.0f;  // 方块最大耐久；工具表里忽略此项
    float efficiency = 0.0f;  // 仅工具：破坏效率
    float chargeSec = 0.5f;     // 前摇（工具/空手）；低于 0.5 按 0.5
    float cooldownSec = 0.5f;   // 后摇（工具/空手）；低于 0.5 按 0.5
};
BreakStats blockBreak(uint8_t block);
BreakStats toolBreak(uint8_t held); // 非工具 = 徒手
BreakStats handBreak();
// 单次尝试扣的耐久。0 表示无法推进。
float mineResist(uint8_t block, uint8_t held); // 1 = 无惩罚
float mineDamage(uint8_t block, uint8_t held);
int mineHits(uint8_t block, uint8_t held); // 由公式反推的尝试次数（仅预览）；0 = 无法破坏
inline float mineRate(uint8_t block, uint8_t held) { return mineDamage(block, held); }

inline constexpr float kDefaultWrongResist = 0.20f;
BreakStats sodBreak();
float sodMineResist(uint8_t held);
float sodMineDamage(uint8_t held);

inline constexpr float kMineTimingFloor = 0.5f;
inline float clampMineTiming(float t) {
    return t < kMineTimingFloor ? kMineTimingFloor : t;
}
float mineChargeSec(uint8_t held);
float mineCooldownSec(uint8_t held);

// Mining crack overlay (data-pack). Each progress step draws one new random
// polyline; `folds` is how many corners that new line has (0 = straight).
inline constexpr int kDefaultCrackFolds = 2;
inline constexpr uint8_t kDefaultCrackR = 18;
inline constexpr uint8_t kDefaultCrackG = 16;
inline constexpr uint8_t kDefaultCrackB = 14;
inline constexpr int kCrackSteps = 8;
inline constexpr int kMaxCrackFolds = 8;
// Length / width ~ N(μ0 + μ1 * (hitStep / maxDurability), σ),
// then truncated to z ∈ [−k, +k] so Box-Muller tails cannot explode.
// hitStep is that hit's formula decrement; σ is fixed so the
// Gaussian peak height stays constant.
inline constexpr float kCrackLenMu0 = -0.10f;
inline constexpr float kCrackLenMu1 = 3.50f;
inline constexpr float kCrackLenSigma = 0.07f;
inline constexpr float kCrackWidMu0 = -0.0040f;
inline constexpr float kCrackWidMu1 = 0.110f;
inline constexpr float kCrackWidSigma = 0.0020f;
inline constexpr float kCrackGaussZAbs = 2.0f; // |z| cap (standard deviations)
struct CrackStyle {
    int folds = kDefaultCrackFolds;
    uint8_t r = kDefaultCrackR, g = kDefaultCrackG, b = kDefaultCrackB;
};
CrackStyle blockCrack(uint8_t block);
void setBlockCrack(uint8_t id, CrackStyle s);
inline int clampCrackFolds(int n) {
    if (n < 0) return 0;
    if (n > kMaxCrackFolds) return kMaxCrackFolds;
    return n;
}

// 采集规则查询 / 写入（数据包与编辑器使用）。
struct HarvestRule {
    bool used = false;
    uint32_t needTags = 0;
    DropSpec drops[4]{};
    int nDrops = 0;
    float wrongResist = kDefaultWrongResist; // 错误工具乘到公式上
};
HarvestRule harvestRule(uint8_t block);

// 运行时表（启动时 resetBuiltin，再由数据包覆盖）。
void resetBuiltin();
void setItemDef(uint8_t id, ItemDef def);
void setBlockBreak(uint8_t id, BreakStats s);
void setToolBreak(uint8_t id, BreakStats s);
void setHand(BreakStats s);
void setSod(BreakStats s, uint32_t needTags, float wrongResist);
void setTags(uint8_t id, uint32_t tags);
void setHarvest(uint8_t block, const HarvestRule& rule);
void clearHarvest(uint8_t block);

struct Drop {
    Vec3 pos{ 0, 0, 0 };
    Vec3 vel{ 0, 0, 0 };
    float yaw = 0.0f;
    float spin = 0.0f;
    float age = 0.0f;
    uint8_t item = AIR;
    uint8_t count = 1;
    bool grounded = false;
};

bool rayAabb(const Vec3& origin, const Vec3& dir, const Vec3& mn, const Vec3& mx, float& tHit);

} // namespace loot
