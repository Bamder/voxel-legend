#include "guide.hpp"

namespace guide {
namespace {

struct Page {
    const char* title;
    const char* lines[kMaxLines];
};

constexpr Page kPages[kPageCount] = {
    { "1. 基础移动和背包", {
        "W/A/S/D 移动，空格跳跃，Ctrl 冲刺，Shift 下蹲。",
        "E 打开背包；移动时只能整理快捷栏，背包主体会锁定。",
        "F 拾取准星指向的地面物品。",
        "屏幕左上角显示方块坐标和区块坐标，可用于会合。",
        "头、胸或核心归零会死亡；受损肢体会影响行动。",
        nullptr, nullptr } },
    { "2. 左右手槽和武器", {
        "快捷栏前 3 格属于左手，后 3 格属于右手。",
        "数字键 1-3 选择左手物品，4-6 选择右手物品。",
        "鼠标滚轮循环右手物品。",
        "左键瞄准敌方玩家会发动近战；瞄准方块仍执行采集。",
        "一只手失能会封印该手的 3 个槽位。",
        nullptr, nullptr } },
    { "3. 阵营玩法", {
        "所有阵营共享同一张由房间种子生成的地图。",
        "同阵营玩家不会受到你的直接伤害，敌对阵营可以 PvP。",
        "部署时选择出生点；队友部署标记会同步显示。",
        "利用左上角坐标与队友会合，并共同推进线索。",
        "线索进度按阵营共享，一名队友发现后全队可查看。",
        nullptr, nullptr } },
    { "4. 圣物与祭坛", {
        "圣物不再是普通建筑散落物，而是 Boss 的权威战利品。",
        "每个阵营会被分配一场仪式和对应的三件圣物。",
        "击败线索链终点的 Boss 后，服务器才会生成圣物。",
        "背包左上角的“笔记”可查看本队仪式与圣物状态。",
        "圣物只能放到本队仪式祭坛的三角区域。",
        nullptr, nullptr } },
    { "5. 如何完成仪式", {
        "在建筑中寻找线索，并按线索坐标前往下一座建筑。",
        "途中建筑可能提供新线索，也可能提供奥术奖励。",
        "奥术奖励节点仍应保留下一条线索，路线不会无故中断。",
        "每条线索链最终指向 Boss；击败后取得一件仪式圣物。",
        "继续追踪并集齐笔记中的三件圣物，再放入祭坛三角区。",
        nullptr, nullptr } },
    { "6. 火球 / 冰封 / 治疗", {
        "选中奥术物品后按右键使用；多人结果由服务器判定。",
        "火球爆炸并造成燃烧，适合范围压制；冷却 3 秒。",
        "冰封造成 8% 全身伤害，5 秒内不能移动、跳跃或冲刺。",
        "治疗准星所指的受伤队友，否则治疗自己；冷却 12 秒。",
        "治疗使所有身体部位恢复 25%，不能复活或超过上限。",
        "奥术有独立冷却；客户端不能自行声明命中或状态。",
        nullptr } },
    { "7. 普通武器与 Boss 神器", {
        "Hand Pick 对玩家更有效：每次约 14%，Boss 倍率 0.35。",
        "Hand Axe 是重型神器：对玩家约 10%，Boss 倍率 2.5。",
        "两种工具都保留原有采集能力。",
        "攻击有前摇、命中时刻和冷却，连续点击不能跳过判定。",
        "Boss 尚未生成前，倍率接口不会凭空创建 Boss。",
        nullptr, nullptr } },
    { "8. 基本操作键位", {
        "移动 W/A/S/D；跳跃 Space；冲刺 Ctrl；下蹲 Shift。",
        "背包 E；拾取 F；视角 F5；暂停/关闭页面 Esc。",
        "左手槽 1/2/3；右手槽 4/5/6；滚轮切换右手。",
        "左键攻击或采集；右键使用奥术、指南、线索或放置。",
        "指南和线索不会消耗，可随时再次打开查看。",
        nullptr, nullptr } },
};

} // namespace

const char* pageTitle(int page) {
    return page >= 0 && page < kPageCount ? kPages[page].title : "新手指南";
}

int pageLineCount(int page) {
    if (page < 0 || page >= kPageCount) return 0;
    int count = 0;
    while (count < kMaxLines && kPages[page].lines[count]) ++count;
    return count;
}

const char* pageLine(int page, int line) {
    if (page < 0 || page >= kPageCount || line < 0 || line >= kMaxLines) return "";
    const char* value = kPages[page].lines[line];
    return value ? value : "";
}

} // namespace guide
