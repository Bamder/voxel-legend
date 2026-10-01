#include "core/config.hpp"
#include "world/blocks.hpp"
#include "world/world.hpp"
#include "world/loot.hpp"
#include "world/drop_geom.hpp"
#include "world/data_pack.hpp"
#include "world/saves.hpp"
#include "world/player.hpp"
#include "world/animation.hpp"
#include "world/player_model.hpp"
#include "world/hold_bind.hpp"
#include "world/vitals.hpp"
#include "world/combat.hpp"
#include "world/guardian_fight.hpp"
#include "world/guardian_fight_world.hpp"
#include "world/clue.hpp"
#include "world/guide.hpp"
#include "world/wear.hpp"
#include "world/matchmap.hpp"
#include "world/ritual.hpp"
#include "world/structure.hpp"
#include "render/renderer.hpp"
#include "core/gl.hpp"
#include "render/textures.hpp"
#include "material/registry.hpp"
#include "plugin/plugin.hpp"
#include "net/room_net.hpp"
#include "net/room_body.hpp"
#include "net/room_inventory.hpp"
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
#include <unordered_map>

// ---------------------------------------------------------------------------
// Window / input globals (populated by WndProc, consumed by the game loop).
// ---------------------------------------------------------------------------
static float g_mouseDX = 0.0f, g_mouseDY = 0.0f;
static float g_mouseScale = 1.0f;
static float g_absScaleX = 1.0f, g_absScaleY = 1.0f;
static LONG g_prevAbsX = -1, g_prevAbsY = -1;
static int g_screenW = 1280, g_screenH = 720;
static bool g_focused = false;
static bool g_storyAdvance = false;
static bool g_clipHeld = false;
static int g_winW = 1280, g_winH = 720;
static bool g_resized = false;
static bool g_cursorHidden = false;
static int g_wheel = 0;
static std::string* g_textTarget = nullptr;
static bool g_textSubmit = false;
static bool g_textDigits = false;
static int g_textMax = 64;

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

static bool windowForeground(HWND hwnd) {
    return hwnd && GetForegroundWindow() == hwnd;
}

static void showGameCursor() {
    if (!g_cursorHidden) return;
    ShowCursor(TRUE);
    g_cursorHidden = false;
}

// ClipCursor is system-wide. A second instance must not release a clip it does not hold.
static void releaseCursorClip() {
    if (!g_clipHeld) return;
    ClipCursor(nullptr);
    g_clipHeld = false;
}

