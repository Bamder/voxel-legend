#include "textures.hpp"
#include "../world/blocks.hpp"
#include "../world/player_skin.hpp"
#include "../core/noise.hpp"
#include <algorithm>
#include <cmath>

namespace tex {

constexpr int TS = TILE / 16; // scale factor relative to the base 16-px tile

struct RGB { float r, g, b; };

static inline RGB mix(RGB a, RGB b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static inline uint8_t cl(float v) {
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)v;
}

static inline float rnd(int x, int y, uint32_t seed) {
    return noise::hash01(noise::hash2(x, y, seed));
}

// Shared leaf-frill alpha: a wide root that curves into a rounded arc head, with
// wobbly edges, cutout holes, and isolated pixel vertices near the head.
// The cutout holes reuse the source leaf block's gap size (2x2 texels) and
// sparseness threshold so the frill matches its host block.
static uint8_t leafFrillAlpha(int px, int py, uint32_t tile, uint32_t cutSeed, float cutThresh) {
    float t = (float)py / (float)(TILE - 1); // 0 = head (outward), 1 = root (leaf face)
    float center = (float)(TILE / 2);
    float dist = std::fabs((float)px - center);

    float wf = 0.10f + 0.40f * t;              // 0.10 (head) .. 0.50 (root)
    if (t < 0.25f) wf *= std::sin((t / 0.25f) * 1.5708f); // rounded arc head
    float wob = noise::noise2((float)px * 0.10f, (float)py * 0.12f, 0x1EAFu + tile);
    float halfW = wf * (float)TILE * (0.72f + 0.28f * wob);

    uint8_t a = 0;
    if (dist < halfW) {
        a = 255;
        if (rnd(px / 2, py / 2, cutSeed + tile) > cutThresh) a = 0;
    }
    if (t < 0.4f && rnd(px, py, 0x3EAFu + tile) > 0.972f) a = 255;
    return a;
}

static void pixelColor(int tile, int px, int py, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    float n = rnd(px, py, (uint32_t)tile * 31337u + 7u);
    RGB c;
    float alpha = 255.0f;

    switch (tile) {
        case TEX_DIRT: {
            RGB base{ 134.0f, 96.0f, 67.0f };
            float v = 0.82f + 0.36f * n;
            c = { base.r * v, base.g * v, base.b * v };
            break;
        }
        case TEX_GRASS_TOP: {
            float v = 0.82f + 0.36f * n;
            c = { 74.0f * v, 140.0f * v, 54.0f * v };
            break;
        }
        case TEX_GRASS_SIDE: {
            // jagged grass strip on top, dirt below
            float jag = rnd(px, 0, (uint32_t)tile * 977u + 3u);
            int cutoff = 3 * TS + (int)(jag * (2 * TS));
            if (py <= cutoff) {
                float v = 0.82f + 0.36f * n;
                c = { 74.0f * v, 140.0f * v, 54.0f * v };
            } else {
                RGB base{ 134.0f, 96.0f, 67.0f };
                float v = 0.82f + 0.36f * n;
                c = { base.r * v, base.g * v, base.b * v };
            }
            break;
        }
        case TEX_STONE: {
            float v = 0.78f + 0.44f * n;
            c = { 130.0f * v, 130.0f * v, 132.0f * v };
            break;
        }
        case TEX_SAND: {
            float v = 0.86f + 0.28f * n;
            c = { 220.0f * v, 205.0f * v, 160.0f * v };
            break;
        }
        case TEX_WATER: {
            float wave = 0.9f + 0.2f * std::sin((float)py * (1.3f / TS)) * n;
            c = { 40.0f * wave, 70.0f * wave, 200.0f * wave };
            break;
        }
        case TEX_LOG_SIDE: {
            // Vertical bark wrap: ridges, cork pits, occasional cracks.
            float fx = (float)px / (float)(TILE - 1);
            float fy = (float)py / (float)(TILE - 1);
            float ridge = 0.55f + 0.45f * std::sin(fx * 18.0f + 0.4f * std::sin(fy * 6.0f));
            float grain = 0.82f + 0.18f * rnd(px / (1 * TS), py / (3 * TS), 551u);
            float pit = rnd(px / (2 * TS), py / (2 * TS), 773u);
            float crack = (rnd(px, py / (4 * TS), 991u) > 0.97f) ? 0.72f : 1.0f;
            float moss = (rnd(px / (3 * TS), py / (5 * TS), 337u) > 0.92f) ? 1.0f : 0.0f;
            RGB bark{ 92.0f, 62.0f, 36.0f };
            RGB dark{ 58.0f, 38.0f, 22.0f };
            RGB cork{ 120.0f, 88.0f, 52.0f };
            float t = ridge * grain * crack;
            RGB c0 = mix(dark, bark, std::min(1.0f, t));
            if (pit > 0.78f) c0 = mix(c0, cork, (pit - 0.78f) / 0.22f);
            if (moss > 0.5f) c0 = mix(c0, RGB{ 48.0f, 72.0f, 32.0f }, 0.22f);
            c = c0;
            break;
        }
        case TEX_LOG_TOP: {
            // Concentric rings around tile center so 2x2 quadrants splice into one ring.
            float cx = (float)px - (float)(TILE - 1) * 0.5f;
            float cy = (float)py - (float)(TILE - 1) * 0.5f;
            float d = std::sqrt(cx * cx + cy * cy);
            float maxR = (float)TILE * 0.50f;
            float rn = d / maxR;
            float ang = std::atan2(cy, cx);
            float warp = 0.035f * std::sin(ang * 3.0f + 1.1f) + 0.02f * rnd((int)(ang * 8.0f), (int)d, 404u);
            float dr = rn + warp;
            float ring = 0.5f + 0.5f * std::sin(dr * 42.0f);
            float pith = std::exp(-d * d / (float)(6 * TS * 6 * TS));
            RGB heart{ 186.0f, 142.0f, 78.0f };
            RGB late{ 142.0f, 102.0f, 52.0f };
            RGB pithC{ 210.0f, 170.0f, 100.0f };
            RGB barkRim{ 78.0f, 52.0f, 30.0f };
            RGB wood = mix(late, heart, ring);
            wood = mix(wood, pithC, pith * 0.85f);
            if (dr > 0.86f) {
                float bt = (dr - 0.86f) / 0.14f;
                if (bt < 0.0f) bt = 0.0f;
                if (bt > 1.0f) bt = 1.0f;
                wood = mix(wood, barkRim, bt);
            }
            float speckle = 0.92f + 0.08f * rnd(px, py, 220u);
            c = { wood.r * speckle, wood.g * speckle, wood.b * speckle };
            break;
        }
        case TEX_LEAVES: {
            float v = 0.75f + 0.5f * n;
            c = { 30.0f * v, 110.0f * v, 30.0f * v };
            if (n > 0.86f) c = { 18.0f, 70.0f, 20.0f }; // darker patches
            // Cutout holes: larger contiguous gaps (2x2 blocks) for a sparser look.
            if (rnd(px / 2, py / 2, 0x3EAFu + (uint32_t)tile) > 0.65f) alpha = 0.0f;
            break;
        }
        case TEX_GRAVEL: {
            float v = 0.7f + 0.6f * n;
            c = { 120.0f * v, 116.0f * v, 110.0f * v };
            break;
        }
        case TEX_SNOW: {
            float v = 0.9f + 0.2f * n;
            c = { 240.0f * v, 245.0f * v, 250.0f * v };
            break;
        }
        case TEX_GLASS: {
            // transparent center, light frame
            int border = 1 * TS;
            bool edge = (px < border || py < border || px >= 15 * TS - border || py >= 15 * TS - border);
            bool streak = (px + py == 8 * TS || px == py + 2 * TS);
            if (edge || streak) {
                c = { 235.0f, 245.0f, 255.0f };
                alpha = 220.0f;
            } else {
                c = { 0.0f, 0.0f, 0.0f };
                alpha = 0.0f;
            }
            break;
        }
        case TEX_BEDROCK: {
            float v = 0.4f + 1.0f * n;
            c = { 60.0f * v, 60.0f * v, 62.0f * v };
            break;
        }
        case TEX_COAL: {
            float v = 0.78f + 0.44f * n;
            c = { 130.0f * v, 130.0f * v, 132.0f * v };
            if (rnd(px, py, 111u) > 0.93f) c = { 20.0f, 20.0f, 20.0f };
            break;
        }
        case TEX_IRON: {
            float v = 0.78f + 0.44f * n;
            c = { 130.0f * v, 130.0f * v, 132.0f * v };
            if (rnd(px, py, 222u) > 0.93f) c = { 216.0f, 180.0f, 148.0f };
            break;
        }
        case TEX_GOLD: {
            float v = 0.78f + 0.44f * n;
            c = { 130.0f * v, 130.0f * v, 132.0f * v };
            if (rnd(px, py, 333u) > 0.93f) c = { 250.0f, 214.0f, 60.0f };
            break;
        }
        case TEX_DIAMOND: {
            float v = 0.78f + 0.44f * n;
            c = { 130.0f * v, 130.0f * v, 132.0f * v };
            if (rnd(px, py, 444u) > 0.93f) c = { 80.0f, 230.0f, 240.0f };
            break;
        }
        case TEX_PLANKS: {
            int row = py / (4 * TS);
            bool seam = (py % (4 * TS) == 0);
            float grain = 0.82f + 0.36f * rnd(px, row, 777u + (uint32_t)tile);
            RGB base{ 176.0f, 132.0f, 78.0f };
            c = { base.r * grain, base.g * grain, base.b * grain };
            if (seam) c = { 130.0f, 96.0f, 54.0f };
            break;
        }
        case TEX_COBBLE: {
            float v = 0.7f + 0.5f * n;
            c = { 118.0f * v, 118.0f * v, 122.0f * v };
            float blob = rnd(px / (4 * TS), py / (4 * TS), 888u + (uint32_t)tile);
            if (blob > 0.55f) c = { c.r * 0.8f, c.g * 0.8f, c.b * 0.8f };
            break;
        }
        case TEX_BRICK: {
            int rh = 4 * TS;
            int row = py / rh;
            bool mortarH = (py % rh == 0);
            int off = (row % 2) * (4 * TS);
            int x2 = (px + off) % (16 * TS);
            bool mortarV = (x2 == 0);
            if (mortarH || mortarV) c = { 200.0f, 196.0f, 190.0f };
            else {
                float v = 0.85f + 0.3f * n;
                c = { 168.0f * v, 72.0f * v, 58.0f * v };
            }
            break;
        }
        case TEX_SANDSTONE: {
            float v = 0.86f + 0.28f * n;
            c = { 220.0f * v, 198.0f * v, 140.0f * v };
            if (py % (5 * TS) == 0) c = { c.r * 0.82f, c.g * 0.82f, c.b * 0.82f };
            break;
        }
        case TEX_SOD_0: case TEX_SOD_1: case TEX_SOD_2: case TEX_SOD_3: {
            // Grass sod (草皮): a single-face overlay. Stage 0 is healthy green;
            // later stages darken toward dry yellow-brown for the wither animation.
            int stage = tile - TEX_SOD_0;
            float t = (float)stage / 3.0f;
            RGB green{ 74.0f, 140.0f, 54.0f };
            RGB dried{ 176.0f, 138.0f, 52.0f };
            RGB base = mix(green, dried, t);
            float v = 0.82f + 0.36f * n;
            c = { base.r * v, base.g * v, base.b * v };
            break;
        }
        case TEX_SHRUB_STEM: {
            // Irregular, wavy branch strips of non-uniform thickness and spacing,
            // crossing at several angles; the polygonal gaps between them are clear.
            float grain = 0.78f + 0.44f * n;
            RGB base{ 92.0f, 64.0f, 36.0f };
            c = { base.r * grain, base.g * grain, base.b * grain };
            uint32_t seed = 0x1A2B3C4Du + (uint32_t)tile;

            // Distance to the nearest wavy branch (u = along the branch, v = across).
            auto branchDist = [&](float u, float v, uint32_t sd) {
                float spacing = 18.0f + noise::noise2(u * 0.015f, 1.7f, sd) * 14.0f; // 18..32
                float wave = noise::noise2(u * 0.045f, 3.3f, sd + 17u) * 11.0f - 5.5f;
                float k = std::floor((v - wave) / spacing);
                float center = wave + (k + 0.5f) * spacing;
                return std::fabs(v - center);
            };
            // Branch thickness varies along its length.
            auto branchThick = [&](float u, uint32_t sd) {
                return 1.0f + noise::noise2(u * 0.035f, 5.9f, sd + 31u) * 4.5f; // 1..5.5
            };

            // Two crossing branch families (diagonal + anti-diagonal).
            float u1 = (float)(px + py), v1 = (float)(px - py);
            float u2 = (float)(px - py), v2 = (float)(px + py);
            bool on = (branchDist(u1, v1, seed) < branchThick(u1, seed)) ||
                      (branchDist(u2, v2, seed + 101u) < branchThick(u2, seed + 101u));
            if (!on) alpha = 0.0f;
            break;
        }
        case TEX_SHRUB_LEAF: {
            // Slightly lighter green bush leaf with cutout holes.
            float v = 0.72f + 0.56f * n;
            c = { 36.0f * v, 108.0f * v, 32.0f * v };
            if (n > 0.85f) c = { 22.0f, 72.0f, 22.0f };
            if (rnd(px / 2, py / 2, 0x7BEEu + (uint32_t)tile) > 0.60f) alpha = 0.0f;
            break;
        }
        case TEX_GRASS_TUFT: {
            // Pixel-style blade: stepwise alpha (wide root, narrow tip) plus a
            // short gradient near the root that settles into a constant color.
            RGB base{ 84.0f, 150.0f, 64.0f }; // root color
            RGB tip{ 76.0f, 143.0f, 56.0f };  // tip color (slightly lighter than sod)

            // Gradient completes within the bottom band (near the root).
            int G = 4 * TS;
            float gradT = 0.0f;
            if (py >= TILE - 1 - G) gradT = (float)(py - (TILE - 1 - G)) / (float)G;
            RGB c2 = mix(tip, base, gradT);

            // Stepwise alpha: discrete steps from a wide root to a narrow tip.
            int steps = 8;
            int stepH = TILE / steps;
            int idx = py / stepH;
            if (idx >= steps) idx = steps - 1;
            float wf = 0.14f + 0.76f * (float)idx / (float)(steps - 1);
            float halfW = wf * 0.5f * (float)TILE;
            if (std::fabs((float)px - (float)(TILE / 2)) > halfW) alpha = 0.0f;

            float v = 0.94f + 0.12f * n;
            c = { c2.r * v, c2.g * v, c2.b * v };
            break;
        }
        case TEX_LEAF_X: {
            // Leaf frill for LEAVES blocks: reuse the tree-leaf noise, dark
            // patches, and 2x2 cutout pattern exactly (same tile seed).
            float ln = rnd(px, py, (uint32_t)TEX_LEAVES * 31337u + 7u);
            float v = 0.75f + 0.5f * ln;
            c = { 30.0f * v, 110.0f * v, 30.0f * v };
            if (ln > 0.86f) c = { 18.0f, 70.0f, 20.0f };
            alpha = leafFrillAlpha(px, py, (uint32_t)TEX_LEAVES, 0x3EAFu, 0.65f);
            break;
        }
        case TEX_SHRUB_LEAF_X: {
            // Leaf frill for SHRUB_LEAF blocks: reuse the bush-leaf noise, dark
            // patches, and 2x2 cutout pattern exactly (same tile seed).
            float ln = rnd(px, py, (uint32_t)TEX_SHRUB_LEAF * 31337u + 7u);
            float v = 0.72f + 0.56f * ln;
            c = { 36.0f * v, 108.0f * v, 32.0f * v };
            if (ln > 0.85f) c = { 22.0f, 72.0f, 22.0f };
            alpha = leafFrillAlpha(px, py, (uint32_t)TEX_SHRUB_LEAF, 0x7BEEu, 0.60f);
            break;
        }
        case TEX_BARK: {
            float fx = (float)px / (float)(TILE - 1);
            float fy = (float)py / (float)(TILE - 1);
            float ridge = 0.55f + 0.45f * std::sin(fx * 16.0f + 0.5f * std::sin(fy * 5.0f));
            float grain = 0.84f + 0.16f * rnd(px / (1 * TS), py / (3 * TS), 661u);
            RGB bark{ 96.0f, 64.0f, 38.0f };
            RGB dark{ 62.0f, 40.0f, 24.0f };
            RGB c0 = mix(dark, bark, ridge * grain);
            c = c0;
            break;
        }
        case TEX_WOOD_SIDE: {
            // Stripped lumber: pale vertical grain (no bark ridges).
            float fx = (float)px / (float)(TILE - 1);
            float fy = (float)py / (float)(TILE - 1);
            float stripe = 0.5f + 0.5f * std::sin(fx * 26.0f + 0.45f * std::sin(fy * 5.5f));
            float pore = rnd(px / (1 * TS), py / (2 * TS), 419u);
            float v = 0.90f + 0.12f * rnd(px, py / (4 * TS), 271u);
            RGB early{ 204.0f, 162.0f, 96.0f };
            RGB late{ 154.0f, 112.0f, 58.0f };
            RGB c0 = mix(late, early, stripe);
            if (pore > 0.82f) c0 = mix(c0, RGB{ 128.0f, 92.0f, 48.0f }, (pore - 0.82f) / 0.18f);
            c = { c0.r * v, c0.g * v, c0.b * v };
            break;
        }
        case TEX_HAND_AXE: {
            alpha = 0.0f;
            c = { 0, 0, 0 };
            float u = (float)px / (float)(TILE - 1);
            float v = (float)py / (float)(TILE - 1);
            // Handle: wood bar from bottom-left toward the head.
            float hx = u - 0.22f, hy = v - 0.78f;
            float hAlong = hx * 0.48f + hy * 0.88f;
            float hAcross = -hx * 0.88f + hy * 0.48f;
            bool handle = hAlong > 0.0f && hAlong < 0.72f && std::fabs(hAcross) < 0.055f;
            // Head: iron wedge sitting on the upper handle.
            bool poll = (u > 0.46f && u < 0.72f && v > 0.16f && v < 0.50f);
            bool blade = (u > 0.62f && u < 0.92f && v > 0.20f && v < 0.46f
                          && (u * 0.7f + v) > 0.62f && (u * 0.55f + v) < 0.92f);
            if (handle) {
                alpha = 255.0f;
                float g = 0.88f + 0.12f * n;
                c = { 176.0f * g, 132.0f * g, 74.0f * g };
            } else if (blade || poll) {
                alpha = 255.0f;
                float shade = blade ? (0.92f + 0.10f * n) : (0.78f + 0.12f * n);
                c = { 186.0f * shade, 190.0f * shade, 198.0f * shade };
            }
            break;
        }
        case TEX_SHEARS: {
            alpha = 0.0f;
            c = { 0, 0, 0 };
            float u = (float)px / (float)(TILE - 1);
            float v = (float)py / (float)(TILE - 1);
            auto blade = [&](float ox, float oy, float ang) {
                float ca = std::cos(ang), sa = std::sin(ang);
                float lx = (u - ox) * ca + (v - oy) * sa;
                float ly = -(u - ox) * sa + (v - oy) * ca;
                return lx > 0.0f && lx < 0.55f && std::fabs(ly) < (0.045f + 0.04f * lx);
            };
            bool b0 = blade(0.32f, 0.38f, -0.42f);
            bool b1 = blade(0.32f, 0.38f, 0.42f);
            bool pivot = (u - 0.34f) * (u - 0.34f) + (v - 0.40f) * (v - 0.40f) < 0.012f;
            bool ring0 = (u - 0.22f) * (u - 0.22f) + (v - 0.72f) * (v - 0.72f) < 0.028f
                      && (u - 0.22f) * (u - 0.22f) + (v - 0.72f) * (v - 0.72f) > 0.012f;
            bool ring1 = (u - 0.46f) * (u - 0.46f) + (v - 0.78f) * (v - 0.78f) < 0.028f
                      && (u - 0.46f) * (u - 0.46f) + (v - 0.78f) * (v - 0.78f) > 0.012f;
            if (b0 || b1 || pivot) {
                alpha = 255.0f;
                float g = 0.90f + 0.10f * n;
                c = { 198.0f * g, 202.0f * g, 210.0f * g };
            } else if (ring0 || ring1) {
                alpha = 255.0f;
                c = { 160.0f, 110.0f, 58.0f };
            }
            break;
        }
        case TEX_HAND_PICK: {
            alpha = 0.0f;
            c = { 0, 0, 0 };
            float u = (float)px / (float)(TILE - 1);
            float v = (float)py / (float)(TILE - 1);
            float hx = u - 0.28f, hy = v - 0.78f;
            float hAlong = hx * 0.35f + hy * 0.94f;
            float hAcross = -hx * 0.94f + hy * 0.35f;
            bool handle = hAlong > 0.0f && hAlong < 0.78f && std::fabs(hAcross) < 0.05f;
            bool head = (u > 0.22f && u < 0.88f && v > 0.14f && v < 0.34f);
            bool tip = (u > 0.70f && u < 0.96f && v > 0.10f && v < 0.38f && (u - v) > 0.42f);
            if (handle) {
                alpha = 255.0f;
                float g = 0.88f + 0.12f * n;
                c = { 176.0f * g, 132.0f * g, 74.0f * g };
            } else if (head || tip) {
                alpha = 255.0f;
                float shade = 0.86f + 0.12f * n;
                c = { 170.0f * shade, 174.0f * shade, 182.0f * shade };
            }
            break;
        }
        case TEX_HAND_SHOVEL: {
            alpha = 0.0f;
            c = { 0, 0, 0 };
            float u = (float)px / (float)(TILE - 1);
            float v = (float)py / (float)(TILE - 1);
            float hx = u - 0.46f, hy = v - 0.82f;
            float hAlong = hx * 0.08f + hy * 0.997f;
            float hAcross = -hx * 0.997f + hy * 0.08f;
            bool handle = hAlong > 0.0f && hAlong < 0.62f && std::fabs(hAcross) < 0.045f;
            bool scoop = (u > 0.28f && u < 0.72f && v > 0.10f && v < 0.42f
                          && std::fabs(u - 0.50f) < 0.18f + 0.22f * (0.42f - v));
            if (handle) {
                alpha = 255.0f;
                float g = 0.88f + 0.12f * n;
                c = { 176.0f * g, 132.0f * g, 74.0f * g };
            } else if (scoop) {
                alpha = 255.0f;
                float shade = 0.88f + 0.10f * n;
                c = { 186.0f * shade, 190.0f * shade, 198.0f * shade };
            }
            break;
        }
        case TEX_CRACK: {
            // Alpha bands encode reveal order (higher alpha = earlier crack).
            // The overlay shader thresholds by break progress so cracks multiply
            // instead of the same lines getting darker.
            alpha = 0.0f;
            c = { 18.0f, 16.0f, 14.0f };
            float wobU = (float)px / (float)(TILE - 1);
            float wobV = (float)py / (float)(TILE - 1);
            wobU += 0.018f * (rnd(px / 2, py / 2, 0xC2A1u) - 0.5f);
            wobV += 0.018f * (rnd(px / 2, py / 2, 0xC2B3u) - 0.5f);
            auto distSeg = [&](float ax, float ay, float bx, float by) {
                float vx = bx - ax, vy = by - ay;
                float wx = wobU - ax, wy = wobV - ay;
                float L = vx * vx + vy * vy;
                float t = (L > 1e-8f) ? (wx * vx + wy * vy) / L : 0.0f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float dx = wobU - (ax + vx * t), dy = wobV - (ay + vy * t);
                return std::sqrt(dx * dx + dy * dy);
            };
            // Matches the overlay shader: unique births, same width family.
            static const float kSeg[][6] = {
                { 0.50f, 0.08f, 0.47f, 0.38f, 0.00f, 0.030f },
                { 0.47f, 0.38f, 0.42f, 0.62f, 0.04f, 0.030f },
                { 0.42f, 0.62f, 0.48f, 0.96f, 0.08f, 0.028f },
                { 0.47f, 0.38f, 0.22f, 0.28f, 0.12f, 0.028f },
                { 0.22f, 0.28f, 0.04f, 0.42f, 0.16f, 0.026f },
                { 0.47f, 0.38f, 0.78f, 0.22f, 0.20f, 0.028f },
                { 0.78f, 0.22f, 0.96f, 0.36f, 0.24f, 0.026f },
                { 0.42f, 0.62f, 0.18f, 0.70f, 0.28f, 0.026f },
                { 0.18f, 0.70f, 0.06f, 0.88f, 0.32f, 0.024f },
                { 0.42f, 0.62f, 0.70f, 0.58f, 0.36f, 0.026f },
                { 0.70f, 0.58f, 0.92f, 0.72f, 0.40f, 0.024f },
                { 0.48f, 0.80f, 0.68f, 0.92f, 0.44f, 0.024f },
                { 0.48f, 0.80f, 0.28f, 0.94f, 0.48f, 0.024f },
                { 0.22f, 0.28f, 0.18f, 0.08f, 0.52f, 0.024f },
                { 0.78f, 0.22f, 0.70f, 0.06f, 0.56f, 0.024f },
                { 0.18f, 0.70f, 0.08f, 0.52f, 0.60f, 0.024f },
                { 0.70f, 0.58f, 0.86f, 0.48f, 0.64f, 0.024f },
                { 0.30f, 0.50f, 0.12f, 0.48f, 0.68f, 0.024f },
                { 0.58f, 0.44f, 0.74f, 0.38f, 0.72f, 0.024f },
                { 0.36f, 0.84f, 0.22f, 0.78f, 0.76f, 0.024f },
                { 0.62f, 0.86f, 0.78f, 0.80f, 0.80f, 0.024f },
                { 0.08f, 0.20f, 0.20f, 0.16f, 0.84f, 0.024f },
                { 0.90f, 0.58f, 0.98f, 0.50f, 0.88f, 0.024f },
                { 0.04f, 0.64f, 0.14f, 0.90f, 0.92f, 0.024f },
            };
            constexpr int kN = (int)(sizeof(kSeg) / sizeof(kSeg[0]));
            float bestBirth = 2.0f;
            for (int i = 0; i < kN; i++) {
                float d = distSeg(kSeg[i][0], kSeg[i][1], kSeg[i][2], kSeg[i][3]);
                if (d < kSeg[i][5] && kSeg[i][4] < bestBirth) bestBirth = kSeg[i][4];
            }
            if (bestBirth < 1.5f) {
                int stage = (int)(bestBirth * 8.0f);
                if (stage < 0) stage = 0;
                if (stage > 7) stage = 7;
                alpha = 255.0f * (1.0f - (float)stage / 8.0f);
            }
            break;
        }
        case TEX_CORE: {
            float v = 0.35f + 0.25f * n;
            c = { 28.0f * v, 36.0f * v, 48.0f * v };
            float cx = (float)px / (float)(TILE - 1) - 0.5f;
            float cy = (float)py / (float)(TILE - 1) - 0.5f;
            float d = std::sqrt(cx * cx + cy * cy);
            if (d < 0.16f) c = { 120.0f, 230.0f, 255.0f };
            else if (d < 0.28f) c = { 40.0f + 80.0f * (0.28f - d), 90.0f, 140.0f };
            break;
        }
        default: {
            c = { 255.0f, 0.0f, 255.0f }; // magenta = error
            break;
        }
    }

    r = cl(c.r); g = cl(c.g); b = cl(c.b); a = cl(alpha);
}

void generateAtlas(std::vector<uint8_t>& out) {
    out.assign((size_t)ATLAS_W * ATLAS_H * 4, 0);
    for (int tile = 0; tile < TEX_COUNT; tile++) {
        int tx = (tile % COLS) * TILE;
        int ty = (tile / COLS) * TILE;
        for (int py = 0; py < TILE; py++) {
            for (int px = 0; px < TILE; px++) {
                uint8_t r, g, b, a;
                pixelColor(tile, px, py, r, g, b, a);
                size_t idx = ((size_t)(ty + py) * ATLAS_W + (tx + px)) * 4;
                out[idx + 0] = r;
                out[idx + 1] = g;
                out[idx + 2] = b;
                out[idx + 3] = a;
            }
        }
    }
}

void generateTileRGBA(int tile, std::vector<uint8_t>& out) {
    out.assign((size_t)TILE * TILE * 4, 0);
    for (int py = 0; py < TILE; py++) {
        for (int px = 0; px < TILE; px++) {
            uint8_t r, g, b, a;
            pixelColor(tile, px, py, r, g, b, a);
            size_t idx = ((size_t)py * TILE + px) * 4;
            out[idx + 0] = r;
            out[idx + 1] = g;
            out[idx + 2] = b;
            out[idx + 3] = a;
        }
    }
}

void overwriteTile(int tile, const uint8_t* rgba, std::vector<uint8_t>& atlas) {
    if (!rgba || (int)atlas.size() != ATLAS_W * ATLAS_H * 4) return;
    int tx = (tile % COLS) * TILE;
    int ty = (tile / COLS) * TILE;
    for (int py = 0; py < TILE; py++) {
        for (int px = 0; px < TILE; px++) {
            size_t src = ((size_t)py * TILE + px) * 4;
            size_t dst = ((size_t)(ty + py) * ATLAS_W + (tx + px)) * 4;
            atlas[dst + 0] = rgba[src + 0];
            atlas[dst + 1] = rgba[src + 1];
            atlas[dst + 2] = rgba[src + 2];
            atlas[dst + 3] = rgba[src + 3];
        }
    }
}

void tileUV(int tileId, float& u0, float& v0, float& u1, float& v1) {
    int tx = tileId % COLS;
    int ty = tileId / COLS;
    const float pad = 0.06f;
    u0 = (tx + pad) / COLS;
    u1 = (tx + 1.0f - pad) / COLS;
    v0 = (ty + pad) / ROWS;          // image top
    v1 = (ty + 1.0f - pad) / ROWS;   // image bottom
}

// Body sheet unwrap. No hair, eyes, or mouth on this sheet.
void generatePlayerSkin(std::vector<uint8_t>& out) {
    const int W = pm::kSkinW, H = pm::kSkinH;
    out.assign((size_t)W * H * 4, 0);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
        size_t i = ((size_t)y * W + x) * 4;
        out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = 255;
    };
    auto fill = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        for (int py = 0; py < h; py++) for (int px = 0; px < w; px++) {
            float n = rnd(x + px, y + py, 0x51E4u);
            uint8_t rr = cl((float)r * (0.94f + 0.10f * n));
            uint8_t gg = cl((float)g * (0.94f + 0.10f * n));
            uint8_t bb = cl((float)b * (0.94f + 0.10f * n));
            bool edge = (px == 0 || py == 0 || px == w - 1 || py == h - 1);
            if (edge) { rr = (uint8_t)(rr * 0.82f); gg = (uint8_t)(gg * 0.82f); bb = (uint8_t)(bb * 0.82f); }
            put(x + px, y + py, rr, gg, bb);
        }
    };
    // Face shades: +X -X +Y -Y +Z -Z.
    // Arm +Y = shoulder top, arm -Y = forearm bottom; extra caps are distinct.
    const uint8_t faceRGB[6][3] = {
        { 214, 164, 126 }, { 198, 150, 114 }, { 236, 196, 162 },
        { 176, 128,  96 }, { 230, 184, 148 }, { 188, 142, 108 }
    };
    auto box = [&](const pm::SkinBox& b) {
        for (int f = 0; f < 6; f++) {
            int x, y, fw, fh;
            pm::boxFacePx(b.bx, b.by, b.w, b.h, b.d, f, x, y, fw, fh);
            fill(x, y, fw, fh, faceRGB[f][0], faceRGB[f][1], faceRGB[f][2]);
        }
    };
    for (int i = 0; i < pm::kSkinBoxCount; i++) box(pm::kSkinBoxes[i]);
    // Leg island caps are joint cross-sections (not the foot).
    for (int i = 0; i < pm::kSkinLimbCount; i++) {
        const pm::SkinLimbSlot& sl = pm::kSkinLimbs[i];
        if (sl.type != 3) continue;
        for (int f = 2; f <= 3; f++) {
            int x, y, fw, fh;
            pm::boxFacePx(sl.bx, sl.by, sl.w, sl.h, sl.d, f, x, y, fw, fh);
            fill(x, y, fw, fh, 196, 108, 96);
        }
    }
    pm::SkinExtraCap cap{};
    for (int i = 0; i < pm::kSkinExtraCapCount; i++) {
        if (!pm::extraCapByIndex(i, cap)) continue;
        uint8_t r = 196, g = 108, b = 96; // xsect
        if (cap.role == 1) {
            if (cap.type == 2) { r = 224; g = 172; b = 136; }
            else { r = 206; g = 154; b = 116; }
        } else if (cap.role == 2) {
            if (cap.type == 2) { r = 158; g = 108; b = 78; }
            else { r = 120; g = 78; b = 52; }
        }
        fill(cap.x, cap.y, cap.w, cap.h, r, g, b);
    }
}

