# VOXEL LEGEND (自带光影的体素沙盒)

A complete, dependency-free voxel sandbox (VOXEL LEGEND) written in C++20, using
**Win32 + OpenGL 3.3 core** only. No GLFW, GLAD, GLM, or stb_image — everything is
implemented from scratch: windowing, a minimal GL function loader, math, noise, a
procedural texture atlas, and a GDI-based bitmap font.

Built and tested with MinGW-w64 g++ 15.2 on Windows (NVIDIA RTX 4060 Ti, OpenGL 3.3).

## Features

- **Infinite voxel world** — deterministic Perlin/value-noise terrain, caves, ores,
  beaches, trees, water, bedrock, and 19 block types. Blocks are **half-size**
  (`BLOCK_SCALE = 0.5`) for 2× resolution and smoother, more detailed terrain/trees.
- **Built-in lighting / 光影** (all in shaders):
  - Directional sun with a full **day/night cycle**, sunrise/sunset colors, moon, stars.
  - **Voxel ambient occlusion** (smooth "VOXEL LEGEND-style" corner shading) baked per vertex.
  - Per-face directional shading, distance **fog**, and a gradient sky with a sun disc.
- **Complete gameplay**:
  - First-person camera (raw-input mouse + keyboard), WASD move, sprint, jump, gravity,
    collision, and flying.
  - Break blocks (left click) / place blocks (right click) with a voxel raycast.
  - 9-slot hotbar (1–9 or scroll), block outline highlight, crosshair.
  - **Backpack inventory** (press `E`) with drag-and-drop item stacking.
  - Swimming/buoyancy in water.
- **Persistence** — chunks (with checksums) and player state are saved to disk and
  restored on relaunch; corrupted data falls back to regeneration.
- **Stability engineering**:
  - Chunk generation is budgeted per frame (nearest-first), so there is no multi-second
    freeze; meshing is also frame-budgeted.
  - Camera-centered transform (floating origin) keeps precision at any distance from spawn.
  - RAII / value semantics throughout; no manual `new`/`delete`; single-threaded and
    deterministic (no races).
  - A headless `--selftest` stress test that exercises generation, meshing, raycast,
    block edits, and save/load round-trips without any window or GPU.

## Build

Requires MinGW-w64 g++ (C++20) on the PATH.

```powershell
pwsh -File .\build.ps1
```

This produces `voxel-legend.exe`. (It links only against system `opengl32`, `gdi32`,
`user32`.)

## Run

```powershell
.\voxel-legend.exe                     # play
.\voxel-legend.exe --seed 12345        # specific world seed
.\voxel-legend.exe --no-save           # don't write saves
.\voxel-legend.exe --selftest 1500     # headless stability stress test (1500 iterations)
.\voxel-legend.exe --frames 400        # run 400 frames then exit (smoke test)
.\voxel-legend.exe --help
```

## Controls

| Input | Action |
|-------|--------|
| Mouse | Look around |
| W A S D | Move |
| Space | Jump / swim up / fly up |
| Left Shift | Fly down |
| Left Ctrl | Sprint |
| Left click | Break block |
| Right click | Place block |
| 1–9 / scroll | Select hotbar slot |
| E | Open/close inventory (backpack) |
| F | Toggle fly |
| F3 | Debug overlay (FPS, position, time, chunks) |
| Esc | Open/close pause menu |

### Inventory (E)

The inventory is a 36-slot backpack: a 9-slot hotbar plus a 3×9 main grid, with item
**quantities** (stacks up to 64). In the inventory screen:

- **Drag** an item to move a **single** item.
- **Shift + drag** to move the **whole stack**.
- Click a block in the bottom palette to pick up a full stack (64) of that block.
- Right-click returns a held stack to its slot, or closes the inventory.

In the world, breaking a block adds it to your inventory, and placing a block consumes
one item from the selected hotbar slot. The inventory is saved with the player. The
inventory starts **empty** — break blocks (or pick stacks from the palette) to fill it.

### Pause menu (Esc)

The pause menu (VOXEL LEGEND-style pixel UI) offers:

- **继续游戏** (Resume) — return to the game.
- **设置** (Settings) — open the settings submenu.
- **退出游戏** (Quit) — save and exit.

In **设置** you can drag the **鼠标灵敏度** (mouse sensitivity) slider; the value is
saved to `world_save/player.bin` and restored on the next launch. Block icons (grass,
dirt, stone) and beveled buttons use the in-game 16×16 pixel-art texture atlas.

## Project layout

```
src/
  config.hpp      constants (chunk size, physics, fog, …)
  math.hpp        Vec3/Mat4 + inverse (column-major)
  noise.hpp       deterministic value noise (2D/3D, fbm)
  blocks.hpp      block enum + per-block properties
  gl.hpp/.cpp     minimal OpenGL 3.3 core + WGL function loader
  shaders.hpp     embedded GLSL 330 shader sources
  textures.cpp    procedural 8x8 tile texture atlas
  world.hpp/.cpp  chunks, generation, meshing (AO), save/load, raycast
  player.hpp/.cpp first-person physics + collision
  renderer.hpp/.cpp  world/sky/HUD rendering, GDI text, UI
  main.cpp        Win32 window/context, input, game loop, self-test
```

## Stability self-test

`--selftest N` teleports a virtual player through a large region N times, each step
generating + meshing chunks, performing voxel raycasts, breaking/placing blocks, and
periodically saving + reloading the world. It runs with no window/GPU and exits 0 on
success. Example result:

```
=== VOXEL LEGEND self-test ===
seed=1337  iterations=1500
placed=1430 broken=1500 loadedChunks=75 pendingMeshes=1
SELF-TEST PASS
```
