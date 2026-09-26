#pragma once
#include <cstdint>

namespace cfg {
    // World
    constexpr int CHUNK_X = 16;
    constexpr int CHUNK_Z = 16;
    constexpr int CHUNK_H = 128;
    constexpr int CHUNK_VOLUME = CHUNK_X * CHUNK_H * CHUNK_Z;

    constexpr int SEA_LEVEL = 36;
    constexpr int LOAD_RADIUS = 6;        // chunk columns around player to keep loaded
    constexpr int UNLOAD_RADIUS = LOAD_RADIUS + 2;

    // Block scale: edge length of one voxel in world units. Half-size blocks give
    // 2x resolution (finer, smoother terrain).
    constexpr float BLOCK_SCALE = 0.5f;

    // Player (world units; with BLOCK_SCALE=0.5 the player is 3.6 blocks tall, so
    // blocks appear half-size = finer world detail).
    constexpr float PLAYER_HALF_WIDTH = 0.30f;
    constexpr float PLAYER_HEIGHT = 1.80f;
    constexpr float EYE_HEIGHT = 1.62f;
    constexpr float GRAVITY = 30.0f;
    constexpr float JUMP_SPEED = 9.2f;
    constexpr float WALK_SPEED = 4.4f;
    constexpr float SPRINT_SPEED = 6.4f;
    constexpr float FLY_SPEED = 12.0f;
    constexpr float FLY_SPRINT_SPEED = 26.0f;
    constexpr float WATER_GRAVITY = 12.0f;
    constexpr float SWIM_SPEED = 3.4f;
    constexpr float REACH = 6.0f;

    // Simulation
    constexpr float FIXED_DT = 1.0f / 120.0f;
    constexpr int MAX_SUBSTEPS = 8;

    // Mouse sensitivity (radians per pixel of cursor movement).
    constexpr float SENS_MIN = 0.0005f;
    constexpr float SENS_MAX = 0.0060f;
    constexpr float SENS_DEFAULT = 0.0022f;

    // Time
    constexpr int TICKS_PER_DAY = 24000;
    constexpr float DAY_LENGTH_SECONDS = 1200.0f; // full day/night cycle in real seconds
    constexpr int TICKS_PER_SECOND = 20;          // game ticks for block updates
    constexpr int SOD_STAGES = 4;                 // grass-sod wither stages (0..3)
    constexpr int WITHER_STAGE_TICKS = 120 * TICKS_PER_SECOND; // ~2 real minutes per stage (at 1x)

    // Water flow
    constexpr int WATER_SOURCE_LEVEL = 16; // temporary source = 16 stacked films (full block)
    constexpr int WATER_MAX_LEVEL = 16;
    constexpr int HUMIDITY_MIN = -256;
    constexpr int HUMIDITY_MAX = 255;

    // Falling-tree leaves: hide when squashed, destroy if still crushed this long.
    constexpr int LEAF_CRUSH_TICKS = 40;
    constexpr float LEAF_HIDE_COMPRESS = 0.42f;
    constexpr float LEAF_ELASTIC_K = 3.5f;
    // Fraction of stored energy discarded on convert (leaves are loose → nearly all heat).
    constexpr float LEAF_ELASTIC_LOSS = 0.96f;
    constexpr float WOOD_ENERGY_LOSS = 1.00f;
    constexpr float WOOD_FRICTION = 0.72f;
    constexpr float LEAF_FRICTION = 0.95f;

    // Rendering
    constexpr float FOV_Y = 70.0f;         // degrees
    constexpr float NEAR_PLANE = 0.1f;
    constexpr float FAR_PLANE = 1000.0f;
    constexpr float FOG_START = 24.0f;
    constexpr float FOG_DENSITY = 0.012f;

    constexpr int HAND_SLOTS = 3;                 // per-hand hotbar
    constexpr int HOTBAR_SLOTS = HAND_SLOTS * 2;  // left 0..2, right 3..5
    constexpr int MAIN_SLOTS = 27;
    constexpr int INVENTORY_SLOTS = HOTBAR_SLOTS + MAIN_SLOTS; // 33
    constexpr int MAX_STACK = 64;            // hard cap; actual stack comes from loot table
    constexpr float DROP_SIZE = BLOCK_SCALE; // same edge as a block, so a drop sits in the broken cell
    constexpr float DROP_LIFETIME = 480.0f;  // seconds on the ground before despawn

    constexpr const char* SAVE_DIR = "GameSaves";
    constexpr const char* SAVE_ACTIVE = "default";
}
