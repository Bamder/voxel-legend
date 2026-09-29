#pragma once
#include <cstdint>
#include <vector>

namespace tex {
    constexpr int TILE = 64;
    constexpr int COLS = 8;
    constexpr int ROWS = 8;
    constexpr int ATLAS_W = TILE * COLS;
    constexpr int ATLAS_H = TILE * ROWS;

    // Fills `out` with ATLAS_W * ATLAS_H * 4 bytes (RGBA8), top-down order.
    void generateAtlas(std::vector<uint8_t>& out);

    // Generate one tile's RGBA (TILE*TILE*4) — used to bootstrap material image files.
    void generateTileRGBA(int tile, std::vector<uint8_t>& out);

    // Overwrite one TILE x TILE tile inside an atlas buffer with an RGBA image.
    void overwriteTile(int tile, const uint8_t* rgba, std::vector<uint8_t>& atlas);

    // UVs for a tile. (u0,v0) is the tile's TOP-LEFT in the image, (u1,v1) bottom-right.
    void tileUV(int tileId, float& u0, float& v0, float& u1, float& v1);

    // Player body sheet: box unwrap, independent hand/foot/arm caps.
    void generatePlayerSkin(std::vector<uint8_t>& out);
    // Eye cutout (8x8). Mouth variants: 0 closed, 1 open, 2 smile (16x8).
    void generatePlayerEye(std::vector<uint8_t>& out);
    void generatePlayerEyelid(std::vector<uint8_t>& out);
    void generatePlayerMouth(std::vector<uint8_t>& out, int variant);
}
