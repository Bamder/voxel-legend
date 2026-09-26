#pragma once
#include "material.hpp"
#include "../world/blocks.hpp"

namespace mat {

// Loaded tile images (indexed by TexId); empty when the file is missing.
extern Image g_tileImages[TEX_COUNT];

// Special per-block materials (image + model + rand). Only blocks with custom
// geometry need one; standard cubes reuse the tile images + a "cube" model.
extern Material g_grassTuft;
extern Material g_handAxe;
extern Material g_handPick;
extern Material g_handShovel;
extern Material g_shears;

inline const Material* toolMaterial(uint8_t block) {
    switch (block) {
        case HAND_AXE: return &g_handAxe;
        case HAND_PICK: return &g_handPick;
        case HAND_SHOVEL: return &g_handShovel;
        case SHEARS: return &g_shears;
        default: return nullptr;
    }
}

// Per-id item/block models (cube or explicit quads). Used for GUI icons,
// held items, and the item model editor's left-panel thumbnails.
extern Model g_itemModels[256];
inline const Model& itemModel(uint8_t block) {
    return g_itemModels[block];
}
void storeItemModel(uint8_t block, const Model& m);

// Bootstrap any missing material files and load them all.
void initMaterials();

// Human-readable file names used by the material editor / bootstrap.
const char* tileName(int tile);
const char* blockName(int block);
int tileIndex(const char* name);

} // namespace mat