void generatePlayerEye(std::vector<uint8_t>& out) {
    const int N = 8;
    out.assign((size_t)N * N * 4, 0);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if ((unsigned)x >= (unsigned)N || (unsigned)y >= (unsigned)N) return;
        size_t i = ((size_t)y * N + x) * 4;
        out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = a;
    };
    // 8x8 cutout: sclera, iris, pupil. Transparent outside the eye.
    for (int y = 1; y <= 6; y++) for (int x = 1; x <= 6; x++) put(x, y, 236, 236, 228, 255);
    for (int y = 2; y <= 5; y++) for (int x = 2; x <= 5; x++) put(x, y, 72, 48, 28, 255);
    put(3, 3, 18, 14, 12, 255); put(4, 3, 18, 14, 12, 255);
    put(3, 4, 18, 14, 12, 255); put(4, 4, 18, 14, 12, 255);
    put(2, 2, 210, 190, 160, 255); // catchlight
}

void generatePlayerEyelid(std::vector<uint8_t>& out) {
    const int N = 8;
    out.assign((size_t)N * N * 4, 0);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if ((unsigned)x >= (unsigned)N || (unsigned)y >= (unsigned)N) return;
        size_t i = ((size_t)y * N + x) * 4;
        out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = a;
    };
    for (int y = 0; y <= 3; y++) for (int x = 1; x <= 6; x++)
        put(x, y, 210, 168, 136, 255);
    for (int x = 1; x <= 6; x++) put(x, 3, 168, 118, 92, 255);
}

