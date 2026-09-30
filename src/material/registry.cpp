#include "registry.hpp"
#include "image.hpp"
#include "../render/textures.hpp"
#include "../world/animation.hpp"
#include "../world/asset_pack.hpp"
#include "../world/player_skin.hpp"
#include "../world/vitals.hpp"
#include "../plugin/plugin.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace mat {

Image g_tileImages[TEX_COUNT];
Material g_grassTuft;
Material g_handAxe;
Material g_handPick;
Material g_handShovel;
Material g_shears;
Model g_itemModels[256];

static const char* kTileNames[TEX_COUNT] = {
    "dirt", "grass_top", "grass_side", "stone", "sand", "water",
    "log_side", "log_top", "leaves", "gravel", "snow", "glass",
    "bedrock", "coal", "iron", "gold", "diamond", "planks",
    "cobble", "brick", "sandstone",
    "sod_0", "sod_1", "sod_2", "sod_3",
    "shrub_stem", "shrub_leaf", "grass_tuft", "leaf_x", "shrub_leaf_x",
    "bark", "hand_axe", "wood_side",
    "shears", "hand_pick", "hand_shovel",
    "crack", "core",
    "timber", "plaster", "thatch", "clay_tile", "ashlar",
    "torch_wood", "flame", "lantern", "lantern_glow",
};

const char* tileName(int tile) {
    if (tile < 0 || tile >= TEX_COUNT || !kTileNames[tile]) return "?";
    return kTileNames[tile];
}
const char* blockName(int block) {
    const char* id = plugin::blockId((uint8_t)block);
    return (id && id[0]) ? id : "?";
}
int tileIndex(const char* name) {
    if (!name || !name[0]) return -1;
    for (int t = 0; t < TEX_COUNT; t++) {
        if (kTileNames[t] && std::strcmp(kTileNames[t], name) == 0) return t;
    }
    return -1;
}

static bool fileExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

static void writeTextFile(const char* path, const char* content) {
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fputs(content, f); std::fclose(f); }
}

static std::string tilePath(int tile) {
    return pack::tilePng(tileName(tile));
}
static std::string blockModelPath(int block) {
    return pack::blockModel(blockName(block));
}
static std::string blockRandPath(int block) {
    return pack::blockRand(blockName(block));
}

// A standard cube block model is symbolic ("cube"): the six faces come from the
// block's top/side/bottom textures. Blocks with custom geometry use explicit quads.
static const char* kGrassTuftModel =
    "# Grass-tuft blade: two crossed X quads (diagonal), normalized to a unit cell.\n"
    "# Corner order: bottom-left, bottom-right, top-right, top-left.\n"
    "# UV: bottom (root) -> v=1, top (tip) -> v=0; left -> u=0, right -> u=1.\n"
    "quad 0.0 0.0 0.0 0.0 1.0  1.0 0.0 1.0 1.0 1.0  1.0 1.0 1.0 1.0 0.0  0.0 1.0 0.0 0.0 0.0  double\n"
    "quad 1.0 0.0 0.0 0.0 1.0  0.0 0.0 1.0 1.0 1.0  0.0 1.0 1.0 1.0 0.0  1.0 1.0 0.0 0.0 0.0  double\n";

static const char* kGrassTuftRand =
    "# Grass tuft material randomization parameters.\n"
    "R 16\n"
    "wb_factor 0.5\n"
    "tall_base 0.375\n"
    "tall_range 0.375\n"
    "short_base 0.1875\n"
    "short_range 0.1875\n"
    "density_mean 0.60\n"
    "density_std 0.08\n"
    "density_min 0.50\n"
    "density_max 0.70\n"
    "cluster_freq 0.22\n"
    "shade_min 0.85\n"
    "shade_max 1.15\n";

static void bootstrap() {
    std::filesystem::create_directories(pack::tilesDir());
    std::filesystem::create_directories(pack::blocksDir());
    std::filesystem::create_directories(pack::extrasDir());
    std::filesystem::create_directories(pack::entitiesDir());

    // One image file per texture tile (procedural bootstrap).
    for (int t = 0; t < TEX_COUNT; t++) {
        std::string p = tilePath(t);
        if (!fileExists(p.c_str())) {
            std::vector<uint8_t> rgba;
            tex::generateTileRGBA(t, rgba);
            savePNG(p.c_str(), tex::TILE, tex::TILE, rgba.data());
        }
    }

    // One model + rand file per block.
    for (int b = 0; b < liveBlockCount(); b++) {
        std::string mp = blockModelPath(b);
        if (!fileExists(mp.c_str())) {
            writeTextFile(mp.c_str(), (b == GRASS_TUFT) ? kGrassTuftModel : "cube\n");
        }
        std::string rp = blockRandPath(b);
        if (!fileExists(rp.c_str())) {
            writeTextFile(rp.c_str(), (b == GRASS_TUFT) ? kGrassTuftRand : "# no randomization\n");
        }
    }
}

void storeItemModel(uint8_t block, const Model& m) {
    g_itemModels[block] = m;
    switch (block) {
        case GRASS_TUFT: g_grassTuft.model = m; break;
        case HAND_AXE: g_handAxe.model = m; break;
        case HAND_PICK: g_handPick.model = m; break;
        case HAND_SHOVEL: g_handShovel.model = m; break;
        case SHEARS: g_shears.model = m; break;
        default: break;
    }
}

