#pragma once
#include "blocks.hpp"
#include <cstdint>

// Six rituals, three props each. Relics enter the match through authoritative
// Boss rewards; ritual recipes and altar placement remain unchanged.
// Each combat team is assigned a different recipe. Place its three items on that altar's triangle.
namespace ritual {

enum Relic : int {
    ElemCore = 0,
    PrimFire,
    StillWater,
    LifeSprout,
    RootWeave,
    JudgeScale,
    GoldCrown,
    JudgeTome,
    ScaleGlass,
    CycleMark,
    AbyssPrism,
    AncientTotem,
    EchoRune,
    OldBones,
    Eyeless,
    RelicCount
};

inline constexpr int kRitualCount = 6;

inline int blockId(int relic) {
    return (int)ITEM_ELEM_CORE + relic;
}

const char* ritualName(int index);
const char* relicName(int relic);
int assignedRitual(int team); // combat team 1..4, or -1
void recipeRelics(int ritual, int out[3]);
bool recipeReady(int ritual, const ItemSlot* inv, int slots);
int storyLineCount(int ritual);
const char* storyLine(int ritual, int line);

void roll(uint32_t seed);
bool relicSpawned(int relic);

} // namespace ritual
