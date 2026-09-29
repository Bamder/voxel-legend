#include "ritual.hpp"

namespace ritual {
namespace {

struct Recipe {
    const char* name;
    int a, b, c;
    const char* lines[8];
};

const Recipe kRecipes[kRitualCount] = {
    { "元素之源", ElemCore, PrimFire, StillWater, {
        "我是还没分开的火与水。",
        "把元素核心带到我的圆坛上。",
        "让原始之火与静滞之水在那里重新相认。",
        "三者齐了，我才会醒来。",
        nullptr
    } },
    { "世界树", ElemCore, LifeSprout, RootWeave, {
        "根还在土里等。",
        "把元素核心、生命嫩枝和根须织毯放到我的坛上。",
        "嫩枝会记得怎么长，织毯会记得怎么抓土。",
        "我从坛心再长出来。",
        nullptr
    } },
    { "世界主神", JudgeScale, GoldCrown, JudgeTome, {
        "裁决还缺秤、冠与书。",
        "把裁决天平、黄金冠冕和审判之书呈到我的坛前。",
        "称过、戴过、读过，我才承认这一局。",
        nullptr
    } },
    { "溯时之蛇", JudgeScale, ScaleGlass, CycleMark, {
        "时间从我的鳞上漏下去。",
        "带来裁决天平、鳞片沙漏和循环刻印。",
        "放到我盘着的圆坛。",
        "沙漏转满一圈，我便咬住自己的尾巴。",
        nullptr
    } },
    { "旧神残魂", AbyssPrism, AncientTotem, EchoRune, {
        "……嗯……深渊……棱镜……",
        "……图腾……还在……",
        "……符石……响……放到……坛上……",
        "……别……让光……进来……",
        nullptr
    } },
    { "外神", AbyssPrism, OldBones, Eyeless, {
        "咿呀——克索斯。深渊棱镜要亮。",
        "呜噜。旧神骸骨还热。",
        "希里呵。无目雕像转过来。",
        "三者放到圆坛，门就开一条缝。",
        "呀啊——别看缝里。",
        nullptr
    } },
};

int g_teamRitual[4] = { -1, -1, -1, -1 };
bool g_rolled = false;

bool holds(const ItemSlot* inv, int slots, int relic) {
    uint8_t id = (uint8_t)blockId(relic);
    for (int i = 0; i < slots; i++) {
        if (!inv[i].empty() && inv[i].block == id && inv[i].count > 0) return true;
    }
    return false;
}

} // namespace

const char* ritualName(int index) {
    if (index < 0 || index >= kRitualCount) return "";
    return kRecipes[index].name;
}

const char* relicName(int relic) {
    if (relic < 0 || relic >= RelicCount) return "";
    return blockOf((uint8_t)blockId(relic)).name;
}

int assignedRitual(int team) {
    if (!g_rolled || team < 1 || team > 4) return -1;
    return g_teamRitual[team - 1];
}

void recipeRelics(int ritual, int out[3]) {
    if (!out) return;
    out[0] = out[1] = out[2] = -1;
    if (ritual < 0 || ritual >= kRitualCount) return;
    out[0] = kRecipes[ritual].a;
    out[1] = kRecipes[ritual].b;
    out[2] = kRecipes[ritual].c;
}

bool recipeReady(int ritual, const ItemSlot* inv, int slots) {
    if (!inv || ritual < 0 || ritual >= kRitualCount) return false;
    const Recipe& r = kRecipes[ritual];
    return holds(inv, slots, r.a) && holds(inv, slots, r.b) && holds(inv, slots, r.c);
}

int storyLineCount(int ritual) {
    if (ritual < 0 || ritual >= kRitualCount) return 0;
    int n = 0;
    while (n < 8 && kRecipes[ritual].lines[n]) n++;
    return n;
}

const char* storyLine(int ritual, int line) {
    if (line < 0 || line >= storyLineCount(ritual)) return "";
    return kRecipes[ritual].lines[line];
}

void roll(uint32_t seed) {
    uint32_t rng = seed ? seed : 1u;
    int order[kRitualCount];
    for (int i = 0; i < kRitualCount; i++) order[i] = i;
    for (int i = kRitualCount - 1; i > 0; i--) {
        rng = rng * 1664525u + 1013904223u;
        int j = (int)((rng >> 16) % (uint32_t)(i + 1));
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    for (int i = 0; i < 4; i++) g_teamRitual[i] = order[i];
    g_rolled = true;
}

bool relicSpawned(int relic) {
    (void)relic;
    // Legacy relic platforms remain harmless landmarks. Relics are no longer
    // stamped into the map; the authoritative Boss reward path creates them.
    return false;
}

} // namespace ritual