void generatePlayerMouth(std::vector<uint8_t>& out, int variant) {
    const int W = 16, H = 8;
    out.assign((size_t)W * H * 4, 0);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
        size_t i = ((size_t)y * W + x) * 4;
        out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = a;
    };
    auto lip = [&](int x, int y) { put(x, y, 168, 86, 86, 255); };
    auto dark = [&](int x, int y) { put(x, y, 48, 22, 22, 255); };
    auto inner = [&](int x, int y) { put(x, y, 92, 28, 36, 255); };
    if (variant <= 0) {
        for (int x = 3; x <= 12; x++) lip(x, 3);
        for (int x = 4; x <= 11; x++) dark(x, 4);
        for (int x = 4; x <= 11; x++) lip(x, 5);
    } else if (variant == 1) {
        for (int x = 4; x <= 11; x++) lip(x, 1);
        for (int x = 3; x <= 12; x++) lip(x, 2);
        for (int y = 3; y <= 5; y++) {
            lip(3, y); lip(12, y);
            for (int x = 4; x <= 11; x++) inner(x, y);
        }
        for (int x = 4; x <= 11; x++) dark(x, 4);
        for (int x = 3; x <= 12; x++) lip(x, 6);
        for (int x = 5; x <= 10; x++) lip(x, 7);
    } else {
        lip(3, 5); lip(4, 4); lip(5, 3); lip(6, 3);
        for (int x = 7; x <= 8; x++) lip(x, 3);
        lip(9, 3); lip(10, 3); lip(11, 4); lip(12, 5);
        dark(4, 5); dark(5, 4); dark(6, 4);
        dark(9, 4); dark(10, 4); dark(11, 5);
        for (int x = 7; x <= 8; x++) dark(x, 4);
        lip(5, 5); lip(6, 5); lip(9, 5); lip(10, 5);
        for (int x = 7; x <= 8; x++) lip(x, 5);
    }
}

} // namespace tex
