#pragma once
#include "asset_pack.hpp"
#include "blocks.hpp"
#include "player_model.hpp"
#include "player_skin.hpp"
#include "hair_voxels.hpp"
#include <string>
#include <vector>

// Worn clothing. The inventory body panel draws the slots that are open.
// Hat / upper / lower / shoes are the ones enabled right now.
namespace wear {

enum Slot : int {
    Hat = 0,    // 帽
    Upper = 1,  // 上衣
    Lower = 2,  // 下裳
    Shoes = 3,  // 鞋履
    Count = 4
};

inline constexpr int kOpenSlots[] = { Hat, Upper, Lower, Shoes };
inline constexpr int kOpenCount = 4;

inline bool isOpen(int slot) {
    for (int i = 0; i < kOpenCount; i++)
        if (kOpenSlots[i] == slot) return true;
    return false;
}

inline const char* id(int slot) {
    switch (slot) {
        case Hat: return "hat";
        case Upper: return "upper";
        case Lower: return "lower";
        case Shoes: return "shoes";
        default: return "";
    }
}

inline const char* label(int slot) {
    switch (slot) {
        case Hat: return "帽";
        case Upper: return "上衣";
        case Lower: return "下裳";
        case Shoes: return "鞋履";
        default: return "";
    }
}

struct GarmentAsset {
    std::vector<pm::Part> parts;
    int sheetW = 0, sheetH = 0;
    std::string png;
};

inline const GarmentAsset& garmentAsset(uint8_t item) {
    static GarmentAsset shirt, shorts, shoes, empty;
    static bool ready = false;
    if (!ready) {
        auto load = [](const char* stem, GarmentAsset& g) {
            std::string path = pack::join(pack::entitiesDir(), std::string("clothes/") + stem + ".model");
            pm::EntityFile ef = pm::loadEntity(path.c_str());
            g.parts = std::move(ef.parts);
            pm::shellClothParts(g.parts);
            g.sheetW = ef.skinW;
            g.sheetH = ef.skinH;
            pm::assignModelSheet(g.parts, g.sheetW, g.sheetH);
            g.png = pack::join(pack::entitiesDir(), std::string("clothes/") + stem + ".png");
        };
        load("shirt", shirt);
        load("shorts", shorts);
        load("shoes", shoes);
        ready = true;
    }
    if (item == SHIRT) return shirt;
    if (item == SHORTS) return shorts;
    if (item == SHOES) return shoes;
    return empty;
}

inline const std::vector<pm::Part>& garment(uint8_t item) {
    return garmentAsset(item).parts;
}

inline int fromId(const std::string& s) {
    if (s == "hat" || s == "cap") return Hat;
    if (s == "upper" || s == "shirt" || s == "top") return Upper;
    if (s == "lower" || s == "pants") return Lower;
    if (s == "shoes" || s == "boots") return Shoes;
    return -1;
}

} // namespace wear
