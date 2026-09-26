#include "core/config.hpp"
#include "world/blocks.hpp"
#include "world/world.hpp"
#include "world/loot.hpp"
#include "world/data_pack.hpp"
#include "world/saves.hpp"
#include "world/player.hpp"
#include "world/player_model.hpp"
#include "world/hold_bind.hpp"
#include "world/vitals.hpp"
#include "world/wear.hpp"
#include "render/renderer.hpp"
#include "core/gl.hpp"
#include "render/textures.hpp"
#include "material/registry.hpp"
#include "plugin/plugin.hpp"
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

// ---------------------------------------------------------------------------
// Window / input globals (populated by WndProc, consumed by the game loop).
// ---------------------------------------------------------------------------
static float g_mouseDX = 0.0f, g_mouseDY = 0.0f;
static float g_mouseScale = 1.0f;
static float g_absScaleX = 1.0f, g_absScaleY = 1.0f;
static LONG g_prevAbsX = -1, g_prevAbsY = -1;
static int g_screenW = 1280, g_screenH = 720;
static bool g_focused = true;
static int g_winW = 1280, g_winH = 720;
static bool g_resized = false;
static bool g_cursorHidden = false;
static int g_wheel = 0;
static std::string* g_textTarget = nullptr;
static bool g_textSubmit = false;

static void popUtf8(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
    s.resize(i);
}

static bool appendUtf8(std::string& s, const char* utf8, size_t maxBytes) {
    if (!utf8 || !*utf8) return false;
    size_t n = std::strlen(utf8);
    if (s.size() + n > maxBytes) return false;
    s.append(utf8, n);
    return true;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_INPUT: {
            UINT size = 0;
            GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size > 0 && size <= 512) {
                BYTE buf[512];
                if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
                    RAWINPUT* raw = (RAWINPUT*)buf;
                    if (raw->header.dwType == RIM_TYPEMOUSE) {
                        RAWMOUSE& m = raw->data.mouse;
                        if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
                            // Absolute device (touchscreen / some touchpads): lLastX/Y are
                            // normalized [0,65535] positions, NOT deltas. Convert to a
                            // pixel-equivalent delta from the previous position.
                            if (g_prevAbsX >= 0) {
                                g_mouseDX += (float)(m.lLastX - g_prevAbsX) * g_absScaleX;
                                g_mouseDY += (float)(m.lLastY - g_prevAbsY) * g_absScaleY;
                            }
                            g_prevAbsX = m.lLastX;
                            g_prevAbsY = m.lLastY;
                        } else {
                            g_mouseDX += (float)m.lLastX * g_mouseScale;
                            g_mouseDY += (float)m.lLastY * g_mouseScale;
                        }
                    }
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            g_wheel += (int)(short)HIWORD(w);
            return 0;
        case WM_CHAR: {
            if (!g_textTarget) return 0;
            wchar_t wc = (wchar_t)w;
            if (wc < 32) return 0;
            wchar_t wcs[2] = { wc, 0 };
            char utf8[8] = {};
            int n = WideCharToMultiByte(CP_UTF8, 0, wcs, -1, utf8, (int)sizeof(utf8), nullptr, nullptr);
            if (n > 1) {
                utf8[n - 1] = 0;
                // Reject Windows-illegal filename characters.
                if (utf8[1] == 0) {
                    char c = utf8[0];
                    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                        c == '\\' || c == '|' || c == '?' || c == '*')
                        return 0;
                }
                appendUtf8(*g_textTarget, utf8, 64);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (g_textTarget) {
                if (w == VK_BACK) { popUtf8(*g_textTarget); return 0; }
                if (w == VK_RETURN) { g_textSubmit = true; return 0; }
            }
            break;
        case WM_SIZE:
            g_winW = LOWORD(l);
            g_winH = HIWORD(l);
            g_resized = true;
            return 0;
        case WM_SETFOCUS:
            g_focused = true;
            return 0;
        case WM_KILLFOCUS:
            g_focused = false;
            ClipCursor(nullptr);
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            ClipCursor(nullptr);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(h, m, w, l);
}

struct WinGL {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC ctx = nullptr;
};

static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    FILE* f = fopen("crash.log", "w");
    if (f) {
        fprintf(f, "exception 0x%08lX at %p\n", code, ep->ExceptionRecord->ExceptionAddress);
        fclose(f);
    }
    fprintf(stderr, "[crash] exception 0x%08lX at %p\n", code, ep->ExceptionRecord->ExceptionAddress);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void loadWglExtensions() {
    HINSTANCE inst = GetModuleHandle(nullptr);
    HWND hw = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hw) return;
    HDC dc = GetDC(hw);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf) SetPixelFormat(dc, pf, &pfd);
    HGLRC rc = wglCreateContext(dc);
    if (rc) {
        wglMakeCurrent(dc, rc);
        gl::WglCreateContextAttribsARB =
            (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
        gl::WglChoosePixelFormatARB =
            (PFNWGLCHOOSEPIXELFORMATARBPROC)wglGetProcAddress("wglChoosePixelFormatARB");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
    }
    ReleaseDC(hw, dc);
    DestroyWindow(hw);
}

static bool createWindowAndContext(WinGL& wg, int w, int h) {
    loadWglExtensions();

    HINSTANCE inst = GetModuleHandle(nullptr);
    WNDCLASSW wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"VoxelLegendWnd";
    if (!RegisterClassW(&wc)) return false;

    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    wg.hwnd = CreateWindowExW(0, wc.lpszClassName, L"VOXEL LEGEND",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, inst, nullptr);
    if (!wg.hwnd) return false;

    // Show the window (CreateWindowEx does not display it by itself).
    ShowWindow(wg.hwnd, SW_SHOW);
    UpdateWindow(wg.hwnd);

    wg.hdc = GetDC(wg.hwnd);

    HGLRC ctx = nullptr;
    if (gl::WglChoosePixelFormatARB && gl::WglCreateContextAttribsARB) {
        const int attrs[] = {
            WGL_DRAW_TO_WINDOW_ARB, 1,
            WGL_SUPPORT_OPENGL_ARB, 1,
            WGL_DOUBLE_BUFFER_ARB, 1,
            WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
            WGL_COLOR_BITS_ARB, 24,
            WGL_DEPTH_BITS_ARB, 24,
            WGL_STENCIL_BITS_ARB, 8,
            0
        };
        int npf = 0;
        UINT nf = 0;
        if (gl::WglChoosePixelFormatARB(wg.hdc, attrs, nullptr, 1, &npf, &nf) && npf > 0) {
            PIXELFORMATDESCRIPTOR pfd2 = {};
            DescribePixelFormat(wg.hdc, npf, sizeof(pfd2), &pfd2);
            SetPixelFormat(wg.hdc, npf, &pfd2);
            const int cattrs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
                0
            };
            ctx = gl::WglCreateContextAttribsARB(wg.hdc, nullptr, cattrs);
        }
    }
    if (!ctx) {
        ReleaseDC(wg.hwnd, wg.hdc);
        DestroyWindow(wg.hwnd);
        return false;
    }
    wglMakeCurrent(wg.hdc, ctx);
    wg.ctx = ctx;
    return true;
}

static bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void updateCursor(HWND hwnd, bool locked) {
    if (locked) {
        if (!g_cursorHidden) { ShowCursor(FALSE); g_cursorHidden = true; }
        // Trap the cursor inside the window so it can't reach the screen border.
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) ClipCursor(&rc);
    } else {
        if (g_cursorHidden) { ShowCursor(TRUE); g_cursorHidden = false; }
        ClipCursor(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Player persistence
// ---------------------------------------------------------------------------
static bool loadPlayer(const std::string& dir, Player& p, float& timeOfDay, ItemSlot* inv,
                       ItemSlot& carry, ItemSlot* worn, float& sens, bool& invertY) {
    auto path = saves::utf8Path(dir) / "player.bin";
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4] = { 0, 0, 0, 0 };
    f.read(magic, 4);
    if (std::memcmp(magic, "VLV1", 4) != 0) return false;

    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, t = 0;
    f.read((char*)&x, 8);
    f.read((char*)&y, 8);
    f.read((char*)&z, 8);
    f.read((char*)&yaw, 4);
    f.read((char*)&pitch, 4);
    f.read((char*)&t, 4);
    if (!f) return false;

    f.read((char*)&sens, 4);
    uint8_t iv = 0;
    f.read((char*)&iv, 1);
    f.read((char*)inv, cfg::INVENTORY_SLOTS * sizeof(ItemSlot));
    if (!f) return false;
    invertY = (iv != 0);
    vitals::reset(p.vitals);
    uint8_t priv = 0;
    f.read((char*)&priv, 1);
    if (!f) return false;
    p.privilegeMode = (priv != 0);
    f.read((char*)&p.vitals, sizeof(p.vitals));
    if (!f) return false;
    vitals::sanitize(p.vitals);
    p.dead = !p.privilegeMode && vitals::isDead(p.vitals);

    carry.clear();
    uint8_t cb = 0, cc = 0;
    if (f.read((char*)&cb, 1) && f.read((char*)&cc, 1)) {
        carry.block = cb;
        carry.count = cc;
    }

    // Validate untrusted inventory data (normalize empty / invalid slots).
    for (int i = 0; i < cfg::INVENTORY_SLOTS; i++) {
        if (inv[i].block == AIR || inv[i].count == 0 ||
            inv[i].block >= liveBlockCount() || inv[i].count > loot::maxStack(inv[i].block)) {
            inv[i].clear();
        }
    }
    if (carry.block == AIR || carry.count == 0 || !hold::isCarryBlock(carry.block)
        || carry.block >= liveBlockCount()) {
        carry.clear();
    } else {
        carry.count = 1;
    }

    for (int i = 0; i < wear::Count; i++) worn[i].clear();
    for (int i = 0; i < wear::Count; i++) {
        uint8_t b = 0, c = 0;
        if (!f.read((char*)&b, 1) || !f.read((char*)&c, 1)) break;
        worn[i].block = b;
        worn[i].count = c;
    }
    for (int i = 0; i < wear::Count; i++) {
        ItemSlot& s = worn[i];
        if (s.block == AIR || s.count == 0 || s.block >= liveBlockCount()
            || !wear::isOpen(i) || loot::itemWear(s.block) != i
            || s.count > loot::maxStack(s.block)) {
            s.clear();
        }
    }

    p.pos = { (float)x, (float)y, (float)z };
    p.vel = { 0, 0, 0 };
    p.yaw = yaw;
    p.pitch = pitch;
    p.bodyYaw = yaw;
    timeOfDay = t;
    return true;
}

static void savePlayer(const std::string& dir, const Player& p, float timeOfDay, const ItemSlot* inv,
                       const ItemSlot& carry, const ItemSlot* worn, float sens, bool invertY) {
    std::error_code ec;
    auto folder = saves::utf8Path(dir);
    std::filesystem::create_directories(folder, ec);
    auto path = folder / "player.bin";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    char magic[4] = { 'V', 'L', 'V', '1' };
    double x = (double)p.pos.x, y = (double)p.pos.y, z = (double)p.pos.z;
    float yaw = p.yaw, pitch = p.pitch, t = timeOfDay;
    uint8_t iv = invertY ? 1 : 0;
    f.write(magic, 4);
    f.write((const char*)&x, 8);
    f.write((const char*)&y, 8);
    f.write((const char*)&z, 8);
    f.write((const char*)&yaw, 4);
    f.write((const char*)&pitch, 4);
    f.write((const char*)&t, 4);
    f.write((const char*)&sens, 4);
    f.write((const char*)&iv, 1);
    f.write((const char*)inv, cfg::INVENTORY_SLOTS * sizeof(ItemSlot));
    uint8_t priv = p.privilegeMode ? 1 : 0;
    f.write((const char*)&priv, 1);
    f.write((const char*)&p.vitals, sizeof(p.vitals));
    uint8_t cb = carry.empty() ? (uint8_t)AIR : carry.block;
    uint8_t cc = carry.empty() ? 0 : 1;
    f.write((const char*)&cb, 1);
    f.write((const char*)&cc, 1);
    for (int i = 0; i < wear::Count; i++) {
        uint8_t b = worn[i].empty() ? (uint8_t)AIR : worn[i].block;
        uint8_t c = worn[i].empty() ? 0 : worn[i].count;
        f.write((const char*)&b, 1);
        f.write((const char*)&c, 1);
    }
}

static bool playerOverlapsCell(const IVec3& bp, const Vec3& pp) {
    float hw = cfg::PLAYER_HALF_WIDTH, hgt = cfg::PLAYER_HEIGHT;
    float S = cfg::BLOCK_SCALE;
    float pminx = pp.x - hw, pmaxx = pp.x + hw;
    float pminy = pp.y, pmaxy = pp.y + hgt;
    float pminz = pp.z - hw, pmaxz = pp.z + hw;
    float minx = (float)bp.x * S, maxx = (float)bp.x * S + S;
    float miny = (float)bp.y * S, maxy = (float)bp.y * S + S;
    float minz = (float)bp.z * S, maxz = (float)bp.z * S + S;
    return (pminx < maxx && pmaxx > minx && pminy < maxy && pmaxy > miny && pminz < maxz && pmaxz > minz);
}

// ---------------------------------------------------------------------------
// Inventory helpers (drag & drop, quantities).
// ---------------------------------------------------------------------------
struct DragState {
    bool active = false;
    uint8_t block = AIR;
    int count = 0;
    int sourceSlot = -1; // inventory index; -1 when not taken from the bag
    int sourceWear = -1; // wear::Slot; -1 when not taken from a wear slot
};

static int inventoryAdd(ItemSlot* inv, uint8_t block, int count) {
    if (block == AIR || count <= 0) return count;
    const int cap = (int)loot::maxStack(block);
    for (int i = 0; i < cfg::INVENTORY_SLOTS && count > 0; i++) {
        if (inv[i].block == block && inv[i].count < cap) {
            int add = std::min(count, cap - (int)inv[i].count);
            inv[i].count += (uint8_t)add;
            count -= add;
        }
    }
    for (int i = 0; i < cfg::INVENTORY_SLOTS && count > 0; i++) {
        if (inv[i].empty()) {
            int add = std::min(count, cap);
            inv[i].block = block;
            inv[i].count = (uint8_t)add;
            count -= add;
        }
    }
    return count;
}

static Vec3 cellCenter(int x, int y, int z) {
    const float S = cfg::BLOCK_SCALE;
    // Stay inside the cell. A lower point overlaps the block underneath, and the
    // drop's upward pop is then treated as a ceiling hit and pushed underground.
    return { (x + 0.5f) * S, (y + 0.5f) * S, (z + 0.5f) * S };
}

static void dropCarriedBlock(World& world, const Player& player, ItemSlot& carry) {
    if (carry.empty() || !hold::isCarryBlock(carry.block)) {
        carry.clear();
        return;
    }
    Vec3 dir = player.lookDir();
    Vec3 pos = player.eye() + dir * 0.55f;
    const float toss = 2.0f;
    world.spawnDrop(pos, carry.block, 1, false, dir * toss);
    carry.clear();
}

static void spawnHarvestDrops(World& world, const Vec3& pos, uint8_t block, uint8_t held, int extraGrass = 0) {
    loot::DropSpec spec[4];
    int n = loot::harvestDrops(block, held, spec, 4);
    for (int i = 0; i < n; i++) {
        int c = (int)spec[i].count;
        if (extraGrass > 0 && spec[i].item == GRASS_ITEM) c += extraGrass;
        if (c > 0) world.spawnDrop(pos, spec[i].item, c, true);
    }
}

static void finishMinedBlock(World& world, uint8_t held, int physHit, const IVec3& hit) {
    if (physHit >= 0) {
        uint8_t b = world.getPhysBlock(physHit, hit.x, hit.y, hit.z);
        if (b == AIR || !plugin::blockStrategy(b)->canBreak(b)) return;
        Vec3 dropPos = tree_fall::worldOf(world.physicsIslands()[(size_t)physHit], hit.x, hit.y, hit.z);
        bool harvest = loot::isHarvestBreak(b, held);
        uint8_t drop = AIR;
        world.breakPhysBlock(physHit, hit.x, hit.y, hit.z, drop);
        if (harvest && drop != AIR) spawnHarvestDrops(world, dropPos, drop, held);
        return;
    }
    uint8_t b = world.getBlock(hit.x, hit.y, hit.z);
    if (b == AIR || !plugin::blockStrategy(b)->canBreak(b)) return;
    int extraGrass = 0;
    if (b == GRASS_TUFT && hit.y + 1 < cfg::CHUNK_H
        && world.getBlock(hit.x, hit.y + 1, hit.z) == GRASS_TUFT)
        extraGrass = 1;
    bool harvest = loot::isHarvestBreak(b, held);
    int barkN = world.takeAllBarkAt(hit.x, hit.y, hit.z);
    Vec3 dropPos = cellCenter(hit.x, hit.y, hit.z);
    world.setBlock(hit.x, hit.y, hit.z, AIR, true);
    if (harvest) {
        spawnHarvestDrops(world, dropPos, b, held, extraGrass);
        if (barkN > 0) world.spawnDrop(dropPos, BARK, barkN, true);
    }
}

static void clearDrag(DragState& d) {
    d.active = false;
    d.block = AIR;
    d.count = 0;
    d.sourceSlot = -1;
    d.sourceWear = -1;
}

static void startDrag(ItemSlot* inv, int slot, bool whole, DragState& d) {
    if (slot < 0 || slot >= cfg::INVENTORY_SLOTS || inv[slot].empty()) return;
    d.block = inv[slot].block;
    d.count = whole ? (int)inv[slot].count : 1;
    d.sourceSlot = slot;
    d.sourceWear = -1;
    d.active = true;
    inv[slot].count -= (uint8_t)d.count;
    if (inv[slot].count <= 0) inv[slot].clear();
}

static void startDragWear(ItemSlot* worn, int slot, bool whole, DragState& d) {
    if (slot < 0 || slot >= wear::Count || !wear::isOpen(slot) || worn[slot].empty()) return;
    d.block = worn[slot].block;
    d.count = whole ? (int)worn[slot].count : 1;
    d.sourceSlot = -1;
    d.sourceWear = slot;
    d.active = true;
    worn[slot].count -= (uint8_t)d.count;
    if (worn[slot].count <= 0) worn[slot].clear();
}

static int putStack(ItemSlot& s, uint8_t block, int count) {
    if (count <= 0) return 0;
    int cap = (int)loot::maxStack(block);
    if (s.empty()) {
        int add = std::min(count, cap);
        s.block = block;
        s.count = (uint8_t)add;
        return add;
    }
    if (s.block != block || s.count >= cap) return 0;
    int add = std::min(count, cap - (int)s.count);
    s.count += (uint8_t)add;
    return add;
}

static void refundDrag(ItemSlot* inv, ItemSlot* worn, DragState& d, int leftover) {
    if (leftover <= 0) return;
    if (d.sourceWear >= 0 && d.sourceWear < wear::Count)
        leftover -= putStack(worn[d.sourceWear], d.block, leftover);
    if (leftover > 0 && d.sourceSlot >= 0 && d.sourceSlot < cfg::INVENTORY_SLOTS)
        leftover -= putStack(inv[d.sourceSlot], d.block, leftover);
    if (leftover > 0) inventoryAdd(inv, d.block, leftover);
}

static void endDrag(ItemSlot* inv, ItemSlot* worn, int targetSlot, int targetWear, DragState& d) {
    if (!d.active) return;
    int placed = 0;
    ItemSlot displaced;
    displaced.clear();
    bool swapped = false;

    auto takeSwap = [&](ItemSlot& t) {
        displaced = t;
        t.clear();
        placed = putStack(t, d.block, d.count);
        if (placed > 0) swapped = true;
        else {
            t = displaced;
            displaced.clear();
        }
    };

    if (targetWear >= 0 && targetWear < wear::Count && wear::isOpen(targetWear)
        && loot::itemWear(d.block) == targetWear) {
        ItemSlot& t = worn[targetWear];
        if (t.empty() || t.block == d.block) placed = putStack(t, d.block, d.count);
        else takeSwap(t);
    } else if (targetSlot >= 0 && targetSlot < cfg::INVENTORY_SLOTS) {
        ItemSlot& t = inv[targetSlot];
        if (t.empty() || t.block == d.block) placed = putStack(t, d.block, d.count);
        else if (d.sourceWear >= 0 && loot::itemWear(t.block) == d.sourceWear) takeSwap(t);
    }

    if (swapped && !displaced.empty()) {
        int left = (int)displaced.count;
        ItemSlot* origin = nullptr;
        if (d.sourceWear >= 0) origin = &worn[d.sourceWear];
        else if (d.sourceSlot >= 0 && d.sourceSlot < cfg::INVENTORY_SLOTS) origin = &inv[d.sourceSlot];
        if (origin) left -= putStack(*origin, displaced.block, left);
        if (left > 0) inventoryAdd(inv, displaced.block, left);
    }
    refundDrag(inv, worn, d, d.count - placed);
    clearDrag(d);
}

// ---------------------------------------------------------------------------
// Headless stress test (no window / no GL): exercises generation, meshing,
// raycast, block edits, and save/load round-trips repeatedly.
// ---------------------------------------------------------------------------
static int runSelfTest(uint32_t seed, int iterations) {
    printf("=== VOXEL LEGEND self-test ===\n");
    printf("seed=%u  iterations=%d\n", seed, iterations);
    std::string tmpDir = "selftest_save";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);

    World world(seed);
    world.setSaveDir(tmpDir);
    world.setSaveEnabled(true);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> ang(-3.14159f, 3.14159f);

    int placed = 0, broken = 0;
    bool ok = true;
    try {
        for (int it = 0; it < iterations; it++) {
            float px = ((float)(it % 40) - 20.0f) * 37.0f + 8.5f;
            float pz = ((float)(it / 40) - 20.0f) * 37.0f + 8.5f;
            Vec3 pos{ px, 0.0f, pz };
            world.update(pos, 64);
            if (world.loadedChunks() <= 0) { ok = false; printf("FAIL: no chunks loaded @it=%d\n", it); break; }

            int h = world.surfaceHeight((int)px, (int)pz);
            float a = ang(rng);
            Vec3 dir{ std::cos(a) * 0.6f, -0.4f, std::sin(a) * 0.6f };
            IVec3 hit, prev;
            Vec3 nrm;
            if (world.raycast({ px, (float)h + 1.0f, pz }, dir.normalized(), cfg::REACH, hit, prev, nrm)) {
                if (world.getBlock(hit.x, hit.y, hit.z) != BEDROCK) {
                    world.setBlock(hit.x, hit.y, hit.z, AIR, true);
                    broken++;
                }
            }
            // Placement test: find an air cell above the surface and fill it.
            int py = h + 2;
            if (py < cfg::CHUNK_H - 1 && world.getBlock((int)px, py, (int)pz) == AIR) {
                world.setBlock((int)px, py, (int)pz, STONE, true);
                placed++;
            }

            if (it % 20 == 0) {
                world.saveAll();
                World w2(seed);
                w2.setSaveDir(tmpDir);
                w2.setSaveEnabled(true);
                w2.update(pos, 4);
                if (w2.loadedChunks() <= 0) { ok = false; printf("FAIL: reload produced no chunks\n"); break; }
            }
        }
    } catch (const std::exception& e) {
        ok = false;
        printf("FAIL: exception: %s\n", e.what());
    }

    world.saveAll();
    printf("placed=%d broken=%d loadedChunks=%d pendingMeshes=%d\n",
           placed, broken, world.loadedChunks(), world.pendingMeshes());
    std::filesystem::remove_all(tmpDir, ec);
    printf(ok ? "SELF-TEST PASS\n" : "SELF-TEST FAIL\n");
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Headless grass-sod test: (1) place glass over a sod face, fast-forward game
// ticks, and verify the sod darkens stage-by-stage and is removed after the last
// stage; (2) break a surface dirt block and verify its sod is removed promptly
// and the freshly exposed dirt below stays bare (no instant regrowth); (3) verify
// the bare pit persists across a save/load round-trip.
// ---------------------------------------------------------------------------
static int sodStageOf(const World& w, int x, int y, int z, int face) {
    int cx = floorDiv(x, cfg::CHUNK_X), cz = floorDiv(z, cfg::CHUNK_Z);
    auto it = w.chunks().find(chunkKey(cx, cz));
    if (it == w.chunks().end()) return -1;
    int lx = x - cx * cfg::CHUNK_X, lz = z - cz * cfg::CHUNK_Z;
    for (const auto& sf : it->second.sodFaces) {
        if ((int)sf.x == lx && (int)sf.y == y && (int)sf.z == lz && (int)sf.face == face)
            return (int)sf.stage;
    }
    return -1; // absent (removed or never grown)
}

static bool findTopSodDirt(const World& w, int& ox, int& oy, int& oz) {
    for (const auto& [key, ch] : w.chunks()) {
        int cx = chunkCX(key), cz = chunkCZ(key);
        for (const auto& sf : ch.sodFaces) {
            if (sf.face != 0 || ch.get(sf.x, sf.y, sf.z) != DIRT) continue;
            int wx = cx * cfg::CHUNK_X + sf.x;
            int wz = cz * cfg::CHUNK_Z + sf.z;
            if (w.getBlock(wx, sf.y + 1, wz) != AIR) continue;
            ox = wx; oy = sf.y; oz = wz;
            return true;
        }
    }
    return false;
}

static int runWitherTest(uint32_t seed) {
    printf("=== Grass-sod test (seed=%u) ===\n", seed);
    std::string tmpDir = "sodtest_save";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);

    World world(seed);
    world.setSaveDir(tmpDir);
    world.setSaveEnabled(true);

    Vec3 pos{ 8.0f, 0.0f, 8.0f };
    for (int i = 0; i < 16; i++) world.update(pos, 64); // fully load the radius

    bool ok = true;

    // ---- Part 1: wither through glass ----
    int dx = 0, dy = 0, dz = 0;
    if (!findTopSodDirt(world, dx, dy, dz)) { printf("FAIL: no dirt block with a top sod face found\n"); return 2; }
    printf("[wither] target dirt (%d,%d,%d), top sod stage = %d (green)\n",
           dx, dy, dz, sodStageOf(world, dx, dy, dz, 0));

    // Glass is transparent (so the fade is observable through it) but NOT air,
    // so the sod stops touching air and starts to wither.
    world.setBlock(dx, dy + 1, dz, GLASS, true);
    printf("[wither] placed glass at (%d,%d,%d)\n", dx, dy + 1, dz);
    world.sodTick(1 << 30, 0); // detect coverage and start the wither clock
    for (int t = 1; t <= cfg::SOD_STAGES; t++) {
        uint64_t wick = (uint64_t)t * (uint64_t)cfg::WITHER_STAGE_TICKS;
        world.sodTick(1 << 30, wick);
        int s = sodStageOf(world, dx, dy, dz, 0);
        if (s < 0) printf("[wither] tick %llu: top sod = REMOVED\n", (unsigned long long)wick);
        else       printf("[wither] tick %llu: top sod stage = %d\n", (unsigned long long)wick, s);
        if (t < cfg::SOD_STAGES && s != t) {
            ok = false;
            printf("  expected stage %d\n", t);
        } else if (t == cfg::SOD_STAGES && s != -1) {
            ok = false;
            printf("  expected REMOVED after %d stages\n", cfg::SOD_STAGES);
        }
    }
    // Breaking the glass re-exposes the dirt; the sod must stay bare (no regrowth).
    world.setBlock(dx, dy + 1, dz, AIR, true);
    if (sodStageOf(world, dx, dy, dz, 0) != -1) {
        ok = false;
        printf("[wither] sod regrew after glass was removed (expected bare)\n");
    } else {
        printf("[wither] broke glass; sod stays bare (no instant regrowth) OK\n");
    }

    // ---- Part 2: breaking supporting dirt removes its sod promptly ----
    int sx = 0, sy = 0, sz = 0;
    if (!findTopSodDirt(world, sx, sy, sz)) { printf("FAIL: no fresh surface dirt for dig test\n"); return 2; }
    printf("[dig] surface dirt (%d,%d,%d) top sod = %d; dirt below top sod = %d\n",
           sx, sy, sz, sodStageOf(world, sx, sy, sz, 0), sodStageOf(world, sx, sy - 1, sz, 0));

    world.setBlock(sx, sy, sz, AIR, true); // break the sod's supporting dirt
    if (world.getBlock(sx, sy, sz) != AIR) { ok = false; printf("[dig] surface block was not removed\n"); }
    {
        // The affected chunk's mesh must be rebuilt synchronously (no ghost block).
        int dcx = floorDiv(sx, cfg::CHUNK_X), dcz = floorDiv(sz, cfg::CHUNK_Z);
        auto dit = world.chunks().find(chunkKey(dcx, dcz));
        if (dit != world.chunks().end() && dit->second.dirty) {
            ok = false;
            printf("[dig] chunk still dirty after setBlock (mesh not rebuilt synchronously)\n");
        } else {
            printf("[dig] chunk mesh rebuilt synchronously OK\n");
        }
    }
    if (sodStageOf(world, sx, sy, sz, 0) != -1) {
        ok = false;
        printf("[dig] sod was not removed with its dirt\n");
    } else {
        printf("[dig] breaking dirt removed its sod promptly OK\n");
    }
    if (sodStageOf(world, sx, sy - 1, sz, 0) != -1) {
        ok = false;
        printf("[dig] freshly exposed dirt grew sod (expected bare pit)\n");
    } else {
        printf("[dig] freshly exposed dirt stays bare OK\n");
    }
    {
        // Adjacent dirt side faces newly exposed by the dig must also stay bare.
        struct N { int dx, dz, face; };
        const N ns[4] = { {1,0,3}, {-1,0,2}, {0,1,5}, {0,-1,4} };
        bool sideSod = false;
        for (const N& n : ns) {
            if (world.getBlock(sx + n.dx, sy, sz + n.dz) != DIRT) continue;
            if (sodStageOf(world, sx + n.dx, sy, sz + n.dz, n.face) != -1) {
                sideSod = true;
                printf("[dig] adjacent dirt side face (%d,%d,%d face %d) has sod (expected bare)\n",
                       sx + n.dx, sy, sz + n.dz, n.face);
            }
        }
        if (sideSod) ok = false;
        else printf("[dig] adjacent dirt side faces stay bare OK\n");
    }

    // ---- Part 3: generation sods surface dirt (top + side) but not deep dirt ----
    {
        // Use a pristine world so the wither/dig mutations above can't affect this check.
        World w3(seed);
        w3.setSaveEnabled(false);
        for (int i = 0; i < 16; i++) w3.update(pos, 64);
        bool foundDeep = false, deepSod = false;
        int ddx = 0, ddy = 0, ddz = 0;
        bool foundCliff = false, cliffMissing = false;
        int ccx = 0, ccy = 0, ccz = 0;
        for (const auto& [key, ch] : w3.chunks()) {
            int cx = chunkCX(key), cz = chunkCZ(key);
            for (int y = 1; y < cfg::CHUNK_H - 1; y++) {
                for (int z = 0; z < cfg::CHUNK_Z; z++) {
                    for (int x = 0; x < cfg::CHUNK_X; x++) {
                        if (ch.get(x, y, z) != DIRT) continue;
                        int above = ch.get(x, y + 1, z);
                        bool surface = !isOpaque((uint8_t)above);
                        int wx = cx * cfg::CHUNK_X + x;
                        int wz = cz * cfg::CHUNK_Z + z;
                        bool s2 = (w3.getBlock(wx + 1, y, wz) == AIR);
                        bool s3 = (w3.getBlock(wx - 1, y, wz) == AIR);
                        bool s4 = (w3.getBlock(wx, y, wz + 1) == AIR);
                        bool s5 = (w3.getBlock(wx, y, wz - 1) == AIR);
                        if (!(s2 || s3 || s4 || s5)) continue;
                        if (!surface) {
                            if (!foundDeep) { foundDeep = true; ddx = wx; ddy = y; ddz = wz; }
                            for (const auto& sf : ch.sodFaces) {
                                if ((int)sf.x == x && (int)sf.y == y && (int)sf.z == z) { deepSod = true; break; }
                            }
                        } else {
                            if (!foundCliff) { foundCliff = true; ccx = wx; ccy = y; ccz = wz; }
                            int need[4] = { 2, 3, 4, 5 };
                            int sdx[4] = { 1, -1, 0, 0 };
                            int sdz[4] = { 0, 0, 1, -1 };
                            for (int k = 0; k < 4; k++) {
                                int nxw = wx + sdx[k], nzw = wz + sdz[k];
                                int ncx = floorDiv(nxw, cfg::CHUNK_X), ncz = floorDiv(nzw, cfg::CHUNK_Z);
                                if (w3.chunks().find(chunkKey(ncx, ncz)) == w3.chunks().end()) continue; // deferred
                                if (w3.getBlock(nxw, y, nzw) != AIR) continue; // covered
                                bool has = false;
                                for (const auto& sf : ch.sodFaces) {
                                    if ((int)sf.x == x && (int)sf.y == y && (int)sf.z == z && (int)sf.face == need[k]) { has = true; break; }
                                }
                                if (!has && !cliffMissing) { cliffMissing = true; ccx = wx; ccy = y; ccz = wz; }
                            }
                        }
                    }
                }
            }
        }
        if (foundDeep && deepSod) {
            ok = false;
            printf("[gen] deep exposed dirt (%d,%d,%d) grew sod (expected bare)\n", ddx, ddy, ddz);
        } else if (foundDeep) {
            printf("[gen] deep exposed dirt (%d,%d,%d) stays bare OK\n", ddx, ddy, ddz);
        } else {
            printf("[gen] no deep exposed dirt found (skipped)\n");
        }
        if (foundCliff && cliffMissing) {
            ok = false;
            printf("[gen] surface cliff dirt (%d,%d,%d) missing side sod\n", ccx, ccy, ccz);
        } else if (foundCliff) {
            printf("[gen] surface cliff dirt (%d,%d,%d) has side sod OK\n", ccx, ccy, ccz);
        } else {
            printf("[gen] no surface cliff dirt found (skipped)\n");
        }
    }

    // ---- Part 4: the bare pit persists across a save/load round-trip ----
    world.saveAll();
    World w2(seed);
    w2.setSaveDir(tmpDir);
    w2.setSaveEnabled(true);
    w2.update(pos, 4);
    if (w2.getBlock(sx, sy, sz) != AIR) { ok = false; printf("[persist] dug block did not reload as AIR\n"); }
    if (sodStageOf(w2, sx, sy - 1, sz, 0) != -1) { ok = false; printf("[persist] bare pit regrew sod after reload\n"); }
    else printf("[persist] bare pit stayed bare after save/load OK\n");

    std::filesystem::remove_all(tmpDir, ec);
    printf(ok ? "SOD TEST PASS\n" : "SOD TEST FAIL\n");
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Headless water-flow smoke test: verify downward flow and horizontal diffusion
// of a placed temporary source (no window / no GL).
// ---------------------------------------------------------------------------
static int runWaterTest(uint32_t seed) {
    printf("=== Water flow test (seed=%u) ===\n", seed);
    World world(seed);
    world.setSaveEnabled(false);
    Vec3 pos{ 8.0f, 0.0f, 8.0f };
    for (int i = 0; i < 8; i++) world.update(pos, 64);

    bool ok = true;
    const int px = 10, pz = 10, py = 100; // high above the surface

    // Diffusion: a source on a stone platform spreads to horizontal neighbors.
    world.setBlock(px, py, pz, STONE, true);
    world.setBlock(px, py + 1, pz, WATER, true);
    printf("[diff] placed source at (%d,%d,%d), level=%d\n",
           px, py + 1, pz, world.getWaterLevel(px, py + 1, pz));
    world.waterTick(0);
    int spread = 0;
    const int DX[4] = { 1, -1, 0, 0 }, DZ[4] = { 0, 0, 1, -1 };
    for (int d = 0; d < 4; d++) if (world.getWaterLevel(px + DX[d], py + 1, pz + DZ[d]) > 0) spread++;
    int srcLvl = world.getWaterLevel(px, py + 1, pz);
    printf("[diff] after 1 tick: source level=%d, spread neighbors=%d\n", srcLvl, spread);
    if (spread == 0 || srcLvl >= 16) { ok = false; printf("  FAIL: no diffusion\n"); }

    // Cleanup, then flow: a source in air falls one cell per tick.
    for (int y = py; y <= py + 6; y++)
        for (int dx = -2; dx <= 2; dx++)
            for (int dz = -2; dz <= 2; dz++)
                world.setBlock(px + dx, y, pz + dz, AIR, true);
    world.setBlock(px, py, pz, STONE, true);
    world.setBlock(px, py + 4, pz, WATER, true);
    printf("[flow] placed source at (%d,%d,%d)\n", px, py + 4, pz);
    world.waterTick(1);
    bool moved = (world.getWaterLevel(px, py + 3, pz) > 0);
    bool gone = (world.getWaterLevel(px, py + 4, pz) == 0);
    printf("[flow] after 1 tick: y+4 level=%d, y+3 level=%d\n",
           world.getWaterLevel(px, py + 4, pz), world.getWaterLevel(px, py + 3, pz));
    if (!moved || !gone) { ok = false; printf("  FAIL: no downward flow\n"); }

    // --- Evaporation: in dry air the water level decreases over time ---
    {
        int ex = -1, ez = -1, ey = 100;
        for (int dx = -300; dx <= 300 && ex < 0; dx++)
            for (int dz = -300; dz <= 300 && ex < 0; dz++)
                if (world.humidityAt(10 + dx, ey, 10 + dz) < -60) { ex = 10 + dx; ez = 10 + dz; }
        if (ex < 0) {
            printf("[evap] no dry location found (skipped)\n");
        } else {
            // Load chunks around the dry location before placing anything.
            const float S = cfg::BLOCK_SCALE;
            Vec3 wp{ (ex + 0.5f) * S, (ey + 0.5f) * S, (ez + 0.5f) * S };
            for (int i = 0; i < 8; i++) world.update(wp, 64);
            // 5x5 stone platform so the puddle stays at this height while evaporating.
            for (int dx = -2; dx <= 2; dx++)
                for (int dz = -2; dz <= 2; dz++)
                    world.setBlock(ex + dx, ey, ez + dz, STONE, true);
            world.setBlock(ex, ey + 1, ez, WATER, true);
            auto sum = [&](int cx, int cz) {
                int s = 0;
                for (int dx = -2; dx <= 2; dx++)
                    for (int dz = -2; dz <= 2; dz++)
                        s += (int)world.getWaterLevel(cx + dx, ey + 1, cz + dz);
                return s;
            };
            int before = sum(ex, ez);
            for (uint64_t t = 0; t < 4000; t++) world.waterTick(t);
            int after = sum(ex, ez);
            printf("[evap] dry(%d): total level %d -> %d after 4000 ticks\n", world.humidityAt(ex, ey, ez), before, after);
            if (after >= before) { ok = false; printf("  FAIL: no evaporation\n"); }
        }
    }

    // --- Merge: water above water falls and merges instead of stacking ---
    {
        int mx = 10, mz = 30, my = 100;
        // Reload chunks around the merge-test location (evaporation test may have
        // moved the world generation elsewhere).
        const float S = cfg::BLOCK_SCALE;
        Vec3 wp{ (mx + 0.5f) * S, (my + 0.5f) * S, (mz + 0.5f) * S };
        for (int i = 0; i < 8; i++) world.update(wp, 64);
        for (int y = my; y <= my + 3; y++)
            for (int dx = -2; dx <= 2; dx++)
                for (int dz = -2; dz <= 2; dz++)
                    world.setBlock(mx + dx, y, mz + dz, AIR, true);
        world.setBlock(mx, my, mz, STONE, true);
        world.setBlock(mx, my + 1, mz, WATER, true);
        world.setBlock(mx, my + 2, mz, WATER, true);
        world.waterTick(2);
        int topL = world.getWaterLevel(mx, my + 2, mz);
        int botL = world.getWaterLevel(mx, my + 1, mz);
        printf("[merge] after tick: top(y+2)=%d, bottom(y+1)=%d\n", topL, botL);
        if (topL != 0 || botL == 0) { ok = false; printf("  FAIL: no merge\n"); }
    }

    printf(ok ? "WATER TEST PASS\n" : "WATER TEST FAIL\n");
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    plugin::init();
    data::init();
    SetUnhandledExceptionFilter(crashFilter);
    SetProcessDPIAware();
    uint32_t seed = 1337;
    bool noSave = false;
    bool sim = false;
    bool witherTest = false;
    bool waterTest = false;
    int frames = 0;       // 0 = run until window closed
    int selfTest = -1;    // -1 = disabled
    bool seedData = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--selftest" && i + 1 < argc) { selfTest = std::atoi(argv[++i]); }
        else if (a == "--seed-data") { seedData = true; }
        else if (a == "--seed" && i + 1 < argc) { seed = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        else if (a == "--frames" && i + 1 < argc) { frames = std::atoi(argv[++i]); }
        else if (a == "--no-save") { noSave = true; }
        else if (a == "--sim") { sim = true; }
        else if (a == "--withertest") { witherTest = true; }
        else if (a == "--watertest") { waterTest = true; }
        else if (a == "--help") {
            printf("VOXEL LEGEND\n");
            printf("  --selftest N   run N iterations of a headless stress test and exit\n");
            printf("  --withertest   run a headless grass-sod wither smoke test and exit\n");
            printf("  --watertest    run a headless water-flow smoke test and exit\n");
            printf("  --frames N     run for N frames then exit cleanly (smoke test)\n");
            printf("  --sim          simulate walking + turning (for testing)\n");
            printf("  --seed N       seed used when creating a new world\n");
            printf("  --no-save      disable save/load\n");
            printf("  --seed-data    write missing assets/data files and exit\n");
            return 0;
        }
    }

    if (seedData) {
        printf("data pack ready under assets/data/\n");
        return 0;
    }
    if (selfTest >= 0) return runSelfTest(seed, selfTest);
    if (witherTest) return runWitherTest(seed);
    if (waterTest) return runWaterTest(seed);

    WinGL wg;
    if (!createWindowAndContext(wg, 1280, 720)) {
        fprintf(stderr, "Failed to create an OpenGL 3.3 core context.\n");
        return 1;
    }
    if (!gl::loadAll()) {
        fprintf(stderr, "Failed to load OpenGL functions.\n");
        return 1;
    }
    printf("GL_VERSION: %s\n", (const char*)gl::GetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", (const char*)gl::GetString(GL_RENDERER));

    // Raw mouse input (1:1 device movement, bypasses Windows pointer acceleration).
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = 0;
    rid.hwndTarget = wg.hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    // Raw input reports hardware "mickeys", not cursor pixels. Normalize to a
    // pixel-equivalent scale using the system pointer speed so the in-game
    // sensitivity matches what the user expects from the desktop cursor.
    int mouseSpeed = 10;
    SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &mouseSpeed, 0);
    if (mouseSpeed < 1) mouseSpeed = 1;
    if (mouseSpeed > 20) mouseSpeed = 20;
    g_mouseScale = (float)mouseSpeed / 10.0f;

    g_screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_screenW < 1) g_screenW = GetSystemMetrics(SM_CXSCREEN);
    if (g_screenH < 1) g_screenH = GetSystemMetrics(SM_CYSCREEN);
    if (g_screenW < 1) g_screenW = 1280;
    if (g_screenH < 1) g_screenH = 720;
    // Absolute devices (RDP/VM) can't be recentered, so map one full [0,65535]
    // sweep to multiple full turns. The sensitivity slider scales this further.
    const float fullTurnsPerSweep = 2.0f;
    g_absScaleX = (fullTurnsPerSweep * 2.0f * 3.14159265358979f) / (65535.0f * cfg::SENS_DEFAULT);
    g_absScaleY = g_absScaleX * ((float)g_screenH / (float)g_screenW);

    g_winW = 1280;
    g_winH = 720;
    RECT cr{ 0, 0, 0, 0 };
    if (GetClientRect(wg.hwnd, &cr) && cr.right > 0 && cr.bottom > 0) {
        g_winW = cr.right;
        g_winH = cr.bottom;
    }
    printf("window: %dx%d visible=%d\n", g_winW, g_winH, (int)IsWindowVisible(wg.hwnd));

    Renderer renderer;
    if (!renderer.init(g_winW, g_winH)) {
        fprintf(stderr, "Renderer init failed.\n");
        return 1;
    }

    // Bootstrap the file-based player model (assets/entities/player.model) so it
    // exists on first run even if the player is never rendered in third person.
    pm::buildPlayerModel();

    World world(seed);
    world.setSaveEnabled(false);

    Player player;
    ItemSlot inv[cfg::INVENTORY_SLOTS];
    for (int i = 0; i < cfg::INVENTORY_SLOTS; i++) inv[i].clear();
    ItemSlot carry;
    carry.clear();
    ItemSlot worn[wear::Count];
    for (int i = 0; i < wear::Count; i++) worn[i].clear();
    float timeOfDay = 6000.0f; // morning
    float mouseSens = cfg::SENS_DEFAULT;
    bool invertY = false;
    std::string currentWorld;
    AppScreen appScreen = AppScreen::Start;

    bool running = true;
    bool paused = false;
    bool settingsOpen = false;
    bool inventoryOpen = false;
    bool showDebug = false;
    bool debugMenuOpen = false;
    bool humidityMode = false;
    std::vector<RoomTeamView> roomTeams;
    std::vector<RoomPlayerView> roomPlayers;
    bool roomSession = false;
    bool spectating = false;
    constexpr int kRoomMinPlayers = 1;
    float tickSpeed = 1.0f;
    float gameTickAccum = 0.0f;
    uint64_t gameTick = 0;
    bool prevF3 = false, prevF = false, prevF5 = false, prevE = false, prevEsc = false;
    bool prevLmb = false, prevRmb = false;
    bool firstLook = true;
    DragState drag;
    float accumulator = 0.0f;

    UIState ui;
    ui.inventory = inv;
    ui.wear = worn;
    ui.carrySlot = &carry;
    ui.appScreen = AppScreen::Start;

    auto spawnPlayer = [&]() {
        int h = world.surfaceHeight(8, 8);
        const float S = cfg::BLOCK_SCALE;
        player.setSpawn({ 8.5f * S, (float)(h + 3) * S, 8.5f * S });
        auto collides = [&](float x, float y, float z) {
            float hw = cfg::PLAYER_HALF_WIDTH, hgt = cfg::PLAYER_HEIGHT;
            int x0 = (int)std::floor((x - hw) / S), x1 = (int)std::floor((x + hw - 1e-6f) / S);
            int y0 = (int)std::floor(y / S), y1 = (int)std::floor((y + hgt - 1e-6f) / S);
            int z0 = (int)std::floor((z - hw) / S), z1 = (int)std::floor((z + hw - 1e-6f) / S);
            for (int bx = x0; bx <= x1; bx++)
                for (int by = y0; by <= y1; by++)
                    for (int bz = z0; bz <= z1; bz++)
                        if (isSolid(world.getBlock(bx, by, bz))) return true;
            return false;
        };
        float sy = player.pos.y;
        float maxY = (float)(cfg::CHUNK_H - 4) * cfg::BLOCK_SCALE;
        while (sy < maxY && collides(player.pos.x, sy, player.pos.z)) sy += cfg::BLOCK_SCALE;
        player.pos.y = sy;
    };

    auto refreshWorldList = [&]() {
        ui.worldNames = saves::listWorlds();
        ui.worldScroll = 0;
        ui.worldItemHover = -1;
        ui.worldDeleteHover = -1;
    };

    auto refreshBackups = [&]() {
        ui.backupNames = saves::listBackups(ui.selectedWorld);
        if (ui.selectedBackup >= (int)ui.backupNames.size()) ui.selectedBackup = -1;
        ui.backupScroll = 0;
        ui.backupItemHover = -1;
    };

    auto enterWorld = [&](const std::string& name) {
        spectating = false;
        roomSession = false;
        player.noclip = false;
        std::string dir = saves::slotDir(name, saves::kActive);
        uint32_t s = saves::readSeed(dir, seed);
        world.reset(s);
        world.setSaveDir(dir);
        world.setSaveEnabled(!noSave);
        seed = s;
        currentWorld = name;
        player = Player();
        if (const plugin::EntityModule* em = plugin::findEntity("player")) {
            plugin::EntityEvent ev{ &world, &player, 0.0f };
            em->strategy->onSpawn(ev);
        }
        for (int i = 0; i < cfg::INVENTORY_SLOTS; i++) inv[i].clear();
        for (int i = 0; i < wear::Count; i++) worn[i].clear();
        carry.clear();
        timeOfDay = 6000.0f;
        if (!noSave && loadPlayer(dir, player, timeOfDay, inv, carry, worn, mouseSens, invertY)) {
            // restored
        } else {
            spawnPlayer();
        }
        world.update(player.pos, 40);
        {
            auto collides = [&](float x, float y, float z) {
                float hw = cfg::PLAYER_HALF_WIDTH, hgt = cfg::PLAYER_HEIGHT;
                float S = cfg::BLOCK_SCALE;
                int x0 = (int)std::floor((x - hw) / S), x1 = (int)std::floor((x + hw - 1e-6f) / S);
                int y0 = (int)std::floor(y / S), y1 = (int)std::floor((y + hgt - 1e-6f) / S);
                int z0 = (int)std::floor((z - hw) / S), z1 = (int)std::floor((z + hw - 1e-6f) / S);
                for (int bx = x0; bx <= x1; bx++)
                    for (int by = y0; by <= y1; by++)
                        for (int bz = z0; bz <= z1; bz++)
                            if (isSolid(world.getBlock(bx, by, bz))) return true;
                return false;
            };
            float sy = player.pos.y;
            float maxY = (float)(cfg::CHUNK_H - 4) * cfg::BLOCK_SCALE;
            while (sy < maxY && collides(player.pos.x, sy, player.pos.z)) sy += cfg::BLOCK_SCALE;
            player.pos.y = sy;
        }
        if (!noSave) saves::writeMeta(dir, seed);
        appScreen = AppScreen::Playing;
        ui.appScreen = AppScreen::Playing;
        g_textTarget = nullptr;
        ui.menuMessage.clear();
        firstLook = true;
        paused = false;
        settingsOpen = false;
        inventoryOpen = false;
        debugMenuOpen = false;
        ui.matEditorOpen = false;
        accumulator = 0.0f;
        gameTickAccum = 0.0f;
        gameTick = 0;
    };

    auto leaveWorld = [&]() {
        if (appScreen == AppScreen::Playing && !roomSession) {
            world.saveAll();
            if (!noSave && !currentWorld.empty())
                savePlayer(world.saveDir(), player, timeOfDay, inv, carry, worn, mouseSens, invertY);
        }
        spectating = false;
        roomSession = false;
        roomTeams.clear();
        roomPlayers.clear();
        world.reset(seed);
        world.setSaveEnabled(false);
        currentWorld.clear();
        player = Player();
        carry.clear();
        player.yaw = 0.4f;
        player.pitch = -0.15f;
        appScreen = AppScreen::Start;
        ui.appScreen = AppScreen::Start;
        paused = false;
        settingsOpen = false;
        inventoryOpen = false;
        debugMenuOpen = false;
        ui.matEditorOpen = false;
        ui.dummyActive = false;
        g_textTarget = nullptr;
        ui.menuMessage.clear();
    };

    const float kTeamCols[8][3] = {
        { 0.86f, 0.22f, 0.22f }, { 0.22f, 0.45f, 0.90f }, { 0.20f, 0.70f, 0.32f },
        { 0.92f, 0.72f, 0.16f }, { 0.58f, 0.28f, 0.82f }, { 0.16f, 0.70f, 0.74f },
        { 0.92f, 0.46f, 0.16f }, { 0.86f, 0.42f, 0.62f },
    };
    auto beginRoom = [&]() {
        roomTeams.clear();
        roomTeams.push_back(RoomTeamView{ "观战", 0.75f, 0.76f, 0.80f, true });
        roomPlayers.clear();
        std::string who = ui.playerName.empty() ? "玩家" : ui.playerName;
        roomPlayers.push_back(RoomPlayerView{ who, -1, true });
        roomSession = false;
        spectating = false;
        uint32_t s = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
        if (s == 0) s = 1;
        world.reset(s);
        world.setSaveEnabled(false);
        seed = s;
        currentWorld.clear();
        player = Player();
        if (const plugin::EntityModule* em = plugin::findEntity("player")) {
            plugin::EntityEvent ev{ &world, &player, 0.0f };
            em->strategy->onSpawn(ev);
        }
        for (int i = 0; i < cfg::INVENTORY_SLOTS; i++) inv[i].clear();
        for (int i = 0; i < wear::Count; i++) worn[i].clear();
        carry.clear();
        timeOfDay = 6000.0f;
        spawnPlayer();
        world.update(player.pos, 40);
        player.yaw = 0.4f;
        player.pitch = -0.15f;
        appScreen = AppScreen::RoomLobby;
        ui.appScreen = AppScreen::RoomLobby;
        ui.menuMessage.clear();
        firstLook = true;
    };
    auto addRoomTeam = [&]() {
        if (roomTeams.size() >= 9) return;
        int i = (int)roomTeams.size() - 1;
        if (i < 0) i = 0;
        RoomTeamView t;
        t.name = "队伍" + std::to_string(i + 1);
        t.r = kTeamCols[i % 8][0];
        t.g = kTeamCols[i % 8][1];
        t.b = kTeamCols[i % 8][2];
        roomTeams.push_back(t);
    };
    auto joinRoomTeam = [&](int team) {
        if (team < 0 || team >= (int)roomTeams.size()) return;
        for (RoomPlayerView& rp : roomPlayers)
            if (rp.local) rp.team = team;
    };
    auto startRoom = [&]() {
        if ((int)roomPlayers.size() < kRoomMinPlayers) return;
        int team = -1;
        for (const RoomPlayerView& rp : roomPlayers)
            if (rp.local) team = rp.team;
        spectating = (team == 0);
        roomSession = true;
        player.flying = spectating;
        player.noclip = spectating;
        player.vel = { 0, 0, 0 };
        player.dead = false;
        if (spectating) ui.camMode = 0;
        appScreen = AppScreen::Playing;
        ui.appScreen = AppScreen::Playing;
        g_textTarget = nullptr;
        ui.menuMessage.clear();
        firstLook = true;
        paused = false;
        settingsOpen = false;
        inventoryOpen = false;
        debugMenuOpen = false;
        ui.matEditorOpen = false;
        accumulator = 0.0f;
        gameTickAccum = 0.0f;
        gameTick = 0;
    };

    auto seedFromName = [](const std::string& name) -> uint32_t {
        unsigned char b[4] = { 0, 0, 0, 0 };
        size_t n = name.size() < 4 ? name.size() : 4;
        for (size_t i = 0; i < n; i++) b[i] = (unsigned char)name[i];
        return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    };
    auto loadProfile = [&]() {
        std::ifstream in(saves::utf8Path(std::string(saves::kRoot) + "/profile.txt"));
        std::string line;
        if (std::getline(in, line) && line.rfind("name ", 0) == 0) {
            ui.playerName = line.substr(5);
            while (!ui.playerName.empty() && (unsigned char)ui.playerName.back() == '\r')
                ui.playerName.pop_back();
        }
        if (ui.playerName.empty()) ui.playerName = "玩家";
    };
    auto saveProfile = [&]() {
        std::filesystem::create_directories(saves::utf8Path(saves::kRoot));
        std::ofstream out(saves::utf8Path(std::string(saves::kRoot) + "/profile.txt"), std::ios::binary);
        if (out) out << "name " << ui.playerName << "\n";
    };
    loadProfile();

    Vec3 menuEye{ 0, 0, 0 }, menuTarget{ 0, 0, 0 }, menuFeet{ 0, 0, 0 };
    uint32_t menuSeedApplied = 0;
    bool menuSeedReady = false;
    auto buildMenuWorld = [&]() {
        uint32_t s = seedFromName(ui.playerName);
        world.reset(s);
        world.setSaveEnabled(false);
        const int ax = 40, az = 40;
        int fx = ax, fz = az, h = world.surfaceHeight(fx, fz);
        auto standOk = [&](int x, int z, int& outH) {
            outH = world.surfaceHeight(x, z);
            if (outH <= cfg::SEA_LEVEL + 1) return false;
            for (int dy = 1; dy <= 4; dy++)
                if (isSolid(world.getBlock(x, outH + dy, z))) return false;
            return true;
        };
        const float S = cfg::BLOCK_SCALE;
        Vec3 probe{ (fx + 0.5f) * S, (h + 2.0f) * S, (fz + 0.5f) * S };
        world.update(probe, 16);
        bool found = standOk(fx, fz, h);
        for (int r = 1; r <= 28 && !found; r++) {
            for (int dz = -r; dz <= r && !found; dz++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (std::abs(dx) != r && std::abs(dz) != r) continue;
                    int hh = 0;
                    if (!standOk(ax + dx, az + dz, hh)) continue;
                    fx = ax + dx;
                    fz = az + dz;
                    h = hh;
                    found = true;
                    break;
                }
            }
        }
        menuFeet = Vec3{ (fx + 0.5f) * S, (h + 1.0f) * S, (fz + 0.5f) * S };
        menuTarget = Vec3{ menuFeet.x, menuFeet.y + 1.5f * S, menuFeet.z };
        menuEye = menuTarget + Vec3{ 16.0f, 9.0f, 18.0f };
        world.update(menuEye, 32);
        world.update(menuFeet + Vec3{ 0.0f, 2.0f, 2.0f }, 16);
        menuSeedApplied = s;
        menuSeedReady = true;
    };
    auto exploreScreen = [&]() {
        return appScreen == AppScreen::Worlds || appScreen == AppScreen::WorldDetail
            || appScreen == AppScreen::CreateWorld;
    };
    auto portraitScreen = [&]() {
        return appScreen == AppScreen::Start || appScreen == AppScreen::PlayerProfile;
    };
    auto openExplore = [&]() {
        uint32_t s = seedFromName(ui.playerName);
        if (!menuSeedReady || menuSeedApplied != s) buildMenuWorld();
        appScreen = AppScreen::Worlds;
        ui.appScreen = AppScreen::Worlds;
        ui.menuMessage.clear();
        refreshWorldList();
    };
    auto importPlayerModel = [&]() {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = wg.hwnd;
        ofn.lpstrFilter = L"模型 (*.model)\0*.model\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        ofn.lpstrTitle = L"导入玩家模型";
        if (!GetOpenFileNameW(&ofn)) return;
        std::filesystem::path src(file);
        std::filesystem::path tmp = saves::utf8Path(pack::join(pack::entitiesDir(), "_import_player.model"));
        std::error_code ec;
        std::filesystem::create_directories(tmp.parent_path(), ec);
        std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            ui.menuMessage = "导入失败";
            return;
        }
        pm::EntityFile ef = pm::loadEntity(pack::join(pack::entitiesDir(), "_import_player.model").c_str());
        if (ef.parts.empty()) {
            std::filesystem::remove(tmp, ec);
            ui.menuMessage = "模型无效";
            return;
        }
        std::filesystem::path dst = saves::utf8Path(pack::entityModel("player"));
        std::filesystem::copy_file(tmp, dst, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code rm;
        std::filesystem::remove(tmp, rm);
        if (ec) {
            ui.menuMessage = "导入失败";
            return;
        }
        if (!ef.skin.empty()) {
            std::filesystem::path skinSrc = src.parent_path() / (ef.skin + ".png");
            if (std::filesystem::exists(skinSrc)) {
                std::filesystem::path skinDst = saves::utf8Path(pack::entityPng(ef.skin));
                std::filesystem::copy_file(skinSrc, skinDst, std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
        renderer.reloadPlayerAssets();
        ui.menuMessage = "已导入模型";
    };

    saves::migrateLegacy(seed);
    if (frames > 0 || sim) {
        auto worlds = saves::listWorlds();
        std::string name = worlds.empty() ? "World" : worlds.front();
        if (worlds.empty()) saves::createWorld(name, seed);
        enterWorld(name);
    } else {
        player.yaw = 0.4f;
        player.pitch = -0.15f;
    }

    // Launch the standalone model editor (editor.exe --model) next to the game
    // executable. Separate process, so the game keeps running; edits to
    // assets/entities/player.model are picked up on the next render.
    auto launchModelEditor = []() {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;
        char* slash = std::strrchr(path, '\\');
        if (!slash) slash = std::strrchr(path, '/');
        if (!slash) return;
        std::strcpy(slash + 1, "editor.exe");
        std::string cmd = std::string("\"") + path + "\" --model";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(path, cmd.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    };

    auto t0 = std::chrono::steady_clock::now();
    float fps = 0.0f, fpsAccum = 0.0f;
    int fpsFrames = 0;
    int frameCounter = 0;

    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            else { TranslateMessage(&msg); DispatchMessage(&msg); }
        }
        if (!running) break;

        auto t1 = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(t1 - t0).count();
        t0 = t1;
        if (dt > 0.1f) dt = 0.1f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        fpsAccum += dt;
        fpsFrames++;
        if (fpsAccum >= 0.5f) {
            fps = (float)fpsFrames / fpsAccum;
            fpsAccum = 0.0f;
            fpsFrames = 0;
        }

        bool playing = (appScreen == AppScreen::Playing);
        bool canMove = (g_focused && playing && !paused && !ui.matEditorOpen && !player.dead);
        bool lookLocked = (canMove && !inventoryOpen);

        InputState in;
        if (canMove) {
            in.forward = keyDown('W');
            in.back = keyDown('S');
            in.left = keyDown('A');
            in.right = keyDown('D');
            in.sprint = keyDown(VK_CONTROL) || (keyDown(VK_SHIFT) && !player.flying);
        }
        in.jump = keyDown(VK_SPACE);
        in.sneak = keyDown(VK_SHIFT);

        if (sim) {
            in.forward = true;
            if ((frameCounter / 150) % 2 == 0) in.jump = true;
        }

        bool f3 = keyDown(VK_F3);
        if (playing && f3 && !prevF3) showDebug = !showDebug;
        prevF3 = f3;
        bool f = keyDown('F');
        bool fPressed = playing && f && !prevF;
        prevF = f;
        bool f5 = keyDown(VK_F5);
        if (playing && f5 && !prevF5 && !spectating) ui.camMode = (ui.camMode + 1) % 3;
        if (spectating) ui.camMode = 0;
        prevF5 = f5;
        bool e = keyDown('E');
        if (playing && e && !prevE && !spectating) {
            if (!paused) {
                if (inventoryOpen && drag.active) endDrag(inv, worn, -1, -1, drag);
                inventoryOpen = !inventoryOpen;
                if (inventoryOpen) { drag.active = false; drag.block = AIR; drag.count = 0; drag.sourceSlot = -1; drag.sourceWear = -1; }
            }
        }
        prevE = e;
        bool esc = keyDown(VK_ESCAPE);
        if (esc && !prevEsc) {
            if (!playing) {
                if (appScreen == AppScreen::CreateWorld) {
                    g_textTarget = nullptr;
                    ui.nameFieldActive = false;
                    appScreen = AppScreen::Worlds;
                    ui.appScreen = AppScreen::Worlds;
                    refreshWorldList();
                } else if (appScreen == AppScreen::WorldDetail) {
                    appScreen = AppScreen::Worlds;
                    ui.appScreen = AppScreen::Worlds;
                    refreshWorldList();
                } else if (appScreen == AppScreen::Worlds) {
                    appScreen = AppScreen::Start;
                    ui.appScreen = AppScreen::Start;
                } else if (appScreen == AppScreen::PlayerProfile) {
                    g_textTarget = nullptr;
                    ui.nameFieldActive = false;
                    if (ui.playerName.empty()) ui.playerName = "玩家";
                    saveProfile();
                    appScreen = AppScreen::Start;
                    ui.appScreen = AppScreen::Start;
                } else if (appScreen == AppScreen::RoomLobby) {
                    roomTeams.clear();
                    roomPlayers.clear();
                    spectating = false;
                    roomSession = false;
                    world.reset(seed);
                    world.setSaveEnabled(false);
                    appScreen = AppScreen::Start;
                    ui.appScreen = AppScreen::Start;
                }
                ui.menuMessage.clear();
            } else if (ui.matEditorOpen) ui.matEditorOpen = false;
            else if (debugMenuOpen) debugMenuOpen = false;
            else if (settingsOpen) settingsOpen = false;
            else if (inventoryOpen) {
                if (drag.active) endDrag(inv, worn, -1, -1, drag);
                inventoryOpen = false;
            }
            else paused = !paused;
        }
        prevEsc = esc;

        if (!playing) {
            if (g_wheel != 0) {
                int steps = g_wheel / 120;
                if (steps == 0) steps = (g_wheel > 0) ? 1 : -1;
                if (appScreen == AppScreen::Worlds) ui.worldScroll -= steps;
                if (appScreen == AppScreen::WorldDetail) ui.backupScroll -= steps;
            }
        } else if (g_wheel != 0) {
            int steps = g_wheel / 120;
            if (steps == 0) steps = (g_wheel > 0) ? 1 : -1;
            const int n = cfg::HAND_SLOTS;
            ui.selectedRight = ((ui.selectedRight - steps) % n + n) % n;
        }
        g_wheel = 0;

        updateCursor(wg.hwnd, lookLocked);

        // Mouse look: raw input gives 1:1 device movement (no Windows pointer
        // acceleration); recentering the cursor each frame keeps rotation infinite.
        if (lookLocked) {
            float dx = g_mouseDX;
            float dy = g_mouseDY;
            if (sim) { dx = 4.0f; dy = 0.3f; }
            g_mouseDX = 0.0f;
            g_mouseDY = 0.0f;
            if (!firstLook) {
                player.yaw += dx * mouseSens;
                player.pitch += (invertY ? 1.0f : -1.0f) * dy * mouseSens;
                const float plim = 1.5533f;
                if (player.pitch > plim) player.pitch = plim;
                if (player.pitch < -plim) player.pitch = -plim;
            }
            firstLook = false;
            POINT c{ g_winW / 2, g_winH / 2 };
            ClientToScreen(wg.hwnd, &c);
            SetCursorPos(c.x, c.y);
            ui.mouseX = (float)(g_winW / 2);
            ui.mouseY = (float)(g_winH / 2);
            player.syncBodyYaw();
        } else {
            g_mouseDX = 0.0f;
            g_mouseDY = 0.0f;
            POINT mp;
            GetCursorPos(&mp);
            ScreenToClient(wg.hwnd, &mp);
            ui.mouseX = (float)mp.x;
            ui.mouseY = (float)mp.y;
        }

        bool lmb = keyDown(VK_LBUTTON);
        bool rmb = keyDown(VK_RBUTTON);
        if (sim) {
            if (frameCounter % 240 == 0) lmb = true;          // break a block
            if (frameCounter % 480 == 120) rmb = true;        // place a block
            if (frameCounter % 600 == 300) { player.flying = !player.flying; player.vel.y = 0.0f; }
            // Exercise the ESC menu + settings submenu.
            if (frameCounter % 400 == 200) { paused = !paused; if (paused) settingsOpen = false; }
            if (paused && frameCounter % 120 == 60) settingsOpen = !settingsOpen;
            // Exercise the inventory screen.
            if (frameCounter % 700 == 350) {
                if (inventoryOpen && drag.active) endDrag(inv, worn, -1, -1, drag);
                inventoryOpen = !inventoryOpen;
                if (inventoryOpen) { drag.active = false; drag.block = AIR; drag.count = 0; drag.sourceSlot = -1; drag.sourceWear = -1; }
            }
        }

        IVec3 hit, prev;
        Vec3 nrm;
        bool hitOk = false;
        int physHit = -1;
        float hitT = cfg::REACH + 1.0f;
        int dropHit = -1;
        float dropT = cfg::REACH + 1.0f;
        if (playing) {
            hitOk = world.raycast(player.eye(), player.lookDir(), cfg::REACH, hit, prev, nrm, &physHit, &hitT);
            dropHit = world.raycastDrop(player.eye(), player.lookDir(), cfg::REACH, dropT);
            if (dropHit >= 0 && dropT <= hitT) {
                hitOk = false;
                physHit = -1;
            } else {
                dropHit = -1;
            }
        }
        ui.hasTarget = hitOk && !inventoryOpen && !paused && playing && !spectating;
        ui.targetBlock = hit;
        ui.hasPlacePreview = false;
        if (ui.hasTarget && physHit < 0 && !player.dead && !carry.empty()
            && hold::isCarryBlock(carry.block)
            && plugin::blockStrategy(carry.block)->canPlace(carry.block)) {
            uint8_t beside = world.getBlock(prev.x, prev.y, prev.z);
            if (beside == AIR) {
                ui.hasPlacePreview = true;
                ui.placePreview = prev;
            }
        }
        ui.targetFace = hitOk ? world.faceFromHitNormal(nrm) : 0;
        ui.targetPhys = hitOk ? physHit : -1;
        ui.targetDrop = (dropHit >= 0 && !inventoryOpen && !paused && playing && !spectating) ? dropHit : -1;
        ui.hasBreakOverlay = false;
        ui.breakProgress = 0.0f;
        ui.breakSod = false;
        if (ui.hasTarget) {
            int face = world.faceFromHitNormal(nrm);
            if (physHit < 0 && world.hasSodFace(hit.x, hit.y, hit.z, face)) {
                ui.breakProgress = world.sodDurProgress(hit.x, hit.y, hit.z, face);
                ui.breakSod = false;
                ui.hasBreakOverlay = false;
            } else {
                ui.breakProgress = world.blockDurProgress(physHit, hit.x, hit.y, hit.z);
                ui.hasBreakOverlay = ui.breakProgress > 0.02f;
            }
        }

        if (fPressed && !paused && !ui.matEditorOpen && playing && !player.dead && !spectating) {
            if (!inventoryOpen && !carry.empty()) {
                dropCarriedBlock(world, player, carry);
            } else if (!inventoryOpen && ui.targetDrop >= 0 && ui.targetDrop < (int)world.drops().size()) {
                const loot::Drop& d = world.drops()[(size_t)ui.targetDrop];
                uint8_t item = d.item;
                uint8_t n = d.count;
                if (hold::isCarryBlock(item)) {
                    if (carry.empty() && n > 0) {
                        carry.block = item;
                        carry.count = 1;
                        if (n <= 1) {
                            uint8_t ignI = AIR, ignN = 0;
                            world.takeDrop(ui.targetDrop, ignI, ignN);
                            ui.targetDrop = -1;
                        } else {
                            world.setDropCount(ui.targetDrop, (uint8_t)(n - 1));
                        }
                    }
                } else {
                    int left = inventoryAdd(inv, item, n);
                    if (left <= 0) {
                        uint8_t ignI = AIR, ignN = 0;
                        world.takeDrop(ui.targetDrop, ignI, ignN);
                        ui.targetDrop = -1;
                    } else if (left < (int)n) {
                        world.setDropCount(ui.targetDrop, (uint8_t)left);
                    }
                }
            }
        }

        // Humidity label: the air block under the crosshair.
        if (playing) {
            if (hitOk && physHit >= 0) {
                ui.hasHumidityBlock = false;
            } else {
                IVec3 airB;
                if (hitOk) airB = prev;
                else {
                    Vec3 p = player.eye() + player.lookDir() * 4.0f;
                    const float S = cfg::BLOCK_SCALE;
                    airB = { (int)std::floor(p.x / S), (int)std::floor(p.y / S), (int)std::floor(p.z / S) };
                }
                if (world.getBlock(airB.x, airB.y, airB.z) == AIR) {
                    ui.hasHumidityBlock = true;
                    ui.humidityBlock = airB;
                    ui.humidityValue = world.humidityAt(airB.x, airB.y, airB.z);
                } else {
                    ui.hasHumidityBlock = false;
                }
            }
        } else {
            ui.hasHumidityBlock = false;
        }

        if (g_focused) {
            if (!playing) {
                auto trimCopy = [](std::string s) {
                    while (!s.empty() && (unsigned char)s.front() <= 32) s.erase(s.begin());
                    while (!s.empty() && (unsigned char)s.back() <= 32) s.pop_back();
                    return s;
                };
                auto tryCreateWorld = [&]() {
                    std::string name = trimCopy(ui.newWorldName);
                    if (name.empty()) name = "新世界";
                    if (!saves::isValidWorldName(name)) {
                        ui.menuMessage = "名称无效";
                        return;
                    }
                    if (!saves::createWorld(name, seed)) {
                        ui.menuMessage = "创建失败（名称可能已存在）";
                        return;
                    }
                    ui.selectedWorld = name;
                    ui.selectedBackup = -1;
                    ui.deleteArmed = false;
                    ui.newWorldName.clear();
                    g_textTarget = nullptr;
                    ui.nameFieldActive = false;
                    appScreen = AppScreen::WorldDetail;
                    ui.appScreen = AppScreen::WorldDetail;
                    refreshBackups();
                    ui.menuMessage = "已创建世界";
                };
                if (g_textSubmit && appScreen == AppScreen::CreateWorld) tryCreateWorld();
                g_textSubmit = false;

                if (lmb && !prevLmb) {
                    if (appScreen == AppScreen::Start) {
                        if (ui.portraitHover) {
                            appScreen = AppScreen::PlayerProfile;
                            ui.appScreen = AppScreen::PlayerProfile;
                            ui.nameFieldActive = true;
                            g_textTarget = &ui.playerName;
                            ui.menuMessage.clear();
                        } else if (ui.startHover == 0) {
                            beginRoom();
                        } else if (ui.startHover == 1) {
                            openExplore();
                        }
                    } else if (appScreen == AppScreen::PlayerProfile) {
                        if (ui.profileHover == 0) {
                            ui.nameFieldActive = true;
                            g_textTarget = &ui.playerName;
                        } else if (ui.profileHover == 1) {
                            importPlayerModel();
                        } else if (ui.profileHover == 2) {
                            g_textTarget = nullptr;
                            ui.nameFieldActive = false;
                            if (ui.playerName.empty()) ui.playerName = "玩家";
                            saveProfile();
                            appScreen = AppScreen::Start;
                            ui.appScreen = AppScreen::Start;
                            ui.menuMessage.clear();
                        } else {
                            ui.nameFieldActive = false;
                            g_textTarget = nullptr;
                        }
                    } else if (appScreen == AppScreen::RoomLobby) {
                        if (ui.lobbyJoinHover >= 0)
                            joinRoomTeam(ui.lobbyJoinHover);
                        else if (ui.lobbyBtnHover == 0)
                            addRoomTeam();
                        else if (ui.lobbyBtnHover == 1)
                            startRoom();
                        else if (ui.lobbyBtnHover == 2) {
                            roomTeams.clear();
                            roomPlayers.clear();
                            spectating = false;
                            roomSession = false;
                            world.reset(seed);
                            world.setSaveEnabled(false);
                            appScreen = AppScreen::Start;
                            ui.appScreen = AppScreen::Start;
                            ui.menuMessage.clear();
                        }
                    } else if (appScreen == AppScreen::Worlds) {
                        if (ui.worldDeleteHover >= 0 && ui.worldDeleteHover < (int)ui.worldNames.size()) {
                            const std::string& name = ui.worldNames[ui.worldDeleteHover];
                            if (ui.pendingDelete != name) {
                                ui.pendingDelete = name;
                                ui.menuMessage = "再点一次「确认」将永久删除该世界";
                            } else if (!saves::deleteWorld(name)) {
                                ui.pendingDelete.clear();
                                ui.menuMessage = "删除失败";
                            } else {
                                ui.pendingDelete.clear();
                                if (ui.selectedWorld == name) {
                                    ui.selectedWorld.clear();
                                    ui.selectedBackup = -1;
                                    ui.backupNames.clear();
                                }
                                refreshWorldList();
                                ui.menuMessage = "已删除世界";
                            }
                        } else if (ui.worldItemHover >= 0 && ui.worldItemHover < (int)ui.worldNames.size()) {
                            ui.pendingDelete.clear();
                            ui.selectedWorld = ui.worldNames[ui.worldItemHover];
                            ui.selectedBackup = -1;
                            ui.deleteArmed = false;
                            appScreen = AppScreen::WorldDetail;
                            ui.appScreen = AppScreen::WorldDetail;
                            ui.menuMessage.clear();
                            refreshBackups();
                        } else if (ui.worldsBtnHover == 0) {
                            ui.pendingDelete.clear();
                            appScreen = AppScreen::CreateWorld;
                            ui.appScreen = AppScreen::CreateWorld;
                            ui.newWorldName.clear();
                            ui.nameFieldActive = true;
                            g_textTarget = &ui.newWorldName;
                            ui.menuMessage.clear();
                        } else if (ui.worldsBtnHover == 1) {
                            ui.pendingDelete.clear();
                            appScreen = AppScreen::Start;
                            ui.appScreen = AppScreen::Start;
                            ui.menuMessage.clear();
                        }
                    } else if (appScreen == AppScreen::WorldDetail) {
                        if (ui.backupItemHover >= 0 && ui.backupItemHover < (int)ui.backupNames.size()) {
                            ui.selectedBackup = ui.backupItemHover;
                            ui.deleteArmed = false;
                        } else if (ui.detailBtnHover == 0) {
                            ui.deleteArmed = false;
                            enterWorld(ui.selectedWorld);
                        } else if (ui.detailBtnHover == 1) {
                            ui.deleteArmed = false;
                            std::string stamp;
                            if (saves::backupDefault(ui.selectedWorld, &stamp)) {
                                ui.menuMessage = std::string("已备份 ") + stamp;
                                refreshBackups();
                            } else {
                                ui.menuMessage = "备份失败";
                            }
                        } else if (ui.detailBtnHover == 2) {
                            ui.deleteArmed = false;
                            if (ui.selectedBackup < 0 || ui.selectedBackup >= (int)ui.backupNames.size()) {
                                ui.menuMessage = "请先在列表中选择一份备份";
                            } else {
                                std::string slot = ui.backupNames[ui.selectedBackup];
                                if (saves::restoreBackup(ui.selectedWorld, slot))
                                    enterWorld(ui.selectedWorld);
                                else
                                    ui.menuMessage = "切换备份失败";
                            }
                        } else if (ui.detailBtnHover == 3) {
                            if (!ui.deleteArmed) {
                                ui.deleteArmed = true;
                                ui.menuMessage = "再点一次「确认删除」将永久删除该世界";
                            } else if (!saves::deleteWorld(ui.selectedWorld)) {
                                ui.deleteArmed = false;
                                ui.menuMessage = "删除失败";
                            } else {
                                ui.deleteArmed = false;
                                ui.selectedWorld.clear();
                                ui.selectedBackup = -1;
                                ui.backupNames.clear();
                                refreshWorldList();
                                appScreen = AppScreen::Worlds;
                                ui.appScreen = AppScreen::Worlds;
                                ui.menuMessage = "已删除世界";
                            }
                        } else if (ui.detailBtnHover == 4) {
                            ui.deleteArmed = false;
                            appScreen = AppScreen::Worlds;
                            ui.appScreen = AppScreen::Worlds;
                            ui.menuMessage.clear();
                            refreshWorldList();
                        }
                    } else if (appScreen == AppScreen::CreateWorld) {
                        if (ui.createBtnHover == 0) {
                            ui.nameFieldActive = true;
                            g_textTarget = &ui.newWorldName;
                        } else if (ui.createBtnHover == 1) {
                            tryCreateWorld();
                        } else if (ui.createBtnHover == 2) {
                            g_textTarget = nullptr;
                            ui.nameFieldActive = false;
                            appScreen = AppScreen::Worlds;
                            ui.appScreen = AppScreen::Worlds;
                            refreshWorldList();
                            ui.menuMessage.clear();
                        } else {
                            ui.nameFieldActive = false;
                            g_textTarget = nullptr;
                        }
                    }
                }
            } else if (ui.matEditorOpen) {
                const int T = tex::TILE;
                static const float kPal[8][3] = {
                    { 0.0f, 1.0f, 0.0f }, { 0.55f, 0.30f, 0.10f }, { 0.5f, 0.5f, 0.5f }, { 1.0f, 1.0f, 1.0f },
                    { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.3f, 1.0f }, { 1.0f, 0.85f, 0.0f },
                };
                ui.matEditorHoverX = -1; ui.matEditorHoverY = -1;
                if (ui.matEditorTileSize > 0.0f) {
                    int hx = (int)((ui.mouseX - ui.matEditorTileX) / (ui.matEditorTileSize / T));
                    int hy = (int)((ui.mouseY - ui.matEditorTileY) / (ui.matEditorTileSize / T));
                    if (hx >= 0 && hx < T && hy >= 0 && hy < T) { ui.matEditorHoverX = hx; ui.matEditorHoverY = hy; }
                }
                mat::Image& img = mat::g_tileImages[ui.matEditorTile];
                if (img.ok() && ui.matEditorHoverX >= 0) {
                    size_t idx = ((size_t)ui.matEditorHoverY * T + ui.matEditorHoverX) * 4;
                    if (lmb) {
                        img.rgba[idx + 0] = (uint8_t)(ui.matEditorR * 255.0f);
                        img.rgba[idx + 1] = (uint8_t)(ui.matEditorG * 255.0f);
                        img.rgba[idx + 2] = (uint8_t)(ui.matEditorB * 255.0f);
                        img.rgba[idx + 3] = (uint8_t)(ui.matEditorA * 255.0f);
                        ui.matEditorDirty = true;
                    } else if (rmb && !prevRmb) {
                        ui.matEditorR = img.rgba[idx + 0] / 255.0f;
                        ui.matEditorG = img.rgba[idx + 1] / 255.0f;
                        ui.matEditorB = img.rgba[idx + 2] / 255.0f;
                        ui.matEditorA = img.rgba[idx + 3] / 255.0f;
                    }
                }
                if (lmb && !prevLmb && ui.matEditorPaletteHover >= 0) {
                    if (ui.matEditorPaletteHover < 8) {
                        ui.matEditorR = kPal[ui.matEditorPaletteHover][0];
                        ui.matEditorG = kPal[ui.matEditorPaletteHover][1];
                        ui.matEditorB = kPal[ui.matEditorPaletteHover][2];
                        ui.matEditorA = 1.0f;
                    } else {
                        ui.matEditorA = 0.0f;
                    }
                }
                static bool prevLbrack = false, prevRbrack = false;
                bool lb = keyDown('['), rb = keyDown(']');
                if (lb && !prevLbrack) ui.matEditorTile = (ui.matEditorTile + TEX_COUNT - 1) % TEX_COUNT;
                if (rb && !prevRbrack) ui.matEditorTile = (ui.matEditorTile + 1) % TEX_COUNT;
                prevLbrack = lb; prevRbrack = rb;
                if (keyDown(VK_F5)) { renderer.saveMaterialTile(ui.matEditorTile); ui.matEditorDirty = false; }
            } else if (paused) {
                if (settingsOpen) {
                    if (lmb && !prevLmb && ui.settingsHover == 0) settingsOpen = false;
                    if (lmb && !prevLmb && ui.settingsHover == 1) invertY = !invertY;
                    if (lmb && ui.sliderW > 1.0f &&
                        ui.mouseX >= ui.sliderX - 12.0f && ui.mouseX <= ui.sliderX + ui.sliderW + 12.0f &&
                        ui.mouseY >= ui.sliderY - 12.0f && ui.mouseY <= ui.sliderY + ui.sliderH + 12.0f) {
                        float t = (ui.mouseX - ui.sliderX) / ui.sliderW;
                        t = clampf(t, 0.0f, 1.0f);
                        mouseSens = cfg::SENS_MIN + t * (cfg::SENS_MAX - cfg::SENS_MIN);
                    }
                } else if (debugMenuOpen) {
                    if (lmb && !prevLmb && ui.debugHover == 0) debugMenuOpen = false; // back
                    if (lmb && !prevLmb && ui.debugHover == 1) humidityMode = !humidityMode;
                    if (lmb && !prevLmb && ui.debugHover == 2) { ui.matEditorOpen = true; debugMenuOpen = false; paused = false; }
                    if (lmb && !prevLmb && ui.debugHover == 3) { launchModelEditor(); } // open the model editor
                    if (lmb && !prevLmb && ui.debugHover == 4) {
                        ui.dummyActive = !ui.dummyActive;
                        if (ui.dummyActive && !ui.dummyPlaced) {
                            ui.dummyPos = player.pos + player.lookDir() * 4.0f;
                            ui.dummyPos.y = player.pos.y;
                            ui.dummyPlaced = true;
                        }
                    }
                    if (lmb && !prevLmb && ui.debugHover == 5) {
                        player.privilegeMode = !player.privilegeMode;
                    }
                    if (lmb && !prevLmb && ui.debugHover == 6) {
                        player.flying = !player.flying;
                        if (!player.flying) player.vel.y = 0.0f;
                    }
                    if (lmb && ui.tickSliderW > 1.0f &&
                        ui.mouseX >= ui.tickSliderX - 12.0f && ui.mouseX <= ui.tickSliderX + ui.tickSliderW + 12.0f &&
                        ui.mouseY >= ui.tickSliderY - 12.0f && ui.mouseY <= ui.tickSliderY + ui.tickSliderH + 12.0f) {
                        float t = (ui.mouseX - ui.tickSliderX) / ui.tickSliderW;
                        t = clampf(t, 0.0f, 1.0f);
                        tickSpeed = t * 20.0f;
                    }
                    if (lmb && ui.timeSliderW > 1.0f &&
                        ui.mouseX >= ui.timeSliderX - 12.0f && ui.mouseX <= ui.timeSliderX + ui.timeSliderW + 12.0f &&
                        ui.mouseY >= ui.timeSliderY - 12.0f && ui.mouseY <= ui.timeSliderY + ui.timeSliderH + 12.0f) {
                        float t = (ui.mouseX - ui.timeSliderX) / ui.timeSliderW;
                        t = clampf(t, 0.0f, 1.0f);
                        timeOfDay = t * (float)cfg::TICKS_PER_DAY;
                    }
                } else {
                    if (lmb && !prevLmb) {
                        if (ui.menuHover == 0) { paused = false; settingsOpen = false; debugMenuOpen = false; }
                        else if (ui.menuHover == 1) { settingsOpen = true; }
                        else if (ui.menuHover == 2) { debugMenuOpen = true; }
                        else if (ui.menuHover == 3) leaveWorld();
                    }
                }
            } else if (spectating) {
                inventoryOpen = false;
                player.strikeName.clear();
                player.pickRaised = false;
                player.mineCharge = 0.0f;
            } else if (inventoryOpen) {
                float hs = std::sqrt(player.vel.x * player.vel.x + player.vel.z * player.vel.z);
                bool moving = in.forward || in.back || in.left || in.right || hs > 0.40f;
                ui.bagLocked = moving;
                if (moving && drag.active && (drag.sourceWear >= 0 || drag.sourceSlot >= cfg::HOTBAR_SLOTS || drag.sourceSlot < 0))
                    endDrag(inv, worn, -1, -1, drag);
                bool shift = keyDown(VK_SHIFT);
                if (lmb && !prevLmb) {
                    if (!moving && player.privilegeMode && ui.hoveredBlock > 0 && !drag.active) {
                        drag.block = (uint8_t)ui.hoveredBlock;
                        drag.count = (int)loot::maxStack((uint8_t)ui.hoveredBlock);
                        drag.sourceSlot = -1;
                        drag.sourceWear = -1;
                        drag.active = true;
                    } else if (!moving && ui.hoveredWear >= 0 && !drag.active) {
                        startDragWear(worn, ui.hoveredWear, shift, drag);
                    } else if (ui.hoveredSlot >= 0 && !drag.active) {
                        if (!moving || ui.hoveredSlot < cfg::HOTBAR_SLOTS)
                            startDrag(inv, ui.hoveredSlot, shift, drag);
                    }
                }
                if (!lmb && prevLmb && drag.active) {
                    int drop = ui.hoveredSlot;
                    int dropWear = ui.hoveredWear;
                    if (moving && drop >= cfg::HOTBAR_SLOTS) drop = -1;
                    if (moving) dropWear = -1;
                    endDrag(inv, worn, drop, dropWear, drag);
                }
                if (rmb && !prevRmb) {
                    if (drag.active) endDrag(inv, worn, -1, -1, drag);
                    else inventoryOpen = false;
                }
            } else if (player.dead) {
                player.strikeName.clear();
                player.pickRaised = false;
                player.mineCharge = 0.0f;
                if (lmb && !prevLmb && ui.deathHover == 0) {
                    vitals::reset(player.vitals);
                    vitals::resetFatigue(player.fatigue);
                    player.dead = false;
                    player.vel = { 0, 0, 0 };
                    spawnPlayer();
                }
            } else if (!carry.empty()) {
                player.mineCharge = 0.0f;
                player.mineCooldown -= dt;
                if (player.mineCooldown < 0.0f) player.mineCooldown = 0.0f;
                player.strikeName.clear();
                player.pickRaised = false;
                if (rmb && !prevRmb && ui.hasPlacePreview) {
                    IVec3 place = ui.placePreview;
                    if (world.getBlock(place.x, place.y, place.z) == AIR
                        && plugin::blockStrategy(carry.block)->canPlace(carry.block)) {
                        world.setBlock(place.x, place.y, place.z, carry.block, true);
                        carry.clear();
                        ui.hasPlacePreview = false;
                    }
                }
            } else {
                if (lmb && !prevLmb && hitOk && ui.targetDrop < 0) {
                    uint8_t held = inv[ui.selectedSlot].block;
                    if (physHit < 0) {
                        int face = world.faceFromHitNormal(nrm);
                        bool barkTool = hasItemTags(held, TAG_AXE | TAG_WOODWORKING | TAG_ONE_HAND);
                        if (barkTool && world.hasBarkFace(hit.x, hit.y, hit.z, face)) {
                            if (world.takeBarkFace(hit.x, hit.y, hit.z, face))
                                world.spawnDrop(cellCenter(hit.x, hit.y, hit.z), BARK, 1, true);
                        }
                    }
                }
                player.mineCooldown -= dt;
                if (player.mineCooldown < 0.0f) player.mineCooldown = 0.0f;
                if (player.mineCharge <= 0.0f && player.mineCooldown <= 0.0f)
                    player.strikeName.clear();

                bool canMine = false;
                uint8_t heldMine = AIR;
                if (lmb && hitOk && ui.targetDrop < 0 && lookLocked && !player.dead) {
                    heldMine = inv[ui.selectedSlot].block;
                    canMine = true;
                    if (physHit < 0) {
                        uint8_t b = world.getBlock(hit.x, hit.y, hit.z);
                        if (!plugin::blockStrategy(b)->canBreak(b)) canMine = false;
                        if (b == GRASS_TUFT && hit.y > 0
                            && world.getBlock(hit.x, hit.y - 1, hit.z) == GRASS_TUFT)
                            canMine = false;
                    } else {
                        uint8_t b = world.getPhysBlock(physHit, hit.x, hit.y, hit.z);
                        if (!plugin::blockStrategy(b)->canBreak(b)) canMine = false;
                    }
                    if (canMine) {
                        int face = world.faceFromHitNormal(nrm);
                        float dmg = 0.0f;
                        if (physHit < 0 && world.hasSodFace(hit.x, hit.y, hit.z, face))
                            dmg = loot::sodMineDamage(heldMine);
                        else
                            dmg = loot::mineDamage(physHit < 0
                                ? world.getBlock(hit.x, hit.y, hit.z)
                                : world.getPhysBlock(physHit, hit.x, hit.y, hit.z), heldMine);
                        if (dmg <= 0.0f) canMine = false;
                    }
                }
                if (!canMine) {
                    player.mineCharge = 0.0f;
                } else if (player.mineCooldown > 0.0f) {
                    // Cooling down: holding or click-spam cannot skip this.
                } else {
                    if (player.strikeName.empty()) {
                        if (heldMine == AIR) player.strikeName = "punch";
                        else if (hasItemTags(heldMine, TAG_AXE)) player.strikeName = "axe_chop";
                        else if (hasItemTags(heldMine, TAG_PICK)) player.strikeName = "pick_mine";
                        if (player.strikeName != "pick_mine") player.pickRaised = false;
                        if (!player.strikeName.empty()) {
                            player.strikeCharge = hasItemTags(heldMine, TAG_AXE)
                                ? anim::axeChopSec()
                                : loot::mineChargeSec(heldMine);
                            player.strikeCool = loot::mineCooldownSec(heldMine);
                        }
                    }
                    player.mineCharge += dt;
                    float need = loot::mineChargeSec(heldMine);
                    float recover = loot::mineCooldownSec(heldMine);
                    if (hasItemTags(heldMine, TAG_AXE)) {
                        // The chop plays in real time. The crack is the last frame,
                        // not the item's charge timer (that used to land late).
                        need = anim::axeChopSec();
                    } else if (hasItemTags(heldMine, TAG_PICK)) {
                        need = player.pickRaised ? anim::pickDownSec() : anim::pickFirstSec();
                        recover = anim::pickUpSec();
                    }
                    if (player.mineCharge >= need) {
                        player.mineCharge = 0.0f;
                        player.mineCooldown = recover;
                        if (hasItemTags(heldMine, TAG_PICK)) player.pickRaised = true;
                        int face = world.faceFromHitNormal(nrm);
                        if (world.applyMineHit(physHit, hit.x, hit.y, hit.z, heldMine, face))
                            finishMinedBlock(world, heldMine, physHit, hit);
                    }
                }
                if (player.mineCharge <= 0.0f && player.mineCooldown <= 0.0f) {
                    player.strikeName.clear();
                    player.pickRaised = false;
                }
                if (rmb && !prevRmb && hitOk) {
                    if (physHit >= 0) {
                        ItemSlot& sel = inv[ui.selectedSlot];
                        if (!sel.empty() && sel.block != BARK && plugin::blockStrategy(sel.block)->canPlace(sel.block)) {
                            if (world.placePhysBlock(physHit, prev.x, prev.y, prev.z, sel.block)) {
                                if (--sel.count == 0) sel.clear();
                            }
                        }
                    } else {
                        ItemSlot& sel = inv[ui.selectedSlot];
                        if (!sel.empty() && sel.block == BARK) {
                            int face = world.faceFromHitNormal(nrm);
                            IVec3 host = hit;
                            if (isLiquid(world.getBlock(hit.x, hit.y, hit.z))) host = prev;
                            if (world.addBarkFace(host.x, host.y, host.z, face)) {
                                if (--sel.count == 0) sel.clear();
                            }
                        } else {
                            uint8_t target = world.getBlock(hit.x, hit.y, hit.z);
                            plugin::BlockEvent ev{ &world, hit.x, hit.y, hit.z, target, target };
                            if (!plugin::blockStrategy(target)->onInteract(ev)) {
                                if (!sel.empty() && plugin::blockStrategy(sel.block)->canPlace(sel.block)) {
                                    IVec3 place = hit;
                                    if (!isLiquid(world.getBlock(hit.x, hit.y, hit.z))) place = prev;
                                    uint8_t existing = world.getBlock(place.x, place.y, place.z);
                                    if ((existing == AIR || isLiquid(existing)) && !playerOverlapsCell(place, player.pos)) {
                                        world.setBlock(place.x, place.y, place.z, sel.block, true);
                                        if (--sel.count == 0) sel.clear();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        prevLmb = lmb;
        prevRmb = rmb;

        // 1-3 选左手，4-6 选右手；滚轮循环右手。
        if (playing) {
            for (int i = 0; i < cfg::HAND_SLOTS; i++) {
                if (keyDown('1' + i)) ui.selectedLeft = i;
                if (keyDown('4' + i)) ui.selectedRight = i;
            }
        }
        if (ui.selectedLeft < 0 || ui.selectedLeft >= cfg::HAND_SLOTS) ui.selectedLeft = 0;
        if (ui.selectedRight < 0 || ui.selectedRight >= cfg::HAND_SLOTS) ui.selectedRight = 0;
        ui.selectedSlot = cfg::HAND_SLOTS + ui.selectedRight;

        // Fixed-step physics (paused while the menu is open or the player is dead).
        if (spectating) {
            player.flying = true;
            player.noclip = true;
            player.dead = false;
        }
        if (playing && !paused && !ui.matEditorOpen && !player.dead) {
            accumulator += dt;
            int sub = 0;
            while (accumulator >= cfg::FIXED_DT && sub < cfg::MAX_SUBSTEPS) {
                player.update(world, in, cfg::FIXED_DT);
                world.treeFallPhysics(cfg::FIXED_DT);
                world.updateDrops(cfg::FIXED_DT);
                if (const plugin::EntityModule* em = plugin::findEntity("player")) {
                    plugin::EntityEvent ev{ &world, &player, cfg::FIXED_DT };
                    em->strategy->onTick(ev);
                    em->strategy->think(ev);
                }
                if (!player.privilegeMode && !spectating) {
                    vitals::TickInput vin;
                    vin.moving = in.forward || in.back || in.left || in.right;
                    vin.sprint = in.sprint && !player.flying && vitals::canSprint(player.vitals);
                    vin.jumpImpulse = player.jumpedThisUpdate;
                    vin.landed = player.landedThisUpdate;
                    vin.swim = player.inWater;
                    vin.mining = lmb && ui.hasTarget && lookLocked && carry.empty();
                    vin.flying = player.flying;
                    vin.landImpact = player.landImpact;
                    vitals::tick(player.vitals, player.fatigue, vin, cfg::FIXED_DT);
                    if (vitals::isDead(player.vitals)) {
                        player.dead = true;
                        player.vel = { 0, 0, 0 };
                    }
                }
                accumulator -= cfg::FIXED_DT;
                sub++;
            }
            if (sub == cfg::MAX_SUBSTEPS) accumulator = 0.0f;

            world.update(player.pos, 6);

            if (player.privilegeMode) {
                vitals::reset(player.vitals);
                vitals::resetFatigue(player.fatigue);
                player.dead = false;
            }

            // Game ticks for block updates (grass-sod wither + water flow). The
            // debug-menu slider multiplies their rate: 0x pauses ticks, 20x fast-forwards.
            gameTickAccum += dt * (float)cfg::TICKS_PER_SECOND * tickSpeed;
            int ticks = (int)gameTickAccum;
            if (ticks > 0) {
                if (ticks > 100) { ticks = 100; gameTickAccum = 0.0f; }
                else gameTickAccum -= (float)ticks;
                for (int i = 0; i < ticks; i++) {
                    world.sodTick(1024, gameTick);
                    world.waterTick(gameTick);
                    world.treeFallGameTick();
                    gameTick++;
                }
            }

            timeOfDay += dt * ((float)cfg::TICKS_PER_DAY / cfg::DAY_LENGTH_SECONDS);
            if (timeOfDay >= (float)cfg::TICKS_PER_DAY) timeOfDay -= (float)cfg::TICKS_PER_DAY;
        } else if (!playing) {
            if (exploreScreen() || portraitScreen()) {
                timeOfDay = 6000.0f;
                uint32_t want = seedFromName(ui.playerName);
                if (!menuSeedReady || menuSeedApplied != want || world.seed() != want) buildMenuWorld();
                world.update(menuEye, 4);
            } else {
                timeOfDay += dt * 80.0f;
                if (timeOfDay >= (float)cfg::TICKS_PER_DAY) timeOfDay -= (float)cfg::TICKS_PER_DAY;
                if (appScreen != AppScreen::Start && appScreen != AppScreen::PlayerProfile) {
                    player.yaw += dt * 0.08f;
                    player.pitch = -0.12f;
                }
            }
        }

        if (g_resized) {
            renderer.setScreenSize(g_winW, g_winH);
            g_resized = false;
        }

        renderer.sync(world);

        ui.showDebug = showDebug;
        ui.inventoryOpen = inventoryOpen;
        ui.bagLocked = inventoryOpen && (in.forward || in.back || in.left || in.right
            || (player.vel.x * player.vel.x + player.vel.z * player.vel.z) > 0.16f);
        ui.appScreen = appScreen;
        ui.menuOpen = paused;
        ui.settingsOpen = settingsOpen;
        ui.mouseSens = mouseSens;
        ui.invertY = invertY;
        ui.debugMenuOpen = debugMenuOpen;
        ui.tickSpeed = tickSpeed;
        ui.humidityMode = humidityMode;
        ui.privilegeMode = player.privilegeMode;
        ui.vitals = &player.vitals;
        ui.playerDead = player.dead;
        if (drag.active) { ui.held.block = drag.block; ui.held.count = (uint8_t)drag.count; }
        else ui.held.clear();
        ui.fps = fps;
        ui.loadedChunks = world.loadedChunks();
        ui.timeOfDay = timeOfDay;
        ui.playerPos = player.pos;
        ui.playerVel = player.vel;
        ui.yaw = player.yaw;
        ui.pitch = player.pitch;
        ui.flying = player.flying;
        ui.onGround = player.onGround;
        ui.seed = seed;
        ui.roomTeams = roomTeams;
        ui.roomPlayers = roomPlayers;
        ui.roomMinPlayers = kRoomMinPlayers;
        ui.spectating = spectating && playing;
        ui.menuWorld = exploreScreen();
        ui.menuEye = menuEye;
        ui.menuTarget = menuTarget;
        ui.menuFeet = menuFeet;

        renderer.render(world, player, timeOfDay, ui);

        SwapBuffers(wg.hdc);

        frameCounter++;
        if (frames > 0 && frameCounter >= frames) running = false;
    }

    if (appScreen == AppScreen::Playing && !roomSession) {
        world.saveAll();
        if (!noSave) savePlayer(world.saveDir(), player, timeOfDay, inv, carry, worn, mouseSens, invertY);
    }

    renderer.shutdown();
    wglMakeCurrent(nullptr, nullptr);
    if (wg.ctx) wglDeleteContext(wg.ctx);
    ReleaseDC(wg.hwnd, wg.hdc);
    DestroyWindow(wg.hwnd);
    return 0;
}
