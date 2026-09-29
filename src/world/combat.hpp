#pragma once
#include "blocks.hpp"
#include "loot.hpp"
#include <cmath>

// Hit resolution.
// 受伤 = max(每击物理 - 目标装甲 - 攻击者物理抵抗, 0)
//        * (基础攻击倍率 + 力量增幅 + 目标物理易伤)
//      + max(每击神秘学 - 攻击者神秘学抵抗, 0)
//        * (基础攻击倍率 + 目标物理易伤)
namespace combat {

struct Attacker {
    float physHit = 0;      // 每击物理伤害
    float occultHit = 0;    // 每击神秘学伤害
    float physResist = 0;   // 物理抵抗
    float occultResist = 0; // 神秘学抵抗
    float baseAtk = 1;      // 基础攻击倍率
    float strAmp = 0;       // 力量增幅
};

struct Target {
    float armor = 0;    // 装甲值
    float physVuln = 0; // 物理易伤倍率
};

inline float hurt(const Attacker& a, const Target& t) {
    float phys = a.physHit - t.armor - a.physResist;
    if (phys < 0.0f) phys = 0.0f;
    float occult = a.occultHit - a.occultResist;
    if (occult < 0.0f) occult = 0.0f;
    return phys * (a.baseAtk + a.strAmp + t.physVuln)
         + occult * (a.baseAtk + t.physVuln);
}

inline int hurtInt(const Attacker& a, const Target& t) {
    float h = hurt(a, t);
    if (h <= 0.0f) return 0;
    return (int)std::lround(h);
}

// Axe 15 physical. Other tools 5 physical. Empty hand and non-tools: 2 physical, 1 occult.
inline Attacker strikeOf(uint8_t held) {
    Attacker a;
    if (held != AIR && hasItemTags(held, TAG_AXE)) {
        a.physHit = 15.0f;
    } else if (held != AIR && loot::isTool(held)) {
        a.physHit = 5.0f;
    } else {
        a.physHit = 2.0f;
        a.occultHit = 1.0f;
    }
    return a;
}

} // namespace combat