// Gray standing body, same cuts as the placed training dummy (no hair, face,
// cutouts, or cloth). Fitted into the unit cube so the icon, the held prop,
// and the ground drop all draw this miniature instead of the snow cube.
static void installTargetFigurine() {
    if (ITEM_TARGET >= liveBlockCount()) return;
    const anim::PlayerClips& lib = anim::playerClips();
    std::vector<pm::Part> parts = lib.rest;
    if (!lib.idle.bones.empty())
        parts = anim::poseParts(lib.rest, lib.idle, 0.0f);

    const Vec3 gray{ 0.62f, 0.62f, 0.64f };
    const Vec3 paper = vitals::healthColor(1.0f);
    std::vector<pm::Part> body;
    body.reserve(parts.size());
    for (pm::Part& p : parts) {
        if (pm::isHairPart(p) || pm::isHairCardPart(p)) continue;
        if (pm::isEyePart(p) || pm::isEyelidPart(p) || pm::isMouthPart(p)) continue;
        if (pm::isCutoutPart(p) || pm::isDecalPart(p)) continue;
        if (p.kind == "cloth") continue;
        if (p.half.x < 1e-5f && p.half.y < 1e-5f && p.half.z < 1e-5f) continue;
        int id = vitals::limbFromName(p.name);
        if (id < 0) id = vitals::limbFromName(p.kind);
        p.color = (id >= 0) ? paper : gray;
        body.push_back(std::move(p));
    }
    if (body.empty()) return;

    float minx = 1e9f, miny = 1e9f, minz = 1e9f;
    float maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;
    const float sx[8] = { -1, 1, 1, -1, -1, 1, 1, -1 };
    const float sy[8] = { -1, -1, -1, -1, 1, 1, 1, 1 };
    const float sz[8] = { -1, -1, 1, 1, -1, -1, 1, 1 };
    for (const pm::Part& p : body) {
        for (int i = 0; i < 8; i++) {
            Vec3 o = pm::rotateEuler({ sx[i] * p.half.x, sy[i] * p.half.y, sz[i] * p.half.z }, p.rot);
            Vec3 w = p.center + o;
            minx = std::min(minx, w.x); miny = std::min(miny, w.y); minz = std::min(minz, w.z);
            maxx = std::max(maxx, w.x); maxy = std::max(maxy, w.y); maxz = std::max(maxz, w.z);
        }
    }
    float span = std::max(maxx - minx, std::max(maxy - miny, maxz - minz));
    if (span < 1e-4f) return;
    const float fit = 0.82f / span;
    const Vec3 mid{ (minx + maxx) * 0.5f, (miny + maxy) * 0.5f, (minz + maxz) * 0.5f };
    const Vec3 slot{ 0.5f, 0.45f, 0.5f };

    Model figurine;
    figurine.cube = false;
    figurine.solids.reserve(body.size());
    for (const pm::Part& p : body) {
        Vec3 c = (p.center - mid) * fit + slot;
        Vec3 h = p.half * fit;
        Solid s;
        s.c[0] = c.x; s.c[1] = c.y; s.c[2] = c.z;
        s.h[0] = h.x; s.h[1] = h.y; s.h[2] = h.z;
        s.rgb[0] = p.color.x; s.rgb[1] = p.color.y; s.rgb[2] = p.color.z;
        s.rot[0] = p.rot.x; s.rot[1] = p.rot.y; s.rot[2] = p.rot.z;
        figurine.solids.push_back(s);
    }
    g_itemModels[ITEM_TARGET] = std::move(figurine);
}

void initMaterials() {
    bootstrap();
    for (int t = 0; t < TEX_COUNT; t++) {
        g_tileImages[t] = loadPNG(tilePath(t).c_str());
    }
    for (int b = 0; b < 256; b++) g_itemModels[b] = {};
    for (int b = 0; b < liveBlockCount(); b++) {
        g_itemModels[b] = loadModel(blockModelPath(b).c_str());
    }
    g_grassTuft.image = g_tileImages[TEX_GRASS_TUFT];
    g_grassTuft.model = g_itemModels[GRASS_TUFT];
    g_grassTuft.rand = loadRand(blockRandPath(GRASS_TUFT).c_str());
    g_handAxe.image = g_tileImages[TEX_HAND_AXE];
    g_handAxe.model = g_itemModels[HAND_AXE];
    g_handAxe.rand = loadRand(blockRandPath(HAND_AXE).c_str());
    g_handPick.image = g_tileImages[TEX_HAND_PICK];
    g_handPick.model = g_itemModels[HAND_PICK];
    g_handPick.rand = loadRand(blockRandPath(HAND_PICK).c_str());
    g_handShovel.image = g_tileImages[TEX_HAND_SHOVEL];
    g_handShovel.model = g_itemModels[HAND_SHOVEL];
    g_handShovel.rand = loadRand(blockRandPath(HAND_SHOVEL).c_str());
    g_shears.image = g_tileImages[TEX_SHEARS];
    g_shears.model = g_itemModels[SHEARS];
    g_shears.rand = loadRand(blockRandPath(SHEARS).c_str());
    installTargetFigurine();
}

} // namespace mat