static void clearPointer() {
    g_mouseDX = 0.0f;
    g_mouseDY = 0.0f;
    g_wheel = 0;
    g_prevAbsX = -1;
    g_prevAbsY = -1;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_INPUT: {
            if (!windowForeground(h)) return 0;
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
            if (!windowForeground(h)) return 0;
            g_wheel += (int)(short)HIWORD(w);
            return 0;
        case WM_CHAR: {
            if (!windowForeground(h) || !g_textTarget) return 0;
            wchar_t wc = (wchar_t)w;
            if (wc < 32) return 0;
            wchar_t wcs[2] = { wc, 0 };
            char utf8[8] = {};
            int n = WideCharToMultiByte(CP_UTF8, 0, wcs, -1, utf8, (int)sizeof(utf8), nullptr, nullptr);
            if (n > 1) {
                utf8[n - 1] = 0;
                if (g_textDigits) {
                    if (utf8[1] != 0 || utf8[0] < '0' || utf8[0] > '9') return 0;
                } else if (utf8[1] == 0) {
                    char c = utf8[0];
                    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                        c == '\\' || c == '|' || c == '?' || c == '*')
                        return 0;
                }
                size_t cap = g_textMax > 0 ? (size_t)g_textMax : 64;
                appendUtf8(*g_textTarget, utf8, cap);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (windowForeground(h)) g_storyAdvance = true;
            break;
        case WM_KEYDOWN:
            if (!windowForeground(h)) return 0;
            if ((l & (1u << 30)) == 0) g_storyAdvance = true;
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
            g_focused = windowForeground(h);
            return 0;
        case WM_KILLFOCUS:
            g_focused = false;
            clearPointer();
            showGameCursor();
            releaseCursorClip();
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            showGameCursor();
            releaseCursorClip();
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

// GetAsyncKeyState is system-wide. Only the foreground window may see keys.
static bool keyDown(int vk) {
    if (!g_focused) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Authoritative focus: WM_SETFOCUS can stay true on a covered window.
static void syncFocus(HWND hwnd) {
    bool now = windowForeground(hwnd);
    if (g_focused != now) {
        clearPointer();
        if (!now) {
            showGameCursor();
            releaseCursorClip();
        }
    }
    g_focused = now;
}

static void updateCursor(HWND hwnd, bool locked) {
    if (!g_focused) locked = false;
    if (locked) {
        if (!g_cursorHidden) { ShowCursor(FALSE); g_cursorHidden = true; }
        // Trap the cursor inside the window so it can't reach the screen border.
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            ClipCursor(&rc);
            g_clipHeld = true;
        }
    } else {
        showGameCursor();
        releaseCursorClip();
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

static bool g_roomRecord = false;
static bool g_roomApplyNet = false;
static std::vector<BlockEditNet> g_roomEdits;
static std::vector<PlayInputNet::BarkEdit> g_roomBark;
static std::vector<PlayInputNet::MineEdit> g_roomMines;
static bool g_treeResync = false;

static void noteRoomEdit(int x, int y, int z, uint8_t b, int face = -1) {
    if (!g_roomRecord || g_roomApplyNet) return;
    if (g_roomEdits.size() >= 256) g_roomEdits.erase(g_roomEdits.begin());
    BlockEditNet e;
    e.x = x;
    e.y = y;
    e.z = z;
    e.block = b;
    e.face = (face >= 0 && face < 6) ? (uint8_t)face : 255;
    g_roomEdits.push_back(e);
}

static void noteRoomBark(int x, int y, int z, int face, bool place) {
    if (!g_roomRecord || g_roomApplyNet || face < 0 || face > 5) return;
    if (g_roomBark.size() >= 64) g_roomBark.erase(g_roomBark.begin());
    PlayInputNet::BarkEdit e;
    e.x = x;
    e.y = y;
    e.z = z;
    e.face = (uint8_t)face;
    e.place = place;
    g_roomBark.push_back(e);
}

static void noteRoomMine(int x, int y, int z, int face, uint8_t tool, uint32_t tree) {
    if (!g_roomRecord || g_roomApplyNet || face < 0 || face > 5) return;
    if (g_roomMines.size() >= 64) g_roomMines.erase(g_roomMines.begin());
    PlayInputNet::MineEdit e;
    e.x = x;
    e.y = y;
    e.z = z;
    e.face = (uint8_t)face;
    e.tool = tool;
    e.tree = tree;
    g_roomMines.push_back(e);
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

static bool tossDrop(World& world, const Player& player, uint8_t item, int count) {
    if (item == AIR || count <= 0 || !validBlock(item)) return false;
    Vec3 dir = player.lookDir();
    const dropgeom::Shape& sh = dropgeom::cached(item);
    float forward = 0.55f + std::max(sh.half.x, sh.half.z);
    Vec3 pos = player.eye() + dir * forward;
    Vec3 vel = dir * 2.4f;
    vel.y += 1.2f;
    world.spawnDrop(pos, item, count, false, vel);
    return true;
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
    if (b == GRASS_TUFT && hit.y + 1 < cfg::WORLD_H
        && world.getBlock(hit.x, hit.y + 1, hit.z) == GRASS_TUFT)
        extraGrass = 1;
    bool harvest = loot::isHarvestBreak(b, held);
    int barkN = world.takeAllBarkAt(hit.x, hit.y, hit.z);
    Vec3 dropPos = cellCenter(hit.x, hit.y, hit.z);
    world.setBlock(hit.x, hit.y, hit.z, AIR, true);
    noteRoomEdit(hit.x, hit.y, hit.z, AIR);
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
            if (py < cfg::WORLD_H - 1 && world.getBlock((int)px, py, (int)pz) == AIR) {
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
    int cx = floorDiv(x, cfg::CHUNK_X);
    int cy = floorDiv(y, cfg::CHUNK_Y);
    int cz = floorDiv(z, cfg::CHUNK_Z);
    auto it = w.chunks().find(chunkKey(cx, cy, cz));
    if (it == w.chunks().end()) return -1;
    int lx = x - cx * cfg::CHUNK_X;
    int ly = y - cy * cfg::CHUNK_Y;
    int lz = z - cz * cfg::CHUNK_Z;
    for (const auto& sf : it->second.sodFaces) {
        if ((int)sf.x == lx && (int)sf.y == ly && (int)sf.z == lz && (int)sf.face == face)
            return (int)sf.stage;
    }
    return -1; // absent (removed or never grown)
}

static bool findTopSodDirt(const World& w, int& ox, int& oy, int& oz) {
    for (const auto& [key, ch] : w.chunks()) {
        int cx = chunkCX(key), cy = chunkCY(key), cz = chunkCZ(key);
        for (const auto& sf : ch.sodFaces) {
            if (sf.face != 0 || ch.get(sf.x, sf.y, sf.z) != DIRT) continue;
            int wx = cx * cfg::CHUNK_X + sf.x;
            int wy = cy * cfg::CHUNK_Y + sf.y;
            int wz = cz * cfg::CHUNK_Z + sf.z;
            if (w.getBlock(wx, wy + 1, wz) != AIR) continue;
            ox = wx; oy = wy; oz = wz;
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
        int dcx = floorDiv(sx, cfg::CHUNK_X), dcy = floorDiv(sy, cfg::CHUNK_Y), dcz = floorDiv(sz, cfg::CHUNK_Z);
        auto dit = world.chunks().find(chunkKey(dcx, dcy, dcz));
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
            int cx = chunkCX(key), cy = chunkCY(key), cz = chunkCZ(key);
            for (int y = 0; y < cfg::CHUNK_Y; y++) {
                int wy = cy * cfg::CHUNK_Y + y;
                if (wy <= 0 || wy >= cfg::WORLD_H - 1) continue;
                for (int z = 0; z < cfg::CHUNK_Z; z++) {
                    for (int x = 0; x < cfg::CHUNK_X; x++) {
                        if (ch.get(x, y, z) != DIRT) continue;
                        int above = (y + 1 < cfg::CHUNK_Y) ? ch.get(x, y + 1, z) : w3.getBlock(cx * cfg::CHUNK_X + x, wy + 1, cz * cfg::CHUNK_Z + z);
                        bool surface = !isOpaque((uint8_t)above);
                        int wx = cx * cfg::CHUNK_X + x;
                        int wz = cz * cfg::CHUNK_Z + z;
                        bool s2 = (w3.getBlock(wx + 1, wy, wz) == AIR);
                        bool s3 = (w3.getBlock(wx - 1, wy, wz) == AIR);
                        bool s4 = (w3.getBlock(wx, wy, wz + 1) == AIR);
                        bool s5 = (w3.getBlock(wx, wy, wz - 1) == AIR);
                        if (!(s2 || s3 || s4 || s5)) continue;
                        if (!surface) {
                            if (!foundDeep) { foundDeep = true; ddx = wx; ddy = wy; ddz = wz; }
                            for (const auto& sf : ch.sodFaces) {
                                if ((int)sf.x == x && (int)sf.y == y && (int)sf.z == z) { deepSod = true; break; }
                            }
                        } else {
                            if (!foundCliff) { foundCliff = true; ccx = wx; ccy = wy; ccz = wz; }
                            int need[4] = { 2, 3, 4, 5 };
                            int sdx[4] = { 1, -1, 0, 0 };
                            int sdz[4] = { 0, 0, 1, -1 };
                            for (int k = 0; k < 4; k++) {
                                int nxw = wx + sdx[k], nzw = wz + sdz[k];
                                int ncx = floorDiv(nxw, cfg::CHUNK_X), ncz = floorDiv(nzw, cfg::CHUNK_Z);
                                if (w3.chunks().find(chunkKey(ncx, cy, ncz)) == w3.chunks().end()) continue; // deferred
                                if (w3.getBlock(nxw, wy, nzw) != AIR) continue; // covered
                                bool has = false;
                                for (const auto& sf : ch.sodFaces) {
                                    if ((int)sf.x == x && (int)sf.y == y && (int)sf.z == z && (int)sf.face == need[k]) { has = true; break; }
                                }
                                if (!has && !cliffMissing) { cliffMissing = true; ccx = wx; ccy = wy; ccz = wz; }
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
struct NetSample {
    uint32_t tick = 0;
    Vec3 pos{};
    float yaw = 0, pitch = 0, bodyYaw = 0;
    float frame = 0, strikeFrame = 0;
    uint8_t clip = 1, strike = 0;
    uint8_t heldL = 0, heldR = 0, carried = 0;
    uint8_t wearU = 0, wearL = 0, wearS = 0;
    bool spectator = false;
    bool dead = false;
    bool hitFlash = false;
    uint8_t status = 0;
    vitals::Vitals vitals{};
    std::string name;
};

static void pushNetSample(std::vector<NetSample>& h, const NetSample& s) {
    if (!h.empty() && h.back().tick == s.tick) {
        h.back() = s;
        return;
    }
    h.push_back(s);
    if (h.size() > 8) h.erase(h.begin());
}

static const NetSample* sampleAt(const std::vector<NetSample>& h, uint32_t tick, const NetSample** next) {
    const NetSample* at = nullptr;
    const NetSample* nxt = nullptr;
    for (const NetSample& s : h) {
        if (s.tick <= tick) at = &s;
        else if (!nxt) nxt = &s;
    }
    if (next) *next = nxt;
    if (at) return at;
    return h.empty() ? nullptr : &h.front();
}

static NetSample sampleFromPose(const PlayerPoseNet& pose, uint32_t tick) {
    NetSample s;
    s.tick = tick;
    s.pos = { pose.x, pose.y, pose.z };
    s.yaw = pose.yaw;
    s.pitch = pose.pitch;
    s.bodyYaw = pose.bodyYaw;
    s.frame = anim::dequantFrame(pose.frameQ);
    s.strikeFrame = anim::dequantFrame(pose.strikeQ);
    s.clip = pose.clip;
    s.strike = pose.strike;
    s.heldL = pose.heldL;
    s.heldR = pose.heldR;
    s.carried = pose.carried;
    s.wearU = pose.wearU;
    s.wearL = pose.wearL;
    s.wearS = pose.wearS;
    s.spectator = pose.spectator;
    s.dead = pose.dead;
    s.hitFlash = pose.hitFlash != 0;
    s.status = pose.status;
    for (int i = 0; i < vitals::Count; ++i) s.vitals.limb[i].health = pose.health[i];
    s.name = pose.name;
    return s;
}

void deployBlockColor(uint8_t b, int y, int& r, int& g, int& bl) {
    if (b == WATER || isLiquid(b)) { r = 42; g = 86; bl = 148; return; }
    if (b == SAND || b == SANDSTONE) { r = 194; g = 174; bl = 108; return; }
    if (b == SNOW) { r = 226; g = 230; bl = 234; return; }
    if (b == STONE || b == COBBLE || b == GRAVEL || b == BEDROCK) { r = 112; g = 112; bl = 116; return; }
    if (b == LOG || b == WOOD || b == PLANKS || b == BARK || b == BARK_BLOCK) { r = 122; g = 84; bl = 48; return; }
    if (b == LEAVES || b == SHRUB_LEAF || b == SHRUB_STEM) { r = 46; g = 108; bl = 44; return; }
    if (b == GRASS || b == DIRT || b == GRASS_TUFT) {
        float t = (float)(y - cfg::SEA_LEVEL) / 30.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        r = (int)(58.0f + t * 40.0f);
        g = (int)(124.0f - t * 42.0f);
        bl = 46;
        return;
    }
    r = 96; g = 96; bl = 100;
}

void paintDeployPreview(World& world, int ox, int oz, int span, std::vector<uint8_t>& px) {
    const int n = matchmap::kDeployPreview;
    px.assign((size_t)n * (size_t)n * 4, 255);
    if (span <= 0) return;
    for (int iz = 0; iz < n; iz++) {
        int wz = oz + (int)(((long long)iz * span) / n);
        for (int ix = 0; ix < n; ix++) {
            int wx = ox + (int)(((long long)ix * span) / n);
            int h = world.surfaceHeight(wx, wz);
            uint8_t top = GRASS;
            if (h < cfg::SEA_LEVEL) top = WATER;
            else if (h >= cfg::SEA_LEVEL + 30) top = SNOW;
            else if (h >= cfg::SEA_LEVEL + 18) top = STONE;
            int r = 0, g = 0, bl = 0;
            deployBlockColor(top, h, r, g, bl);
            size_t i = ((size_t)iz * (size_t)n + (size_t)ix) * 4;
            px[i] = (uint8_t)r;
            px[i + 1] = (uint8_t)g;
            px[i + 2] = (uint8_t)bl;
            px[i + 3] = 255;
        }
    }
}

static bool stepLocalGuardians(World& world, Player& player, float dt) {
    guardian_fight::Home homes[guardian_fight::kSlots];
    int homeCount = 0;
    std::vector<structure::GuardianSync> live;
    structure::collectRoomGuardians(live);
    const float block = cfg::BLOCK_SCALE;
    for (const structure::GuardianSync& g : live) {
        if (homeCount >= guardian_fight::kSlots) break;
        guardian_fight::Home& home = homes[homeCount++];
        home.relic = g.relic;
        home.hp = (float)g.hp;
        home.maxHp = (float)(g.maxHp > 0 ? g.maxHp : 1);
        home.feet = { (g.x + 0.5f) * block, (float)g.y * block, (g.z + 0.5f) * block };
    }
    guardian_fight::Rival rival;
    rival.id = 1;
    rival.feet = player.pos;
    rival.alive = !player.dead && !vitals::isDead(player.vitals);
    rival.active = rival.alive && !player.noclip;
    if (player.strikeCharge > guardian_ai::kWindup && player.mineCharge < player.strikeCharge)
        rival.windup = player.strikeCharge - player.mineCharge;
    guardian_fight::Blow blows[8];
    int blowCount = guardian_fight::tick(homes, homeCount, &rival, 1, dt, guardianGround(world), blows, 8);
    bool struck = false;
    for (int i = 0; i < blowCount; ++i) {
        const guardian_fight::Blow& blow = blows[i];
        if (blow.target != rival.id || player.dead) continue;
        combat::DamageSource source{guardian_fight::attackerId(blow.relic), 1, AIR,
                                    combat::DamageCategory::Physical};
        auto result = combat::damagePlayer(player.vitals, source,
                                            {blow.amount, 1.0f, blow.wholeBody}, blow.limb);
        if (!result.applied) continue;
        struck = true;
        if (result.killed) {
            player.dead = true;
            player.vel = {};
        }
    }
    guardian_fight::Pose posed[guardian_fight::kSlots];
    int poseCount = guardian_fight::poses(posed, guardian_fight::kSlots);
    bool posedRelic[guardian_fight::kSlots]{};
    for (int i = 0; i < poseCount; ++i) {
        const guardian_fight::Pose& pose = posed[i];
        if (pose.relic < 0 || pose.relic >= guardian_fight::kSlots) continue;
        posedRelic[pose.relic] = true;
        structure::setGuardianPose(pose.relic, pose.feet.x, pose.feet.y, pose.feet.z, pose.yaw, pose.swing);
    }
    for (int relic = 0; relic < guardian_fight::kSlots; ++relic)
        if (!posedRelic[relic]) structure::clearGuardianPose(relic);
    return struck;
}

int main(int argc, char** argv) {
    // Resolve assets/ relative to the executable, not the caller's cwd.
    {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (!slash) slash = wcsrchr(path, L'/');
            if (slash) {
                *slash = 0;
                SetCurrentDirectoryW(path);
            }
        }
    }
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
    bool roomServer = false;
    bool clueQa = false;
    bool editStructure = false;
    std::string editStructurePath;
    uint16_t roomServerPort = kRoomPortDefault;
    std::string roomHandoff;

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
        else if (a == "--room-server") { roomServer = true; }
        else if (a == "--qa-clue") { clueQa = true; }
        else if (a == "--edit-structure") {
            editStructure = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') editStructurePath = argv[++i];
        }
        else if (a == "--port" && i + 1 < argc) { roomServerPort = (uint16_t)std::atoi(argv[++i]); }
        else if (a == "--handoff" && i + 1 < argc) { roomHandoff = argv[++i]; }
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
            printf("  --room-server  headless room server (started by the host)\n");
            printf("  --qa-clue     local in-game clue/Boss test; F8 travels to the next objective\n");
            printf("  --port N       room server port (default 35535)\n");
            printf("  --handoff PATH lobby roster for --room-server\n");
            printf("  --edit-structure [file]  fly-build a structure file\n");
            return 0;
        }
    }

    if (roomServer) return runRoomServer(roomServerPort, roomHandoff, clueQa);
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
    std::vector<TrainingTarget> targets;
    int targetPanel = -1;
    bool showDebug = false;
    bool debugMenuOpen = false;
    bool humidityMode = false;
    std::vector<RoomTeamView> roomTeams;
    std::vector<RoomPlayerView> roomPlayers;
    bool roomSession = false;
    bool roomHost = false;
    int roomPort = kRoomPortDefault;
    std::string roomHostAddr = "127.0.0.1";
    LobbyHost lobbyHost;
    LobbyGuest lobbyGuest;
    GameClient gameClient;
    ServerProcess roomProc;
    bool loadFailed = false;
    bool loadSeedApplied = false;
    float loadShown = 0.0f;
    float lobbyBroadcastAccum = 0.0f;
    bool lobbyDirty = false;
    float roomNetAccum = 0.0f;
    uint8_t roomMovement = 0;
    uint32_t roomInventoryRevision = 0, roomLayoutNext = 0, roomLayoutPending = 0;
    ItemSlot roomInventory[cfg::INVENTORY_SLOTS]{};
    bool roomInventoryKnown = false, roomLayoutDirty = false;
    uint32_t roomAttackNext = 0, roomAttackPending = 0;
    uint8_t roomAttackHand = 1;
    float roomAttackVisualTime = 0.0f;
    uint8_t roomAttackVisualItem = AIR;
    uint32_t roomCastNext = 0, roomCastPending = 0;
    uint8_t roomCastHand = 1;
    uint32_t roomPickupNext = 0, roomPickupPending = 0, roomPickupDrop = 0;
    uint32_t roomGuardianNext = 0, roomGuardianPending = 0;
    uint8_t roomGuardianRelic = 255;
    uint32_t roomCombatAck = 0;
    uint32_t roomArcaneAck = 0;
    float hitMarker = 0.0f;
    float damageFlash = 0.0f;
    uint8_t roomPlayerStatus = 0;
    ClueTargetNet roomClueTarget{};
    ClueQuizNet clueQuiz{};
    bool clueQuizSubmitting = false;
    std::vector<ArcaneProjectileView> arcaneProjectiles;
    std::vector<ArcaneBurstView> arcaneBursts;
    struct ChunkNetState {
        uint32_t rev = 0;
        bool ready = false;
        bool hold = false;
    };
    std::unordered_map<int64_t, ChunkNetState> chunkNet;
    std::vector<PlayInputNet::ChunkAsk> resyncAsk;
    std::unordered_map<uint32_t, std::vector<NetSample>> remoteHist;
    std::vector<NetSample> selfHist;
    std::vector<RemoteAvatar> remotes;
    uint32_t roomTick = 0;
    bool roomTickInit = false;
    std::chrono::steady_clock::time_point roomTickAt{};
    NetSample selfShown{};
    bool selfShownOk = false;
    Vec3 roomCameraOffset{ 0, 0, 0 };
    bool spectating = false;
    bool deploying = false;
    bool deployDeath = false;
    std::vector<uint8_t> deployPixels;
    std::vector<DeployPinNet> deployPins;
    int deployOx = 0, deployOz = 0, deploySpan = 0, deployStamp = 0;
    bool structureEdit = editStructure;
    std::string structurePath = editStructurePath;
    uint8_t editBlock = PLANKS;
    bool blockBarOpen = false;
    bool ritualDone = false;
    bool storyShown = false;
    bool storyOpen = false;
    bool guideOpen = false;
    int guidePage = 0;
    bool clueOpen = false;
    int storyIndex = 0;
    int storyPhase = 0;
    float storyAlpha = 0.0f;
    bool structurePainted = false;
    bool structurePicker = false;
    bool structureNaming = false;
    bool structureChosen = false;
    bool structureCanReturn = false;
    std::string structureNote;
    float saveFlash = 0.0f;
    constexpr int kRoomMinPlayers = 1;
    float tickSpeed = 1.0f;
    float gameTickAccum = 0.0f;
    uint64_t gameTick = 0;
    bool prevF3 = false, prevF = false, prevF5 = false, prevF8 = false, prevE = false, prevEsc = false;
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
                        if (blocksMotion(world.getBlock(bx, by, bz))) return true;
            return false;
        };
        float sy = player.pos.y;
        float maxY = (float)(cfg::WORLD_H - 4) * cfg::BLOCK_SCALE;
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

    auto shutdownRoom = [&]() {
        roomMovement = 0;
        roomInventoryRevision = roomLayoutNext = roomLayoutPending = 0;
        roomInventoryKnown = roomLayoutDirty = false;
        roomAttackNext = roomAttackPending = roomCastNext = roomCastPending = 0;
        roomAttackVisualTime = 0.0f;
        roomAttackVisualItem = AIR;
        roomPickupNext = roomPickupPending = roomPickupDrop = 0;
        roomGuardianNext = roomGuardianPending = 0;
        roomGuardianRelic = 255;
        roomCombatAck = roomArcaneAck = 0; hitMarker = damageFlash = 0; roomPlayerStatus = 0;
        roomClueTarget = {};
        clueQuiz = {};
        clueQuizSubmitting = false;
        arcaneProjectiles.clear(); arcaneBursts.clear();
        lobbyHost.close();
        lobbyGuest.close();
        gameClient.close();
        roomProc.kill();
        roomHost = false;
        g_roomRecord = false;
        g_roomEdits.clear();
        g_roomBark.clear();
        g_roomMines.clear();
        g_treeResync = false;
        world.setLocalTrees(true);
        remoteHist.clear();
        selfHist.clear();
        remotes.clear();
        roomTick = 0;
        roomTickInit = false;
        selfShownOk = false;
        roomCameraOffset = { 0, 0, 0 };
        chunkNet.clear();
        resyncAsk.clear();
        loadSeedApplied = false;
        loadFailed = false;
        g_textDigits = false;
        ui.portFieldActive = false;
        ui.joinAddrActive = false;
        ui.joinPortActive = false;
    };

    auto enterWorld = [&](const std::string& name) {
        shutdownRoom();
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
                            if (blocksMotion(world.getBlock(bx, by, bz))) return true;
                return false;
            };
            float sy = player.pos.y;
            float maxY = (float)(cfg::WORLD_H - 4) * cfg::BLOCK_SCALE;
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
        targets.clear();
        targetPanel = -1;
        debugMenuOpen = false;
        ui.matEditorOpen = false;
        accumulator = 0.0f;
        gameTickAccum = 0.0f;
        gameTick = 0;
    };

    struct TrialAnchor {
        bool active = false;
        Vec3 pos{ 0, 0, 0 };
        float yaw = 0.0f;
        float pitch = 0.0f;
        bool flying = false;
    };
    TrialAnchor trialAnchor;

    auto exitTrial = [&]() {
        if (!trialAnchor.active && !world.guardianArena()) return;
        world.clearTrialDrops();
        structure::closeTrial();
        world.setGuardianArena(false);
        if (trialAnchor.active) {
            player.pos = trialAnchor.pos;
            player.vel = { 0, 0, 0 };
            player.yaw = trialAnchor.yaw;
            player.pitch = trialAnchor.pitch;
            player.flying = trialAnchor.flying;
            player.syncBodyYaw();
            trialAnchor.active = false;
        }
        ui.trialPick = false;
        ui.inTrial = false;
        debugMenuOpen = false;
        paused = false;
    };

    auto placeTrialSpawn = [&]() {
        const float S = cfg::BLOCK_SCALE;
        int sx = structure::kTrialCX;
        int sz = structure::kTrialCZ + 48;
        player.pos = { (sx + 0.5f) * S, (structure::kTrialFloor + 1) * S + 0.05f, (sz + 0.5f) * S };
        player.vel = { 0, 0, 0 };
        player.yaw = 0.0f;
        player.pitch = 0.0f;
        player.flying = false;
        player.syncBodyYaw();
    };

    auto enterTrial = [&](int relic) {
        if (roomSession || structureEdit || !player.privilegeMode) return;
        if (relic < 0 || relic >= ritual::RelicCount) return;
        if (!trialAnchor.active) {
            trialAnchor.pos = player.pos;
            trialAnchor.yaw = player.yaw;
            trialAnchor.pitch = player.pitch;
            trialAnchor.flying = player.flying;
            trialAnchor.active = true;
        }
        world.setGuardianArena(true);
        world.discardGuardianArenaChunks();
        structure::openTrial(relic);
        world.clearTrialDrops();
        placeTrialSpawn();
        world.loadGuardianArena();
        structure::ensureTrialCore(world);
        ui.trialPick = false;
        ui.inTrial = true;
        debugMenuOpen = false;
        settingsOpen = false;
        paused = false;
        firstLook = true;
    };

    auto leaveWorld = [&]() {
        if (appScreen == AppScreen::Playing && !roomSession) {
            exitTrial();
            world.saveAll();
            if (!noSave && !currentWorld.empty())
                savePlayer(world.saveDir(), player, timeOfDay, inv, carry, worn, mouseSens, invertY);
        }
        shutdownRoom();
        spectating = false;
        deploying = false;
        deployDeath = false;
        storyShown = false;
        storyOpen = false;
        guideOpen = false;
        clueOpen = false;
        clueQuiz = {};
        clueQuizSubmitting = false;
        guidePage = 0;
        ui.noteOpen = false;
        ui.storyOpen = false;
        deployPins.clear();
        roomSession = false;
        roomTeams.clear();
        roomPlayers.clear();
        matchmap::setSpan(matchmap::kFullSpan);
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
        targets.clear();
        targetPanel = -1;
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
        shutdownRoom();
        std::string err;
        roomPort = kRoomPortDefault;
        if (!lobbyHost.open((uint16_t)roomPort, err)) {
            ui.menuMessage = err.empty() ? "无法监听端口" : err;
            return;
        }
        roomHost = true;
        roomHostAddr = "127.0.0.1";
        ui.roomPort = roomPort;
        ui.roomPortText = std::to_string(roomPort);
        ui.portFieldActive = false;
        roomTeams.clear();
        roomTeams.push_back(RoomTeamView{ "观战", 0.75f, 0.76f, 0.80f, true });
        roomPlayers.clear();
        RoomPlayerView me;
        me.name = ui.playerName.empty() ? "玩家" : ui.playerName;
        me.team = -1;
        me.local = true;
        me.host = true;
        me.id = 1;
        roomPlayers.push_back(me);
        roomSession = false;
        spectating = false;
        lobbyDirty = true;
        appScreen = AppScreen::RoomLobby;
        ui.appScreen = AppScreen::RoomLobby;
        ui.menuMessage.clear();
        g_textTarget = nullptr;
        firstLook = true;
    };
    auto addRoomTeam = [&]() {
        if (!roomHost) return;
        if (roomTeams.size() >= 1 + matchmap::kCombatTeams) return;
        int i = (int)roomTeams.size() - 1;
        if (i < 0) i = 0;
        RoomTeamView t;
        t.name = "队伍" + std::to_string(i + 1);
        t.r = kTeamCols[i % 8][0];
        t.g = kTeamCols[i % 8][1];
        t.b = kTeamCols[i % 8][2];
        roomTeams.push_back(t);
        lobbyDirty = true;
    };
    auto joinRoomTeam = [&](int team) {
        if (team < 0 || team >= (int)roomTeams.size()) return;
        for (RoomPlayerView& rp : roomPlayers)
            if (rp.local) rp.team = team;
        if (!roomHost) lobbyGuest.sendJoinTeam(team);
        lobbyDirty = true;
    };
    auto rosterTeams = [&]() {
        std::vector<RoomTeamNet> teams;
        for (const RoomTeamView& t : roomTeams) {
            RoomTeamNet n{};
            n.name = t.name;
            n.r = t.r;
            n.g = t.g;
            n.b = t.b;
            n.spectator = t.spectator;
            teams.push_back(n);
        }
        return teams;
    };
    auto rosterPlayers = [&]() {
        std::vector<RoomPlayerNet> players;
        for (const RoomPlayerView& p : roomPlayers) {
            RoomPlayerNet n{};
            n.id = p.id;
            n.name = p.name;
            n.team = p.team;
            n.host = p.host;
            players.push_back(n);
        }
        return players;
    };
    auto applyRoomPort = [&]() {
        ui.portFieldActive = false;
        g_textTarget = nullptr;
        g_textDigits = false;
        if (!roomHost || !lobbyHost.listening()) {
            ui.roomPortText = std::to_string(roomPort);
            return;
        }
        int p = 0;
        if (ui.roomPortText.empty()) p = -1;
        for (char c : ui.roomPortText) {
            if (p < 0) break;
            if (c < '0' || c > '9') { p = -1; break; }
            p = p * 10 + (c - '0');
            if (p > 65535) { p = -1; break; }
        }
        if (p < 1) {
            ui.menuMessage = "端口无效";
            ui.roomPortText = std::to_string(roomPort);
            return;
        }
        if (p == roomPort) {
            ui.roomPortText = std::to_string(roomPort);
            return;
        }
        if (lobbyHost.remoteCount() > 0) {
            ui.menuMessage = "已有玩家加入，无法修改端口";
            ui.roomPortText = std::to_string(roomPort);
            return;
        }
        std::string err;
        if (!lobbyHost.open((uint16_t)p, err)) {
            ui.menuMessage = err.empty() ? "端口被占用" : err;
            ui.roomPortText = std::to_string(roomPort);
            lobbyHost.open((uint16_t)roomPort, err);
            return;
        }
        roomPort = p;
        ui.roomPort = p;
        ui.roomPortText = std::to_string(p);
        ui.menuMessage = "端口已更新";
        lobbyDirty = true;
    };
    auto pullGuestLobby = [&]() {
        uint16_t port = 0;
        std::vector<RoomTeamNet> teams;
        std::vector<RoomPlayerNet> players;
        if (!lobbyGuest.takeLobby(port, teams, players)) return;
        roomPort = port;
        ui.roomPort = (int)port;
        if (!ui.portFieldActive) ui.roomPortText = std::to_string(port);
        roomTeams.clear();
        for (const RoomTeamNet& t : teams) {
            RoomTeamView v;
            v.name = t.name;
            v.r = t.r;
            v.g = t.g;
            v.b = t.b;
            v.spectator = t.spectator;
            roomTeams.push_back(v);
        }
        uint32_t me = lobbyGuest.localId();
        roomPlayers.clear();
        for (const RoomPlayerNet& p : players) {
            RoomPlayerView v;
            v.name = p.name;
            v.team = p.team;
            v.host = p.host;
            v.id = p.id;
            v.local = (p.id == me);
            roomPlayers.push_back(v);
        }
    };
    auto liftSpawn = [&]() {
        const float S = cfg::BLOCK_SCALE;
        auto collides = [&](float x, float y, float z) {
            float hw = cfg::PLAYER_HALF_WIDTH, hgt = cfg::PLAYER_HEIGHT;
            int x0 = (int)std::floor((x - hw) / S), x1 = (int)std::floor((x + hw - 1e-6f) / S);
            int y0 = (int)std::floor(y / S), y1 = (int)std::floor((y + hgt - 1e-6f) / S);
            int z0 = (int)std::floor((z - hw) / S), z1 = (int)std::floor((z + hw - 1e-6f) / S);
            for (int bx = x0; bx <= x1; bx++)
                for (int by = y0; by <= y1; by++)
                    for (int bz = z0; bz <= z1; bz++)
                        if (blocksMotion(world.getBlock(bx, by, bz))) return true;
            return false;
        };
        float sy = player.pos.y;
        float maxY = (float)(cfg::WORLD_H - 4) * cfg::BLOCK_SCALE;
        while (sy < maxY && collides(player.pos.x, sy, player.pos.z)) sy += cfg::BLOCK_SCALE;
        player.pos.y = sy;
    };
    auto beginLoading = [&](const std::string& addr) {
        roomHostAddr = addr;
        loadFailed = false;
        loadSeedApplied = false;
        loadShown = 0.0f;
        g_roomRecord = false;
        g_roomEdits.clear();
        g_roomBark.clear();
        g_roomMines.clear();
        g_treeResync = false;
        appScreen = AppScreen::RoomLoading;
        ui.appScreen = AppScreen::RoomLoading;
        ui.loadStatus = "正在连接服务器";
        ui.menuMessage.clear();
        g_textTarget = nullptr;
        g_textDigits = false;
        ui.portFieldActive = false;
        std::string err;
        if (!gameClient.startConnect(addr, (uint16_t)roomPort, ui.playerName, err)) {
            loadFailed = true;
            ui.loadStatus = err.empty() ? "无法连接服务器" : err;
        }
    };
    auto openDeploy = [&](bool death) {
        int team = gameClient.team();
        if (spectating || team < 1 || team > matchmap::kCombatTeams) return;
        clueOpen = false;
        clueQuiz = {};
        clueQuizSubmitting = false;
        matchmap::Zone zone = matchmap::combatZone(team - 1);
        deployOx = zone.cx0 * cfg::CHUNK_X;
        deployOz = zone.cz0 * cfg::CHUNK_Z;
        deploySpan = zone.columns * cfg::CHUNK_X;
        paintDeployPreview(world, deployOx, deployOz, deploySpan, deployPixels);
        deployStamp++;
        deployPins.clear();
        deploying = true;
        deployDeath = death;
        player.vel = { 0, 0, 0 };
        player.flying = true;
        if (death) gameClient.sendDeploy(2, 0, 0);
    };
    auto enterRoomPlay = [&]() {
        roomMovement = 0;
        roomInventoryRevision = roomLayoutPending = 0;
        roomInventoryKnown = roomLayoutDirty = false;
        roomAttackPending = roomCastPending = roomPickupPending = roomPickupDrop = 0;
        roomAttackVisualTime = 0.0f;
        roomAttackVisualItem = AIR;
        roomGuardianPending = 0;
        roomGuardianRelic = 255;
        roomCombatAck = roomArcaneAck = 0; hitMarker = damageFlash = 0; roomPlayerStatus = 0;
        roomClueTarget = {};
        clueQuiz = {};
        clueQuizSubmitting = false;
        arcaneProjectiles.clear(); arcaneBursts.clear();
        int team = gameClient.team();
        for (RoomPlayerView& rp : roomPlayers)
            if (rp.local) rp.team = team;
        spectating = gameClient.spectator() || team == 0;
        roomSession = true;
        g_roomRecord = true;
        g_roomEdits.clear();
        g_roomBark.clear();
        g_roomMines.clear();
        g_treeResync = false;
        world.setLocalTrees(false);
        world.clearFallingTrees();
        chunkNet.clear();
        resyncAsk.clear();
        remoteHist.clear();
        selfHist.clear();
        remotes.clear();
        roomTick = 0;
        roomTickInit = false;
        selfShownOk = false;
        roomCameraOffset = { 0, 0, 0 };
        player.privilegeMode = false;
        player.flying = spectating;
        player.noclip = spectating;
        player.vel = { 0, 0, 0 };
        player.dead = false;
        humidityMode = false;
        showDebug = false;
        tickSpeed = 1.0f;
        if (spectating) ui.camMode = 0;
        liftSpawn();
        appScreen = AppScreen::Playing;
        ui.appScreen = AppScreen::Playing;
        ui.loadStatus.clear();
        g_textTarget = nullptr;
        ui.menuMessage.clear();
        firstLook = true;
        paused = false;
        settingsOpen = false;
        inventoryOpen = false;
        debugMenuOpen = false;
        ui.matEditorOpen = false;
        ui.dummyActive = false;
        accumulator = 0.0f;
        gameTickAccum = 0.0f;
        gameTick = 0;
        roomNetAccum = 0.0f;
        if (!spectating && team >= 1 && team <= matchmap::kCombatTeams) openDeploy(false);
    };
    auto startRoom = [&]() {
        if (!roomHost) return;
        if ((int)roomPlayers.size() < kRoomMinPlayers) return;
        std::string path = std::string(saves::kRoot) + "/room_handoff.bin";
        std::string err;
        if (!writeRoomHandoff(path, rosterTeams(), rosterPlayers(), err)) {
            ui.menuMessage = err.empty() ? "无法写入房间交接" : err;
            return;
        }
        lobbyHost.sendMatchStart();
        lobbyHost.flushOut(400);
        lobbyHost.close();
        if (!spawnRoomServer((uint16_t)roomPort, path, roomProc, err, clueQa)) {
            ui.menuMessage = err.empty() ? "无法启动服务器进程" : err;
            std::string reopen;
            if (!lobbyHost.open((uint16_t)roomPort, reopen))
                ui.menuMessage = err.empty() ? "无法启动服务器进程" : err;
            return;
        }
        beginLoading("127.0.0.1");
    };
    auto pollRoom = [&](float frameDt) {
        if (appScreen == AppScreen::RoomLobby && roomHost && lobbyHost.listening()) {
            lobbyHost.poll();
            for (const LobbyHost::Join& j : lobbyHost.takeJoins()) {
                RoomPlayerView v;
                v.name = j.name;
                v.id = j.id;
                v.team = -1;
                v.host = false;
                v.local = false;
                roomPlayers.push_back(v);
                lobbyDirty = true;
            }
            for (uint32_t id : lobbyHost.takeLeaves()) {
                roomPlayers.erase(std::remove_if(roomPlayers.begin(), roomPlayers.end(),
                                                 [&](const RoomPlayerView& p) { return p.id == id && !p.local; }),
                                  roomPlayers.end());
                lobbyDirty = true;
            }
            for (const LobbyHost::TeamCmd& cmd : lobbyHost.takeTeamCmds()) {
                if (cmd.team < -1 || cmd.team >= (int)roomTeams.size()) continue;
                for (RoomPlayerView& rp : roomPlayers)
                    if (rp.id == cmd.id) rp.team = cmd.team;
                lobbyDirty = true;
            }
            lobbyBroadcastAccum += frameDt;
            if (lobbyDirty || lobbyBroadcastAccum >= 0.1f) {
                lobbyHost.broadcast(rosterTeams(), rosterPlayers());
                lobbyDirty = false;
                lobbyBroadcastAccum = 0.0f;
            }
        } else if (appScreen == AppScreen::RoomLobby && !roomHost) {
            lobbyGuest.poll();
            if (lobbyGuest.matchStarting()) {
                std::string addr = lobbyGuest.host();
                roomPort = lobbyGuest.port();
                ui.roomPort = roomPort;
                lobbyGuest.close();
                beginLoading(addr);
            } else if (!lobbyGuest.alive()) {
                shutdownRoom();
                roomTeams.clear();
                roomPlayers.clear();
                roomSession = false;
                spectating = false;
                appScreen = AppScreen::Start;
                ui.appScreen = AppScreen::Start;
                ui.menuMessage = "与主机断开";
            } else {
                pullGuestLobby();
            }
        } else if (appScreen == AppScreen::RoomLoading) {
            if (loadFailed) return;
            gameClient.poll();
            if (gameClient.failed()) {
                loadFailed = true;
                ui.loadStatus = gameClient.failReason().empty() ? "无法连接服务器" : gameClient.failReason();
                return;
            }
            if (!gameClient.welcomed()) {
                ui.loadStatus = gameClient.handshaking() ? "服务器正在创建世界" : "正在连接服务器";
                return;
            }
            if (!loadSeedApplied) {
                uint32_t s = gameClient.seed();
                if (s == 0) s = 1;
                matchmap::setSpan(gameClient.span());
                world.reset(s);
                world.setSaveEnabled(false);
                world.setMatchBounds(true);
                seed = s;
                currentWorld.clear();
                player = Player();
                player.privilegeMode = false;
                if (const plugin::EntityModule* em = plugin::findEntity("player")) {
                    plugin::EntityEvent ev{ &world, &player, 0.0f };
                    em->strategy->onSpawn(ev);
                }
                for (int i = 0; i < cfg::INVENTORY_SLOTS; i++) inv[i].clear();
                for (int i = 0; i < wear::Count; i++) worn[i].clear();
                carry.clear();
                targets.clear();
                targetPanel = -1;
                timeOfDay = 6000.0f;
                player.setSpawn({ gameClient.spawnX(), gameClient.spawnY(), gameClient.spawnZ() });
                player.yaw = 0.4f;
                player.pitch = -0.15f;
                loadSeedApplied = true;
                ui.loadStatus = "正在生成世界";
            }
            world.update(player.pos, 4);
            loadShown += frameDt;
            const float S = cfg::BLOCK_SCALE;
            int cx = floorDiv((int)std::floor(player.pos.x / S), cfg::CHUNK_X);
            int cz = floorDiv((int)std::floor(player.pos.z / S), cfg::CHUNK_Z);
            ui.loadStatus = "正在生成世界 " + std::to_string(world.loadedChunks());
            if (loadShown >= 0.4f && world.columnLoaded(cx, cz) && world.loadedChunks() >= 48)
                enterRoomPlay();
        } else if (roomSession && appScreen == AppScreen::Playing) {
            gameClient.poll();
            if (gameClient.failed()) {
                leaveWorld();
                ui.menuMessage = "与服务器断开";
                return;
            }
            for (ClueQuizNet& message : gameClient.takeClueQuizzes()) {
                clueQuiz = std::move(message);
                clueQuizSubmitting = false;
                clueOpen = true;
                guideOpen = false;
                inventoryOpen = false;
                roomMovement = 0;
            }
            for (PlayDeltaNet& d : gameClient.takeDeltas()) {
                if (!spectating && !structureEdit) {
                    player.vitals = d.body.vitals;
                    player.fatigue = d.body.fatigue;
                    player.dead = vitals::isDead(player.vitals);
                    if (!deploying && !storyOpen && (d.body.flags & 1)) {
                        Vec3 serverPos{d.body.x, d.body.y, d.body.z};
                        // Keep the rendered camera continuous across the
                        // authoritative correction. The actual player position
                        // remains server-authoritative for physics, raycasts,
                        // and outgoing input.
                        Vec3 renderedPos = player.pos + roomCameraOffset;
                        roomCameraOffset = renderedPos - serverPos;
                        float err2 = roomCameraOffset.lengthSq();
                        if (err2 > 1.5f * 1.5f)
                            roomCameraOffset = roomCameraOffset * (1.5f / std::sqrt(err2));
                        player.pos = serverPos;
                        player.vel = {d.body.vx, d.body.vy, d.body.vz};
                        player.onGround = (d.body.flags & 2) != 0;
                        player.inWater = (d.body.flags & 4) != 0;
                        player.flying = player.noclip = false;
                    }
                    if (player.dead && !deploying) openDeploy(true);
                }
                std::vector<loot::Drop> netDrops;
                netDrops.reserve(d.drops.size());
                for (const DropNet& src : d.drops) {
                    loot::Drop drop;
                    drop.netId = src.id; drop.pos = {src.x, src.y, src.z};
                    drop.vel = {src.vx, src.vy, src.vz};
                    drop.ax = {src.axx, src.axy, src.axz};
                    drop.ay = {src.ayx, src.ayy, src.ayz};
                    drop.az = {src.azx, src.azy, src.azz};
                    drop.angVel = {src.avx, src.avy, src.avz};
                    drop.age = src.age;
                    drop.item = src.item; drop.count = src.count; drop.grounded = src.grounded;
                    netDrops.push_back(drop);
                }
                world.replaceNetworkDrops(netDrops);
                arcaneProjectiles.clear();
                arcaneProjectiles.reserve(d.projectiles.size());
                for (const ArcaneProjectileNet& source : d.projectiles) {
                    ArcaneProjectileView view;
                    view.id = source.id; view.owner = source.owner;
                    view.kind = (uint8_t)source.kind;
                    view.pos = {source.x, source.y, source.z};
                    view.vel = {source.vx, source.vy, source.vz};
                    arcaneProjectiles.push_back(view);
                }
                for (const CombatEventNet& event : d.combat) {
                    if (event.serial <= roomCombatAck) continue;
                    roomCombatAck = event.serial;
                    if (event.attacker == gameClient.selfId()) hitMarker = .22f;
                    if (event.target == gameClient.selfId()) damageFlash = .20f;
                }
                for (const GuardianNet& guardian : d.guardians)
                    structure::applyRoomGuardian(guardian.relic, guardian.hp,
                                                 guardian.x, guardian.y, guardian.z, guardian.yaw, guardian.swing);
                for (const ArcaneEventNet& event : d.arcane) {
                    if (event.serial <= roomArcaneAck) continue;
                    roomArcaneAck = event.serial;
                    arcaneBursts.push_back({(uint8_t)event.kind,{event.x,event.y,event.z},0.0f});
                    if (arcaneBursts.size() > 32) arcaneBursts.erase(arcaneBursts.begin());
                }
                roomClueTarget = d.clue;
                bool layoutAnswered = roomLayoutPending && d.inventoryLayoutAck == roomLayoutPending;
                bool acceptInventory = !roomInventoryKnown || layoutAnswered ||
                    (!roomLayoutDirty && !roomLayoutPending && d.inventoryRevision >= roomInventoryRevision);
                if (acceptInventory && !drag.active) {
                    for (int i = 0; i < cfg::INVENTORY_SLOTS; ++i) {
                        inv[i] = d.inventory[(size_t)i];
                        roomInventory[i] = inv[i];
                    }
                    roomInventoryRevision = d.inventoryRevision;
                    roomInventoryKnown = true;
                    if (layoutAnswered) { roomLayoutPending = 0; roomLayoutDirty = false; }
                }
                if (!roomTickInit || d.serverTick > roomTick) {
                    roomTick = d.serverTick;
                    roomTickAt = std::chrono::steady_clock::now();
                    roomTickInit = true;
                }
                for (uint32_t id : d.removed) remoteHist.erase(id);
                for (const PlayerPoseNet& pose : d.players) {
                    NetSample s = sampleFromPose(pose, d.serverTick);
                    if (pose.id == gameClient.selfId()) {
                        roomPlayerStatus = pose.status;
                        pushNetSample(selfHist, s);
                    }
                    else pushNetSample(remoteHist[pose.id], s);
                }
                auto dropAsk = [&](int cx, int cy, int cz) {
                    resyncAsk.erase(std::remove_if(resyncAsk.begin(), resyncAsk.end(), [&](const PlayInputNet::ChunkAsk& a) {
                        return a.cx == cx && a.cy == cy && a.cz == cz;
                    }), resyncAsk.end());
                };
                auto askResync = [&](int cx, int cy, int cz) {
                    int64_t key = chunkKey(cx, cy, cz);
                    ChunkNetState& st = chunkNet[key];
                    st.ready = false;
                    st.hold = true;
                    for (const PlayInputNet::ChunkAsk& a : resyncAsk)
                        if (a.cx == cx && a.cy == cy && a.cz == cz) return;
                    if (resyncAsk.size() < 32)
                        resyncAsk.push_back(PlayInputNet::ChunkAsk{ cx, cy, cz });
                };
                for (const ChunkBaseNet& base : d.bases) {
                    if (base.pristine) {
                        world.acceptSeedChunk(base.cx, base.cy, base.cz);
                    } else if (base.blocks.size() == (size_t)cfg::CHUNK_VOLUME &&
                               base.water.size() == (size_t)cfg::CHUNK_VOLUME &&
                               base.flags.size() == (size_t)cfg::CHUNK_VOLUME) {
                        std::vector<World::AuthSod> sod;
                        sod.reserve(base.sod.size());
                        for (const AuthSodNet& s : base.sod)
                            sod.push_back(World::AuthSod{ s.x, s.z, s.y, s.face, s.stage, s.rem });
                        std::vector<World::AuthBark> bark;
                        bark.reserve(base.bark.size());
                        for (const AuthBarkNet& bk : base.bark)
                            bark.push_back(World::AuthBark{ bk.x, bk.z, bk.y, bk.face });
                        g_roomApplyNet = true;
                        world.writeAuthChunk(base.cx, base.cy, base.cz, base.blocks.data(),
                                             base.water.data(), base.flags.data(), sod, bark);
                        g_roomApplyNet = false;
                    }
                    ChunkNetState& st = chunkNet[chunkKey(base.cx, base.cy, base.cz)];
                    st.rev = base.rev;
                    st.ready = true;
                    st.hold = false;
                    dropAsk(base.cx, base.cy, base.cz);
                }
                for (const ChunkDeltaNet& delta : d.deltas) {
                    int64_t key = chunkKey(delta.cx, delta.cy, delta.cz);
                    ChunkNetState& st = chunkNet[key];
                    if (!st.ready || st.hold) {
                        askResync(delta.cx, delta.cy, delta.cz);
                        continue;
                    }
                    if (delta.rev <= st.rev) continue;
                    if (delta.rev != st.rev + 1) {
                        askResync(delta.cx, delta.cy, delta.cz);
                        continue;
                    }
                    std::vector<World::AuthCell> cells;
                    cells.reserve(delta.cells.size());
                    for (const AuthCellNet& c : delta.cells)
                        cells.push_back(World::AuthCell{ c.x, c.y, c.z, c.block, c.water, c.flags });
                    std::vector<World::AuthSod> sod;
                    if (delta.sod) {
                        sod.reserve(delta.sods.size());
                        for (const AuthSodNet& s : delta.sods)
                            sod.push_back(World::AuthSod{ s.x, s.z, s.y, s.face, s.stage, s.rem });
                    }
                    std::vector<World::AuthBark> bark;
                    if (delta.bark) {
                        bark.reserve(delta.barks.size());
                        for (const AuthBarkNet& bk : delta.barks)
                            bark.push_back(World::AuthBark{ bk.x, bk.z, bk.y, bk.face });
                    }
                    g_roomApplyNet = true;
                    bool ok = world.writeAuthDelta(delta.cx, delta.cy, delta.cz, cells, delta.sod, sod,
                                                   delta.bark, bark);
                    g_roomApplyNet = false;
                    if (!ok) {
                        askResync(delta.cx, delta.cy, delta.cz);
                        continue;
                    }
                    st.rev = delta.rev;
                }
                for (const ChunkHashNet& chk : d.checks) {
                    int64_t key = chunkKey(chk.cx, chk.cy, chk.cz);
                    auto it = chunkNet.find(key);
                    if (it == chunkNet.end() || !it->second.ready || it->second.hold) continue;
                    if (it->second.rev != chk.rev) {
                        askResync(chk.cx, chk.cy, chk.cz);
                        continue;
                    }
                    World::Chunk* ch = world.getChunk(chk.cx, chk.cy, chk.cz);
                    if (!ch || world.chunkAuthHash(*ch) != chk.hash)
                        askResync(chk.cx, chk.cy, chk.cz);
                }
                std::vector<World::NetTree> trees;
                trees.reserve(d.trees.size());
                for (const TreeNet& t : d.trees) {
                    World::NetTree n;
                    n.id = t.id;
                    n.rev = t.rev;
                    n.cells = t.cells;
                    n.ox = t.ox;
                    n.oy = t.oy;
                    n.oz = t.oz;
                    n.com = { t.cx, t.cy, t.cz };
                    n.vel = { t.vx, t.vy, t.vz };
                    n.omega = { t.wx, t.wy, t.wz };
                    n.ax = { t.ax, t.ay, t.az };
                    n.ay = { t.bx, t.by, t.bz };
                    n.az = { t.dx, t.dy, t.dz };
                    n.pivot = { t.px, t.py, t.pz };
                    n.hold = t.hold;
                    n.still = t.still;
                    n.body.reserve(t.body.size());
                    for (const TreeCellNet& c : t.body)
                        n.body.push_back(World::NetTreeCell{ c.x, c.y, c.z, c.block, c.flags });
                    trees.push_back(std::move(n));
                }
                if (!world.applyNetTrees(trees, d.treesGone)) g_treeResync = true;
                if (d.treeCheck && world.treeAuthHash() != d.treeHash) g_treeResync = true;
                for (const MineGoneNet& g : d.mineGone)
                    world.clearMineView(g.tree, g.x, g.y, g.z);
                for (const MineNet& m : d.mines) {
                    World::MineHit hits[World::kMaxMineHits];
                    int n = (int)m.hits.size();
                    if (n > World::kMaxMineHits) n = World::kMaxMineHits;
                    for (int i = 0; i < n; i++) hits[i] = World::MineHit{ m.hits[(size_t)i].step, m.hits[(size_t)i].face };
                    world.applyMineView(m.tree, m.x, m.y, m.z, m.rem, hits, n);
                }
            }
            float renderAt = 0.0f;
            if (roomTickInit) {
                float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - roomTickAt).count();
                if (since < 0.0f) since = 0.0f;
                if (since > 0.25f) since = 0.25f;
                float serverNow = (float)roomTick + since * (float)cfg::TICKS_PER_SECOND;
                renderAt = serverNow - 2.0f;
                if (renderAt < 0.0f) renderAt = 0.0f;
                float cap = roomTick > 0 ? (float)(roomTick - 1) : 0.0f;
                if (renderAt > cap) renderAt = cap;
            }
            uint32_t display = (uint32_t)renderAt;
            float frac = renderAt - (float)display;
            auto poseAt = [&](const std::vector<NetSample>& hist, RemoteAvatar& av) {
                const NetSample* next = nullptr;
                const NetSample* at = sampleAt(hist, display, &next);
                if (!at) return;
                av.name = at->name;
                av.spectator = at->spectator;
                av.clip = at->clip;
                av.frame = at->frame;
                av.strike = at->strike;
                av.strikeFrame = at->strikeFrame;
                av.heldL = at->heldL;
                av.heldR = at->heldR;
                av.carried = at->carried;
                av.wearU = at->wearU;
                av.wearL = at->wearL;
                av.wearS = at->wearS;
                av.yaw = at->yaw;
                av.pitch = at->pitch;
                av.bodyYaw = at->bodyYaw;
                av.dead = at->dead;
                av.hitFlash = at->hitFlash;
                av.status = at->status;
                av.vitals = at->vitals;
                av.pos = at->pos;
                if (next && next->tick > at->tick) {
                    float span = (float)(next->tick - at->tick);
                    float along = ((float)display + frac - (float)at->tick) / span;
                    float u = clampf(along, 0.0f, 1.0f);
                    av.pos = at->pos + (next->pos - at->pos) * u;
                }
            };
            remotes.clear();
            for (auto& entry : remoteHist) {
                RemoteAvatar av;
                av.id = entry.first;
                poseAt(entry.second, av);
                remotes.push_back(av);
            }
            selfShownOk = false;
            if (!selfHist.empty()) {
                RemoteAvatar selfAv;
                poseAt(selfHist, selfAv);
                selfShown.clip = selfAv.clip;
                selfShown.frame = selfAv.frame;
                selfShown.strike = selfAv.strike;
                selfShown.strikeFrame = selfAv.strikeFrame;
                selfShown.yaw = selfAv.yaw;
                selfShown.pitch = selfAv.pitch;
                selfShown.bodyYaw = selfAv.bodyYaw;
                selfShownOk = true;
            }
            if (gameClient.takeDeploy(deployPins) && deploying) {
                for (const DeployPinNet& pin : deployPins) {
                    if (pin.id != gameClient.selfId() || pin.phase != 2) continue;
                    const float S = cfg::BLOCK_SCALE;
                    int by = world.surfaceHeight(pin.bx, pin.bz);
                    player.setSpawn({ (pin.bx + 0.5f) * S, (float)(by + 3) * S, (pin.bz + 0.5f) * S });
                    player.vel = { 0, 0, 0 };
                    player.flying = spectating;
                    player.noclip = spectating;
                    if (player.dead || deployDeath) {
                        vitals::reset(player.vitals);
                        vitals::resetFatigue(player.fatigue);
                        player.dead = false;
                    }
                    bool wasDeath = deployDeath;
                    deployDeath = false;
                    liftSpawn();
                    deploying = false;
                    int lines = 0;
                    if (!wasDeath && !storyShown)
                        lines = ritual::storyLineCount(ritual::assignedRitual(gameClient.team()));
                    if (lines > 0) {
                        storyShown = true;
                        storyOpen = true;
                        storyIndex = 0;
                        storyPhase = 0;
                        storyAlpha = 0.0f;
                        player.flying = true;
                        player.vel = { 0, 0, 0 };
                    } else {
                        firstLook = true;
                    }
                    break;
                }
            }
            roomNetAccum += frameDt;
            if (roomNetAccum >= 0.05f) {
                roomNetAccum = 0.0f;
                size_t n = g_roomEdits.size() < 32 ? g_roomEdits.size() : 32;
                PlayInputNet netIn;
                netIn.movement = (g_focused && !paused && !deploying && !storyOpen && !player.dead)
                    ? roomMovement : 0;
                netIn.x = player.pos.x;
                netIn.y = player.pos.y;
                netIn.z = player.pos.z;
                netIn.yaw = player.yaw;
                netIn.pitch = player.pitch;
                netIn.bodyYaw = player.bodyYaw;
                netIn.vx = player.vel.x;
                netIn.vz = player.vel.z;
                netIn.spectator = spectating;
                netIn.ack = 0;
                netIn.selectedLeft = (uint8_t)ui.selectedLeft;
                netIn.selectedRight = (uint8_t)ui.selectedRight;
                netIn.attackSequence = roomAttackPending;
                netIn.attackHand = roomAttackHand;
                netIn.castSequence = roomCastPending;
                netIn.castHand = roomCastHand;
                netIn.pickupSequence = roomPickupPending;
                netIn.pickupDrop = roomPickupDrop;
                netIn.combatAck = roomCombatAck;
                netIn.arcaneAck = roomArcaneAck;
                netIn.guardianSequence = roomGuardianPending;
                netIn.guardianRelic = roomGuardianRelic;
                if (roomLayoutDirty && !roomLayoutPending && !drag.active && roomInventoryKnown) {
                    if (!++roomLayoutNext) ++roomLayoutNext;
                    roomLayoutPending = roomLayoutNext;
                    netIn.layoutSequence = roomLayoutPending;
                    netIn.layoutBaseRevision = roomInventoryRevision;
                    for (int i = 0; i < cfg::INVENTORY_SLOTS; ++i) netIn.layout[(size_t)i] = inv[i];
                }
                {
                    size_t nask = resyncAsk.size() < 8 ? resyncAsk.size() : 8;
                    netIn.resync.assign(resyncAsk.begin(), resyncAsk.begin() + (std::ptrdiff_t)nask);
                }
                if (player.sprinting) netIn.flags = (uint8_t)(netIn.flags | kPfSprint);
                if (player.flying) netIn.flags = (uint8_t)(netIn.flags | kPfFly);
                if (player.onGround) netIn.flags = (uint8_t)(netIn.flags | kPfGround);
                auto itemOf = [](const ItemSlot& s) -> uint8_t {
                    return s.empty() ? (uint8_t)AIR : s.block;
                };
                if (ui.selectedLeft >= 0 && ui.selectedLeft < cfg::HAND_SLOTS)
                    netIn.heldL = itemOf(inv[ui.selectedLeft]);
                int right = cfg::HAND_SLOTS + ui.selectedRight;
                if (right >= cfg::HAND_SLOTS && right < cfg::HOTBAR_SLOTS)
                    netIn.heldR = itemOf(inv[right]);
                netIn.carried = itemOf(carry);
                netIn.wearU = itemOf(worn[wear::Upper]);
                netIn.wearL = itemOf(worn[wear::Lower]);
                netIn.wearS = itemOf(worn[wear::Shoes]);
                if (player.strikeName == "punch") netIn.strikeKind = kStrikePunch;
                else if (player.strikeName == "axe_chop") netIn.strikeKind = kStrikeAxe;
                else if (player.strikeName == "pick_mine") netIn.strikeKind = kStrikePick;
                netIn.strikeCharge = player.strikeCharge;
                netIn.strikeCool = player.strikeCool;
                netIn.mineCharge = player.mineCharge;
                netIn.mineCooldown = player.mineCooldown;
                netIn.pickRaised = player.pickRaised;
                netIn.edits.assign(g_roomEdits.begin(), g_roomEdits.begin() + (std::ptrdiff_t)n);
                size_t nb = g_roomBark.size() < 8 ? g_roomBark.size() : 8;
                netIn.bark.assign(g_roomBark.begin(), g_roomBark.begin() + (std::ptrdiff_t)nb);
                size_t nm = g_roomMines.size() < 8 ? g_roomMines.size() : 8;
                netIn.mines.assign(g_roomMines.begin(), g_roomMines.begin() + (std::ptrdiff_t)nm);
                netIn.treeResync = g_treeResync;
                g_treeResync = false;
                gameClient.sendInput(netIn);
                roomAttackPending = 0;
                roomCastPending = 0;
                roomPickupPending = roomPickupDrop = 0;
                roomGuardianPending = 0;
                roomGuardianRelic = 255;
                g_roomEdits.erase(g_roomEdits.begin(), g_roomEdits.begin() + (std::ptrdiff_t)n);
                g_roomBark.erase(g_roomBark.begin(), g_roomBark.begin() + (std::ptrdiff_t)nb);
                g_roomMines.erase(g_roomMines.begin(), g_roomMines.begin() + (std::ptrdiff_t)nm);
            }
        }
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
                if (blocksMotion(world.getBlock(x, outH + dy, z))) return false;
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

    if (structureEdit) {
        structure::ensureLibrary();
        structure::listFiles(ui.structureNames);
        structurePath.clear();
        structurePicker = true;
        structureChosen = false;
        structureCanReturn = false;
        carry.clear();
        world.reset(12345);  // 初始化编辑器世界的种子
        world.setSaveEnabled(false);
        world.setBuildCanvas(true);
        world.update({ 12.0f, 10.0f, 18.0f }, 64);  // 预加载玩家周围区块
        player.privilegeMode = true;
        player.flying = true;
        player.setSpawn({ 12.0f, 10.0f, 18.0f });
        appScreen = AppScreen::Playing;
        ui.appScreen = AppScreen::Playing;
        ui.structurePicker = true;
        firstLook = true;
    }

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

        syncFocus(wg.hwnd);

        auto t1 = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(t1 - t0).count();
        t0 = t1;
        if (dt > 0.1f) dt = 0.1f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        if (hitMarker > 0.0f) { hitMarker -= dt; if (hitMarker < 0.0f) hitMarker = 0.0f; }
        if (ui.quickBreakWait > 0.0f) {
            ui.quickBreakWait -= dt;
            if (ui.quickBreakWait < 0.0f) ui.quickBreakWait = 0.0f;
        }
        if (damageFlash > 0.0f) { damageFlash -= dt; if (damageFlash < 0.0f) damageFlash = 0.0f; }
        for (ArcaneBurstView& burst : arcaneBursts) burst.age += dt;
        arcaneBursts.erase(std::remove_if(arcaneBursts.begin(), arcaneBursts.end(),
            [](const ArcaneBurstView& burst) { return burst.age >= .65f; }), arcaneBursts.end());
        fpsAccum += dt;
        fpsFrames++;
        if (fpsAccum >= 0.5f) {
            fps = (float)fpsFrames / fpsAccum;
            fpsAccum = 0.0f;
            fpsFrames = 0;
        }

        bool playing = (appScreen == AppScreen::Playing);
        pollRoom(dt);
        playing = (appScreen == AppScreen::Playing);
        if (roomSession) {
            player.privilegeMode = false;
            humidityMode = false;
            showDebug = false;
            debugMenuOpen = false;
            ui.matEditorOpen = false;
            ui.dummyActive = false;
            tickSpeed = 1.0f;
        }
        if (structureEdit) player.dead = false;
        if (!player.privilegeMode || roomSession || structureEdit) ui.quickBreak = false;
        if (player.dead) targetPanel = -1;
        bool canMove = (g_focused && playing && !paused && !ui.matEditorOpen && !player.dead && !deploying && !storyOpen
                        && !guideOpen && !clueOpen
                        && !(structureEdit && structurePicker));
        bool lookLocked = (canMove && !inventoryOpen && targetPanel < 0 && !(structureEdit && blockBarOpen));

        auto refreshStructureList = [&]() {
            structure::listFiles(ui.structureNames);
        };
        auto openStructurePicker = [&](bool canReturn) {
            refreshStructureList();
            structurePicker = true;
            structureNaming = false;
            structureCanReturn = canReturn;
            blockBarOpen = false;
            ui.structurePendingDelete.clear();
            ui.structureScroll = 0;
            ui.menuMessage.clear();
            g_textTarget = nullptr;
            ui.nameFieldActive = false;
            g_textDigits = false;
        };
        auto loadStructureFile = [&](const std::string& name) {
            structurePath = structure::pathFor(name);
            structureChosen = true;
            structurePainted = false;
            if (world.columnLoaded(0, 0)) {
                structure::clearVolume(world);
                structure::paintFile(world, structurePath);
                structurePainted = true;
            }
            structurePicker = false;
            structureNaming = false;
            blockBarOpen = false;
            ui.structurePendingDelete.clear();
            ui.menuMessage.clear();
            g_textTarget = nullptr;
            ui.nameFieldActive = false;
            firstLook = true;
        };
        auto tryCreateStructure = [&]() {
            std::string name = ui.structureNewName;
            while (!name.empty() && (unsigned char)name.front() <= 32) name.erase(name.begin());
            while (!name.empty() && (unsigned char)name.back() <= 32) name.pop_back();
            if (name.size() > 9) {
                std::string tail = name.substr(name.size() - 9);
                for (char& c : tail) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                if (tail == ".vlstruct") name.resize(name.size() - 9);
                while (!name.empty() && (unsigned char)name.back() <= 32) name.pop_back();
            }
            if (name.empty()) {
                ui.menuMessage = "请输入名称";
                return;
            }
            if (!structure::validName(name)) {
                ui.menuMessage = "名称无效";
                return;
            }
            if (!structure::createFile(name)) {
                ui.menuMessage = "创建失败（名称可能已存在）";
                return;
            }
            ui.structureNewName.clear();
            loadStructureFile(name);
        };
        auto saveCurrentStructure = [&]() {
            if (!structureChosen || structurePath.empty() || !structurePainted) {
                structureNote = "还不能保存";
                saveFlash = 2.0f;
                return;
            }
            structureNote = structure::saveFile(world, structurePath) ? "已保存建筑" : "保存失败";
            saveFlash = 2.0f;
        };

        InputState in;
        if (canMove) {
            in.forward = keyDown('W');
            in.back = keyDown('S');
            in.left = keyDown('A');
            in.right = keyDown('D');
            in.sprint = keyDown(VK_CONTROL) || (keyDown(VK_SHIFT) && !player.flying);
            in.jump = keyDown(VK_SPACE);
            in.sneak = keyDown(VK_SHIFT);
        }

        if (sim) {
            in.forward = true;
            if ((frameCounter / 150) % 2 == 0) in.jump = true;
        }

        // Frozen is authoritative on the dedicated server. Mirror the last
        // snapshot locally only to avoid prediction jitter; view rotation is
        // deliberately left untouched.
        if (roomSession && (roomPlayerStatus & kStatusFrozen)) {
            in.forward = in.back = in.left = in.right = false;
            in.sprint = in.jump = false;
            player.vel.x = player.vel.z = 0.0f;
        }

        roomMovement = (in.forward ? kMoveForward : 0) | (in.back ? kMoveBack : 0) |
            (in.left ? kMoveLeft : 0) | (in.right ? kMoveRight : 0) |
            (in.jump ? kMoveJump : 0) | (in.sneak ? kMoveSneak : 0) | (in.sprint ? kMoveSprint : 0);

        bool f3 = keyDown(VK_F3);
        if (playing && !roomSession && f3 && !prevF3) showDebug = !showDebug;
        if (roomSession) showDebug = false;
        prevF3 = f3;
        bool f = keyDown('F');
        bool fPressed = playing && f && !prevF;
        prevF = f;
        bool f5 = keyDown(VK_F5);
        if (playing && f5 && !prevF5 && !spectating && !structureEdit) ui.camMode = (ui.camMode + 1) % 3;
        if (structureEdit && playing && f5 && !prevF5 && !structurePicker)
            saveCurrentStructure();
        if (spectating) ui.camMode = 0;
        prevF5 = f5;
        bool f8 = keyDown(VK_F8);
        if (clueQa && playing && roomSession && !paused && !deploying && !clueOpen && !spectating &&
            !player.dead && f8 && !prevF8)
            gameClient.sendDeploy(3, 0, 0);
        prevF8 = f8;
        bool e = keyDown('E');
        if (playing && e && !prevE && !spectating && !deploying && !guideOpen && !clueOpen) {
            if (structureEdit) {
                if (!structurePicker) blockBarOpen = !blockBarOpen;
            } else if (!paused) {
                if (targetPanel >= 0) {
                    targetPanel = -1;
                } else {
                    if (inventoryOpen && drag.active) endDrag(inv, worn, -1, -1, drag);
                    inventoryOpen = !inventoryOpen;
                    ui.noteOpen = false;
                    if (inventoryOpen) { drag.active = false; drag.block = AIR; drag.count = 0; drag.sourceSlot = -1; drag.sourceWear = -1; }
                }
            }
        }
        prevE = e;
        if (playing && roomSession && !ritualDone && !paused && !storyOpen && !deploying) {
            int rid = ritual::assignedRitual(gameClient.team());
            if (rid >= 0 && structure::offeringReady(world, rid))
                ritualDone = true;
        }
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
                } else if (appScreen == AppScreen::RoomLobby || appScreen == AppScreen::JoinRoom ||
                           appScreen == AppScreen::RoomLoading) {
                    if (ui.portFieldActive) applyRoomPort();
                    shutdownRoom();
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
            else if (debugMenuOpen && ui.trialPick) ui.trialPick = false;
            else if (debugMenuOpen) debugMenuOpen = false;
            else if (settingsOpen) settingsOpen = false;
            else if (deploying) {
            }
            else if (storyOpen) {
            }
            else if (guideOpen) {
                guideOpen = false;
                firstLook = true;
            }
            else if (clueOpen) {
                clueOpen = false;
                clueQuiz = {};
                clueQuizSubmitting = false;
                firstLook = true;
            }
            else if (targetPanel >= 0) {
                targetPanel = -1;
            }
            else if (inventoryOpen) {
                if (drag.active) endDrag(inv, worn, -1, -1, drag);
                inventoryOpen = false;
            }
            else if (structureEdit && structureNaming) {
                structureNaming = false;
                g_textTarget = nullptr;
                ui.nameFieldActive = false;
                ui.menuMessage.clear();
            }
            else if (structureEdit && structurePicker) {
                if (structureCanReturn && structureChosen) {
                    structurePicker = false;
                    ui.menuMessage.clear();
                    firstLook = true;
                } else {
                    paused = !paused;
                }
            }
            else if (structureEdit && blockBarOpen) {
                blockBarOpen = false;
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
        } else if (g_wheel != 0 && structureEdit && structurePicker && !structureNaming && !paused) {
            int steps = g_wheel / 120;
            if (steps == 0) steps = (g_wheel > 0) ? 1 : -1;
            ui.structureScroll -= steps;
        } else if (g_wheel != 0 && debugMenuOpen && ui.trialPick) {
            int steps = g_wheel / 120;
            if (steps == 0) steps = (g_wheel > 0) ? 1 : -1;
            ui.trialScroll -= steps;
        } else if (g_wheel != 0 && structureEdit && blockBarOpen) {
            int steps = g_wheel / 120;
            if (steps == 0) steps = (g_wheel > 0) ? 1 : -1;
            ui.blockBarScroll -= steps;
        } else if (g_wheel != 0 && !structureEdit) {
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
        if (storyOpen && playing) {
            bool advance = g_storyAdvance || (lmb && !prevLmb) || (rmb && !prevRmb);
            g_storyAdvance = false;
            const float fade = 0.4f;
            int count = ritual::storyLineCount(ritual::assignedRitual(gameClient.team()));
            if (count < 1) {
                storyOpen = false;
                player.flying = spectating;
                player.noclip = spectating;
                firstLook = true;
            } else if (storyPhase == 0) {
                storyAlpha += dt / fade;
                if (storyAlpha >= 1.0f) { storyAlpha = 1.0f; storyPhase = 1; }
                if (advance && storyAlpha > 0.2f) storyPhase = 2;
            } else if (storyPhase == 1) {
                if (advance) storyPhase = 2;
            } else {
                storyAlpha -= dt / fade;
                if (storyAlpha <= 0.0f) {
                    storyAlpha = 0.0f;
                    storyIndex++;
                    if (storyIndex >= count) {
                        storyOpen = false;
                        player.flying = spectating;
                        player.noclip = spectating;
                        player.vel = { 0, 0, 0 };
                        firstLook = true;
                    } else {
                        storyPhase = 0;
                    }
                }
            }
        } else {
            g_storyAdvance = false;
        }
        if (deploying && playing && !paused && deploySpan > 0) {
            if (lmb && !prevLmb && ui.deployMapS > 1.0f) {
                float lx = ui.mouseX - ui.deployMapX;
                float ly = ui.mouseY - ui.deployMapY;
                if (lx >= 0.0f && ly >= 0.0f && lx < ui.deployMapS && ly < ui.deployMapS) {
                    int localX = (int)(lx / ui.deployMapS * (float)deploySpan);
                    int localZ = (int)(ly / ui.deployMapS * (float)deploySpan);
                    if (localX < 0) localX = 0;
                    if (localZ < 0) localZ = 0;
                    if (localX >= deploySpan) localX = deploySpan - 1;
                    if (localZ >= deploySpan) localZ = deploySpan - 1;
                    gameClient.sendDeploy(1, deployOx + localX, deployOz + localZ);
                }
            }
            if (rmb && !prevRmb) gameClient.sendDeploy(0, 0, 0);
        }
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
        bool entityTarget = false;
        float entityDist = 1.0e9f;
        uint8_t entityAttackHand = 1;
        uint8_t entityAttackItem = AIR;
        int dummyAim = -1;
        bool specialUseClick = false;
        auto selectedCombatWeapon = [&](uint8_t& item, uint8_t& hand) {
            int rightSlot = cfg::HAND_SLOTS + ui.selectedRight;
            int leftSlot = ui.selectedLeft;
            if (rightSlot >= cfg::HAND_SLOTS && rightSlot < cfg::HOTBAR_SLOTS &&
                combat::slotUsable(player.vitals, rightSlot) && combat::weapon(inv[rightSlot].block)) {
                item = inv[rightSlot].block;
                hand = 1;
                return true;
            }
            if (leftSlot >= 0 && leftSlot < cfg::HAND_SLOTS &&
                combat::slotUsable(player.vitals, leftSlot) && combat::weapon(inv[leftSlot].block)) {
                item = inv[leftSlot].block;
                hand = 0;
                return true;
            }
            item = AIR;
            hand = 1;
            return false;
        };
        if (playing) {
            hitOk = world.raycast(player.eye(), player.lookDir(), cfg::REACH, hit, prev, nrm, &physHit, &hitT);
            dropHit = world.raycastDrop(player.eye(), player.lookDir(), cfg::REACH, dropT);
            if (dropHit >= 0 && dropT <= hitT) {
                hitOk = false;
                physHit = -1;
            } else {
                dropHit = -1;
            }
            if (roomSession && !spectating && !player.dead && !inventoryOpen && !paused &&
                !guideOpen && !clueOpen) {
                uint8_t weaponItem = AIR;
                selectedCombatWeapon(weaponItem, entityAttackHand);
                entityAttackItem = weaponItem;
                if (auto def = combat::weapon(weaponItem)) {
                    float obstruction = std::min(def->reach + .01f, hitT);
                    if (dropHit >= 0) obstruction = std::min(obstruction, dropT);
                    float best = obstruction;
                    for (const RemoteAvatar& remote : remotes) {
                        int remoteTeam = -1;
                        for (const RoomPlayerView& rp : roomPlayers)
                            if (rp.id == remote.id) { remoteTeam = rp.team; break; }
                        if (remote.spectator || remote.dead || remoteTeam == gameClient.team()) continue;
                        auto entity = combat::rayPlayer(player.eye(), player.lookDir(), remote.pos,
                                                        remote.bodyYaw, def->reach, best);
                        if (entity && entity->distance < best) {
                            best = entity->distance;
                            entityDist = entity->distance;
                            entityTarget = true;
                        }
                    }
                    if (entityTarget) { hitOk = false; physHit = -1; dropHit = -1; }
                }
            }
        }
        if (playing && trialAnchor.active)
            structure::ensureTrialCore(world);
        int guardianRelic = -1;
        structure::GuardianSpan guardianHit;
        float guardianT = cfg::REACH + 1.0f;
        if (playing && !inventoryOpen && !paused && !spectating && !structureEdit) {
            if (structure::raycastGuardian(world, player.eye(), player.lookDir(), cfg::REACH, guardianT, guardianHit)) {
                bool closerThanBlock = !hitOk || guardianT <= hitT;
                bool closerThanDrop = dropHit < 0 || guardianT <= dropT;
                if (closerThanBlock && closerThanDrop) {
                    hitOk = false;
                    physHit = -1;
                    dropHit = -1;
                    guardianRelic = guardianHit.relic;
                }
            }
        }
        ui.targetGuardian = guardianRelic;
        ui.guardianHurt = 0.0f;
        ui.bossNear = false;
        ui.bossRelic = -1;
        if (playing && !spectating) {
            structure::GuardianSpan nearBoss;
            if (structure::nearestGuardian(world, player.pos, 16.0f, nearBoss)) {
                ui.bossNear = true;
                ui.bossRelic = nearBoss.relic;
                ui.bossName = ritual::relicName(nearBoss.relic);
                ui.bossHp = nearBoss.hp;
                ui.bossMaxHp = nearBoss.maxHp > 0 ? nearBoss.maxHp : 1;
            }
        }
        if (guardianRelic >= 0 && guardianHit.maxHp > 0) {
            ui.guardianCenter = {
                (guardianHit.minX + guardianHit.maxX) * 0.5f,
                (guardianHit.minY + guardianHit.maxY) * 0.5f,
                (guardianHit.minZ + guardianHit.maxZ) * 0.5f
            };
            ui.guardianSize = {
                (guardianHit.maxX - guardianHit.minX) * 1.04f,
                (guardianHit.maxY - guardianHit.minY) * 1.04f,
                (guardianHit.maxZ - guardianHit.minZ) * 1.04f
            };
            ui.guardianHurt = 1.0f - (float)guardianHit.hp / (float)guardianHit.maxHp;
        }
        if (playing && !inventoryOpen && !paused && !spectating && !structureEdit &&
            !player.dead && targetPanel < 0) {
            float limit = cfg::REACH;
            if (hitOk) limit = std::min(limit, hitT);
            if (dropHit >= 0) limit = std::min(limit, dropT);
            if (guardianRelic >= 0) limit = std::min(limit, guardianT);
            if (entityTarget) limit = std::min(limit, entityDist);
            float bestDummy = limit;
            for (int i = 0; i < (int)targets.size(); ++i) {
                auto hit = combat::rayPlayer(player.eye(), player.lookDir(), targets[(size_t)i].feet,
                                             targets[(size_t)i].yaw, limit, bestDummy);
                if (hit && hit->distance < bestDummy) {
                    bestDummy = hit->distance;
                    dummyAim = i;
                }
            }
            if (dummyAim >= 0) {
                hitOk = false;
                physHit = -1;
                dropHit = -1;
                guardianRelic = -1;
                entityTarget = false;
            }
        }
        ui.targetGuardian = guardianRelic;
        ui.hasTarget = hitOk && !inventoryOpen && !paused && !guideOpen && !clueOpen &&
                       playing && !spectating;
        ui.targetBlock = hit;
        ui.hasPlacePreview = false;
        ui.targetPlaceReady = false;
        if (hitOk && physHit < 0 && !player.dead && carry.empty() && lookLocked && targetPanel < 0) {
            auto heldTarget = [&](int slot) {
                return slot >= 0 && slot < cfg::INVENTORY_SLOTS && !inv[slot].empty()
                    && inv[slot].block == ITEM_TARGET;
            };
            int slot = heldTarget(ui.selectedSlot) ? ui.selectedSlot
                     : (heldTarget(ui.selectedLeft) ? ui.selectedLeft : -1);
            if (slot >= 0) {
                uint8_t beside = world.getBlock(prev.x, prev.y, prev.z);
                if ((beside == AIR || isLiquid(beside)) && !playerOverlapsCell(prev, player.pos))
                    ui.targetPlaceReady = true;
            }
        }
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
        ui.targetDrop = (dropHit >= 0 && !inventoryOpen && !paused && !guideOpen && !clueOpen &&
                         playing && !spectating) ? dropHit : -1;
        // Small rotating item meshes are hard to hit precisely with the
        // crosshair. In a room, F may also pick a nearby visible floor drop;
        // the dedicated server validates the same ID again before transfer.
        if (roomSession && ui.targetDrop < 0 && playing && !spectating && !player.dead &&
            !inventoryOpen && !paused && !guideOpen && !clueOpen && !structureEdit)
            ui.targetDrop = room_inventory::nearbyDrop(world, player.eye(), player.pos);
        ui.processLogReady = false;
        if (hitOk && physHit < 0 && !player.dead && carry.empty() && lookLocked &&
            !inventoryOpen && !paused && !guideOpen && !clueOpen && playing && !spectating &&
            !structureEdit && ui.targetDrop < 0 && dummyAim < 0) {
            uint8_t heldProc = inv[ui.selectedSlot].block;
            bool stripTool = hasItemTags(heldProc, TAG_AXE | TAG_WOODWORKING | TAG_ONE_HAND);
            if (stripTool && world.getBlock(hit.x, hit.y, hit.z) == LOG)
                ui.processLogReady = true;
        }
        // Air, a hostile player, or a guardian all start the same swing. The
        // server raycasts again at the damage frame, so the click itself does
        // not need a target. A block under the crosshair stays a mining swing.
        bool canStartMelee = dummyAim < 0 && dropHit < 0 && (entityTarget || !hitOk);
        if (roomSession && playing && !structureEdit && canStartMelee && lmb && !prevLmb &&
            !roomAttackPending && roomAttackVisualItem == AIR && lookLocked && !paused && !inventoryOpen && !deploying &&
            !storyOpen && !guideOpen && !clueOpen && !spectating && !player.dead && carry.empty()) {
            uint8_t attackItem = entityAttackItem;
            uint8_t attackHand = entityAttackHand;
            if (attackItem == AIR) selectedCombatWeapon(attackItem, attackHand);
            if (combat::weapon(attackItem)) {
                if (!++roomAttackNext) ++roomAttackNext;
                roomAttackPending = roomAttackNext;
                roomAttackHand = attackHand;
                roomAttackVisualItem = attackItem;
                roomAttackVisualTime = 0.0001f;
            }
        }
        if (roomSession && rmb && !prevRmb && lookLocked && !paused && !inventoryOpen &&
            !deploying && !storyOpen && !structureEdit && !spectating && !player.dead && carry.empty()) {
            int rightSlot = cfg::HAND_SLOTS + ui.selectedRight;
            int leftSlot = ui.selectedLeft;
            int useSlot = -1;
            uint8_t useHand = 1;
            auto specialItem = [](uint8_t item) {
                return item == ITEM_ARCANE_FIREBALL || item == ITEM_ARCANE_FREEZE ||
                       item == ITEM_ARCANE_HEAL || item == ITEM_GUIDE_BOOK || item == ITEM_CLUE;
            };
            if (rightSlot >= cfg::HAND_SLOTS && rightSlot < cfg::HOTBAR_SLOTS &&
                specialItem(inv[rightSlot].block)) {
                useSlot = rightSlot;
            } else if (leftSlot >= 0 && leftSlot < cfg::HAND_SLOTS && specialItem(inv[leftSlot].block)) {
                useSlot = leftSlot;
                useHand = 0;
            }
            if (useSlot >= 0) {
                specialUseClick = true;
                uint8_t used = inv[useSlot].block;
                if (combat::slotUsable(player.vitals, useSlot)) {
                    if (used == ITEM_ARCANE_FIREBALL || used == ITEM_ARCANE_FREEZE ||
                        used == ITEM_ARCANE_HEAL) {
                        if (!roomCastPending) {
                            if (!++roomCastNext) ++roomCastNext;
                            roomCastPending = roomCastNext;
                            roomCastHand = useHand;
                        }
                    } else if (used == ITEM_GUIDE_BOOK) {
                        guidePage = 0;
                        guideOpen = true;
                        clueOpen = false;
                        clueQuiz = {};
                        roomMovement = 0;
                        in = InputState{};
                    } else if (used == ITEM_CLUE) {
                        clueOpen = true;
                        clueQuiz = {};
                        guideOpen = false;
                        roomMovement = 0;
                        in = InputState{};
                    }
                }
            }
        }
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

        if (fPressed && !paused && !ui.matEditorOpen && playing && !player.dead && !spectating &&
            !structureEdit && !guideOpen && !clueOpen) {
            if (targetPanel >= 0) {
                targetPanel = -1;
            } else if (!inventoryOpen && dummyAim >= 0) {
                targetPanel = dummyAim;
            } else if (!inventoryOpen && !carry.empty()) {
                if (!roomSession) dropCarriedBlock(world, player, carry);
            } else if (!inventoryOpen && ui.targetDrop >= 0 && ui.targetDrop < (int)world.drops().size()) {
                const loot::Drop& d = world.drops()[(size_t)ui.targetDrop];
                if (roomSession) {
                    if (d.netId && !roomPickupPending) {
                        if (!++roomPickupNext) ++roomPickupNext;
                        roomPickupPending = roomPickupNext;
                        roomPickupDrop = d.netId;
                    }
                    ui.targetDrop = -1;
                } else {
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
            } else if (!inventoryOpen && ui.processLogReady && lookLocked) {
                // Hand axe: strip LOG in place to WOOD (same axis), drop 4 BARK sheets.
                IVec3 cell = ui.targetBlock;
                if (physHit < 0 && world.getBlock(cell.x, cell.y, cell.z) == LOG) {
                    uint8_t heldProc = inv[ui.selectedSlot].block;
                    if (hasItemTags(heldProc, TAG_AXE | TAG_WOODWORKING | TAG_ONE_HAND)) {
                        int axis = world.logAxisAt(cell.x, cell.y, cell.z);
                        int placeFace = (axis == 0) ? 2 : ((axis == 2) ? 4 : 0);
                        uint8_t keep = (uint8_t)(world.getFlags(cell.x, cell.y, cell.z)
                                                 & (uint8_t)~(FLAG_ALIVE | FLAG_SETTLED));
                        int overlayBark = world.takeAllBarkAt(cell.x, cell.y, cell.z);
                        world.setBlock(cell.x, cell.y, cell.z, WOOD, true, true, placeFace, (int)keep);
                        noteRoomEdit(cell.x, cell.y, cell.z, WOOD, placeFace);
                        int barkN = 4 + overlayBark;
                        world.spawnDrop(cellCenter(cell.x, cell.y, cell.z), BARK, barkN, true);
                        ui.processLogReady = false;
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
                bool submitJoin = g_textSubmit && appScreen == AppScreen::JoinRoom;
                if (g_textSubmit && appScreen == AppScreen::CreateWorld) tryCreateWorld();
                if (g_textSubmit && appScreen == AppScreen::RoomLobby && ui.portFieldActive) applyRoomPort();
                g_textSubmit = false;

                auto parsePortText = [](const std::string& text) -> int {
                    if (text.empty()) return -1;
                    int p = 0;
                    for (char c : text) {
                        if (c < '0' || c > '9') return -1;
                        p = p * 10 + (c - '0');
                        if (p > 65535) return -1;
                    }
                    return p < 1 ? -1 : p;
                };
                auto tryJoinRoom = [&]() {
                    int p = parsePortText(ui.joinPortText);
                    if (p < 0) {
                        ui.menuMessage = "端口无效";
                        return;
                    }
                    std::string host = ui.joinHost;
                    while (!host.empty() && (unsigned char)host.front() <= 32) host.erase(host.begin());
                    while (!host.empty() && (unsigned char)host.back() <= 32) host.pop_back();
                    if (host.empty()) host = "127.0.0.1";
                    ui.joinHost = host;
                    g_textTarget = nullptr;
                    g_textDigits = false;
                    ui.joinAddrActive = false;
                    ui.joinPortActive = false;
                    shutdownRoom();
                    std::string err;
                    if (!lobbyGuest.connect(host, (uint16_t)p, ui.playerName, err)) {
                        ui.menuMessage = err.empty() ? "无法连接主机" : err;
                        return;
                    }
                    roomHost = false;
                    roomPort = p;
                    roomHostAddr = host;
                    ui.roomPort = p;
                    ui.roomPortText = std::to_string(p);
                    roomTeams.clear();
                    roomPlayers.clear();
                    pullGuestLobby();
                    appScreen = AppScreen::RoomLobby;
                    ui.appScreen = AppScreen::RoomLobby;
                    ui.menuMessage.clear();
                };
                if (submitJoin) tryJoinRoom();

                if (lmb && !prevLmb) {
                    if (appScreen == AppScreen::Start) {
                        if (ui.portraitHover) {
                            appScreen = AppScreen::PlayerProfile;
                            ui.appScreen = AppScreen::PlayerProfile;
                            ui.nameFieldActive = true;
                            g_textDigits = false;
                            g_textMax = 64;
                            g_textTarget = &ui.playerName;
                            ui.menuMessage.clear();
                        } else if (ui.startHover == 0) {
                            beginRoom();
                        } else if (ui.startHover == 1) {
                            openExplore();
                        } else if (ui.startHover == 2) {
                            appScreen = AppScreen::JoinRoom;
                            ui.appScreen = AppScreen::JoinRoom;
                            ui.menuMessage.clear();
                            ui.joinAddrActive = false;
                            ui.joinPortActive = false;
                            g_textTarget = nullptr;
                            g_textDigits = false;
                        }
                    } else if (appScreen == AppScreen::PlayerProfile) {
                        if (ui.profileHover == 0) {
                            ui.nameFieldActive = true;
                            g_textDigits = false;
                            g_textMax = 64;
                            g_textTarget = &ui.playerName;
                        } else if (ui.profileHover == 1) {
                            importPlayerModel();
                        } else if (ui.profileHover == 2) {
                            g_textTarget = nullptr;
                            g_textDigits = false;
                            ui.nameFieldActive = false;
                            if (ui.playerName.empty()) ui.playerName = "玩家";
                            saveProfile();
                            appScreen = AppScreen::Start;
                            ui.appScreen = AppScreen::Start;
                            ui.menuMessage.clear();
                        } else {
                            ui.nameFieldActive = false;
                            g_textTarget = nullptr;
                            g_textDigits = false;
                        }
                    } else if (appScreen == AppScreen::JoinRoom) {
                        if (ui.joinHover == 0) {
                            ui.joinAddrActive = true;
                            ui.joinPortActive = false;
                            g_textDigits = false;
                            g_textMax = 48;
                            g_textTarget = &ui.joinHost;
                        } else if (ui.joinHover == 1) {
                            ui.joinPortActive = true;
                            ui.joinAddrActive = false;
                            g_textDigits = true;
                            g_textMax = 5;
                            g_textTarget = &ui.joinPortText;
                        } else if (ui.joinHover == 2) {
                            tryJoinRoom();
                        } else if (ui.joinHover == 3) {
                            g_textTarget = nullptr;
                            g_textDigits = false;
                            ui.joinAddrActive = false;
                            ui.joinPortActive = false;
                            appScreen = AppScreen::Start;
                            ui.appScreen = AppScreen::Start;
                            ui.menuMessage.clear();
                        } else {
                            ui.joinAddrActive = false;
                            ui.joinPortActive = false;
                            g_textTarget = nullptr;
                            g_textDigits = false;
                        }
                    } else if (appScreen == AppScreen::RoomLoading) {
                        if (ui.loadHover == 0) {
                            shutdownRoom();
                            roomTeams.clear();
                            roomPlayers.clear();
                            spectating = false;
                            roomSession = false;
                            appScreen = AppScreen::Start;
                            ui.appScreen = AppScreen::Start;
                            ui.loadStatus.clear();
                            ui.menuMessage.clear();
                        }
                    } else if (appScreen == AppScreen::RoomLobby) {
                        if (ui.portFieldHover && roomHost) {
                            ui.portFieldActive = true;
                            g_textDigits = true;
                            g_textMax = 5;
                            g_textTarget = &ui.roomPortText;
                        } else if (ui.portFieldActive) {
                            applyRoomPort();
                        }
                        if (!ui.portFieldActive) {
                            if (ui.lobbyJoinHover >= 0)
                                joinRoomTeam(ui.lobbyJoinHover);
                            else if (ui.lobbyBtnHover == 0)
                                addRoomTeam();
                            else if (ui.lobbyBtnHover == 1)
                                startRoom();
                            else if (ui.lobbyBtnHover == 2) {
                                shutdownRoom();
                                roomTeams.clear();
                                roomPlayers.clear();
                                spectating = false;
                                roomSession = false;
                                appScreen = AppScreen::Start;
                                ui.appScreen = AppScreen::Start;
                                ui.menuMessage.clear();
                            }
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
            } else if (structureEdit && structurePicker && !paused) {
                if (structureNaming && g_textSubmit) tryCreateStructure();
                g_textSubmit = false;
                if (lmb && !prevLmb) {
                    if (structureNaming) {
                        if (ui.structureBtnHover == 0) {
                            ui.nameFieldActive = true;
                            g_textTarget = &ui.structureNewName;
                            g_textMax = 48;
                        } else if (ui.structureBtnHover == 1) {
                            tryCreateStructure();
                        } else if (ui.structureBtnHover == 2) {
                            structureNaming = false;
                            g_textTarget = nullptr;
                            ui.nameFieldActive = false;
                            ui.menuMessage.clear();
                        } else {
                            ui.nameFieldActive = false;
                            g_textTarget = nullptr;
                        }
                    } else if (ui.structureDeleteHover >= 0 && ui.structureDeleteHover < (int)ui.structureNames.size()) {
                        const std::string& name = ui.structureNames[(size_t)ui.structureDeleteHover];
                        if (ui.structurePendingDelete != name) {
                            ui.structurePendingDelete = name;
                            ui.menuMessage = "再点一次确认删除";
                        } else if (!structure::deleteFile(name)) {
                            ui.menuMessage = "删除失败";
                        } else {
                            ui.structurePendingDelete.clear();
                            ui.menuMessage = "已删除";
                            refreshStructureList();
                        }
                    } else if (ui.structureItemHover >= 0 && ui.structureItemHover < (int)ui.structureNames.size()) {
                        loadStructureFile(ui.structureNames[(size_t)ui.structureItemHover]);
                    } else if (ui.structureBtnHover == 0) {
                        structureNaming = true;
                        ui.structureNewName.clear();
                        ui.nameFieldActive = true;
                        g_textTarget = &ui.structureNewName;
                        g_textMax = 48;
                        g_textDigits = false;
                        ui.menuMessage.clear();
                        ui.structurePendingDelete.clear();
                    } else if (ui.structureBtnHover == 1 && structureCanReturn && structureChosen) {
                        structurePicker = false;
                        ui.menuMessage.clear();
                        g_textTarget = nullptr;
                        ui.nameFieldActive = false;
                        firstLook = true;
                    } else {
                        ui.structurePendingDelete.clear();
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
                    if (ui.trialPick) {
                        if (lmb && !prevLmb && ui.trialHover == -2) ui.trialPick = false;
                        else if (lmb && !prevLmb && ui.trialHover >= 0 && ui.trialHover < ritual::RelicCount)
                            enterTrial(ui.trialHover);
                    } else if (lmb && !prevLmb && ui.debugHover == 0) debugMenuOpen = false; // back
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
                        if (!player.privilegeMode && !trialAnchor.active) ui.railOpen = false;
                        if (!player.privilegeMode) ui.quickBreak = false;
                    }
                    if (!ui.trialPick && lmb && !prevLmb && ui.railHover == 0)
                        ui.railOpen = !ui.railOpen;
                    if (!ui.trialPick && lmb && !prevLmb && ui.railHover == 1) {
                        if (trialAnchor.active) exitTrial();
                        else if (player.privilegeMode && !roomSession && !structureEdit) {
                            ui.trialPick = true;
                            ui.trialScroll = 0;
                        }
                    }
                    if (!ui.trialPick && lmb && !prevLmb && ui.railHover == 2) {
                        player.flying = !player.flying;
                        if (!player.flying) player.vel.y = 0.0f;
                    }
                    if (!ui.trialPick && lmb && !prevLmb && ui.railHover == 3 && player.privilegeMode)
                        ui.quickBreak = !ui.quickBreak;
                    if (!ui.trialPick && lmb && ui.tickSliderW > 1.0f &&
                        ui.mouseX >= ui.tickSliderX - 12.0f && ui.mouseX <= ui.tickSliderX + ui.tickSliderW + 12.0f &&
                        ui.mouseY >= ui.tickSliderY - 12.0f && ui.mouseY <= ui.tickSliderY + ui.tickSliderH + 12.0f) {
                        float t = (ui.mouseX - ui.tickSliderX) / ui.tickSliderW;
                        t = clampf(t, 0.0f, 1.0f);
                        tickSpeed = t * 20.0f;
                    }
                    if (!ui.trialPick && lmb && ui.timeSliderW > 1.0f &&
                        ui.mouseX >= ui.timeSliderX - 12.0f && ui.mouseX <= ui.timeSliderX + ui.timeSliderW + 12.0f &&
                        ui.mouseY >= ui.timeSliderY - 12.0f && ui.mouseY <= ui.timeSliderY + ui.timeSliderH + 12.0f) {
                        float t = (ui.mouseX - ui.timeSliderX) / ui.timeSliderW;
                        t = clampf(t, 0.0f, 1.0f);
                        timeOfDay = t * (float)cfg::TICKS_PER_DAY;
                    }
                } else {
                    if (structureEdit && lmb && ui.timeSliderW > 1.0f &&
                        ui.mouseX >= ui.timeSliderX - 12.0f && ui.mouseX <= ui.timeSliderX + ui.timeSliderW + 12.0f &&
                        ui.mouseY >= ui.timeSliderY - 12.0f && ui.mouseY <= ui.timeSliderY + ui.timeSliderH + 12.0f) {
                        float t = (ui.mouseX - ui.timeSliderX) / ui.timeSliderW;
                        t = clampf(t, 0.0f, 1.0f);
                        timeOfDay = t * (float)cfg::TICKS_PER_DAY;
                    }
                    if (lmb && !prevLmb) {
                        if (roomSession) {
                            if (ui.menuHover == 0) { paused = false; settingsOpen = false; debugMenuOpen = false; }
                            else if (ui.menuHover == 1) { settingsOpen = true; }
                            else if (ui.menuHover == 2) leaveWorld();
                        } else if (ui.menuHover == 0) { paused = false; settingsOpen = false; debugMenuOpen = false; }
                        else if (ui.menuHover == 1) { settingsOpen = true; }
                        else if (ui.menuHover == 2) { debugMenuOpen = true; }
                        else if (ui.menuHover == 3) leaveWorld();
                    }
                }
            } else if (guideOpen) {
                if (lmb && !prevLmb) {
                    if (ui.guidePrevHover && guidePage > 0) --guidePage;
                    else if (ui.guideNextHover && guidePage + 1 < guide::kPageCount) ++guidePage;
                    else if (ui.guideCloseHover) { guideOpen = false; firstLook = true; }
                }
            } else if (clueOpen) {
                if (lmb && !prevLmb) {
                    if (clueQuiz.status == 1 && !clueQuizSubmitting) {
                        for (int option = 0; option < 4; ++option) {
                            if (!ui.clueQuizOptionHover[(size_t)option]) continue;
                            gameClient.sendClueAnswer(clueQuiz.challengeId, (uint8_t)option);
                            clueQuizSubmitting = true;
                            break;
                        }
                    }
                    if (clueQuiz.status ? ui.clueQuizCloseHover : ui.clueCloseHover) {
                        clueOpen = false;
                        clueQuiz = {};
                        clueQuizSubmitting = false;
                        firstLook = true;
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
                if (lmb && !prevLmb && ui.noteHover) {
                    if (drag.active) endDrag(inv, worn, -1, -1, drag);
                    ui.noteOpen = !ui.noteOpen;
                } else if (ui.noteOpen && lmb && !prevLmb && ui.noteBackHover) {
                    ui.noteOpen = false;
                } else if (!ui.noteOpen && lmb && !prevLmb) {
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
                    if (!roomSession && drop < 0 && dropWear < 0 && !ui.pointerInInventory
                        && tossDrop(world, player, drag.block, drag.count))
                        clearDrag(drag);
                    else
                        endDrag(inv, worn, drop, dropWear, drag);
                }
                if (rmb && !prevRmb) {
                    if (drag.active) endDrag(inv, worn, -1, -1, drag);
                    else inventoryOpen = false;
                }
            } else if (targetPanel >= 0) {
                player.mineCharge = 0.0f;
                player.mineCooldown = 0.0f;
                player.strikeName.clear();
                player.pickRaised = false;
                if (lmb && !prevLmb && targetPanel < (int)targets.size()) {
                    if (ui.targetBtnHover == 0) {
                        targets.erase(targets.begin() + targetPanel);
                        targetPanel = -1;
                        dummyAim = -1;
                    } else if (ui.targetBtnHover == 1) {
                        vitals::reset(targets[(size_t)targetPanel].vitals);
                    }
                }
            } else if (player.dead && !deploying) {
                player.strikeName.clear();
                player.pickRaised = false;
                player.mineCharge = 0.0f;
                if (lmb && !prevLmb && ui.deathHover == 0) {
                    vitals::reset(player.vitals);
                    vitals::resetFatigue(player.fatigue);
                    player.dead = false;
                    player.vel = { 0, 0, 0 };
                    if (trialAnchor.active) placeTrialSpawn();
                    else spawnPlayer();
                }
            } else if (!carry.empty() && !structureEdit) {
                player.mineCharge = 0.0f;
                player.mineCooldown -= dt;
                if (player.mineCooldown < 0.0f) player.mineCooldown = 0.0f;
                player.strikeName.clear();
                player.pickRaised = false;
                if (rmb && !prevRmb && ui.hasPlacePreview) {
                    IVec3 place = ui.placePreview;
                    if (world.getBlock(place.x, place.y, place.z) == AIR
                        && plugin::blockStrategy(carry.block)->canPlace(carry.block)) {
                        int face = world.faceFromHitNormal(nrm);
                        if (world.setBlock(place.x, place.y, place.z, carry.block, true, true, face)) {
                            noteRoomEdit(place.x, place.y, place.z, carry.block, face);
                            carry.clear();
                            ui.hasPlacePreview = false;
                        }
                    }
                }
            } else {
                if (lmb && !prevLmb && hitOk && ui.targetDrop < 0) {
                    uint8_t held = inv[ui.selectedSlot].block;
                    if (physHit < 0) {
                        int face = world.faceFromHitNormal(nrm);
                        bool barkTool = hasItemTags(held, TAG_AXE | TAG_WOODWORKING | TAG_ONE_HAND);
                        if (barkTool && world.hasBarkFace(hit.x, hit.y, hit.z, face)) {
                            if (world.takeBarkFace(hit.x, hit.y, hit.z, face)) {
                                noteRoomBark(hit.x, hit.y, hit.z, face, false);
                                world.spawnDrop(cellCenter(hit.x, hit.y, hit.z), BARK, 1, true);
                            }
                        }
                    }
                }
                player.mineCooldown -= dt;
                if (player.mineCooldown < 0.0f) player.mineCooldown = 0.0f;
                if (player.mineCharge <= 0.0f && player.mineCooldown <= 0.0f)
                    player.strikeName.clear();

                uint8_t heldMine = inv[ui.selectedSlot].block;
                if (structureEdit && blockBarOpen && lmb && !prevLmb && ui.structureOpHover == 0)
                    saveCurrentStructure();
                if (structureEdit && blockBarOpen && lmb && !prevLmb && ui.structureOpHover == 1)
                    openStructurePicker(true);
                if (structureEdit && blockBarOpen && lmb && !prevLmb && ui.blockBarHover >= 0) {
                    std::vector<uint8_t> blocks;
                    structure::collectBuildBlocks(blocks);
                    if (ui.blockBarHover < (int)blocks.size())
                        editBlock = blocks[(size_t)ui.blockBarHover];
                }
                if (structureEdit && lookLocked && hitOk && lmb && !prevLmb && structure::inVolume(hit.x, hit.y, hit.z))
                    world.setBlock(hit.x, hit.y, hit.z, AIR, false, true);
                if (structureEdit && lookLocked && hitOk && rmb && !prevRmb && structure::inVolume(prev.x, prev.y, prev.z)
                    && editBlock != AIR && plugin::blockStrategy(editBlock)->canPlace(editBlock))
                    world.setBlock(prev.x, prev.y, prev.z, editBlock, false, true,
                                   world.faceFromHitNormal(nrm));
                bool quickBroke = false;
                if (ui.quickBreak && player.privilegeMode && !roomSession && !structureEdit
                    && lmb && hitOk && ui.targetDrop < 0 && lookLocked && !player.dead) {
                    uint8_t b = physHit < 0
                        ? world.getBlock(hit.x, hit.y, hit.z)
                        : world.getPhysBlock(physHit, hit.x, hit.y, hit.z);
                    if (b != AIR && plugin::blockStrategy(b)->canBreak(b)) {
                        player.mineCharge = 0.0f;
                        player.mineCooldown = 0.0f;
                        player.strikeName.clear();
                        player.pickRaised = false;
                        quickBroke = true;
                        // A click stays down for several frames. Space the breaks so one
                        // click removes a single block; holding keeps breaking at this rate.
                        if (ui.quickBreakWait <= 0.0f) {
                            finishMinedBlock(world, inv[ui.selectedSlot].block, physHit, hit);
                            ui.quickBreakWait = 0.28f;
                        }
                    }
                }
                // Left click starts the swing with no target. Releasing does not
                // cancel it. Damage and block cracks are applied only if the
                // crosshair is on that object or block at the impact frame.
                bool handOk = !roomSession || combat::slotUsable(player.vitals, ui.selectedSlot);
                bool swingGate = !structureEdit && lookLocked && !player.dead && !spectating &&
                    roomAttackVisualItem == AIR && handOk && !quickBroke;
                bool beginSwing = swingGate && lmb && ui.targetDrop < 0 &&
                    player.mineCharge <= 0.0f && player.mineCooldown <= 0.0f;
                bool continueSwing = swingGate && player.mineCharge > 0.0f;
                if (!beginSwing && !continueSwing) {
                    if (player.mineCharge > 0.0f && !swingGate) player.mineCharge = 0.0f;
                } else if (player.mineCooldown > 0.0f && player.mineCharge <= 0.0f) {
                    // Cooling down: holding or click-spam cannot skip this.
                } else {
                    if (player.strikeName.empty()) {
                        if (hasItemTags(heldMine, TAG_AXE)) player.strikeName = "axe_chop";
                        else if (hasItemTags(heldMine, TAG_PICK)) player.strikeName = "pick_mine";
                        else if (heldMine == AIR || !loot::isTool(heldMine)) player.strikeName = "punch";
                        if (player.strikeName != "pick_mine") player.pickRaised = false;
                        player.strikeCharge = hasItemTags(heldMine, TAG_AXE)
                            ? anim::axeChopSec()
                            : loot::mineChargeSec(heldMine);
                        player.strikeCool = loot::mineCooldownSec(heldMine);
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
                        bool connected = false;
                        if (!roomSession && ui.targetGuardian >= 0) {
                            connected = true;
                            if (guardian_fight::vulnerable(ui.targetGuardian)) {
                                int gx = 0, gy = 0, gz = 0;
                                int dmg = structure::guardianStrikeHurt(heldMine, ui.targetGuardian);
                                guardian_fight::noteDamage(ui.targetGuardian, 1, dmg);
                                bool slain = structure::damageGuardian(world, ui.targetGuardian, dmg, gx, gy, gz);
                                if (ui.bossRelic == ui.targetGuardian) {
                                    ui.bossHp -= dmg;
                                    if (ui.bossHp < 0) ui.bossHp = 0;
                                    if (ui.bossMaxHp > 0)
                                        ui.guardianHurt = 1.0f - (float)ui.bossHp / (float)ui.bossMaxHp;
                                    if (slain) ui.bossNear = false;
                                }
                                if (slain) {
                                    uint8_t item = (uint8_t)ritual::blockId(ui.targetGuardian);
                                    const dropgeom::Shape& sh = dropgeom::cached(item);
                                    const float S = cfg::BLOCK_SCALE;
                                    Vec3 dropPos{ (gx + 0.5f) * S, (gy + 1) * S + sh.half.y + 0.04f, (gz + 0.5f) * S };
                                    world.spawnDrop(dropPos, item, 1, true);
                                }
                            }
                        }
                        if (!connected && dummyAim >= 0) {
                            connected = true;
                            uint8_t weaponItem = AIR;
                            uint8_t weaponHand = 1;
                            auto def = selectedCombatWeapon(weaponItem, weaponHand)
                                ? combat::weapon(weaponItem) : std::nullopt;
                            if (def) {
                                float obstruction = def->reach + 0.01f;
                                IVec3 blockHit{}, prevHit{};
                                Vec3 hitNormal{};
                                float blockT = obstruction;
                                if (world.raycast(player.eye(), player.lookDir(), def->reach,
                                                  blockHit, prevHit, hitNormal, nullptr, &blockT))
                                    obstruction = blockT;
                                int best = -1;
                                std::optional<combat::LimbHit> bestHit;
                                for (int i = 0; i < (int)targets.size(); ++i) {
                                    auto limb = combat::rayPlayer(player.eye(), player.lookDir(),
                                                                  targets[(size_t)i].feet, targets[(size_t)i].yaw,
                                                                  def->reach, obstruction);
                                    if (limb && (!bestHit || limb->distance < bestHit->distance)) {
                                        bestHit = limb;
                                        best = i;
                                    }
                                }
                                if (best >= 0 && bestHit) {
                                    combat::DamageSource source{ 1, 1, def->item, combat::DamageCategory::Physical };
                                    auto result = combat::damagePlayer(targets[(size_t)best].vitals, source,
                                                                       def->damage, bestHit->limb);
                                    if (result.applied) hitMarker = 0.22f;
                                }
                            }
                        }
                        bool canBreak = false;
                        if (!connected && hitOk && ui.targetDrop < 0) {
                            if (physHit < 0) {
                                uint8_t b = world.getBlock(hit.x, hit.y, hit.z);
                                canBreak = plugin::blockStrategy(b)->canBreak(b);
                                if (b == GRASS_TUFT && hit.y > 0
                                    && world.getBlock(hit.x, hit.y - 1, hit.z) == GRASS_TUFT)
                                    canBreak = false;
                            } else {
                                uint8_t b = world.getPhysBlock(physHit, hit.x, hit.y, hit.z);
                                canBreak = plugin::blockStrategy(b)->canBreak(b);
                            }
                            if (canBreak) {
                                int face = world.faceFromHitNormal(nrm);
                                float dmg = (physHit < 0 && world.hasSodFace(hit.x, hit.y, hit.z, face))
                                    ? loot::sodMineDamage(heldMine)
                                    : loot::mineDamage(physHit < 0
                                        ? world.getBlock(hit.x, hit.y, hit.z)
                                        : world.getPhysBlock(physHit, hit.x, hit.y, hit.z), heldMine);
                                canBreak = dmg > 0.0f;
                            }
                        }
                        if (canBreak) {
                            int face = world.faceFromHitNormal(nrm);
                            if (roomSession) {
                                uint32_t tree = 0;
                                if (physHit >= 0) {
                                    const std::vector<PhysicsIsland>& islands = world.physicsIslands();
                                    if (physHit < (int)islands.size()) tree = islands[(size_t)physHit].netId;
                                }
                                if (physHit < 0 || tree != 0)
                                    noteRoomMine(hit.x, hit.y, hit.z, face, heldMine, tree);
                            }
                            if (world.applyMineHit(physHit, hit.x, hit.y, hit.z, heldMine, face)) {
                                if (roomSession && physHit >= 0) {
                                    uint8_t b = world.getPhysBlock(physHit, hit.x, hit.y, hit.z);
                                    if (b != AIR && loot::isHarvestBreak(b, heldMine) &&
                                        physHit < (int)world.physicsIslands().size()) {
                                        Vec3 dropPos = tree_fall::worldOf(
                                            world.physicsIslands()[(size_t)physHit], hit.x, hit.y, hit.z);
                                        spawnHarvestDrops(world, dropPos, b, heldMine);
                                    }
                                } else {
                                    finishMinedBlock(world, heldMine, physHit, hit);
                                }
                            }
                        }
                    }
                }
                if (player.mineCharge <= 0.0f && player.mineCooldown <= 0.0f) {
                    player.strikeName.clear();
                    player.pickRaised = false;
                }
                if (!structureEdit && rmb && !prevRmb && hitOk && !specialUseClick && ui.targetPlaceReady) {
                    int slot = (!inv[ui.selectedSlot].empty() && inv[ui.selectedSlot].block == ITEM_TARGET)
                        ? ui.selectedSlot : ui.selectedLeft;
                    if (slot >= 0 && slot < cfg::INVENTORY_SLOTS && !inv[slot].empty()
                        && inv[slot].block == ITEM_TARGET) {
                        const float S = cfg::BLOCK_SCALE;
                        TrainingTarget placed;
                        placed.feet = { (prev.x + 0.5f) * S, prev.y * S, (prev.z + 0.5f) * S };
                        float dx = player.pos.x - placed.feet.x;
                        float dz = player.pos.z - placed.feet.z;
                        placed.yaw = std::atan2(dx, -dz);
                        bool stacked = false;
                        for (const TrainingTarget& old : targets) {
                            float ox = old.feet.x - placed.feet.x;
                            float oy = old.feet.y - placed.feet.y;
                            float oz = old.feet.z - placed.feet.z;
                            if (ox * ox + oy * oy + oz * oz < 0.16f) stacked = true;
                        }
                        if (!stacked) {
                            targets.push_back(placed);
                            if (--inv[slot].count == 0) inv[slot].clear();
                        }
                    }
                } else if (!structureEdit && rmb && !prevRmb && hitOk && !specialUseClick) {
                    if (roomSession && !combat::slotUsable(player.vitals, ui.selectedSlot)) {
                        // A destroyed right hand preserves the slot but cannot use it.
                    } else
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
                                noteRoomBark(host.x, host.y, host.z, face, true);
                                if (--sel.count == 0) sel.clear();
                            }
                        } else {
                            uint8_t target = world.getBlock(hit.x, hit.y, hit.z);
                            plugin::BlockEvent ev{ &world, hit.x, hit.y, hit.z, target, target };
                            if (!plugin::blockStrategy(target)->onInteract(ev)) {
                                bool relic = sel.block >= ITEM_ELEM_CORE && sel.block <= ITEM_EYELESS;
                                IVec3 place = hit;
                                if (!isLiquid(world.getBlock(hit.x, hit.y, hit.z))) place = prev;
                                int rid = ritual::assignedRitual(gameClient.team());
                                bool offering = relic && structure::isOfferingCell(world, rid, place.x, place.y, place.z);
                                if (!sel.empty() && (offering || plugin::blockStrategy(sel.block)->canPlace(sel.block))) {
                                    uint8_t existing = world.getBlock(place.x, place.y, place.z);
                                    if ((existing == AIR || isLiquid(existing)) && !playerOverlapsCell(place, player.pos)) {
                                        int face = world.faceFromHitNormal(nrm);
                                        if (world.setBlock(place.x, place.y, place.z, sel.block, true, true, face)) {
                                            noteRoomEdit(place.x, place.y, place.z, sel.block, face);
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
        if (playing && !structureEdit) {
            for (int i = 0; i < cfg::HAND_SLOTS; i++) {
                if (keyDown('1' + i)) ui.selectedLeft = i;
                if (keyDown('4' + i)) ui.selectedRight = i;
            }
        }
        if (ui.selectedLeft < 0 || ui.selectedLeft >= cfg::HAND_SLOTS) ui.selectedLeft = 0;
        if (ui.selectedRight < 0 || ui.selectedRight >= cfg::HAND_SLOTS) ui.selectedRight = 0;
        ui.selectedSlot = cfg::HAND_SLOTS + ui.selectedRight;

        if (roomSession && roomInventoryKnown && !drag.active && !roomLayoutPending) {
            bool changed = false;
            for (int i = 0; i < cfg::INVENTORY_SLOTS; ++i) {
                if (inv[i].block != roomInventory[i].block || inv[i].count != roomInventory[i].count) {
                    changed = true;
                    break;
                }
            }
            roomLayoutDirty = changed;
        }

        // Fixed-step physics (paused while the menu is open or the player is dead).
        if (spectating) {
            player.flying = true;
            player.noclip = true;
            player.dead = false;
        }
        if (deploying || storyOpen) {
            player.flying = true;
            player.vel = { 0, 0, 0 };
        }
        if (playing && !paused && !ui.matEditorOpen && (!player.dead || deploying || storyOpen)) {
            uint8_t swingItem = roomAttackVisualItem != AIR ? roomAttackVisualItem
                                                            : inv[ui.selectedSlot].block;
            bool toolSwinging = loot::isTool(swingItem) &&
                (player.mineCharge > 1e-4f || player.mineCooldown > 1e-4f || roomAttackVisualItem != AIR);
            player.swingStrafe = toolSwinging ? 0.5f : 1.0f;
            if (deploying || storyOpen) accumulator = 0.0f;
            else accumulator += dt;
            int sub = 0;
            while (accumulator >= cfg::FIXED_DT && sub < cfg::MAX_SUBSTEPS && !deploying && !storyOpen) {
                // Local motion is prediction only; server snapshots correct it.
                player.update(world, in, cfg::FIXED_DT,
                    roomSession && !spectating && !structureEdit
                        ? room_body::limits(player, in, (roomPlayerStatus & kStatusFrozen) != 0)
                        : MovementLimits{});
                if (roomSession && !structureEdit)
                    matchmap::clampOutside(player.pos, player.vel);
                if (!roomSession) world.treeFallPhysics(cfg::FIXED_DT);
                if (!roomSession && !spectating && !structureEdit
                    && stepLocalGuardians(world, player, cfg::FIXED_DT))
                    damageFlash = 0.20f;
                world.updateDrops(cfg::FIXED_DT);
                if (const plugin::EntityModule* em = plugin::findEntity("player")) {
                    plugin::EntityEvent ev{ &world, &player, cfg::FIXED_DT };
                    em->strategy->onTick(ev);
                    em->strategy->think(ev);
                }
                if ((!player.privilegeMode || trialAnchor.active) && !spectating && !roomSession) {
                    vitals::TickInput vin;
                    vin.moving = in.forward || in.back || in.left || in.right;
                    vin.sprint = in.sprint && !player.flying && vitals::canSprint(player.vitals);
                    vin.jumpImpulse = player.jumpedThisUpdate;
                    vin.landed = player.landedThisUpdate;
                    vin.swim = player.inWater;
                    vin.mining = lmb && ui.hasTarget && lookLocked && carry.empty();
                    vin.flying = player.flying;
                    vin.landImpact = player.landImpact;
                    if (roomSession)
                        vin.borderDrain = matchmap::vitalRate(matchmap::outwardT(player.pos.x, player.pos.z));
                    vitals::tick(player.vitals, player.fatigue, vin, cfg::FIXED_DT);
                    if (vitals::isDead(player.vitals) && !structureEdit) {
                        player.dead = true;
                        player.vel = { 0, 0, 0 };
                        if (roomSession && !deploying) openDeploy(true);
                    }
                }
                accumulator -= cfg::FIXED_DT;
                sub++;
            }
            if (sub == cfg::MAX_SUBSTEPS) accumulator = 0.0f;

            int meshBudget = cfg::CLIENT_MESH_BUDGET;
            if (dt > 0.030f) meshBudget = 1;
            else if (dt > 0.022f) meshBudget = 2;
            if (roomSession && meshBudget > 2) meshBudget = 2;
            world.update(player.pos, meshBudget);
            if (deploying && deploySpan > 0) {
                const float S = cfg::BLOCK_SCALE;
                Vec3 zone{
                    (deployOx + deploySpan * 0.5f) * S,
                    player.pos.y,
                    (deployOz + deploySpan * 0.5f) * S
                };
                world.update(zone, 12);
            }
            if (structureEdit && structureChosen && !structurePainted && world.columnLoaded(0, 0)) {
                structure::clearVolume(world);
                structure::paintFile(world, structurePath);
                structurePainted = true;
            }

            if (player.privilegeMode && !trialAnchor.active) {
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
                    if (!roomSession) {
                        world.sodTick(1024, gameTick);
                        world.waterTick(gameTick);
                        world.treeFallGameTick();
                    }
                    gameTick++;
                }
            }

            if (!structureEdit) {
                timeOfDay += dt * ((float)cfg::TICKS_PER_DAY / cfg::DAY_LENGTH_SECONDS);
                if (timeOfDay >= (float)cfg::TICKS_PER_DAY) timeOfDay -= (float)cfg::TICKS_PER_DAY;
            }
        } else if (!playing) {
            if (exploreScreen() || portraitScreen()) {
                timeOfDay = 6000.0f;
                uint32_t want = seedFromName(ui.playerName);
                if (!menuSeedReady || menuSeedApplied != want || world.seed() != want) buildMenuWorld();
                world.update(menuEye, 4);
            } else {
                timeOfDay += dt * 80.0f;
                if (timeOfDay >= (float)cfg::TICKS_PER_DAY) timeOfDay -= (float)cfg::TICKS_PER_DAY;
                if (appScreen != AppScreen::Start && appScreen != AppScreen::PlayerProfile &&
                    appScreen != AppScreen::RoomLobby && appScreen != AppScreen::JoinRoom &&
                    appScreen != AppScreen::RoomLoading) {
                    player.yaw += dt * 0.08f;
                    player.pitch = -0.12f;
                }
            }
        }

        // First-person feedback starts immediately, while damage remains tied to
        // the server's authoritative windup/raycast and resulting CombatEvent.
        if (roomAttackVisualItem != AIR) {
            auto weapon = combat::weapon(roomAttackVisualItem);
            if (!weapon || !playing || player.dead) {
                roomAttackVisualTime = 0.0f;
                roomAttackVisualItem = AIR;
            } else {
                roomAttackVisualTime += dt;
                bool axe = roomAttackVisualItem == HAND_AXE;
                player.strikeName = axe ? "axe_chop" : "pick_mine";
                player.strikeCharge = weapon->windup;
                player.strikeCool = weapon->recovery;
                player.pickRaised = false;
                if (roomAttackVisualTime < weapon->windup) {
                    player.mineCharge = roomAttackVisualTime;
                    player.mineCooldown = 0.0f;
                } else if (roomAttackVisualTime < weapon->windup + weapon->recovery) {
                    player.mineCharge = 0.0f;
                    player.mineCooldown = weapon->windup + weapon->recovery - roomAttackVisualTime;
                } else {
                    player.mineCharge = player.mineCooldown = 0.0f;
                    player.strikeName.clear();
                    roomAttackVisualTime = 0.0f;
                    roomAttackVisualItem = AIR;
                }
            }
        }
        }

        if (g_resized) {
            renderer.setScreenSize(g_winW, g_winH);
            g_resized = false;
        }

        renderer.sync(world);

        ui.showDebug = roomSession ? false : showDebug;
        ui.inventoryOpen = inventoryOpen;
        ui.targets = &targets;
        ui.targetAim = targetPanel >= 0 ? -1 : dummyAim;
        ui.targetPanel = targetPanel;
        ui.bagLocked = inventoryOpen && (in.forward || in.back || in.left || in.right
            || (player.vel.x * player.vel.x + player.vel.z * player.vel.z) > 0.16f);
        ui.appScreen = appScreen;
        ui.menuOpen = paused;
        ui.settingsOpen = settingsOpen;
        ui.mouseSens = mouseSens;
        ui.invertY = invertY;
        ui.debugMenuOpen = roomSession ? false : debugMenuOpen;
        ui.tickSpeed = tickSpeed;
        ui.humidityMode = roomSession ? false : humidityMode;
        ui.privilegeMode = (roomSession && !structureEdit) ? false : player.privilegeMode;
        ui.inTrial = trialAnchor.active;
        if (!debugMenuOpen) ui.trialPick = false;
        ui.hideAvatar = structureEdit || deploying || storyOpen || guideOpen || clueOpen;
        ui.deploying = deploying;
        ui.deployPixels = deploying ? &deployPixels : nullptr;
        ui.deployStamp = deployStamp;
        ui.deploySpan = deploySpan;
        ui.deployPreview = deploying ? matchmap::kDeployPreview : 0;
        ui.deployOx = deployOx;
        ui.deployOz = deployOz;
        ui.deploySeconds = -1;
        ui.deployPins.clear();
        if (deploying) {
            int ti = gameClient.team();
            if (ti >= 0 && ti < (int)roomTeams.size()) {
                ui.deployR = roomTeams[ti].r;
                ui.deployG = roomTeams[ti].g;
                ui.deployB = roomTeams[ti].b;
            }
            for (const DeployPinNet& pin : deployPins) {
                DeployPinView v;
                v.id = pin.id;
                v.name = pin.name;
                v.bx = pin.bx;
                v.bz = pin.bz;
                v.phase = pin.phase;
                v.t = pin.t;
                if (pin.id == gameClient.selfId() && pin.phase == 1) {
                    int sec = (int)std::ceil(pin.t - 0.001f);
                    if (sec < 1) sec = 1;
                    ui.deploySeconds = sec;
                }
                ui.deployPins.push_back(std::move(v));
            }
        }
        ui.structureEdit = structureEdit;
        ui.blockBarOpen = structureEdit && blockBarOpen && !structurePicker;
        ui.structureBlock = editBlock;
        ui.structurePicker = structurePicker;
        ui.structureNaming = structureNaming;
        ui.structureCanReturn = structureCanReturn && structureChosen;
        ui.structureFile = structurePath.empty() ? std::string() : std::filesystem::path(structurePath).stem().string();
        if (roomSession) {
            float t = matchmap::outwardT(player.pos.x, player.pos.z);
            ui.borderT = t;
            ui.borderFog = matchmap::visualFog(t);
            ui.borderActive = true;
        } else {
            ui.borderT = 0.0f;
            ui.borderFog = 0.0f;
            ui.borderActive = false;
        }
        ui.storyOpen = storyOpen && playing;
        ui.storyHold = storyOpen && storyPhase == 1;
        ui.storyFade = storyAlpha;
        ui.storySentence.clear();
        if (storyOpen) {
            int rid = ritual::assignedRitual(gameClient.team());
            if (storyIndex >= 0 && storyIndex < ritual::storyLineCount(rid))
                ui.storySentence = ritual::storyLine(rid, storyIndex);
        }
        if (guidePage < 0) guidePage = 0;
        if (guidePage >= guide::kPageCount) guidePage = guide::kPageCount - 1;
        ui.guideOpen = guideOpen && playing;
        ui.guidePage = guidePage;
        ui.guidePageCount = guide::kPageCount;
        ui.guideTitle = guide::pageTitle(guidePage);
        ui.guideLineCount = guide::pageLineCount(guidePage);
        for (int i = 0; i < 7; ++i)
            ui.guideLines[i] = i < ui.guideLineCount ? guide::pageLine(guidePage, i) : "";
        ui.clueOpen = clueOpen && playing && clueQuiz.status == 0;
        ui.clueQuizOpen = clueOpen && playing && clueQuiz.status != 0;
        ui.clueQuizStatus = clueQuiz.status;
        ui.clueQuizRetrySeconds = clueQuiz.retrySeconds;
        ui.clueQuizSubmitting = clueQuizSubmitting;
        ui.clueQuizSubject = clueQuiz.subject;
        ui.clueQuizPrompt = clueQuiz.prompt;
        ui.clueQuizOptions = clueQuiz.options;
        ui.clueTargetActive = roomClueTarget.active;
        ui.clueStage = roomClueTarget.stage;
        ui.clueDestination = roomClueTarget.active
            ? clue::destinationName((clue::Destination)roomClueTarget.destination) : "";
        ui.clueReward = roomClueTarget.active && roomClueTarget.rewardItem != AIR
            ? blockOf(roomClueTarget.rewardItem).name : "";
        ui.cluePosition = {roomClueTarget.x, roomClueTarget.y, roomClueTarget.z};
        ui.clueBossRewardClaimed = roomClueTarget.bossRewardClaimed;
        ui.noteRitual = -1;
        ui.noteDone = ritualDone;
        ui.noteLineCount = 0;
        ui.noteTitle.clear();
        for (int i = 0; i < 3; i++) {
            ui.noteItems[i].clear();
            ui.noteHeld[i] = false;
            ui.notePlaced[i] = false;
            ui.noteItemId[i] = 0;
        }
        if (roomSession) {
            int rid = ritual::assignedRitual(gameClient.team());
            ui.noteRitual = rid;
            if (rid >= 0) {
                ui.noteTitle = ritual::ritualName(rid);
                int lines = ritual::storyLineCount(rid);
                if (lines > 8) lines = 8;
                ui.noteLineCount = lines;
                for (int i = 0; i < lines; i++) ui.noteLines[i] = ritual::storyLine(rid, i);
                int relics[3];
                ritual::recipeRelics(rid, relics);
                for (int i = 0; i < 3; i++) {
                    if (relics[i] < 0) continue;
                    ui.noteItems[i] = ritual::relicName(relics[i]);
                    ui.noteItemId[i] = (uint8_t)ritual::blockId(relics[i]);
                    ui.noteHeld[i] = false;
                    ui.notePlaced[i] = structure::offeringPlaced(world, rid, relics[i]);
                    for (int s = 0; s < cfg::INVENTORY_SLOTS; s++) {
                        if (!inv[s].empty() && inv[s].block == ui.noteItemId[i] && inv[s].count > 0)
                            ui.noteHeld[i] = true;
                    }
                }
            }
        }
        if (saveFlash > 0.0f) saveFlash -= dt;
        if (structureEdit && !structurePicker) {
            if (saveFlash > 0.0f) ui.goalText = structureNote;
            else if (ui.structureFile.empty()) ui.goalText = "E 打开菜单   左键拆除   右键放置";
            else ui.goalText = ui.structureFile + "    E 打开菜单   左键拆除   右键放置";
        } else {
            ui.goalText.clear();
        }
        ui.vitals = &player.vitals;
        ui.playerDead = player.dead;
        ui.hitMarker = hitMarker;
        ui.damageFlash = damageFlash;
        ui.playerStatus = roomSession ? roomPlayerStatus : 0;
        ui.arcaneProjectiles = roomSession ? arcaneProjectiles : std::vector<ArcaneProjectileView>{};
        ui.arcaneBursts = roomSession ? arcaneBursts : std::vector<ArcaneBurstView>{};
        if (roomSession && roomTickInit) {
            float extrapolate = std::chrono::duration<float>(std::chrono::steady_clock::now() - roomTickAt).count();
            extrapolate = clampf(extrapolate, 0.0f, 0.08f);
            for (ArcaneProjectileView& projectile : ui.arcaneProjectiles)
                projectile.pos += projectile.vel * extrapolate;
        }
        if (drag.active) { ui.held.block = drag.block; ui.held.count = (uint8_t)drag.count; }
        else ui.held.clear();
        ui.fps = fps;
        ui.loadedChunks = world.loadedChunks();
        if (roomSession) {
            float k = 1.0f - std::exp(-dt * 18.0f);
            roomCameraOffset = roomCameraOffset * (1.0f - k);
            if (roomCameraOffset.lengthSq() < 1.0e-5f) roomCameraOffset = { 0, 0, 0 };
        } else {
            roomCameraOffset = { 0, 0, 0 };
        }
        ui.cameraOffset = roomCameraOffset;
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
        ui.roomHost = roomHost;
        ui.roomSession = roomSession;
        ui.roomPort = roomPort;
        if (!ui.portFieldActive) ui.roomPortText = std::to_string(roomPort);
        ui.remotes = roomSession ? remotes : std::vector<RemoteAvatar>{};
        ui.netAnim = false;
        if (roomSession && ui.camMode != 0 && selfShownOk) {
            ui.netAnim = true;
            ui.netClip = selfShown.clip;
            ui.netStrike = selfShown.strike;
            ui.netFrame = selfShown.frame;
            ui.netStrikeFrame = selfShown.strikeFrame;
            ui.netYaw = selfShown.yaw;
            ui.netPitch = selfShown.pitch;
            ui.netBodyYaw = selfShown.bodyYaw;
        }
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
        exitTrial();
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
