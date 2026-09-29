// Standalone material/model editor. Separate entry point from the game:
// launches its own window, edits texture images and item models, saves them.
#include "../core/gl.hpp"
#include "../core/math.hpp"
#include "../render/textures.hpp"
#include "../render/player_draw.hpp"
#include "../world/blocks.hpp"
#include "../world/asset_pack.hpp"
#include "../world/player_model.hpp"
#include "../world/player_skin.hpp"
#include "../world/hair_voxels.hpp"
#include "../world/animation.hpp"
#include "../world/hold_bind.hpp"
#include "../material/registry.hpp"
#include "../material/block_preview.hpp"
#include "../render/block_geo.hpp"
#include "../plugin/plugin.hpp"
#include "../world/data_pack.hpp"
#include "data_editor.hpp"
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal Win32 + WGL 3.3 core context (compact version of the game's setup).
// ---------------------------------------------------------------------------
static HWND g_hwnd;
static HDC g_hdc;
static int g_winW = 1280, g_winH = 720;
static int g_wheel = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    if (m == WM_SIZE) { g_winW = LOWORD(l); g_winH = HIWORD(l); return 0; }
    if (m == WM_MOUSEWHEEL) { g_wheel += (int)(short)HIWORD(w); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void loadWglExtensions() {
    HINSTANCE inst = GetModuleHandle(nullptr);
    HWND hw = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hw) return;
    HDC dc = GetDC(hw);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 24; pfd.cDepthBits = 24; pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf) SetPixelFormat(dc, pf, &pfd);
    HGLRC rc = wglCreateContext(dc);
    if (rc) {
        wglMakeCurrent(dc, rc);
        gl::WglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
        gl::WglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)wglGetProcAddress("wglChoosePixelFormatARB");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
    }
    ReleaseDC(hw, dc);
    DestroyWindow(hw);
}

static bool createWindow() {
    loadWglExtensions();
    HINSTANCE inst = GetModuleHandle(nullptr);
    WNDCLASSW wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MatEditorWnd";
    if (!RegisterClassW(&wc)) return false;
    RECT rc = { 0, 0, g_winW, g_winH };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Material Editor", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;
    ShowWindow(g_hwnd, SW_SHOW);
    g_hdc = GetDC(g_hwnd);
    if (!gl::WglChoosePixelFormatARB || !gl::WglCreateContextAttribsARB) return false;
    const int attrs[] = { WGL_DRAW_TO_WINDOW_ARB,1, WGL_SUPPORT_OPENGL_ARB,1, WGL_DOUBLE_BUFFER_ARB,1,
                          WGL_PIXEL_TYPE_ARB,WGL_TYPE_RGBA_ARB, WGL_COLOR_BITS_ARB,24, WGL_DEPTH_BITS_ARB,24, 0 };
    int npf = 0; UINT nf = 0;
    if (!gl::WglChoosePixelFormatARB(g_hdc, attrs, nullptr, 1, &npf, &nf) || npf == 0) return false;
    PIXELFORMATDESCRIPTOR pfd = {};
    DescribePixelFormat(g_hdc, npf, sizeof(pfd), &pfd);
    SetPixelFormat(g_hdc, npf, &pfd);
    const int cattrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB,3, WGL_CONTEXT_MINOR_VERSION_ARB,3,
                           WGL_CONTEXT_PROFILE_MASK_ARB,WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                           WGL_CONTEXT_FLAGS_ARB,WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, 0 };
    HGLRC ctx = gl::WglCreateContextAttribsARB(g_hdc, nullptr, cattrs);
    if (!ctx) return false;
    wglMakeCurrent(g_hdc, ctx);
    if (!gl::loadAll()) return false; // load OpenGL 3.3 function pointers
    return true;
}

static unsigned compileShader(unsigned type, const char* src) {
    unsigned s = gl::CreateShader(type);
    gl::ShaderSource(s, 1, &src, nullptr);
    gl::CompileShader(s);
    int ok = 0; gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; gl::GetShaderInfoLog(s, sizeof(log), nullptr, log); fprintf(stderr, "[shader] %s\n", log); }
    return s;
}

// Guide cubes are centered on the stored points. The measure length is the span
// between their outer sides along the segment, not the gap between the inner sides.
static void guideOuterSpan(const float a[3], const float b[3], float h,
                           float oa[3], float ob[3], float& len) {
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    for (int k = 0; k < 3; k++) {
        float s = (d[k] > 1e-8f) ? 1.0f : (d[k] < -1e-8f) ? -1.0f : 0.0f;
        oa[k] = a[k] - s * h;
        ob[k] = b[k] + s * h;
    }
    float lx = ob[0] - oa[0], ly = ob[1] - oa[1], lz = ob[2] - oa[2];
    len = std::sqrt(lx * lx + ly * ly + lz * lz);
}

static unsigned linkProgram(const char* vs, const char* fs) {
    unsigned v = compileShader(GL_VERTEX_SHADER, vs);
    unsigned f = compileShader(GL_FRAGMENT_SHADER, fs);
    unsigned p = gl::CreateProgram();
    gl::AttachShader(p, v); gl::AttachShader(p, f); gl::LinkProgram(p);
    gl::DeleteShader(v); gl::DeleteShader(f);
    return p;
}

// ---------------------------------------------------------------------------
// Editor state
// ---------------------------------------------------------------------------
struct Color4 { uint8_t r = 0, g = 0, b = 0, a = 0; };

static const float kPaintPal[8][3] = {
    { 0, 1, 0 }, { 0.55f, 0.3f, 0.1f }, { 0.5f, 0.5f, 0.5f }, { 1, 1, 1 },
    { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0.3f, 1 }, { 1, 0.85f, 0 },
};

struct Editor {
    bool modelMode = false;          // false = texture painter, true = model editor
    bool entityMode = false;         // edit the entity (player) model parts
    bool animMode = false;           // edit reusable .animation clips
    int animView = 1;                // 0 skeleton only, 1 bound model + skeleton
    bool animPlaying = false;
    float animClock = 0.0f;          // fractional frame
    int animFrame = 0;
    int animSpanA = -1;             // unwrap range, -1 = unset
    int animSpanB = -1;
    int animSelBone = 0;
    int animClipHover = -1;
    int animBoneHover = -1;
    int animBindHover = -1;
    int animBtnHover = -1;           // transport / view buttons
    int animTool = 1;                // 0 move, 1 rotate, 2 select, 3 roll around bone axis
    bool animRollChain = true;      // roll: true = limb below follows, false = this bone only
    bool animHoldEdit = false;       // gizmos edit item-on-palm bind instead of bones
    bool animGrasp = false;          // gizmos edit the per-frame grasp point
    bool animLockDir = false;        // main-hand rotation keeps the tool's facing
    bool animLockPos = false;        // main-hand motion keeps the tool's grip in place
    bool animMask = false;           // paint a tint on cuboid faces
    int animMaskColor = 0;           // 0..5 shared mask color
    struct FaceMark { std::string part; int face = 0; int color = 0; };
    std::vector<FaceMark> faceMarks;
    bool animHoldDirty = false;
    uint8_t animHoldItem = HAND_AXE; // 0 = wildcard *
    int animHoldSide = 1;            // 1 right, -1 left
    int animHoldHover = -1;
    int animScrub = 0;               // 1 = dragging playhead
    bool animDirty = false;
    bool animModelDirty = false;    // bound .model rolled around a bone axis
    float animScrollL = 0.0f;        // left column content scroll
    float animScrollR = 0.0f;        // right column content scroll
    float entScroll = 0.0f;          // entity list preview column scroll
    float animBarZoom = 1.0f;        // timeline magnify; 1 = fit by icon width
    float animBarScroll = 0.0f;      // pixels into the frame axis
    float arotY = 0.55f, arotX = 0.22f, azoom = 3.4f;
    std::vector<pm::Part> entityParts;
    int entitySel = 0;
    std::string entityName = "player";
    int entityHover = -1;            // hovered entity list slot (-1 none, -2 add)
    int tile = TEX_GRASS_TUFT;
    int hoverX = -1, hoverY = -1;
    float tileX = 0, tileY = 0, tileSize = 0;
    float r = 0, g = 1, b = 0, a = 1;
    int paletteHover = -1;
    bool dirty = false;
    int selQuad = 0, selCorner = 0;
    std::string selMat = "grass_tuft"; // material assigned to new faces / picker selection
    int matHover = -1;               // hovered material thumb (-1 none, -2 = add extra)
    int modelTool = -1;              // -1 none, 0 move, 1 stretch, 2 fill, 3 rot45, 4 mirror, 5 uv, 6 tiny, 7 tile
    int modelToolHover = -1;         // hovered model tool button
    int gizmoHover = 0;              // 0 none; see gizmo ids in the model editor
    bool refTool = false;            // sticky; stays on when other tools are selected
    bool hairPaint = false;          // sticky voxel hair brush; LMB paint
    int paintDiv = 0;                // paint cell: 0..3 = 1, 1/2, 1/4, 1/16 of the grid
    bool hairErase = false;          // sticky voxel hair eraser; LMB carve
    bool hairSelect = false;         // sticky voxel/part hair select
    bool hairCard = false;           // sticky 1/4-voxel textured hair-card brush
    bool partCut = false;            // sticky: click removes one grid cell from a solid cuboid
    bool partAdd = false;            // cloth: two helper cubes, their bounds become one part
    bool partMeasure = false;        // cloth: two helper cubes define a guide edge
    bool measureOn = false;
    float measureA[3] = {}, measureB[3] = {};
    bool partPlane = false;          // paint a mask onto one face
    bool planeOn = false;
    bool planeBody = false;          // mask is on the mannequin, not a garment part
    int planePart = -1;
    int planeFace = 0;               // 0+X 1-X 2+Y 3-Y 4+Z 5-Z, the face the mask is bound to
    bool partAddVis = false;
    bool partAnchor = false;
    float partA[3] = {}, partB[3] = {};
    float partMn[3] = {}, partMx[3] = {};
    bool cutVis = false;
    float cutMn[3] = {}, cutMx[3] = {};
    std::string hairCardTex;          // empty = solid color fill; else material name
    bool hairSelDrag = false;
    bool hairSelRmb = false;
    float hairSelX0 = 0, hairSelY0 = 0, hairSelX1 = 0, hairSelY1 = 0;
    std::vector<pm::HairAtom> hairVoxSel;
    std::vector<pm::HairAtom> hairCardSel;
    int refDecalIdx = -1;            // entity ref-line source decal; -1 = none
    int refEdge = -1;                // locked edge 0..3; -1 = show all four extensions
    int refLineHover = -1;           // hovered extension edge while picking (-1 none)
    int rotAxis = 1;                 // 0 X, 1 Y, 2 Z (for arrow-key rotate)
    int modelBlock = GRASS_TUFT;     // block whose .model file is being edited
    int selSolid = -1;               // selected colored cuboid in the item model, or -1
    int modelRightTab = 0;           // item editor right pane: 0 rand preview, 1 color paint
    int itemPaintTool = 0;           // color tab: 0 pencil, 1 eraser, 2 fill, 3 picker
    int modelHover = -1;             // hovered block in the model selection panel (-1 = none)
    float rotY = 0.5f, rotX = 0.3f, zoom = 2.6f;
    float brotY = 0.0f, brotX = 1.1f, bzoom = 2.9f; // block-model camera (orbit, pitch ±almost-90°)
    float rrotY = 0.5f, rrotX = 0.55f, rzoom = 2.4f; // rand-preview camera (right pane)
    float erotY = 0.55f, erotX = 0.22f, ezoom = 3.4f; // entity overview camera (right pane)
    float entPivotY = 0.90f;         // overview orbit pivot height (world Y, 0..1.8)
    float entPanX = 0.0f, entPanZ = 0.0f; // right-pane orbit center offset on the ground plane
    bool entSkinView = true;         // center pane: false = 3D part, true = planar skin / overlay
    bool clothLocal = true;          // cloth left pane: selected clump, else the whole garment
    bool clothBody = true;           // cloth right pane: draw the body mannequin
    int randWx = 0, randWz = 0;                      // noise seed for apply-rand preview
    int tool = -1;                   // -1 none, 0 paintbrush, 1 brush, 2 bucket, 4 eraser, 5 rectbucket, 6 rect, 7 line, 8 circle
    bool picker = false;             // temporary eyedropper (keeps base tool highlighted)
    bool justPicked = false;         // suppress paint/fill until the mouse is released after picking
    bool brushMod = false;           // brush modifier stacked on bucket (fill uses random colors)
    bool dragging = false;           // shape drag in progress
    int dragX0 = 0, dragY0 = 0;      // shape drag start
    int dragX1 = 0, dragY1 = 0;      // shape drag end (updated while dragging)
    int brushSize = 1;               // 1..4 (picker capped at 4x4)
    int hoverTile = -1;              // hovered tile thumbnail in the selection panel
    int toolHover = -1;              // hovered toolbar button (-1 = none)
    float tbRect[13][4] = {};        // toolbar button rects {x,y,w,h}
    float svRect[4] = {};            // SV (saturation/value) rectangle {x,y,w,h}
    float hueCX = 0, hueCY = 0, hueRO = 0, hueRI = 0; // hue ring center + radii
    std::vector<Color4> brushColors; // multi-color brush palette (16 slots, unique RGBA)
    std::vector<uint8_t> brushSet;   // whether each brush block has been assigned a color
    std::vector<uint8_t> brushSel;   // selected slots in the brush matrix
    std::vector<Color4> recent;      // recent colors, FIFO (<=8)
    float canvasZoom = 1.0f;         // canvas display zoom (image stays 64x64 on save)
    float skinViewZoom = 1.0f;       // 1 = fit whole sheet in the center pane
    float skinPanX = 0.0f, skinPanY = 0.0f;
    bool pickAlpha0 = false;         // allow fully-transparent (a=0) colors into the brush matrix
    float alphaRect[4] = {};         // alpha slider rect {x,y,w,h}
    float brushMat[4] = {};          // brush matrix rect {x,y,w,h}
    std::vector<std::vector<uint8_t>> undoStack;
};

static bool editorFocused() {
    return g_hwnd && GetForegroundWindow() == g_hwnd;
}
static bool keyDown(int vk) {
    if (!editorFocused()) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static Mat4 orbitMvp(float aspect, float yaw, float pitch, float zoom, float px, float py, float pz) {
    if (aspect < 0.05f) aspect = 0.05f;
    Mat4 proj = Mat4::perspective(45.0f, aspect, 0.05f, 100.0f);
    float eyex = std::sin(yaw) * std::cos(pitch) * zoom + px;
    float eyey = std::sin(pitch) * zoom + py;
    float eyez = std::cos(yaw) * std::cos(pitch) * zoom + pz;
    return proj * Mat4::lookAt({eyex, eyey, eyez}, {px, py, pz}, {0, 1, 0});
}

static Vec3 viewOfModel(const float p[3]) { return { p[0] - 0.5f, p[1], p[2] - 0.5f }; }

static float& vecComp(Vec3& v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }
static float vecCompC(const Vec3& v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }

// Paint / part cell stops: full grid, half, quarter, sixteenth.
static int paintDenom(int step) {
    static const int d[4] = { 1, 2, 4, 16 };
    if (step < 0) step = 0;
    if (step > 3) step = 3;
    return d[step];
}

// Texels covered by one paint cell on a face. worldLen is that edge in world units, pxLen is its texel span.
static int paintCellPx(float worldLen, int pxLen, int step) {
    float cell = 0.04f / (float)paintDenom(step);
    if (!(worldLen > 1e-5f) || pxLen < 1) return 1;
    int n = (int)std::lround(cell * (float)pxLen / worldLen);
    if (n < 1) n = 1;
    if (n > pxLen) n = pxLen;
    return n;
}

static bool rayHitAABB(const Vec3& ro, const Vec3& rd, const Vec3& mn, const Vec3& mx, float& tHit) {
    float tmin = 0.001f, tmax = 1e9f;
    for (int i = 0; i < 3; i++) {
        float o = vecCompC(ro, i), d = vecCompC(rd, i), a = vecCompC(mn, i), b = vecCompC(mx, i);
        if (std::fabs(d) < 1e-8f) {
            if (o < a || o > b) return false;
        } else {
            float t0 = (a - o) / d, t1 = (b - o) / d;
            if (t0 > t1) std::swap(t0, t1);
            if (t0 > tmin) tmin = t0;
            if (t1 < tmax) tmax = t1;
            if (tmin > tmax) return false;
        }
    }
    tHit = tmin;
    return true;
}

static bool rayHitTri(const Vec3& ro, const Vec3& rd, const Vec3& a, const Vec3& b, const Vec3& c, float& t) {
    Vec3 e1 = b - a, e2 = c - a;
    Vec3 pvec = rd.cross(e2);
    float det = e1.dot(pvec);
    if (std::fabs(det) < 1e-8f) return false;
    float inv = 1.0f / det;
    Vec3 tvec = ro - a;
    float u = tvec.dot(pvec) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    Vec3 qvec = tvec.cross(e1);
    float v = rd.dot(qvec) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = e2.dot(qvec) * inv;
    return t > 0.001f;
}

static float distPointSeg2(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;
    float d = vx * vx + vy * vy;
    float t = (d > 1e-8f) ? (wx * vx + wy * vy) / d : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float dx = ax + vx * t - px, dy = ay + vy * t - py;
    return std::sqrt(dx * dx + dy * dy);
}

static void decalEdgeExtended(const Vec3 q[4], int edge, float extend, Vec3& outA, Vec3& outB) {
    const Vec3& c0 = q[edge];
    const Vec3& c1 = q[(edge + 1) % 4];
    Vec3 d = c1 - c0;
    float elen = d.length();
    if (elen < 1e-8f) { outA = c0; outB = c1; return; }
    d = d / elen;
    Vec3 mid = (c0 + c1) * 0.5f;
    float half = elen * 0.5f + extend;
    outA = mid - d * half;
    outB = mid + d * half;
}

static Vec3 closestOnInfiniteLine(const Vec3& a, const Vec3& u, const Vec3& p) {
    return a + u * u.dot(p - a);
}

static int pickDecalRay(const std::vector<pm::Part>& parts, const Vec3& ro, const Vec3& rd) {
    float best = 1e9f;
    int hit = -1;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (!pm::isDecalPart(parts[i])) continue;
        Vec3 q[4];
        pm::texQuadLocal(parts[i], q);
        float t;
        if (rayHitTri(ro, rd, q[0], q[1], q[2], t) && t < best) { best = t; hit = i; }
        if (rayHitTri(ro, rd, q[0], q[2], q[3], t) && t < best) { best = t; hit = i; }
    }
    return hit;
}

// Closest parameter t on line p+t*u (u unit) to ray ro+s*rd.
static float closestAxisT(const Vec3& p, const Vec3& u, const Vec3& ro, const Vec3& rd) {
    Vec3 r = p - ro;
    float a = 1.0f, b = u.dot(rd), c = rd.dot(rd);
    float d = u.dot(r), e = rd.dot(r);
    float den = a * c - b * b;
    if (std::fabs(den) < 1e-8f) return 0.0f;
    return (b * e - c * d) / den;
}

// Flood fill (paint bucket) over a connected region of equal color.
struct PxClip { bool on = false; int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
static PxClip gPxClip;
static bool pxInClip(int x, int y) {
    return !gPxClip.on || (x >= gPxClip.x0 && y >= gPxClip.y0 && x < gPxClip.x1 && y < gPxClip.y1);
}
static void floodFill(mat::Image& img, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int W = img.w, H = img.h;
    if (x < 0 || x >= W || y < 0 || y >= H || !pxInClip(x, y)) return;
    size_t si = ((size_t)y * W + x) * 4;
    uint8_t tr = img.rgba[si + 0], tg = img.rgba[si + 1], tb = img.rgba[si + 2], ta = img.rgba[si + 3];
    if (tr == r && tg == g && tb == b && ta == a) return;
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(x, y);
    while (!stack.empty()) {
        auto [px, py] = stack.back(); stack.pop_back();
        if (px < 0 || px >= W || py < 0 || py >= H || !pxInClip(px, py)) continue;
        size_t i = ((size_t)py * W + px) * 4;
        if (img.rgba[i + 0] != tr || img.rgba[i + 1] != tg || img.rgba[i + 2] != tb || img.rgba[i + 3] != ta) continue;
        img.rgba[i + 0] = r; img.rgba[i + 1] = g; img.rgba[i + 2] = b; img.rgba[i + 3] = a;
        stack.emplace_back(px + 1, py); stack.emplace_back(px - 1, py);
        stack.emplace_back(px, py + 1); stack.emplace_back(px, py - 1);
    }
}

// HSV (h,s,v in [0,1]) -> RGB.
// Flood fill that assigns a random color (from a palette) to each filled pixel.
static void floodFillRandom(mat::Image& img, int x, int y, const std::vector<Color4>& colors) {
    int W = img.w, H = img.h;
    if (x < 0 || x >= W || y < 0 || y >= H || !pxInClip(x, y) || colors.empty()) return;
    size_t si = ((size_t)y * W + x) * 4;
    uint8_t tr = img.rgba[si + 0], tg = img.rgba[si + 1], tb = img.rgba[si + 2], ta = img.rgba[si + 3];
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(x, y);
    while (!stack.empty()) {
        auto [px, py] = stack.back(); stack.pop_back();
        if (px < 0 || px >= W || py < 0 || py >= H || !pxInClip(px, py)) continue;
        size_t i = ((size_t)py * W + px) * 4;
        if (img.rgba[i + 0] != tr || img.rgba[i + 1] != tg || img.rgba[i + 2] != tb || img.rgba[i + 3] != ta) continue;
        Color4 c = colors[rand() % colors.size()];
        img.rgba[i + 0] = c.r; img.rgba[i + 1] = c.g; img.rgba[i + 2] = c.b; img.rgba[i + 3] = c.a;
        stack.emplace_back(px + 1, py); stack.emplace_back(px - 1, py);
        stack.emplace_back(px, py + 1); stack.emplace_back(px, py - 1);
    }
}

static void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    h = h - std::floor(h);
    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r1, g1, b1;
    if (h < 1.0f / 6.0f)      { r1 = c; g1 = x; b1 = 0; }
    else if (h < 2.0f / 6.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 3.0f / 6.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 4.0f / 6.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 5.0f / 6.0f) { r1 = x; g1 = 0; b1 = c; }
    else                      { r1 = c; g1 = 0; b1 = x; }
    r = r1 + m; g = g1 + m; b = b1 + m;
}

// RGB -> HSV (h,s,v in [0,1]).
static void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    v = mx;
    float d = mx - mn;
    s = (mx < 1e-6f) ? 0.0f : d / mx;
    if (d < 1e-6f) { h = 0.0f; return; }
    if (mx == r)      h = std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else              h = (r - g) / d + 4.0f;
    h /= 6.0f;
    if (h < 0) h += 1.0f;
}

// Generate a 32x32 RGBA tool-icon image (used to bootstrap PNG icon files).
static void genToolIcon(int icon, std::vector<uint8_t>& out) {    const int S = 32;
    out.assign((size_t)S * S * 4, 0);
    auto px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= S || y < 0 || y >= S) return;
        size_t i = ((size_t)y * S + x) * 4;
        out[i + 0] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = 255;
    };
    auto rect = [&](int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) px(x, y, r, g, b);
    };
    auto pat = [&](const char* const rows[8], uint8_t r, uint8_t g, uint8_t b) {
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
            if (rows[y][x] == 'Q') rect(x * 4, y * 4, x * 4 + 3, y * 4 + 3, r, g, b);
    };
    switch (icon) {
        case 0: { static const char* const P[8] = {
            "OOOOOOOO","OOOOOQQO","OOOOQQQO","OOOQQQOO",
            "OOOQQOOO","OQQOOOOO","OQQOOOOO","OOOOOOOO" }; pat(P, 255, 255, 255); break; }
        case 1: {
            rect(4, 4, 13, 13, 255, 0, 0);     // red
            rect(18, 4, 27, 13, 255, 128, 0);  // orange
            rect(4, 18, 13, 27, 140, 77, 26);  // brown
            rect(18, 18, 27, 27, 255, 217, 0); // yellow
            break;
        }
        case 2: {
            rect(8, 14, 23, 25, 255, 255, 255);
            rect(12, 10, 19, 11, 255, 255, 255);
            rect(10, 6, 11, 10, 255, 255, 255);
            rect(20, 6, 21, 10, 255, 255, 255);
            break;
        }
        case 3: {
            for (int y = 6; y <= 15; y++) { int half = (y - 6) * 5 / 9; rect(16 - half, y, 16 + half, y, 255, 255, 255); }
            rect(15, 15, 16, 25, 255, 255, 255);
            break;
        }
        case 4: {
            rect(8, 12, 22, 24, 255, 255, 255);
            rect(5, 9, 19, 21, 255, 255, 255);
            rect(2, 6, 16, 18, 255, 255, 255);
            break;
        }
        case 5: { // rect bucket (filled rectangle + inner indicator)
            rect(7, 8, 24, 23, 255, 255, 255);
            rect(12, 13, 19, 18, 60, 60, 60);
            break;
        }
        case 6: { // rectangle outline
            rect(7, 8, 24, 9, 255, 255, 255);
            rect(7, 22, 24, 23, 255, 255, 255);
            rect(7, 9, 8, 22, 255, 255, 255);
            rect(23, 9, 24, 22, 255, 255, 255);
            break;
        }
        case 7: { // line (diagonal)
            for (int i = 0; i < 24; i++) { px(5 + i, 6 + i, 255, 255, 255); px(6 + i, 6 + i, 255, 255, 255); }
            break;
        }
        case 8: { // circle outline
            int ccx = 15, ccy = 15, r = 11, x = r, y = 0;
            while (x >= y) {
                px(ccx + x, ccy + y, 255, 255, 255); px(ccx - x, ccy + y, 255, 255, 255);
                px(ccx + x, ccy - y, 255, 255, 255); px(ccx - x, ccy - y, 255, 255, 255);
                px(ccx + y, ccy + x, 255, 255, 255); px(ccx - y, ccy + x, 255, 255, 255);
                px(ccx + y, ccy - x, 255, 255, 255); px(ccx - y, ccy - x, 255, 255, 255);
                y++;
                if (x * x + y * y > r * r) x--;
            }
            break;
        }
        case 9: { static const char* const U[8] = {
            "OOQQQQQO","OOOOOOQO","OOOOOOQO","OOOOOOQO",
            "OOOOOOQO","QQOOOOQO","OOQQQQQO","OOOOOOOO" }; pat(U, 255, 255, 255); break; }
        case 10: rect(6, 15, 25, 16, 255, 255, 255); break;
        case 11: rect(6, 15, 25, 16, 255, 255, 255); rect(15, 6, 16, 25, 255, 255, 255); break;
        case 12: { static const char* const S2[8] = {
            "OOOOOOOO","OQQQQQOO","OQQQQQQO","OQQQQQQO",
            "OQOOOOQO","OQOOOOQO","OQQQQQQO","OOOOOOOO" }; pat(S2, 255, 255, 255); break; }
    }
}

// Slider specs for grass-tuft .rand (order is the on-screen layout, two columns).
struct RandSlider {
    const char* key;
    const char* label;
    float minv, maxv, def;
    bool integer;
};
static const RandSlider kRandSliders[] = {
    { "R",            "Grid R",      2.0f, 32.0f, 16.0f,   true  },
    { "wb_factor",    "Width",       0.05f, 2.0f,  0.5f,   false },
    { "tall_base",    "Tall base",   0.0f,  1.0f,  0.375f, false },
    { "tall_range",   "Tall range",  0.0f,  1.0f,  0.375f, false },
    { "short_base",   "Short base",  0.0f,  1.0f,  0.1875f,false },
    { "short_range",  "Short range", 0.0f,  1.0f,  0.1875f,false },
    { "density_mean", "Density",     0.0f,  1.0f,  0.60f,  false },
    { "density_std",  "Dens std",    0.0f,  0.5f,  0.08f,  false },
    { "density_min",  "Dens min",    0.0f,  1.0f,  0.50f,  false },
    { "density_max",  "Dens max",    0.0f,  1.0f,  0.70f,  false },
    { "cluster_freq", "Cluster",     0.0f,  1.0f,  0.22f,  false },
    { "shade_min",    "Shade min",   0.0f,  2.0f,  0.85f,  false },
    { "shade_max",    "Shade max",   0.0f,  2.0f,  1.15f,  false },
};
static const int kNRandSliders = (int)(sizeof(kRandSliders) / sizeof(kRandSliders[0]));

// Shape drawing helpers (pixel-level, used by the shape tools).
static void fillRectImg(mat::Image& img, int x0, int y0, int x1, int y1, Color4 c) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= img.w || y < 0 || y >= img.h || !pxInClip(x, y)) continue;
        size_t i = ((size_t)y * img.w + x) * 4;
        img.rgba[i + 0] = c.r; img.rgba[i + 1] = c.g; img.rgba[i + 2] = c.b; img.rgba[i + 3] = c.a;
    }
}
static void drawLineImg(mat::Image& img, int x0, int y0, int x1, int y1, Color4 c, int thick) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    auto put = [&](int x, int y) {
        for (int ty = -thick / 2; ty <= thick / 2; ty++) for (int tx = -thick / 2; tx <= thick / 2; tx++) {
            int xx = x + tx, yy = y + ty;
            if (xx < 0 || xx >= img.w || yy < 0 || yy >= img.h || !pxInClip(xx, yy)) continue;
            size_t i = ((size_t)yy * img.w + xx) * 4;
            img.rgba[i + 0] = c.r; img.rgba[i + 1] = c.g; img.rgba[i + 2] = c.b; img.rgba[i + 3] = c.a;
        }
    };
    while (true) {
        put(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void drawCircleImg(mat::Image& img, int cx, int cy, int r, Color4 c) {
    int x = r, y = 0;
    while (x >= y) {
        auto put8 = [&](int px, int py) {
            int P[8][2] = { {px,py},{-px,py},{px,-py},{-px,-py},{py,px},{-py,px},{py,-px},{-py,-px} };
            for (auto& e : P) {
                int xx = cx + e[0], yy = cy + e[1];
                if (xx < 0 || xx >= img.w || yy < 0 || yy >= img.h || !pxInClip(xx, yy)) continue;
                size_t i = ((size_t)yy * img.w + xx) * 4;
                img.rgba[i + 0] = c.r; img.rgba[i + 1] = c.g; img.rgba[i + 2] = c.b; img.rgba[i + 3] = c.a;
            }
        };
        put8(x, y);
        y++;
        if (x * x + y * y > r * r) x--;
    }
}

// Editor mode entry. editor.exe accepts --texture / --item / --model to skip
// the chooser screen; without an argument it shows the mode chooser.
static void launchStructureEditor() {
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring path(self);
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    std::wstring exe = dir + L"\\voxel-legend.exe";
    std::wstring cmd = L"\"" + exe + L"\" --edit-structure";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(nullptr, L"找不到 voxel-legend.exe，请先编译游戏。", L"Structure Editor", MB_ICONWARNING);
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    int startMode = -1; // -1 = chooser screen, 0 = texture, 1 = item model, 2 = entity model, 3 = animation
    if (lpCmdLine && *lpCmdLine) {
        std::string cl(lpCmdLine);
        for (char& c : cl) if (c >= 'A' && c <= 'Z') c += 32;
        if (cl.find("--data") != std::string::npos || cl.find("--datapack") != std::string::npos) startMode = 4;
        else if (cl.find("--animation") != std::string::npos || cl.find("--anim") != std::string::npos) startMode = 3;
        else if (cl.find("--model") != std::string::npos) startMode = 2;
        else if (cl.find("--item") != std::string::npos || cl.find("--block") != std::string::npos) startMode = 1;
        else if (cl.find("--texture") != std::string::npos) startMode = 0;
    }
    const char* kAppName = "Editor";

    if (startMode == 4) {
        plugin::init();
        data::init();
        dataed::runModal(nullptr);
        return 0;
    }

    if (!createWindow()) {
        MessageBoxW(nullptr, L"Failed to initialize an OpenGL 3.3 window", L"Editor", MB_ICONERROR);
        return 1;
    }
    plugin::init();
    data::init();
    mat::initMaterials();

    const char* vs = R"(#version 330 core
        layout(location=0) in vec3 aPos; layout(location=1) in vec4 aColor;
        uniform mat4 uMVP; out vec4 vColor;
        void main(){ gl_Position = uMVP * vec4(aPos,1.0); vColor = aColor; })";
    const char* fs = R"(#version 330 core
        in vec4 vColor; out vec4 fragColor;
        void main(){ fragColor = vColor; })";
    unsigned prog = linkProgram(vs, fs);
    int uMVP = gl::GetUniformLocation(prog, "uMVP");

    unsigned vao = 0, vbo = 0;
    gl::GenVertexArrays(1, &vao);
    gl::GenBuffers(1, &vbo);
    gl::BindVertexArray(vao);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    gl::EnableVertexAttribArray(1);

    // Textured shader + VAO for tooltip text.
    const char* tvs = R"(#version 330 core
        layout(location=0) in vec3 aPos; layout(location=1) in vec4 aColor; layout(location=2) in vec2 aUV;
        uniform mat4 uMVP; out vec4 vColor; out vec2 vUV;
        void main(){ gl_Position = uMVP * vec4(aPos,1.0); vColor = aColor; vUV = aUV; })";
    const char* tfs = R"(#version 330 core
        in vec4 vColor; in vec2 vUV; uniform sampler2D uTex; out vec4 fragColor;
        void main(){ fragColor = vColor * texture(uTex, vUV); })";
    unsigned tprog = linkProgram(tvs, tfs);
    int tuMVP = gl::GetUniformLocation(tprog, "uMVP");

    // Cutout textured shader (alpha-discard) for the 3D block preview — matches
    // the game's opaque pass so grass blades / cutout textures render correctly.
    const char* cfs = R"(#version 330 core
        in vec4 vColor; in vec2 vUV; uniform sampler2D uTex; out vec4 fragColor;
        void main(){ vec4 t = texture(uTex, vUV); if (t.a < 0.1) discard; fragColor = vColor * t; })";
    unsigned cprog = linkProgram(tvs, cfs);
    int cuMVP = gl::GetUniformLocation(cprog, "uMVP");
    unsigned tvao = 0, tvbo = 0;
    gl::GenVertexArrays(1, &tvao);
    gl::GenBuffers(1, &tvbo);
    gl::BindVertexArray(tvao);
    gl::BindBuffer(GL_ARRAY_BUFFER, tvbo);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)0);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(3 * sizeof(float)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(7 * sizeof(float)));
    gl::EnableVertexAttribArray(2);
    gl::BindVertexArray(0);

    struct TextTex { unsigned tex = 0; int w = 0, h = 0; };
    std::map<std::string, TextTex> textCache;
    auto getTextTex = [&](const std::string& s) -> TextTex& {
        auto it = textCache.find(s);
        if (it != textCache.end()) return it->second;
        TextTex tt;
        HDC dc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(dc);
        static HFONT bigFont = CreateFontA(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HGDIOBJ of0 = SelectObject(mem, bigFont);
        RECT m = { 0, 0, 0, 0 };
        DrawTextA(mem, s.c_str(), -1, &m, DT_CALCRECT | DT_SINGLELINE);
        tt.w = m.right - m.left; tt.h = m.bottom - m.top;
        if (tt.w > 0 && tt.h > 0) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = tt.w; bi.bmiHeader.biHeight = -tt.h;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
            std::memset(bits, 0, (size_t)tt.w * tt.h * 4);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(255, 255, 255));
            RECT r = { 0, 0, tt.w, tt.h };
            DrawTextA(mem, s.c_str(), -1, &r, DT_SINGLELINE | DT_NOCLIP);
            for (int p = 0; p < tt.w * tt.h; p++) {
                ((uint8_t*)bits)[p * 4 + 3] = ((uint8_t*)bits)[p * 4 + 2]; // alpha = R (white text)
            }
            gl::GenTextures(1, &tt.tex);
            gl::BindTexture(GL_TEXTURE_2D, tt.tex);
            gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tt.w, tt.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, bits);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
        }
        SelectObject(mem, of0);
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
        textCache[s] = tt;
        return textCache[s];
    };

    // Load/generate tool-icon PNG files and upload them as textures.
    std::filesystem::create_directories("assets/editor_icons");
    std::vector<unsigned> iconTex(13, 0);
    for (int k = 0; k < 13; k++) {
        std::string p = "assets/editor_icons/icon_" + std::to_string(k) + ".png";
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::vector<uint8_t> rgba;
            genToolIcon(k, rgba);
            mat::savePNG(p.c_str(), 32, 32, rgba.data());
        }
        mat::Image img = mat::loadPNG(p.c_str());
        if (img.ok()) {
            gl::GenTextures(1, &iconTex[k]);
            gl::BindTexture(GL_TEXTURE_2D, iconTex[k]);
            gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl::BindTexture(GL_TEXTURE_2D, 0);
        }
    }

    // Build + upload the game atlas (procedural base with edited tiles overlaid),
    // exactly as the game renders blocks. NEAREST filtering keeps it pixel-consistent.
    std::vector<uint8_t> atlas;
    tex::generateAtlas(atlas);
    for (int t = 0; t < TEX_COUNT; t++) {
        if (mat::g_tileImages[t].ok()) tex::overwriteTile(t, mat::g_tileImages[t].rgba.data(), atlas);
    }
    unsigned atlasTex = 0;
    gl::GenTextures(1, &atlasTex);
    gl::BindTexture(GL_TEXTURE_2D, atlasTex);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex::ATLAS_W, tex::ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::BindTexture(GL_TEXTURE_2D, 0);

    std::vector<unsigned> tileGL(TEX_COUNT, 0);
    auto uploadImgTex = [&](const mat::Image& img) -> unsigned {
        if (!img.ok()) return 0;
        unsigned t = 0;
        gl::GenTextures(1, &t);
        gl::BindTexture(GL_TEXTURE_2D, t);
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        gl::BindTexture(GL_TEXTURE_2D, 0);
        return t;
    };
    for (int t = 0; t < TEX_COUNT; t++) tileGL[t] = uploadImgTex(mat::g_tileImages[t]);

    unsigned leafGL = 0, skinGL = 0, eyeGL = 0, eyelidGL = 0, mouthGL[3] = {};
    std::map<std::string, unsigned> overlayGL;
    auto clampTex = [&](unsigned t) {
        if (!t) return;
        gl::BindTexture(GL_TEXTURE_2D, t);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl::BindTexture(GL_TEXTURE_2D, 0);
    };
    auto uploadGen = [&](const char* path, const std::vector<uint8_t>& rgba, int w, int h) -> unsigned {
        std::filesystem::create_directories(pack::entitiesDir());
        if (!pm::fileExists(path)) mat::savePNG(path, w, h, rgba.data());
        mat::Image img = mat::loadPNG(path);
        if (!img.ok()) {
            img.w = w;
            img.h = h;
            img.rgba = rgba;
        }
        unsigned t = uploadImgTex(img);
        clampTex(t);
        return t;
    };
    auto bindOverlayGL = [&](const std::string& name, unsigned t) {
        if (!name.empty() && t) overlayGL[name] = t;
    };
    {
        pm::EntityFile boot = pm::buildPlayerEntity();
        std::string skinStem = boot.skin.empty() ? "player" : boot.skin;
        std::vector<uint8_t> rgba;
        tex::generatePlayerSkin(rgba);
        std::string skinP = pack::entityPng(skinStem);
        skinGL = uploadGen(skinP.c_str(), rgba, pm::kSkinW, pm::kSkinH);
        tex::generatePlayerEye(rgba);
        std::string eyeP = pack::entityPng("eye");
        eyeGL = uploadGen(eyeP.c_str(), rgba, pm::kEyeSize, pm::kEyeSize);
        bindOverlayGL("eye", eyeGL);
        tex::generatePlayerEyelid(rgba);
        std::string lidP = pack::entityPng("eyelid");
        eyelidGL = uploadGen(lidP.c_str(), rgba, pm::kEyelidSize, pm::kEyelidSize);
        bindOverlayGL("eyelid", eyelidGL);
        for (int i = 0; i < pm::kMouthVariantCount; i++) {
            tex::generatePlayerMouth(rgba, i);
            const char* nm = (i < (int)boot.mouthTex.size()) ? boot.mouthTex[i].c_str() : pm::kMouthNames[i];
            std::string mp = pack::entityPng(nm);
            mouthGL[i] = uploadGen(mp.c_str(), rgba, pm::kMouthW, pm::kMouthH);
            bindOverlayGL(nm, mouthGL[i]);
        }
        std::string leafP = pack::resolvePng("leaf");
        mat::Image leafImg = mat::loadPNG(leafP.c_str());
        if (leafImg.ok()) {
            leafGL = uploadImgTex(leafImg);
            clampTex(leafGL);
            bindOverlayGL("leaf", leafGL);
        }
        for (const pm::OverlaySpec& o : boot.overlays) {
            if (overlayGL.count(o.name)) continue;
            std::string p = pack::resolvePng(o.name);
            mat::Image img = mat::loadPNG(p.c_str());
            if (!img.ok()) continue;
            unsigned t = uploadImgTex(img);
            clampTex(t);
            bindOverlayGL(o.name, t);
        }
    }
    auto loadOverlayImg = [&](const char* path, int w, int h) -> mat::Image {
        mat::Image img = mat::loadPNG(path);
        if (!img.ok() || img.w != w || img.h != h) {
            img.w = w;
            img.h = h;
            img.rgba.assign((size_t)w * h * 4, 0);
        }
        return img;
    };
    std::string eyePath = pack::entityPng("eye");
    std::string lidPath = pack::entityPng("eyelid");
    mat::Image eyeImg = loadOverlayImg(eyePath.c_str(), pm::kEyeSize, pm::kEyeSize);
    mat::Image eyelidImg = loadOverlayImg(lidPath.c_str(), pm::kEyelidSize, pm::kEyelidSize);
    mat::Image mouthImgs[3];
    for (int i = 0; i < pm::kMouthVariantCount; i++) {
        std::string mp = pack::entityPng(pm::kMouthNames[i]);
        mouthImgs[i] = loadOverlayImg(mp.c_str(), pm::kMouthW, pm::kMouthH);
    }
    mat::Image savedEye = eyeImg, savedEyelid = eyelidImg;
    mat::Image savedMouth[3] = { mouthImgs[0], mouthImgs[1], mouthImgs[2] };
    std::vector<std::vector<uint8_t>> eyeUndo, eyelidUndo, mouthUndo[3];
    std::string playerSkinP = pack::entityPng("player");
    mat::Image skinImg = mat::loadPNG(playerSkinP.c_str());
    if (!skinImg.ok()) {
        std::vector<uint8_t> rgba;
        tex::generatePlayerSkin(rgba);
        skinImg.w = pm::kSkinW;
        skinImg.h = pm::kSkinH;
        skinImg.rgba = std::move(rgba);
    }
    mat::Image savedSkin = skinImg;
    unsigned bodySkinGL = uploadImgTex(skinImg);
    clampTex(bodySkinGL);
    std::vector<std::vector<uint8_t>> skinUndo;
    std::string entityPath = pack::entityModel("player");
    std::string entitySkinPath = pack::entityPng("player");
    std::vector<std::string> entNames;
    std::vector<std::string> garmentNames;
    bool entIsCloth = false;
    int entSheetW = 0, entSheetH = 0;
    std::map<std::string, unsigned> clothSheetGL;
    std::vector<std::string> entMouthNames = pm::defaultMouthTex();
    std::vector<std::string> entFaceNames = pm::defaultFaceNames();
    std::vector<pm::OverlaySpec> entOverlays = pm::defaultOverlays();
    std::string entSkin = "player";
    std::vector<std::string> entExtraLines;

    auto uploadImgPixels = [&](unsigned tex, const mat::Image& img) {
        if (!tex || !img.ok()) return;
        gl::BindTexture(GL_TEXTURE_2D, tex);
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        gl::BindTexture(GL_TEXTURE_2D, 0);
    };
    auto syncTilesToGL = [&]() {
        for (int t = 0; t < TEX_COUNT; t++) {
            const mat::Image& img = mat::g_tileImages[t];
            if (!img.ok()) continue;
            if (!tileGL[t]) tileGL[t] = uploadImgTex(img);
            else uploadImgPixels(tileGL[t], img);
            tex::overwriteTile(t, img.rgba.data(), atlas);
        }
        if (atlasTex && (int)atlas.size() == tex::ATLAS_W * tex::ATLAS_H * 4) {
            gl::BindTexture(GL_TEXTURE_2D, atlasTex);
            gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex::ATLAS_W, tex::ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas.data());
            gl::BindTexture(GL_TEXTURE_2D, 0);
        }
        if (mat::g_tileImages[TEX_GRASS_TUFT].ok())
            mat::g_grassTuft.image = mat::g_tileImages[TEX_GRASS_TUFT];
    };

    struct ExtraMat { std::string name; mat::Image img; unsigned tex = 0; };
    std::vector<ExtraMat> extraMats;
    const std::string kExtrasDir = pack::extrasDir();
    std::filesystem::create_directories(kExtrasDir);
    auto loadExtraFile = [&](const std::string& name, const std::string& path) {
        ExtraMat e;
        e.name = name;
        e.img = mat::loadPNG(path.c_str());
        e.tex = uploadImgTex(e.img);
        extraMats.push_back(std::move(e));
    };
    for (auto& ent : std::filesystem::directory_iterator(kExtrasDir)) {
        if (!ent.is_regular_file()) continue;
        auto ext = ent.path().extension().string();
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (ext != ".png") continue;
        loadExtraFile(ent.path().stem().string(), ent.path().string());
    }

    auto glTexForName = [&](const std::string& name) -> unsigned {
        auto it = overlayGL.find(name);
        if (it != overlayGL.end() && it->second) return it->second;
        for (int i = 0; i < (int)entMouthNames.size() && i < 3; i++)
            if (name == entMouthNames[i] && mouthGL[i]) return mouthGL[i];
        int slot = pm::overlayFileSlotOf(name, entMouthNames);
        if (slot == 0 && leafGL) return leafGL;
        if (slot == 1 && eyeGL) return eyeGL;
        if (slot >= 2 && slot < 5 && mouthGL[slot - 2]) return mouthGL[slot - 2];
        if (slot == 5 && eyelidGL) return eyelidGL;
        int ti = mat::tileIndex(name.c_str());
        if (ti >= 0 && tileGL[ti]) return tileGL[ti];
        for (const ExtraMat& e : extraMats) if (e.tex && e.name == name) return e.tex;
        return atlasTex;
    };
    Editor ed;
    mat::Model editModel;       // block model currently being edited
    mat::RandParams editRand;   // matching .rand (density, blade size, …)
    mat::Model savedModel;      // last loaded/saved snapshot (Cancel)
    mat::RandParams savedRand;
    std::vector<uint8_t> selMark;
    std::vector<uint8_t> solidMark;
    float fillBaseArea = 0.0f;
    ed.entityParts = pm::buildPlayerModel();          // entity (player) model parts
    std::vector<pm::Part> savedEntity = ed.entityParts;
    std::vector<std::vector<pm::Part>> entUndo;
    std::vector<uint8_t> entMark;
    auto ensureEntMark = [&]() {
        if ((int)entMark.size() != (int)ed.entityParts.size())
            entMark.assign(ed.entityParts.size(), 0);
        if (ed.entityParts.empty()) { ed.entitySel = -1; return; }
        if (ed.entitySel >= (int)ed.entityParts.size())
            ed.entitySel = (int)ed.entityParts.size() - 1;
    };
    auto markEnt = [&](int i, bool add) {
        ensureEntMark();
        if (i < 0 || i >= (int)ed.entityParts.size()) return;
        if (add && entMark[i] && keyDown(VK_CONTROL) && !keyDown(VK_SHIFT)) {
            entMark[i] = 0;
            if (ed.entitySel == i) {
                ed.entitySel = 0;
                for (int j = 0; j < (int)entMark.size(); j++)
                    if (entMark[j]) { ed.entitySel = j; break; }
            }
            return;
        }
        if (!add) std::fill(entMark.begin(), entMark.end(), 0);
        entMark[i] = 1;
        ed.entitySel = i;
        if (!add && ed.entityParts[i].bind >= 0) {
            int b = ed.entityParts[i].bind;
            for (int j = 0; j < (int)ed.entityParts.size(); j++)
                if (ed.entityParts[j].bind == b) entMark[j] = 1;
        }
    };
    auto entSelected = [&]() {
        ensureEntMark();
        std::vector<int> out;
        bool any = false;
        for (uint8_t m : entMark) if (m) { any = true; break; }
        if (!any && ed.entitySel >= 0 && !ed.entityParts.empty()) markEnt(ed.entitySel, false);
        for (int i = 0; i < (int)entMark.size(); i++) if (entMark[i]) out.push_back(i);
        return out;
    };
    auto pushEntUndo = [&]() {
        entUndo.push_back(ed.entityParts);
        if (entUndo.size() > 32) entUndo.erase(entUndo.begin());
    };
    auto undoEnt = [&]() {
        if (entUndo.empty()) return;
        ed.entityParts = entUndo.back();
        entUndo.pop_back();
        entMark.clear();
        ensureEntMark();
        ed.hairVoxSel.clear();
        ed.hairCardSel.clear();
        ed.dirty = true;
    };
    auto paintKind = [&]() -> int {
        if (ed.entityParts.empty() || ed.entitySel < 0 || ed.entitySel >= (int)ed.entityParts.size())
            return -1;
        const pm::Part& sp = ed.entityParts[ed.entitySel];
        if (pm::isEyePart(sp)) return 0;
        if (pm::isEyelidPart(sp)) return 1;
        if (pm::isMouthPart(sp)) {
            for (int i = 0; i < (int)entMouthNames.size() && i < 3; i++)
                if (sp.tex == entMouthNames[i]) return 2 + i;
            return 2 + pm::mouthIndex(sp.tex);
        }
        return -1;
    };
    auto paintImgOf = [&](int k) -> mat::Image* {
        if (k == 0) return &eyeImg;
        if (k == 1) return &eyelidImg;
        if (k >= 2 && k < 5) return &mouthImgs[k - 2];
        return &skinImg;
    };
    auto paintUndoOf = [&](int k) -> std::vector<std::vector<uint8_t>>* {
        if (k == 0) return &eyeUndo;
        if (k == 1) return &eyelidUndo;
        if (k >= 2 && k < 5) return &mouthUndo[k - 2];
        return &skinUndo;
    };
    auto syncPaintGL = [&]() {
        int k = paintKind();
        if (k == 0 && eyeGL && eyeImg.ok()) uploadImgPixels(eyeGL, eyeImg);
        else if (k == 1 && eyelidGL && eyelidImg.ok()) uploadImgPixels(eyelidGL, eyelidImg);
        else if (k >= 2 && k < 5 && mouthGL[k - 2] && mouthImgs[k - 2].ok())
            uploadImgPixels(mouthGL[k - 2], mouthImgs[k - 2]);
        else if (skinGL && skinImg.ok()) uploadImgPixels(skinGL, skinImg);
    };
    auto pushSkinUndo = [&]() {
        int k = paintKind();
        mat::Image* img = paintImgOf(k);
        auto* st = paintUndoOf(k);
        if (!img || !img->ok() || !st) return;
        st->push_back(img->rgba);
        if (st->size() > 32) st->erase(st->begin());
    };
    auto syncSkinGL = [&]() { syncPaintGL(); };
    uint64_t clothSheetSig = 1;
    auto clothGeomSig = [&]() {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        mix((uint64_t)ed.entityParts.size());
        for (const pm::Part& p : ed.entityParts) {
            if (!pm::partWantsModelSheet(p)) continue;
            auto f = [&](float x) { mix((uint64_t)std::llround(x * 10000.0f)); };
            f(p.center.x); f(p.center.y); f(p.center.z);
            f(p.half.x); f(p.half.y); f(p.half.z);
            f(p.color.x); f(p.color.y); f(p.color.z);
        }
        return h;
    };
    auto rebuildClothSheet = [&]() {
        if (!entIsCloth) return;
        for (pm::Part& p : ed.entityParts) {
            if (!pm::partWantsModelSheet(p)) continue;
            p.boxX = p.boxY = -1;
            p.boxW = p.boxH = p.boxD = 0;
        }
        entSheetW = 0;
        entSheetH = 0;
        pm::assignModelSheet(ed.entityParts, entSheetW, entSheetH);
        if (entSheetW < 1 || entSheetH < 1) return;
        std::vector<uint8_t> rgba;
        pm::fillSheetRgba(rgba, entSheetW, entSheetH, ed.entityParts);
        skinImg.w = entSheetW;
        skinImg.h = entSheetH;
        skinImg.rgba = std::move(rgba);
        if (!skinGL) skinGL = uploadImgTex(skinImg);
        else uploadImgPixels(skinGL, skinImg);
        clampTex(skinGL);
        if (!ed.entityName.empty()) clothSheetGL[ed.entityName] = skinGL;
        clothSheetSig = clothGeomSig();
    };
    auto undoSkin = [&]() {
        int k = paintKind();
        mat::Image* img = paintImgOf(k);
        auto* st = paintUndoOf(k);
        if (!img || !st || st->empty()) return;
        img->rgba = st->back();
        st->pop_back();
        syncPaintGL();
        ed.dirty = true;
    };
    auto paintUndoEmpty = [&]() -> bool {
        auto* st = paintUndoOf(paintKind());
        return !st || st->empty();
    };
    auto undoEntityCmd = [&]() {
        bool paintTool = ed.picker || (ed.tool >= 0 && ed.tool <= 8);
        if ((paintTool || ed.entSkinView) && !paintUndoEmpty()) undoSkin();
        else undoEnt();
    };
    auto migrateFaceOverlays = [&](std::vector<pm::Part>& parts) {
        for (pm::Part& p : parts) {
            if (p.name.empty() && !p.tex.empty()) {
                if (p.tex == "eye_l" || p.tex == "eye_r") {
                    p.name = p.tex;
                    p.tex = "eye";
                    p.kind = "eye";
                } else if (p.tex == "eyelid_l" || p.tex == "eyelid_r") {
                    p.name = p.tex;
                    p.tex = "eyelid";
                    p.kind = "eyelid";
                } else if (pm::isMouthTex(p.tex) && p.tex != "mouth") {
                    p.name = "mouth";
                    p.kind = "mouth";
                } else if (pm::isHairPart(p)) {
                    p.name = "hair";
                    p.kind = "hair";
                }
            }
            if ((p.tex == "eye" || p.tex == "eye.png") && p.name.empty()) {
                p.name = (p.center.x < 0.0f) ? "eye_l" : "eye_r";
                p.tex = "eye";
                p.kind = "eye";
                p.side = (p.center.x < 0.0f) ? -1 : 1;
            }
            if ((p.tex == "eyelid" || p.tex == "eyelid.png") && p.name.empty()) {
                p.name = (p.center.x < 0.0f) ? "eyelid_l" : "eyelid_r";
                p.tex = "eyelid";
                p.kind = "eyelid";
                p.side = (p.center.x < 0.0f) ? -1 : 1;
            }
            if (p.kind.empty()) p.kind = pm::effectiveKind(p);
        }
        bool hasL = false, hasR = false, hasLidL = false, hasLidR = false, hasMouth = false;
        for (const pm::Part& p : parts) {
            if (pm::partName(p) == "eye_l") hasL = true;
            if (pm::partName(p) == "eye_r") hasR = true;
            if (pm::partName(p) == "eyelid_l") hasLidL = true;
            if (pm::partName(p) == "eyelid_r") hasLidR = true;
            if (pm::isMouthPart(p)) hasMouth = true;
        }
        auto addOv = [&](float x, float y, float z, float hx, float hy, const char* id, const char* mat, int side) {
            pm::Part q;
            q.center = { x, y, z };
            q.half = { hx, hy, 0.004f };
            q.color = { 1, 1, 1 };
            q.type = 0;
            q.side = side;
            q.name = id;
            q.tex = mat;
            if (std::strcmp(id, "mouth") == 0 || pm::isMouthTex(mat)) q.kind = "mouth";
            else if (pm::isEyeTex(id) || pm::isEyeTex(mat)) q.kind = "eye";
            else if (pm::isEyelidTex(id) || pm::isEyelidTex(mat)) q.kind = "eyelid";
            parts.push_back(q);
        };
        if (!hasL) addOv(-0.0623f, 1.6319f, 0.1840f, 0.0388f, 0.0249f, "eye_l", "eye", -1);
        if (!hasR) addOv( 0.0623f, 1.6319f, 0.1840f, 0.0388f, 0.0249f, "eye_r", "eye", 1);
        if (!hasLidL) addOv(-0.0623f, 1.6782f, 0.1860f, 0.0388f, 0.0111f, "eyelid_l", "eyelid", -1);
        if (!hasLidR) addOv( 0.0623f, 1.6782f, 0.1860f, 0.0388f, 0.0111f, "eyelid_r", "eyelid", 1);
        if (!hasMouth) addOv(0.0000f, 1.5409f, 0.1840f, 0.0582f, 0.0194f, "mouth", "mouth_closed", 0);
    };
    auto findPartTex = [&](const char* name) -> int {
        for (int i = 0; i < (int)ed.entityParts.size(); i++)
            if (pm::partName(ed.entityParts[i]) == name) return i;
        return -1;
    };
    auto geomSelected = [&]() {
        auto idx = entSelected();
        std::vector<int> out;
        for (int i : idx)
            if (pm::canGizmoPart(ed.entityParts[i])) out.push_back(i);
        return out;
    };
    auto entitySkinPathFor = [&](const std::string& name) -> std::string {
        return pack::entityPng(name);
    };
    auto entityModelPathFor = [&](const std::string& name) -> std::string {
        return pack::entityModel(name);
    };
    auto scanEntFiles = [&]() {
        entNames.clear();
        for (const plugin::EntityModule& m : plugin::entities())
            entNames.push_back(m.id);
        if (entNames.empty()) entNames.push_back("player");
        std::sort(entNames.begin(), entNames.end());
        garmentNames.clear();
        std::filesystem::path clothDir = std::filesystem::path(pack::entitiesDir()) / "clothes";
        std::error_code ec;
        if (std::filesystem::is_directory(clothDir, ec)) {
            for (const auto& ent : std::filesystem::directory_iterator(clothDir, ec)) {
                if (!ent.is_regular_file()) continue;
                if (ent.path().extension() == ".model")
                    garmentNames.push_back(ent.path().stem().string());
            }
        }
        std::sort(garmentNames.begin(), garmentNames.end());
    };
    auto entityListCount = [&]() {
        return (int)entNames.size() + (int)garmentNames.size();
    };
    auto entityListName = [&](int i) -> const std::string* {
        if (i < 0) return nullptr;
        if (i < (int)entNames.size()) return &entNames[i];
        int g = i - (int)entNames.size();
        if (g >= 0 && g < (int)garmentNames.size()) return &garmentNames[g];
        return nullptr;
    };
    auto loadGarmentFile = [&](const std::string& stem) {
        if (stem.empty()) return;
        entIsCloth = true;
        ed.entityName = stem;
        entityPath = pack::join(pack::entitiesDir(), "clothes/" + stem + ".model");
        pm::EntityFile ef = pm::loadEntity(entityPath.c_str());
        ed.entityParts = std::move(ef.parts);
        pm::shellClothParts(ed.entityParts);
        savedEntity = ed.entityParts;
        entExtraLines = std::move(ef.extraLines);
        entMark.clear();
        ensureEntMark();
        ed.refTool = false;
        ed.refDecalIdx = -1;
        ed.refEdge = -1;
        ed.refLineHover = -1;
        ed.hairPaint = false;
        ed.hairErase = false;
        ed.hairSelect = false;
        ed.hairCard = false;
        ed.hairSelDrag = false;
        ed.hairVoxSel.clear();
        ed.hairCardSel.clear();
        ed.measureOn = false;
        ed.partMeasure = false;
        ed.planeOn = false;
        ed.partPlane = false;
        if (!ed.entityParts.empty()) markEnt(0, false);
        entUndo.clear();
        ed.dirty = false;
        entitySkinPath = pack::join(pack::entitiesDir(), "clothes/" + stem + ".png");
        clothSheetSig = 0;
        rebuildClothSheet();
        savedSkin = skinImg;
        savedEntity = ed.entityParts;
        skinUndo.clear();
        ed.dirty = false;
        ed.entSkinView = false;
    };
    auto loadEntityFile = [&](const std::string& name) {
        if (name.empty()) return;
        entIsCloth = false;
        const plugin::EntityModule* mod = plugin::findEntity(name.c_str());
        if (!mod) return;
        ed.entityName = mod->id;
        const std::string& stem = mod->appearance.empty() ? mod->id : mod->appearance;
        entityPath = entityModelPathFor(stem);
        entitySkinPath = entitySkinPathFor(stem);
        pm::EntityFile ef = pm::loadEntity(entityPath.c_str());
        pm::fillEntityDefaults(ef);
        std::vector<pm::Part> parts = std::move(ef.parts);
        entSkin = ef.skin.empty() ? name : ef.skin;
        entMouthNames = ef.mouthTex;
        entFaceNames = ef.faceNames;
        entOverlays = ef.overlays;
        entExtraLines = std::move(ef.extraLines);
        migrateFaceOverlays(parts);
        ed.entityParts = std::move(parts);
        savedEntity = ed.entityParts;
        entMark.clear();
        ensureEntMark();
        ed.refTool = false;
        ed.refDecalIdx = -1;
        ed.refEdge = -1;
        ed.refLineHover = -1;
        ed.hairPaint = false;
        ed.hairErase = false;
        ed.hairSelect = false;
        ed.hairCard = false;
        ed.hairSelDrag = false;
        ed.hairVoxSel.clear();
        ed.hairCardSel.clear();
        ed.planeOn = false;
        ed.partPlane = false;
        if (!ed.entityParts.empty()) markEnt(0, false);
        entSheetW = 0;
        entSheetH = 0;
        ed.entSkinView = !entSkin.empty();
        mat::Image img = mat::loadPNG(entitySkinPath.c_str());
        if (!img.ok() && name == "player") {
            std::string defSkin = pack::entityPng("player");
            img = mat::loadPNG(defSkin.c_str());
        }
        if (!img.ok()) {
            std::vector<uint8_t> rgba;
            tex::generatePlayerSkin(rgba);
            img.w = pm::kSkinW;
            img.h = pm::kSkinH;
            img.rgba = std::move(rgba);
        }
        if (img.w != pm::kSkinW || img.h != pm::kSkinH) {
            std::vector<uint8_t> rgba;
            tex::generatePlayerSkin(rgba);
            img.w = pm::kSkinW;
            img.h = pm::kSkinH;
            img.rgba = std::move(rgba);
        }
        skinImg = std::move(img);
        savedSkin = skinImg;
        savedEye = eyeImg;
        savedEyelid = eyelidImg;
        for (int i = 0; i < 3; i++) {
            if (i < (int)entMouthNames.size()) {
                std::string path = pack::entityPng(entMouthNames[i]);
                std::vector<uint8_t> rgba;
                tex::generatePlayerMouth(rgba, pm::mouthIndex(entMouthNames[i]));
                if (!pm::fileExists(path.c_str()))
                    mat::savePNG(path.c_str(), pm::kMouthW, pm::kMouthH, rgba.data());
                mouthImgs[i] = loadOverlayImg(path.c_str(), pm::kMouthW, pm::kMouthH);
                if (mouthGL[i] && mouthImgs[i].ok())
                    uploadImgPixels(mouthGL[i], mouthImgs[i]);
            }
            savedMouth[i] = mouthImgs[i];
        }
        skinUndo.clear();
        eyeUndo.clear();
        eyelidUndo.clear();
        for (int i = 0; i < 3; i++) mouthUndo[i].clear();
        syncSkinGL();
        if (bodySkinGL && skinImg.ok()) uploadImgPixels(bodySkinGL, skinImg);
        entUndo.clear();
        ed.dirty = false;
    };
    struct EntThumb {
        std::string key;
        std::vector<float> solid;
        struct Batch { unsigned tex = 0; std::vector<float> verts; };
        std::vector<Batch> batches;
        float px = 0, py = 0.9f, pz = 0, ext = 1.8f;
    };
    std::vector<EntThumb> entThumbCache;
    auto dropEntThumb = [&](const std::string& key) {
        entThumbCache.erase(std::remove_if(entThumbCache.begin(), entThumbCache.end(),
            [&](const EntThumb& t) { return t.key == key; }), entThumbCache.end());
    };
    auto thumbKeyFor = [&](int i) -> std::string {
        const std::string* nm = entityListName(i);
        if (!nm) return {};
        return (i < (int)entNames.size() ? "e:" : "c:") + *nm;
    };
    auto fillEntThumb = [&](EntThumb& t, const std::vector<pm::Part>& parts,
                            unsigned skinTex, unsigned sheetTex, int sheetW, int sheetH) {
        t.solid.clear();
        t.batches.clear();
        bool any = false;
        float mn[3] = {}, mxv[3] = {};
        for (const pm::Part& p : parts) {
            Vec3 cs[8];
            pm::partWorldCorners(p, cs);
            for (int k = 0; k < 8; k++) {
                if (!any) {
                    mn[0] = mxv[0] = cs[k].x; mn[1] = mxv[1] = cs[k].y; mn[2] = mxv[2] = cs[k].z;
                    any = true;
                } else {
                    if (cs[k].x < mn[0]) mn[0] = cs[k].x;
                    if (cs[k].x > mxv[0]) mxv[0] = cs[k].x;
                    if (cs[k].y < mn[1]) mn[1] = cs[k].y;
                    if (cs[k].y > mxv[1]) mxv[1] = cs[k].y;
                    if (cs[k].z < mn[2]) mn[2] = cs[k].z;
                    if (cs[k].z > mxv[2]) mxv[2] = cs[k].z;
                }
            }
        }
        t.px = any ? 0.5f * (mn[0] + mxv[0]) : 0.0f;
        t.py = any ? 0.5f * (mn[1] + mxv[1]) : 0.90f;
        t.pz = any ? 0.5f * (mn[2] + mxv[2]) : 0.0f;
        float extY = any ? (mxv[1] - mn[1]) : 1.8f;
        float extX = any ? (mxv[0] - mn[0]) : 0.6f;
        float extZ = any ? (mxv[2] - mn[2]) : 0.6f;
        t.ext = extY;
        if (extX > t.ext) t.ext = extX;
        if (extZ > t.ext) t.ext = extZ;
        if (t.ext < 0.2f) t.ext = 0.2f;
        auto hasNamed = [&](const std::string& name) {
            if (name.empty()) return false;
            auto it = overlayGL.find(name);
            if (it != overlayGL.end() && it->second) return true;
            for (const ExtraMat& e : extraMats) if (e.tex && e.name == name) return true;
            return false;
        };
        pdraw::Mesh mesh;
        pdraw::build(parts, pdraw::identXform, hasNamed, skinTex != 0, false, mesh, false, sheetW, sheetH);
        t.solid = std::move(mesh.solid);
        auto add = [&](unsigned tex, std::vector<float>& v) {
            if (!tex || v.empty()) return;
            EntThumb::Batch b;
            b.tex = tex;
            b.verts = std::move(v);
            t.batches.push_back(std::move(b));
        };
        add(skinTex, mesh.skin);
        add(sheetTex, mesh.sheet);
        add(atlasTex, mesh.atlas);
        for (auto& kv : mesh.named) {
            unsigned tex = glTexForName(kv.first);
            add(tex, kv.second);
        }
    };
    auto ensureEntThumb = [&](int i) -> const EntThumb* {
        std::string key = thumbKeyFor(i);
        if (key.empty()) return nullptr;
        for (const EntThumb& t : entThumbCache)
            if (t.key == key) return &t;
        const std::string* nm = entityListName(i);
        std::vector<pm::Part> parts;
        unsigned skinTex = entIsCloth ? bodySkinGL : skinGL;
        unsigned sheetTex = 0;
        int sw = 0, sh = 0;
        if (nm && i < (int)entNames.size()) {
            if (const plugin::EntityModule* mod = plugin::findEntity(nm->c_str())) {
                const std::string& stem = mod->appearance.empty() ? mod->id : mod->appearance;
                pm::EntityFile ef = pm::loadEntity(entityModelPathFor(stem).c_str());
                pm::fillEntityDefaults(ef);
                parts = std::move(ef.parts);
            }
        } else if (nm) {
            std::string path = pack::join(pack::entitiesDir(), "clothes/" + *nm + ".model");
            pm::EntityFile ef = pm::loadEntity(path.c_str());
            sw = ef.skinW;
            sh = ef.skinH;
            pm::shellClothParts(ef.parts);
            pm::assignModelSheet(ef.parts, sw, sh);
            auto cached = clothSheetGL.find(*nm);
            if (cached != clothSheetGL.end() && cached->second) sheetTex = cached->second;
            else if (entIsCloth && *nm == ed.entityName && skinGL) sheetTex = skinGL;
            else {
                std::string png = pack::join(pack::entitiesDir(), "clothes/" + *nm + ".png");
                mat::Image img = mat::loadPNG(png.c_str());
                if (!img.ok() || img.w != sw || img.h != sh) {
                    std::vector<uint8_t> rgba;
                    pm::fillSheetRgba(rgba, sw, sh, ef.parts);
                    img.w = sw;
                    img.h = sh;
                    img.rgba = std::move(rgba);
                }
                sheetTex = uploadImgTex(img);
                clampTex(sheetTex);
                if (sheetTex) clothSheetGL[*nm] = sheetTex;
            }
            parts = std::move(ef.parts);
            skinTex = 0;
        }
        EntThumb t;
        t.key = key;
        fillEntThumb(t, parts, skinTex, sheetTex, sw, sh);
        entThumbCache.push_back(std::move(t));
        return &entThumbCache.back();
    };
    auto saveEntity = [&]() {
        if (entIsCloth) {
            pm::EntityFile ef;
            ef.skin = "clothes/" + ed.entityName;
            ef.skinW = entSheetW;
            ef.skinH = entSheetH;
            ef.parts = ed.entityParts;
            ef.extraLines = entExtraLines;
            pm::saveEntity(entityPath.c_str(), ef);
            if (skinImg.ok()) mat::savePNG(entitySkinPath.c_str(), skinImg.w, skinImg.h, skinImg.rgba.data());
            savedEntity = ed.entityParts;
            savedSkin = skinImg;
            ed.dirty = false;
            dropEntThumb(std::string("c:") + ed.entityName);
            clothSheetGL.erase(ed.entityName);
            return;
        }
        pm::EntityFile ef;
        ef.skin = entSkin.empty() ? ed.entityName : entSkin;
        ef.overlays = entOverlays;
        ef.faceNames = entFaceNames;
        ef.parts = ed.entityParts;
        ef.mouthTex = entMouthNames;
        ef.extraLines = entExtraLines;
        pm::saveEntity(entityPath.c_str(), ef);
        if (skinImg.ok()) mat::savePNG(entitySkinPath.c_str(), skinImg.w, skinImg.h, skinImg.rgba.data());
        std::string eyeP = pack::entityPng("eye");
        std::string lidP = pack::entityPng("eyelid");
        if (eyeImg.ok()) mat::savePNG(eyeP.c_str(), eyeImg.w, eyeImg.h, eyeImg.rgba.data());
        if (eyelidImg.ok()) mat::savePNG(lidP.c_str(), eyelidImg.w, eyelidImg.h, eyelidImg.rgba.data());
        for (int i = 0; i < 3 && i < (int)entMouthNames.size(); i++) {
            if (mouthImgs[i].ok()) {
                std::string path = pack::entityPng(entMouthNames[i]);
                mat::savePNG(path.c_str(), mouthImgs[i].w, mouthImgs[i].h, mouthImgs[i].rgba.data());
            }
        }
        savedEntity = ed.entityParts;
        savedSkin = skinImg;
        savedEye = eyeImg;
        savedEyelid = eyelidImg;
        for (int i = 0; i < pm::kMouthVariantCount; i++) savedMouth[i] = mouthImgs[i];
        ed.dirty = false;
        dropEntThumb(std::string("e:") + ed.entityName);
    };
    auto cancelEntity = [&]() {
        ed.entityParts = savedEntity;
        skinImg = savedSkin;
        eyeImg = savedEye;
        eyelidImg = savedEyelid;
        for (int i = 0; i < pm::kMouthVariantCount; i++) mouthImgs[i] = savedMouth[i];
        syncSkinGL();
        skinUndo.clear();
        eyeUndo.clear();
        eyelidUndo.clear();
        for (int i = 0; i < 3; i++) mouthUndo[i].clear();
        entMark.clear();
        ensureEntMark();
        ed.refTool = false;
        ed.refDecalIdx = -1;
        ed.refEdge = -1;
        ed.refLineHover = -1;
        ed.hairPaint = false;
        ed.hairErase = false;
        ed.hairSelect = false;
        ed.hairCard = false;
        ed.hairSelDrag = false;
        ed.hairVoxSel.clear();
        ed.hairCardSel.clear();
        ed.dirty = false;
    };
    auto clearRefGuide = [&]() {
        ed.refDecalIdx = -1;
        ed.refEdge = -1;
        ed.refLineHover = -1;
    };
    auto toggleRefTool = [&]() {
        ed.refTool = !ed.refTool;
        if (!ed.refTool) clearRefGuide();
    };
    auto snapMovedDecalsToRef = [&]() {
        if (ed.refDecalIdx < 0 || ed.refEdge < 0 || !ed.refTool) return;
        if (ed.refDecalIdx >= (int)ed.entityParts.size()) {
            ed.refDecalIdx = -1;
            ed.refEdge = -1;
            return;
        }
        const pm::Part& refP = ed.entityParts[ed.refDecalIdx];
        if (!pm::isDecalPart(refP)) return;
        Vec3 rq[4];
        pm::texQuadLocal(refP, rq);
        Vec3 ra = rq[ed.refEdge], rb = rq[(ed.refEdge + 1) % 4];
        Vec3 ru = rb - ra;
        float rlen = ru.length();
        if (rlen < 1e-8f) return;
        ru = ru / rlen;
        const float snapThr = 0.035f;
        for (int i : geomSelected()) {
            if (i == ed.refDecalIdx || !pm::isDecalPart(ed.entityParts[i])) continue;
            pm::Part& p = ed.entityParts[i];
            Vec3 q[4];
            pm::texQuadLocal(p, q);
            Vec3 bestCorr{};
            float bestD = snapThr;
            for (int e = 0; e < 4; e++) {
                Vec3 ma = q[e], mb = q[(e + 1) % 4];
                Vec3 mu = mb - ma;
                float mlen = mu.length();
                if (mlen < 1e-8f) continue;
                mu = mu / mlen;
                if (std::fabs(mu.dot(ru)) < 0.92f) continue;
                Vec3 mMid = (ma + mb) * 0.5f;
                Vec3 closest = closestOnInfiniteLine(ra, ru, mMid);
                Vec3 corr = closest - mMid;
                float d = corr.length();
                if (d < bestD) { bestD = d; bestCorr = corr; }
            }
            if (bestCorr.lengthSq() > 1e-12f) {
                p.center.x += bestCorr.x;
                p.center.y += bestCorr.y;
                p.center.z += bestCorr.z;
            }
        }
    };
    auto entCenter = [&](float& cx, float& cy, float& cz) {
        auto idx = geomSelected();
        if (idx.empty()) idx = entSelected();
        cx = 0; cy = 0.9f; cz = 0;
        if (idx.empty()) return;
        cx = cy = cz = 0;
        for (int i : idx) {
            cx += ed.entityParts[i].center.x;
            cy += ed.entityParts[i].center.y;
            cz += ed.entityParts[i].center.z;
        }
        float n = (float)idx.size();
        cx /= n; cy /= n; cz /= n;
    };
    auto uniquePartName = [&](const char* base) -> std::string {
        for (int n = 1; n < 1000; n++) {
            std::string s = (n == 1) ? std::string(base) : (std::string(base) + "_" + std::to_string(n));
            bool used = false;
            for (const pm::Part& p : ed.entityParts)
                if (p.name == s) { used = true; break; }
            if (!used) return s;
        }
        return std::string(base) + "_x";
    };
    auto findHostIdx = [&](int di) -> int {
        if (di < 0 || di >= (int)ed.entityParts.size()) return -1;
        const pm::Part& d = ed.entityParts[di];
        if (!d.support.empty()) {
            for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                if (i == di || pm::isDecalPart(ed.entityParts[i])) continue;
                if (pm::partName(ed.entityParts[i]) == d.support) return i;
            }
        }
        int best = -1;
        float bestR = 0.0f;
        for (int i = 0; i < (int)ed.entityParts.size(); i++) {
            if (i == di || pm::isDecalPart(ed.entityParts[i])) continue;
            float r = pm::decalOnHost(d, ed.entityParts[i]);
            if (r > bestR) { bestR = r; best = i; }
        }
        return best;
    };
    auto selectedDecalsOnModel = [&]() -> bool {
        for (int i : entSelected()) {
            if (!pm::isDecalPart(ed.entityParts[i])) continue;
            int h = findHostIdx(i);
            if (h < 0) return false;
            if (pm::decalOnHost(ed.entityParts[i], ed.entityParts[h]) < pm::kDecalMinCover)
                return false;
        }
        return true;
    };
    auto revertIfDecalOff = [&](const std::vector<pm::Part>& before) {
        if (!selectedDecalsOnModel()) ed.entityParts = before;
    };
    auto copyBoundPart = [&](pm::Part& out) -> bool {
        if (!ed.planeOn || ed.planeFace < 0 || ed.planeFace > 5) return false;
        if (ed.planeBody) {
            std::vector<pm::Part> body = pm::buildPlayerModel();
            if (ed.planePart < 0 || ed.planePart >= (int)body.size()) { ed.planeOn = false; return false; }
            out = body[ed.planePart];
            return true;
        }
        if (ed.planePart < 0 || ed.planePart >= (int)ed.entityParts.size()) { ed.planeOn = false; return false; }
        out = ed.entityParts[ed.planePart];
        return true;
    };
    auto boundFacePlane = [&](int& axis, float& pos) -> bool {
        pm::Part p;
        if (!copyBoundPart(p)) return false;
        Vec3 q[4];
        pm::cuboidFaceCorners(p, ed.planeFace, q);
        Vec3 n = (q[1] - q[0]).cross(q[2] - q[0]);
        float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        axis = (ay >= ax && ay >= az) ? 1 : (az >= ax ? 2 : 0);
        float mag = (axis == 0) ? ax : (axis == 1) ? ay : az;
        float len = std::sqrt(ax * ax + ay * ay + az * az);
        if (len < 1e-8f || mag / len < 0.92f) return false;
        pos = 0.0f;
        for (int k = 0; k < 4; k++)
            pos += (axis == 0) ? q[k].x : (axis == 1) ? q[k].y : q[k].z;
        pos *= 0.25f;
        return true;
    };
    auto snapGroupToPlane = [&](int axis) {
        int pAxis = 0;
        float pPos = 0.0f;
        if (!boundFacePlane(pAxis, pPos) || axis != pAxis || axis < 0 || axis > 2) return;
        auto idx = geomSelected();
        if (idx.empty()) return;
        float mn = 1e9f, mx = -1e9f;
        for (int i : idx) {
            Vec3 cs[8];
            pm::partWorldCorners(ed.entityParts[i], cs);
            for (int k = 0; k < 8; k++) {
                float v = (axis == 0) ? cs[k].x : (axis == 1) ? cs[k].y : cs[k].z;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
        }
        float dMn = pPos - mn;
        float dMx = pPos - mx;
        float corr = 0.0f;
        if (std::fabs(dMn) <= 0.03f && std::fabs(dMn) <= std::fabs(dMx)) corr = dMn;
        else if (std::fabs(dMx) <= 0.03f) corr = dMx;
        if (corr == 0.0f) return;
        for (int i : idx) {
            float& c = (axis == 0) ? ed.entityParts[i].center.x
                : (axis == 1) ? ed.entityParts[i].center.y : ed.entityParts[i].center.z;
            c += corr;
        }
    };
    auto snapFaceToPlane = [&](pm::Part& p, int axis, bool oneSided, float sideSign) {
        int pAxis = 0;
        float pPos = 0.0f;
        if (!boundFacePlane(pAxis, pPos) || pAxis != axis || p.rot.lengthSq() > 1e-6f) return;
        float& h = vecComp(p.half, axis);
        float& c = vecComp(p.center, axis);
        const float tol = 0.03f;
        if (oneSided) {
            float pinned = c - sideSign * h;
            float face = c + sideSign * h;
            if (std::fabs(face - pPos) >= tol) return;
            if ((pPos - pinned) * sideSign <= 1e-4f) return;
            float nh = (pPos - pinned) * sideSign * 0.5f;
            if (nh < 1e-4f) nh = 1e-4f;
            h = nh;
            c = pinned + sideSign * h;
        } else {
            float pos = c + h, neg = c - h;
            if (std::fabs(pos - pPos) < tol && pPos > c + 1e-4f) h = pPos - c;
            else if (std::fabs(neg - pPos) < tol && pPos < c - 1e-4f) h = c - pPos;
            if (h < 1e-4f) h = 1e-4f;
        }
    };
    auto translateEnt = [&](float dx, float dy, float dz) {
        auto idx = geomSelected();
        if (idx.empty()) return;
        auto before = ed.entityParts;
        for (int i : idx) {
            ed.entityParts[i].center.x += dx;
            ed.entityParts[i].center.y += dy;
            ed.entityParts[i].center.z += dz;
        }
        snapMovedDecalsToRef();
        if (ed.planeOn) {
            if (std::fabs(dx) > 1e-8f) snapGroupToPlane(0);
            if (std::fabs(dy) > 1e-8f) snapGroupToPlane(1);
            if (std::fabs(dz) > 1e-8f) snapGroupToPlane(2);
        }
        revertIfDecalOff(before);
        ed.dirty = true;
    };
    auto snapEnt = [&]() {
        auto idx = geomSelected();
        if (idx.empty()) return;
        pushEntUndo();
        const float grid = 0.05f;
        float mn[3] = { 1e9f, 1e9f, 1e9f };
        for (int i : idx) {
            Vec3 cs[8];
            pm::partWorldCorners(ed.entityParts[i], cs);
            for (int k = 0; k < 8; k++) {
                mn[0] = std::min(mn[0], cs[k].x);
                mn[1] = std::min(mn[1], cs[k].y);
                mn[2] = std::min(mn[2], cs[k].z);
            }
        }
        float d[3];
        for (int k = 0; k < 3; k++) d[k] = std::round(mn[k] / grid) * grid - mn[k];
        translateEnt(d[0], d[1], d[2]);
    };
    auto alignEnt = [&]() {
        auto idx = geomSelected();
        if (idx.empty()) return;
        pushEntUndo();
        const float grid = 0.05f;
        for (int i : idx) {
            pm::Part& p = ed.entityParts[i];
            p.center.x = std::round(p.center.x / grid) * grid;
            p.center.y = std::round(p.center.y / grid) * grid;
            p.center.z = std::round(p.center.z / grid) * grid;
            p.half.x = std::max(0.01f, std::round(p.half.x / grid) * grid);
            p.half.y = std::max(0.01f, std::round(p.half.y / grid) * grid);
            p.half.z = std::max(0.01f, std::round(p.half.z / grid) * grid);
        }
        if (!selectedDecalsOnModel()) undoEnt();
        else ed.dirty = true;
    };
    auto mirrorEnt = [&](int axis) {
        auto idx = geomSelected();
        if (idx.empty() || axis < 0 || axis > 2) return;
        pushEntUndo();
        float cx, cy, cz;
        entCenter(cx, cy, cz);
        float c[3] = { cx, cy, cz };
        for (int i : idx) {
            pm::Part& p = ed.entityParts[i];
            vecComp(p.center, axis) = 2.0f * c[axis] - vecComp(p.center, axis);
            pm::mirrorPartRot(p.rot, axis);
            if (axis == 0 && p.side != 0) p.side = -p.side;
        }
        if (!selectedDecalsOnModel()) undoEnt();
        else ed.dirty = true;
    };
    auto stretchEnt = [&](int axis, float dt, bool oneSided, float sideSign) {
        auto idx = geomSelected();
        if (idx.empty() || axis < 0 || axis > 2) return;
        auto before = ed.entityParts;
        for (int i : idx) {
            pm::Part& p = ed.entityParts[i];
            float& h = vecComp(p.half, axis);
            float& c = vecComp(p.center, axis);
            if (oneSided) {
                c += dt * 0.5f;
                h += dt * 0.5f * sideSign;
            } else {
                h += dt * 0.5f;
            }
            if (h < 1e-4f) {
                if (oneSided) c += (1e-4f - h) * sideSign;
                h = 1e-4f;
            }
            bool nearPlane = false;
            int pAxis = 0;
            float pPos = 0.0f;
            if (boundFacePlane(pAxis, pPos) && pAxis == axis && p.rot.lengthSq() < 1e-6f) {
                if (oneSided) nearPlane = std::fabs((c + sideSign * h) - pPos) < 0.03f;
                else nearPlane = std::fabs(c + h - pPos) < 0.03f || std::fabs(c - h - pPos) < 0.03f;
            }
            if (ed.measureOn && !nearPlane) {
                float oa[3], ob[3], len = 0.0f;
                guideOuterSpan(ed.measureA, ed.measureB, 0.0015f, oa, ob, len);
                float full = h * 2.0f;
                if (len > 1e-4f && std::fabs(full - len) < 0.03f) {
                    float nh = len * 0.5f;
                    if (oneSided) c += (nh - h) * sideSign;
                    h = nh;
                }
            }
            snapFaceToPlane(p, axis, oneSided, sideSign);
        }
        revertIfDecalOff(before);
        ed.dirty = true;
    };
    auto fillEnt = [&](int axis, float dt, bool oneSided, float sideSign) {
        auto idx = geomSelected();
        if (idx.empty()) return;
        auto before = ed.entityParts;
        for (int i : idx) {
            pm::Part& p = ed.entityParts[i];
            for (int a = 0; a < 3; a++) {
                float& h = vecComp(p.half, a);
                float& c = vecComp(p.center, a);
                bool pin = oneSided && a == axis;
                if (pin) {
                    c += dt * 0.5f;
                    h += dt * 0.5f * sideSign;
                } else {
                    h += dt * 0.5f;
                }
                if (h < 0.01f) {
                    if (pin) c += (0.01f - h) * sideSign;
                    h = 0.01f;
                }
                snapFaceToPlane(p, a, pin, sideSign);
            }
        }
        revertIfDecalOff(before);
        ed.dirty = true;
    };
    auto rotateEntBy = [&](int axis, float ang) {
        auto idx = geomSelected();
        if (idx.empty() || axis < 0 || axis > 2) return;
        auto before = ed.entityParts;
        float cx, cy, cz;
        entCenter(cx, cy, cz);
        Vec3 pivot{ cx, cy, cz };
        Vec3 ax = (axis == 0) ? Vec3{ 1, 0, 0 } : (axis == 1 ? Vec3{ 0, 1, 0 } : Vec3{ 0, 0, 1 });
        for (int i : idx) {
            pm::Part& p = ed.entityParts[i];
            p.center = pivot + pm::rotateAxis(p.center - pivot, ax, ang);
            pm::composePartRot(p.rot, ax, ang);
        }
        revertIfDecalOff(before);
        ed.dirty = true;
    };
    auto rotateEnt45 = [&](int axis, float sign) {
        if (geomSelected().empty() || axis < 0 || axis > 2) return;
        pushEntUndo();
        rotateEntBy(axis, sign * 0.78539816f);
    };
    auto flipEntUV = [&](int uvAxis) {
        auto idx = geomSelected();
        if (idx.empty()) return;
        pushEntUndo();
        int bit = (uvAxis == 1) ? 2 : 1;
        for (int i : idx) ed.entityParts[i].uvFlip ^= bit;
        ed.dirty = true;
    };
    auto bindEnt = [&]() {
        auto idx = entSelected();
        if (idx.size() < 2) return;
        pushEntUndo();
        int b = 0;
        for (const pm::Part& p : ed.entityParts)
            if (p.bind >= b) b = p.bind + 1;
        for (int i : idx) ed.entityParts[i].bind = b;
        ed.dirty = true;
        markEnt(idx[0], false);
    };
    auto unbindEnt = [&]() {
        auto idx = geomSelected();
        if (idx.empty()) return;
        std::vector<int> binds;
        for (int i : idx) {
            int b = ed.entityParts[i].bind;
            if (b < 0) continue;
            bool have = false;
            for (int x : binds) if (x == b) have = true;
            if (!have) binds.push_back(b);
        }
        if (binds.empty()) return;
        pushEntUndo();
        for (pm::Part& p : ed.entityParts)
            for (int b : binds) if (p.bind == b) p.bind = -1;
        ed.dirty = true;
        markEnt(ed.entitySel, false);
    };
    auto addEntPart = [&]() {
        pushEntUndo();
        pm::Part np;
        np.half = { 0.10f, 0.06f, 0.12f };
        np.color = { 0.18f, 0.12f, 0.08f };
        np.type = 4;
        np.side = 0;
        if (entIsCloth) {
            np.kind = "cloth";
            np.color = { ed.r, ed.g, ed.b };
            np.type = 1;
            np.center = { 0.0f, 1.10f, 0.02f };
            np.name = uniquePartName("cloth");
        } else {
            np.kind = "hair";
            np.name = uniquePartName("hair");
            np.center = { 0.0f, 1.74f, 0.02f };
        }
        if (!entIsCloth && !ed.entityParts.empty() && ed.entitySel >= 0 && ed.entitySel < (int)ed.entityParts.size() &&
            pm::isHairPart(ed.entityParts[ed.entitySel])) {
            np = ed.entityParts[ed.entitySel];
            np.center.x += 0.12f;
            np.name = uniquePartName("hair");
            np.bind = -1;
        }
        ed.entityParts.push_back(np);
        markEnt((int)ed.entityParts.size() - 1, false);
        ed.dirty = true;
    };
    auto addEntDecal = [&]() {
        int host = ed.entitySel;
        if (host < 0 || host >= (int)ed.entityParts.size()) {
            for (int i = 0; i < (int)ed.entityParts.size(); i++)
                if (!pm::isDecalPart(ed.entityParts[i])) { host = i; break; }
        } else if (pm::isDecalPart(ed.entityParts[host])) {
            int h = findHostIdx(host);
            host = h;
        }
        if (host < 0 || host >= (int)ed.entityParts.size() || pm::isDecalPart(ed.entityParts[host]))
            return;
        pm::Part& hp = ed.entityParts[host];
        if (hp.name.empty()) hp.name = uniquePartName("part");
        int bestF = 4;
        float bestA = -1.0f;
        for (int f = 0; f < 6; f++) {
            int ax = f / 2;
            int ua = (ax + 1) % 3, va = (ax + 2) % 3;
            float a = 4.0f * pm::axisComp(hp.half, ua) * pm::axisComp(hp.half, va);
            if (a > bestA) { bestA = a; bestF = f; }
        }
        pushEntUndo();
        pm::Part np;
        np.color = { 1.0f, 1.0f, 1.0f };
        np.type = hp.type;
        np.side = hp.side;
        np.kind = "cutout";
        np.name = uniquePartName("tex");
        np.support = hp.name;
        np.tex = "leaf";
        np.center = hp.center;
        np.half = hp.half * 0.80f;
        np.rot = hp.rot;
        int axis = bestF / 2;
        float sgn = (bestF % 2 == 0) ? 1.0f : -1.0f;
        const float thin = 0.004f;
        vecComp(np.half, axis) = thin;
        Vec3 off{};
        vecComp(off, axis) = sgn * (pm::axisComp(hp.half, axis) + thin);
        np.center = pm::partWorldOffset(hp, off);
        ed.entityParts.push_back(np);
        if (pm::decalOnHost(ed.entityParts.back(), ed.entityParts[host]) < pm::kDecalMinCover) {
            ed.entityParts.pop_back();
            entUndo.pop_back();
            return;
        }
        markEnt((int)ed.entityParts.size() - 1, false);
        ed.dirty = true;
    };
    auto dupEnt = [&]() {
        auto idx = entSelected();
        if (idx.empty()) return;
        pushEntUndo();
        int first = (int)ed.entityParts.size();
        for (int i : idx) {
            if (!pm::canCreateDestroyPart(ed.entityParts[i])) continue;
            pm::Part np = ed.entityParts[i];
            np.bind = -1;
            if (!np.name.empty()) np.name = uniquePartName(np.name.c_str());
            float oldx = np.center.x;
            np.center.x += 0.12f;
            ed.entityParts.push_back(np);
            if (pm::isDecalPart(ed.entityParts.back())) {
                int di = (int)ed.entityParts.size() - 1;
                int h = findHostIdx(di);
                if (h < 0 || pm::decalOnHost(ed.entityParts[di], ed.entityParts[h]) < pm::kDecalMinCover)
                    ed.entityParts[di].center.x = oldx;
            }
        }
        if ((int)ed.entityParts.size() <= first) {
            entUndo.pop_back();
            return;
        }
        markEnt(first, false);
        for (int i = first + 1; i < (int)ed.entityParts.size(); i++) entMark[i] = 1;
        ed.dirty = true;
    };
    auto delEnt = [&]() {
        auto idx = entSelected();
        if (idx.empty()) return;
        std::vector<uint8_t> kill(ed.entityParts.size(), 0);
        int nKill = 0;
        for (int i : idx) {
            if (!pm::canCreateDestroyPart(ed.entityParts[i])) continue;
            kill[i] = 1;
            nKill++;
        }
        if (nKill <= 0) return;
        bool grew = true;
        while (grew) {
            grew = false;
            for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                if (kill[i] || !pm::isDecalPart(ed.entityParts[i])) continue;
                const pm::Part& d = ed.entityParts[i];
                int host = -1;
                if (!d.support.empty()) {
                    for (int j = 0; j < (int)ed.entityParts.size(); j++) {
                        if (j == i || kill[j] || pm::isDecalPart(ed.entityParts[j])) continue;
                        if (pm::partName(ed.entityParts[j]) == d.support) { host = j; break; }
                    }
                } else {
                    float bestR = 0.0f;
                    for (int j = 0; j < (int)ed.entityParts.size(); j++) {
                        if (j == i || kill[j] || pm::isDecalPart(ed.entityParts[j])) continue;
                        float r = pm::decalOnHost(d, ed.entityParts[j]);
                        if (r > bestR) { bestR = r; host = j; }
                    }
                    if (host >= 0 && bestR < pm::kDecalMinCover) host = -1;
                }
                if (host < 0) { kill[i] = 1; nKill++; grew = true; }
            }
        }
        if (nKill >= (int)ed.entityParts.size()) return;
        pushEntUndo();
        std::vector<pm::Part> kept;
        for (int i = 0; i < (int)ed.entityParts.size(); i++)
            if (!kill[i]) kept.push_back(ed.entityParts[i]);
        ed.entityParts = std::move(kept);
        entMark.clear();
        ensureEntMark();
        ed.hairVoxSel.clear();
        ed.hairCardSel.clear();
        ed.refDecalIdx = -1;
        ed.refEdge = -1;
        ed.refLineHover = -1;
        ed.dirty = true;
    };
    auto hairSelAtoms = [&]() {
        if (!ed.hairVoxSel.empty()) return ed.hairVoxSel;
        std::vector<pm::HairAtom> out;
        ensureEntMark();
        for (int i = 0; i < (int)ed.entityParts.size(); i++) {
            if (i >= (int)entMark.size() || !entMark[i]) continue;
            if (!pm::isGridHairPart(ed.entityParts[i])) continue;
            pm::hairVoxelizeClump(ed.entityParts, i, out);
        }
        return out;
    };
    auto hairSelCardAtoms = [&]() {
        if (!ed.hairCardSel.empty()) return ed.hairCardSel;
        std::vector<pm::HairAtom> out;
        ensureEntMark();
        for (int i = 0; i < (int)ed.entityParts.size(); i++) {
            if (i >= (int)entMark.size() || !entMark[i]) continue;
            if (!pm::isHairCardPart(ed.entityParts[i])) continue;
            pm::hairCardVoxelizeClump(ed.entityParts, i, out);
        }
        return out;
    };
    auto syncHairVoxMarks = [&]() {
        ensureEntMark();
        std::fill(entMark.begin(), entMark.end(), 0);
        bool any = false;
        for (int i = 0; i < (int)ed.entityParts.size(); i++) {
            if (pm::isGridHairPart(ed.entityParts[i])) {
                std::vector<pm::HairAtom> local;
                pm::voxelizeHairAtoms(ed.entityParts[i], local);
                for (const pm::HairAtom& a : local) {
                    if (!pm::hairAtomInSel(ed.hairVoxSel, a)) continue;
                    entMark[i] = 1;
                    if (!any) ed.entitySel = i;
                    any = true;
                    break;
                }
            }
            if (pm::isHairCardPart(ed.entityParts[i])) {
                std::vector<pm::HairAtom> local;
                pm::voxelizeHairCardAtoms(ed.entityParts[i], local);
                for (const pm::HairAtom& a : local) {
                    if (!pm::hairAtomInSel(ed.hairCardSel, a)) continue;
                    entMark[i] = 1;
                    if (!any) ed.entitySel = i;
                    any = true;
                    break;
                }
            }
        }
    };
    auto finishHairEdit = [&]() {
        for (pm::Part& p : ed.entityParts)
            if (p.name.empty()) p.name = uniquePartName(pm::isHairCardPart(p) ? "card" : "hair");
        syncHairVoxMarks();
        ensureEntMark();
        std::vector<int> binds;
        for (int i = 0; i < (int)ed.entityParts.size(); i++) {
            if (i >= (int)entMark.size() || !entMark[i]) continue;
            int b = ed.entityParts[i].bind;
            if (b < 0) continue;
            bool have = false;
            for (int x : binds) if (x == b) have = true;
            if (!have) binds.push_back(b);
        }
        if (!binds.empty()) {
            for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                int b = ed.entityParts[i].bind;
                if (b < 0) continue;
                for (int x : binds) if (x == b) entMark[i] = 1;
            }
        }
        ed.dirty = true;
    };
    auto doHairMerge = [&]() {
        std::vector<pm::HairAtom> vox = hairSelAtoms();
        std::vector<pm::HairAtom> cards = hairSelCardAtoms();
        if (vox.empty() && cards.empty()) return;
        auto before = ed.entityParts;
        std::vector<pm::HairAtom> afterV, afterC;
        bool okV = !vox.empty() && pm::mergeHairSelection(ed.entityParts, vox, &afterV);
        bool okC = !cards.empty() && pm::mergeHairCardSelection(ed.entityParts, cards, &afterC);
        if (!okV && !okC) {
            ed.entityParts = std::move(before);
            return;
        }
        entUndo.push_back(before);
        if (entUndo.size() > 32) entUndo.erase(entUndo.begin());
        for (pm::Part& p : ed.entityParts)
            if (p.name.empty()) p.name = uniquePartName(pm::isHairCardPart(p) ? "card" : "hair");
        ed.hairVoxSel.clear();
        ed.hairCardSel.clear();
        ed.hairSelDrag = false;
        entMark.clear();
        ed.entitySel = -1;
        ensureEntMark();
        ed.dirty = true;
    };
    auto doHairSplit = [&]() {
        std::vector<pm::HairAtom> vox = hairSelAtoms();
        std::vector<pm::HairAtom> cards = hairSelCardAtoms();
        if (vox.empty() && cards.empty()) return;
        auto before = ed.entityParts;
        std::vector<pm::HairAtom> afterV, afterC;
        bool okV = !vox.empty() && pm::splitHairSelection(ed.entityParts, vox, &afterV);
        bool okC = !cards.empty() && pm::splitHairCardSelection(ed.entityParts, cards, &afterC);
        if (!okV && !okC) {
            ed.entityParts = std::move(before);
            return;
        }
        entUndo.push_back(before);
        if (entUndo.size() > 32) entUndo.erase(entUndo.begin());
        ed.hairVoxSel = std::move(afterV);
        ed.hairCardSel = std::move(afterC);
        finishHairEdit();
    };
    scanEntFiles();
    loadEntityFile("player");

    anim::Clip editClip;
    hold::File editHold;
    std::vector<std::string> animNames;
    std::vector<pm::Part> animBindParts;
    std::string animBindName = "player";
    const uint8_t kHoldItems[7] = { 0, HAND_AXE, HAND_PICK, HAND_SHOVEL, SHEARS, STICK, PLANKS };
    auto holdItemName = [&]() -> std::string {
        if (ed.animHoldItem == 0) return "*";
        return hold::itemNameOf(ed.animHoldItem);
    };
    auto holdSideName = [&]() -> std::string {
        return (ed.animHoldSide < 0) ? "left" : "right";
    };
    auto loadHoldFile = [&]() {
        std::string stem = animBindName.empty() ? "player" : animBindName;
        editHold = hold::load(pack::holdFile(stem).c_str());
        if (editHold.rows.empty()) editHold = hold::defaults(stem);
        editHold.rig = stem;
        ed.animHoldDirty = false;
    };
    auto saveHoldFile = [&]() {
        std::string stem = editHold.rig.empty() ? (animBindName.empty() ? "player" : animBindName) : editHold.rig;
        editHold.rig = stem;
        hold::save(pack::holdFile(stem).c_str(), editHold);
        ed.animHoldDirty = false;
    };
    // Selected side is the main-hand preview. If that side has no item row, reuse the
    // other side's grasp and retarget the palm so the tool actually moves hands.
    auto previewHoldSpec = [&]() {
        std::string side = holdSideName();
        std::string item = holdItemName();
        auto itemRow = [&](const std::string& sd) {
            for (const hold::Spec& s : editHold.rows) {
                if (hold::isWild(s.item) || s.item != item) continue;
                if (!hold::isWild(s.side) && s.side != sd) continue;
                if (!hold::isWild(s.clip) && s.clip != editClip.name) continue;
                return true;
            }
            return false;
        };
        auto finish = [&](hold::Spec spec) {
            if (spec.bone.empty()) spec.bone = hold::defaultBone(side);
            spec.side = side;
            return spec;
        };
        if (itemRow(side) || hold::isWild(item))
            return finish(hold::resolve(editHold, item, editClip.name, side));
        std::string other = (side == "left") ? "right" : "left";
        if (itemRow(other)) {
            hold::Spec spec = hold::resolve(editHold, item, editClip.name, other);
            const char* from = (other == "left") ? "arm_l_" : "arm_r_";
            const char* to = (side == "left") ? "arm_l_" : "arm_r_";
            if (spec.bone.rfind(from, 0) == 0) spec.bone.replace(0, 6, to);
            else spec.bone = hold::defaultBone(side);
            return finish(spec);
        }
        return finish(hold::resolve(editHold, item, editClip.name, side));
    };
    auto editableHoldSpec = [&]() -> hold::Spec& {
        hold::Spec s = previewHoldSpec();
        s.item = holdItemName();
        s.clip = editClip.name.empty() ? "*" : editClip.name;
        s.side = holdSideName();
        if (s.bone.empty()) s.bone = hold::defaultBone(s.side);
        return hold::upsert(editHold, s);
    };
    // Main L previews the clip with the arms exchanged. Local +X is the same
    // direction on both arms, so the swap also mirrors X translation and Ry/Rz.
    auto mirrorArmClip = [](anim::Clip c) {
        for (anim::Track& tr : c.tracks) {
            bool right = tr.bone.rfind("arm_r_", 0) == 0;
            bool left = tr.bone.rfind("arm_l_", 0) == 0;
            if (!right && !left) continue;
            tr.bone = std::string(right ? "arm_l_" : "arm_r_") + tr.bone.substr(6);
            for (anim::Key& k : tr.keys) {
                k.t.x = -k.t.x;
                k.r.y = -k.r.y;
                k.r.z = -k.r.z;
            }
        }
        return c;
    };
    auto previewClipNow = [&]() {
        if (ed.animHoldSide >= 0) return editClip;
        return mirrorArmClip(editClip);
    };
    auto storeViewClip = [&](anim::Clip view) {
        hold::Spec spec = previewHoldSpec();
        if (ed.animHoldSide < 0) hold::mirrorPalmX(spec);
        hold::recordTouches(view, spec);
        if (ed.animHoldSide < 0) view = mirrorArmClip(std::move(view));
        editClip.tracks = std::move(view.tracks);
        editClip.toolTurns = std::move(view.toolTurns);
    };
    auto viewBoneName = [&](const std::string& name) {
        if (ed.animHoldSide >= 0) return name;
        if (name.rfind("arm_r_", 0) == 0) return "arm_l_" + name.substr(6);
        if (name.rfind("arm_l_", 0) == 0) return "arm_r_" + name.substr(6);
        return name;
    };
    // Arm swap mirrors the pose (M*R*M). The grasp was authored on the right palm,
    // so the preview also flips local X or the pick stays right-handed.
    auto previewToolSpec = [&]() {
        hold::Spec spec = previewHoldSpec();
        if (ed.animHoldSide < 0) hold::mirrorPalmX(spec);
        return spec;
    };
    // Between keyframes the off-hand stays on the stored hold_touch points.
    auto applyHoldContact = [&](const anim::Clip& view, std::vector<anim::BoneXform>& pose, float frame) {
        hold::Spec spec = previewToolSpec();
        hold::keepToolContact(view, pose, view, frame, spec);
    };
    auto toolSpecAt = [&](const anim::Clip& view, float frame) {
        hold::Spec spec = previewToolSpec();
        hold::composeFace(spec, anim::evalFace(view, frame), anim::evalFaceOff(view, frame));
        return spec;
    };
    auto plantTool = [&](anim::Clip& view, const anim::BoneXform& palm0, const float toolR[9], const Vec3& grip0) {
        if (!ed.animLockDir && !ed.animLockPos) return;
        std::string sel = (ed.animSelBone >= 0 && ed.animSelBone < (int)view.bones.size())
            ? view.bones[ed.animSelBone].name : std::string{};
        hold::Spec base = previewToolSpec();
        std::string prefix = (base.bone.rfind("arm_l_", 0) == 0) ? "arm_l_" : "arm_r_";
        if (sel.rfind(prefix, 0) != 0) return;
        auto pose = anim::evalPose(view, (float)ed.animFrame);
        anim::BoneXform palm{};
        if (!anim::boneXformOf(view, pose, base.bone, palm)) return;
        Vec3 faceR = anim::evalFace(view, (float)ed.animFrame);
        Vec3 faceT = anim::evalFaceOff(view, (float)ed.animFrame);
        if (ed.animLockDir) {
            float Rh[9], Rt[9], Rp[9], tmp[9], faceNew[9];
            pm::eulerToMat(base.rot, Rh);
            anim::transpose9(palm.R, Rp);
            anim::transpose9(Rh, Rt);
            pm::mat3Mul(Rp, toolR, tmp);
            pm::mat3Mul(Rt, tmp, faceNew);
            faceR = pm::matToEuler(faceNew);
        }
        if (ed.animLockPos) {
            float Rp[9];
            anim::transpose9(palm.R, Rp);
            faceT = anim::mul9(Rp, grip0 - palm.pivot) - base.offset;
        }
        anim::setFaceKey(view, ed.animFrame, faceT, faceR);
    };
    auto poseShown = [&](const anim::Clip& view, float frame) {
        auto pose = anim::evalPose(view, frame);
        hold::applyToolAxis(view, pose, frame, previewToolSpec(), [&](float f) {
            return anim::evalPose(view, f);
        });
        applyHoldContact(view, pose, frame);
        return pose;
    };
    auto captureTool = [&](const anim::Clip& view, anim::BoneXform& palm0, float toolR[9], Vec3& grip0) -> bool {
        hold::Spec spec = toolSpecAt(view, (float)ed.animFrame);
        spec.grip = anim::evalGrip(view, (float)ed.animFrame, spec.grip);
        auto pose = poseShown(view, (float)ed.animFrame);
        if (!anim::boneXformOf(view, pose, spec.bone, palm0)) return false;
        grip0 = hold::pointOnBone(spec, palm0, spec.grip.x, spec.grip.y, spec.grip.z);
        float Rh[9];
        pm::eulerToMat(spec.rot, Rh);
        pm::mat3Mul(palm0.R, Rh, toolR);
        return true;
    };
    struct AnimSnap {
        anim::Clip clip;
        hold::File hold;
        std::vector<pm::Part> parts;
        int frame = 0;
        float clock = 0.0f;
        int selBone = 0;
        bool dirty = false;
        bool holdDirty = false;
        bool modelDirty = false;
    };
    std::vector<AnimSnap> animUndo;
    struct PoseCopy {
        bool has = false;
        std::vector<std::string> bones;
        std::vector<anim::Key> keys;
        Vec3 grip{};
        Vec3 face{};
        Vec3 faceOff{};
    };
    PoseCopy poseCopy;
    // Repeated S-/S+ clicks rescale from this copy so rounding does not drift.
    static bool scaleHoldOn = false;
    static anim::Clip scaleHoldClip;
    static int scaleHoldLen = 0, scaleHoldTarget = 0, scaleHoldFrame = 0;
    static int scaleHoldSpanA = -1, scaleHoldSpanB = -1;
    static float scaleHoldClock = 0.0f;
    auto pushAnimUndo = [&]() {
        scaleHoldOn = false;
        AnimSnap s;
        s.clip = editClip;
        s.hold = editHold;
        s.parts = animBindParts;
        s.frame = ed.animFrame;
        s.clock = ed.animClock;
        s.selBone = ed.animSelBone;
        s.dirty = ed.animDirty;
        s.holdDirty = ed.animHoldDirty;
        s.modelDirty = ed.animModelDirty;
        animUndo.push_back(std::move(s));
        if (animUndo.size() > 48) animUndo.erase(animUndo.begin());
    };
    auto undoAnim = [&]() {
        scaleHoldOn = false;
        if (animUndo.empty()) return;
        AnimSnap s = std::move(animUndo.back());
        animUndo.pop_back();
        editClip = std::move(s.clip);
        editHold = std::move(s.hold);
        if (!s.parts.empty()) animBindParts = std::move(s.parts);
        ed.animFrame = s.frame;
        ed.animClock = s.clock;
        ed.animSelBone = s.selBone;
        if (ed.animSelBone < 0) ed.animSelBone = 0;
        if (!editClip.bones.empty() && ed.animSelBone >= (int)editClip.bones.size())
            ed.animSelBone = (int)editClip.bones.size() - 1;
        ed.animDirty = s.dirty;
        ed.animHoldDirty = s.holdDirty;
        ed.animModelDirty = s.modelDirty;
        ed.animPlaying = false;
    };
    auto scanAnimFiles = [&]() {
        animNames.clear();
        std::error_code ec;
        std::filesystem::create_directories(pack::animsDir(), ec);
        for (const auto& e : std::filesystem::directory_iterator(pack::animsDir(), ec)) {
            if (!e.is_regular_file()) continue;
            auto p = e.path();
            if (p.extension() == ".animation") animNames.push_back(p.stem().string());
        }
        std::sort(animNames.begin(), animNames.end());
    };
    auto loadAnimBind = [&](const std::string& name) {
        std::string stem = name.empty() ? "player" : name;
        const plugin::EntityModule* mod = plugin::findEntity(stem.c_str());
        if (mod) stem = mod->appearance.empty() ? mod->id : mod->appearance;
        pm::EntityFile ef = pm::loadEntity(pack::entityModel(stem).c_str());
        pm::fillEntityDefaults(ef);
        animBindParts = std::move(ef.parts);
        animBindName = stem;
        editClip.rig = stem;
        ed.animModelDirty = false;
        anim::rebindPivots(editClip, animBindParts);
        loadHoldFile();
    };
    auto loadAnimClip = [&](const std::string& stem) {
        anim::Clip c = anim::load(pack::animationFile(stem).c_str());
        if (c.bones.empty()) {
            if (animBindParts.empty()) loadAnimBind("player");
            c = anim::rigFromParts(animBindParts, animBindName);
            c.name = stem;
        }
        if (c.name.empty()) c.name = stem;
        editClip = std::move(c);
        ed.animFrame = 0;
        ed.animClock = 0;
        ed.animSpanA = -1;
        ed.animSpanB = -1;
        ed.animPlaying = false;
        ed.animSelBone = 0;
        ed.animDirty = false;
        animUndo.clear();
        if (!editClip.rig.empty()) loadAnimBind(editClip.rig);
        else if (animBindParts.empty()) loadAnimBind("player");
        if (ed.animSelBone >= (int)editClip.bones.size()) ed.animSelBone = 0;
        anim::ensurePalmBones(editClip, animBindParts);
    };
    auto saveAnimClip = [&]() {
        std::error_code ec;
        std::filesystem::create_directories(pack::animsDir(), ec);
        std::string stem = editClip.name.empty() ? "clip" : editClip.name;
        anim::save(pack::animationFile(stem).c_str(), editClip);
        ed.animDirty = false;
        scanAnimFiles();
    };
    auto saveAnimModel = [&]() {
        std::string stem = animBindName.empty() ? "player" : animBindName;
        std::string path = pack::entityModel(stem);
        pm::EntityFile ef = pm::loadEntity(path.c_str());
        ef.parts = animBindParts;
        if (ef.parts.empty()) return;
        pm::saveEntity(path.c_str(), ef);
        ed.animModelDirty = false;
    };
    auto uniqueAnimName = [&](const std::string& base) {
        auto used = [&](const std::string& s) {
            for (const std::string& n : animNames) if (n == s) return true;
            return false;
        };
        if (!used(base)) return base;
        for (int i = 2; i < 1000; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s_%d", base.c_str(), i);
            if (!used(buf)) return std::string(buf);
        }
        return base + "_x";
    };
    auto newAnimClip = [&]() {
        if (animBindParts.empty()) loadAnimBind("player");
        editClip = anim::makeIdle(anim::rigFromParts(animBindParts, animBindName));
        editClip.name = uniqueAnimName("clip");
        saveAnimClip();
    };
    auto dupAnimClip = [&]() {
        std::string src = editClip.name.empty() ? "clip" : editClip.name;
        editClip.name = uniqueAnimName(src);
        saveAnimClip();
    };
    auto delAnimClip = [&]() {
        if (editClip.name.empty()) return;
        std::error_code ec;
        std::filesystem::remove(pack::animationFile(editClip.name), ec);
        scanAnimFiles();
        if (!animNames.empty()) loadAnimClip(animNames[0]);
        else newAnimClip();
    };
    auto enterAnimMode = [&]() {
        ed.animMode = true;
        scanAnimFiles();
        if (animBindParts.empty()) loadAnimBind("player");
        if (animNames.empty()) {
            anim::Clip rig = anim::rigFromParts(animBindParts, animBindName);
            anim::Clip idle = anim::makeIdle(rig);
            idle.name = "idle";
            anim::save(pack::animationFile("idle").c_str(), idle);
            anim::Clip walk = anim::makeWalk(rig);
            walk.name = "walk";
            anim::save(pack::animationFile("walk").c_str(), walk);
            scanAnimFiles();
        }
        loadAnimClip(animNames.empty() ? "idle" : animNames[0]);
        loadHoldFile();
    };
    scanAnimFiles();
    auto clearEditorTools = [&]() {
        ed.tool = -1;
        ed.picker = false;
        ed.brushMod = false;
        ed.justPicked = false;
        ed.dragging = false;
        ed.modelTool = -1;
        ed.gizmoHover = 0;
        ed.refTool = false;
        ed.refDecalIdx = -1;
        ed.refEdge = -1;
        ed.refLineHover = -1;
        ed.hairPaint = false;
        ed.hairErase = false;
        ed.hairSelect = false;
        ed.hairCard = false;
        ed.hairSelDrag = false;
        ed.hairVoxSel.clear();
        ed.hairCardSel.clear();
    };
    auto ensureBrushMat = [&]() {
        if ((int)ed.brushColors.size() < 16) ed.brushColors.resize(16, Color4{});
        if ((int)ed.brushSet.size() < 16) ed.brushSet.resize(16, 0);
        if ((int)ed.brushSel.size() < 16) ed.brushSel.resize(16, 0);
    };
    auto compactBrushMat = [&]() {
        ensureBrushMat();
        int w = 0;
        for (int i = 0; i < 16; i++) {
            if (!ed.brushSet[i]) continue;
            if (w != i) {
                ed.brushColors[w] = ed.brushColors[i];
                ed.brushSet[w] = 1;
            }
            w++;
        }
        for (int i = w; i < 16; i++) {
            ed.brushColors[i] = Color4{};
            ed.brushSet[i] = 0;
        }
        for (uint8_t& s : ed.brushSel) s = 0;
    };
    auto brushColorEq = [](const Color4& a, const Color4& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };
    auto addBrushColor = [&](Color4 c) -> bool {
        ensureBrushMat();
        if (!ed.pickAlpha0 && c.a == 0) return false;
        for (int i = 0; i < 16; i++)
            if (ed.brushSet[i] && brushColorEq(ed.brushColors[i], c)) return false;
        for (int i = 0; i < 16; i++) {
            if (!ed.brushSet[i]) {
                ed.brushColors[i] = c;
                ed.brushSet[i] = 1;
                return true;
            }
        }
        return false;
    };
    auto brushFilledCount = [&]() {
        ensureBrushMat();
        int n = 0;
        for (int i = 0; i < 16; i++) if (ed.brushSet[i]) n++;
        return n;
    };

    auto randPathFor = [](int b) {
        return pack::blockRand(mat::blockName(b));
    };
    auto applyRand = [&]() {
        ed.randWx += 3;
        ed.randWz += 17;
    };
    auto ensureRandDefaults = [&]() {
        for (int i = 0; i < kNRandSliders; i++) {
            if (editRand.f.find(kRandSliders[i].key) == editRand.f.end())
                editRand.f[kRandSliders[i].key] = kRandSliders[i].def;
        }
    };
    mat::Image itemSheet, savedItemSheet;
    unsigned itemSheetGL = 0;
    bool itemSheetDirty = false;
    std::vector<std::vector<uint8_t>> itemSheetUndo;
    auto snapshotSaved = [&]() {
        savedModel = editModel;
        savedRand = editRand;
        savedItemSheet = itemSheet;
    };
    struct PaintSnap { std::string name; std::vector<uint8_t> rgba; };
    std::vector<PaintSnap> itemPaintUndo;
    bool paintDirty = false;
    auto imageByName = [&](const std::string& name, unsigned& glTex) -> mat::Image* {
        glTex = 0;
        int ti = mat::tileIndex(name.c_str());
        if (ti >= 0 && ti < TEX_COUNT) {
            glTex = tileGL[ti];
            return &mat::g_tileImages[ti];
        }
        for (ExtraMat& e : extraMats) if (e.name == name) { glTex = e.tex; return &e.img; }
        return nullptr;
    };
    auto publishPaint = [&](const std::string& name, mat::Image* img, unsigned glTex) {
        if (!img) return;
        if (mat::tileIndex(name.c_str()) >= 0) syncTilesToGL();
        else if (glTex) uploadImgPixels(glTex, *img);
    };
    auto revertPaint = [&]() {
        std::vector<std::string> done;
        for (const PaintSnap& s : itemPaintUndo) {
            bool seen = false;
            for (const std::string& d : done) if (d == s.name) { seen = true; break; }
            if (seen) continue;
            done.push_back(s.name);
            unsigned glt = 0;
            mat::Image* img = imageByName(s.name, glt);
            if (img && img->rgba.size() == s.rgba.size()) img->rgba = s.rgba;
            publishPaint(s.name, img, glt);
        }
        itemPaintUndo.clear();
        paintDirty = false;
    };
    auto savePaintedImages = [&]() {
        std::vector<std::string> done;
        for (const PaintSnap& s : itemPaintUndo) {
            bool seen = false;
            for (const std::string& d : done) if (d == s.name) { seen = true; break; }
            if (seen) continue;
            done.push_back(s.name);
            unsigned glt = 0;
            mat::Image* img = imageByName(s.name, glt);
            if (!img || !img->ok()) continue;
            int ti = mat::tileIndex(s.name.c_str());
            std::string path = (ti >= 0) ? pack::tilePng(s.name) : (kExtrasDir + "/" + s.name + ".png");
            mat::savePNG(path.c_str(), img->w, img->h, img->rgba.data());
            publishPaint(s.name, img, glt);
        }
        itemPaintUndo.clear();
        paintDirty = false;
    };
    auto itemPaintPath = [&]() {
        return kExtrasDir + "/" + mat::blockName(ed.modelBlock) + "_paint.png";
    };
    auto uploadItemSheet = [&]() {
        if (!itemSheet.ok()) return;
        if (!itemSheetGL) itemSheetGL = uploadImgTex(itemSheet);
        else uploadImgPixels(itemSheetGL, itemSheet);
        gl::BindTexture(GL_TEXTURE_2D, itemSheetGL);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl::BindTexture(GL_TEXTURE_2D, 0);
    };
    auto sheetPow2 = [](int n) {
        int p = 32;
        while (p < n && p < 1024) p <<= 1;
        return p < n ? n : p;
    };
    auto sheetTexelN = [](float full) {
        int n = (int)std::lround(std::fabs(full) / (mat::kSolidGrid / 16.0f));
        if (n < 1) n = 1;
        if (n > 48) n = 48;
        return n;
    };
    auto paintIsland = [&](const mat::Solid& s) {
        if (!mat::solidHasBox(s) || !itemSheet.ok()) return;
        uint8_t r = (uint8_t)std::lround(std::clamp(s.rgb[0], 0.0f, 1.0f) * 255.0f);
        uint8_t g = (uint8_t)std::lround(std::clamp(s.rgb[1], 0.0f, 1.0f) * 255.0f);
        uint8_t b = (uint8_t)std::lround(std::clamp(s.rgb[2], 0.0f, 1.0f) * 255.0f);
        for (int f = 0; f < 6; f++) {
            int x, y, fw, fh;
            pm::boxFacePx(s.boxX, s.boxY, s.boxW, s.boxH, s.boxD, f, x, y, fw, fh);
            for (int yy = y; yy < y + fh; yy++) for (int xx = x; xx < x + fw; xx++) {
                if (xx < 0 || yy < 0 || xx >= itemSheet.w || yy >= itemSheet.h) continue;
                size_t i = ((size_t)yy * (size_t)itemSheet.w + (size_t)xx) * 4;
                itemSheet.rgba[i] = r; itemSheet.rgba[i + 1] = g;
                itemSheet.rgba[i + 2] = b; itemSheet.rgba[i + 3] = 255;
            }
        }
    };
    auto growItemSheet = [&](int needW, int needH) {
        int W = sheetPow2(std::max(needW, 1));
        int H = sheetPow2(std::max(needH, 1));
        if (itemSheet.ok() && itemSheet.w >= W && itemSheet.h >= H) return;
        int nw = sheetPow2(std::max(W, itemSheet.ok() ? itemSheet.w : 0));
        int nh = sheetPow2(std::max(H, itemSheet.ok() ? itemSheet.h : 0));
        mat::Image n;
        n.w = nw; n.h = nh;
        n.rgba.assign((size_t)nw * (size_t)nh * 4, 255);
        for (size_t i = 0; i + 3 < n.rgba.size(); i += 4) {
            n.rgba[i] = 48; n.rgba[i + 1] = 48; n.rgba[i + 2] = 52;
        }
        if (itemSheet.ok()) {
            for (int y = 0; y < itemSheet.h && y < nh; y++) {
                const uint8_t* src = itemSheet.rgba.data() + (size_t)y * (size_t)itemSheet.w * 4;
                uint8_t* dst = n.rgba.data() + (size_t)y * (size_t)nw * 4;
                for (int x = 0; x < itemSheet.w * 4; x++) dst[x] = src[x];
            }
        }
        itemSheet = std::move(n);
    };
    auto loadItemSheet = [&]() {
        itemSheetUndo.clear();
        itemSheetDirty = false;
        itemSheet = mat::loadPNG(itemPaintPath().c_str());
        int usedW = 1, usedH = 1;
        bool any = false;
        for (const mat::Solid& s : editModel.solids) {
            if (!mat::solidHasBox(s)) continue;
            any = true;
            int r = s.boxX + 2 * s.boxD + 2 * s.boxW;
            int b = s.boxY + s.boxD + s.boxH;
            if (r > usedW) usedW = r;
            if (b > usedH) usedH = b;
        }
        if (any) {
            bool fresh = !itemSheet.ok();
            growItemSheet(usedW, usedH);
            if (fresh) for (const mat::Solid& s : editModel.solids) paintIsland(s);
        }
        uploadItemSheet();
    };
    auto claimSolidSheet = [&](mat::Solid& s) {
        if (!s.tex.empty()) { s.tex.clear(); ed.dirty = true; }
        if (mat::solidHasBox(s)) {
            growItemSheet(s.boxX + 2 * s.boxD + 2 * s.boxW, s.boxY + s.boxD + s.boxH);
            uploadItemSheet();
            return;
        }
        int w = sheetTexelN(s.h[0] * 2.0f);
        int h = sheetTexelN(s.h[1] * 2.0f);
        int d = sheetTexelN(s.h[2] * 2.0f);
        int usedH = 0;
        for (const mat::Solid& o : editModel.solids) {
            if (!mat::solidHasBox(o)) continue;
            int b = o.boxY + o.boxD + o.boxH;
            if (b > usedH) usedH = b;
        }
        s.boxX = 0;
        s.boxY = usedH > 0 ? usedH + 1 : 0;
        s.boxW = w; s.boxH = h; s.boxD = d;
        growItemSheet(2 * d + 2 * w, s.boxY + d + h);
        paintIsland(s);
        uploadItemSheet();
        ed.dirty = true;
    };
    auto saveRandFile = [&]() {
        mat::saveRand(randPathFor(ed.modelBlock).c_str(), editRand);
        std::string p = pack::blockModel(mat::blockName(ed.modelBlock));
        mat::saveModel(p.c_str(), editModel);
        mat::storeItemModel((uint8_t)ed.modelBlock, editModel);
        if (paintDirty) savePaintedImages();
        if (itemSheet.ok() && itemSheetDirty) {
            mat::savePNG(itemPaintPath().c_str(), itemSheet.w, itemSheet.h, itemSheet.rgba.data());
            itemSheetDirty = false;
        }
        snapshotSaved();
        ed.dirty = false;
    };
    auto cancelChanges = [&]() {
        if (paintDirty) revertPaint();
        editModel = savedModel;
        editRand = savedRand;
        itemSheet = savedItemSheet;
        itemSheetUndo.clear();
        itemSheetDirty = false;
        uploadItemSheet();
        if (ed.selQuad >= (int)editModel.quads.size()) ed.selQuad = (int)editModel.quads.size() - 1;
        if (ed.selQuad < 0) ed.selQuad = 0;
        selMark.clear();
        solidMark.clear();
        if (ed.selSolid >= (int)editModel.solids.size()) ed.selSolid = -1;
        ed.dirty = false;
    };

    std::vector<mat::Model> modelUndo;
    auto pushModelUndo = [&]() {
        modelUndo.push_back(editModel);
        if (modelUndo.size() > 32) modelUndo.erase(modelUndo.begin());
    };
    auto undoModel = [&]() {
        if (modelUndo.empty()) return;
        editModel = modelUndo.back();
        modelUndo.pop_back();
        if (ed.selQuad >= (int)editModel.quads.size()) ed.selQuad = (int)editModel.quads.size() - 1;
        if (ed.selQuad < 0) ed.selQuad = 0;
        selMark.clear();
        solidMark.clear();
        if (ed.selSolid >= (int)editModel.solids.size()) ed.selSolid = -1;
        ed.dirty = true;
    };
    auto undoModelOrPaint = [&]() {
        if (ed.modelRightTab == 1 && !itemSheetUndo.empty()) {
            if (itemSheet.ok() && itemSheetUndo.back().size() == itemSheet.rgba.size()) {
                itemSheet.rgba = itemSheetUndo.back();
                itemSheetUndo.pop_back();
                uploadItemSheet();
                ed.dirty = true;
                if (itemSheetUndo.empty()) itemSheetDirty = false;
                return;
            }
            itemSheetUndo.pop_back();
        }
        undoModel();
    };
    auto defaultTexFor = [&](int b) -> std::string {
        if (b == GRASS_TUFT) return "grass_tuft";
        return mat::tileName(blockOf(b).texSide);
    };
    auto applyCropUV = [](mat::Quad& q) {
        if (!q.crop) return;
        float s = mat::quadTexScale(q);
        int f = q.face;
        for (int c = 0; c < 4; c++) {
            float x = q.p[c][0] / s, y = q.p[c][1] / s, z = q.p[c][2] / s;
            if (f == 0 || f == 1) { q.uv[c][0] = x; q.uv[c][1] = z; }
            else if (f == 2 || f == 3) { q.uv[c][0] = z; q.uv[c][1] = 1.0f / s - y; }
            else { q.uv[c][0] = x; q.uv[c][1] = 1.0f / s - y; }
        }
    };
    auto applyFillUV = [](mat::Quad& q) {
        Vec3 p0{ q.p[0][0], q.p[0][1], q.p[0][2] };
        Vec3 p1{ q.p[1][0], q.p[1][1], q.p[1][2] };
        Vec3 p3{ q.p[3][0], q.p[3][1], q.p[3][2] };
        Vec3 n = (p1 - p0).cross(p3 - p0);
        float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        float s = mat::quadTexScale(q);
        for (int c = 0; c < 4; c++) {
            float x = q.p[c][0] / s, y = q.p[c][1] / s, z = q.p[c][2] / s;
            if (ay >= ax && ay >= az) { q.uv[c][0] = x; q.uv[c][1] = z; }
            else if (ax >= az) { q.uv[c][0] = z; q.uv[c][1] = 1.0f / s - y; }
            else { q.uv[c][0] = x; q.uv[c][1] = 1.0f / s - y; }
        }
    };
    auto faceAreaOf = [](const mat::Quad& q) {
        Vec3 a{ q.p[0][0], q.p[0][1], q.p[0][2] };
        Vec3 b{ q.p[1][0], q.p[1][1], q.p[1][2] };
        Vec3 d{ q.p[3][0], q.p[3][1], q.p[3][2] };
        return (b - a).cross(d - a).length();
    };
    auto applyFillBySize = [&](mat::Quad& q) {
        q.uvLock = false;
        float a = faceAreaOf(q);
        q.uvMode = (fillBaseArea > 1e-8f && a + 1e-6f < fillBaseArea) ? 2 : 1;
        applyFillUV(q);
    };
    auto refreshUV = [&](mat::Quad& q) {
        if (q.crop) applyCropUV(q);
        else if (q.uvMode >= 1 && !q.uvLock) applyFillUV(q);
    };
    auto lockFillUV = [&](mat::Quad& q) {
        if (q.crop || q.uvMode < 1) return;
        applyFillUV(q);
        q.uvLock = true;
    };
    auto objectId = [&](int qi) -> long long {
        if (qi < 0 || qi >= (int)editModel.quads.size()) return -1;
        const mat::Quad& q = editModel.quads[qi];
        if (q.bind >= 0) return 1000000LL + q.bind;
        if (q.group >= 0) return 2000000LL + q.group;
        return qi;
    };
    auto ensureSelMark = [&]() {
        if ((int)selMark.size() != (int)editModel.quads.size())
            selMark.assign(editModel.quads.size(), 0);
    };
    auto markObject = [&](int qi, bool add) {
        ensureSelMark();
        ed.selSolid = -1;
        if (qi < 0 || qi >= (int)editModel.quads.size()) return;
        long long id = objectId(qi);
        if (add && keyDown(VK_CONTROL) && !keyDown(VK_SHIFT)) {
            bool on = false;
            for (int i = 0; i < (int)editModel.quads.size(); i++)
                if (objectId(i) == id && selMark[i]) { on = true; break; }
            if (on) {
                for (int i = 0; i < (int)editModel.quads.size(); i++)
                    if (objectId(i) == id) selMark[i] = 0;
                if (objectId(ed.selQuad) == id) {
                    ed.selQuad = 0;
                    for (int i = 0; i < (int)selMark.size(); i++)
                        if (selMark[i]) { ed.selQuad = i; break; }
                }
                return;
            }
        }
        if (!add) std::fill(selMark.begin(), selMark.end(), 0);
        for (int i = 0; i < (int)editModel.quads.size(); i++)
            if (objectId(i) == id) selMark[i] = 1;
        ed.selQuad = qi;
        if (!editModel.quads[qi].tex.empty()) ed.selMat = editModel.quads[qi].tex;
    };
    auto selectedIndices = [&]() {
        ensureSelMark();
        std::vector<int> out;
        bool any = false;
        for (int i = 0; i < (int)selMark.size(); i++) if (selMark[i]) { any = true; break; }
        if (!any && !editModel.quads.empty()) markObject(ed.selQuad, false);
        for (int i = 0; i < (int)selMark.size(); i++) if (selMark[i]) out.push_back(i);
        return out;
    };
    auto objectReps = [&]() {
        std::vector<int> reps;
        std::vector<long long> seen;
        for (int i = 0; i < (int)editModel.quads.size(); i++) {
            long long id = objectId(i);
            bool have = false;
            for (long long s : seen) if (s == id) { have = true; break; }
            if (!have) { seen.push_back(id); reps.push_back(i); }
        }
        return reps;
    };
    auto isDeformableFace = [&](int qi) {
        if (qi < 0 || qi >= (int)editModel.quads.size()) return false;
        const mat::Quad& q = editModel.quads[qi];
        if (q.solid || q.crop || q.group >= 0) return false; // small / intact block: move as a whole
        return true;
    };
    auto isSolidGroup = [&](int qi) {
        if (qi < 0 || qi >= (int)editModel.quads.size()) return false;
        const mat::Quad& q = editModel.quads[qi];
        return q.crop && q.solid && q.group >= 0;
    };
    auto boundsOfIdx = [&](const std::vector<int>& idx, float mn[3], float mx[3]) -> bool {
        bool any = false;
        for (int i : idx) {
            if (i < 0 || i >= (int)editModel.quads.size()) continue;
            const mat::Quad& q = editModel.quads[i];
            for (int c = 0; c < 4; c++) {
                for (int k = 0; k < 3; k++) {
                    float v = q.p[c][k];
                    if (!any) { mn[k] = mx[k] = v; }
                    else {
                        if (v < mn[k]) mn[k] = v;
                        if (v > mx[k]) mx[k] = v;
                    }
                }
            }
            any = true;
        }
        return any;
    };
    auto stretchSolidSel = [&](int axis, float dt, bool oneSided, float sideSign) {
        if (axis < 0 || axis > 2) return;
        auto idx = selectedIndices();
        float mn[3], mx[3];
        if (!boundsOfIdx(idx, mn, mx)) return;
        float nmin = mn[axis], nmax = mx[axis];
        if (oneSided) {
            if (sideSign >= 0.0f) nmax += dt;
            else nmin += dt;
        } else {
            nmin -= dt * 0.5f;
            nmax += dt * 0.5f;
        }
        if (nmax - nmin < 0.03125f) {
            float mid = 0.5f * (nmin + nmax);
            nmin = mid - 0.015625f;
            nmax = mid + 0.015625f;
        }
        float oldS = mx[axis] - mn[axis];
        if (oldS < 1e-8f) return;
        float newS = nmax - nmin;
        for (int i : idx) {
            mat::Quad& q = editModel.quads[i];
            for (int c = 0; c < 4; c++) {
                float t = (q.p[c][axis] - mn[axis]) / oldS;
                q.p[c][axis] = nmin + t * newS;
            }
            refreshUV(q);
        }
        ed.dirty = true;
    };
    auto snapshotFillBase = [&]() {
        fillBaseArea = 0.0f;
        if (ed.selQuad >= 0 && ed.selQuad < (int)editModel.quads.size() && isDeformableFace(ed.selQuad))
            fillBaseArea = faceAreaOf(editModel.quads[ed.selQuad]);
    };
    auto faceNormalOf = [](const mat::Quad& q) {
        Vec3 p0{ q.p[0][0], q.p[0][1], q.p[0][2] };
        Vec3 p1{ q.p[1][0], q.p[1][1], q.p[1][2] };
        Vec3 p3{ q.p[3][0], q.p[3][1], q.p[3][2] };
        return (p1 - p0).cross(p3 - p0).normalized();
    };
    auto nearestWorldAxis = [](Vec3 n) {
        float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        Vec3 a = (ay >= ax && ay >= az) ? Vec3{ 0, 1, 0 } : (ax >= az ? Vec3{ 1, 0, 0 } : Vec3{ 0, 0, 1 });
        if (n.dot(a) < 0) a = a * -1.0f;
        return a;
    };
    auto nextBindId = [&]() {
        int g = 0;
        for (const mat::Quad& q : editModel.quads) if (q.bind + 1 > g) g = q.bind + 1;
        for (const mat::Solid& s : editModel.solids) if (s.bind + 1 > g) g = s.bind + 1;
        return g;
    };
    auto ensureSolidMark = [&]() {
        if ((int)solidMark.size() != (int)editModel.solids.size())
            solidMark.assign(editModel.solids.size(), 0);
    };
    auto solidIds = [&]() {
        ensureSolidMark();
        std::vector<int> out;
        for (int i = 0; i < (int)solidMark.size(); i++) if (solidMark[i]) out.push_back(i);
        if (out.empty() && ed.selSolid >= 0 && ed.selSolid < (int)editModel.solids.size())
            out.push_back(ed.selSolid);
        return out;
    };
    auto selectOnlySolid = [&](int i) {
        ensureSelMark();
        ensureSolidMark();
        std::fill(selMark.begin(), selMark.end(), 0);
        std::fill(solidMark.begin(), solidMark.end(), 0);
        if (i >= 0 && i < (int)editModel.solids.size()) {
            int b = editModel.solids[i].bind;
            solidMark[i] = 1;
            ed.selSolid = i;
            if (b >= 0) {
                for (int j = 0; j < (int)editModel.solids.size(); j++)
                    if (editModel.solids[j].bind == b) solidMark[j] = 1;
            }
        } else ed.selSolid = -1;
    };
    auto selCenter = [&](float& cx, float& cy, float& cz) {
        auto idx = selectedIndices();
        cx = 0; cy = 0; cz = 0;
        if (idx.empty()) {
            auto ss = solidIds();
            if (ss.empty()) return;
            for (int i : ss) {
                cx += editModel.solids[i].c[0];
                cy += editModel.solids[i].c[1];
                cz += editModel.solids[i].c[2];
            }
            float n = (float)ss.size();
            cx /= n; cy /= n; cz /= n;
            return;
        }
        int n = 0;
        for (int i : idx) for (int c = 0; c < 4; c++) {
            cx += editModel.quads[i].p[c][0];
            cy += editModel.quads[i].p[c][1];
            cz += editModel.quads[i].p[c][2];
            n++;
        }
        if (n > 0) { cx /= n; cy /= n; cz /= n; }
    };
    auto rotateSelAxis = [&](Vec3 axis, float ang) {
        auto idx = selectedIndices();
        if (idx.empty()) return;
        axis = axis.normalized();
        if (axis.lengthSq() < 1e-10f) return;
        float cx, cy, cz;
        selCenter(cx, cy, cz);
        float cs = std::cos(ang), sn = std::sin(ang), omc = 1.0f - cs;
        for (int i : idx) {
            mat::Quad& q = editModel.quads[i];
            for (int c = 0; c < 4; c++) {
                Vec3 r{ q.p[c][0] - cx, q.p[c][1] - cy, q.p[c][2] - cz };
                Vec3 rp = r * cs + axis.cross(r) * sn + axis * (axis.dot(r) * omc);
                q.p[c][0] = rp.x + cx; q.p[c][1] = rp.y + cy; q.p[c][2] = rp.z + cz;
            }
            refreshUV(q);
        }
        ed.dirty = true;
    };
    auto rotateSelBy = [&](int axis, float ang) {
        Vec3 a = (axis == 0) ? Vec3{ 1, 0, 0 } : (axis == 1 ? Vec3{ 0, 1, 0 } : Vec3{ 0, 0, 1 });
        rotateSelAxis(a, ang);
    };
    auto rotateSel = [&](int axis, float ang) {
        if (!selectedIndices().empty()) {
            pushModelUndo();
            rotateSelBy(axis, ang);
            return;
        }
        auto ss = solidIds();
        if (ss.empty() || axis < 0 || axis > 2) return;
        pushModelUndo();
        for (int i : ss) editModel.solids[i].rot[axis] += ang;
        ed.dirty = true;
    };
    auto translateSel = [&](float dx, float dy, float dz) {
        auto idx = selectedIndices();
        if (idx.empty()) {
            auto ss = solidIds();
            if (ss.empty()) return;
            for (int i : ss) {
                mat::Solid& s = editModel.solids[i];
                s.c[0] += dx; s.c[1] += dy; s.c[2] += dz;
            }
            ed.dirty = true;
            return;
        }
        for (int i : idx) {
            mat::Quad& q = editModel.quads[i];
            for (int c = 0; c < 4; c++) {
                q.p[c][0] += dx; q.p[c][1] += dy; q.p[c][2] += dz;
            }
            refreshUV(q);
        }
        ed.dirty = true;
    };
    auto snapSel = [&]() {
        auto idx = selectedIndices();
        if (idx.empty()) {
            auto ss = solidIds();
            if (ss.empty()) return;
            pushModelUndo();
            const float grid = 0.25f;
            float mn[3] = { 1e9f, 1e9f, 1e9f };
            for (int i : ss) for (int k = 0; k < 3; k++) {
                float v = editModel.solids[i].c[k] - editModel.solids[i].h[k];
                if (v < mn[k]) mn[k] = v;
            }
            float d[3];
            for (int k = 0; k < 3; k++) d[k] = std::round(mn[k] / grid) * grid - mn[k];
            translateSel(d[0], d[1], d[2]);
            return;
        }
        pushModelUndo();
        const float grid = 0.25f;
        float mn[3] = { 1e9f, 1e9f, 1e9f };
        for (int i : idx) for (int c = 0; c < 4; c++)
            for (int k = 0; k < 3; k++) if (editModel.quads[i].p[c][k] < mn[k]) mn[k] = editModel.quads[i].p[c][k];
        float d[3];
        for (int k = 0; k < 3; k++) d[k] = std::round(mn[k] / grid) * grid - mn[k];
        translateSel(d[0], d[1], d[2]);
    };
    auto alignSel = [&]() {
        if (selectedIndices().empty()) {
            auto ss = solidIds();
            if (ss.empty()) return;
            pushModelUndo();
            for (int i : ss) editModel.solids[i].rot[0] = editModel.solids[i].rot[1] = editModel.solids[i].rot[2] = 0.0f;
            ed.dirty = true;
            return;
        }
        if (editModel.quads.empty() || ed.selQuad < 0 || ed.selQuad >= (int)editModel.quads.size()) return;
        const mat::Quad& q0 = editModel.quads[ed.selQuad];
        Vec3 n = faceNormalOf(q0);
        Vec3 e0{ q0.p[1][0] - q0.p[0][0], q0.p[1][1] - q0.p[0][1], q0.p[1][2] - q0.p[0][2] };
        if (n.lengthSq() < 1e-10f || e0.lengthSq() < 1e-12f) return;
        n = n.normalized();
        pushModelUndo();
        Vec3 an = nearestWorldAxis(n);
        Vec3 rotN = n.cross(an);
        float cn = n.dot(an);
        if (cn > 1.0f) cn = 1.0f;
        if (cn < -1.0f) cn = -1.0f;
        float angN = std::acos(cn);
        if (rotN.length() > 1e-6f && angN > 1e-5f) rotateSelAxis(rotN.normalized(), angN);
        const mat::Quad& q1 = editModel.quads[ed.selQuad];
        Vec3 e{ q1.p[1][0] - q1.p[0][0], q1.p[1][1] - q1.p[0][1], q1.p[1][2] - q1.p[0][2] };
        e = e - an * e.dot(an);
        if (e.lengthSq() < 1e-12f) return;
        e = e.normalized();
        const Vec3 world[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        Vec3 ae{ 0, 0, 0 };
        float best = -2.0f;
        for (int k = 0; k < 3; k++) {
            if (std::fabs(world[k].dot(an)) > 0.9f) continue;
            for (int s = 0; s < 2; s++) {
                Vec3 cand = world[k] * (s ? -1.0f : 1.0f);
                float d = e.dot(cand);
                if (d > best) { best = d; ae = cand; }
            }
        }
        if (best < -1.5f) return;
        float ce = e.dot(ae);
        if (ce > 1.0f) ce = 1.0f;
        if (ce < -1.0f) ce = -1.0f;
        float angE = std::atan2(e.cross(ae).dot(an), ce);
        if (std::fabs(angE) > 1e-5f) rotateSelAxis(an, angE);
    };
    auto reverseWinding = [](mat::Quad& q) {
        for (int k = 0; k < 3; k++) std::swap(q.p[1][k], q.p[3][k]);
        std::swap(q.uv[1][0], q.uv[3][0]);
        std::swap(q.uv[1][1], q.uv[3][1]);
    };
    auto mirrorSel = [&](int axis) {
        auto idx = selectedIndices();
        if (idx.empty()) {
            auto ss = solidIds();
            if (ss.empty() || axis < 0 || axis > 2) return;
            pushModelUndo();
            float cx, cy, cz;
            selCenter(cx, cy, cz);
            float c[3] = { cx, cy, cz };
            for (int i : ss) editModel.solids[i].c[axis] = 2.0f * c[axis] - editModel.solids[i].c[axis];
            ed.dirty = true;
            return;
        }
        if (axis < 0 || axis > 2) return;
        pushModelUndo();
        float cx, cy, cz;
        selCenter(cx, cy, cz);
        float c[3] = { cx, cy, cz };
        for (int i : idx) {
            mat::Quad& q = editModel.quads[i];
            for (int v = 0; v < 4; v++)
                q.p[v][axis] = 2.0f * c[axis] - q.p[v][axis];
            reverseWinding(q);
            if (q.crop) applyCropUV(q);
        }
        ed.dirty = true;
    };
    auto flipSelUV = [&](int uvAxis) {
        auto idx = selectedIndices();
        if (idx.empty()) {
            auto ss = solidIds();
            if (ss.empty()) return;
            pushModelUndo();
            int bit = (uvAxis == 1) ? 2 : 1;
            for (int i : ss) editModel.solids[i].uvFlip ^= bit;
            ed.dirty = true;
            return;
        }
        pushModelUndo();
        for (int i : idx) {
            mat::Quad& q = editModel.quads[i];
            if (uvAxis == 0) {
                std::swap(q.uv[0][0], q.uv[1][0]); std::swap(q.uv[0][1], q.uv[1][1]);
                std::swap(q.uv[3][0], q.uv[2][0]); std::swap(q.uv[3][1], q.uv[2][1]);
            } else {
                std::swap(q.uv[0][0], q.uv[3][0]); std::swap(q.uv[0][1], q.uv[3][1]);
                std::swap(q.uv[1][0], q.uv[2][0]); std::swap(q.uv[1][1], q.uv[2][1]);
            }
            q.uvLock = true;
        }
        ed.dirty = true;
    };
    auto applyTexScaleSel = [&](float s) {
        if (s < 0.03125f) s = 0.03125f;
        if (s > 2.0f) s = 2.0f;
        auto idx = selectedIndices();
        if (idx.empty()) {
            auto ss = solidIds();
            if (ss.empty()) return;
            for (int i : ss) editModel.solids[i].texScale = s;
            ed.dirty = true;
            return;
        }
        for (int i : idx) {
            mat::Quad& q = editModel.quads[i];
            q.texScale = s;
            q.uvMode = 1;
            q.uvLock = false;
            refreshUV(q);
        }
        ed.dirty = true;
    };
    auto stepTexScaleSel = [&](int dir) {
        auto idx = selectedIndices();
        float cur = 1.0f;
        if (!idx.empty()) cur = mat::quadTexScale(editModel.quads[idx[0]]);
        else {
            auto ss = solidIds();
            if (ss.empty()) return;
            cur = editModel.solids[ss[0]].texScale;
            if (cur < 1e-4f) cur = 1.0f;
        }
        const float steps[] = { 0.03125f, 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f };
        const int n = 7;
        int best = 0;
        float bd = std::fabs(cur - steps[0]);
        for (int i = 1; i < n; i++) {
            float d = std::fabs(cur - steps[i]);
            if (d < bd) { bd = d; best = i; }
        }
        int nxt = best + dir;
        if (nxt < 0) nxt = 0;
        if (nxt > n - 1) nxt = n - 1;
        if (std::fabs(steps[nxt] - cur) < 1e-6f && dir != 0) {
            nxt = best + dir;
            if (nxt < 0) nxt = 0;
            if (nxt > n - 1) nxt = n - 1;
        }
        pushModelUndo();
        applyTexScaleSel(steps[nxt]);
    };
    auto bindSel = [&]() {
        auto idx = selectedIndices();
        if (idx.size() < 2) {
            auto ss = solidIds();
            if (ss.size() < 2) return;
            pushModelUndo();
            int b = nextBindId();
            for (int i : ss) editModel.solids[i].bind = b;
            ed.dirty = true;
            selectOnlySolid(ss[0]);
            return;
        }
        pushModelUndo();
        int b = nextBindId();
        for (int i : idx) editModel.quads[i].bind = b;
        ed.dirty = true;
        markObject(idx[0], false);
    };
    auto unbindSel = [&]() {
        auto idx = selectedIndices();
        if (idx.empty()) {
            auto ss = solidIds();
            if (ss.empty()) return;
            std::vector<int> binds;
            for (int i : ss) {
                int b = editModel.solids[i].bind;
                if (b < 0) continue;
                bool have = false;
                for (int x : binds) if (x == b) have = true;
                if (!have) binds.push_back(b);
            }
            if (binds.empty()) return;
            pushModelUndo();
            for (mat::Solid& s : editModel.solids)
                for (int b : binds) if (s.bind == b) s.bind = -1;
            ed.dirty = true;
            return;
        }
        bool anyBind = false;
        for (int i : idx) if (editModel.quads[i].bind >= 0) { anyBind = true; break; }
        pushModelUndo();
        if (anyBind) {
            std::vector<int> binds;
            for (int i : idx) {
                int b = editModel.quads[i].bind;
                if (b < 0) continue;
                bool have = false;
                for (int x : binds) if (x == b) have = true;
                if (!have) binds.push_back(b);
            }
            for (mat::Quad& q : editModel.quads)
                for (int b : binds) if (q.bind == b) q.bind = -1;
        } else {
            // Unbind a native block (not small): each face becomes independent.
            std::vector<int> groups;
            for (int i : idx) {
                const mat::Quad& q = editModel.quads[i];
                if (q.solid || q.crop || q.group < 0) continue;
                bool have = false;
                for (int g : groups) if (g == q.group) have = true;
                if (!have) groups.push_back(q.group);
            }
            if (groups.empty()) { modelUndo.pop_back(); return; } // small-block: refuse
            for (mat::Quad& q : editModel.quads)
                for (int g : groups) if (q.group == g && !q.solid && !q.crop) q.group = -1;
        }
        ed.dirty = true;
        markObject(ed.selQuad, false);
    };
    auto bakeFillIfLeaving = [&]() {
        if (ed.modelTool != 2) return;
        for (int i : selectedIndices()) lockFillUV(editModel.quads[i]);
    };
    auto clearHairBrush = [&]() {
        ed.hairPaint = false;
        ed.hairErase = false;
        ed.hairSelect = false;
        ed.hairCard = false;
        ed.hairSelDrag = false;
        ed.partCut = false;
        ed.partAdd = false;
        ed.partAddVis = false;
        ed.partAnchor = false;
        ed.partMeasure = false;
        ed.partPlane = false;
    };
    auto setHairBrush = [&](bool erase) {
        bool& flag = erase ? ed.hairErase : ed.hairPaint;
        bool& other = erase ? ed.hairPaint : ed.hairErase;
        if (flag) {
            flag = false;
            return;
        }
        flag = true;
        other = false;
        ed.hairSelect = false;
        ed.hairCard = false;
        ed.hairSelDrag = false;
        ed.partCut = false;
        ed.partAdd = false;
        ed.partMeasure = false;
        ed.partPlane = false;
        ed.modelTool = -1;
        ed.tool = -1;
        ed.picker = false;
        ed.brushMod = false;
    };
    auto setHairCard = [&]() {
        if (ed.hairCard) {
            ed.hairCard = false;
            return;
        }
        ed.hairCard = true;
        ed.hairPaint = false;
        ed.hairErase = false;
        ed.hairSelect = false;
        ed.hairSelDrag = false;
        ed.partCut = false;
        ed.partAdd = false;
        ed.partMeasure = false;
        ed.partPlane = false;
        ed.modelTool = -1;
        ed.tool = -1;
        ed.picker = false;
        ed.brushMod = false;
    };
    auto setHairSelect = [&]() {
        if (ed.hairSelect) {
            ed.hairSelect = false;
            ed.hairSelDrag = false;
            return;
        }
        ed.hairSelect = true;
        ed.hairPaint = false;
        ed.hairErase = false;
        ed.hairCard = false;
        ed.hairSelDrag = false;
        ed.partCut = false;
        ed.partAdd = false;
        ed.partMeasure = false;
        ed.partPlane = false;
        ed.modelTool = -1;
        ed.tool = -1;
        ed.picker = false;
        ed.brushMod = false;
    };
    auto setPartAdd = [&]() {
        if (ed.partAdd) { ed.partAdd = false; ed.partAddVis = false; ed.partAnchor = false; return; }
        clearHairBrush();
        ed.partAdd = true;
        ed.modelTool = -1;
        ed.tool = -1;
        ed.picker = false;
        ed.brushMod = false;
    };
    auto setPartMeasure = [&]() {
        if (ed.partMeasure) { ed.partMeasure = false; ed.partAddVis = false; ed.partAnchor = false; return; }
        clearHairBrush();
        ed.partMeasure = true;
        ed.modelTool = -1;
        ed.tool = -1;
        ed.picker = false;
        ed.brushMod = false;
    };
    auto setPartPlane = [&]() {
        if (ed.partPlane) { ed.partPlane = false; ed.partAddVis = false; ed.partAnchor = false; return; }
        clearHairBrush();
        ed.partPlane = true;
        ed.modelTool = -1;
        ed.tool = -1;
        ed.picker = false;
        ed.brushMod = false;
    };
    auto setPartCut = [&]() {
        if (ed.partCut) { ed.partCut = false; return; }
        clearHairBrush();
        ed.partCut = true;
        ed.modelTool = -1;
        ed.tool = -1;
        ed.picker = false;
        ed.brushMod = false;
    };
    auto setModelTool = [&](int t) {
        if (t == ed.modelTool) {
            if (ed.modelMode) bakeFillIfLeaving();
            ed.modelTool = -1;
            return;
        }
        if (ed.modelMode) bakeFillIfLeaving();
        ed.modelTool = t;
        clearHairBrush();
        if (ed.modelMode && t == 2) {
            for (int i : selectedIndices()) {
                if (!isDeformableFace(i)) continue;
                mat::Quad& q = editModel.quads[i];
                q.uvMode = 1;
                q.uvLock = false;
                applyFillUV(q);
                ed.dirty = true;
            }
            snapshotFillBase();
        }
    };
    auto pushTexSolid = [&](float cx, float cy, float cz, float hx, float hy, float hz, int kind, int face, bool crop, float texS) {
        pushModelUndo();
        mat::Solid s;
        s.c[0] = cx; s.c[1] = cy; s.c[2] = cz;
        s.h[0] = hx; s.h[1] = hy; s.h[2] = hz;
        s.rgb[0] = ed.r; s.rgb[1] = ed.g; s.rgb[2] = ed.b;
        s.kind = kind;
        s.face = face;
        s.crop = crop;
        s.texScale = texS;
        s.tex = ed.selMat;
        editModel.solids.push_back(s);
        selectOnlySolid((int)editModel.solids.size() - 1);
        ed.dirty = true;
    };
    auto addFace = [&]() {
        pushTexSolid(0.5f, 0.5f, 0.02f, 0.5f, 0.5f, 0.02f, 1, 5, false, 1.0f);
    };
    auto addBlock = [&]() {
        pushTexSolid(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0, false, 1.0f);
    };
    const float kSmallS = 0.25f;
    const float kTinyS = 0.125f;
    auto addSmall = [&]() {
        pushTexSolid(kSmallS * 0.5f, kSmallS * 0.5f, kSmallS * 0.5f,
                     kSmallS * 0.5f, kSmallS * 0.5f, kSmallS * 0.5f, 0, 0, true, kSmallS);
    };
    auto addTinyAt = [&](float ox, float oy, float oz, float texS) {
        if (texS < 1e-4f) texS = kTinyS;
        float h = kTinyS * 0.5f;
        pushTexSolid(ox + h, oy + h, oz + h, h, h, h, 0, 0, true, texS);
    };
    auto cubeGroupBounds = [&](int qi, float mn[3], float mx[3]) -> bool {
        if (qi < 0 || qi >= (int)editModel.quads.size()) return false;
        int g = editModel.quads[qi].group;
        bool any = false;
        for (const mat::Quad& q : editModel.quads) {
            if (g >= 0) {
                if (q.group != g) continue;
            } else if (&q != &editModel.quads[qi]) {
                continue;
            }
            for (int c = 0; c < 4; c++) {
                for (int k = 0; k < 3; k++) {
                    float v = q.p[c][k];
                    if (!any) { mn[k] = mx[k] = v; }
                    else {
                        if (v < mn[k]) mn[k] = v;
                        if (v > mx[k]) mx[k] = v;
                    }
                }
            }
            any = true;
        }
        return any;
    };
    auto isTinyCubeQi = [&](int qi) -> bool {
        if (qi < 0 || qi >= (int)editModel.quads.size()) return false;
        const mat::Quad& q = editModel.quads[qi];
        if (!q.crop || !q.solid || q.group < 0) return false;
        float mn[3], mx[3];
        if (!cubeGroupBounds(qi, mn, mx)) return false;
        for (int k = 0; k < 3; k++)
            if (std::fabs((mx[k] - mn[k]) - kTinyS) > 0.02f) return false;
        return true;
    };
    auto tinyOccupied = [&](float ox, float oy, float oz) -> bool {
        const float eps = 0.02f;
        std::vector<int> seen;
        for (int i = 0; i < (int)editModel.quads.size(); i++) {
            const mat::Quad& q = editModel.quads[i];
            if (!q.crop || !q.solid || q.group < 0) continue;
            bool dup = false;
            for (int g : seen) if (g == q.group) { dup = true; break; }
            if (dup) continue;
            seen.push_back(q.group);
            if (!isTinyCubeQi(i)) continue;
            float mn[3], mx[3];
            if (!cubeGroupBounds(i, mn, mx)) continue;
            if (std::fabs(mn[0] - ox) < eps && std::fabs(mn[1] - oy) < eps && std::fabs(mn[2] - oz) < eps)
                return true;
        }
        for (const mat::Solid& s : editModel.solids) {
            if (!s.crop) continue;
            float mn[3] = { s.c[0] - s.h[0], s.c[1] - s.h[1], s.c[2] - s.h[2] };
            bool tiny = true;
            for (int k = 0; k < 3; k++) if (std::fabs(s.h[k] * 2.0f - kTinyS) > 0.02f) tiny = false;
            if (!tiny) continue;
            if (std::fabs(mn[0] - ox) < eps && std::fabs(mn[1] - oy) < eps && std::fabs(mn[2] - oz) < eps)
                return true;
        }
        return false;
    };
    auto snapTiny = [&](float v) {
        return std::floor(v / kTinyS + 1e-4f) * kTinyS;
    };
    auto pickTinyOrigin = [&](const Vec3& ro, const Vec3& rd, float& ox, float& oy, float& oz, float& texS) -> bool {
        texS = kTinyS;
        float best = 1e9f;
        int hit = -1;
        for (int i = 0; i < (int)editModel.quads.size(); i++) {
            Vec3 p[4];
            for (int c = 0; c < 4; c++) p[c] = viewOfModel(editModel.quads[i].p[c]);
            float t;
            if (rayHitTri(ro, rd, p[0], p[1], p[2], t) && t < best) { best = t; hit = i; }
            if (rayHitTri(ro, rd, p[0], p[2], p[3], t) && t < best) { best = t; hit = i; }
        }
        float solidT = 1e9f;
        int solidHit = -1;
        Vec3 solidN{};
        for (int i = 0; i < (int)editModel.solids.size(); i++) {
            const mat::Solid& s = editModel.solids[i];
            if (!s.crop) continue;
            bool tiny = true;
            for (int k = 0; k < 3; k++) if (std::fabs(s.h[k] * 2.0f - kTinyS) > 0.02f) tiny = false;
            if (!tiny) continue;
            Vec3 c{ s.c[0] - 0.5f, s.c[1], s.c[2] - 0.5f };
            Vec3 h{ s.h[0], s.h[1], s.h[2] };
            float t;
            if (!rayHitAABB(ro, rd, c - h, c + h, t) || t >= solidT) continue;
            solidT = t;
            solidHit = i;
            Vec3 hp = ro + rd * t;
            float d[6] = {
                std::fabs(hp.y - (c.y + h.y)), std::fabs(hp.y - (c.y - h.y)),
                std::fabs(hp.x - (c.x + h.x)), std::fabs(hp.x - (c.x - h.x)),
                std::fabs(hp.z - (c.z + h.z)), std::fabs(hp.z - (c.z - h.z))
            };
            int fi = 0;
            for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
            solidN = { (float)geo::kFaces[fi].n[0], (float)geo::kFaces[fi].n[1], (float)geo::kFaces[fi].n[2] };
        }
        if (solidHit >= 0 && (hit < 0 || solidT <= best)) {
            const mat::Solid& s = editModel.solids[solidHit];
            texS = s.texScale > 1e-4f ? s.texScale : kTinyS;
            float mn[3] = { s.c[0] - s.h[0], s.c[1] - s.h[1], s.c[2] - s.h[2] };
            ox = mn[0] + solidN.x * kTinyS;
            oy = mn[1] + solidN.y * kTinyS;
            oz = mn[2] + solidN.z * kTinyS;
            if (tinyOccupied(ox, oy, oz)) return false;
            return true;
        }
        if (hit >= 0 && isTinyCubeQi(hit)) {
            float mn[3], mx[3];
            if (!cubeGroupBounds(hit, mn, mx)) return false;
            texS = mat::quadTexScale(editModel.quads[hit]);
            Vec3 hm = ro + rd * best;
            hm.x += 0.5f;
            hm.z += 0.5f;
            float d[6] = {
                std::fabs(hm.y - mx[1]), std::fabs(hm.y - mn[1]),
                std::fabs(hm.x - mx[0]), std::fabs(hm.x - mn[0]),
                std::fabs(hm.z - mx[2]), std::fabs(hm.z - mn[2])
            };
            int f = 0;
            for (int i = 1; i < 6; i++) if (d[i] < d[f]) f = i;
            const int* n = geo::kFaces[f].n;
            float s = mx[0] - mn[0];
            ox = mn[0] + (float)n[0] * s;
            oy = mn[1] + (float)n[1] * s;
            oz = mn[2] + (float)n[2] * s;
            if (tinyOccupied(ox, oy, oz)) return false;
            return true;
        }
        Vec3 h;
        if (hit >= 0) {
            h = ro + rd * best;
        } else if (std::fabs(rd.y) > 1e-6f) {
            float t = (0.0f - ro.y) / rd.y;
            if (t < 0.001f) return false;
            h = ro + rd * t;
        } else {
            return false;
        }
        float mxp = h.x + 0.5f, myp = h.y, mzp = h.z + 0.5f;
        ox = snapTiny(mxp);
        oy = snapTiny(myp);
        oz = snapTiny(mzp);
        if (ox < -1.0f || ox > 2.0f || oy < -1.0f || oy > 2.0f || oz < -1.0f || oz > 2.0f) {
            if (hit < 0) { ox = 0; oy = 0; oz = 0; }
            else return false;
        }
        if (tinyOccupied(ox, oy, oz)) return false;
        return true;
    };
    auto assignSelTex = [&](const std::string& name) {
        ed.selMat = name;
        auto idx0 = selectedIndices();
        if (idx0.empty()) {
            auto ss = solidIds();
            if (ss.empty()) return;
            pushModelUndo();
            for (int i : ss) editModel.solids[i].tex = name;
            ed.dirty = true;
            return;
        }
        if (editModel.quads.empty()) return;
        pushModelUndo();
        auto idx = selectedIndices();
        for (int i : idx) {
            mat::Quad& q = editModel.quads[i];
            if (q.solid && q.group >= 0) {
                for (mat::Quad& o : editModel.quads) if (o.group == q.group) o.tex = name;
            } else {
                q.tex = name;
            }
        }
        ed.dirty = true;
    };
    auto uniqueExtraName = [&]() {
        for (int n = 0; ; n++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "extra_%d", n);
            bool used = false;
            for (const ExtraMat& e : extraMats) if (e.name == buf) used = true;
            std::string path = std::string(kExtrasDir) + "/" + buf + ".png";
            if (!used && !std::filesystem::exists(path)) return std::string(buf);
        }
    };
    auto addExtraMat = [&]() {
        char file[MAX_PATH] = {};
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd;
        ofn.lpstrFilter = "PNG images\0*.png\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        mat::Image img;
        std::string name;
        if (GetOpenFileNameA(&ofn)) {
            img = mat::loadPNG(file);
            name = std::filesystem::path(file).stem().string();
            bool used = false;
            for (const ExtraMat& e : extraMats) if (e.name == name) used = true;
            if (name.empty() || used) name = uniqueExtraName();
        }
        if (!img.ok()) {
            name = uniqueExtraName();
            img.w = tex::TILE;
            img.h = tex::TILE;
            img.rgba.assign((size_t)tex::TILE * tex::TILE * 4, 0);
            for (int y = 0; y < tex::TILE; y++) for (int x = 0; x < tex::TILE; x++) {
                bool chk = ((x / 8) + (y / 8)) & 1;
                size_t i = ((size_t)y * tex::TILE + x) * 4;
                uint8_t v = chk ? 200 : 150;
                img.rgba[i + 0] = v; img.rgba[i + 1] = v; img.rgba[i + 2] = v; img.rgba[i + 3] = 255;
            }
        }
        std::string path = std::string(kExtrasDir) + "/" + name + ".png";
        mat::savePNG(path.c_str(), img.w, img.h, img.rgba.data());
        ExtraMat e;
        e.name = name;
        e.img = std::move(img);
        e.tex = uploadImgTex(e.img);
        extraMats.push_back(std::move(e));
        assignSelTex(name);
    };

    // Load a block's .model file into the editor (called on selection + startup).
    auto loadBlockModel = [&](int b) {
        if (paintDirty) revertPaint();
        if (b < 1 || b >= liveBlockCount()) b = GRASS_TUFT;
        ed.modelBlock = b;
        std::string p = pack::blockModel(mat::blockName(b));
        editModel = mat::loadModel(p.c_str());
        editRand = mat::loadRand(randPathFor(b).c_str());
        loadItemSheet();
        if (b == GRASS_TUFT) ensureRandDefaults();
        std::string def = defaultTexFor(b);
        for (mat::Quad& q : editModel.quads) if (q.tex.empty()) q.tex = def;
        ed.selMat = (!editModel.quads.empty() && !editModel.quads[0].tex.empty()) ? editModel.quads[0].tex : def;
        snapshotSaved();
        ed.selQuad = 0; ed.selCorner = 0;
        ed.selSolid = -1;
        solidMark.clear();
        ed.partAnchor = false;
        ed.partAddVis = false;
        ed.measureOn = false;
        ed.planeOn = false;
        markObject(0, false);
        ed.dirty = false;
        modelUndo.clear();
    };
    loadBlockModel(GRASS_TUFT);

    // Enter the requested mode (or stay on the chooser when startMode < 0).
    if (startMode == 3) enterAnimMode();
    else if (startMode == 2) ed.entityMode = true;
    else if (startMode == 1) ed.modelMode = true;

    MSG msg;
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            else { TranslateMessage(&msg); DispatchMessage(&msg); }
        }
        if (!running) break;

        POINT mp; GetCursorPos(&mp); ScreenToClient(g_hwnd, &mp);
        float mx = (float)mp.x, my = (float)mp.y;

        static LARGE_INTEGER qpf = {}, qprev = {};
        if (qpf.QuadPart == 0) { QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&qprev); }
        LARGE_INTEGER qnow; QueryPerformanceCounter(&qnow);
        float dt = (float)(qnow.QuadPart - qprev.QuadPart) / (float)qpf.QuadPart;
        qprev = qnow;
        if (dt < 0.0f || dt > 0.1f) dt = 0.016f;
        if (ed.animMode && ed.animPlaying && editClip.fps > 0.1f) {
            ed.animClock += dt * editClip.fps;
            float L = (float)std::max(1, editClip.length);
            if (editClip.loop) {
                while (ed.animClock >= L) ed.animClock -= L;
                while (ed.animClock < 0.0f) ed.animClock += L;
            } else if (ed.animClock > L - 1.0f) {
                ed.animClock = L - 1.0f;
                ed.animPlaying = false;
            }
            ed.animFrame = (int)ed.animClock;
            if (ed.animFrame < 0) ed.animFrame = 0;
            if (ed.animFrame > editClip.length - 1) ed.animFrame = editClip.length - 1;
        } else if (ed.animMode) {
            int last = editClip.length > 0 ? editClip.length - 1 : 0;
            if (ed.animFrame > last) ed.animFrame = last;
            if (ed.animFrame < 0) ed.animFrame = 0;
            ed.animClock = (float)ed.animFrame;
        }

        // Block-model split layout (left = edit, right = apply-rand preview + sliders).
        const float kThumb = 36.0f, kGap = 6.0f, kCols = 4.0f;
        const float kPanelRight = 4.0f + kCols * kThumb + (kCols - 1.0f) * kGap + 8.0f; // 174
        float leftBarW = kPanelRight;
        const float kTopH = 40.0f;
        const float kHdrH = 40.0f;
        const float kToolH = 34.0f;
        float toolBandH = kToolH;
        const float kArW = 132.0f, kArH = 32.0f, kArM = 8.0f;
        const float kSaveS = 32.0f;
        const float kCancelW = 90.0f, kCancelH = 32.0f;
        const float kExW = 120.0f, kExH = 44.0f, kExM = 20.0f;
        float exX = g_winW - kExW - kExM, exY = g_winH - kExH - kExM;
        float splitX = leftBarW + ((float)g_winW - leftBarW) * 0.5f;
        float paneLX = leftBarW;
        float paneLW = splitX - leftBarW;
        const float kMatThumb = 26.0f, kMatGap = 4.0f, kMatPad = 8.0f;
        int nMatItems = TEX_COUNT + (int)extraMats.size() + 1; // last slot = add extra
        int matCols = (int)((paneLW - kMatPad * 2.0f) / (kMatThumb + kMatGap));
        if (matCols < 1) matCols = 1;
        int matRows = (nMatItems + matCols - 1) / matCols;
        if (matRows < 2) matRows = 2;
        float kMatBarH = kMatPad * 2.0f + matRows * (kMatThumb + kMatGap);
        float paneLY = kTopH + kHdrH + kToolH;
        float paneLH = (float)g_winH - paneLY - kMatBarH;
        if (paneLH < 40.0f) paneLH = 40.0f;
        float matBarY = paneLY + paneLH;
        float paneRX = splitX, paneRW = (float)g_winW - splitX;
        float paneRY = kTopH + kHdrH;
        float paneRH = (float)g_winH - paneRY;
        const float kEntTypeH = 52.0f;
        const float kEntSlideW = 22.0f, kEntSlidePad = 10.0f;
        float kEntBarH = kEntTypeH;
        float entSlideX = 0, entSlideY = 0, entSlideH = 0, entViewRW = 0;
        float paintSlideX = 0, paintSlideY = 0, paintSlideW = 0, paintSlideH = 0;
        float skinImgX = 0, skinImgY = 0, skinImgS = 0;
        float togX = 0, togY = 0, togW = 74.0f, togH = 26.0f;
        float cBody[4] = {};
        float palColX = 0, palColW = 0;
        float entToolX = 0, entToolColW = 0;
        if (ed.entityMode) {
            scanEntFiles();
            const float kPalColW = 208.0f;
            const float kEntToolColW = 92.0f;
            leftBarW = 236.0f;
            paneLX = leftBarW;
            toolBandH = 0.0f;
            kEntBarH = 124.0f;
            entToolColW = kEntToolColW;
            entToolX = (float)g_winW - entToolColW;
            float rest = (float)g_winW - paneLX - kPalColW - entToolColW;
            if (rest < 520.0f) rest = (float)g_winW - paneLX - entToolColW;
            splitX = paneLX + rest * 0.55f;
            if (splitX < paneLX + 280.0f) splitX = paneLX + 280.0f;
            if (splitX > (float)g_winW - kPalColW - 240.0f - entToolColW)
                splitX = (float)g_winW - kPalColW - 240.0f - entToolColW;
            paneLW = splitX - paneLX;
            palColX = splitX;
            palColW = kPalColW;
            paneRX = palColX + palColW;
            paneRW = entToolX - paneRX;
            if (paneRW < 200.0f) {
                paneRW = 200.0f;
                paneRX = entToolX - paneRW;
                palColW = paneRX - palColX;
            }
            paneLY = kTopH + kHdrH + toolBandH;
            paneRY = kTopH + kHdrH;
            paneLH = (float)g_winH - paneLY - kEntBarH;
            if (paneLH < 40.0f) paneLH = 40.0f;
            paneRH = (float)g_winH - paneRY - kEntBarH;
            if (paneRH < 40.0f) paneRH = 40.0f;
            matBarY = paneLY + paneLH;
            matCols = 4;
            matRows = (nMatItems + matCols - 1) / matCols;
            if (matRows < 1) matRows = 1;
            kMatBarH = kMatPad * 2.0f + matRows * (kMatThumb + kMatGap);
            entSlideX = paneRX + paneRW - kEntSlidePad - kEntSlideW;
            entSlideY = paneRY + 16.0f;
            entSlideH = paneRH - 32.0f;
            if (entSlideH < 40.0f) entSlideH = 40.0f;
            entViewRW = entSlideX - paneRX - 8.0f;
            if (entViewRW < 40.0f) entViewRW = 40.0f;
            bool entScale = ed.hairPaint || ed.entSkinView || ed.picker || (ed.tool >= 0 && ed.tool <= 8);
            if (entScale) {
                paintSlideH = 16.0f;
                bool onLeft = !ed.hairPaint;
                float avail = onLeft ? paneLW : entViewRW;
                float originX = onLeft ? paneLX : paneRX;
                float bottom = onLeft ? (paneLY + paneLH) : (paneRY + paneRH);
                paintSlideW = avail - 24.0f;
                if (paintSlideW > 168.0f) paintSlideW = 168.0f;
                if (paintSlideW < 48.0f) paintSlideW = 48.0f;
                paintSlideX = originX + 12.0f;
                paintSlideY = bottom - 12.0f - paintSlideH;
            }
            if (entIsCloth) togW = 108.0f;
            togX = paneLX + paneLW - 12.0f - togW;
            togY = paneLY + 8.0f;
            cBody[2] = 52.0f;
            cBody[3] = 26.0f;
            cBody[0] = paneRX + paneRW - 10.0f - cBody[2];
            cBody[1] = kTopH + 7.0f;
        }
        float viewLW = paneLW;
        float partColX = 0.0f, partColW = 0.0f;
        float partToolRect[13][4] = {};
        float partChipRect[8][4] = {};
        int partToolHover = -1;
        float rightTab[2][4] = {};
        int rightTabHover = -1;
        float colorViewW = paneRW;
        float colorSlideX = 0, colorSlideY = 0, colorSlideW = 0, colorSlideH = 0;
        bool colorFaceVis = false;
        Vec3 colorFaceQ[4] = {};
        float colorSv[4] = {};
        float colorHueCX = 0, colorHueCY = 0, colorHueRI = 0, colorHueRO = 0;
        float colorAlpha[4] = {};
        float colorTool[9][4] = {};
        float colorRecent[8][4] = {};
        float colorCur[4] = {};
        int colorToolHover = -1;
        int colorRecentHover = -1;
        int colorHx = -1, colorHy = -1;
        std::string itemPaintName;
        const char* partLab[13] = {
            "Part", "Select", "Paint", "Card", "Erase", "Merge", "Split",
            "Tex", "Dup", "Del", "Cut", "Measure", "Plane"
        };
        const char* colorLab[9] = {
            "Pen", "Brush", "Fill", "Pick", "Erase", "RFill", "Rect", "Line", "Circ"
        };
        if (ed.modelMode && paneLW > 160.0f && paneLH > 80.0f) {
            partColW = 78.0f;
            if (paneLW < partColW + 160.0f) partColW = paneLW - 160.0f;
            if (partColW < 64.0f) partColW = 64.0f;
            viewLW = paneLW - partColW;
            partColX = paneLX + viewLW;
            const float pad = 4.0f, gap = 3.0f;
            const float chipH = 18.0f, chipGap = 3.0f;
            const float chipBlock = chipH * 4.0f + chipGap * 3.0f;
            float avail = paneLH - pad * 2.0f - chipBlock - 8.0f;
            float bh = (avail - gap * 12.0f) / 13.0f;
            if (bh > 28.0f) bh = 28.0f;
            if (bh < 15.0f) bh = 15.0f;
            float bw = partColW - pad * 2.0f;
            float y = paneLY + pad;
            for (int i = 0; i < 13; i++) {
                partToolRect[i][0] = partColX + pad;
                partToolRect[i][1] = y;
                partToolRect[i][2] = bw;
                partToolRect[i][3] = bh;
                y += bh + gap;
            }
            if (ed.hairPaint && viewLW > 80.0f) {
                paintSlideW = viewLW - 24.0f;
                if (paintSlideW > 180.0f) paintSlideW = 180.0f;
                paintSlideH = 16.0f;
                paintSlideX = paneLX + 12.0f;
                paintSlideY = paneLY + paneLH - 8.0f - paintSlideH;
            }
            float cw = (bw - chipGap) * 0.5f;
            float cy = paneLY + paneLH - pad - chipBlock;
            for (int i = 0; i < 8; i++) {
                int col = i % 2, row = i / 2;
                partChipRect[i][0] = partColX + pad + col * (cw + chipGap);
                partChipRect[i][1] = cy + row * (chipH + chipGap);
                partChipRect[i][2] = cw;
                partChipRect[i][3] = chipH;
            }
        }
        float animVX = 0, animVY = 0, animVW = 0, animVH = 0;
        float animLeftW = 206.0f, animRightW = 280.0f;
        float animClipRect[96][4] = {};
        int animClipN = 0;
        float animBoneRect[80][4] = {};
        int animBoneN = 0;
        float animBindRect[48][4] = {};
        int animBindN = 0;
        float animBtnRect[13][4] = {};
        float animModelSave[4] = {};
        float animMaskSwatch[6][4] = {};
        float animKeyOp[10][4] = {};
        float animTurnRect[3][4] = {};
        float animFlipRect[3][4] = {};
        float animTimeRect[4] = {};
        float animBarPitch = 8.0f;
        float animBarScrollMax = 0.0f;
        float animBarSlider[4] = {};
        const float animBarIcon = 8.0f;
        auto layoutAnimBar = [&]() {
            int n = editClip.length > 1 ? editClip.length : 2;
            float inner = animTimeRect[2] - animBarIcon;
            if (inner < 8.0f) inner = 8.0f;
            if (ed.animBarZoom < 1.0f) ed.animBarZoom = 1.0f;
            if (ed.animBarZoom > 16.0f) ed.animBarZoom = 16.0f;
            float fit = inner / (float)(n - 1);
            float base = fit > animBarIcon ? fit : animBarIcon;
            animBarPitch = base * ed.animBarZoom;
            float content = animBarIcon + (float)(n - 1) * animBarPitch;
            animBarScrollMax = content - animTimeRect[2];
            if (animBarScrollMax < 0.5f) animBarScrollMax = 0.0f;
            if (ed.animBarScroll < 0.0f) ed.animBarScroll = 0.0f;
            if (ed.animBarScroll > animBarScrollMax) ed.animBarScroll = animBarScrollMax;
            animBarSlider[0] = animTimeRect[0];
            animBarSlider[1] = animTimeRect[1] + animTimeRect[3] + 4.0f;
            animBarSlider[2] = animTimeRect[2];
            animBarSlider[3] = 10.0f;
        };
        auto frameToX = [&](float frame) {
            float last = editClip.length > 1 ? (float)(editClip.length - 1) : 0.0f;
            if (frame < 0.0f) frame = 0.0f;
            if (frame > last) frame = last;
            return animTimeRect[0] + animBarIcon * 0.5f + frame * animBarPitch - ed.animBarScroll;
        };
        auto xToFrame = [&](float x) {
            int last = editClip.length > 1 ? editClip.length - 1 : 0;
            if (animBarPitch < 1e-4f) return 0;
            float f = (x - animTimeRect[0] - animBarIcon * 0.5f + ed.animBarScroll) / animBarPitch;
            int fi = (int)std::lround(f);
            if (fi < 0) fi = 0;
            if (fi > last) fi = last;
            return fi;
        };
        float animNewRect[4] = {}, animDupRect[4] = {}, animDelRect[4] = {};
        float animSaveRect[4] = {}, animRebuildRect[4] = {};
        float animHoldPrev[4] = {}, animHoldNext[4] = {}, animHoldL[4] = {}, animHoldR[4] = {};
        float animHoldGrip[4] = {}, animHoldGrasp[4] = {}, animHoldBone[4] = {}, animHoldSave[4] = {};
        float animHoldLower[4] = {}, animHoldHigher[4] = {};
        float animLockDir[4] = {}, animLockPos[4] = {};
        float animLenMinus[4] = {}, animLenPlus[4] = {};
        float animScaleRect[4][4] = {};
        float animUndoRect[4] = {};
        float animPaneTop = 0, animPaneBot = 0;
        float animScrollMaxL = 0, animScrollMaxR = 0;
        bool inAnim3d = false;
        if (ed.animMode) {
            scanEntFiles();
            const float kTimeH = 160.0f;
            animVX = animLeftW;
            animVY = kTopH + kHdrH;
            animVW = (float)g_winW - animLeftW - animRightW;
            if (animVW < 240.0f) animVW = 240.0f;
            animVH = (float)g_winH - animVY - kTimeH;
            if (animVH < 40.0f) animVH = 40.0f;
            animPaneTop = animVY;
            animPaneBot = animVY + animVH;
            inAnim3d = mx >= animVX && mx < animVX + animVW && my >= animVY && my < animVY + animVH;
            if (g_wheel != 0) {
                float ticks = (float)g_wheel / 120.0f;
                bool overLeft = mx >= 0.0f && mx < animLeftW && my >= animPaneTop && my < animPaneBot;
                bool overRight = mx >= (float)g_winW - animRightW && mx < (float)g_winW
                    && my >= animPaneTop && my < animPaneBot;
                bool usedWheel = false;
                if (overLeft) { ed.animScrollL -= ticks * 56.0f; usedWheel = true; }
                else if (overRight) { ed.animScrollR -= ticks * 56.0f; usedWheel = true; }
                else if (inAnim3d) {
                    ed.azoom *= (g_wheel > 0) ? 0.90f : 1.11f;
                    if (ed.azoom < 1.2f) ed.azoom = 1.2f;
                    if (ed.azoom > 12.0f) ed.azoom = 12.0f;
                    usedWheel = true;
                }
                if (usedWheel) g_wheel = 0;
            }
            const float clipH = 22.0f, clipGap = 3.0f, boneH = 20.0f, boneGap = 2.0f;
            animClipN = (int)animNames.size();
            if (animClipN > 96) animClipN = 96;
            animBoneN = (int)editClip.bones.size();
            if (animBoneN > 80) animBoneN = 80;
            float leftH = 8.0f + (float)animClipN * (clipH + clipGap) + 6.0f + 22.0f + 28.0f
                + (float)animBoneN * (boneH + boneGap) + 8.0f;
            animScrollMaxL = leftH - animVH;
            if (animScrollMaxL < 0.0f) animScrollMaxL = 0.0f;
            if (ed.animScrollL < 0.0f) ed.animScrollL = 0.0f;
            if (ed.animScrollL > animScrollMaxL) ed.animScrollL = animScrollMaxL;
            float y = animPaneTop + 8.0f - ed.animScrollL;
            for (int i = 0; i < animClipN; i++) {
                animClipRect[i][0] = 8.0f; animClipRect[i][1] = y;
                animClipRect[i][2] = animLeftW - 20.0f; animClipRect[i][3] = clipH;
                y += clipH + clipGap;
            }
            y += 6.0f;
            animNewRect[0] = 8.0f; animNewRect[1] = y; animNewRect[2] = 58.0f; animNewRect[3] = 22.0f;
            animDupRect[0] = 70.0f; animDupRect[1] = y; animDupRect[2] = 58.0f; animDupRect[3] = 22.0f;
            animDelRect[0] = 132.0f; animDelRect[1] = y; animDelRect[2] = 58.0f; animDelRect[3] = 22.0f;
            y += 22.0f + 28.0f;
            for (int i = 0; i < animBoneN; i++) {
                animBoneRect[i][0] = 8.0f; animBoneRect[i][1] = y;
                animBoneRect[i][2] = animLeftW - 20.0f; animBoneRect[i][3] = boneH;
                y += boneH + boneGap;
            }
            float rx = (float)g_winW - animRightW;
            animBindN = (int)entNames.size();
            if (animBindN > 48) animBindN = 48;
            const float holdRow = 36.0f, holdH = 28.0f, holdPad = 10.0f;
            float rightH = 8.0f + (float)animBindN * 24.0f + 470.0f;
            animScrollMaxR = rightH - animVH;
            if (animScrollMaxR < 0.0f) animScrollMaxR = 0.0f;
            if (ed.animScrollR < 0.0f) ed.animScrollR = 0.0f;
            if (ed.animScrollR > animScrollMaxR) ed.animScrollR = animScrollMaxR;
            float by = animPaneTop + 8.0f - ed.animScrollR;
            for (int i = 0; i < animBindN; i++) {
                animBindRect[i][0] = rx + 8.0f; animBindRect[i][1] = by;
                animBindRect[i][2] = animRightW - 20.0f; animBindRect[i][3] = 22.0f;
                by += 24.0f;
            }
            animRebuildRect[0] = rx + 10.0f;
            animRebuildRect[1] = by + 10.0f;
            animRebuildRect[2] = animRightW - 20.0f; animRebuildRect[3] = 28.0f;
            animModelSave[0] = rx + 10.0f;
            animModelSave[1] = animRebuildRect[1] + animRebuildRect[3] + 26.0f;
            animModelSave[2] = animRightW - 20.0f; animModelSave[3] = 26.0f;
            float hy = animModelSave[1] + animModelSave[3] + 34.0f;
            float inner = animRightW - holdPad * 2.0f;
            animHoldPrev[0] = rx + holdPad; animHoldPrev[1] = hy; animHoldPrev[2] = 36.0f; animHoldPrev[3] = holdH;
            animHoldNext[0] = rx + animRightW - holdPad - 36.0f; animHoldNext[1] = hy; animHoldNext[2] = 36.0f; animHoldNext[3] = holdH;
            float half = (inner - 8.0f) * 0.5f;
            animHoldL[0] = rx + holdPad; animHoldL[1] = hy + holdRow; animHoldL[2] = half; animHoldL[3] = holdH;
            animHoldR[0] = rx + holdPad + half + 8.0f; animHoldR[1] = hy + holdRow; animHoldR[2] = half; animHoldR[3] = holdH;
            animHoldGrip[0] = rx + holdPad; animHoldGrip[1] = hy + holdRow * 2.0f; animHoldGrip[2] = half; animHoldGrip[3] = holdH;
            animHoldGrasp[0] = rx + holdPad + half + 8.0f; animHoldGrasp[1] = hy + holdRow * 2.0f; animHoldGrasp[2] = half; animHoldGrasp[3] = holdH;
            animHoldLower[0] = rx + holdPad; animHoldLower[1] = hy + holdRow * 3.0f; animHoldLower[2] = half; animHoldLower[3] = holdH;
            animHoldHigher[0] = rx + holdPad + half + 8.0f; animHoldHigher[1] = hy + holdRow * 3.0f; animHoldHigher[2] = half; animHoldHigher[3] = holdH;
            const float gripGap = 22.0f;
            animLockDir[0] = rx + holdPad; animLockDir[1] = hy + holdRow * 4.0f + gripGap; animLockDir[2] = half; animLockDir[3] = holdH;
            animLockPos[0] = rx + holdPad + half + 8.0f; animLockPos[1] = hy + holdRow * 4.0f + gripGap; animLockPos[2] = half; animLockPos[3] = holdH;
            animHoldBone[0] = rx + holdPad; animHoldBone[1] = hy + holdRow * 5.0f + gripGap; animHoldBone[2] = inner; animHoldBone[3] = holdH;
            animHoldSave[0] = rx + holdPad; animHoldSave[1] = hy + holdRow * 6.0f + gripGap; animHoldSave[2] = inner; animHoldSave[3] = holdH;
            float ty = (float)g_winH - kTimeH;
            float btnY = ty + 8.0f;
            const float bw = 36.0f, bh = 26.0f, gap = 6.0f;
            for (int i = 0; i < 6; i++) {
                animBtnRect[i][0] = 8.0f + i * (bw + gap);
                animBtnRect[i][1] = btnY;
                animBtnRect[i][2] = bw; animBtnRect[i][3] = bh;
            }
            animBtnRect[8][0] = animVX + 8.0f; animBtnRect[8][1] = kTopH + 6.0f;
            animBtnRect[8][2] = 48.0f; animBtnRect[8][3] = 26.0f;
            animBtnRect[9][0] = animVX + 60.0f; animBtnRect[9][1] = kTopH + 6.0f;
            animBtnRect[9][2] = 40.0f; animBtnRect[9][3] = 26.0f;
            animBtnRect[12][0] = animVX + 104.0f; animBtnRect[12][1] = kTopH + 6.0f;
            animBtnRect[12][2] = 48.0f; animBtnRect[12][3] = 26.0f;
            animBtnRect[10][0] = animVX + 156.0f; animBtnRect[10][1] = kTopH + 6.0f;
            animBtnRect[10][2] = 58.0f; animBtnRect[10][3] = 26.0f;
            animBtnRect[11][0] = animVX + 218.0f; animBtnRect[11][1] = kTopH + 6.0f;
            animBtnRect[11][2] = 112.0f; animBtnRect[11][3] = 26.0f;
            for (int i = 0; i < 6; i++) {
                animMaskSwatch[i][0] = animVX + 338.0f + i * 12.0f;
                animMaskSwatch[i][1] = kTopH + 10.0f;
                animMaskSwatch[i][2] = 10.0f;
                animMaskSwatch[i][3] = 18.0f;
            }
            animBtnRect[6][0] = animVX + 418.0f; animBtnRect[6][1] = kTopH + 6.0f;
            animBtnRect[6][2] = 84.0f; animBtnRect[6][3] = 26.0f;
            animBtnRect[7][0] = animVX + 510.0f; animBtnRect[7][1] = kTopH + 6.0f;
            animBtnRect[7][2] = 68.0f; animBtnRect[7][3] = 26.0f;
            animSaveRect[0] = animVX + 586.0f; animSaveRect[1] = kTopH + 6.0f;
            animSaveRect[2] = 70.0f; animSaveRect[3] = 26.0f;
            animLenMinus[0] = 8.0f + 6.0f * (bw + gap) + 8.0f;
            animLenMinus[1] = btnY; animLenMinus[2] = 26.0f; animLenMinus[3] = bh;
            animLenPlus[0] = animLenMinus[0] + 78.0f;
            animLenPlus[1] = btnY; animLenPlus[2] = 26.0f; animLenPlus[3] = bh;
            {
                const float sw[4] = { 52.0f, 36.0f, 40.0f, 44.0f };
                float sx = animLenPlus[0] + animLenPlus[2] + 16.0f;
                for (int i = 0; i < 4; i++) {
                    animScaleRect[i][0] = sx;
                    animScaleRect[i][1] = btnY;
                    animScaleRect[i][2] = sw[i];
                    animScaleRect[i][3] = bh;
                    sx += sw[i] + 4.0f;
                }
            }
            animUndoRect[0] = (float)g_winW - 78.0f; animUndoRect[1] = btnY;
            animUndoRect[2] = 68.0f; animUndoRect[3] = bh;
            {
                const float kw[10] = { 64.0f, 64.0f, 64.0f, 72.0f, 48.0f, 32.0f, 32.0f, 52.0f, 52.0f, 78.0f };
                float kx = 8.0f;
                for (int i = 0; i < 10; i++) {
                    animKeyOp[i][0] = kx;
                    animKeyOp[i][1] = ty + 40.0f;
                    animKeyOp[i][2] = kw[i];
                    animKeyOp[i][3] = bh;
                    kx += kw[i] + 6.0f;
                }
                const float tw[3] = { 52.0f, 36.0f, 36.0f };
                kx += 10.0f;
                for (int i = 0; i < 3; i++) {
                    animTurnRect[i][0] = kx;
                    animTurnRect[i][1] = ty + 40.0f;
                    animTurnRect[i][2] = tw[i];
                    animTurnRect[i][3] = bh;
                    kx += tw[i] + 4.0f;
                }
                kx += 52.0f;
                for (int i = 0; i < 3; i++) {
                    animFlipRect[i][0] = kx;
                    animFlipRect[i][1] = ty + 40.0f;
                    animFlipRect[i][2] = 36.0f;
                    animFlipRect[i][3] = bh;
                    kx += 40.0f;
                }
            }
            animTimeRect[0] = 8.0f;
            animTimeRect[1] = ty + 96.0f;
            animTimeRect[2] = (float)g_winW - animRightW - 16.0f;
            if (animTimeRect[2] < 80.0f) animTimeRect[2] = 80.0f;
            animTimeRect[3] = 28.0f;
            layoutAnimBar();
            if (g_wheel != 0 && editClip.length > 1) {
                bool overBar = mx >= animTimeRect[0] && mx < animTimeRect[0] + animTimeRect[2]
                    && my >= animTimeRect[1] && my < animBarSlider[1] + animBarSlider[3];
                if (overBar) {
                    float frame = (mx - animTimeRect[0] - animBarIcon * 0.5f + ed.animBarScroll) / animBarPitch;
                    ed.animBarZoom *= (g_wheel > 0) ? 1.15f : (1.0f / 1.15f);
                    layoutAnimBar();
                    ed.animBarScroll = animTimeRect[0] + animBarIcon * 0.5f + frame * animBarPitch - mx;
                    if (ed.animBarScroll < 0.0f) ed.animBarScroll = 0.0f;
                    if (ed.animBarScroll > animBarScrollMax) ed.animBarScroll = animBarScrollMax;
                    g_wheel = 0;
                }
            }
            inAnim3d = mx >= animVX && mx < animVX + animVW && my >= animVY && my < animVY + animVH;
        }
        const float kBtnH = 32.0f, kFaceW = 72.0f, kBlkW = 84.0f, kSmW = 108.0f, kTinyW = 64.0f;
        float midUndoX = splitX - 8.0f - kSaveS;
        float midSaveX = midUndoX - 8.0f - kSaveS;
        float tinyX = midSaveX - 8.0f - kTinyW;
        float smX = tinyX - 6.0f - kSmW;
        float blkX = smX - 6.0f - kBlkW;
        float faceX = blkX - 6.0f - kFaceW;
        float midBtnY = kTopH + (kHdrH - kBtnH) * 0.5f;
        float entCancelX = midSaveX - 8.0f - kCancelW;
        const int kNMTools = 11;
        const int kEntTools = 11;
        const char* kMToolLab[11] = { "Bind", "Unbind", "Move", "Snap", "Align", "45", "Stretch", "Fill", "Mirror", "UV", "Tile" };
        const char* kEntToolLab[11] = { "Bind", "Unbind", "Move", "Snap", "Align", "45", "Stretch", "Fill", "Mirror", "UV", "Ref" };
        const float kMToolW[11] = { 46, 56, 46, 44, 48, 34, 56, 40, 54, 34, 40 };
        float kMToolHbtn = 26.0f;
        float mtoolY = kTopH + kHdrH + (kToolH - kMToolHbtn) * 0.5f;
        float mtoolRect[11][4] = {};
        {
            float tx = paneLX + 8.0f;
            for (int i = 0; i < kNMTools; i++) {
                mtoolRect[i][0] = tx; mtoolRect[i][1] = mtoolY;
                mtoolRect[i][2] = kMToolW[i]; mtoolRect[i][3] = kMToolHbtn;
                tx += kMToolW[i] + 4.0f;
            }
        }
        ed.modelToolHover = -1;
        if (ed.modelMode) {
            for (int i = 0; i < 13; i++) {
                if (mx >= partToolRect[i][0] && mx < partToolRect[i][0] + partToolRect[i][2] &&
                    my >= partToolRect[i][1] && my < partToolRect[i][1] + partToolRect[i][3])
                    partToolHover = i;
            }
            for (int i = 0; i < 8; i++) {
                if (mx >= partChipRect[i][0] && mx < partChipRect[i][0] + partChipRect[i][2] &&
                    my >= partChipRect[i][1] && my < partChipRect[i][1] + partChipRect[i][3])
                    partToolHover = 20 + i;
            }
        }
        float etoolRect[11][4] = {};
        const int kEActs = 13;
        float eactRect[13][4] = {};
        if (ed.entityMode) {
            float bx = entToolX + 6.0f;
            float bw = entToolColW - 12.0f;
            if (bw < 40.0f) bw = 40.0f;
            float colH = paneRH - 16.0f;
            if (colH < 80.0f) colH = 80.0f;
            int actShow[13];
            int nAct = 0;
            for (int i = 0; i < kEActs; i++) actShow[nAct++] = i;
            int nSlot = kEntTools + nAct;
            float slot = colH / (float)nSlot;
            if (slot > 30.0f) slot = 30.0f;
            if (slot < 18.0f) slot = 18.0f;
            float kEToolHbtn = slot - 3.0f;
            if (kEToolHbtn < 15.0f) kEToolHbtn = 15.0f;
            float kEGap = slot - kEToolHbtn;
            float ty = kTopH + kHdrH + 8.0f;
            for (int i = 0; i < kEntTools; i++) {
                etoolRect[i][0] = bx; etoolRect[i][1] = ty;
                etoolRect[i][2] = bw; etoolRect[i][3] = kEToolHbtn;
                ty += kEToolHbtn + kEGap;
            }
            ty += 4.0f;
            for (int i = 0; i < kEActs; i++) eactRect[i][2] = 0;
            for (int s = 0; s < nAct; s++) {
                int i = actShow[s];
                eactRect[i][0] = bx; eactRect[i][1] = ty;
                eactRect[i][2] = bw; eactRect[i][3] = kEToolHbtn;
                ty += kEToolHbtn + kEGap;
            }
        }
        if (ed.modelMode && mx >= paneLX && mx < splitX && my >= kTopH + kHdrH && my < kTopH + kHdrH + kToolH) {
            for (int i = 0; i < kNMTools; i++) {
                if (mx >= mtoolRect[i][0] && mx < mtoolRect[i][0] + mtoolRect[i][2] &&
                    my >= mtoolRect[i][1] && my < mtoolRect[i][1] + mtoolRect[i][3])
                    ed.modelToolHover = i;
            }
        }
        if (ed.entityMode && mx >= entToolX && mx < entToolX + entToolColW &&
            my >= kTopH + kHdrH && my < paneRY + paneRH) {
            for (int i = 0; i < kEntTools; i++) {
                if (mx >= etoolRect[i][0] && mx < etoolRect[i][0] + etoolRect[i][2] &&
                    my >= etoolRect[i][1] && my < etoolRect[i][1] + etoolRect[i][3])
                    ed.modelToolHover = i;
            }
        }
        float arX = paneRX + paneRW - kArM - kArW;
        float arY = kTopH + (kHdrH - kArH) * 0.5f;
        float saveX = arX - 8.0f - kSaveS;
        float saveY = kTopH + (kHdrH - kSaveS) * 0.5f;
        float cancelX = saveX - 8.0f - kCancelW;
        float cancelY = kTopH + (kHdrH - kCancelH) * 0.5f;
        if (ed.modelMode) {
            float tabH = kHdrH - 4.0f;
            float tabW = 72.0f;
            float room = cancelX - 12.0f - (paneRX + 8.0f);
            if (room < tabW * 2.0f + 4.0f) tabW = std::max(46.0f, (room - 4.0f) * 0.5f);
            float tabY = kTopH + 4.0f;
            rightTab[0][0] = paneRX + 8.0f; rightTab[0][1] = tabY; rightTab[0][2] = tabW; rightTab[0][3] = tabH;
            rightTab[1][0] = paneRX + 8.0f + tabW + 4.0f; rightTab[1][1] = tabY; rightTab[1][2] = tabW; rightTab[1][3] = tabH;
            for (int i = 0; i < 2; i++) {
                if (mx >= rightTab[i][0] && mx < rightTab[i][0] + rightTab[i][2] &&
                    my >= rightTab[i][1] && my < rightTab[i][1] + rightTab[i][3])
                    rightTabHover = i;
            }
        }
        bool showSliders = ed.modelMode && ed.modelRightTab == 0 && ed.modelBlock == GRASS_TUFT;
        const float slPad = 12.0f, slRowH = 30.0f, slColGap = 16.0f;
        const float slLabelW = 132.0f, slValueW = 66.0f;
        const int slCols = 2, slRows = (kNRandSliders + slCols - 1) / slCols;
        float kSliderH = showSliders ? (slPad * 2.0f + slRows * slRowH) : 0.0f;
        float sliderTop = exY - 8.0f - kSliderH;
        if (sliderTop < paneRY + 80.0f) sliderTop = paneRY + 80.0f;
        if (!ed.entityMode) {
            paneRY = kTopH + kHdrH;
            paneRH = (showSliders ? sliderTop : (exY - 8.0f)) - paneRY;
            if (paneRH < 40.0f) paneRH = 40.0f;
        }
        if (ed.modelMode && ed.modelRightTab == 1 && paneRW > 80.0f && paneRH > 80.0f) {
            float colW = 168.0f;
            if (paneRW < 460.0f) colW = 148.0f;
            if (colW > paneRW - 80.0f) colW = paneRW - 80.0f;
            float colX = paneRX + paneRW - colW - 8.0f;
            colorViewW = colX - paneRX;
            if (colorViewW < 40.0f) colorViewW = 40.0f;
            if (colorViewW > 80.0f) {
                colorSlideW = colorViewW - 24.0f;
                if (colorSlideW > 180.0f) colorSlideW = 180.0f;
                colorSlideH = 16.0f;
                colorSlideX = paneRX + 12.0f;
                colorSlideY = paneRY + paneRH - 8.0f - colorSlideH;
            }
            float inner = colW - 16.0f;
            float gap = 6.0f;
            float toolsH = 22.0f, swH = 16.0f, alphaH = 12.0f, recH = 16.0f;
            float y = paneRY + 8.0f;
            float tw = (inner - gap * 2.0f) / 3.0f;
            for (int i = 0; i < 9; i++) {
                int col = i % 3, row = i / 3;
                colorTool[i][0] = colX + col * (tw + gap);
                colorTool[i][1] = y + row * (toolsH + gap);
                colorTool[i][2] = tw;
                colorTool[i][3] = toolsH;
            }
            y += 3.0f * (toolsH + gap);
            colorCur[0] = colX; colorCur[1] = y; colorCur[2] = inner; colorCur[3] = swH;
            y += swH + gap;
            float fixedBelow = gap + alphaH + gap + recH + 8.0f;
            float rest = paneRY + paneRH - 8.0f - y - fixedBelow;
            float sv = inner;
            float hueD = inner * 0.92f;
            if (sv + gap + hueD > rest) {
                float s = rest - gap;
                if (s < 64.0f) s = 64.0f;
                sv = s / 1.92f;
                hueD = sv * 0.92f;
            }
            colorSv[0] = colX; colorSv[1] = y; colorSv[2] = sv; colorSv[3] = sv;
            y += sv + gap;
            colorHueCX = colX + inner * 0.5f;
            colorHueCY = y + hueD * 0.5f;
            colorHueRO = hueD * 0.5f;
            colorHueRI = colorHueRO * 0.72f;
            y += hueD + gap;
            colorAlpha[0] = colX; colorAlpha[1] = y; colorAlpha[2] = inner; colorAlpha[3] = alphaH;
            y += alphaH + gap;
            float recS = recH;
            if (recS * 8.0f + 4.0f * 7.0f > inner) recS = (inner - 4.0f * 7.0f) / 8.0f;
            for (int i = 0; i < 8; i++) {
                colorRecent[i][0] = colX + i * (recS + 4.0f);
                colorRecent[i][1] = y;
                colorRecent[i][2] = recS;
                colorRecent[i][3] = recS;
            }
            for (int i = 0; i < 9; i++) {
                if (mx >= colorTool[i][0] && mx < colorTool[i][0] + colorTool[i][2] &&
                    my >= colorTool[i][1] && my < colorTool[i][1] + colorTool[i][3])
                    colorToolHover = i;
            }
            for (int i = 0; i < (int)ed.recent.size() && i < 8; i++) {
                if (mx >= colorRecent[i][0] && mx < colorRecent[i][0] + colorRecent[i][2] &&
                    my >= colorRecent[i][1] && my < colorRecent[i][1] + colorRecent[i][3])
                    colorRecentHover = i;
            }
        }
        auto sliderTrack = [&](int i, float& lx, float& ly, float& tx, float& ty, float& tw, float& th, float& vx) {
            int col = i % slCols, row = i / slCols;
            float cellW = (paneRW - slPad * 2.0f - slColGap) / (float)slCols;
            float cx = paneRX + slPad + col * (cellW + slColGap);
            ly = sliderTop + slPad + row * slRowH;
            lx = cx;
            tx = cx + slLabelW;
            ty = ly + 9.0f;
            tw = cellW - slLabelW - slValueW;
            if (tw < 8.0f) tw = 8.0f;
            th = 12.0f;
            vx = cx + cellW - slValueW;
        };
        bool applyRandHov = ed.modelMode && startMode >= 0 &&
            mx >= arX && mx < arX + kArW && my >= arY && my < arY + kArH;
        bool saveHov = ed.modelMode && startMode >= 0 &&
            mx >= saveX && mx < saveX + kSaveS && my >= saveY && my < saveY + kSaveS;
        bool cancelHov = ed.modelMode && startMode >= 0 &&
            mx >= cancelX && mx < cancelX + kCancelW && my >= cancelY && my < cancelY + kCancelH;
        bool overExit = startMode >= 0 && mx >= exX && mx < exX + kExW && my >= exY && my < exY + kExH;
        bool overBlockPanel = ed.modelMode && mx < kPanelRight;
        bool overSliders = showSliders && mx >= paneRX && my >= sliderTop && my < sliderTop + kSliderH;
        bool overRightHdr = ed.modelMode && mx >= paneRX && my >= kTopH && my < paneRY;
        bool faceHov = ed.modelMode && mx >= faceX && mx < faceX + kFaceW && my >= midBtnY && my < midBtnY + kBtnH;
        bool blkHov = ed.modelMode && mx >= blkX && mx < blkX + kBlkW && my >= midBtnY && my < midBtnY + kBtnH;
        bool smHov = ed.modelMode && mx >= smX && mx < smX + kSmW && my >= midBtnY && my < midBtnY + kBtnH;
        bool tinyHov = ed.modelMode && mx >= tinyX && mx < tinyX + kTinyW && my >= midBtnY && my < midBtnY + kBtnH;
        bool midSaveHov = (ed.modelMode || ed.entityMode) && startMode >= 0 &&
            mx >= midSaveX && mx < midSaveX + kSaveS && my >= midBtnY && my < midBtnY + kSaveS;
        bool midUndoHov = (ed.modelMode || ed.entityMode) && startMode >= 0 &&
            mx >= midUndoX && mx < midUndoX + kSaveS && my >= midBtnY && my < midBtnY + kSaveS;
        bool entCancelHov = ed.entityMode && mx >= entCancelX && mx < entCancelX + kCancelW && my >= midBtnY && my < midBtnY + kCancelH;
        auto hitEact = [&](int i) -> bool {
            return ed.entityMode && eactRect[i][2] > 1.0f &&
                mx >= eactRect[i][0] && mx < eactRect[i][0] + eactRect[i][2] &&
                my >= eactRect[i][1] && my < eactRect[i][1] + eactRect[i][3];
        };
        bool entPartHov = hitEact(0);
        bool entSelHov = hitEact(1);
        bool entPaintHov = hitEact(2);
        bool entCardHov = hitEact(3);
        bool entEraseHov = hitEact(4);
        bool entMergeHov = hitEact(5);
        bool entSplitHov = hitEact(6);
        bool entTexHov = hitEact(7);
        bool entDupHov = hitEact(8);
        bool entDelHov = hitEact(9);
        bool entCutHov = hitEact(10);
        bool entMeasureHov = hitEact(11);
        bool entPlaneHov = hitEact(12);
        const char* kFaceChipLab[4] = { "Eye L", "Eye R", "Lid L", "Lid R" };
        const char* kFaceChipTex[4] = { "eye_l", "eye_r", "eyelid_l", "eyelid_r" };
        auto faceNameAt = [&](int i) -> const char* {
            if (i >= 0 && i < (int)entFaceNames.size()) return entFaceNames[i].c_str();
            return (i >= 0 && i < 4) ? kFaceChipTex[i] : "";
        };
        const float kEntCardPad = 10.0f, kEntCardNameH = 22.0f, kEntCardGap = 10.0f;
        float faceChip[4][4] = {}, mouthChip[3][4] = {};
        struct EntCardR { float x, y, w, h; };
        std::vector<EntCardR> entCardRect;
        float entCardPrevH = 0;
        int entCardN = 0;
        float entListTop = 0, entListBot = 0, entScrollMax = 0;
        float entScrollTrack[4] = {};
        float entScrollThumbY = 0, entScrollThumbH = 0, entScrollTravel = 0;
        float entMatY = 0;
        bool hairToolsOn = false;
        bool selEyeKind = false, selMouthKind = false;
        int nMouthChip = 0, nFaceChip = 0;
        int paintTw = pm::kSkinW, paintTh = pm::kSkinH;
        unsigned paintGL = skinGL;
        mat::Image* paintImg = &skinImg;
        float paintDispW = 0, paintDispH = 0;
        float palRect[9][4] = {};
        float palS = 28.0f;
        float recX0 = 0, recY = 0, recS = 20.0f;
        float curX = 0, curY = 0, curW = 36.0f, curH = 36.0f;
        float entBmS = 32.0f;
        const float kTbS = 32.0f, kTbGap = 4.0f;
        if (ed.entityMode) {
            entCardN = entityListCount();
            float cardW = leftBarW - kEntCardPad * 2.0f;
            entCardPrevH = cardW * 1.12f;
            float cardH = entCardPrevH + kEntCardNameH;
            entListTop = kTopH + 36.0f;
            entListBot = (float)g_winH;
            float listH = entListBot - entListTop;
            if (listH < 8.0f) listH = 8.0f;
            float contentH = entCardN > 0
                ? (float)entCardN * cardH + (float)(entCardN - 1) * kEntCardGap : 0.0f;
            entScrollMax = contentH - listH;
            if (entScrollMax < 0.0f) entScrollMax = 0.0f;
            bool overEntList = mx >= 0.0f && mx < leftBarW && my >= entListTop && my < entListBot && !overExit;
            if (g_wheel != 0 && overEntList && entScrollMax > 0.0f) {
                float ticks = (float)g_wheel / 120.0f;
                ed.entScroll -= ticks * 80.0f;
                g_wheel = 0;
            }
            if (ed.entScroll < 0.0f) ed.entScroll = 0.0f;
            if (ed.entScroll > entScrollMax) ed.entScroll = entScrollMax;
            entCardRect.resize(entCardN);
            float cy = entListTop - ed.entScroll;
            for (int i = 0; i < entCardN; i++) {
                entCardRect[i].x = kEntCardPad;
                entCardRect[i].y = cy;
                entCardRect[i].w = cardW;
                entCardRect[i].h = cardH;
                cy += cardH + kEntCardGap;
            }
            if (entScrollMax > 1.0f) {
                entScrollTrack[0] = leftBarW - 14.0f;
                entScrollTrack[1] = entListTop;
                entScrollTrack[2] = 8.0f;
                entScrollTrack[3] = listH;
                entScrollThumbH = listH * (listH / (listH + entScrollMax));
                if (entScrollThumbH < 28.0f) entScrollThumbH = 28.0f;
                if (entScrollThumbH > listH) entScrollThumbH = listH;
                entScrollTravel = listH - entScrollThumbH;
                if (entScrollTravel < 1.0f) entScrollTravel = 1.0f;
                float u = ed.entScroll / entScrollMax;
                entScrollThumbY = entListTop + entScrollTravel * u;
            }
            if (!ed.entityParts.empty() && ed.entitySel >= 0 && ed.entitySel < (int)ed.entityParts.size()) {
                const pm::Part& sp = ed.entityParts[ed.entitySel];
                hairToolsOn = pm::isHairPart(sp) || pm::isHairCardPart(sp) ||
                    (pm::isDecalPart(sp) && !pm::isEyePart(sp) && !pm::isEyelidPart(sp) && !pm::isMouthPart(sp));
                selEyeKind = pm::isEyePart(sp) || pm::isEyelidPart(sp);
                selMouthKind = pm::isMouthPart(sp);
                int pk = paintKind();
                if (pk == 0) { paintTw = pm::kEyeSize; paintTh = pm::kEyeSize; paintGL = eyeGL; paintImg = &eyeImg; }
                else if (pk == 1) { paintTw = pm::kEyelidSize; paintTh = pm::kEyelidSize; paintGL = eyelidGL; paintImg = &eyelidImg; }
                else if (pk >= 2 && pk < 5) {
                    paintTw = pm::kMouthW; paintTh = pm::kMouthH;
                    paintGL = mouthGL[pk - 2];
                    paintImg = &mouthImgs[pk - 2];
                }
            }
            if (ed.hairPaint || ed.hairErase || ed.hairSelect || ed.hairCard) hairToolsOn = true;
            if (entIsCloth && skinImg.ok() && paintImg == &skinImg) {
                paintTw = skinImg.w;
                paintTh = skinImg.h;
            }
            {
                float chipH = (selEyeKind || selMouthKind) ? 48.0f : 0.0f;
                float tx = paneLX + 8.0f;
                float cy = paneLY + 8.0f;
                if (selEyeKind) {
                    nFaceChip = (int)entFaceNames.size();
                    if (nFaceChip > 4) nFaceChip = 4;
                    if (nFaceChip < 1) nFaceChip = 4;
                    for (int i = 0; i < nFaceChip; i++) {
                        faceChip[i][0] = tx; faceChip[i][1] = cy;
                        faceChip[i][2] = 64.0f; faceChip[i][3] = 40.0f;
                        tx += 68.0f;
                    }
                }
                if (selMouthKind) {
                    nMouthChip = (int)entMouthNames.size();
                    if (nMouthChip > 3) nMouthChip = 3;
                    if (nMouthChip < 1) nMouthChip = 3;
                    for (int i = 0; i < nMouthChip; i++) {
                        mouthChip[i][0] = tx; mouthChip[i][1] = cy;
                        mouthChip[i][2] = 72.0f; mouthChip[i][3] = 40.0f;
                        tx += 76.0f;
                    }
                }
                float pad = 18.0f;
                float aw = paneLW - pad * 2.0f;
                float ah = paneLH - pad * 2.0f - 36.0f - chipH;
                if (aw < 8.0f) aw = 8.0f;
                if (ah < 8.0f) ah = 8.0f;
                float scW = aw / (float)paintTw;
                float scH = ah / (float)paintTh;
                float fit = (scW < scH) ? scW : scH;
                if (ed.skinViewZoom < 1.0f) ed.skinViewZoom = 1.0f;
                float sc = fit * ed.skinViewZoom;
                paintDispW = (float)paintTw * sc;
                paintDispH = (float)paintTh * sc;
                skinImgS = paintDispW;
                float topY = paneLY + 40.0f + chipH;
                skinImgX = paneLX + (paneLW - paintDispW) * 0.5f + ed.skinPanX;
                skinImgY = topY + (ah - paintDispH) * 0.5f + ed.skinPanY;
            }
            matCols = (int)((paneLW - kMatPad * 2.0f) / (kMatThumb + kMatGap));
            if (matCols < 1) matCols = 1;
            matRows = (nMatItems + matCols - 1) / matCols;
            if (matRows < 1) matRows = 1;
            entMatY = paneLY + paneLH - kMatPad - matRows * (kMatThumb + kMatGap);
            float tbY = matBarY + 8.0f;
            if (!ed.modelMode) ed.toolHover = -1;
            float tbAvail = paneLW - 16.0f;
            float tbSlot = tbAvail / 13.0f;
            float tbS = tbSlot - kTbGap;
            if (tbS > kTbS) tbS = kTbS;
            if (tbS < 18.0f) tbS = 18.0f;
            float tbRowW = 13.0f * tbS + 12.0f * kTbGap;
            float tbX0 = paneLX + 8.0f;
            if (tbRowW < tbAvail) tbX0 += (tbAvail - tbRowW) * 0.5f;
            for (int i = 0; i < 13; i++) {
                float bx = tbX0 + i * (tbS + kTbGap);
                ed.tbRect[i][0] = bx; ed.tbRect[i][1] = tbY; ed.tbRect[i][2] = tbS; ed.tbRect[i][3] = tbS;
                if (mx >= bx && mx < bx + tbS && my >= tbY && my < tbY + tbS) ed.toolHover = i;
            }
            palS = 28.0f;
            float palY = matBarY + 50.0f;
            for (int i = 0; i < 9; i++) {
                palRect[i][0] = paneLX + 8.0f + i * (palS + 6.0f);
                palRect[i][1] = palY;
                palRect[i][2] = palS;
                palRect[i][3] = palS;
            }
            recS = 22.0f;
            recX0 = paneLX + 8.0f;
            recY = palY + palS + 8.0f;
            float x0 = palColX + 14.0f;
            float inner = palColW - 28.0f;
            if (inner < 80.0f) inner = 80.0f;
            float y = kTopH + kHdrH + 8.0f;
            curX = x0; curY = y; curW = inner; curH = 34.0f;
            y += curH + 12.0f;
            float remain = matBarY - 12.0f - y;
            float bmNeed = 0.0f;
            if (ed.tool == 1) bmNeed = (entBmS * 4.0f + 12.0f) + 58.0f;
            float hueNeed = 92.0f;
            float alNeed = 22.0f;
            float sv = remain - hueNeed - alNeed - bmNeed;
            if (sv > inner) sv = inner;
            if (sv < 56.0f) sv = 56.0f;
            ed.svRect[0] = x0 + (inner - sv) * 0.5f;
            ed.svRect[1] = y;
            ed.svRect[2] = sv;
            ed.svRect[3] = sv;
            y += sv + 12.0f;
            ed.hueCX = x0 + inner * 0.5f;
            ed.hueRO = inner * 0.42f;
            if (ed.hueRO > 44.0f) ed.hueRO = 44.0f;
            if (ed.hueRO < 28.0f) ed.hueRO = 28.0f;
            ed.hueRI = ed.hueRO * 0.62f;
            ed.hueCY = y + ed.hueRO;
            y += ed.hueRO * 2.0f + 10.0f;
            ed.alphaRect[0] = x0;
            ed.alphaRect[1] = y;
            ed.alphaRect[2] = inner;
            ed.alphaRect[3] = 12.0f;
            y += (ed.tool == 1) ? 38.0f : 22.0f;
            ed.brushMat[0] = x0;
            ed.brushMat[1] = y;
            ed.brushMat[2] = entBmS * 4.0f + 12.0f;
            ed.brushMat[3] = entBmS * 4.0f + 12.0f;
        }
        int faceChipHov = -1, mouthChipHov = -1;
        ed.entityHover = -1;
        ed.paletteHover = -1;
        if (ed.entityMode && mx < leftBarW && my >= entListTop && my < entListBot && !overExit) {
            bool overTrack = entScrollMax > 1.0f &&
                mx >= entScrollTrack[0] && mx < entScrollTrack[0] + entScrollTrack[2] &&
                my >= entScrollTrack[1] && my < entScrollTrack[1] + entScrollTrack[3];
            if (!overTrack) {
                for (int i = 0; i < entCardN; i++) {
                    if (mx >= entCardRect[i].x && mx < entCardRect[i].x + entCardRect[i].w &&
                        my >= entCardRect[i].y && my < entCardRect[i].y + entCardRect[i].h) ed.entityHover = i;
                }
            }
        }
        if (ed.entityMode && !overExit) {
            if (selEyeKind) {
                for (int i = 0; i < nFaceChip; i++)
                    if (mx >= faceChip[i][0] && mx < faceChip[i][0] + faceChip[i][2] &&
                        my >= faceChip[i][1] && my < faceChip[i][1] + faceChip[i][3]) faceChipHov = i;
            }
            if (selMouthKind) {
                for (int i = 0; i < nMouthChip; i++)
                    if (mx >= mouthChip[i][0] && mx < mouthChip[i][0] + mouthChip[i][2] &&
                        my >= mouthChip[i][1] && my < mouthChip[i][1] + mouthChip[i][3]) mouthChipHov = i;
            }
        }
        if (ed.entityMode) {
            for (int i = 0; i < 9; i++) {
                if (mx >= palRect[i][0] && mx < palRect[i][0] + palRect[i][2] &&
                    my >= palRect[i][1] && my < palRect[i][1] + palRect[i][3]) ed.paletteHover = i;
            }
        }
        auto matItemRect = [&](int i, float& x, float& y) {
            int col = i % matCols, row = i / matCols;
            if (ed.entityMode) {
                x = paneLX + kMatPad + col * (kMatThumb + kMatGap);
                y = entMatY + row * (kMatThumb + kMatGap);
            } else {
                x = paneLX + kMatPad + col * (kMatThumb + kMatGap);
                y = matBarY + kMatPad + row * (kMatThumb + kMatGap);
            }
        };
        ed.matHover = -1;
        if (ed.modelMode && mx >= paneLX && mx < splitX) {
            if (my >= matBarY && my < matBarY + kMatBarH) {
                for (int i = 0; i < nMatItems; i++) {
                    float x, y;
                    matItemRect(i, x, y);
                    if (mx >= x && mx < x + kMatThumb && my >= y && my < y + kMatThumb) ed.matHover = i;
                }
            }
        }
        if (ed.entityMode && hairToolsOn && mx >= paneLX && mx < paneLX + paneLW && my >= entMatY) {
            for (int i = 0; i < nMatItems; i++) {
                float x, y;
                matItemRect(i, x, y);
                if (mx >= x && mx < x + kMatThumb && my >= y && my < y + kMatThumb) ed.matHover = i;
            }
        }
        bool overMidHdr = (ed.modelMode || ed.entityMode) && mx >= paneLX && mx < paneLX + paneLW && my >= kTopH && my < paneLY;
        bool overMatBar = ed.modelMode && mx >= paneLX && mx < splitX && my >= matBarY;
        bool overEntBar = ed.entityMode && mx >= paneLX && my >= matBarY;
        bool overPalCol = ed.entityMode && palColW > 1.0f && mx >= palColX && mx < palColX + palColW &&
            my >= kTopH && my < matBarY;
        bool overEntTools = ed.entityMode && entToolColW > 1.0f && mx >= entToolX &&
            my >= kTopH && my < paneRY + paneRH;
        bool overEntPanel = ed.entityMode && mx < leftBarW;
        bool overHairBar = ed.entityMode && hairToolsOn && mx >= paneLX && mx < paneLX + paneLW &&
            my >= entMatY && my < paneLY + paneLH;
        float leftHitW = ed.modelMode ? viewLW : paneLW;
        bool inLeft3d = (ed.modelMode || ed.entityMode) && mx >= paneLX && mx < paneLX + leftHitW && my >= paneLY && my < paneLY + paneLH;
        float rightHitW = ed.entityMode ? entViewRW : ((ed.modelRightTab == 1 && colorViewW > 1.0f) ? colorViewW : paneRW);
        bool inRight3d = (ed.modelMode || ed.entityMode) && mx >= paneRX &&
            mx < paneRX + rightHitW &&
            my >= paneRY && my < paneRY + paneRH;
        bool togHov = ed.entityMode && mx >= togX && mx < togX + togW && my >= togY && my < togY + togH;
        bool slideHov = ed.entityMode && mx >= entSlideX - 4.0f && mx < entSlideX + kEntSlideW + 4.0f &&
            my >= entSlideY && my < entSlideY + entSlideH;
        bool paintSlideHov = paintSlideW > 1.0f &&
            mx >= paintSlideX && mx < paintSlideX + paintSlideW &&
            my >= paintSlideY && my < paintSlideY + paintSlideH;
        bool colorSlideHov = ed.modelMode && ed.modelRightTab == 1 && colorSlideW > 1.0f &&
            mx >= colorSlideX && mx < colorSlideX + colorSlideW &&
            my >= colorSlideY && my < colorSlideY + colorSlideH;

        float leftPivX = 0.0f, leftPivY = 0.5f, leftPivZ = 0.0f;
        Mat4 leftMvp;
        Mat4 colorMvp;
        Mat4 animMvp;
        {
            Mat4 o;
            o.m[0] = 2.0f / (g_winW > 0 ? g_winW : 1); o.m[5] = -2.0f / (g_winH > 0 ? g_winH : 1); o.m[10] = 1.0f;
            o.m[12] = -1.0f; o.m[13] = 1.0f; o.m[15] = 1.0f;
            leftMvp = o;
        }
        if (ed.modelMode && viewLW > 1.0f && paneLH > 1.0f) {
            bool any = false;
            float mn[3] = {}, mxv[3] = {};
            auto acc = [&](float x, float y, float z) {
                if (!any) { mn[0] = mxv[0] = x; mn[1] = mxv[1] = y; mn[2] = mxv[2] = z; any = true; }
                else {
                    if (x < mn[0]) mn[0] = x; if (x > mxv[0]) mxv[0] = x;
                    if (y < mn[1]) mn[1] = y; if (y > mxv[1]) mxv[1] = y;
                    if (z < mn[2]) mn[2] = z; if (z > mxv[2]) mxv[2] = z;
                }
            };
            if (editModel.ok()) {
                for (const mat::Quad& q : editModel.quads)
                    for (int c = 0; c < 4; c++) {
                        Vec3 v = viewOfModel(q.p[c]);
                        acc(v.x, v.y, v.z);
                    }
            } else {
                acc(-0.5f, -0.5f, -0.5f); acc(0.5f, 0.5f, 0.5f);
            }
            if (any) {
                leftPivX = 0.5f * (mn[0] + mxv[0]);
                leftPivY = 0.5f * (mn[1] + mxv[1]);
                leftPivZ = 0.5f * (mn[2] + mxv[2]);
            }
            float aspectL = viewLW / paneLH;
            leftMvp = orbitMvp(aspectL, ed.brotY, ed.brotX, ed.bzoom, leftPivX, leftPivY, leftPivZ);
            colorMvp = leftMvp;
            if (ed.modelRightTab == 1 && colorViewW > 1.0f && paneRH > 1.0f)
                colorMvp = orbitMvp(colorViewW / paneRH, ed.rrotY, ed.rrotX, ed.rzoom, leftPivX, leftPivY, leftPivZ);
        } else if (ed.entityMode && paneLW > 1.0f && paneLH > 1.0f) {
            ensureEntMark();
            bool any = false;
            float mn[3] = {}, mxv[3] = {};
            auto acc = [&](float x, float y, float z) {
                if (!any) { mn[0] = mxv[0] = x; mn[1] = mxv[1] = y; mn[2] = mxv[2] = z; any = true; }
                else {
                    if (x < mn[0]) mn[0] = x; if (x > mxv[0]) mxv[0] = x;
                    if (y < mn[1]) mn[1] = y; if (y > mxv[1]) mxv[1] = y;
                    if (z < mn[2]) mn[2] = z; if (z > mxv[2]) mxv[2] = z;
                }
            };
            bool anySel = false;
            for (int i = 0; i < (int)ed.entityParts.size(); i++)
                if (i < (int)entMark.size() && entMark[i]) { anySel = true; break; }
            bool localView = !entIsCloth || ed.clothLocal;
            for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                if (localView && anySel && (i >= (int)entMark.size() || !entMark[i])) continue;
                Vec3 cs[8];
                pm::partWorldCorners(ed.entityParts[i], cs);
                for (int k = 0; k < 8; k++) acc(cs[k].x, cs[k].y, cs[k].z);
            }
            if (!any) { acc(-0.3f, 0, -0.3f); acc(0.3f, 1.8f, 0.3f); }
            leftPivX = 0.5f * (mn[0] + mxv[0]);
            leftPivY = 0.5f * (mn[1] + mxv[1]);
            leftPivZ = 0.5f * (mn[2] + mxv[2]);
            float aspectL = paneLW / paneLH;
            leftMvp = orbitMvp(aspectL, ed.rotY, ed.rotX, ed.zoom, leftPivX, leftPivY, leftPivZ);
        }
        Mat4 rightEntMvp = leftMvp;
        float rightPivX = 0.0f, rightPivY = ed.entPivotY, rightPivZ = 0.0f;
        if (ed.entityMode && entViewRW > 1.0f && paneRH > 1.0f) {
            bool any = false;
            float mn[3] = {}, mxv[3] = {};
            auto acc = [&](float x, float y, float z) {
                if (!any) { mn[0] = mxv[0] = x; mn[1] = mxv[1] = y; mn[2] = mxv[2] = z; any = true; }
                else {
                    if (x < mn[0]) mn[0] = x; if (x > mxv[0]) mxv[0] = x;
                    if (y < mn[1]) mn[1] = y; if (y > mxv[1]) mxv[1] = y;
                    if (z < mn[2]) mn[2] = z; if (z > mxv[2]) mxv[2] = z;
                }
            };
            for (const pm::Part& p : ed.entityParts) {
                Vec3 cs[8];
                pm::partWorldCorners(p, cs);
                for (int k = 0; k < 8; k++) acc(cs[k].x, cs[k].y, cs[k].z);
            }
            if (!any) { acc(-0.3f, 0, -0.3f); acc(0.3f, 1.8f, 0.3f); }
            if (inRight3d) {
                float yaw = ed.erotY;
                float fwdX = -std::sin(yaw), fwdZ = -std::cos(yaw);
                float rgtX =  std::cos(yaw), rgtZ = -std::sin(yaw);
                float speed = ed.ezoom * 0.85f * dt;
                if (keyDown(VK_SHIFT)) speed *= 3.0f;
                float mxvX = 0.0f, mxvZ = 0.0f;
                if (keyDown('W')) { mxvX += fwdX; mxvZ += fwdZ; }
                if (keyDown('S')) { mxvX -= fwdX; mxvZ -= fwdZ; }
                if (keyDown('D')) { mxvX += rgtX; mxvZ += rgtZ; }
                if (keyDown('A')) { mxvX -= rgtX; mxvZ -= rgtZ; }
                ed.entPanX += mxvX * speed;
                ed.entPanZ += mxvZ * speed;
            }
            rightPivX = 0.5f * (mn[0] + mxv[0]) + ed.entPanX;
            rightPivZ = 0.5f * (mn[2] + mxv[2]) + ed.entPanZ;
            rightPivY = ed.entPivotY;
            float aspectR = entViewRW / paneRH;
            rightEntMvp = orbitMvp(aspectR, ed.erotY, ed.erotX, ed.ezoom, rightPivX, rightPivY, rightPivZ);
        }
        if (ed.animMode && animVW > 1.0f && animVH > 1.0f) {
            float aspect = animVW / animVH;
            animMvp = orbitMvp(aspect, ed.arotY, ed.arotX, ed.azoom, 0.0f, 0.90f, 0.0f);
        }
        auto projectOnto = [&](const Mat4& m, float ox, float oy, float ow, float oh,
                               float x, float y, float z, float& sx, float& sy) -> bool {
            Vec4 c = m * Vec4(x, y, z, 1);
            if (std::fabs(c.w) < 1e-5f) return false;
            float nx = c.x / c.w, ny = c.y / c.w;
            sx = ox + (nx * 0.5f + 0.5f) * ow;
            sy = oy + (0.5f - ny * 0.5f) * oh;
            return true;
        };
        auto projectView = [&](float x, float y, float z, float& sx, float& sy) -> bool {
            float ow = ed.modelMode ? viewLW : paneLW;
            return projectOnto(leftMvp, paneLX, paneLY, ow, paneLH, x, y, z, sx, sy);
        };
        auto projectRight = [&](float x, float y, float z, float& sx, float& sy) -> bool {
            return projectOnto(rightEntMvp, paneRX, paneRY, entViewRW, paneRH, x, y, z, sx, sy);
        };
        auto projectGizmo = [&](float x, float y, float z, float& sx, float& sy) -> bool {
            if (ed.entityMode) return projectRight(x, y, z, sx, sy);
            return projectView(x, y, z, sx, sy);
        };
        auto leftRay = [&](float px, float py, Vec3& ro, Vec3& rd) {
            float ow = ed.modelMode ? viewLW : paneLW;
            float u = (ow > 1.0f) ? (px - paneLX) / ow : 0.5f;
            float v = (paneLH > 1.0f) ? 1.0f - (py - paneLY) / paneLH : 0.5f;
            float ndcX = u * 2.0f - 1.0f, ndcY = v * 2.0f - 1.0f;
            Mat4 inv = inverse(leftMvp);
            auto unp = [&](float z) {
                Vec4 p = inv * Vec4(ndcX, ndcY, z, 1);
                float w = (std::fabs(p.w) < 1e-8f) ? 1.0f : p.w;
                return Vec3{ p.x / w, p.y / w, p.z / w };
            };
            Vec3 n = unp(-1.0f), f = unp(1.0f);
            ro = n;
            rd = (f - n).normalized();
        };
        auto colorRay = [&](float px, float py, Vec3& ro, Vec3& rd) {
            float ow = colorViewW > 1.0f ? colorViewW : paneRW;
            float u = (ow > 1.0f) ? (px - paneRX) / ow : 0.5f;
            float v = (paneRH > 1.0f) ? 1.0f - (py - paneRY) / paneRH : 0.5f;
            float ndcX = u * 2.0f - 1.0f, ndcY = v * 2.0f - 1.0f;
            Mat4 inv = inverse(colorMvp);
            auto unp = [&](float z) {
                Vec4 p = inv * Vec4(ndcX, ndcY, z, 1);
                float w = (std::fabs(p.w) < 1e-8f) ? 1.0f : p.w;
                return Vec3{ p.x / w, p.y / w, p.z / w };
            };
            Vec3 n = unp(-1.0f), f = unp(1.0f);
            ro = n;
            rd = (f - n).normalized();
        };
        auto rightRay = [&](float px, float py, Vec3& ro, Vec3& rd) {
            float u = (entViewRW > 1.0f) ? (px - paneRX) / entViewRW : 0.5f;
            float v = (paneRH > 1.0f) ? 1.0f - (py - paneRY) / paneRH : 0.5f;
            float ndcX = u * 2.0f - 1.0f, ndcY = v * 2.0f - 1.0f;
            Mat4 inv = inverse(rightEntMvp);
            auto unp = [&](float z) {
                Vec4 p = inv * Vec4(ndcX, ndcY, z, 1);
                float w = (std::fabs(p.w) < 1e-8f) ? 1.0f : p.w;
                return Vec3{ p.x / w, p.y / w, p.z / w };
            };
            Vec3 n = unp(-1.0f), f = unp(1.0f);
            ro = n;
            rd = (f - n).normalized();
        };
        const Vec3 kAxis[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        auto gizmoCenterView = [&](Vec3& g) {
            if (ed.entityMode) {
                float cx, cy, cz;
                entCenter(cx, cy, cz);
                g = { cx, cy, cz };
                return;
            }
            float cx, cy, cz;
            selCenter(cx, cy, cz);
            g = { cx - 0.5f, cy, cz };
        };
        auto gizmoSize = [&]() {
            float z = ed.entityMode ? ed.ezoom : ed.bzoom;
            float s = z * 0.16f;
            if (s < 0.22f) s = 0.22f;
            if (s > 0.65f) s = 0.65f;
            return s;
        };
        auto rotBasis = [&](int axis, Vec3& u, Vec3& v) {
            if (axis == 0) { u = { 0,1,0 }; v = { 0,0,1 }; }
            else if (axis == 1) { u = { 1,0,0 }; v = { 0,0,1 }; }
            else { u = { 1,0,0 }; v = { 0,1,0 }; }
        };
        auto hitGizmo = [&](bool wantTrans, bool wantRot) -> int {
            if (ed.entityMode) {
                if (ed.entityParts.empty()) return 0;
            } else if (!ed.modelMode) return 0;
            else if (editModel.quads.empty() &&
                     (ed.selSolid < 0 || ed.selSolid >= (int)editModel.solids.size())) return 0;
            Vec3 g; gizmoCenterView(g);
            float L = gizmoSize();
            float R = L * 0.72f;
            float best = 16.0f;
            int hit = 0;
            auto consider = [&](int id, float d) {
                if (d < best) { best = d; hit = id; }
            };
            for (int a = 0; a < 3; a++) {
                if (wantTrans) {
                    Vec3 p0 = { g.x - kAxis[a].x * L, g.y - kAxis[a].y * L, g.z - kAxis[a].z * L };
                    Vec3 p1 = { g.x + kAxis[a].x * L, g.y + kAxis[a].y * L, g.z + kAxis[a].z * L };
                    float dHead0 = 1e9f, dHead1 = 1e9f;
                    float hx, hy;
                    if (projectGizmo(p0.x, p0.y, p0.z, hx, hy)) {
                        dHead0 = std::hypot(mx - hx, my - hy);
                        if (dHead0 < 22.0f) consider(10 + a * 2 + 0, dHead0);
                    }
                    if (projectGizmo(p1.x, p1.y, p1.z, hx, hy)) {
                        dHead1 = std::hypot(mx - hx, my - hy);
                        if (dHead1 < 22.0f) consider(10 + a * 2 + 1, dHead1);
                    }
                    float sx0, sy0, sx1, sy1;
                    if (dHead0 >= 16.0f && dHead1 >= 16.0f &&
                        projectGizmo(p0.x, p0.y, p0.z, sx0, sy0) && projectGizmo(p1.x, p1.y, p1.z, sx1, sy1))
                        consider(1 + a, distPointSeg2(mx, my, sx0, sy0, sx1, sy1));
                }
                if (!wantRot) continue;
                Vec3 u, v; rotBasis(a, u, v);
                const int segs = 24;
                float a0 = -2.3f, a1 = 2.3f;
                for (int i = 0; i < segs; i++) {
                    float t0 = a0 + (a1 - a0) * (float)i / (float)segs;
                    float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)segs;
                    Vec3 q0 = { g.x + (u.x * std::cos(t0) + v.x * std::sin(t0)) * R,
                                g.y + (u.y * std::cos(t0) + v.y * std::sin(t0)) * R,
                                g.z + (u.z * std::cos(t0) + v.z * std::sin(t0)) * R };
                    Vec3 q1 = { g.x + (u.x * std::cos(t1) + v.x * std::sin(t1)) * R,
                                g.y + (u.y * std::cos(t1) + v.y * std::sin(t1)) * R,
                                g.z + (u.z * std::cos(t1) + v.z * std::sin(t1)) * R };
                    float ax, ay, bx, by;
                    if (projectGizmo(q0.x, q0.y, q0.z, ax, ay) && projectGizmo(q1.x, q1.y, q1.z, bx, by))
                        consider(4 + a, distPointSeg2(mx, my, ax, ay, bx, by));
                }
                for (int side = 0; side < 2; side++) {
                    float ang = (side == 0) ? a0 : a1;
                    Vec3 hp = { g.x + (u.x * std::cos(ang) + v.x * std::sin(ang)) * R,
                                g.y + (u.y * std::cos(ang) + v.y * std::sin(ang)) * R,
                                g.z + (u.z * std::cos(ang) + v.z * std::sin(ang)) * R };
                    float hx, hy;
                    if (projectGizmo(hp.x, hp.y, hp.z, hx, hy)) {
                        float d = std::hypot(mx - hx, my - hy);
                        if (d < 18.0f) consider(20 + a * 2 + side, d);
                    }
                }
            }
            return hit;
        };
        auto projectAnim = [&](float x, float y, float z, float& sx, float& sy) -> bool {
            Vec4 c = animMvp * Vec4(x, y, z, 1);
            if (!(c.w > 1e-4f)) return false;
            float nx = c.x / c.w, ny = c.y / c.w;
            if (nx < -1.8f || nx > 1.8f || ny < -1.8f || ny > 1.8f) return false;
            sx = animVX + (nx * 0.5f + 0.5f) * animVW;
            sy = animVY + (0.5f - ny * 0.5f) * animVH;
            return true;
        };
        auto animRay = [&](float px, float py, Vec3& ro, Vec3& rd) {
            float u = (animVW > 1.0f) ? (px - animVX) / animVW : 0.5f;
            float v = (animVH > 1.0f) ? 1.0f - (py - animVY) / animVH : 0.5f;
            float ndcX = u * 2.0f - 1.0f, ndcY = v * 2.0f - 1.0f;
            Mat4 inv = inverse(animMvp);
            auto unp = [&](float z) {
                Vec4 p = inv * Vec4(ndcX, ndcY, z, 1);
                float w = (std::fabs(p.w) < 1e-8f) ? 1.0f : p.w;
                return Vec3{ p.x / w, p.y / w, p.z / w };
            };
            Vec3 n = unp(-1.0f), f = unp(1.0f);
            ro = n;
            rd = (f - n).normalized();
        };
        auto itemModelAxes = [&](Vec3 out[3]) -> bool {
            anim::Clip viewClip = previewClipNow();
            auto pose = poseShown(viewClip, ed.animClock);
            hold::Spec spec = toolSpecAt(viewClip, ed.animClock);
            anim::BoneXform xf{};
            if (!anim::boneXformOf(viewClip, pose, spec.bone, xf)) return false;
            float Rh[9], Ri[9];
            pm::eulerToMat(spec.rot, Rh);
            pm::mat3Mul(xf.R, Rh, Ri);
            for (int a = 0; a < 3; a++) {
                out[a] = { Ri[a * 3 + 0], Ri[a * 3 + 1], Ri[a * 3 + 2] };
                if (out[a].lengthSq() < 1e-8f) out[a] = kAxis[a];
                else out[a] = out[a].normalized();
            }
            return true;
        };
        auto animGizmoCenter = [&](Vec3& g) -> bool {
            if (!ed.animMode) return false;
            anim::Clip viewClip = previewClipNow();
            auto pose = poseShown(viewClip, ed.animClock);
            if (ed.animHoldEdit || ed.animGrasp) {
                hold::Spec spec = previewToolSpec();
                anim::BoneXform xf{};
                if (!anim::boneXformOf(viewClip, pose, spec.bone, xf)) return false;
                g = hold::originOnBone(spec, xf);
                return true;
            }
            if (ed.animSelBone < 0 || ed.animSelBone >= (int)editClip.bones.size()) return false;
            if (ed.animSelBone >= (int)pose.size()) return false;
            g = pose[ed.animSelBone].pivot;
            return true;
        };
        auto animGizmoSize = [&]() {
            float s = ed.azoom * 0.14f;
            if (s < 0.18f) s = 0.18f;
            if (s > 0.55f) s = 0.55f;
            return s;
        };
        auto hitAnimGizmo = [&](bool wantTrans, bool wantRot) -> int {
            Vec3 g;
            if (!animGizmoCenter(g)) return 0;
            Vec3 axes[3] = { kAxis[0], kAxis[1], kAxis[2] };
            if (ed.animGrasp) itemModelAxes(axes);
            float L = animGizmoSize();
            float R = L * 0.72f;
            float best = 16.0f;
            int hit = 0;
            auto consider = [&](int id, float d) {
                if (d < best) { best = d; hit = id; }
            };
            for (int a = 0; a < 3; a++) {
                if (wantTrans) {
                    Vec3 p0 = { g.x - axes[a].x * L, g.y - axes[a].y * L, g.z - axes[a].z * L };
                    Vec3 p1 = { g.x + axes[a].x * L, g.y + axes[a].y * L, g.z + axes[a].z * L };
                    float dHead0 = 1e9f, dHead1 = 1e9f;
                    float hx, hy;
                    if (projectAnim(p0.x, p0.y, p0.z, hx, hy)) {
                        dHead0 = std::hypot(mx - hx, my - hy);
                        if (dHead0 < 22.0f) consider(10 + a * 2 + 0, dHead0);
                    }
                    if (projectAnim(p1.x, p1.y, p1.z, hx, hy)) {
                        dHead1 = std::hypot(mx - hx, my - hy);
                        if (dHead1 < 22.0f) consider(10 + a * 2 + 1, dHead1);
                    }
                    float sx0, sy0, sx1, sy1;
                    if (dHead0 >= 16.0f && dHead1 >= 16.0f &&
                        projectAnim(p0.x, p0.y, p0.z, sx0, sy0) && projectAnim(p1.x, p1.y, p1.z, sx1, sy1))
                        consider(1 + a, distPointSeg2(mx, my, sx0, sy0, sx1, sy1));
                }
                if (!wantRot) continue;
                Vec3 u, vv; rotBasis(a, u, vv);
                const int segs = 24;
                float a0 = -2.3f, a1 = 2.3f;
                for (int i = 0; i < segs; i++) {
                    float t0 = a0 + (a1 - a0) * (float)i / (float)segs;
                    float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)segs;
                    Vec3 q0 = { g.x + (u.x * std::cos(t0) + vv.x * std::sin(t0)) * R,
                                g.y + (u.y * std::cos(t0) + vv.y * std::sin(t0)) * R,
                                g.z + (u.z * std::cos(t0) + vv.z * std::sin(t0)) * R };
                    Vec3 q1 = { g.x + (u.x * std::cos(t1) + vv.x * std::sin(t1)) * R,
                                g.y + (u.y * std::cos(t1) + vv.y * std::sin(t1)) * R,
                                g.z + (u.z * std::cos(t1) + vv.z * std::sin(t1)) * R };
                    float ax, ay, bx, by;
                    if (projectAnim(q0.x, q0.y, q0.z, ax, ay) && projectAnim(q1.x, q1.y, q1.z, bx, by))
                        consider(4 + a, distPointSeg2(mx, my, ax, ay, bx, by));
                }
                for (int side = 0; side < 2; side++) {
                    float ang = (side == 0) ? a0 : a1;
                    Vec3 hp = { g.x + (u.x * std::cos(ang) + vv.x * std::sin(ang)) * R,
                                g.y + (u.y * std::cos(ang) + vv.y * std::sin(ang)) * R,
                                g.z + (u.z * std::cos(ang) + vv.z * std::sin(ang)) * R };
                    float hx, hy;
                    if (projectAnim(hp.x, hp.y, hp.z, hx, hy)) {
                        float d = std::hypot(mx - hx, my - hy);
                        if (d < 18.0f) consider(20 + a * 2 + side, d);
                    }
                }
            }
            return hit;
        };
        auto boneRollFrame = [&](Vec3& origin, Vec3& axis, Vec3& u, Vec3& v) -> bool {
            if (ed.animSelBone < 0 || ed.animSelBone >= (int)editClip.bones.size()) return false;
            Vec3 restO, restD;
            if (!anim::boneSegmentAxis(editClip, ed.animSelBone, restO, restD)) return false;
            anim::Clip viewClip = previewClipNow();
            auto pose = poseShown(viewClip, ed.animClock);
            if (ed.animSelBone >= (int)pose.size()) return false;
            origin = pose[ed.animSelBone].pivot;
            axis = anim::mul9(pose[ed.animSelBone].R, restD);
            if (axis.lengthSq() < 1e-10f) return false;
            axis = axis.normalized();
            u = axis.cross(Vec3{ 0, 1, 0 });
            if (u.lengthSq() < 1e-8f) u = axis.cross(Vec3{ 1, 0, 0 });
            u = u.normalized();
            v = axis.cross(u).normalized();
            int ch = anim::distalBone(editClip, ed.animSelBone);
            if (ch >= 0 && ch < (int)pose.size()) origin = (origin + pose[ch].pivot) * 0.5f;
            return true;
        };
        auto boneRollAngle = [&](const Vec3& g, const Vec3& axis, const Vec3& u, const Vec3& v,
                                 const Vec3& ro, const Vec3& rd) -> float {
            float den = axis.dot(rd);
            if (std::fabs(den) < 1e-5f) return 0.0f;
            float t = axis.dot(g - ro) / den;
            Vec3 hit = ro + rd * t;
            Vec3 d = hit - g;
            return std::atan2(d.dot(v), d.dot(u));
        };
        auto hitBoneRoll = [&]() -> int {
            Vec3 g, axis, u, v;
            if (!boneRollFrame(g, axis, u, v)) return 0;
            float R = animGizmoSize() * 0.95f;
            float best = 16.0f;
            int hit = 0;
            const int segs = 28;
            for (int i = 0; i < segs; i++) {
                float t0 = 6.2831853f * (float)i / (float)segs;
                float t1 = 6.2831853f * (float)(i + 1) / (float)segs;
                Vec3 q0 = g + (u * std::cos(t0) + v * std::sin(t0)) * R;
                Vec3 q1 = g + (u * std::cos(t1) + v * std::sin(t1)) * R;
                float ax, ay, bx, by;
                if (projectAnim(q0.x, q0.y, q0.z, ax, ay) && projectAnim(q1.x, q1.y, q1.z, bx, by)) {
                    float d = distPointSeg2(mx, my, ax, ay, bx, by);
                    if (d < best) { best = d; hit = 30; }
                }
            }
            return hit;
        };
        auto deformGeom = [&](int qi, Vec3 corner[4], Vec3 edgeMid[4], Vec3 cornerOut[4], Vec3 edgeOut[4], Vec3& faceN) -> bool {
            if (!isDeformableFace(qi)) return false;
            const mat::Quad& q = editModel.quads[qi];
            Vec3 vp[4];
            for (int c = 0; c < 4; c++) vp[c] = viewOfModel(q.p[c]);
            Vec3 n = (vp[1] - vp[0]).cross(vp[3] - vp[0]);
            if (n.lengthSq() < 1e-12f) return false;
            n = n.normalized();
            faceN = n;
            Vec3 cen = (vp[0] + vp[1] + vp[2] + vp[3]) * 0.25f;
            float off = ed.bzoom * 0.035f;
            if (off < 0.04f) off = 0.04f;
            if (off > 0.12f) off = 0.12f;
            for (int c = 0; c < 4; c++) {
                int nx = (c + 1) % 4;
                Vec3 out = vp[c] - cen;
                out = out - n * out.dot(n);
                if (out.lengthSq() < 1e-10f) out = n.cross(vp[nx] - vp[(c + 3) % 4]);
                cornerOut[c] = out.normalized();
                corner[c] = vp[c] + cornerOut[c] * off;
                edgeMid[c] = (vp[c] + vp[nx]) * 0.5f;
                Vec3 e = vp[nx] - vp[c];
                Vec3 o = n.cross(e);
                if (o.dot(edgeMid[c] - cen) < 0) o = o * -1.0f;
                edgeOut[c] = o.normalized();
                edgeMid[c] = edgeMid[c] + edgeOut[c] * off;
            }
            return true;
        };
        auto hitDeformGizmo = [&]() -> int {
            if (!ed.modelMode || (ed.modelTool != 1 && ed.modelTool != 2) || editModel.quads.empty()) return 0;
            Vec3 corner[4], mid[4], cOut[4], eOut[4], n;
            if (!deformGeom(ed.selQuad, corner, mid, cOut, eOut, n)) return 0;
            float best = 16.0f;
            int hit = 0;
            auto considerPt = [&](int id, const Vec3& p) {
                float sx, sy;
                if (!projectView(p.x, p.y, p.z, sx, sy)) return;
                float d = std::hypot(mx - sx, my - sy);
                if (d < best) { best = d; hit = id; }
            };
            float alen = ed.bzoom * 0.07f;
            if (alen < 0.06f) alen = 0.06f;
            if (alen > 0.18f) alen = 0.18f;
            for (int c = 0; c < 4; c++) {
                considerPt(40 + c, corner[c]);
                Vec3 a0 = corner[c] - cOut[c] * alen, a1 = corner[c] + cOut[c] * alen;
                float sx0, sy0, sx1, sy1;
                if (projectView(a0.x, a0.y, a0.z, sx0, sy0) && projectView(a1.x, a1.y, a1.z, sx1, sy1)) {
                    float d = distPointSeg2(mx, my, sx0, sy0, sx1, sy1);
                    if (d < best) { best = d; hit = 40 + c; }
                }
                considerPt(50 + c, mid[c]);
                Vec3 b0 = mid[c] - eOut[c] * alen, b1 = mid[c] + eOut[c] * alen;
                if (projectView(b0.x, b0.y, b0.z, sx0, sy0) && projectView(b1.x, b1.y, b1.z, sx1, sy1)) {
                    float d = distPointSeg2(mx, my, sx0, sy0, sx1, sy1);
                    if (d < best) { best = d; hit = 50 + c; }
                }
            }
            return hit;
        };
        auto uvGeom = [&](int qi, Vec3& origin, Vec3& uDir, Vec3& vDir) -> bool {
            if (qi < 0 || qi >= (int)editModel.quads.size()) return false;
            const mat::Quad& q = editModel.quads[qi];
            Vec3 vp[4];
            for (int c = 0; c < 4; c++) vp[c] = viewOfModel(q.p[c]);
            origin = (vp[0] + vp[1] + vp[2] + vp[3]) * 0.25f;
            uDir = (vp[1] - vp[0]);
            vDir = (vp[3] - vp[0]);
            if (uDir.lengthSq() < 1e-10f || vDir.lengthSq() < 1e-10f) return false;
            uDir = uDir.normalized();
            vDir = vDir.normalized();
            return true;
        };
        auto hitUVGizmo = [&]() -> int {
            Vec3 o, u, v;
            bool ok = false;
            if (ed.entityMode) {
                if (ed.modelTool != 5) return 0;
                auto idx = geomSelected();
                if (idx.empty()) return 0;
                int pi = ed.entitySel;
                if (pi < 0 || pi >= (int)ed.entityParts.size() ||
                    std::find(idx.begin(), idx.end(), pi) == idx.end())
                    pi = idx[0];
                const pm::Part& p = ed.entityParts[pi];
                if (!p.tex.empty() && !pm::isHairPart(p)) {
                    Vec3 q[4];
                    pm::texQuadLocal(p, q);
                    o = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
                    u = q[1] - q[0];
                    v = q[3] - q[0];
                } else {
                    o = p.center;
                    u = { 1, 0, 0 };
                    v = { 0, 1, 0 };
                }
                ok = u.lengthSq() > 1e-10f && v.lengthSq() > 1e-10f;
                if (ok) { u = u.normalized(); v = v.normalized(); }
            } else {
                if (!ed.modelMode || ed.modelTool != 5 || editModel.quads.empty()) return 0;
                ok = uvGeom(ed.selQuad, o, u, v);
            }
            if (!ok) return 0;
            float L = gizmoSize() * 0.55f;
            float best = 16.0f;
            int hit = 0;
            auto considerSeg = [&](int id, const Vec3& a, const Vec3& b) {
                float sx0, sy0, sx1, sy1;
                if (!projectGizmo(a.x, a.y, a.z, sx0, sy0) || !projectGizmo(b.x, b.y, b.z, sx1, sy1)) return;
                float d = distPointSeg2(mx, my, sx0, sy0, sx1, sy1);
                if (d < best) { best = d; hit = id; }
                float hx, hy;
                if (projectGizmo(a.x, a.y, a.z, hx, hy) && std::hypot(mx - hx, my - hy) < 18.0f) {
                    float dh = std::hypot(mx - hx, my - hy);
                    if (dh < best) { best = dh; hit = id; }
                }
                if (projectGizmo(b.x, b.y, b.z, hx, hy) && std::hypot(mx - hx, my - hy) < 18.0f) {
                    float dh = std::hypot(mx - hx, my - hy);
                    if (dh < best) { best = dh; hit = id; }
                }
            };
            considerSeg(60, o - u * L, o + u * L);
            considerSeg(61, o - v * L, o + v * L);
            return hit;
        };
        auto rayPlaneHit = [](const Vec3& ro, const Vec3& rd, const Vec3& p0, const Vec3& n, Vec3& hit) -> bool {
            float den = n.dot(rd);
            if (std::fabs(den) < 1e-6f) return false;
            float t = n.dot(p0 - ro) / den;
            if (t < 0.001f) return false;
            hit = ro + rd * t;
            return true;
        };
        bool canEntGizmo = ed.entityMode && !geomSelected().empty();
        bool entGzMove = ed.modelTool == 0;
        bool entGzStretch = ed.modelTool == 1;
        bool entGzFill = ed.modelTool == 2;
        bool entGzRot = ed.modelTool == 3;
        bool entGzMirror = ed.modelTool == 4;
        bool entGzUV = ed.modelTool == 5;
        auto hitRefLines2d = [&](float pmx, float pmy) -> int {
            if (ed.refDecalIdx < 0 || ed.refDecalIdx >= (int)ed.entityParts.size()) return -1;
            if (ed.refEdge >= 0) return -1;
            Vec3 q[4];
            pm::texQuadLocal(ed.entityParts[ed.refDecalIdx], q);
            float extend = std::max(1.2f, gizmoSize() * 5.0f);
            float best = 14.0f;
            int hit = -1;
            for (int e = 0; e < 4; e++) {
                Vec3 a, b;
                decalEdgeExtended(q, e, extend, a, b);
                float sx0, sy0, sx1, sy1;
                if (!projectRight(a.x, a.y, a.z, sx0, sy0) || !projectRight(b.x, b.y, b.z, sx1, sy1)) continue;
                float d = distPointSeg2(pmx, pmy, sx0, sy0, sx1, sy1);
                if (d < best) { best = d; hit = e; }
            }
            return hit;
        };
        ed.refLineHover = -1;
        if (ed.entityMode && inRight3d && ed.refTool && ed.refDecalIdx >= 0 && ed.refEdge < 0)
            ed.refLineHover = hitRefLines2d(mx, my);
        if (ed.entityMode) {
            if (inRight3d && canEntGizmo && !slideHov && !ed.hairPaint && !ed.hairErase && !ed.hairSelect && !ed.hairCard) {
                if (entGzUV) ed.gizmoHover = hitUVGizmo();
                else if (entGzRot) ed.gizmoHover = hitGizmo(false, true);
                else if (entGzMove) ed.gizmoHover = hitGizmo(true, true);
                else if (entGzStretch || entGzFill || entGzMirror)
                    ed.gizmoHover = hitGizmo(true, false);
                else ed.gizmoHover = 0;
            } else ed.gizmoHover = 0;
        }         else if (inLeft3d && ed.modelTool == 0) ed.gizmoHover = hitGizmo(true, true);
        else if (inLeft3d && ed.modelTool == 3) ed.gizmoHover = hitGizmo(false, true);
        else if (inLeft3d && ed.modelTool == 4) ed.gizmoHover = hitGizmo(true, false);
        else if (inLeft3d && (ed.modelTool == 1 || ed.modelTool == 2)) {
            if (isDeformableFace(ed.selQuad)) ed.gizmoHover = hitDeformGizmo();
            else if (ed.modelTool == 1 && isSolidGroup(ed.selQuad)) ed.gizmoHover = hitGizmo(true, false);
            else ed.gizmoHover = 0;
        }
        else if (inLeft3d && ed.modelTool == 5) ed.gizmoHover = hitUVGizmo();
        else ed.gizmoHover = 0;
        bool animBonePick = ed.animMode && !ed.animMask && (ed.animTool == 2 || keyDown(VK_SHIFT));
        if (ed.animMode) {
            if (!(inAnim3d && !ed.animMask && !animBonePick && (ed.animHoldEdit || ed.animGrasp || ed.animSelBone >= 0)))
                ed.gizmoHover = 0;
            else if (ed.animTool == 3 && !ed.animHoldEdit && !ed.animGrasp)
                ed.gizmoHover = hitBoneRoll();
            else
                ed.gizmoHover = hitAnimGizmo(ed.animGrasp || ed.animTool == 0, !ed.animGrasp && ed.animTool == 1);
        }
        auto gizmoAxisOf = [](int id) -> int {
            if (id >= 1 && id <= 3) return id - 1;
            if (id >= 4 && id <= 6) return id - 4;
            if (id >= 10 && id <= 15) return (id - 10) / 2;
            if (id >= 20 && id <= 25) return (id - 20) / 2;
            return 0;
        };
        auto gizmoIsRot = [](int id) { return (id >= 4 && id <= 6) || (id >= 20 && id <= 25); };
        auto gizmoIsHead = [](int id) { return (id >= 10 && id <= 15) || (id >= 20 && id <= 25); };
        auto gizmoHeadSign = [](int id) -> float {
            if (id >= 10 && id <= 15) return ((id - 10) % 2) ? 1.0f : -1.0f;
            if (id >= 20 && id <= 25) return ((id - 20) % 2) ? 1.0f : -1.0f;
            return 1.0f;
        };
        auto rotAngleAt = [&](int axis, const Vec3& g, const Vec3& ro, const Vec3& rd) -> float {
            Vec3 u, v; rotBasis(axis, u, v);
            Vec3 n = kAxis[axis];
            float den = n.dot(rd);
            if (std::fabs(den) < 1e-5f) return 0.0f;
            float t = n.dot(g - ro) / den;
            Vec3 hit = ro + rd * t;
            Vec3 d = hit - g;
            return std::atan2(d.dot(v), d.dot(u));
        };

        // mouse wheel: zoom the 3D view in model modes, or the canvas in texture mode.
        if (g_wheel != 0) {
            if (ed.modelMode && ed.modelTool == 7 && inLeft3d) {
                stepTexScaleSel(g_wheel > 0 ? 1 : -1);
                g_wheel = 0;
            } else if (ed.modelMode) {
                if (inRight3d || inLeft3d) {
                    float& z = inRight3d ? ed.rzoom : ed.bzoom;
                    z *= std::pow(1.55f, (float)g_wheel / 120.0f);
                    if (z < 0.12f) z = 0.12f;
                    if (z > 48.0f) z = 48.0f;
                }
            } else if (ed.entityMode) {
                if (inRight3d) {
                    ed.ezoom *= std::pow(1.55f, (float)g_wheel / 120.0f);
                    if (ed.ezoom < 0.12f) ed.ezoom = 0.12f;
                    if (ed.ezoom > 48.0f) ed.ezoom = 48.0f;
                } else if (inLeft3d && !ed.entSkinView && !togHov) {
                    ed.zoom *= std::pow(1.55f, (float)g_wheel / 120.0f);
                    if (ed.zoom < 0.12f) ed.zoom = 0.12f;
                    if (ed.zoom > 48.0f) ed.zoom = 48.0f;
                } else if (inLeft3d && (ed.entSkinView || paintKind() >= 0) && !togHov) {
                    float oldZ = ed.skinViewZoom;
                    float u = (paintDispW > 1.0f) ? (mx - skinImgX) / paintDispW : 0.5f;
                    float v = (paintDispH > 1.0f) ? (my - skinImgY) / paintDispH : 0.5f;
                    ed.skinViewZoom *= (g_wheel > 0) ? 1.12f : (1.0f / 1.12f);
                    if (ed.skinViewZoom < 1.0f) ed.skinViewZoom = 1.0f;
                    if (ed.skinViewZoom > 12.0f) ed.skinViewZoom = 12.0f;
                    float ratio = ed.skinViewZoom / oldZ;
                    ed.skinPanX += paintDispW * (ratio - 1.0f) * (0.5f - u);
                    ed.skinPanY += paintDispH * (ratio - 1.0f) * (0.5f - v);
                    if (ed.skinViewZoom <= 1.001f) { ed.skinPanX = 0.0f; ed.skinPanY = 0.0f; }
                }
            } else if (!ed.animMode) {
                ed.canvasZoom *= (g_wheel > 0) ? 1.2f : (1.0f / 1.2f);
                if (ed.canvasZoom < 0.25f) ed.canvasZoom = 0.25f;
                if (ed.canvasZoom > 3.0f) ed.canvasZoom = 3.0f;
            }
            g_wheel = 0;
        }

        // ---- input ----
        if (!editorFocused()) g_wheel = 0;
        bool lmb = keyDown(VK_LBUTTON), rmb = keyDown(VK_RBUTTON), mmb = keyDown(VK_MBUTTON);
        static bool prevLmb = false, prevRmb = false, prevMmb = false;
        static float lastMx = 0, lastMy = 0; // previous mouse pos for right-drag orbit
        static bool prevTab = false, prevLbrack = false, prevRbrack = false, prevF5 = false;

        bool tab = keyDown(VK_TAB);
        if (tab && !prevTab && startMode >= 0) {
            if (ed.modelMode) bakeFillIfLeaving();
            if (ed.animMode) { ed.animMode = false; ed.animPlaying = false; }
            else if (!ed.modelMode && !ed.entityMode) { syncTilesToGL(); ed.modelMode = true; }
            else if (ed.modelMode) { ed.modelMode = false; ed.entityMode = true; }
            else { ed.entityMode = false; enterAnimMode(); }
            clearEditorTools();
        }
        prevTab = tab;

        if (startMode < 0) {
            // ---- mode chooser (entry screen): pick texture / block / entity ----
            const float bw = 380.0f, bh = 64.0f, gap = 20.0f;
            const int nOpts = 6;
            const float totalH = nOpts * bh + (nOpts - 1) * gap;
            const float bx = (g_winW - bw) * 0.5f;
            const float y0 = (g_winH - totalH) * 0.5f - 50.0f;
            int choice = -1;
            for (int i = 0; i < nOpts; i++) {
                float by = y0 + i * (bh + gap);
                if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) choice = i;
            }
            if (lmb && !prevLmb && choice >= 0) {
                if (choice == 5) {
                    launchStructureEditor();
                } else if (choice == 4) {
                    dataed::runModal(g_hwnd);
                } else {
                    startMode = choice;
                    clearEditorTools();
                    if (choice == 1) { syncTilesToGL(); ed.modelMode = true; }
                    else if (choice == 2) ed.entityMode = true;
                    else if (choice == 3) enterAnimMode();
                }
            }
        } else if (ed.animMode) {
            auto hitR = [&](const float r[4]) {
                return r[2] > 1.0f && mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3];
            };
            ed.animClipHover = -1;
            ed.animBoneHover = -1;
            ed.animBindHover = -1;
            ed.animBtnHover = -1;
            auto hitCol = [&](const float r[4], bool left) {
                if (!hitR(r)) return false;
                float x0 = left ? 0.0f : (float)g_winW - animRightW;
                float x1 = left ? animLeftW : (float)g_winW;
                if (mx < x0 || mx >= x1 || my < animPaneTop || my >= animPaneBot) return false;
                if (r[1] + r[3] <= animPaneTop || r[1] >= animPaneBot) return false;
                return true;
            };
            for (int i = 0; i < animClipN; i++) if (hitCol(animClipRect[i], true)) ed.animClipHover = i;
            for (int i = 0; i < animBoneN; i++) if (hitCol(animBoneRect[i], true)) ed.animBoneHover = i;
            for (int i = 0; i < animBindN; i++) if (hitCol(animBindRect[i], false)) ed.animBindHover = i;
            for (int i = 0; i < 13; i++) if (hitR(animBtnRect[i])) ed.animBtnHover = i;
            if (lmb && !prevLmb) {
                bool scaleClick = hitR(animScaleRect[0]) || hitR(animScaleRect[1])
                    || hitR(animScaleRect[2]) || hitR(animScaleRect[3]);
                if (!scaleClick) scaleHoldOn = false;
                if (ed.animClipHover >= 0 && ed.animClipHover < (int)animNames.size())
                    loadAnimClip(animNames[ed.animClipHover]);
                else if (ed.animBoneHover >= 0) ed.animSelBone = ed.animBoneHover;
                else if (ed.animBindHover >= 0 && ed.animBindHover < (int)entNames.size())
                    loadAnimBind(entNames[ed.animBindHover]);
                else if (hitCol(animNewRect, true)) newAnimClip();
                else if (hitCol(animDupRect, true)) dupAnimClip();
                else if (hitCol(animDelRect, true)) delAnimClip();
                else if (hitR(animSaveRect)) saveAnimClip();
                else if (hitR(animModelSave)) saveAnimModel();
                else if (hitR(animUndoRect)) undoAnim();
                else if (hitCol(animRebuildRect, false)) {
                    pushAnimUndo();
                    if (animBindParts.empty()) loadAnimBind(animBindName);
                    std::string keep = editClip.name;
                    auto tracks = editClip.tracks;
                    editClip = anim::rigFromParts(animBindParts, animBindName);
                    editClip.name = keep;
                    editClip.tracks = std::move(tracks);
                    anim::ensurePalmBones(editClip, animBindParts);
                    ed.animDirty = true;
                }
                else if (hitCol(animHoldPrev, false) || hitCol(animHoldNext, false)) {
                    int dir = hitCol(animHoldNext, false) ? 1 : -1;
                    int n = (int)(sizeof(kHoldItems) / sizeof(kHoldItems[0]));
                    int idx = 0;
                    for (int i = 0; i < n; i++) if (kHoldItems[i] == ed.animHoldItem) { idx = i; break; }
                    idx = (idx + dir + n) % n;
                    ed.animHoldItem = kHoldItems[idx];
                }
                else if (hitCol(animHoldL, false)) ed.animHoldSide = -1;
                else if (hitCol(animHoldR, false)) ed.animHoldSide = 1;
                else if (hitCol(animHoldGrip, false)) { ed.animHoldEdit = !ed.animHoldEdit; if (ed.animHoldEdit) ed.animGrasp = false; }
                else if (hitCol(animHoldGrasp, false)) { ed.animGrasp = !ed.animGrasp; if (ed.animGrasp) ed.animHoldEdit = false; }
                else if (hitCol(animLockDir, false)) ed.animLockDir = !ed.animLockDir;
                else if (hitCol(animLockPos, false)) ed.animLockPos = !ed.animLockPos;
                else if (hitCol(animHoldLower, false) || hitCol(animHoldHigher, false)) {
                    pushAnimUndo();
                    float step = keyDown(VK_SHIFT) ? 0.10f : 0.02f;
                    float dy = hitCol(animHoldHigher, false) ? step : -step;
                    anim::Clip view = previewClipNow();
                    Vec3 g = anim::evalGrip(view, (float)ed.animFrame, previewHoldSpec().grip);
                    g.y += dy;
                    anim::setGripKey(view, ed.animFrame, g, previewHoldSpec().grip);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animPlaying = false;
                }
                else if (hitCol(animHoldBone, false)) {
                    if (ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()) {
                        pushAnimUndo();
                        editableHoldSpec().bone = editClip.bones[ed.animSelBone].name;
                        ed.animHoldDirty = true;
                    }
                }
                else if (hitCol(animHoldSave, false)) saveHoldFile();
                else if (ed.animBtnHover == 0) {
                    std::string bn = ed.animGrasp ? std::string(anim::kGripTrack)
                        : ((ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size())
                        ? viewBoneName(editClip.bones[ed.animSelBone].name) : std::string{});
                    int p = anim::prevKeyFrame(editClip, bn, ed.animFrame);
                    if (p >= 0) { ed.animFrame = p; ed.animClock = (float)p; }
                } else if (ed.animBtnHover == 1) {
                    ed.animPlaying = !ed.animPlaying;
                } else if (ed.animBtnHover == 2) {
                    std::string bn = ed.animGrasp ? std::string(anim::kGripTrack)
                        : ((ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size())
                        ? viewBoneName(editClip.bones[ed.animSelBone].name) : std::string{});
                    int n = anim::nextKeyFrame(editClip, bn, ed.animFrame);
                    ed.animFrame = n; ed.animClock = (float)n;
                } else if (ed.animBtnHover == 3) {
                    ed.animPlaying = false; ed.animFrame = 0; ed.animClock = 0;
                }                 else if (ed.animBtnHover == 4) {
                    if (ed.animGrasp || (ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size())) {
                        pushAnimUndo();
                        anim::Clip view = previewClipNow();
                        if (ed.animGrasp) {
                            hold::Spec spec = previewHoldSpec();
                            Vec3 g = anim::evalGrip(view, (float)ed.animFrame, spec.grip);
                            anim::setGripKey(view, ed.animFrame, g, spec.grip);
                        } else {
                            auto pose = poseShown(view, (float)ed.animFrame);
                            anim::writeShownPose(view, ed.animFrame, pose);
                        }
                        storeViewClip(std::move(view));
                        ed.animDirty = true;
                    }
                } else if (ed.animBtnHover == 5) {
                    if (ed.animGrasp || (ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size())) {
                        pushAnimUndo();
                        anim::Clip view = previewClipNow();
                        if (ed.animGrasp) anim::deleteKey(view, anim::kGripTrack, ed.animFrame);
                        else anim::deleteKey(view, view.bones[ed.animSelBone].name, ed.animFrame);
                        storeViewClip(std::move(view));
                        ed.animDirty = true;
                    }
                } else if (ed.animBtnHover == 6) ed.animView = 0;
                else if (ed.animBtnHover == 7) ed.animView = 1;
                else if (ed.animBtnHover == 8) ed.animTool = 0;
                else if (ed.animBtnHover == 9) ed.animTool = 1;
                else if (ed.animBtnHover == 10) ed.animTool = 2;
                else if (ed.animBtnHover == 12) {
                    if (ed.animTool == 3) ed.animRollChain = !ed.animRollChain;
                    else ed.animTool = 3;
                }
                else if (ed.animBtnHover == 11) ed.animMask = !ed.animMask;
                else if (hitR(animMaskSwatch[0]) || hitR(animMaskSwatch[1]) || hitR(animMaskSwatch[2])
                      || hitR(animMaskSwatch[3]) || hitR(animMaskSwatch[4]) || hitR(animMaskSwatch[5])) {
                    for (int i = 0; i < 6; i++) if (hitR(animMaskSwatch[i])) ed.animMaskColor = i;
                }
                else if (hitR(animKeyOp[0])) {
                    pushAnimUndo();
                    anim::Clip view = previewClipNow();
                    anim::blankKeyframe(view, ed.animFrame, previewHoldSpec().grip);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animPlaying = false;
                } else if (hitR(animKeyOp[1])) {
                    poseCopy.bones.clear();
                    poseCopy.keys.clear();
                    anim::Clip view = previewClipNow();
                    auto shown = poseShown(view, (float)ed.animFrame);
                    for (int i = 0; i < (int)view.bones.size(); i++) {
                        poseCopy.bones.push_back(view.bones[i].name);
                        poseCopy.keys.push_back(anim::keyFromWorld(view, i, shown));
                    }
                    poseCopy.grip = anim::evalGrip(view, (float)ed.animFrame, previewHoldSpec().grip);
                    poseCopy.face = anim::evalFace(view, (float)ed.animFrame);
                    poseCopy.faceOff = anim::evalFaceOff(view, (float)ed.animFrame);
                    poseCopy.has = !poseCopy.bones.empty();
                } else if (hitR(animKeyOp[2]) && poseCopy.has) {
                    pushAnimUndo();
                    anim::Clip view = previewClipNow();
                    anim::writePose(view, ed.animFrame, poseCopy.bones, poseCopy.keys, poseCopy.grip, previewHoldSpec().grip);
                    anim::setFaceKey(view, ed.animFrame, poseCopy.faceOff, poseCopy.face);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animPlaying = false;
                }                 else if (hitR(animKeyOp[3])) {
                    pushAnimUndo();
                    anim::Clip view = previewClipNow();
                    hold::Spec spec = previewHoldSpec();
                    auto pose = poseShown(view, (float)ed.animFrame);
                    Vec3 g = anim::evalGrip(view, (float)ed.animFrame, spec.grip);
                    anim::writeShownPose(view, ed.animFrame, pose);
                    anim::setGripKey(view, ed.animFrame, g, spec.grip);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animPlaying = false;
                } else if (hitR(animKeyOp[4])) {
                    int lo = ed.animSpanA, hi = ed.animSpanB;
                    bool span = lo >= 0 && hi >= 0 && lo != hi;
                    if (span) {
                        anim::Clip view = previewClipNow();
                        if (anim::deleteKeyframeSpan(view, lo, hi) > 0) {
                            pushAnimUndo();
                            storeViewClip(std::move(view));
                            ed.animDirty = true;
                            ed.animPlaying = false;
                        }
                    } else if (anim::frameHasKeys(editClip, ed.animFrame)) {
                        pushAnimUndo();
                        anim::Clip view = previewClipNow();
                        anim::deleteKeyframe(view, ed.animFrame);
                        storeViewClip(std::move(view));
                        ed.animDirty = true;
                        ed.animPlaying = false;
                    }
                } else if (hitR(animKeyOp[5]) || hitR(animKeyOp[6])) {
                    int step = keyDown(VK_SHIFT) ? 5 : 1;
                    int dir = hitR(animKeyOp[6]) ? 1 : -1;
                    int lo = ed.animSpanA, hi = ed.animSpanB;
                    bool span = lo >= 0 && hi >= 0 && lo != hi;
                    if (span) {
                        anim::Clip view = previewClipNow();
                        int na = lo, nb = hi;
                        if (anim::moveKeyframeSpan(view, lo, hi, dir * step, na, nb)) {
                            pushAnimUndo();
                            storeViewClip(std::move(view));
                            ed.animSpanA = na;
                            ed.animSpanB = nb;
                            ed.animFrame = nb;
                            ed.animClock = (float)nb;
                            ed.animDirty = true;
                            ed.animPlaying = false;
                        }
                    } else {
                        int to = ed.animFrame + dir * step;
                        if (to < 0) to = 0;
                        if (editClip.length > 0 && to >= editClip.length) to = editClip.length - 1;
                        if (to != ed.animFrame && anim::frameHasKeys(editClip, ed.animFrame)) {
                            pushAnimUndo();
                            anim::Clip view = previewClipNow();
                            if (anim::moveKeyframe(view, ed.animFrame, to)) {
                                storeViewClip(std::move(view));
                                ed.animFrame = to;
                                ed.animClock = (float)to;
                                ed.animDirty = true;
                                ed.animPlaying = false;
                            }
                        }
                    }
                } else if (hitR(animKeyOp[7])) {
                    ed.animSpanA = ed.animFrame;
                } else if (hitR(animKeyOp[8])) {
                    ed.animSpanB = ed.animFrame;
                } else if (hitR(animKeyOp[9])) {
                    if (ed.animSpanA >= 0 && ed.animSpanB >= 0 && ed.animSpanA != ed.animSpanB) {
                        anim::Clip view = previewClipNow();
                        if (anim::clearToolSpin(view, previewToolSpec().bone, ed.animSpanA, ed.animSpanB)) {
                            pushAnimUndo();
                            storeViewClip(std::move(view));
                            ed.animDirty = true;
                            ed.animPlaying = false;
                        }
                    }
                } else if (hitR(animTurnRect[0]) || hitR(animTurnRect[1]) || hitR(animTurnRect[2])) {
                    anim::Clip view = previewClipNow();
                    std::vector<int> marks;
                    anim::armKeyFrames(view, previewToolSpec().bone, marks);
                    int lo = 0, hi = 0;
                    bool span = ed.animSpanA >= 0 && ed.animSpanB >= 0 && ed.animSpanA != ed.animSpanB;
                    if (span) {
                        lo = ed.animSpanA < ed.animSpanB ? ed.animSpanA : ed.animSpanB;
                        hi = ed.animSpanA < ed.animSpanB ? ed.animSpanB : ed.animSpanA;
                    }
                    int sense = 1, laps = 0;
                    bool have = false;
                    for (int i = 0; i + 1 < (int)marks.size(); i++) {
                        int a = marks[i], b = marks[i + 1];
                        bool use = span ? (a >= lo && b <= hi) : (ed.animFrame > a && ed.animFrame < b);
                        if (!use) continue;
                        int ss = 1, ll = 0;
                        anim::toolTurnOf(view, a, b, ss, ll);
                        if (!have) { sense = ss; laps = ll; have = true; }
                    }
                    if (hitR(animTurnRect[0])) sense = -sense;
                    else if (hitR(animTurnRect[1])) { if (laps > 0) laps--; }
                    else if (laps < 8) laps++;
                    int n = 0;
                    for (int i = 0; i + 1 < (int)marks.size(); i++) {
                        int a = marks[i], b = marks[i + 1];
                        bool use = span ? (a >= lo && b <= hi) : (ed.animFrame > a && ed.animFrame < b);
                        if (!use) continue;
                        anim::upsertToolTurn(view, a, b, sense, laps);
                        n++;
                    }
                    if (n > 0) {
                        pushAnimUndo();
                        storeViewClip(std::move(view));
                        ed.animDirty = true;
                        ed.animPlaying = false;
                    }
                } else if (hitR(animFlipRect[0]) || hitR(animFlipRect[1]) || hitR(animFlipRect[2])) {
                    int axis = hitR(animFlipRect[1]) ? 1 : hitR(animFlipRect[2]) ? 2 : 0;
                    anim::Clip view = previewClipNow();
                    Vec3 face = anim::evalFace(view, (float)ed.animFrame);
                    float R[9], Spin[9], Out[9];
                    pm::eulerToMat(face, R);
                    Vec3 ax{ 1, 0, 0 };
                    if (axis == 1) ax = { 0, 1, 0 };
                    else if (axis == 2) ax = { 0, 0, 1 };
                    pm::axisAngleMat(ax, 3.14159265f, Spin);
                    pm::mat3Mul(R, Spin, Out);
                    anim::setFaceKey(view, ed.animFrame, anim::evalFaceOff(view, (float)ed.animFrame), pm::matToEuler(Out));
                    pushAnimUndo();
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animPlaying = false;
                }
                else if (hitR(animLenMinus) && editClip.length > 2) {
                    pushAnimUndo();
                    editClip.length--;
                    anim::clampClipFrames(editClip);
                    int last = editClip.length - 1;
                    if (ed.animFrame > last) ed.animFrame = last;
                    if (ed.animSpanA > last) ed.animSpanA = last;
                    if (ed.animSpanB > last) ed.animSpanB = last;
                    ed.animClock = (float)ed.animFrame;
                    ed.animDirty = true;
                    layoutAnimBar();
                }
                else if (hitR(animLenPlus) && editClip.length < 240) { pushAnimUndo(); editClip.length++; ed.animDirty = true; layoutAnimBar(); }
                else if (hitR(animScaleRect[0]) || hitR(animScaleRect[1]) || hitR(animScaleRect[2]) || hitR(animScaleRect[3])) {
                    int which = hitR(animScaleRect[0]) ? 0 : hitR(animScaleRect[1]) ? 1 : hitR(animScaleRect[2]) ? 2 : 3;
                    bool step = (which == 1 || which == 2);
                    if (step && !scaleHoldOn) {
                        scaleHoldOn = true;
                        scaleHoldClip = editClip;
                        scaleHoldLen = editClip.length;
                        scaleHoldTarget = editClip.length;
                        scaleHoldFrame = ed.animFrame;
                        scaleHoldSpanA = ed.animSpanA;
                        scaleHoldSpanB = ed.animSpanB;
                        scaleHoldClock = ed.animClock;
                    }
                    int target = editClip.length;
                    if (which == 0) target = std::max(2, editClip.length / 2);
                    else if (which == 3) target = std::min(240, editClip.length * 2);
                    else if (scaleHoldOn) target = scaleHoldTarget + (which == 2 ? 1 : -1);
                    if (target < 2) target = 2;
                    if (target > 240) target = 240;
                    if (target != (step ? scaleHoldTarget : editClip.length)) {
                        AnimSnap snap;
                        snap.clip = editClip;
                        snap.hold = editHold;
                        snap.parts = animBindParts;
                        snap.frame = ed.animFrame;
                        snap.clock = ed.animClock;
                        snap.selBone = ed.animSelBone;
                        snap.dirty = ed.animDirty;
                        snap.holdDirty = ed.animHoldDirty;
                        snap.modelDirty = ed.animModelDirty;
                        animUndo.push_back(std::move(snap));
                        if (animUndo.size() > 48) animUndo.erase(animUndo.begin());
                        int srcLen = editClip.length;
                        int srcFrame = ed.animFrame;
                        int srcA = ed.animSpanA, srcB = ed.animSpanB;
                        float srcClock = ed.animClock;
                        if (step && scaleHoldOn) {
                            editClip = scaleHoldClip;
                            srcLen = scaleHoldLen;
                            srcFrame = scaleHoldFrame;
                            srcA = scaleHoldSpanA;
                            srcB = scaleHoldSpanB;
                            srcClock = scaleHoldClock;
                            scaleHoldTarget = target;
                        } else {
                            scaleHoldOn = false;
                        }
                        anim::scaleClipLength(editClip, target);
                        int oldLast = srcLen > 1 ? srcLen - 1 : 0;
                        int newLast = editClip.length > 1 ? editClip.length - 1 : 0;
                        auto mapPlay = [&](int f) {
                            if (f < 0) return f;
                            return anim::scaledFrame(f, srcLen, editClip.length);
                        };
                        ed.animFrame = mapPlay(srcFrame);
                        ed.animSpanA = mapPlay(srcA);
                        ed.animSpanB = mapPlay(srcB);
                        float u = (oldLast > 0) ? srcClock / (float)oldLast : 0.0f;
                        ed.animClock = u * (float)newLast;
                        if (ed.animFrame < 0) ed.animFrame = 0;
                        if (ed.animFrame > newLast) ed.animFrame = newLast;
                        ed.animDirty = true;
                        ed.animPlaying = false;
                        layoutAnimBar();
                    }
                }
                else if (hitR(animTimeRect) && editClip.length > 1) {
                    auto frameAt = [&](float x) { return xToFrame(x); };
                    ed.animPlaying = false;
                    if (keyDown(VK_SHIFT)) {
                        int hit = frameAt(mx);
                        ed.animSpanA = ed.animFrame;
                        ed.animSpanB = hit;
                        ed.animFrame = hit;
                        ed.animClock = (float)hit;
                    } else {
                        int nearest = -1;
                        float best = 10.0f;
                        for (const anim::Track& tr : editClip.tracks) {
                            for (const anim::Key& k : tr.keys) {
                                float kx = frameToX((float)k.frame);
                                float d = std::fabs(mx - kx);
                                if (d < best) { best = d; nearest = k.frame; }
                            }
                        }
                        if (nearest >= 0) {
                            ed.animFrame = nearest;
                            ed.animClock = (float)nearest;
                            ed.animScrub = 2;
                        } else {
                            ed.animFrame = frameAt(mx);
                            ed.animClock = (float)ed.animFrame;
                            ed.animScrub = 1;
                        }
                    }
                } else if (inAnim3d && ed.animMask && ed.gizmoHover == 0) {
                    Vec3 ro, rd;
                    animRay(mx, my, ro, rd);
                    anim::Clip viewClip = previewClipNow();
                    auto pose = poseShown(viewClip, ed.animClock);
                    std::vector<pm::Part> posed = anim::poseParts(animBindParts, viewClip, pose);
                    float best = 1e9f;
                    int hitPart = -1, hitFace = 0;
                    for (int i = 0; i < (int)posed.size(); i++) {
                        const pm::Part& p = posed[i];
                        if (!anim::isRigPart(p) || p.half.x < 1e-4f) continue;
                        Vec3 roL = pm::unrotateEuler(ro - p.center, p.rot);
                        Vec3 rdL = pm::unrotateEuler(rd, p.rot);
                        float t0 = 0.0f, t1 = 1e6f;
                        bool ok = true;
                        float h[3] = { p.half.x, p.half.y, p.half.z };
                        float o[3] = { roL.x, roL.y, roL.z };
                        float d[3] = { rdL.x, rdL.y, rdL.z };
                        for (int a = 0; a < 3 && ok; a++) {
                            if (std::fabs(d[a]) < 1e-8f) {
                                if (o[a] < -h[a] || o[a] > h[a]) ok = false;
                                continue;
                            }
                            float inv = 1.0f / d[a];
                            float ta = (-h[a] - o[a]) * inv;
                            float tb = (h[a] - o[a]) * inv;
                            if (ta > tb) { float s = ta; ta = tb; tb = s; }
                            if (ta > t0) t0 = ta;
                            if (tb < t1) t1 = tb;
                            if (t0 > t1) ok = false;
                        }
                        if (!ok || t0 < 0.0f || t0 >= best) continue;
                        Vec3 hp = roL + rdL * t0;
                        float ax = std::fabs(hp.x) / p.half.x;
                        float ay = std::fabs(hp.y) / p.half.y;
                        float az = std::fabs(hp.z) / p.half.z;
                        int f = 4;
                        if (ax >= ay && ax >= az) f = hp.x >= 0.0f ? 2 : 3;
                        else if (ay >= az) f = hp.y >= 0.0f ? 0 : 1;
                        else f = hp.z >= 0.0f ? 4 : 5;
                        best = t0;
                        hitPart = i;
                        hitFace = f;
                    }
                    if (hitPart >= 0) {
                        const std::string& name = pm::partName(posed[hitPart]);
                        int found = -1;
                        for (int i = 0; i < (int)ed.faceMarks.size(); i++)
                            if (ed.faceMarks[i].part == name && ed.faceMarks[i].face == hitFace) found = i;
                        if (found >= 0 && ed.faceMarks[found].color == ed.animMaskColor)
                            ed.faceMarks.erase(ed.faceMarks.begin() + found);
                        else if (found >= 0) ed.faceMarks[found].color = ed.animMaskColor;
                        else ed.faceMarks.push_back({ name, hitFace, ed.animMaskColor });
                    }
                } else if (inAnim3d) {
                    float best = 18.0f, altD = 18.0f;
                    int hit = -1, alt = -1;
                    anim::Clip viewClip = previewClipNow();
                    auto pose = poseShown(viewClip, ed.animClock);
                    for (int i = 0; i < (int)pose.size(); i++) {
                        float sx, sy;
                        if (!projectAnim(pose[i].pivot.x, pose[i].pivot.y, pose[i].pivot.z, sx, sy)) continue;
                        float d = std::hypot(mx - sx, my - sy);
                        if (d < best) { alt = hit; altD = best; best = d; hit = i; }
                        else if (d < altD) { altD = d; alt = i; }
                    }
                    if (hit == ed.animSelBone && alt >= 0 && altD < best + 8.0f) hit = alt;
                    if (hit >= 0) {
                        ed.animSelBone = hit;
                        ed.gizmoHover = 0;
                    }
                }
            }
            static bool barDrag = false;
            auto barThumb = [&](float& tx, float& tw) {
                float content = animTimeRect[2] + animBarScrollMax;
                tw = (content > 1.0f) ? animBarSlider[2] * animTimeRect[2] / content : animBarSlider[2];
                if (tw < 16.0f) tw = 16.0f;
                if (tw > animBarSlider[2]) tw = animBarSlider[2];
                float travel = animBarSlider[2] - tw;
                float u = (animBarScrollMax > 0.0f) ? ed.animBarScroll / animBarScrollMax : 0.0f;
                tx = animBarSlider[0] + travel * u;
            };
            if (lmb && !prevLmb && animBarSlider[2] > 1.0f && animBarScrollMax > 0.0f
                && mx >= animBarSlider[0] && mx < animBarSlider[0] + animBarSlider[2]
                && my >= animBarSlider[1] && my < animBarSlider[1] + animBarSlider[3]) {
                barDrag = true;
                float tx, tw;
                barThumb(tx, tw);
                if (mx < tx || mx >= tx + tw) {
                    float travel = animBarSlider[2] - tw;
                    float u = (travel > 1.0f) ? (mx - animBarSlider[0] - tw * 0.5f) / travel : 0.0f;
                    if (u < 0.0f) u = 0.0f;
                    if (u > 1.0f) u = 1.0f;
                    ed.animBarScroll = u * animBarScrollMax;
                }
            }
            if (lmb && barDrag && animBarScrollMax > 0.0f) {
                float tx, tw;
                barThumb(tx, tw);
                float travel = animBarSlider[2] - tw;
                float u = (travel > 1.0f) ? (mx - animBarSlider[0] - tw * 0.5f) / travel : 0.0f;
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                ed.animBarScroll = u * animBarScrollMax;
            }
            if (!lmb) barDrag = false;
            if (lmb && ed.animScrub == 1 && editClip.length > 1) {
                ed.animFrame = xToFrame(mx);
                ed.animClock = (float)ed.animFrame;
            }
            static int keyHold = -1;
            static bool keyUndo = false;
            if (lmb && ed.animScrub == 2 && editClip.length > 1) {
                if (keyHold < 0) keyHold = ed.animFrame;
                int to = xToFrame(mx);
                ed.animFrame = to;
                ed.animClock = (float)to;
                if (to != keyHold && !anim::frameHasKeys(editClip, to)) {
                    if (!keyUndo) { pushAnimUndo(); keyUndo = true; }
                    anim::Clip view = previewClipNow();
                    if (anim::moveKeyframe(view, keyHold, to)) {
                        storeViewClip(std::move(view));
                        keyHold = to;
                        ed.animDirty = true;
                    }
                }
            }
            if (!lmb && ed.animScrub == 2 && keyHold >= 0 && ed.animFrame != keyHold
                && anim::frameHasKeys(editClip, ed.animFrame)) {
                if (!keyUndo) pushAnimUndo();
                anim::Clip view = previewClipNow();
                if (anim::moveKeyframe(view, keyHold, ed.animFrame)) {
                    storeViewClip(std::move(view));
                    ed.animClock = (float)ed.animFrame;
                    ed.animDirty = true;
                }
            }
            if (!lmb) {
                ed.animScrub = 0;
                keyHold = -1;
                keyUndo = false;
            }
            static bool animOrbit = false;
            if (rmb && !prevRmb) animOrbit = inAnim3d;
            if (rmb && animOrbit) {
                ed.arotY += (mx - lastMx) * 0.008f;
                ed.arotX += (my - lastMy) * 0.008f;
                if (ed.arotX > 1.55f) ed.arotX = 1.55f;
                if (ed.arotX < -1.55f) ed.arotX = -1.55f;
            }
            if (!rmb) animOrbit = false;
            static int animGzDrag = 0;
            static bool animDragUndo = false;
            static Vec3 animGzGrabC, animRollAxis, animRollU, animRollV;
            static float animGzT = 0, animGzAng = 0;
            if (!lmb) animDragUndo = false;
            if (lmb && !prevLmb && inAnim3d && ed.gizmoHover != 0 && !ed.animScrub) {
                animGzDrag = ed.gizmoHover;
                ed.animPlaying = false;
                animGizmoCenter(animGzGrabC);
                Vec3 ro, rd;
                animRay(mx, my, ro, rd);
                if (animGzDrag == 30 && boneRollFrame(animGzGrabC, animRollAxis, animRollU, animRollV))
                    animGzAng = boneRollAngle(animGzGrabC, animRollAxis, animRollU, animRollV, ro, rd);
                else {
                    int axis = gizmoAxisOf(animGzDrag);
                    Vec3 dragU = kAxis[axis];
                    if (ed.animGrasp) {
                        Vec3 axes[3];
                        if (itemModelAxes(axes)) dragU = axes[axis];
                    }
                    if (gizmoIsRot(animGzDrag)) animGzAng = rotAngleAt(axis, animGzGrabC, ro, rd);
                    else animGzT = closestAxisT(animGzGrabC, dragU, ro, rd);
                }
            }
            if (lmb && animGzDrag != 0 && !animDragUndo &&
                (ed.animGrasp || ed.animHoldEdit || (ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()))) {
                pushAnimUndo();
                animDragUndo = true;
            }
            if (lmb && animGzDrag == 30 && ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()) {
                Vec3 ro, rd;
                animRay(mx, my, ro, rd);
                float ang = boneRollAngle(animGzGrabC, animRollAxis, animRollU, animRollV, ro, rd);
                float d = ang - animGzAng;
                while (d > 3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                if (std::fabs(d) > 1e-5f) {
                    anim::Clip view = previewClipNow();
                    anim::rollBoneKey(view, ed.animSelBone, ed.animFrame, animRollAxis, d, ed.animRollChain);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                }
                animGzAng = ang;
            } else if (lmb && animGzDrag != 0 && ed.animGrasp && !gizmoIsRot(animGzDrag)) {
                Vec3 ro, rd;
                animRay(mx, my, ro, rd);
                int axis = gizmoAxisOf(animGzDrag);
                Vec3 dragU = kAxis[axis];
                Vec3 axes[3];
                if (itemModelAxes(axes)) dragU = axes[axis];
                float t = closestAxisT(animGzGrabC, dragU, ro, rd);
                float dt = t - animGzT;
                if (std::fabs(dt) > 1e-6f) {
                    anim::Clip view = previewClipNow();
                    hold::Spec spec = previewHoldSpec();
                    float scale = (spec.scale < 1e-4f) ? 0.48f : spec.scale;
                    Vec3 g = anim::evalGrip(view, (float)ed.animFrame, spec.grip);
                    float delta = -dt / scale;
                    if (axis == 0) g.x += delta;
                    else if (axis == 1) g.y += delta;
                    else g.z += delta;
                    anim::setGripKey(view, ed.animFrame, g, spec.grip);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                }
                animGzT = t;
            } else if (lmb && animGzDrag != 0 && ed.animHoldEdit) {
                Vec3 ro, rd;
                animRay(mx, my, ro, rd);
                int axis = gizmoAxisOf(animGzDrag);
                hold::Spec& spec = editableHoldSpec();
                const bool mirrorTool = ed.animHoldSide < 0;
                if (mirrorTool) hold::mirrorPalmX(spec);
                anim::Clip viewClip = previewClipNow();
                auto pose = anim::evalPose(viewClip, ed.animClock);
                anim::BoneXform xf{};
                if (anim::boneXformOf(viewClip, pose, spec.bone, xf)) {
                    if (gizmoIsRot(animGzDrag)) {
                        float ang = rotAngleAt(axis, animGzGrabC, ro, rd);
                        float d = ang - animGzAng;
                        while (d > 3.14159265f) d -= 6.2831853f;
                        while (d < -3.14159265f) d += 6.2831853f;
                        if (std::fabs(d) > 1e-5f) {
                            hold::nudge(spec, xf, {}, kAxis[axis], d);
                            ed.animHoldDirty = true;
                        }
                        animGzAng = ang;
                    } else {
                        float t = closestAxisT(animGzGrabC, kAxis[axis], ro, rd);
                        float dt = t - animGzT;
                        if (std::fabs(dt) > 1e-6f) {
                            hold::nudge(spec, xf, kAxis[axis] * dt, {}, 0.0f);
                            ed.animHoldDirty = true;
                        }
                        animGzT = t;
                    }
                }
                if (mirrorTool) hold::mirrorPalmX(spec);
            } else if (lmb && animGzDrag != 0 && ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()) {
                Vec3 ro, rd;
                animRay(mx, my, ro, rd);
                int axis = gizmoAxisOf(animGzDrag);
                if (gizmoIsRot(animGzDrag)) {
                    float ang = rotAngleAt(axis, animGzGrabC, ro, rd);
                    float d = ang - animGzAng;
                    while (d > 3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    if (std::fabs(d) > 1e-5f) {
                        anim::Clip view = previewClipNow();
                        anim::BoneXform palm0{};
                        float toolR[9]{};
                        Vec3 grip0{};
                        bool held = captureTool(view, palm0, toolR, grip0);
                        anim::nudgeBoneWorld(view, ed.animSelBone, ed.animFrame, {}, kAxis[axis], d);
                        if (held) plantTool(view, palm0, toolR, grip0);
                        storeViewClip(std::move(view));
                        ed.animDirty = true;
                        ed.animClock = (float)ed.animFrame;
                    }
                    animGzAng = ang;
                } else {
                    float t = closestAxisT(animGzGrabC, kAxis[axis], ro, rd);
                    float dt = t - animGzT;
                    if (std::fabs(dt) > 1e-6f) {
                        anim::Clip view = previewClipNow();
                        anim::BoneXform palm0{};
                        float toolR[9]{};
                        Vec3 grip0{};
                        bool held = captureTool(view, palm0, toolR, grip0);
                        anim::nudgeBoneWorld(view, ed.animSelBone, ed.animFrame, kAxis[axis] * dt, {}, 0.0f);
                        if (held) plantTool(view, palm0, toolR, grip0);
                        storeViewClip(std::move(view));
                        ed.animDirty = true;
                        ed.animClock = (float)ed.animFrame;
                    }
                    animGzT = t;
                }
            }
            if (!lmb) animGzDrag = 0;
            static bool prevSpace = false, prevI = false, prevDel = false, prevK1 = false, prevK2 = false, prevK3 = false, prevK4 = false;
            bool space = keyDown(VK_SPACE);
            if (space && !prevSpace) ed.animPlaying = !ed.animPlaying;
            prevSpace = space;
            bool k1 = keyDown('1'), k2 = keyDown('2'), k3 = keyDown('3'), k4 = keyDown('4');
            if (k1 && !prevK1) ed.animTool = 0;
            if (k2 && !prevK2) ed.animTool = 1;
            if (k3 && !prevK3) ed.animTool = 2;
            if (k4 && !prevK4) ed.animTool = 3;
            prevK1 = k1; prevK2 = k2; prevK3 = k3; prevK4 = k4;
            bool keyI = keyDown('I');
            if (keyI && !prevI && ed.animGrasp) {
                pushAnimUndo();
                anim::Clip view = previewClipNow();
                hold::Spec spec = previewHoldSpec();
                Vec3 g = anim::evalGrip(view, (float)ed.animFrame, spec.grip);
                anim::setGripKey(view, ed.animFrame, g, spec.grip);
                storeViewClip(std::move(view));
                ed.animDirty = true;
            } else if (keyI && !prevI && ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()) {
                pushAnimUndo();
                anim::Clip view = previewClipNow();
                auto pose = poseShown(view, (float)ed.animFrame);
                anim::writeShownPose(view, ed.animFrame, pose);
                storeViewClip(std::move(view));
                ed.animDirty = true;
            }
            prevI = keyI;
            bool del = keyDown(VK_DELETE);
            if (del && !prevDel && ed.animGrasp) {
                pushAnimUndo();
                anim::Clip view = previewClipNow();
                anim::deleteKey(view, anim::kGripTrack, ed.animFrame);
                storeViewClip(std::move(view));
                ed.animDirty = true;
            } else if (del && !prevDel && ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()) {
                pushAnimUndo();
                anim::Clip view = previewClipNow();
                anim::deleteKey(view, view.bones[ed.animSelBone].name, ed.animFrame);
                storeViewClip(std::move(view));
                ed.animDirty = true;
            }
            prevDel = del;
            bool axisHover = ed.gizmoHover == 30
                || (ed.gizmoHover >= 1 && ed.gizmoHover <= 6)
                || (ed.gizmoHover >= 10 && ed.gizmoHover <= 15)
                || (ed.gizmoHover >= 20 && ed.gizmoHover <= 25);
            static bool prevAxisL = false, prevAxisR = false;
            bool axisL = keyDown(VK_LEFT), axisR = keyDown(VK_RIGHT);
            int axisDir = 0;
            if (axisHover && animGzDrag == 0 && !ed.animMask) {
                if (axisL && !prevAxisL) axisDir = -1;
                if (axisR && !prevAxisR) axisDir = 1;
            }
            prevAxisL = axisL;
            prevAxisR = axisR;
            if (axisDir != 0 && (ed.animGrasp || ed.animHoldEdit
                    || (ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()))) {
                pushAnimUndo();
                const float deg = 0.017453292f * (float)axisDir;
                const float dist = 0.01f * (float)axisDir;
                if (ed.gizmoHover == 30) {
                    anim::Clip view = previewClipNow();
                    Vec3 origin, dir;
                    if (anim::boneSegmentAxis(view, ed.animSelBone, origin, dir)) {
                        auto pose = anim::evalPose(view, (float)ed.animFrame);
                        if (ed.animSelBone < (int)pose.size())
                            anim::rollBoneKey(view, ed.animSelBone, ed.animFrame, anim::mul9(pose[ed.animSelBone].R, dir), deg, ed.animRollChain);
                    }
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                } else if (ed.animHoldEdit) {
                    int axis = gizmoAxisOf(ed.gizmoHover);
                    hold::Spec& spec = editableHoldSpec();
                    const bool mirrorTool = ed.animHoldSide < 0;
                    if (mirrorTool) hold::mirrorPalmX(spec);
                    anim::Clip viewClip = previewClipNow();
                    auto pose = anim::evalPose(viewClip, (float)ed.animFrame);
                    anim::BoneXform xf{};
                    if (anim::boneXformOf(viewClip, pose, spec.bone, xf)) {
                        if (gizmoIsRot(ed.gizmoHover)) hold::nudge(spec, xf, {}, kAxis[axis], deg);
                        else hold::nudge(spec, xf, kAxis[axis] * dist, {}, 0.0f);
                        ed.animHoldDirty = true;
                    }
                    if (mirrorTool) hold::mirrorPalmX(spec);
                } else if (ed.animGrasp) {
                    int axis = gizmoAxisOf(ed.gizmoHover);
                    anim::Clip view = previewClipNow();
                    hold::Spec spec = previewHoldSpec();
                    Vec3 g = anim::evalGrip(view, (float)ed.animFrame, spec.grip);
                    if (axis == 0) g.x += dist;
                    else if (axis == 1) g.y += dist;
                    else g.z += dist;
                    anim::setGripKey(view, ed.animFrame, g, spec.grip);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                } else if (gizmoIsRot(ed.gizmoHover)) {
                    int axis = gizmoAxisOf(ed.gizmoHover);
                    anim::Clip view = previewClipNow();
                    anim::BoneXform palm0{};
                    float toolR[9]{};
                    Vec3 grip0{};
                    bool held = captureTool(view, palm0, toolR, grip0);
                    anim::nudgeBoneWorld(view, ed.animSelBone, ed.animFrame, {}, kAxis[axis], deg);
                    if (held) plantTool(view, palm0, toolR, grip0);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                } else {
                    int axis = gizmoAxisOf(ed.gizmoHover);
                    Vec3 dragU = kAxis[axis];
                    anim::Clip view = previewClipNow();
                    anim::BoneXform palm0{};
                    float toolR[9]{};
                    Vec3 grip0{};
                    bool held = captureTool(view, palm0, toolR, grip0);
                    anim::nudgeBoneWorld(view, ed.animSelBone, ed.animFrame, dragU * dist, {}, 0.0f);
                    if (held) plantTool(view, palm0, toolR, grip0);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                }
            }
            static bool animKeyUndo = false;
            if (animGzDrag == 0 && ed.animGrasp) {
                float step = keyDown(VK_SHIFT) ? 0.08f : 0.02f;
                Vec3 d{};
                if (!axisHover && keyDown(VK_LEFT)) d.x -= step;
                if (!axisHover && keyDown(VK_RIGHT)) d.x += step;
                if (keyDown(VK_UP)) d.y += step;
                if (keyDown(VK_DOWN)) d.y -= step;
                if (keyDown('Q')) d.z -= step;
                if (keyDown('E')) d.z += step;
                if (d.lengthSq() <= 0.0f) animKeyUndo = false;
                else {
                    if (!animKeyUndo) { pushAnimUndo(); animKeyUndo = true; }
                    anim::Clip view = previewClipNow();
                    hold::Spec spec = previewHoldSpec();
                    Vec3 g = anim::evalGrip(view, (float)ed.animFrame, spec.grip) + d;
                    anim::setGripKey(view, ed.animFrame, g, spec.grip);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                }
            } else if (animGzDrag == 0 && ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()) {
                float step = keyDown(VK_SHIFT) ? 0.08f : 0.02f;
                Vec3 tadd{};
                float rang = 0.0f;
                int raxis = -1;
                if (ed.animTool == 3) {
                    if ((!axisHover && keyDown(VK_LEFT)) || keyDown(VK_UP) || keyDown('Q')) rang += step;
                    if ((!axisHover && keyDown(VK_RIGHT)) || keyDown(VK_DOWN) || keyDown('E')) rang -= step;
                    if (std::fabs(rang) < 1e-8f) animKeyUndo = false;
                    else {
                        if (!animKeyUndo) { pushAnimUndo(); animKeyUndo = true; }
                        anim::Clip view = previewClipNow();
                        Vec3 origin, dir;
                        if (anim::boneSegmentAxis(view, ed.animSelBone, origin, dir)) {
                            auto pose = anim::evalPose(view, (float)ed.animFrame);
                            if (ed.animSelBone < (int)pose.size())
                                anim::rollBoneKey(view, ed.animSelBone, ed.animFrame, anim::mul9(pose[ed.animSelBone].R, dir), rang, ed.animRollChain);
                        }
                        storeViewClip(std::move(view));
                        ed.animDirty = true;
                        ed.animClock = (float)ed.animFrame;
                    }
                    tadd = {};
                    raxis = -1;
                    rang = 0.0f;
                } else if (ed.animTool == 0) {
                    if (!axisHover && keyDown(VK_LEFT)) tadd.x -= step;
                    if (!axisHover && keyDown(VK_RIGHT)) tadd.x += step;
                    if (keyDown(VK_UP)) tadd.y += step;
                    if (keyDown(VK_DOWN)) tadd.y -= step;
                    if (keyDown('Q')) tadd.z -= step;
                    if (keyDown('E')) tadd.z += step;
                } else {
                    if (!axisHover && keyDown(VK_LEFT)) { raxis = 1; rang += step; }
                    if (!axisHover && keyDown(VK_RIGHT)) { raxis = 1; rang -= step; }
                    if (keyDown(VK_UP)) { raxis = 0; rang -= step; }
                    if (keyDown(VK_DOWN)) { raxis = 0; rang += step; }
                    if (keyDown('Q')) { raxis = 2; rang += step; }
                    if (keyDown('E')) { raxis = 2; rang -= step; }
                }
                if (ed.animTool != 3) {
                bool keyNudge = tadd.lengthSq() > 0.0f || (raxis >= 0 && std::fabs(rang) > 0.0f);
                if (!keyNudge) animKeyUndo = false;
                else if (!animKeyUndo) { pushAnimUndo(); animKeyUndo = true; }
                if (tadd.lengthSq() > 0.0f) {
                    anim::Clip view = previewClipNow();
                    anim::BoneXform palm0{};
                    float toolR[9]{};
                    Vec3 grip0{};
                    bool held = captureTool(view, palm0, toolR, grip0);
                    anim::nudgeBoneWorld(view, ed.animSelBone, ed.animFrame, tadd, {}, 0.0f);
                    if (held) plantTool(view, palm0, toolR, grip0);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                } else if (raxis >= 0 && std::fabs(rang) > 0.0f) {
                    anim::Clip view = previewClipNow();
                    anim::BoneXform palm0{};
                    float toolR[9]{};
                    Vec3 grip0{};
                    bool held = captureTool(view, palm0, toolR, grip0);
                    anim::nudgeBoneWorld(view, ed.animSelBone, ed.animFrame, {}, kAxis[raxis], rang);
                    if (held) plantTool(view, palm0, toolR, grip0);
                    storeViewClip(std::move(view));
                    ed.animDirty = true;
                    ed.animClock = (float)ed.animFrame;
                }
                }
            } else animKeyUndo = false;
            static bool prevAnimCtrlZ = false;
            bool animCtrlZ = keyDown(VK_CONTROL) && keyDown('Z') && !keyDown(VK_SHIFT);
            if (animCtrlZ && !prevAnimCtrlZ) undoAnim();
            prevAnimCtrlZ = animCtrlZ;
            lastMx = mx; lastMy = my;
        } else if (ed.entityMode) {
            // entity (player) model editing: cuboid parts, tools mirrored from the block editor
            if (lmb && !prevLmb && entPartHov) {
                if (entIsCloth) setPartAdd();
                else addEntPart();
            }
            if (lmb && !prevLmb && entSelHov) setHairSelect();
            if (lmb && !prevLmb && entPaintHov) setHairBrush(false);
            if (lmb && !prevLmb && entCardHov) setHairCard();
            if (lmb && !prevLmb && entEraseHov) setHairBrush(true);
            if (lmb && !prevLmb && entMergeHov) doHairMerge();
            if (lmb && !prevLmb && entSplitHov) doHairSplit();
            if (lmb && !prevLmb && entTexHov) addEntDecal();
            if (lmb && !prevLmb && entDupHov) dupEnt();
            if (lmb && !prevLmb && entDelHov) {
                if (ed.partPlane) { ed.planeOn = false; ed.partAnchor = false; }
                else if (ed.partMeasure) { ed.measureOn = false; ed.partAnchor = false; }
                else delEnt();
            }
            if (lmb && !prevLmb && entCutHov) setPartCut();
            if (lmb && !prevLmb && entMeasureHov) setPartMeasure();
            if (lmb && !prevLmb && entPlaneHov) setPartPlane();
            if (lmb && !prevLmb && midSaveHov) saveEntity();
            if (lmb && !prevLmb && midUndoHov) undoEntityCmd();
            if (lmb && !prevLmb && entCancelHov) cancelEntity();
            if (lmb && !prevLmb && ed.modelToolHover >= 0) {
                int i = ed.modelToolHover;
                if (i == 0) bindEnt();
                else if (i == 1) unbindEnt();
                else if (i == 2) setModelTool(0);
                else if (i == 3) snapEnt();
                else if (i == 4) alignEnt();
                else if (i == 5) setModelTool(3);
                else if (i == 6) setModelTool(1);
                else if (i == 7) setModelTool(2);
                else if (i == 8) setModelTool(4);
                else if (i == 9) setModelTool(5);
                else if (i == 10) toggleRefTool();
            }
            if (lmb && !prevLmb && faceChipHov >= 0) {
                int hit = findPartTex(faceNameAt(faceChipHov));
                if (hit >= 0) markEnt(hit, false);
            }
            if (lmb && !prevLmb && mouthChipHov >= 0) {
                pushEntUndo();
                const char* nm = (mouthChipHov < (int)entMouthNames.size())
                    ? entMouthNames[mouthChipHov].c_str() : pm::kMouthNames[mouthChipHov];
                bool any = false;
                for (int i : entSelected()) {
                    if (pm::isMouthPart(ed.entityParts[i])) {
                        ed.entityParts[i].tex = nm;
                        any = true;
                    }
                }
                if (!any) {
                    for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                        if (pm::isMouthPart(ed.entityParts[i])) {
                            ed.entityParts[i].tex = nm;
                            markEnt(i, false);
                            any = true;
                            break;
                        }
                    }
                }
                ed.dirty = true;
            }
            static bool entScrollDrag = false;
            static float entScrollGrab = 0.0f;
            if (ed.entityMode && entScrollMax > 1.0f) {
                bool overTrack = mx >= entScrollTrack[0] && mx < entScrollTrack[0] + entScrollTrack[2] &&
                    my >= entScrollTrack[1] && my < entScrollTrack[1] + entScrollTrack[3];
                if (lmb && !prevLmb && overTrack) {
                    entScrollDrag = true;
                    entScrollGrab = my - entScrollThumbY;
                    if (entScrollGrab < 0.0f) entScrollGrab = 0.0f;
                    if (entScrollGrab > entScrollThumbH) entScrollGrab = entScrollThumbH;
                }
                if (!lmb) entScrollDrag = false;
                if (entScrollDrag) {
                    float u = (my - entScrollGrab - entListTop) / entScrollTravel;
                    if (u < 0.0f) u = 0.0f;
                    if (u > 1.0f) u = 1.0f;
                    ed.entScroll = u * entScrollMax;
                    ed.entityHover = -1;
                }
            } else entScrollDrag = false;
            if (lmb && !prevLmb && !entScrollDrag && ed.entityHover >= 0 && ed.entityHover < entityListCount()) {
                if (const std::string* nm = entityListName(ed.entityHover)) {
                    if (*nm != ed.entityName) {
                        if (ed.entityHover < (int)entNames.size()) loadEntityFile(*nm);
                        else loadGarmentFile(*nm);
                    }
                }
            }
            if (lmb && !prevLmb && ed.toolHover >= 0 &&
                !entPartHov && !entSelHov && !entPaintHov && !entCardHov && !entEraseHov && !entMergeHov && !entSplitHov &&
                !entTexHov && !entDupHov && !entDelHov && !entCutHov &&
                ed.modelToolHover < 0) {
                int i = ed.toolHover;
                if (i == 0) { ed.tool = (ed.tool == 0) ? -1 : 0; ed.picker = false; ed.brushMod = false; clearHairBrush(); }
                else if (i == 1) {
                    if (ed.tool == 2) { ed.brushMod = !ed.brushMod; if (ed.brushMod) ed.picker = false; }
                    else { ed.tool = (ed.tool == 1) ? -1 : 1; ed.picker = false; ed.brushMod = false; }
                    clearHairBrush();
                }
                else if (i == 2) { ed.tool = (ed.tool == 2) ? -1 : 2; ed.picker = false; ed.brushMod = false; clearHairBrush(); }
                else if (i == 3) { ed.picker = !ed.picker; if (ed.picker) ed.brushMod = false; clearHairBrush(); }
                else if (i == 4) { ed.tool = (ed.tool == 4) ? -1 : 4; ed.picker = false; ed.brushMod = false; clearHairBrush(); }
                else if (i == 5) { ed.tool = (ed.tool == 5) ? -1 : 5; ed.picker = false; ed.brushMod = false; clearHairBrush(); }
                else if (i == 6) { ed.tool = (ed.tool == 6) ? -1 : 6; ed.picker = false; ed.brushMod = false; clearHairBrush(); }
                else if (i == 7) { ed.tool = (ed.tool == 7) ? -1 : 7; ed.picker = false; ed.brushMod = false; clearHairBrush(); }
                else if (i == 8) { ed.tool = (ed.tool == 8) ? -1 : 8; ed.picker = false; ed.brushMod = false; clearHairBrush(); }
                else if (i == 9) undoEntityCmd();
                else if (i == 10) { if (ed.brushSize > 1) ed.brushSize--; }
                else if (i == 11) { if (ed.brushSize < 4) ed.brushSize++; }
                else if (i == 12) saveEntity();
            }
            if (lmb && !prevLmb && ed.paletteHover >= 0) {
                if (ed.paletteHover < 8) {
                    ed.r = kPaintPal[ed.paletteHover][0]; ed.g = kPaintPal[ed.paletteHover][1];
                    ed.b = kPaintPal[ed.paletteHover][2]; ed.a = 1;
                } else ed.a = 0;
                if (!ed.entSkinView && !hairToolsOn && paintKind() < 0) {
                    pushEntUndo();
                    for (int i : entSelected()) {
                        ed.entityParts[i].color = { ed.r, ed.g, ed.b };
                    }
                    ed.dirty = true;
                }
            }
            if (lmb) {
                bool overBrushUi = ed.tool == 1 && ed.brushMat[2] > 1.0f &&
                    mx >= ed.brushMat[0] - 4.0f && mx < ed.brushMat[0] + ed.brushMat[2] + 4.0f &&
                    my >= ed.brushMat[1] - 4.0f && my < ed.brushMat[1] + ed.brushMat[3] + 42.0f;
                if (!overBrushUi) {
                if (mx >= ed.svRect[0] && mx < ed.svRect[0] + ed.svRect[2] &&
                    my >= ed.svRect[1] && my < ed.svRect[1] + ed.svRect[3]) {
                    float s = (mx - ed.svRect[0]) / ed.svRect[2];
                    float v = 1.0f - (my - ed.svRect[1]) / ed.svRect[3];
                    if (s < 0) s = 0; if (s > 1) s = 1;
                    if (v < 0) v = 0; if (v > 1) v = 1;
                    float hh, ss, vv; rgbToHsv(ed.r, ed.g, ed.b, hh, ss, vv);
                    hsvToRgb(hh, s, v, ed.r, ed.g, ed.b);
                }
                float ddx = mx - ed.hueCX, ddy = my - ed.hueCY;
                float dist = std::sqrt(ddx * ddx + ddy * ddy);
                if (dist >= ed.hueRI - 6.0f && dist <= ed.hueRO + 6.0f) {
                    float h = std::atan2(ddy, ddx) / 6.2831853f;
                    if (h < 0) h += 1.0f;
                    float hh, ss, vv; rgbToHsv(ed.r, ed.g, ed.b, hh, ss, vv);
                    hsvToRgb(h, ss, vv, ed.r, ed.g, ed.b);
                }
                float alX = ed.alphaRect[0], alY = ed.alphaRect[1], alW = ed.alphaRect[2], alH = ed.alphaRect[3];
                if (mx >= alX - 6 && mx < alX + alW + 6 && my >= alY - 6 && my < alY + alH + 6 && alW > 1.0f) {
                    ed.a = (mx - alX) / alW;
                    if (ed.a < 0) ed.a = 0;
                    if (ed.a > 1) ed.a = 1;
                }
                }
            }
            if (ed.tool == 1) {
                ensureBrushMat();
                float bmX = ed.brushMat[0], bmY = ed.brushMat[1];
                float mw = ed.brushMat[2];
                for (int k = 0; k < 16; k++) {
                    int cxi = k % 4, cyi = k / 4;
                    float bx2 = bmX + cxi * (entBmS + 4.0f), by2 = bmY + cyi * (entBmS + 4.0f);
                    if (lmb && !prevLmb && mx >= bx2 && mx < bx2 + entBmS && my >= by2 && my < by2 + entBmS) {
                        ed.brushSel[k] ^= 1;
                        if (ed.brushSet[k]) {
                            ed.r = ed.brushColors[k].r / 255.0f;
                            ed.g = ed.brushColors[k].g / 255.0f;
                            ed.b = ed.brushColors[k].b / 255.0f;
                            ed.a = ed.brushColors[k].a / 255.0f;
                        }
                    }
                }
                const float btnY = bmY + mw + 10.0f;
                const float ckS = 16.0f, ckX = bmX, ckY = btnY + 4.0f;
                const float ckHitW = 58.0f;
                bool ckHov = mx >= ckX && mx < ckX + ckHitW && my >= ckY && my < ckY + ckS;
                if (lmb && !prevLmb && ckHov) ed.pickAlpha0 = !ed.pickAlpha0;
                bool anySel = false;
                for (int k = 0; k < 16; k++) if (ed.brushSel[k]) { anySel = true; break; }
                if (anySel) {
                    float crx = bmX + mw - 52.0f, chkx = bmX + mw - 24.0f;
                    bool crsHov = mx >= crx && mx < crx + 24.0f && my >= btnY && my < btnY + 24.0f;
                    bool chkHov = mx >= chkx && mx < chkx + 24.0f && my >= btnY && my < btnY + 24.0f;
                    if (lmb && !prevLmb && chkHov) {
                        Color4 c = { (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                        for (int k = 0; k < 16; k++) if (ed.brushSel[k]) {
                            ed.brushColors[k] = c;
                            ed.brushSet[k] = 1;
                        }
                        for (uint8_t& s : ed.brushSel) s = 0;
                    }
                    if (lmb && !prevLmb && crsHov) {
                        for (int k = 0; k < 16; k++) if (ed.brushSel[k]) {
                            ed.brushColors[k] = Color4{};
                            ed.brushSet[k] = 0;
                        }
                        compactBrushMat();
                    }
                }
            }
            if (lmb && !prevLmb && ed.matHover >= 0) {
                if (ed.matHover == nMatItems - 1) {
                    addExtraMat();
                    if (!extraMats.empty()) {
                        const std::string& nm = extraMats.back().name;
                        ed.hairCardTex = nm;
                        ed.selMat = nm;
                        pushEntUndo();
                        for (int i : entSelected()) {
                            if (pm::isHairPart(ed.entityParts[i]) || pm::isHairCardPart(ed.entityParts[i]) ||
                                (pm::isDecalPart(ed.entityParts[i]) && !pm::isEyePart(ed.entityParts[i])
                                 && !pm::isEyelidPart(ed.entityParts[i]) && !pm::isMouthPart(ed.entityParts[i])))
                                ed.entityParts[i].tex = nm;
                        }
                        ed.dirty = true;
                    }
                } else {
                    std::string nm;
                    if (ed.matHover < TEX_COUNT) nm = mat::tileName(ed.matHover);
                    else {
                        int ei = ed.matHover - TEX_COUNT;
                        if (ei >= 0 && ei < (int)extraMats.size()) nm = extraMats[ei].name;
                    }
                    if (!nm.empty()) {
                        ed.hairCardTex = nm;
                        ed.selMat = nm;
                        pushEntUndo();
                        for (int i : entSelected()) {
                            if (pm::isHairPart(ed.entityParts[i]) || pm::isHairCardPart(ed.entityParts[i]) ||
                                (pm::isDecalPart(ed.entityParts[i]) && !pm::isEyePart(ed.entityParts[i])
                                 && !pm::isEyelidPart(ed.entityParts[i]) && !pm::isMouthPart(ed.entityParts[i])))
                                ed.entityParts[i].tex = nm;
                        }
                        ed.dirty = true;
                    }
                }
            }

            static bool prevCtrlZ = false;
            bool ctrlZ = keyDown(VK_CONTROL) && keyDown('Z');
            if (ctrlZ && !prevCtrlZ) undoEntityCmd();
            prevCtrlZ = ctrlZ;

            static int gzDrag = 0;
            static float gzPressMx = 0, gzPressMy = 0;
            static bool gzDidDrag = false, gzUndoPushed = false;
            static Vec3 gzGrabC;
            static float gzAppliedT = 0, gzAppliedAng = 0;
            static bool gzOneSided = false;
            static float gzSideSign = 1.0f;
            auto clickOnlyTool = [&]() { return ed.modelTool == 3 || ed.modelTool == 4 || ed.modelTool == 5; };
            auto beginEntGzDrag = [&]() {
                gzDrag = ed.gizmoHover;
                gzPressMx = mx; gzPressMy = my;
                gzDidDrag = false; gzUndoPushed = false;
                gzOneSided = false;
                gzSideSign = 1.0f;
                if (clickOnlyTool()) return;
                gizmoCenterView(gzGrabC);
                Vec3 ro, rd;
                rightRay(mx, my, ro, rd);
                int axis = gizmoAxisOf(gzDrag);
                if (gizmoIsRot(gzDrag)) gzAppliedAng = rotAngleAt(axis, gzGrabC, ro, rd);
                else {
                    gzAppliedT = closestAxisT(gzGrabC, kAxis[axis], ro, rd);
                    if (ed.modelTool == 1 || ed.modelTool == 2) {
                        gzOneSided = true;
                        if (gizmoIsHead(gzDrag)) gzSideSign = gizmoHeadSign(gzDrag);
                        else gzSideSign = (gzAppliedT >= 0.0f) ? 1.0f : -1.0f;
                    }
                }
            };
            if (lmb && gzDrag != 0) {
                float dist = std::hypot(mx - gzPressMx, my - gzPressMy);
                if (dist > 5.0f) gzDidDrag = true;
                if (gzDidDrag && !gzUndoPushed && !clickOnlyTool()) { pushEntUndo(); gzUndoPushed = true; }
                if (gzDidDrag && !clickOnlyTool()) {
                    Vec3 ro, rd;
                    rightRay(mx, my, ro, rd);
                    int axis = gizmoAxisOf(gzDrag);
                    if (gizmoIsRot(gzDrag)) {
                        float ang = rotAngleAt(axis, gzGrabC, ro, rd);
                        float d = ang - gzAppliedAng;
                        while (d > 3.14159265f) d -= 6.2831853f;
                        while (d < -3.14159265f) d += 6.2831853f;
                        if (std::fabs(d) > 1e-5f) rotateEntBy(axis, d);
                        gzAppliedAng = ang;
                    } else {
                        float t = closestAxisT(gzGrabC, kAxis[axis], ro, rd);
                        float dt = t - gzAppliedT;
                        if (std::fabs(dt) > 1e-5f) {
                            if (ed.modelTool == 2) fillEnt(axis, dt, gzOneSided, gzSideSign);
                            else if (ed.modelTool == 1)
                                stretchEnt(axis, dt, gzOneSided, gzSideSign);
                            else
                                translateEnt(kAxis[axis].x * dt, kAxis[axis].y * dt, kAxis[axis].z * dt);
                        }
                        gzAppliedT = t;
                    }
                }
            }
            bool overEntUi = overMidHdr || overEntBar || overEntPanel || overPalCol || overHairBar || overEntTools || overExit || entPartHov || entSelHov || entPaintHov || entCardHov || entEraseHov || entMergeHov || entSplitHov || entTexHov || entDupHov || entDelHov || entCutHov || entMeasureHov || entPlaneHov || entCancelHov ||
                midSaveHov || midUndoHov || ed.modelToolHover >= 0 || overMatBar || togHov || slideHov || paintSlideHov ||
                ed.toolHover >= 0 || ed.paletteHover >= 0 || faceChipHov >= 0 || mouthChipHov >= 0 || ed.matHover >= 0;
            static bool slideDrag = false;
            if (lmb && !prevLmb && slideHov) slideDrag = true;
            if (!lmb) slideDrag = false;
            if (slideDrag && entSlideH > 1.0f) {
                float t = 1.0f - (my - entSlideY) / entSlideH;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                ed.entPivotY = t * 1.80f;
            }
            static bool paintSlideDrag = false;
            if (lmb && !prevLmb && paintSlideHov) paintSlideDrag = true;
            if (!lmb) paintSlideDrag = false;
            if (paintSlideDrag && paintSlideW > 1.0f) {
                float u = (mx - paintSlideX) / paintSlideW;
                int step = (int)std::lround(u * 3.0f);
                if (step < 0) step = 0;
                if (step > 3) step = 3;
                ed.paintDiv = step;
            }
            if (lmb && !prevLmb && togHov) {
                if (entIsCloth) ed.clothLocal = !ed.clothLocal;
                else ed.entSkinView = !ed.entSkinView;
            }
            auto inBtn = [&](const float r[4]) {
                return r[2] > 1.0f && mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3];
            };
            bool cBodyHov = entIsCloth && inBtn(cBody);
            if (lmb && !prevLmb && cBodyHov) ed.clothBody = !ed.clothBody;

            auto pickPartHit = [&](const Vec3& ro, const Vec3& rd, float& tHit, Vec3& nrm, bool skipDecals, bool skipHair) -> int {
                float best = 1e9f;
                int hit = -1;
                Vec3 bestN{};
                for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                    const pm::Part& p = ed.entityParts[i];
                    if (skipDecals && (pm::isDecalPart(p) || pm::isHairCardPart(p))) continue;
                    if (skipHair && pm::isHairPart(p)) continue;
                    Vec3 roL = pm::partLocalOffset(p, ro);
                    Vec3 rdL = pm::unrotateEuler(rd, p.rot);
                    Vec3 mn{ -p.half.x, -p.half.y, -p.half.z };
                    Vec3 mxb{ p.half.x, p.half.y, p.half.z };
                    float t;
                    if (!rayHitAABB(roL, rdL, mn, mxb, t) || t >= best) continue;
                    Vec3 h = roL + rdL * t;
                    float d[6] = {
                        std::fabs(h.x - mn.x), std::fabs(h.x - mxb.x),
                        std::fabs(h.y - mn.y), std::fabs(h.y - mxb.y),
                        std::fabs(h.z - mn.z), std::fabs(h.z - mxb.z)
                    };
                    int fi = 0;
                    for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
                    Vec3 nL{};
                    if (fi == 0) nL = { -1, 0, 0 };
                    else if (fi == 1) nL = { 1, 0, 0 };
                    else if (fi == 2) nL = { 0, -1, 0 };
                    else if (fi == 3) nL = { 0, 1, 0 };
                    else if (fi == 4) nL = { 0, 0, -1 };
                    else nL = { 0, 0, 1 };
                    bestN = pm::rotateEuler(nL, p.rot);
                    best = t;
                    hit = i;
                }
                tHit = best;
                nrm = bestN;
                return hit;
            };
            auto pickPartRay = [&](const Vec3& ro, const Vec3& rd) -> int {
                float t = 0.0f;
                Vec3 n{};
                return pickPartHit(ro, rd, t, n, false, false);
            };
            static bool hairStrokeUndo = false;
            static pm::HairCell lastHairCell{ 1000000, 0, 0 };
            static Vec3 lastHairN{};
            static float lastHairMx = -1e9f, lastHairMy = -1e9f;
            static int strokeAxis = 1;
            static float strokeSign = 1.0f, strokePlane = 0.0f;
            static bool paintLineHold = false;
            static int paintLineAxis = -1;
            static pm::HairAtom paintLineAnchor{};
            static pm::HairAtom lastPaintAtom{};
            if (!lmb && !rmb) {
                hairStrokeUndo = false;
                lastHairCell = { 1000000, 0, 0 };
                lastHairN = {};
                lastHairMx = lastHairMy = -1e9f;
                paintLineHold = false;
                paintLineAxis = -1;
            }
            auto applyHairBrush = [&](bool erase) -> bool {
                if (!inRight3d || overEntUi) return false;
                const float paintCell = pm::kHairGrid / (float)paintDenom(ed.paintDiv);
                if (lastHairMx > -1e8f) {
                    float pdx = mx - lastHairMx, pdy = my - lastHairMy;
                    if (pdx * pdx + pdy * pdy < 3.0f * 3.0f) return false;
                }
                Vec3 ro, rd;
                rightRay(mx, my, ro, rd);
                float t = 0.0f;
                Vec3 n{};
                if (erase) {
                    int hitC = pickPartHit(ro, rd, t, n, false, false);
                    if (hitC >= 0 && pm::isHairCardPart(ed.entityParts[hitC])) {
                        auto before = ed.entityParts;
                        ed.entityParts.erase(ed.entityParts.begin() + hitC);
                        if (!hairStrokeUndo) {
                            entUndo.push_back(before);
                            if (entUndo.size() > 32) entUndo.erase(entUndo.begin());
                            hairStrokeUndo = true;
                        }
                        entMark.clear();
                        ensureEntMark();
                        ed.dirty = true;
                        lastHairMx = mx;
                        lastHairMy = my;
                        return true;
                    }
                }
                int hit = pickPartHit(ro, rd, t, n, true, false);
                pm::Part bodyHost;
                bool onBody = false;
                if (entIsCloth && hit < 0) {
                    float best = 1e9f;
                    Vec3 bestN{};
                    for (const pm::Part& p : pm::buildPlayerModel()) {
                        if (pm::isHairPart(p) || pm::isDecalPart(p)) continue;
                        Vec3 roL = pm::partLocalOffset(p, ro);
                        Vec3 rdL = pm::unrotateEuler(rd, p.rot);
                        Vec3 mn{ -p.half.x, -p.half.y, -p.half.z };
                        Vec3 mxb{ p.half.x, p.half.y, p.half.z };
                        float tb;
                        if (!rayHitAABB(roL, rdL, mn, mxb, tb) || tb >= best) continue;
                        Vec3 h = roL + rdL * tb;
                        float d[6] = {
                            std::fabs(h.x - mn.x), std::fabs(h.x - mxb.x),
                            std::fabs(h.y - mn.y), std::fabs(h.y - mxb.y),
                            std::fabs(h.z - mn.z), std::fabs(h.z - mxb.z)
                        };
                        int fi = 0;
                        for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
                        Vec3 nL{};
                        if (fi == 0) nL = { -1, 0, 0 };
                        else if (fi == 1) nL = { 1, 0, 0 };
                        else if (fi == 2) nL = { 0, -1, 0 };
                        else if (fi == 3) nL = { 0, 1, 0 };
                        else if (fi == 4) nL = { 0, 0, -1 };
                        else nL = { 0, 0, 1 };
                        bestN = pm::rotateEuler(nL, p.rot);
                        best = tb;
                        bodyHost = p;
                        onBody = true;
                    }
                    if (!onBody) return false;
                    t = best;
                    n = bestN;
                } else if (hit < 0) return false;
                Vec3 world = ro + rd * t;
                if (n.lengthSq() > 1e-12f) n = n.normalized();
                else n = { 0, 1, 0 };
                Vec3 strokeN = n;
                float tBody = 0.0f;
                Vec3 nBody{};
                int body = pickPartHit(ro, rd, tBody, nBody, true, true);
                if (nBody.lengthSq() > 1e-12f) nBody = nBody.normalized();
                pm::HairAtom paintAtom{};
                pm::HairCell cell;
                bool newStroke = (lastHairCell.x >= 1000000);
                if (erase) {
                    Vec3 inside = world - n * 0.002f;
                    if (!pm::hairAtomAt(ed.entityParts, inside, paintAtom)) return false;
                    cell = paintAtom.id;
                } else if (newStroke) {
                    const pm::Part& surf = onBody ? bodyHost : ed.entityParts[hit];
                    paintAtom = pm::hairAtomOnSurface(surf, world, n, paintCell);
                    int ax = pm::hairDomAxis(n);
                    float s = pm::hairComp(n, ax);
                    for (int k = 0; k < 8 && pm::hairAtomBlocked(ed.entityParts, paintAtom); k++)
                        pm::hairShiftAtom(paintAtom, ax, s, paintCell);
                    if (pm::hairAtomBlocked(ed.entityParts, paintAtom)) return false;
                    strokeAxis = ax;
                    strokeSign = s;
                    strokePlane = (s >= 0.0f) ? pm::hairComp(paintAtom.mn, ax) : pm::hairComp(paintAtom.mx, ax);
                    strokeN = n;
                    cell = paintAtom.id;
                } else {
                    int ax = strokeAxis;
                    float s = strokeSign;
                    Vec3 nPlace{};
                    pm::hairSet(nPlace, ax, (s >= 0.0f) ? 1.0f : -1.0f);
                    float denom = pm::hairComp(rd, ax);
                    if (std::fabs(denom) < 1e-8f) return false;
                    float tPlane = (strokePlane - pm::hairComp(ro, ax)) / denom;
                    if (tPlane < 1e-4f) return false;
                    Vec3 pt = ro + rd * tPlane;
                    int tanHost = (body >= 0 && !pm::isHairPart(ed.entityParts[body])) ? body : hit;
                    const pm::Part& surf = (onBody || tanHost < 0) ? bodyHost : ed.entityParts[tanHost];
                    paintAtom = pm::hairAtomOnSurface(surf, pt, nPlace, paintCell);
                    pm::hairForcePlane(paintAtom, ax, s, strokePlane, paintCell);
                    if (!keyDown(VK_SHIFT) && pm::hairAtomBlocked(ed.entityParts, paintAtom)) return false;
                    strokeN = nPlace;
                    cell = paintAtom.id;
                }
                bool shiftLine = false;
                if (!erase && keyDown(VK_SHIFT)) {
                    if (newStroke) {
                        paintLineHold = true;
                        paintLineAxis = -1;
                        paintLineAnchor = paintAtom;
                    } else {
                        if (!paintLineHold) {
                            paintLineHold = true;
                            paintLineAxis = -1;
                            paintLineAnchor = lastPaintAtom;
                        }
                        int t0 = (strokeAxis + 1) % 3;
                        int t1 = (strokeAxis + 2) % 3;
                        auto mid = [](const pm::HairAtom& a, int ax) {
                            return 0.5f * (pm::hairComp(a.mn, ax) + pm::hairComp(a.mx, ax));
                        };
                        float d0 = mid(paintAtom, t0) - mid(paintLineAnchor, t0);
                        float d1 = mid(paintAtom, t1) - mid(paintLineAnchor, t1);
                        if (paintLineAxis < 0) {
                            if (std::fabs(d0) < paintCell * 0.5f && std::fabs(d1) < paintCell * 0.5f)
                                return false;
                            paintLineAxis = (std::fabs(d0) >= std::fabs(d1)) ? t0 : t1;
                        }
                        int keep = (paintLineAxis == t0) ? t1 : t0;
                        pm::hairSet(paintAtom.mn, keep, pm::hairComp(paintLineAnchor.mn, keep));
                        pm::hairSet(paintAtom.mx, keep, pm::hairComp(paintLineAnchor.mx, keep));
                        pm::hairSet(paintAtom.mn, strokeAxis, pm::hairComp(paintLineAnchor.mn, strokeAxis));
                        pm::hairSet(paintAtom.mx, strokeAxis, pm::hairComp(paintLineAnchor.mx, strokeAxis));
                        paintAtom.id = {
                            (int)std::floor(paintAtom.mn.x / paintCell + 1e-4f),
                            (int)std::floor(paintAtom.mn.y / paintCell + 1e-4f),
                            (int)std::floor(paintAtom.mn.z / paintCell + 1e-4f)
                        };
                        cell = paintAtom.id;
                        shiftLine = paintLineAxis >= 0;
                    }
                } else if (!erase) {
                    paintLineHold = false;
                    paintLineAxis = -1;
                }
                pm::Part tmpl;
                if (erase) {
                    const pm::Part* src = pm::gridHairAt(ed.entityParts, cell);
                    if (!src && hit >= 0 && pm::isGridHairPart(ed.entityParts[hit]))
                        src = &ed.entityParts[hit];
                    if (!src) return false;
                    tmpl = *src;
                } else {
                    bool hostOk = hit >= 0 || (body >= 0 && body < (int)ed.entityParts.size());
                    const pm::Part& hp = onBody || !hostOk
                        ? bodyHost
                        : ed.entityParts[(body >= 0 && !pm::isHairPart(ed.entityParts[body])) ? body : hit];
                    tmpl.color = { ed.r, ed.g, ed.b };
                    tmpl.tex.clear();
                    tmpl.kind = entIsCloth ? "cloth" : "hair";
                    tmpl.bind = -1;
                    if (hp.type == 1 || hp.type == 2 || hp.type == 3) {
                        tmpl.type = hp.type;
                        tmpl.side = hp.side;
                    } else {
                        tmpl.type = 4;
                        tmpl.side = 0;
                    }
                    if (hit >= 0 && pm::isHairPart(ed.entityParts[hit])) {
                        tmpl.tex = ed.entityParts[hit].tex;
                        tmpl.type = ed.entityParts[hit].type;
                        tmpl.side = ed.entityParts[hit].side;
                    }
                }
                if (cell.x == lastHairCell.x && cell.y == lastHairCell.y && cell.z == lastHairCell.z)
                    return false;
                if (!erase && !newStroke && !shiftLine) {
                    if (pm::hairIsOutwardStack(lastHairCell, cell, lastHairN)) return false;
                    if (pm::hairIsOutwardStack(lastHairCell, cell, strokeN)) return false;
                }
                if (shiftLine && paintLineAxis >= 0) {
                    auto mid = [](const pm::HairAtom& a, int ax) {
                        return 0.5f * (pm::hairComp(a.mn, ax) + pm::hairComp(a.mx, ax));
                    };
                    float from = mid(lastPaintAtom, paintLineAxis);
                    float to = mid(paintAtom, paintLineAxis);
                    int steps = (int)std::lround(std::fabs(to - from) / paintCell);
                    if (steps < 1) return false;
                    if (steps > 512) steps = 512;
                    float dir = (to >= from) ? 1.0f : -1.0f;
                    auto before = ed.entityParts;
                    pm::HairAtom cur = lastPaintAtom;
                    bool any = false;
                    for (int i = 0; i < steps; i++) {
                        pm::hairShiftAtom(cur, paintLineAxis, dir, paintCell);
                        if (pm::hairIsOutwardStack(lastHairCell, cur.id, strokeN)) continue;
                        if (!pm::paintHairVoxel(ed.entityParts, cur, tmpl)) continue;
                        any = true;
                        lastHairCell = cur.id;
                    }
                    if (!any) {
                        ed.entityParts = std::move(before);
                        return false;
                    }
                    if (!hairStrokeUndo) {
                        entUndo.push_back(before);
                        if (entUndo.size() > 32) entUndo.erase(entUndo.begin());
                        hairStrokeUndo = true;
                    }
                    for (pm::Part& p : ed.entityParts)
                        if (p.name.empty()) p.name = uniquePartName(entIsCloth ? "cloth" : "hair");
                    entMark.clear();
                    ensureEntMark();
                    for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                        if (!pm::isGridHairPart(ed.entityParts[i])) continue;
                        if (pm::partOwnsHairCell(ed.entityParts[i], lastHairCell)) {
                            markEnt(i, false);
                            break;
                        }
                    }
                    ed.dirty = true;
                    lastHairN = strokeN;
                    lastPaintAtom = cur;
                    lastHairMx = mx;
                    lastHairMy = my;
                    return true;
                }
                auto before = ed.entityParts;
                bool ok = erase
                    ? pm::eraseHairVoxel(ed.entityParts, paintAtom)
                    : pm::paintHairVoxel(ed.entityParts, paintAtom, tmpl);
                if (!ok) {
                    ed.entityParts = std::move(before);
                    return false;
                }
                if (!hairStrokeUndo) {
                    entUndo.push_back(before);
                    if (entUndo.size() > 32) entUndo.erase(entUndo.begin());
                    hairStrokeUndo = true;
                }
                for (pm::Part& p : ed.entityParts)
                    if (p.name.empty()) p.name = uniquePartName(entIsCloth ? "cloth" : "hair");
                entMark.clear();
                ensureEntMark();
                for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                    if (!pm::isGridHairPart(ed.entityParts[i])) continue;
                    if (pm::partOwnsHairCell(ed.entityParts[i], cell)) {
                        markEnt(i, false);
                        break;
                    }
                }
                ed.dirty = true;
                lastHairCell = cell;
                lastHairN = strokeN;
                lastPaintAtom = paintAtom;
                lastHairMx = mx;
                lastHairMy = my;
                return true;
            };
            auto applyHairCardBrush = [&](bool erase) -> bool {
                if (!inRight3d || overEntUi) return false;
                if (lastHairMx > -1e8f) {
                    float pdx = mx - lastHairMx, pdy = my - lastHairMy;
                    if (pdx * pdx + pdy * pdy < 3.0f * 3.0f) return false;
                }
                Vec3 ro, rd;
                rightRay(mx, my, ro, rd);
                float t = 0.0f;
                Vec3 n{};
                if (erase) {
                    int hit = pickPartHit(ro, rd, t, n, false, false);
                    if (hit < 0 || !pm::isHairCardPart(ed.entityParts[hit])) return false;
                    auto before = ed.entityParts;
                    ed.entityParts.erase(ed.entityParts.begin() + hit);
                    if (!hairStrokeUndo) {
                        entUndo.push_back(before);
                        if (entUndo.size() > 32) entUndo.erase(entUndo.begin());
                        hairStrokeUndo = true;
                    }
                    entMark.clear();
                    ensureEntMark();
                    ed.dirty = true;
                    lastHairMx = mx;
                    lastHairMy = my;
                    lastHairCell = { 1000001, hit, 0 };
                    return true;
                }
                int hit = pickPartHit(ro, rd, t, n, true, false);
                if (hit < 0) return false;
                if (n.lengthSq() > 1e-12f) n = n.normalized();
                else n = { 0, 1, 0 };
                Vec3 world = ro + rd * t;
                pm::Part& host = ed.entityParts[hit];
                if (pm::isDecalPart(host) || pm::isHairCardPart(host)) return false;
                if (host.name.empty()) host.name = uniquePartName("part");
                pm::HairCardSlot sl;
                if (!pm::hairCardSlotOnHost(host, world, n, sl)) return false;
                if (lastHairCell.x == sl.iu && lastHairCell.y == sl.iv && lastHairCell.z == hit)
                    return false;
                std::string tex = ed.hairCardTex;
                Vec3 col{ ed.r, ed.g, ed.b };
                int exist = pm::findHairCard(ed.entityParts, sl);
                if (exist >= 0) {
                    const pm::Part& cur = ed.entityParts[exist];
                    if (cur.tex == tex && pm::hairColorSame(cur.color, col)) {
                        lastHairCell = { sl.iu, sl.iv, hit };
                        lastHairMx = mx;
                        lastHairMy = my;
                        return false;
                    }
                }
                if (exist < 0 && pm::hairCardCount(ed.entityParts) >= pm::kHairCardMax) return false;
                auto before = ed.entityParts;
                if (exist >= 0) {
                    ed.entityParts[exist].color = col;
                    ed.entityParts[exist].tex = tex;
                } else {
                    ed.entityParts.push_back(pm::makeHairCard(host, sl, col, tex, uniquePartName("card")));
                }
                if (!hairStrokeUndo) {
                    entUndo.push_back(before);
                    if (entUndo.size() > 32) entUndo.erase(entUndo.begin());
                    hairStrokeUndo = true;
                }
                entMark.clear();
                ensureEntMark();
                markEnt(exist >= 0 ? exist : (int)ed.entityParts.size() - 1, false);
                ed.dirty = true;
                lastHairCell = { sl.iu, sl.iv, hit };
                lastHairMx = mx;
                lastHairMy = my;
                return true;
            };
            bool addSel = keyDown(VK_SHIFT) || keyDown(VK_CONTROL);
            bool ctrlOnly = keyDown(VK_CONTROL) && !keyDown(VK_SHIFT);
            auto hairAtomInRect = [&](const pm::HairAtom& a, float x0, float y0, float x1, float y1) -> bool {
                float sx, sy;
                Vec3 c = (a.mn + a.mx) * 0.5f;
                if (!projectRight(c.x, c.y, c.z, sx, sy)) return false;
                float rx0 = std::min(x0, x1), rx1 = std::max(x0, x1);
                float ry0 = std::min(y0, y1), ry1 = std::max(y0, y1);
                return sx >= rx0 && sx <= rx1 && sy >= ry0 && sy <= ry1;
            };
            auto hairToggleList = [&](std::vector<pm::HairAtom>& dst, const std::vector<pm::HairAtom>& list, bool partClick) {
                auto add = [&](const pm::HairAtom& a) {
                    if (pm::hairAtomSelIndex(dst, a) < 0) dst.push_back(a);
                };
                if (partClick) {
                    bool allOn = !list.empty();
                    for (const pm::HairAtom& a : list)
                        if (pm::hairAtomSelIndex(dst, a) < 0) { allOn = false; break; }
                    if (allOn) {
                        for (const pm::HairAtom& a : list) {
                            int i = pm::hairAtomSelIndex(dst, a);
                            if (i >= 0) dst.erase(dst.begin() + i);
                        }
                    } else {
                        for (const pm::HairAtom& a : list) add(a);
                    }
                    return;
                }
                for (const pm::HairAtom& a : list) {
                    int i = pm::hairAtomSelIndex(dst, a);
                    if (i >= 0) dst.erase(dst.begin() + i);
                    else dst.push_back(a);
                }
            };
            auto hairAddList = [&](std::vector<pm::HairAtom>& dst, const std::vector<pm::HairAtom>& list) {
                for (const pm::HairAtom& a : list)
                    if (pm::hairAtomSelIndex(dst, a) < 0) dst.push_back(a);
            };
            auto hairApplySel = [&](const std::vector<pm::HairAtom>& vox, const std::vector<pm::HairAtom>& cards,
                                   bool replace, bool toggle, bool partClick) {
                if (vox.empty() && cards.empty()) {
                    if (replace) {
                        ed.hairVoxSel.clear();
                        ed.hairCardSel.clear();
                        syncHairVoxMarks();
                    }
                    return;
                }
                if (replace) {
                    ed.hairVoxSel.clear();
                    ed.hairCardSel.clear();
                }
                if (toggle) {
                    if (!vox.empty()) hairToggleList(ed.hairVoxSel, vox, partClick);
                    if (!cards.empty()) hairToggleList(ed.hairCardSel, cards, partClick);
                } else {
                    hairAddList(ed.hairVoxSel, vox);
                    hairAddList(ed.hairCardSel, cards);
                }
                syncHairVoxMarks();
            };
            auto pickHairHitAt = [&](float px, float py, int& hit, Vec3& inside) -> bool {
                Vec3 ro, rd;
                rightRay(px, py, ro, rd);
                float t = 0.0f;
                Vec3 n{};
                hit = pickPartHit(ro, rd, t, n, false, false);
                if (hit < 0) return false;
                if (n.lengthSq() > 1e-12f) n = n.normalized();
                const pm::Part& p = ed.entityParts[hit];
                if (pm::isHairCardPart(p))
                    inside = ro + rd * (t + 1e-4f);
                else if (pm::isGridHairPart(p))
                    inside = ro + rd * t - n * 0.002f;
                else
                    return false;
                return true;
            };
            auto hairCardAtomOnPart = [&](int hi, const Vec3& inside, pm::HairAtom& out) -> bool {
                if (hi < 0 || hi >= (int)ed.entityParts.size()) return false;
                const pm::Part& p = ed.entityParts[hi];
                if (!pm::isHairCardPart(p)) return false;
                std::vector<pm::HairAtom> atoms;
                pm::voxelizeHairCardAtoms(p, atoms);
                for (const pm::HairAtom& a : atoms) {
                    if (pm::hairPointInAabb(inside, a.mn, a.mx, 1e-3f)) {
                        out = a;
                        return true;
                    }
                }
                out = pm::hairCardPartAtom(p);
                return true;
            };
            auto finishHairSelect = [&](bool rmbSel) {
                float dx = ed.hairSelX1 - ed.hairSelX0, dy = ed.hairSelY1 - ed.hairSelY0;
                bool drag = dx * dx + dy * dy >= 16.0f;
                bool replace = !addSel;
                bool toggle = ctrlOnly;
                std::vector<pm::HairAtom> vox, cards;
                if (!drag) {
                    if (rmbSel) {
                        Vec3 ro, rd;
                        rightRay(ed.hairSelX0, ed.hairSelY0, ro, rd);
                        int hit = pickPartRay(ro, rd);
                        if (hit >= 0 && pm::isGridHairPart(ed.entityParts[hit]))
                            pm::hairVoxelizeClump(ed.entityParts, hit, vox);
                        else if (hit >= 0 && pm::isHairCardPart(ed.entityParts[hit]))
                            pm::hairCardVoxelizeClump(ed.entityParts, hit, cards);
                    } else {
                        int hit = -1;
                        Vec3 inside{};
                        if (pickHairHitAt(ed.hairSelX0, ed.hairSelY0, hit, inside)) {
                            if (pm::isHairCardPart(ed.entityParts[hit])) {
                                pm::HairAtom a;
                                if (hairCardAtomOnPart(hit, inside, a)) cards.push_back(a);
                            } else {
                                pm::HairAtom a;
                                if (pm::hairAtomAt(ed.entityParts, inside, a)) vox.push_back(a);
                            }
                        }
                    }
                    hairApplySel(vox, cards, replace, toggle, rmbSel);
                    return;
                }
                if (rmbSel) {
                    for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                        if (pm::isGridHairPart(ed.entityParts[i])) {
                            std::vector<pm::HairAtom> local;
                            pm::voxelizeHairAtoms(ed.entityParts[i], local);
                            for (const pm::HairAtom& a : local) {
                                if (!hairAtomInRect(a, ed.hairSelX0, ed.hairSelY0, ed.hairSelX1, ed.hairSelY1))
                                    continue;
                                pm::hairVoxelizeClump(ed.entityParts, i, vox);
                                break;
                            }
                        } else if (pm::isHairCardPart(ed.entityParts[i])) {
                            std::vector<pm::HairAtom> local;
                            pm::voxelizeHairCardAtoms(ed.entityParts[i], local);
                            for (const pm::HairAtom& a : local) {
                                if (!hairAtomInRect(a, ed.hairSelX0, ed.hairSelY0, ed.hairSelX1, ed.hairSelY1))
                                    continue;
                                pm::hairCardVoxelizeClump(ed.entityParts, i, cards);
                                break;
                            }
                        }
                    }
                } else {
                    for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                        if (pm::isGridHairPart(ed.entityParts[i])) {
                            std::vector<pm::HairAtom> local;
                            pm::voxelizeHairAtoms(ed.entityParts[i], local);
                            for (const pm::HairAtom& a : local)
                                if (hairAtomInRect(a, ed.hairSelX0, ed.hairSelY0, ed.hairSelX1, ed.hairSelY1))
                                    vox.push_back(a);
                        } else if (pm::isHairCardPart(ed.entityParts[i])) {
                            std::vector<pm::HairAtom> local;
                            pm::voxelizeHairCardAtoms(ed.entityParts[i], local);
                            for (const pm::HairAtom& a : local)
                                if (hairAtomInRect(a, ed.hairSelX0, ed.hairSelY0, ed.hairSelX1, ed.hairSelY1))
                                    cards.push_back(a);
                        }
                    }
                }
                hairApplySel(vox, cards, replace, toggle, rmbSel);
            };
            ed.cutVis = false;
            if (ed.partCut && inRight3d && !overEntUi) {
                Vec3 ro, rd;
                rightRay(mx, my, ro, rd);
                float tHit = 0;
                Vec3 nrm{};
                int hit = pickPartHit(ro, rd, tHit, nrm, true, false);
                if (hit >= 0 && ed.entityParts[hit].rot.lengthSq() <= 1e-6f
                    && !pm::isDecalPart(ed.entityParts[hit]) && !pm::isHairCardPart(ed.entityParts[hit])) {
                    if (nrm.lengthSq() > 1e-12f) nrm = nrm.normalized();
                    Vec3 inside = ro + rd * tHit - nrm * 0.002f;
                    pm::HairCell cell = pm::worldToHairCell(inside);
                    float g = pm::kHairGrid;
                    const pm::Part& hp = ed.entityParts[hit];
                    Vec3 pmn = hp.center - hp.half, pmx = hp.center + hp.half;
                    int ax = std::fabs(nrm.x) > std::fabs(nrm.y) && std::fabs(nrm.x) > std::fabs(nrm.z) ? 0
                        : (std::fabs(nrm.y) > std::fabs(nrm.z) ? 1 : 2);
                    bool outward = (ax == 0 ? nrm.x : ax == 1 ? nrm.y : nrm.z) >= 0.0f;
                    float surf = outward ? (ax == 0 ? pmx.x : ax == 1 ? pmx.y : pmx.z)
                                         : (ax == 0 ? pmn.x : ax == 1 ? pmn.y : pmn.z);
                    float c0 = outward ? surf - g : surf;
                    float c1 = outward ? surf : surf + g;
                    ed.cutMn[0] = cell.x * g; ed.cutMn[1] = cell.y * g; ed.cutMn[2] = cell.z * g;
                    ed.cutMx[0] = ed.cutMn[0] + g; ed.cutMx[1] = ed.cutMn[1] + g; ed.cutMx[2] = ed.cutMn[2] + g;
                    if (ax == 0) { ed.cutMn[0] = c0; ed.cutMx[0] = c1; }
                    else if (ax == 1) { ed.cutMn[1] = c0; ed.cutMx[1] = c1; }
                    else { ed.cutMn[2] = c0; ed.cutMx[2] = c1; }
                    ed.cutVis = true;
                    if (lmb && !prevLmb) {
                        pushEntUndo();
                        bool any = pm::subtractPartBox(ed.entityParts, hit, { ed.cutMn[0], ed.cutMn[1], ed.cutMn[2] },
                                                       { ed.cutMx[0], ed.cutMx[1], ed.cutMx[2] });
                        if (!any) entUndo.pop_back();
                        else {
                            entMark.clear();
                            ensureEntMark();
                            if (!ed.entityParts.empty()) markEnt((int)ed.entityParts.size() - 1, false);
                            ed.dirty = true;
                        }
                    }
                }
            }
            {
                static int stickAxis = -1;
                static float stickPlane = 0, sU0 = 0, sU1 = 0, sV0 = 0, sV1 = 0, stickSign = 1;
                if ((!ed.partAdd && !ed.partMeasure && !ed.partPlane) || (!entIsCloth && !ed.partPlane)) {
                    ed.partAddVis = false;
                    ed.partAnchor = false;
                    stickAxis = -1;
                } else if (!(inRight3d && !overEntUi)) {
                    ed.partAddVis = false;
                } else {
                struct R2 { float u0, u1, v0, v1; };
                struct SrcFace {
                    int box = 0, axis = 0, ua = 0, va = 0;
                    float plane = 0, u0 = 0, u1 = 0, v0 = 0, v1 = 0;
                    std::vector<R2> exposed;
                };
                std::vector<SrcFace> srcs;
                int boxId = 0;
                auto addSolid = [&](const pm::Part& p) {
                    if (p.rot.lengthSq() > 1e-6f || pm::isDecalPart(p) || pm::isHairCardPart(p)) return;
                    Vec3 bmn = p.center - p.half, bmx = p.center + p.half;
                    float mn[3] = { bmn.x, bmn.y, bmn.z }, mx[3] = { bmx.x, bmx.y, bmx.z };
                    int id = boxId++;
                    for (int fi = 0; fi < 6; fi++) {
                        SrcFace f;
                        f.box = id;
                        f.axis = fi / 2;
                        f.ua = (f.axis + 1) % 3;
                        f.va = (f.axis + 2) % 3;
                        f.plane = (fi % 2 == 0) ? mn[f.axis] : mx[f.axis];
                        f.u0 = mn[f.ua]; f.u1 = mx[f.ua];
                        f.v0 = mn[f.va]; f.v1 = mx[f.va];
                        f.exposed.push_back({ f.u0, f.u1, f.v0, f.v1 });
                        srcs.push_back(std::move(f));
                    }
                };
                for (const pm::Part& p : ed.entityParts) addSolid(p);
                if (ed.clothBody) {
                    for (const pm::Part& p : pm::buildPlayerModel()) addSolid(p);
                }
                const float eps = 1e-4f;
                auto subtractRect = [&](std::vector<R2>& rects, const R2& cut) {
                    std::vector<R2> next;
                    for (const R2& r : rects) {
                        float iu0 = std::max(r.u0, cut.u0), iu1 = std::min(r.u1, cut.u1);
                        float iv0 = std::max(r.v0, cut.v0), iv1 = std::min(r.v1, cut.v1);
                        if (iu1 - iu0 <= eps || iv1 - iv0 <= eps) { next.push_back(r); continue; }
                        if (r.u0 < iu0 - eps) next.push_back({ r.u0, iu0, r.v0, r.v1 });
                        if (iu1 < r.u1 - eps) next.push_back({ iu1, r.u1, r.v0, r.v1 });
                        if (r.v0 < iv0 - eps) next.push_back({ iu0, iu1, r.v0, iv0 });
                        if (iv1 < r.v1 - eps) next.push_back({ iu0, iu1, iv1, r.v1 });
                    }
                    rects.swap(next);
                };
                auto splitRect = [&](std::vector<R2>& rects, bool alongU, float c, float s0, float s1) {
                    std::vector<R2> next;
                    for (const R2& r : rects) {
                        float span0 = alongU ? r.v0 : r.u0, span1 = alongU ? r.v1 : r.u1;
                        float cross0 = alongU ? r.u0 : r.v0, cross1 = alongU ? r.u1 : r.v1;
                        float iv0 = std::max(span0, s0), iv1 = std::min(span1, s1);
                        if (iv1 - iv0 <= eps || c <= cross0 + eps || c >= cross1 - eps) { next.push_back(r); continue; }
                        auto push = [&](float a0, float a1, float b0, float b1) {
                            if (a1 - a0 <= eps || b1 - b0 <= eps) return;
                            if (alongU) next.push_back({ a0, a1, b0, b1 });
                            else next.push_back({ b0, b1, a0, a1 });
                        };
                        if (span0 < iv0 - eps) push(cross0, cross1, span0, iv0);
                        if (iv1 < span1 - eps) push(cross0, cross1, iv1, span1);
                        push(cross0, c, iv0, iv1);
                        push(c, cross1, iv0, iv1);
                    }
                    rects.swap(next);
                };
                for (int pass = 0; pass < 3; pass++) {
                    std::vector<std::vector<R2>> prev;
                    prev.reserve(srcs.size());
                    for (const SrcFace& s : srcs) prev.push_back(s.exposed);
                    for (int i = 0; i < (int)srcs.size(); i++) {
                        std::vector<R2> rects = { { srcs[i].u0, srcs[i].u1, srcs[i].v0, srcs[i].v1 } };
                        for (int j = 0; j < (int)srcs.size(); j++) {
                            if (j == i || srcs[j].box == srcs[i].box) continue;
                            const SrcFace& cut = srcs[j];
                            if (cut.axis == srcs[i].axis) {
                                if (std::fabs(cut.plane - srcs[i].plane) > eps) continue;
                                for (const R2& er : prev[j]) subtractRect(rects, er);
                            } else if (cut.axis == srcs[i].ua || cut.axis == srcs[i].va) {
                                bool alongU = cut.axis == srcs[i].ua;
                                int spanAxis = alongU ? srcs[i].va : srcs[i].ua;
                                for (const R2& er : prev[j]) {
                                    float s0, s1;
                                    auto coordRange = [&](int ax, float& a0, float& a1) {
                                        if (ax == cut.axis) { a0 = a1 = cut.plane; }
                                        else if (ax == cut.ua) { a0 = er.u0; a1 = er.u1; }
                                        else { a0 = er.v0; a1 = er.v1; }
                                    };
                                    float c0, c1;
                                    coordRange(srcs[i].axis, c0, c1);
                                    if (srcs[i].plane < c0 - eps || srcs[i].plane > c1 + eps) continue;
                                    coordRange(spanAxis, s0, s1);
                                    splitRect(rects, alongU, cut.plane, s0, s1);
                                }
                            }
                        }
                        srcs[i].exposed.swap(rects);
                    }
                }
                struct Face { int axis, ua, va; float plane, u0, u1, v0, v1; };
                std::vector<Face> faces;
                for (const SrcFace& s : srcs) {
                    for (const R2& r : s.exposed) {
                        if (r.u1 - r.u0 <= eps || r.v1 - r.v0 <= eps) continue;
                        faces.push_back({ s.axis, s.ua, s.va, s.plane, r.u0, r.u1, r.v0, r.v1 });
                    }
                }
                const float faceStick = 0.03f;
                Vec3 ro, rd;
                rightRay(mx, my, ro, rd);
                float tHit = 0;
                Vec3 nrm{};
                int hit = -1;
                auto consider = [&](const std::vector<pm::Part>& src, int base) {
                    for (int i = 0; i < (int)src.size(); i++) {
                        const pm::Part& p = src[i];
                        if (p.rot.lengthSq() > 1e-6f || pm::isDecalPart(p) || pm::isHairCardPart(p)) continue;
                        Vec3 roL = pm::partLocalOffset(p, ro);
                        Vec3 rdL = pm::unrotateEuler(rd, p.rot);
                        float t;
                        if (!rayHitAABB(roL, rdL, { -p.half.x, -p.half.y, -p.half.z }, { p.half.x, p.half.y, p.half.z }, t)) continue;
                        if (hit >= 0 && t >= tHit) continue;
                        tHit = t;
                        hit = base + i;
                        Vec3 h = roL + rdL * t;
                        float d[6] = {
                            std::fabs(h.x + p.half.x), std::fabs(h.x - p.half.x),
                            std::fabs(h.y + p.half.y), std::fabs(h.y - p.half.y),
                            std::fabs(h.z + p.half.z), std::fabs(h.z - p.half.z)
                        };
                        int fi = 0;
                        for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
                        Vec3 nL{};
                        if (fi == 0) nL = { -1, 0, 0 };
                        else if (fi == 1) nL = { 1, 0, 0 };
                        else if (fi == 2) nL = { 0, -1, 0 };
                        else if (fi == 3) nL = { 0, 1, 0 };
                        else if (fi == 4) nL = { 0, 0, -1 };
                        else nL = { 0, 0, 1 };
                        nrm = nL;
                    }
                };
                consider(ed.entityParts, 0);
                std::vector<pm::Part> bodyParts;
                if (ed.clothBody) {
                    bodyParts = pm::buildPlayerModel();
                    consider(bodyParts, 100000);
                }
                ed.partAddVis = hit >= 0;
                if (hit >= 0) {
                    const float h = 0.0015f;
                    bool snapOn = !keyDown(VK_CONTROL);
                    bool alignOn = !keyDown(VK_SHIFT);
                    Vec3 hp = ro + rd * tHit;
                    float p[3] = { hp.x, hp.y, hp.z };
                    float n[3] = { nrm.x, nrm.y, nrm.z };
                    int ax = 0;
                    if (std::fabs(n[1]) > std::fabs(n[ax])) ax = 1;
                    if (std::fabs(n[2]) > std::fabs(n[ax])) ax = 2;
                    auto clampSpan = [&](float v, float a0, float a1) {
                        float lo = a0, hi = a1;
                        if (alignOn) {
                            lo += h; hi -= h;
                            if (lo > hi) lo = hi = 0.5f * (a0 + a1);
                        }
                        if (v < lo) v = lo;
                        if (v > hi) v = hi;
                        return v;
                    };
                    bool placed = false;
                    if (snapOn && stickAxis >= 0) {
                        float roA = stickAxis == 0 ? ro.x : stickAxis == 1 ? ro.y : ro.z;
                        float rdA = stickAxis == 0 ? rd.x : stickAxis == 1 ? rd.y : rd.z;
                        if (std::fabs(rdA) > 1e-6f) {
                            float t = (stickPlane - roA) / rdA;
                            if (t >= 0.0f) {
                                Vec3 q = ro + rd * t;
                                float qv[3] = { q.x, q.y, q.z };
                                int ua = (stickAxis + 1) % 3, va = (stickAxis + 2) % 3;
                                if (qv[ua] >= sU0 - faceStick && qv[ua] <= sU1 + faceStick &&
                                    qv[va] >= sV0 - faceStick && qv[va] <= sV1 + faceStick) {
                                    p[stickAxis] = stickPlane;
                                    p[ua] = clampSpan(qv[ua], sU0, sU1);
                                    p[va] = clampSpan(qv[va], sV0, sV1);
                                    if (alignOn) p[stickAxis] += stickSign * h;
                                    placed = true;
                                }
                            }
                        }
                    }
                    if (!placed) {
                        stickAxis = -1;
                        const Face* on = nullptr;
                        for (const Face& f : faces) {
                            if (f.axis != ax || std::fabs(p[ax] - f.plane) > 0.004f) continue;
                            if (p[f.ua] < f.u0 || p[f.ua] > f.u1 || p[f.va] < f.v0 || p[f.va] > f.v1) continue;
                            on = &f;
                            break;
                        }
                        if (on && snapOn) {
                            stickAxis = on->axis;
                            stickPlane = on->plane;
                            sU0 = on->u0; sU1 = on->u1; sV0 = on->v0; sV1 = on->v1;
                            stickSign = n[ax] >= 0.0f ? 1.0f : -1.0f;
                            p[on->axis] = on->plane;
                            p[on->ua] = clampSpan(p[on->ua], on->u0, on->u1);
                            p[on->va] = clampSpan(p[on->va], on->v0, on->v1);
                        }
                        if (alignOn) p[ax] += (n[ax] >= 0.0f ? h : -h);
                    }
                    ed.partB[0] = p[0]; ed.partB[1] = p[1]; ed.partB[2] = p[2];
                    if (ed.partAnchor) {
                        for (int k = 0; k < 3; k++) {
                            ed.partMn[k] = std::min(ed.partA[k], p[k]) - h;
                            ed.partMx[k] = std::max(ed.partA[k], p[k]) + h;
                        }
                    }
                    if (lmb && !prevLmb) {
                        if (ed.partPlane) {
                            int fax = 0;
                            if (std::fabs(n[1]) > std::fabs(n[fax])) fax = 1;
                            if (std::fabs(n[2]) > std::fabs(n[fax])) fax = 2;
                            int face = fax * 2 + (n[fax] >= 0.0f ? 0 : 1);
                            bool body = hit >= 100000;
                            int part = body ? hit - 100000 : hit;
                            if (ed.planeOn && ed.planeBody == body && ed.planePart == part && ed.planeFace == face)
                                ed.planeOn = false;
                            else {
                                ed.planeOn = true;
                                ed.planeBody = body;
                                ed.planePart = part;
                                ed.planeFace = face;
                            }
                        } else if (!ed.partAnchor) {
                            ed.partAnchor = true;
                            ed.partA[0] = p[0]; ed.partA[1] = p[1]; ed.partA[2] = p[2];
                        } else if (ed.partMeasure) {
                            ed.measureOn = true;
                            ed.measureA[0] = ed.partA[0]; ed.measureA[1] = ed.partA[1]; ed.measureA[2] = ed.partA[2];
                            ed.measureB[0] = p[0]; ed.measureB[1] = p[1]; ed.measureB[2] = p[2];
                            ed.partAnchor = false;
                        } else {
                            float dx = ed.partMx[0] - ed.partMn[0];
                            float dy = ed.partMx[1] - ed.partMn[1];
                            float dz = ed.partMx[2] - ed.partMn[2];
                            pushEntUndo();
                            pm::Part np;
                            np.kind = "cloth";
                            np.type = 1;
                            np.color = { ed.r, ed.g, ed.b };
                            np.name = uniquePartName("cloth");
                            np.center = { (ed.partMn[0] + ed.partMx[0]) * 0.5f, (ed.partMn[1] + ed.partMx[1]) * 0.5f, (ed.partMn[2] + ed.partMx[2]) * 0.5f };
                            np.half = { dx * 0.5f, dy * 0.5f, dz * 0.5f };
                            ed.entityParts.push_back(np);
                            markEnt((int)ed.entityParts.size() - 1, false);
                            ed.dirty = true;
                            ed.partAnchor = false;
                        }
                    }
                }
                }
            }
            bool eraseStroke = ed.hairErase || ((ed.hairPaint || ed.hairCard) && keyDown('E'));
            if (ed.hairCard && !ed.picker && lmb && inRight3d && !overEntUi)
                applyHairCardBrush(eraseStroke);
            else if ((ed.hairPaint || ed.hairErase) && !ed.picker && lmb && inRight3d && !overEntUi)
                applyHairBrush(eraseStroke);
            if (ed.hairSelect && inRight3d && !overEntUi && !slideHov) {
                if (lmb && !prevLmb) {
                    ed.hairSelDrag = true;
                    ed.hairSelRmb = false;
                    ed.hairSelX0 = ed.hairSelX1 = mx;
                    ed.hairSelY0 = ed.hairSelY1 = my;
                }
                if (rmb && !prevRmb && !keyDown(VK_MENU) && !mmb) {
                    ed.hairSelDrag = true;
                    ed.hairSelRmb = true;
                    ed.hairSelX0 = ed.hairSelX1 = mx;
                    ed.hairSelY0 = ed.hairSelY1 = my;
                }
            }
            if (ed.hairSelDrag) {
                bool held = ed.hairSelRmb ? rmb : lmb;
                if (held) {
                    ed.hairSelX1 = mx;
                    ed.hairSelY1 = my;
                } else {
                    finishHairSelect(ed.hairSelRmb);
                    ed.hairSelDrag = false;
                }
            }
            if (!ed.hairSelect) ed.hairSelDrag = false;
            if (lmb && !prevLmb && inRight3d && !slideHov && !overExit) {
                if (ed.picker && !overEntUi) {
                    // eyedropper samples the right 3D model; do not select
                } else if ((ed.hairPaint || ed.hairErase || ed.hairSelect || ed.hairCard || ed.partAdd || ed.partMeasure || ed.partPlane) && !overEntUi) {
                    // voxel brush / select owns the click
                } else if (ed.refTool && !overEntUi &&
                    ed.refDecalIdx >= 0 && ed.refEdge < 0 && ed.refLineHover >= 0) {
                    ed.refEdge = ed.refLineHover;
                } else if (ed.refTool && !overEntUi && ed.refEdge < 0) {
                    Vec3 ro, rd;
                    rightRay(mx, my, ro, rd);
                    int hit = pickDecalRay(ed.entityParts, ro, rd);
                    if (hit >= 0) {
                        ed.refDecalIdx = hit;
                        ed.refEdge = -1;
                        markEnt(hit, addSel);
                    } else if (!addSel && ed.gizmoHover != 0 && canEntGizmo &&
                        (entGzMove || entGzStretch || entGzFill || entGzRot || clickOnlyTool())) {
                        beginEntGzDrag();
                    } else {
                        int hitP = pickPartRay(ro, rd);
                        if (hitP >= 0) markEnt(hitP, addSel);
                    }
                } else if (!addSel && ed.gizmoHover != 0 && canEntGizmo &&
                    (entGzMove || entGzStretch || entGzFill || entGzRot || clickOnlyTool())) {
                    beginEntGzDrag();
                } else {
                    Vec3 ro, rd;
                    rightRay(mx, my, ro, rd);
                    int hit = pickPartRay(ro, rd);
                    if (hit >= 0) markEnt(hit, addSel);
                }
            }
            ed.hoverX = -1; ed.hoverY = -1;
            gPxClip.on = false;
            bool paintView = !hairToolsOn && (paintKind() >= 0 || ed.entSkinView);
            bool clothPaint3d = !hairToolsOn && !ed.entSkinView && paintKind() < 0 && (inLeft3d || inRight3d) && !overEntUi && skinImg.ok()
                && (ed.picker || (ed.tool >= 0 && ed.tool <= 8));
            bool paintOn = (paintView || clothPaint3d) && (ed.picker || (ed.tool >= 0 && ed.tool <= 8));
            if (paintView && paintDispW > 1.0f && paintImg && paintImg->ok()) {
                int hx = (int)((mx - skinImgX) / paintDispW * (float)paintTw);
                int hy = (int)((my - skinImgY) / paintDispH * (float)paintTh);
                if (hx >= 0 && hx < paintTw && hy >= 0 && hy < paintTh) {
                    ed.hoverX = hx; ed.hoverY = hy;
                }
            }
            auto pushRecent = [&](uint8_t rr, uint8_t gg, uint8_t bb, uint8_t aa) {
                for (size_t k = 0; k < ed.recent.size(); k++) {
                    if (ed.recent[k].r == rr && ed.recent[k].g == gg && ed.recent[k].b == bb && ed.recent[k].a == aa) {
                        ed.recent.erase(ed.recent.begin() + k);
                        break;
                    }
                }
                ed.recent.push_back({ rr, gg, bb, aa });
                if (ed.recent.size() > 8) ed.recent.erase(ed.recent.begin());
            };
            if (ed.picker && lmb && !prevLmb && inRight3d && !overEntUi && !slideHov) {
                auto cuboidHitFace = [&](const pm::Part& p, const Vec3& world) -> int {
                    Vec3 h = pm::partLocalOffset(p, world);
                    Vec3 mn{ -p.half.x, -p.half.y, -p.half.z };
                    Vec3 mxb{ p.half.x, p.half.y, p.half.z };
                    float d[6] = {
                        std::fabs(h.x - mn.x), std::fabs(h.x - mxb.x),
                        std::fabs(h.y - mn.y), std::fabs(h.y - mxb.y),
                        std::fabs(h.z - mn.z), std::fabs(h.z - mxb.z)
                    };
                    int fi = 0;
                    for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
                    return fi;
                };
                auto quadST = [&](const Vec3 q[4], const Vec3& hit, float& su, float& sv) {
                    Vec3 e1 = q[1] - q[0], e2 = q[3] - q[0], d = hit - q[0];
                    float a = e1.dot(e1), b = e2.dot(e2);
                    su = (a > 1e-12f) ? e1.dot(d) / a : 0.5f;
                    sv = (b > 1e-12f) ? e2.dot(d) / b : 0.5f;
                    if (su < 0) su = 0; if (su > 1) su = 1;
                    if (sv < 0) sv = 0; if (sv > 1) sv = 1;
                };
                auto sampleImgUV = [&](const mat::Image& img, float u, float v, Color4& out) -> bool {
                    if (!img.ok()) return false;
                    int px = (int)std::floor(u * (float)img.w);
                    int py = (int)std::floor(v * (float)img.h);
                    if (px < 0) px = 0; if (px >= img.w) px = img.w - 1;
                    if (py < 0) py = 0; if (py >= img.h) py = img.h - 1;
                    size_t i = ((size_t)py * (size_t)img.w + (size_t)px) * 4;
                    out = { img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3] };
                    return true;
                };
                auto cpuImgForTex = [&](const std::string& name) -> const mat::Image* {
                    if (name.empty()) return nullptr;
                    if (pm::isEyeTex(name) && eyeImg.ok()) return &eyeImg;
                    if (pm::isEyelidTex(name) && eyelidImg.ok()) return &eyelidImg;
                    for (int i = 0; i < (int)entMouthNames.size() && i < 3; i++)
                        if (name == entMouthNames[i] && mouthImgs[i].ok()) return &mouthImgs[i];
                    if (pm::isMouthTex(name)) {
                        int mi = pm::mouthIndex(name);
                        if (mi >= 0 && mi < 3 && mouthImgs[mi].ok()) return &mouthImgs[mi];
                    }
                    int ti = mat::tileIndex(name.c_str());
                    if (ti >= 0 && mat::g_tileImages[ti].ok()) return &mat::g_tileImages[ti];
                    for (const ExtraMat& e : extraMats)
                        if (e.name == name && e.img.ok()) return &e.img;
                    return nullptr;
                };
                Vec3 ro, rd;
                rightRay(mx, my, ro, rd);
                float tHit = 0.0f;
                Vec3 nrm{};
                int hit = pickPartHit(ro, rd, tHit, nrm, false, false);
                if (hit >= 0) {
                    const pm::Part& p = ed.entityParts[hit];
                    Vec3 hitW = ro + rd * tHit;
                    auto commitPick = [&](float rr, float gg, float bb, float aa) {
                        ed.r = rr; ed.g = gg; ed.b = bb; ed.a = aa;
                        Color4 c{ (uint8_t)(rr * 255.0f), (uint8_t)(gg * 255.0f),
                                  (uint8_t)(bb * 255.0f), (uint8_t)(aa * 255.0f) };
                        if (ed.tool == 1) addBrushColor(c);
                        pushRecent(c.r, c.g, c.b, c.a);
                        ed.picker = false;
                        ed.justPicked = true;
                    };
                    auto sampleTinted = [&](const mat::Image& img, float u, float v,
                                            float tr, float tg, float tb) -> bool {
                        Color4 c;
                        if (!sampleImgUV(img, u, v, c)) return false;
                        if (!ed.pickAlpha0 && c.a == 0) return false;
                        commitPick((c.r / 255.0f) * tr, (c.g / 255.0f) * tg,
                                   (c.b / 255.0f) * tb, c.a / 255.0f);
                        return true;
                    };
                    auto uvFromFace = [&](const Vec3 q[4], float u0, float v0, float u1, float v1,
                                          float& u, float& v) {
                        float su = 0.5f, sv = 0.5f;
                        quadST(q, hitW, su, sv);
                        u = u0 + (u1 - u0) * su;
                        v = v1 + (v0 - v1) * sv;
                    };
                    bool got = false;
                    if (pm::isDecalPart(p) || pm::isHairCardPart(p)) {
                        Vec3 q[4];
                        pm::texQuadLocal(p, q);
                        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                        if (p.side < 0) { float t = u0; u0 = u1; u1 = t; }
                        pm::applyPartUvFlip(p, u0, v0, u1, v1);
                        float u, v;
                        uvFromFace(q, u0, v0, u1, v1, u, v);
                        if (const mat::Image* img = cpuImgForTex(p.tex))
                            got = sampleTinted(*img, u, v, p.color.x, p.color.y, p.color.z);
                    } else if (pm::isHairPart(p)) {
                        if (!p.tex.empty() && !pm::isCutoutOverlay(p.tex)) {
                            Vec3 q[4];
                            pm::cuboidFaceCorners(p, cuboidHitFace(p, hitW), q);
                            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                            pm::applyPartUvFlip(p, u0, v0, u1, v1);
                            float u, v;
                            uvFromFace(q, u0, v0, u1, v1, u, v);
                            if (const mat::Image* img = cpuImgForTex(p.tex))
                                got = sampleTinted(*img, u, v, p.color.x, p.color.y, p.color.z);
                        }
                    } else if (pm::partHasBox(p) && entSheetW > 0 && skinImg.ok()) {
                        int fi = cuboidHitFace(p, hitW);
                        Vec3 q[4];
                        pm::cuboidFaceCorners(p, fi, q);
                        float u0, v0, u1, v1;
                        pm::modelBoxUV(p, fi, entSheetW, entSheetH, u0, v0, u1, v1);
                        float u, v;
                        uvFromFace(q, u0, v0, u1, v1, u, v);
                        got = sampleTinted(skinImg, u, v, 1.0f, 1.0f, 1.0f);
                    } else if (!entIsCloth && !entSkin.empty() && skinImg.ok()) {
                        int fi = cuboidHitFace(p, hitW);
                        Vec3 q[4];
                        pm::cuboidFaceCorners(p, fi, q);
                        float u0, v0, u1, v1;
                        pm::skinFaceUV(p, fi, u0, v0, u1, v1);
                        float u, v;
                        uvFromFace(q, u0, v0, u1, v1, u, v);
                        got = sampleTinted(skinImg, u, v, 1.0f, 1.0f, 1.0f);
                    }
                    if (!got) commitPick(p.color.x, p.color.y, p.color.z, 1.0f);
                }
            }
            static PxClip clothClip;
            int faceStampW = 1, faceStampH = 1;
            if (clothPaint3d) {
                static int lockPart = -1, lockFace = -1;
                auto aabbToFace = [](int fi) {
                    const int map[6] = { 1, 0, 3, 2, 5, 4 };
                    return (fi >= 0 && fi < 6) ? map[fi] : 4;
                };
                auto faceOf = [&](const pm::Part& p, const Vec3& world) {
                    Vec3 h = pm::partLocalOffset(p, world);
                    Vec3 mn{ -p.half.x, -p.half.y, -p.half.z };
                    Vec3 mxb{ p.half.x, p.half.y, p.half.z };
                    float d[6] = {
                        std::fabs(h.x - mn.x), std::fabs(h.x - mxb.x),
                        std::fabs(h.y - mn.y), std::fabs(h.y - mxb.y),
                        std::fabs(h.z - mn.z), std::fabs(h.z - mxb.z)
                    };
                    int fi = 0;
                    for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
                    return aabbToFace(fi);
                };
                Vec3 ro, rd;
                if (inRight3d && !inLeft3d) rightRay(mx, my, ro, rd);
                else leftRay(mx, my, ro, rd);
                float best = 1e9f;
                int hit = -1;
                for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                    const pm::Part& p = ed.entityParts[i];
                    if (pm::isHairPart(p) || pm::isHairCardPart(p) || pm::isDecalPart(p)) continue;
                    if (entIsCloth && !pm::partHasBox(p)) continue;
                    if (ed.clothLocal) {
                        bool anySel = false;
                        for (int j = 0; j < (int)ed.entityParts.size(); j++)
                            if (j < (int)entMark.size() && entMark[j]) { anySel = true; break; }
                        if (anySel && (i >= (int)entMark.size() || !entMark[i])) continue;
                    }
                    Vec3 roL = pm::partLocalOffset(p, ro);
                    Vec3 rdL = pm::unrotateEuler(rd, p.rot);
                    float t;
                    if (!rayHitAABB(roL, rdL, { -p.half.x, -p.half.y, -p.half.z }, { p.half.x, p.half.y, p.half.z }, t) || t >= best)
                        continue;
                    best = t;
                    hit = i;
                }
                int face = -1;
                Vec3 hitW = ro + rd * best;
                if (hit >= 0) {
                    face = faceOf(ed.entityParts[hit], hitW);
                    if (lmb && !prevLmb) { lockPart = hit; lockFace = face; }
                }
                bool shaping = ed.tool == 5 || ed.tool == 6 || ed.tool == 7 || ed.tool == 8;
                if (shaping && ed.dragging && lockPart >= 0 && lockPart < (int)ed.entityParts.size()) {
                    hit = lockPart;
                    face = lockFace;
                }
                if (hit >= 0 && face >= 0 && pm::partHasBox(ed.entityParts[hit])) {
                    const pm::Part& p = ed.entityParts[hit];
                    int fx, fy, fw, fh;
                    pm::boxFacePx(p.boxX, p.boxY, p.boxW, p.boxH, p.boxD, face, fx, fy, fw, fh);
                    Vec3 q[4];
                    pm::cuboidFaceCorners(p, face, q);
                    Vec3 e1 = q[1] - q[0], e2 = q[3] - q[0];
                    Vec3 n = { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
                    float denom = n.x * rd.x + n.y * rd.y + n.z * rd.z;
                    Vec3 on = hitW;
                    if (std::fabs(denom) > 1e-8f) {
                        float t = (n.x * (q[0].x - ro.x) + n.y * (q[0].y - ro.y) + n.z * (q[0].z - ro.z)) / denom;
                        on = ro + rd * t;
                    }
                    Vec3 dlt = on - q[0];
                    float a = e1.x * e1.x + e1.y * e1.y + e1.z * e1.z;
                    float b = e2.x * e2.x + e2.y * e2.y + e2.z * e2.z;
                    float su = (a > 1e-12f) ? (e1.x * dlt.x + e1.y * dlt.y + e1.z * dlt.z) / a : 0.5f;
                    float sv = (b > 1e-12f) ? (e2.x * dlt.x + e2.y * dlt.y + e2.z * dlt.z) / b : 0.5f;
                    float u0, v0, u1, v1;
                    pm::modelBoxUV(p, face, entSheetW, entSheetH, u0, v0, u1, v1);
                    float u = u0 + su * (u1 - u0);
                    float v = v1 + sv * (v0 - v1);
                    ed.hoverX = (int)std::floor(u * (float)entSheetW);
                    ed.hoverY = (int)std::floor(v * (float)entSheetH);
                    gPxClip = { true, fx, fy, fx + fw, fy + fh };
                    clothClip = gPxClip;
                    faceStampW = paintCellPx(std::sqrt(a), fw, ed.paintDiv);
                    faceStampH = paintCellPx(std::sqrt(b), fh, ed.paintDiv);
                } else if (hit >= 0 && face >= 0 && paintImg && paintImg->ok()) {
                    const pm::Part& p = ed.entityParts[hit];
                    Vec3 q[4];
                    pm::cuboidFaceCorners(p, face, q);
                    Vec3 e1 = q[1] - q[0], e2 = q[3] - q[0];
                    Vec3 n = { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
                    float denom = n.x * rd.x + n.y * rd.y + n.z * rd.z;
                    Vec3 on = hitW;
                    if (std::fabs(denom) > 1e-8f) {
                        float t = (n.x * (q[0].x - ro.x) + n.y * (q[0].y - ro.y) + n.z * (q[0].z - ro.z)) / denom;
                        on = ro + rd * t;
                    }
                    Vec3 dlt = on - q[0];
                    float a = e1.x * e1.x + e1.y * e1.y + e1.z * e1.z;
                    float b = e2.x * e2.x + e2.y * e2.y + e2.z * e2.z;
                    float su = (a > 1e-12f) ? (e1.x * dlt.x + e1.y * dlt.y + e1.z * dlt.z) / a : 0.5f;
                    float sv = (b > 1e-12f) ? (e2.x * dlt.x + e2.y * dlt.y + e2.z * dlt.z) / b : 0.5f;
                    if (su < 0) su = 0;
                    if (su > 1) su = 1;
                    if (sv < 0) sv = 0;
                    if (sv > 1) sv = 1;
                    float u0, v0, u1, v1;
                    pm::skinFaceUV(p, face, u0, v0, u1, v1);
                    float u = u0 + su * (u1 - u0);
                    float v = v1 + sv * (v0 - v1);
                    int sw = paintImg->w, sh = paintImg->h;
                    ed.hoverX = (int)std::floor(u * (float)sw);
                    ed.hoverY = (int)std::floor(v * (float)sh);
                    int x0 = (int)std::floor(std::min(u0, u1) * (float)sw);
                    int y0 = (int)std::floor(std::min(v0, v1) * (float)sh);
                    int x1 = (int)std::ceil(std::max(u0, u1) * (float)sw);
                    int y1 = (int)std::ceil(std::max(v0, v1) * (float)sh);
                    if (x1 <= x0) x1 = x0 + 1;
                    if (y1 <= y0) y1 = y0 + 1;
                    gPxClip = { true, x0, y0, x1, y1 };
                    clothClip = gPxClip;
                    faceStampW = paintCellPx(std::sqrt(a), x1 - x0, ed.paintDiv);
                    faceStampH = paintCellPx(std::sqrt(b), y1 - y0, ed.paintDiv);
                }
                if (!lmb && !ed.dragging) { lockPart = -1; lockFace = -1; }
            } else if (ed.dragging && clothClip.on) {
                gPxClip = clothClip;
            }
            int stampW = ed.brushSize, stampH = ed.brushSize;
            if (clothPaint3d && gPxClip.on) {
                stampW = faceStampW;
                stampH = faceStampH;
            } else if ((ed.entSkinView) && paintKind() < 0) {
                int cell = (int)std::lround(1.0f / (float)paintDenom(ed.paintDiv));
                if (cell < 1) cell = 1;
                stampW = stampH = cell;
            }
            auto forStamp = [&](auto&& fn) {
                if (gPxClip.on) {
                    int ox = gPxClip.x0 + ((ed.hoverX - gPxClip.x0) / stampW) * stampW;
                    int oy = gPxClip.y0 + ((ed.hoverY - gPxClip.y0) / stampH) * stampH;
                    for (int dy = 0; dy < stampH; ++dy)
                        for (int dx = 0; dx < stampW; ++dx)
                            fn(ox + dx, oy + dy);
                } else {
                    int hx0 = ed.hoverX - stampW / 2, hy0 = ed.hoverY - stampH / 2;
                    for (int dy = 0; dy < stampH; ++dy)
                        for (int dx = 0; dx < stampW; ++dx)
                            fn(hx0 + dx, hy0 + dy);
                }
            };
            if (paintOn && (inLeft3d || clothPaint3d) && !overEntUi && paintImg && paintImg->ok() && ed.hoverX >= 0) {
                const int TW = paintTw, TH = paintTh;
                mat::Image& img = *paintImg;
                int eff = ed.picker ? 3 : ed.tool;
                if (eff == 3) {
                    if (lmb || (rmb && !prevRmb)) {
                        auto samplePx = [&](int px, int py, Color4& out) -> bool {
                            if (px < 0 || px >= TW || py < 0 || py >= TH) return false;
                            size_t i = ((size_t)py * TW + px) * 4;
                            out = { img.rgba[i + 0], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3] };
                            return true;
                        };
                        Color4 center{};
                        samplePx(ed.hoverX, ed.hoverY, center);
                        ed.r = center.r / 255.0f; ed.g = center.g / 255.0f;
                        ed.b = center.b / 255.0f; ed.a = center.a / 255.0f;
                        if (ed.tool == 1) {
                            forStamp([&](int px, int py) {
                                Color4 c;
                                if (!samplePx(px, py, c)) return;
                                addBrushColor(c);
                            });
                        } else if (stampW > 1 || stampH > 1) {
                            long sr = 0, sg = 0, sb = 0, sa = 0; int cnt = 0;
                            forStamp([&](int px, int py) {
                                Color4 c;
                                if (!samplePx(px, py, c)) return;
                                if (!ed.pickAlpha0 && c.a == 0) return;
                                sr += c.r; sg += c.g; sb += c.b; sa += c.a; cnt++;
                            });
                            if (cnt > 0) {
                                ed.r = (float)(sr / cnt) / 255.0f; ed.g = (float)(sg / cnt) / 255.0f;
                                ed.b = (float)(sb / cnt) / 255.0f; ed.a = (float)(sa / cnt) / 255.0f;
                            }
                        }
                        pushRecent((uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                        ed.picker = false;
                        ed.justPicked = true;
                    }
                } else if (!ed.justPicked && eff == 2) {
                    if (lmb && !prevLmb) {
                        pushSkinUndo();
                        if (ed.brushMod) {
                            std::vector<Color4> valid;
                            for (size_t k = 0; k < ed.brushColors.size(); k++)
                                if (k < ed.brushSet.size() && ed.brushSet[k]) valid.push_back(ed.brushColors[k]);
                            if (valid.empty()) floodFill(img, ed.hoverX, ed.hoverY, (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                            else floodFillRandom(img, ed.hoverX, ed.hoverY, valid);
                        } else {
                            floodFill(img, ed.hoverX, ed.hoverY, (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                        }
                        pushRecent((uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                        syncSkinGL();
                        ed.dirty = true;
                    }
                } else if (!ed.justPicked && (eff == 5 || eff == 6 || eff == 7 || eff == 8)) {
                    if (lmb && !prevLmb) {
                        ed.dragging = true;
                        ed.dragX0 = ed.hoverX; ed.dragY0 = ed.hoverY;
                        ed.dragX1 = ed.hoverX; ed.dragY1 = ed.hoverY;
                        pushSkinUndo();
                    }
                    if (ed.dragging) { ed.dragX1 = ed.hoverX; ed.dragY1 = ed.hoverY; }
                } else if (!ed.justPicked && (eff == 0 || eff == 1 || eff == 4) && lmb) {
                    if (lmb && !prevLmb) pushSkinUndo();
                    forStamp([&](int pxx, int pyy) {
                        if (pxx < 0 || pxx >= TW || pyy < 0 || pyy >= TH || !pxInClip(pxx, pyy)) return;
                        size_t i = ((size_t)pyy * TW + pxx) * 4;
                        if (eff == 4) {
                            img.rgba[i + 0] = 0; img.rgba[i + 1] = 0; img.rgba[i + 2] = 0; img.rgba[i + 3] = 0;
                        } else if (eff == 1) {
                            std::vector<int> valid;
                            for (size_t k = 0; k < ed.brushColors.size(); k++)
                                if (k < ed.brushSet.size() && ed.brushSet[k]) valid.push_back((int)k);
                            Color4 c;
                            if (valid.empty()) c = { (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                            else c = ed.brushColors[valid[rand() % valid.size()]];
                            img.rgba[i + 0] = c.r; img.rgba[i + 1] = c.g; img.rgba[i + 2] = c.b; img.rgba[i + 3] = c.a;
                        } else {
                            img.rgba[i + 0] = (uint8_t)(ed.r * 255); img.rgba[i + 1] = (uint8_t)(ed.g * 255);
                            img.rgba[i + 2] = (uint8_t)(ed.b * 255); img.rgba[i + 3] = (uint8_t)(ed.a * 255);
                        }
                    });
                    if (eff == 0) pushRecent((uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                    syncSkinGL();
                    ed.dirty = true;
                }
            }
            if (ed.justPicked && !lmb && !rmb) ed.justPicked = false;
            if (ed.dragging && !lmb && paintImg && paintImg->ok()) {
                Color4 c = { (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                int x0 = ed.dragX0, y0 = ed.dragY0, x1 = ed.dragX1, y1 = ed.dragY1;
                switch (ed.tool) {
                    case 5: fillRectImg(*paintImg, x0, y0, x1, y1, c); break;
                    case 6:
                        drawLineImg(*paintImg, x0, y0, x1, y0, c, 1);
                        drawLineImg(*paintImg, x1, y0, x1, y1, c, 1);
                        drawLineImg(*paintImg, x1, y1, x0, y1, c, 1);
                        drawLineImg(*paintImg, x0, y1, x0, y0, c, 1);
                        break;
                    case 7: drawLineImg(*paintImg, x0, y0, x1, y1, c, std::max(stampW, stampH)); break;
                    case 8: {
                        int r = (int)std::sqrt((float)((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)));
                        drawCircleImg(*paintImg, x0, y0, r, c);
                        break;
                    }
                }
                ed.dragging = false;
                gPxClip.on = false;
                clothClip.on = false;
                syncSkinGL();
                ed.dirty = true;
            }
            if (!lmb && gzDrag != 0) {
                if (!ed.entityParts.empty()) {
                    if (ed.modelTool == 3) {
                        int axis = gizmoAxisOf(gzDrag);
                        float s = gizmoIsHead(gzDrag) ? gizmoHeadSign(gzDrag) : 1.0f;
                        rotateEnt45(axis, s);
                    } else if (ed.modelTool == 4) {
                        mirrorEnt(gizmoAxisOf(gzDrag));
                    } else if (ed.modelTool == 5) {
                        flipEntUV(gzDrag == 61 ? 1 : 0);
                    } else if (!gzDidDrag && gizmoIsHead(gzDrag)) {
                        int axis = gizmoAxisOf(gzDrag);
                        float s = gizmoHeadSign(gzDrag);
                        if (gizmoIsRot(gzDrag)) {
                            pushEntUndo();
                            rotateEntBy(axis, s * 1.5707963f);
                        } else if (ed.modelTool == 2) {
                            pushEntUndo();
                            fillEnt(axis, s * 0.05f, true, s);
                        } else if (ed.modelTool == 1) {
                            pushEntUndo();
                            stretchEnt(axis, s * 0.05f, true, s);
                        } else {
                            pushEntUndo();
                            translateEnt(kAxis[axis].x * s * 0.05f, kAxis[axis].y * s * 0.05f, kAxis[axis].z * s * 0.05f);
                        }
                    }
                }
                gzDrag = 0;
            }

            static bool prevQ = false, prevE = false, prevN = false, prevDel = false, prevA = false, prevC = false;
            if (keyDown('Q') && !prevQ && !ed.entityParts.empty()) {
                int n = (int)ed.entityParts.size();
                markEnt((ed.entitySel + n - 1) % n, keyDown(VK_SHIFT) || keyDown(VK_CONTROL));
            }
            if (keyDown('E') && !prevE && !ed.hairPaint && !ed.hairErase && !ed.hairSelect && !ed.hairCard && !ed.entityParts.empty()) {
                int n = (int)ed.entityParts.size();
                markEnt((ed.entitySel + 1) % n, keyDown(VK_SHIFT) || keyDown(VK_CONTROL));
            }
            if (keyDown('N') && !prevN) { if (entIsCloth) setPartAdd(); else addEntPart(); }
            if (keyDown(VK_DELETE) && !prevDel) {
                if (ed.partPlane) { ed.planeOn = false; ed.partAnchor = false; }
                else if (ed.partMeasure) { ed.measureOn = false; ed.partAnchor = false; }
                else delEnt();
            }
            if (keyDown('A') && !prevA && !inRight3d) {
                addBrushColor({ (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) });
            }
            if (keyDown('C') && !prevC) {
                ed.brushColors.assign(16, Color4{});
                ed.brushSet.assign(16, 0);
                ed.brushSel.assign(16, 0);
            }
            prevQ = keyDown('Q'); prevE = keyDown('E');
            prevN = keyDown('N'); prevDel = keyDown(VK_DELETE);
            prevA = keyDown('A'); prevC = keyDown('C');

            float step = keyDown(VK_SHIFT) ? 0.1f : 0.02f;
            static bool prevMoved = false;
            bool wantMove = !geomSelected().empty() &&
                (keyDown(VK_LEFT) || keyDown(VK_RIGHT) || keyDown(VK_UP) || keyDown(VK_DOWN) ||
                 keyDown(VK_NEXT) || keyDown(VK_PRIOR));
            if (wantMove && !prevMoved) pushEntUndo();
            if (wantMove && ed.modelTool != 0) {
                float dx = 0, dy = 0, dz = 0;
                if (keyDown(VK_LEFT)) dx -= step;
                if (keyDown(VK_RIGHT)) dx += step;
                if (keyDown(VK_UP)) dy += step;
                if (keyDown(VK_DOWN)) dy -= step;
                if (keyDown(VK_NEXT)) dz += step;
                if (keyDown(VK_PRIOR)) dz -= step;
                if (ed.modelTool == 2) {
                    int axis = (std::fabs(dx) >= std::fabs(dy) && std::fabs(dx) >= std::fabs(dz)) ? 0
                             : (std::fabs(dy) >= std::fabs(dz) ? 1 : 2);
                    float dt = (axis == 0) ? dx : (axis == 1 ? dy : dz);
                    fillEnt(axis, dt, true, dt >= 0.0f ? 1.0f : -1.0f);
                } else if (ed.modelTool == 1) {
                    int axis = (std::fabs(dx) >= std::fabs(dy) && std::fabs(dx) >= std::fabs(dz)) ? 0
                             : (std::fabs(dy) >= std::fabs(dz) ? 1 : 2);
                    float dt = (axis == 0) ? dx : (axis == 1 ? dy : dz);
                    stretchEnt(axis, dt, true, dt >= 0.0f ? 1.0f : -1.0f);
                } else {
                    translateEnt(dx, dy, dz);
                }
            }
            prevMoved = wantMove;
            static bool prevPlus = false, prevMinus = false;
            bool plus = keyDown(VK_ADD) || keyDown(VK_OEM_PLUS);
            bool minus = keyDown(VK_SUBTRACT) || keyDown(VK_OEM_MINUS);
            if ((plus && !prevPlus) || (minus && !prevMinus)) {
                auto idx = geomSelected();
                if (!idx.empty()) {
                    pushEntUndo();
                    float s = plus ? 1.05f : (1.0f / 1.05f);
                    for (int i : idx) {
                        ed.entityParts[i].half.x = std::max(0.01f, ed.entityParts[i].half.x * s);
                        ed.entityParts[i].half.y = std::max(0.01f, ed.entityParts[i].half.y * s);
                        ed.entityParts[i].half.z = std::max(0.01f, ed.entityParts[i].half.z * s);
                    }
                    if (!selectedDecalsOnModel()) undoEnt();
                    else ed.dirty = true;
                }
            }
            prevPlus = plus; prevMinus = minus;

            if ((rmb && !prevRmb) || (mmb && !prevMmb)) {
                lastMx = mx; lastMy = my;
            }
            bool blockOrbit = overEntUi || ed.gizmoHover != 0 || gzDrag != 0 || slideDrag || paintSlideDrag || ed.hairSelDrag;
            bool altOrbit = keyDown(VK_MENU);
            bool doOrbit = mmb || (rmb && !(ed.hairSelect && inRight3d && !altOrbit));
            if (doOrbit && !blockOrbit) {
                if (inRight3d) {
                    ed.erotY += (mx - lastMx) * 0.008f;
                    ed.erotX += (my - lastMy) * 0.008f;
                    if (ed.erotX > 1.55f) ed.erotX = 1.55f;
                    if (ed.erotX < -1.55f) ed.erotX = -1.55f;
                    lastMx = mx; lastMy = my;
                } else if (inLeft3d && !ed.entSkinView) {
                    ed.rotY += (mx - lastMx) * 0.008f;
                    ed.rotX += (my - lastMy) * 0.008f;
                    if (ed.rotX > 1.55f) ed.rotX = 1.55f;
                    if (ed.rotX < -1.55f) ed.rotX = -1.55f;
                    lastMx = mx; lastMy = my;
                } else if (inLeft3d && (ed.entSkinView || paintKind() >= 0) && ed.skinViewZoom > 1.001f) {
                    ed.skinPanX += mx - lastMx;
                    ed.skinPanY += my - lastMy;
                    lastMx = mx; lastMy = my;
                }
            }
        } else if (ed.modelMode) {
            // ---- block model editing ----
            const float partG = mat::kSolidGrid;
            int paintStep = ed.paintDiv;
            if (paintStep < 0) paintStep = 0;
            if (paintStep > 3) paintStep = 3;
            const float paintCell = mat::kSolidGrid / (float)paintDenom(paintStep);
            auto partDom = [](const Vec3& n) {
                int ax = 0;
                if (std::fabs(n.y) > std::fabs(n.x) && std::fabs(n.y) > std::fabs(n.z)) ax = 1;
                else if (std::fabs(n.z) > std::fabs(n.x) && std::fabs(n.z) > std::fabs(n.y)) ax = 2;
                return ax;
            };
            auto partNormAt = [](int fi) {
                Vec3 n{};
                if (fi == 0) n = { -1, 0, 0 };
                else if (fi == 1) n = { 1, 0, 0 };
                else if (fi == 2) n = { 0, -1, 0 };
                else if (fi == 3) n = { 0, 1, 0 };
                else if (fi == 4) n = { 0, 0, -1 };
                else n = { 0, 0, 1 };
                return n;
            };
            auto pickItemSolid = [&](const Vec3& ro, const Vec3& rd, float& tHit, Vec3& nrm) -> int {
                tHit = 1e9f;
                int si = -1;
                nrm = { 0, 1, 0 };
                for (int i = 0; i < (int)editModel.solids.size(); i++) {
                    const mat::Solid& s = editModel.solids[i];
                    Vec3 c = viewOfModel(s.c);
                    Vec3 h{ s.h[0], s.h[1], s.h[2] };
                    Vec3 roL = mat::solidUnEuler(ro - c, s.rot);
                    Vec3 rdL = mat::solidUnEuler(rd, s.rot);
                    float t;
                    if (!rayHitAABB(roL, rdL, h * -1.0f, h, t) || t >= tHit) continue;
                    tHit = t;
                    si = i;
                    Vec3 hp = roL + rdL * t;
                    float d[6] = {
                        std::fabs(hp.x + h.x), std::fabs(hp.x - h.x),
                        std::fabs(hp.y + h.y), std::fabs(hp.y - h.y),
                        std::fabs(hp.z + h.z), std::fabs(hp.z - h.z)
                    };
                    int fi = 0;
                    for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
                    nrm = mat::solidEuler(partNormAt(fi), s.rot);
                }
                return si;
            };
            auto pickItemSurface = [&](const Vec3& ro, const Vec3& rd, Vec3& model, Vec3& nrm, int& solid) -> bool {
                float best = 1e9f;
                bool hit = false;
                solid = -1;
                nrm = { 0, 1, 0 };
                float tS;
                Vec3 nS;
                int si = pickItemSolid(ro, rd, tS, nS);
                if (si >= 0) { best = tS; hit = true; nrm = nS; solid = si; }
                for (int i = 0; i < (int)editModel.quads.size(); i++) {
                    Vec3 p[4];
                    for (int c = 0; c < 4; c++) p[c] = viewOfModel(editModel.quads[i].p[c]);
                    float t;
                    auto take = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
                        if (!rayHitTri(ro, rd, a, b, c, t) || t >= best) return;
                        best = t;
                        hit = true;
                        solid = -1;
                        Vec3 n = (b - a).cross(c - a);
                        if (n.lengthSq() > 1e-12f) nrm = n.normalized();
                    };
                    take(p[0], p[1], p[2]);
                    take(p[0], p[2], p[3]);
                }
                if (!hit && editModel.quads.empty()) {
                    float t;
                    Vec3 b0{ -0.5f, 0.0f, -0.5f }, b1{ 0.5f, 1.0f, 0.5f };
                    if (rayHitAABB(ro, rd, b0, b1, t)) {
                        best = t;
                        hit = true;
                        Vec3 hp = ro + rd * t;
                        float d[6] = {
                            std::fabs(hp.x - b0.x), std::fabs(hp.x - b1.x),
                            std::fabs(hp.y - b0.y), std::fabs(hp.y - b1.y),
                            std::fabs(hp.z - b0.z), std::fabs(hp.z - b1.z)
                        };
                        int fi = 0;
                        for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
                        nrm = partNormAt(fi);
                    }
                }
                if (!hit && std::fabs(rd.y) > 1e-6f) {
                    float t = (0.0f - ro.y) / rd.y;
                    if (t > 0.001f) {
                        Vec3 hp = ro + rd * t;
                        if (hp.x >= -1.5f && hp.x <= 1.5f && hp.z >= -1.5f && hp.z <= 1.5f) {
                            best = t;
                            hit = true;
                            nrm = { 0, 1, 0 };
                            solid = -1;
                        }
                    }
                }
                if (!hit) return false;
                Vec3 hv = ro + rd * best;
                model = { hv.x + 0.5f, hv.y, hv.z + 0.5f };
                return true;
            };
            auto cellOnFace = [&](const Vec3& model, const Vec3& nrm, bool outward, float cell, float mn[3], float mx[3]) {
                int ax = partDom(nrm);
                float sign = (ax == 0 ? nrm.x : ax == 1 ? nrm.y : nrm.z);
                if (sign == 0.0f) sign = 1.0f;
                float dir = sign >= 0.0f ? 1.0f : -1.0f;
                if (!outward) dir = -dir;
                float p[3] = { model.x, model.y, model.z };
                float plane = p[ax];
                for (int k = 0; k < 3; k++) {
                    if (k == ax) {
                        float lift = 0.0015f;
                        if (dir > 0.0f) { mn[k] = plane + lift; mx[k] = plane + lift + cell; }
                        else { mx[k] = plane - lift; mn[k] = plane - lift - cell; }
                    } else {
                        float s = std::floor(p[k] / cell + 1e-4f) * cell;
                        mn[k] = s;
                        mx[k] = s + cell;
                    }
                }
            };
            auto solidHolds = [&](float x, float y, float z) {
                const float e = 1e-3f;
                for (const mat::Solid& s : editModel.solids) {
                    if (x < s.c[0] - s.h[0] - e || x > s.c[0] + s.h[0] + e) continue;
                    if (y < s.c[1] - s.h[1] - e || y > s.c[1] + s.h[1] + e) continue;
                    if (z < s.c[2] - s.h[2] - e || z > s.c[2] + s.h[2] + e) continue;
                    return true;
                }
                return false;
            };
            auto addSolidBox = [&](const float mn[3], const float mx[3], bool undo, int kind, int face) {
                float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
                if (dx < 1e-4f || dy < 1e-4f || dz < 1e-4f) return;
                float cx = (mn[0] + mx[0]) * 0.5f, cy = (mn[1] + mx[1]) * 0.5f, cz = (mn[2] + mx[2]) * 0.5f;
                if (solidHolds(cx, cy, cz)) return;
                if (undo) pushModelUndo();
                mat::Solid s;
                s.c[0] = cx; s.c[1] = cy; s.c[2] = cz;
                s.h[0] = dx * 0.5f; s.h[1] = dy * 0.5f; s.h[2] = dz * 0.5f;
                s.rgb[0] = ed.r; s.rgb[1] = ed.g; s.rgb[2] = ed.b;
                s.kind = kind;
                s.face = face;
                editModel.solids.push_back(s);
                selectOnlySolid((int)editModel.solids.size() - 1);
                ed.dirty = true;
            };
            auto eraseSolids = [&](const std::vector<int>& ids) {
                if (ids.empty()) return;
                pushModelUndo();
                std::vector<uint8_t> kill(editModel.solids.size(), 0);
                for (int i : ids) if (i >= 0 && i < (int)kill.size()) kill[i] = 1;
                std::vector<mat::Solid> kept;
                for (int i = 0; i < (int)editModel.solids.size(); i++)
                    if (!kill[i]) kept.push_back(editModel.solids[i]);
                editModel.solids.swap(kept);
                solidMark.clear();
                ed.selSolid = -1;
                if (ed.planePart >= (int)editModel.solids.size()) { ed.planeOn = false; ed.planePart = -1; }
                ed.dirty = true;
            };
            auto mergeItemSolids = [&]() {
                auto ids = solidIds();
                if (ids.size() < 2) return;
                pushModelUndo();
                float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
                mat::Solid keep = editModel.solids[ids[0]];
                for (int i : ids) {
                    const mat::Solid& s = editModel.solids[i];
                    for (int k = 0; k < 3; k++) {
                        if (s.c[k] - s.h[k] < mn[k]) mn[k] = s.c[k] - s.h[k];
                        if (s.c[k] + s.h[k] > mx[k]) mx[k] = s.c[k] + s.h[k];
                    }
                }
                std::vector<uint8_t> kill(editModel.solids.size(), 0);
                for (int i : ids) kill[i] = 1;
                std::vector<mat::Solid> kept;
                for (int i = 0; i < (int)editModel.solids.size(); i++)
                    if (!kill[i]) kept.push_back(editModel.solids[i]);
                mat::Solid m = keep;
                m.kind = 0;
                m.rot[0] = m.rot[1] = m.rot[2] = 0.0f;
                for (int k = 0; k < 3; k++) {
                    m.c[k] = 0.5f * (mn[k] + mx[k]);
                    m.h[k] = 0.5f * (mx[k] - mn[k]);
                    if (m.h[k] < 1e-4f) m.h[k] = 1e-4f;
                }
                kept.push_back(m);
                editModel.solids.swap(kept);
                selectOnlySolid((int)editModel.solids.size() - 1);
                ed.dirty = true;
            };
            auto splitItemSolids = [&]() {
                auto ids = solidIds();
                if (ids.empty()) return;
                pushModelUndo();
                std::vector<mat::Solid> next;
                std::vector<uint8_t> kill(editModel.solids.size(), 0);
                for (int i : ids) if (i >= 0 && i < (int)kill.size()) kill[i] = 1;
                bool any = false;
                for (int i = 0; i < (int)editModel.solids.size(); i++) {
                    if (!kill[i]) { next.push_back(editModel.solids[i]); continue; }
                    const mat::Solid src = editModel.solids[i];
                    if (mat::solidRotated(src)) { next.push_back(src); continue; }
                    int nx = std::max(1, (int)std::lround((src.h[0] * 2.0f) / partG));
                    int ny = std::max(1, (int)std::lround((src.h[1] * 2.0f) / partG));
                    int nz = std::max(1, (int)std::lround((src.h[2] * 2.0f) / partG));
                    if (nx * ny * nz <= 1) { next.push_back(src); continue; }
                    any = true;
                    if (nx * ny * nz > 64) {
                        int ax = 0;
                        if (src.h[1] > src.h[ax]) ax = 1;
                        if (src.h[2] > src.h[ax]) ax = 2;
                        mat::Solid a = src, b = src;
                        a.h[ax] *= 0.5f; b.h[ax] *= 0.5f;
                        a.c[ax] -= a.h[ax]; b.c[ax] += b.h[ax];
                        next.push_back(a); next.push_back(b);
                    } else {
                        float x0 = src.c[0] - src.h[0], y0 = src.c[1] - src.h[1], z0 = src.c[2] - src.h[2];
                        for (int iz = 0; iz < nz; iz++)
                            for (int iy = 0; iy < ny; iy++)
                                for (int ix = 0; ix < nx; ix++) {
                                    mat::Solid c = src;
                                    c.h[0] = partG * 0.5f; c.h[1] = partG * 0.5f; c.h[2] = partG * 0.5f;
                                    c.c[0] = x0 + (ix + 0.5f) * partG;
                                    c.c[1] = y0 + (iy + 0.5f) * partG;
                                    c.c[2] = z0 + (iz + 0.5f) * partG;
                                    next.push_back(c);
                                }
                    }
                }
                if (!any) { modelUndo.pop_back(); return; }
                int firstNew = 0;
                for (int i = 0; i < (int)editModel.solids.size(); i++) {
                    if (kill[i]) break;
                    firstNew++;
                }
                editModel.solids.swap(next);
                solidMark.assign(editModel.solids.size(), 0);
                ed.selSolid = firstNew;
                if (ed.selSolid >= 0 && ed.selSolid < (int)solidMark.size()) solidMark[ed.selSolid] = 1;
                ed.dirty = true;
            };
            auto dupItemSolids = [&]() {
                auto ids = solidIds();
                if (ids.empty()) return;
                pushModelUndo();
                int first = (int)editModel.solids.size();
                for (int i : ids) {
                    mat::Solid s = editModel.solids[i];
                    s.bind = -1;
                    s.c[0] += partG;
                    editModel.solids.push_back(s);
                }
                solidMark.assign(editModel.solids.size(), 0);
                for (int i = first; i < (int)editModel.solids.size(); i++) solidMark[i] = 1;
                ed.selSolid = first;
                ed.dirty = true;
            };
            auto texItemSolids = [&]() {
                auto ids = solidIds();
                if (ids.empty()) return;
                pushModelUndo();
                bool clear = keyDown(VK_SHIFT);
                for (int i : ids) {
                    if (clear) editModel.solids[i].tex.clear();
                    else editModel.solids[i].tex = ed.selMat;
                }
                ed.dirty = true;
            };
            // item selection list (left panel, 3D icon grid like the tile panel)
            ed.modelHover = -1;
            {
                const float pX = 8.0f, pY = 52.0f, thumb = 36.0f, gap = 6.0f;
                const int cols = 4;
                for (int b = 1; b < liveBlockCount(); b++) {
                    int i = b - 1;
                    float cx = pX + (i % cols) * (thumb + gap);
                    float cy = pY + (i / cols) * (thumb + gap);
                    if (mx >= cx && mx < cx + thumb && my >= cy && my < cy + thumb) ed.modelHover = b;
                }
            }
            if (lmb && !prevLmb && ed.modelHover >= 1 && ed.modelHover < liveBlockCount()) loadBlockModel(ed.modelHover);

            static bool prevApplyR = false;
            bool applyKey = keyDown('R');
            if ((lmb && !prevLmb && applyRandHov) || (applyKey && !prevApplyR)) applyRand();
            if (lmb && !prevLmb && rightTabHover >= 0) ed.modelRightTab = rightTabHover;
            prevApplyR = applyKey;
            if (lmb && !prevLmb && saveHov) saveRandFile();
            if (lmb && !prevLmb && cancelHov) cancelChanges();
            if (lmb && !prevLmb && faceHov) addFace();
            if (lmb && !prevLmb && blkHov) addBlock();
            if (lmb && !prevLmb && smHov) addSmall();
            if (lmb && !prevLmb && tinyHov) setModelTool(6);
            if (lmb && !prevLmb && midSaveHov) saveRandFile();
            if (lmb && !prevLmb && midUndoHov) undoModelOrPaint();
            if (lmb && !prevLmb && partToolHover >= 0) {
                if (partToolHover == 0) setPartAdd();
                else if (partToolHover == 1) setHairSelect();
                else if (partToolHover == 2) setHairBrush(false);
                else if (partToolHover == 3) setHairCard();
                else if (partToolHover == 4) setHairBrush(true);
                else if (partToolHover == 5) mergeItemSolids();
                else if (partToolHover == 6) splitItemSolids();
                else if (partToolHover == 7) texItemSolids();
                else if (partToolHover == 8) dupItemSolids();
                else if (partToolHover == 9) eraseSolids(solidIds());
                else if (partToolHover == 10) setPartCut();
                else if (partToolHover == 11) setPartMeasure();
                else if (partToolHover == 12) setPartPlane();
                else if (partToolHover >= 20 && partToolHover < 28) {
                    int ci = partToolHover - 20;
                    ed.r = kPaintPal[ci][0];
                    ed.g = kPaintPal[ci][1];
                    ed.b = kPaintPal[ci][2];
                    ed.a = 1.0f;
                    auto ss = solidIds();
                    if (!ss.empty()) {
                        pushModelUndo();
                        for (int i : ss) {
                            editModel.solids[i].rgb[0] = ed.r;
                            editModel.solids[i].rgb[1] = ed.g;
                            editModel.solids[i].rgb[2] = ed.b;
                            editModel.solids[i].tex.clear();
                        }
                        ed.dirty = true;
                    }
                }
            }
            if (lmb && !prevLmb && ed.modelToolHover >= 0) {
                int i = ed.modelToolHover;
                if (i == 0) bindSel();
                else if (i == 1) unbindSel();
                else if (i == 2) setModelTool(0);
                else if (i == 3) snapSel();
                else if (i == 4) alignSel();
                else if (i == 5) setModelTool(3);
                else if (i == 6) setModelTool(1);
                else if (i == 7) setModelTool(2);
                else if (i == 8) setModelTool(4);
                else if (i == 9) setModelTool(5);
                else if (i == 10) setModelTool(7);
            }
            static bool prevCtrlZ = false;
            bool ctrlZ = keyDown(VK_CONTROL) && keyDown('Z');
            if (ctrlZ && !prevCtrlZ) undoModelOrPaint();
            prevCtrlZ = ctrlZ;
            if (ed.modelRightTab == 1) {
                auto overR = [&](const float* r) {
                    return r[2] > 1.0f && mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3];
                };
                if (lmb && !prevLmb && colorToolHover >= 0) {
                    int i = colorToolHover;
                    if (i == 0) { ed.tool = (ed.tool == 0) ? -1 : 0; ed.picker = false; ed.brushMod = false; }
                    else if (i == 1) {
                        if (ed.tool == 2) { ed.brushMod = !ed.brushMod; if (ed.brushMod) ed.picker = false; }
                        else { ed.tool = (ed.tool == 1) ? -1 : 1; ed.picker = false; ed.brushMod = false; }
                    }
                    else if (i == 2) { ed.tool = (ed.tool == 2) ? -1 : 2; ed.picker = false; ed.brushMod = false; }
                    else if (i == 3) { ed.picker = !ed.picker; if (ed.picker) ed.brushMod = false; }
                    else if (i == 4) { ed.tool = (ed.tool == 4) ? -1 : 4; ed.picker = false; ed.brushMod = false; }
                    else if (i == 5) { ed.tool = (ed.tool == 5) ? -1 : 5; ed.picker = false; ed.brushMod = false; }
                    else if (i == 6) { ed.tool = (ed.tool == 6) ? -1 : 6; ed.picker = false; ed.brushMod = false; }
                    else if (i == 7) { ed.tool = (ed.tool == 7) ? -1 : 7; ed.picker = false; ed.brushMod = false; }
                    else if (i == 8) { ed.tool = (ed.tool == 8) ? -1 : 8; ed.picker = false; ed.brushMod = false; }
                }
                if (lmb && !prevLmb && colorRecentHover >= 0 && colorRecentHover < (int)ed.recent.size()) {
                    const Color4& c = ed.recent[colorRecentHover];
                    ed.r = c.r / 255.0f; ed.g = c.g / 255.0f; ed.b = c.b / 255.0f; ed.a = c.a / 255.0f;
                }
                if (lmb && overR(colorSv)) {
                    float s = (mx - colorSv[0]) / colorSv[2];
                    float v = 1.0f - (my - colorSv[1]) / colorSv[3];
                    if (s < 0) s = 0;
                    if (s > 1) s = 1;
                    if (v < 0) v = 0;
                    if (v > 1) v = 1;
                    float hh, ss, vv; rgbToHsv(ed.r, ed.g, ed.b, hh, ss, vv);
                    hsvToRgb(hh, s, v, ed.r, ed.g, ed.b);
                }
                float hdx = mx - colorHueCX, hdy = my - colorHueCY;
                float hdist = std::sqrt(hdx * hdx + hdy * hdy);
                bool onHue = hdist >= colorHueRI - 6.0f && hdist <= colorHueRO + 6.0f;
                if (lmb && onHue) {
                    float h = std::atan2(hdy, hdx) / 6.2831853f;
                    if (h < 0) h += 1.0f;
                    float hh, ss, vv; rgbToHsv(ed.r, ed.g, ed.b, hh, ss, vv);
                    hsvToRgb(h, ss, vv, ed.r, ed.g, ed.b);
                }
                if (lmb && overR(colorAlpha)) {
                    float t = (mx - colorAlpha[0]) / colorAlpha[2];
                    if (t < 0) t = 0;
                    if (t > 1) t = 1;
                    ed.a = t;
                }
                bool overWidgets = colorToolHover >= 0 || colorRecentHover >= 0 || colorSlideHov ||
                    overR(colorSv) || overR(colorAlpha) || onHue;
                colorFaceVis = false;
                gPxClip.on = false;
                static int colorStampW = 1, colorStampH = 1;
                int hitSi = -1, hitFace = -1;
                if (inRight3d && !overWidgets && rightTabHover < 0 && !editModel.solids.empty()) {
                    const int kBoxFace[6] = { 1, 0, 3, 2, 5, 4 };
                    Vec3 ro, rd;
                    colorRay(mx, my, ro, rd);
                    float best = 1e9f;
                    for (int i = 0; i < (int)editModel.solids.size(); i++) {
                        const mat::Solid& s = editModel.solids[i];
                        Vec3 c = viewOfModel(s.c);
                        Vec3 h{ s.h[0], s.h[1], s.h[2] };
                        Vec3 roL = mat::solidUnEuler(ro - c, s.rot);
                        Vec3 rdL = mat::solidUnEuler(rd, s.rot);
                        float t;
                        if (!rayHitAABB(roL, rdL, h * -1.0f, h, t) || t >= best) continue;
                        best = t;
                        hitSi = i;
                        Vec3 hp = roL + rdL * t;
                        float d[6] = {
                            std::fabs(hp.x + h.x), std::fabs(hp.x - h.x),
                            std::fabs(hp.y + h.y), std::fabs(hp.y - h.y),
                            std::fabs(hp.z + h.z), std::fabs(hp.z - h.z)
                        };
                        int fi = 0;
                        for (int k = 1; k < 6; k++) if (d[k] < d[fi]) fi = k;
                        hitFace = kBoxFace[fi];
                    }
                    if (hitSi >= 0 && hitFace >= 0) {
                        mat::Solid& s = editModel.solids[hitSi];
                        if (lmb && !ed.picker) claimSolidSheet(s);
                        auto faceLocal = [&](int face, Vec3 q[4]) {
                                float x0 = -s.h[0], x1 = s.h[0], y0 = -s.h[1], y1 = s.h[1], z0 = -s.h[2], z1 = s.h[2];
                                switch (face) {
                                    case 0: q[0] = {x1,y0,z1}; q[1] = {x1,y0,z0}; q[2] = {x1,y1,z0}; q[3] = {x1,y1,z1}; break;
                                    case 1: q[0] = {x0,y0,z0}; q[1] = {x0,y0,z1}; q[2] = {x0,y1,z1}; q[3] = {x0,y1,z0}; break;
                                    case 2: q[0] = {x0,y1,z0}; q[1] = {x1,y1,z0}; q[2] = {x1,y1,z1}; q[3] = {x0,y1,z1}; break;
                                    case 3: q[0] = {x0,y0,z1}; q[1] = {x1,y0,z1}; q[2] = {x1,y0,z0}; q[3] = {x0,y0,z0}; break;
                                    case 4: q[0] = {x0,y0,z1}; q[1] = {x1,y0,z1}; q[2] = {x1,y1,z1}; q[3] = {x0,y1,z1}; break;
                                    default:q[0] = {x1,y0,z0}; q[1] = {x0,y0,z0}; q[2] = {x0,y1,z0}; q[3] = {x1,y1,z0}; break;
                                }
                            };
                            Vec3 ql[4];
                            faceLocal(hitFace, ql);
                            Vec3 q[4];
                            for (int c = 0; c < 4; c++) {
                                Vec3 w = mat::solidEuler(ql[c], s.rot);
                                float mp[3] = { s.c[0] + w.x, s.c[1] + w.y, s.c[2] + w.z };
                                q[c] = viewOfModel(mp);
                            }
                            for (int c = 0; c < 4; c++) colorFaceQ[c] = q[c];
                            colorFaceVis = true;
                            if (mat::solidHasBox(s) && itemSheet.ok()) {
                            Vec3 e1 = q[1] - q[0], e2 = q[3] - q[0];
                            Vec3 nrm = { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
                            float denom = nrm.x * rd.x + nrm.y * rd.y + nrm.z * rd.z;
                            Vec3 on = ro + rd * best;
                            if (std::fabs(denom) > 1e-8f) {
                                float t = (nrm.x * (q[0].x - ro.x) + nrm.y * (q[0].y - ro.y) + nrm.z * (q[0].z - ro.z)) / denom;
                                on = ro + rd * t;
                            }
                            Vec3 dlt = on - q[0];
                            float a = e1.dot(e1), b = e2.dot(e2);
                            float su = (a > 1e-12f) ? e1.dot(dlt) / a : 0.5f;
                            float sv = (b > 1e-12f) ? e2.dot(dlt) / b : 0.5f;
                            if (su < 0) su = 0;
                            if (su > 1) su = 1;
                            if (sv < 0) sv = 0;
                            if (sv > 1) sv = 1;
                            int fx, fy, fw, fh;
                            pm::boxFacePx(s.boxX, s.boxY, s.boxW, s.boxH, s.boxD, hitFace, fx, fy, fw, fh);
                            float u0 = (float)fx / (float)itemSheet.w;
                            float v0 = (float)fy / (float)itemSheet.h;
                            float u1 = (float)(fx + fw) / (float)itemSheet.w;
                            float v1 = (float)(fy + fh) / (float)itemSheet.h;
                            float u = u0 + su * (u1 - u0);
                            float v = v1 + sv * (v0 - v1);
                            colorHx = (int)std::floor(u * (float)itemSheet.w);
                            colorHy = (int)std::floor(v * (float)itemSheet.h);
                            gPxClip = { true, fx, fy, fx + fw, fy + fh };
                            colorStampW = paintCellPx(std::sqrt(a), fw, ed.paintDiv);
                            colorStampH = paintCellPx(std::sqrt(b), fh, ed.paintDiv);
                            }
                    }
                }
                static bool paintStroke = false;
                static bool colorShape = false;
                static PxClip colorClip;
                if (!lmb) paintStroke = false;
                if (lmb && colorHx >= 0 && hitSi >= 0 && itemSheet.ok() && rightTabHover < 0 && !overWidgets) {
                    auto pushRecent = [&](uint8_t rr, uint8_t gg, uint8_t bb, uint8_t aa) {
                        for (size_t k = 0; k < ed.recent.size(); k++) {
                            if (ed.recent[k].r == rr && ed.recent[k].g == gg && ed.recent[k].b == bb && ed.recent[k].a == aa) {
                                ed.recent.erase(ed.recent.begin() + (int)k);
                                break;
                            }
                        }
                        ed.recent.push_back({ rr, gg, bb, aa });
                        if (ed.recent.size() > 8) ed.recent.erase(ed.recent.begin());
                    };
                    auto beginStroke = [&]() {
                        if (paintStroke) return;
                        itemSheetUndo.push_back(itemSheet.rgba);
                        if (itemSheetUndo.size() > 32) itemSheetUndo.erase(itemSheetUndo.begin());
                        itemSheetDirty = true;
                        paintStroke = true;
                    };
                    int iw = itemSheet.w, ih = itemSheet.h;
                    auto writePx = [&](int px, int py, uint8_t rr, uint8_t gg, uint8_t bb, uint8_t aa) {
                        if (!pxInClip(px, py)) return;
                        if (px < 0 || py < 0 || px >= iw || py >= ih) return;
                        size_t i = ((size_t)py * iw + px) * 4;
                        itemSheet.rgba[i + 0] = rr; itemSheet.rgba[i + 1] = gg;
                        itemSheet.rgba[i + 2] = bb; itemSheet.rgba[i + 3] = aa;
                    };
                    int tool = ed.picker ? 3 : ed.tool;
                    if (tool < 0 || tool > 8) tool = 0;
                    int cellW = colorStampW < 1 ? 1 : colorStampW;
                    int cellH = colorStampH < 1 ? 1 : colorStampH;
                    auto forCell = [&](auto&& fn) {
                        int ox = gPxClip.on ? gPxClip.x0 + ((colorHx - gPxClip.x0) / cellW) * cellW : colorHx;
                        int oy = gPxClip.on ? gPxClip.y0 + ((colorHy - gPxClip.y0) / cellH) * cellH : colorHy;
                        for (int dy = 0; dy < cellH; dy++)
                            for (int dx = 0; dx < cellW; dx++)
                                fn(ox + dx, oy + dy);
                    };
                    auto randColor = [&]() -> Color4 {
                        std::vector<int> valid;
                        for (size_t k = 0; k < ed.brushColors.size(); k++)
                            if (k < ed.brushSet.size() && ed.brushSet[k]) valid.push_back((int)k);
                        if (valid.empty())
                            return { (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                        return ed.brushColors[valid[rand() % (int)valid.size()]];
                    };
                    if (tool == 3) {
                        if (!prevLmb && pxInClip(colorHx, colorHy)) {
                            size_t i = ((size_t)colorHy * iw + colorHx) * 4;
                            ed.r = itemSheet.rgba[i + 0] / 255.0f; ed.g = itemSheet.rgba[i + 1] / 255.0f;
                            ed.b = itemSheet.rgba[i + 2] / 255.0f; ed.a = itemSheet.rgba[i + 3] / 255.0f;
                            if (ed.tool == 1) {
                                forCell([&](int px, int py) {
                                    if (!pxInClip(px, py) || px < 0 || py < 0 || px >= iw || py >= ih) return;
                                    size_t pi = ((size_t)py * iw + px) * 4;
                                    addBrushColor({ itemSheet.rgba[pi], itemSheet.rgba[pi + 1], itemSheet.rgba[pi + 2], itemSheet.rgba[pi + 3] });
                                });
                            }
                            ed.picker = false;
                        }
                    } else if (tool == 2) {
                        if (!prevLmb) {
                            beginStroke();
                            if (ed.brushMod) {
                                std::vector<Color4> valid;
                                for (size_t k = 0; k < ed.brushColors.size(); k++)
                                    if (k < ed.brushSet.size() && ed.brushSet[k]) valid.push_back(ed.brushColors[k]);
                                if (valid.empty())
                                    floodFill(itemSheet, colorHx, colorHy,
                                              (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255),
                                              (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                                else floodFillRandom(itemSheet, colorHx, colorHy, valid);
                            } else {
                                floodFill(itemSheet, colorHx, colorHy,
                                          (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255),
                                          (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                            }
                            pushRecent((uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                            uploadItemSheet();
                            ed.dirty = true;
                        }
                    } else if (tool >= 5 && tool <= 8) {
                        if (!prevLmb) {
                            beginStroke();
                            colorShape = true;
                            colorClip = gPxClip;
                            ed.dragX0 = ed.dragX1 = colorHx;
                            ed.dragY0 = ed.dragY1 = colorHy;
                        } else if (colorShape) {
                            ed.dragX1 = colorHx;
                            ed.dragY1 = colorHy;
                        }
                    } else if (tool == 0 || tool == 1 || tool == 4) {
                        if (!prevLmb || paintStroke) {
                            beginStroke();
                            forCell([&](int px, int py) {
                                Color4 c = (tool == 4) ? Color4{ 0, 0, 0, 0 } : (tool == 1) ? randColor()
                                    : Color4{ (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                                writePx(px, py, c.r, c.g, c.b, c.a);
                            });
                            if (tool == 0) pushRecent((uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                            uploadItemSheet();
                            ed.dirty = true;
                        }
                    }
                }
                if (colorShape && !lmb) {
                    if ((ed.tool >= 5 && ed.tool <= 8) && itemSheet.ok()) {
                        PxClip saved = gPxClip;
                        gPxClip = colorClip;
                        Color4 c = { (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                        int x0 = ed.dragX0, y0 = ed.dragY0, x1 = ed.dragX1, y1 = ed.dragY1;
                        int thick = std::max(colorStampW, colorStampH);
                        if (thick < 1) thick = 1;
                        switch (ed.tool) {
                            case 5: fillRectImg(itemSheet, x0, y0, x1, y1, c); break;
                            case 6:
                                drawLineImg(itemSheet, x0, y0, x1, y0, c, 1);
                                drawLineImg(itemSheet, x1, y0, x1, y1, c, 1);
                                drawLineImg(itemSheet, x1, y1, x0, y1, c, 1);
                                drawLineImg(itemSheet, x0, y1, x0, y0, c, 1);
                                break;
                            case 7: drawLineImg(itemSheet, x0, y0, x1, y1, c, thick); break;
                            case 8: {
                                int r = (int)std::sqrt((float)((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)));
                                drawCircleImg(itemSheet, x0, y0, r, c);
                                break;
                            }
                        }
                        uploadItemSheet();
                        ed.dirty = true;
                        gPxClip = saved;
                    }
                    colorShape = false;
                }
            }
            if (lmb && !prevLmb && ed.matHover >= 0) {
                if (ed.matHover == nMatItems - 1) addExtraMat();
                else if (ed.matHover < TEX_COUNT) assignSelTex(mat::tileName(ed.matHover));
                else {
                    int ei = ed.matHover - TEX_COUNT;
                    if (ei >= 0 && ei < (int)extraMats.size()) assignSelTex(extraMats[ei].name);
                }
            }

            static int gzDrag = 0;
            static float gzPressMx = 0, gzPressMy = 0;
            static bool gzDidDrag = false, gzUndoPushed = false;
            static Vec3 gzGrabC;
            static float gzAppliedT = 0, gzAppliedAng = 0;
            static float dfGrabP[4][3] = {};
            static Vec3 dfGrabHit, dfOut, dfFaceN, dfPlaneP;
            static std::vector<int> ssIdx;
            static std::vector<float> ssP;
            static float ssMn[3], ssMx[3];
            auto applyDeformUV = [&](mat::Quad& q) {
                if (ed.modelTool == 2) applyFillBySize(q);
            };
            auto gizmoIsDeform = [](int id) { return id >= 40 && id <= 53; };
            auto solidStretchOn = [&]() {
                return ed.modelTool == 1 && isSolidGroup(ed.selQuad);
            };
            static std::vector<int> partGrabI;
            static std::vector<float> partGrabH, partGrabC;
            static float partGrabT0 = 0.0f;
            auto partStretchOn = [&]() {
                return ed.modelTool == 1 && selectedIndices().empty() && !solidIds().empty();
            };
            auto captureSolidGrab = [&]() {
                ssIdx = selectedIndices();
                ssP.clear();
                ssP.reserve(ssIdx.size() * 12);
                for (int i : ssIdx) {
                    const mat::Quad& q = editModel.quads[i];
                    for (int c = 0; c < 4; c++)
                        for (int k = 0; k < 3; k++) ssP.push_back(q.p[c][k]);
                }
                boundsOfIdx(ssIdx, ssMn, ssMx);
            };
            auto stretchSolidFromGrab = [&](int axis, float dt, bool oneSided, float sideSign) {
                if (axis < 0 || axis > 2 || ssIdx.size() * 12 != ssP.size()) return;
                for (size_t i = 0; i < ssIdx.size(); i++) {
                    mat::Quad& q = editModel.quads[ssIdx[i]];
                    for (int c = 0; c < 4; c++)
                        for (int k = 0; k < 3; k++)
                            q.p[c][k] = ssP[i * 12 + c * 3 + k];
                }
                float nmin = ssMn[axis], nmax = ssMx[axis];
                if (oneSided) {
                    if (sideSign >= 0.0f) nmax += dt;
                    else nmin += dt;
                } else {
                    nmin -= dt * 0.5f;
                    nmax += dt * 0.5f;
                }
                if (nmax - nmin < 0.03125f) {
                    float mid = 0.5f * (nmin + nmax);
                    nmin = mid - 0.015625f;
                    nmax = mid + 0.015625f;
                }
                float oldS = ssMx[axis] - ssMn[axis];
                if (oldS < 1e-8f) return;
                float newS = nmax - nmin;
                for (int i : ssIdx) {
                    mat::Quad& q = editModel.quads[i];
                    for (int c = 0; c < 4; c++) {
                        float t = (q.p[c][axis] - ssMn[axis]) / oldS;
                        q.p[c][axis] = nmin + t * newS;
                    }
                    refreshUV(q);
                }
                ed.dirty = true;
            };
            auto clickOnlyTool = [&]() { return ed.modelTool == 3 || ed.modelTool == 4 || ed.modelTool == 5; };
            if (lmb && gzDrag != 0) {
                float dist = std::hypot(mx - gzPressMx, my - gzPressMy);
                if (dist > 5.0f) gzDidDrag = true;
                if (gzDidDrag && !gzUndoPushed && !clickOnlyTool()) { pushModelUndo(); gzUndoPushed = true; }
                if (gzDidDrag && !clickOnlyTool()) {
                    Vec3 ro, rd;
                    leftRay(mx, my, ro, rd);
                    if (gizmoIsDeform(gzDrag) && ed.selQuad >= 0 && ed.selQuad < (int)editModel.quads.size()) {
                        Vec3 hit;
                        if (rayPlaneHit(ro, rd, dfPlaneP, dfFaceN, hit)) {
                            Vec3 delta = hit - dfGrabHit;
                            mat::Quad& q = editModel.quads[ed.selQuad];
                            if (gzDrag >= 40 && gzDrag <= 43) {
                                int c = gzDrag - 40;
                                float s = delta.dot(dfOut);
                                Vec3 d = dfOut * s;
                                q.p[c][0] = dfGrabP[c][0] + d.x;
                                q.p[c][1] = dfGrabP[c][1] + d.y;
                                q.p[c][2] = dfGrabP[c][2] + d.z;
                            } else {
                                int e = gzDrag - 50;
                                float s = delta.dot(dfOut);
                                Vec3 d = dfOut * s;
                                int c0 = e, c1 = (e + 1) % 4;
                                q.p[c0][0] = dfGrabP[c0][0] + d.x;
                                q.p[c0][1] = dfGrabP[c0][1] + d.y;
                                q.p[c0][2] = dfGrabP[c0][2] + d.z;
                                q.p[c1][0] = dfGrabP[c1][0] + d.x;
                                q.p[c1][1] = dfGrabP[c1][1] + d.y;
                                q.p[c1][2] = dfGrabP[c1][2] + d.z;
                            }
                            applyDeformUV(q);
                            ed.dirty = true;
                        }
                    } else {
                        int axis = gizmoAxisOf(gzDrag);
                        if (gizmoIsRot(gzDrag)) {
                            float ang = rotAngleAt(axis, gzGrabC, ro, rd);
                            float d = ang - gzAppliedAng;
                            while (d > 3.14159265f) d -= 6.2831853f;
                            while (d < -3.14159265f) d += 6.2831853f;
                            if (std::fabs(d) > 1e-5f) rotateSelBy(axis, d);
                            gzAppliedAng = ang;
                        } else if (solidStretchOn()) {
                            float t = closestAxisT(gzGrabC, kAxis[axis], ro, rd);
                            float dt = t - gzAppliedT;
                            bool oneSided = gizmoIsHead(gzDrag);
                            float sign = oneSided ? gizmoHeadSign(gzDrag) : 1.0f;
                            stretchSolidFromGrab(axis, dt, oneSided, sign);
                        } else if (partStretchOn() && partGrabI.size() * 3 == partGrabH.size()) {
                            float t = closestAxisT(gzGrabC, kAxis[axis], ro, rd);
                            float dt = t - partGrabT0;
                            bool oneSided = gizmoIsHead(gzDrag);
                            float sign = oneSided ? gizmoHeadSign(gzDrag) : 1.0f;
                            for (size_t n = 0; n < partGrabI.size(); n++) {
                                int i = partGrabI[n];
                                if (i < 0 || i >= (int)editModel.solids.size()) continue;
                                mat::Solid& s = editModel.solids[i];
                                for (int k = 0; k < 3; k++) {
                                    s.h[k] = partGrabH[n * 3 + k];
                                    s.c[k] = partGrabC[n * 3 + k];
                                }
                                float grow = oneSided ? sign * dt : dt;
                                s.h[axis] = std::max(mat::kSolidGrid * 0.5f, partGrabH[n * 3 + axis] + grow * 0.5f);
                                if (oneSided)
                                    s.c[axis] = partGrabC[n * 3 + axis] + sign * (s.h[axis] - partGrabH[n * 3 + axis]);
                            }
                            ed.dirty = true;
                        } else {
                            float t = closestAxisT(gzGrabC, kAxis[axis], ro, rd);
                            float dt = t - gzAppliedT;
                            if (std::fabs(dt) > 1e-5f)
                                translateSel(kAxis[axis].x * dt, kAxis[axis].y * dt, kAxis[axis].z * dt);
                            gzAppliedT = t;
                        }
                    }
                }
            }
            bool partBrush = ed.partAdd || ed.hairPaint || ed.hairErase || ed.partCut || ed.hairCard || ed.partMeasure || ed.partPlane;
            static bool partStroke = false;
            static bool partCutAny = false;
            static bool havePaintCell = false;
            static int lastPaintCell[3] = {};
            if (!lmb) { partStroke = false; partCutAny = false; havePaintCell = false; }
            static bool itemPaintSlideDrag = false;
            if (lmb && !prevLmb && paintSlideHov) itemPaintSlideDrag = true;
            if (!lmb) itemPaintSlideDrag = false;
            if (itemPaintSlideDrag && paintSlideW > 1.0f) {
                float u = (mx - paintSlideX) / paintSlideW;
                int step = (int)std::lround(u * 3.0f);
                if (step < 0) step = 0;
                if (step > 3) step = 3;
                ed.paintDiv = step;
            }
            static bool colorSlideDrag = false;
            if (lmb && !prevLmb && colorSlideHov) colorSlideDrag = true;
            if (!lmb) colorSlideDrag = false;
            if (colorSlideDrag && colorSlideW > 1.0f) {
                float u = (mx - colorSlideX) / colorSlideW;
                int step = (int)std::lround(u * 3.0f);
                if (step < 0) step = 0;
                if (step > 3) step = 3;
                ed.paintDiv = step;
            }
            auto faceOfNorm = [&](const Vec3& n) {
                int ax = partDom(n);
                float sign = ax == 0 ? n.x : ax == 1 ? n.y : n.z;
                return ax * 2 + (sign >= 0.0f ? 1 : 0);
            };
            if (!(partBrush && inLeft3d && partToolHover < 0)) ed.partAddVis = false;
            if (partBrush && inLeft3d && partToolHover < 0 && ed.modelToolHover < 0 && ed.matHover < 0 &&
                !paintSlideHov && !faceHov && !blkHov && !smHov && !tinyHov && !midSaveHov && !midUndoHov) {
                Vec3 ro, rd, model, nrm;
                leftRay(mx, my, ro, rd);
                int solid = -1;
                if (pickItemSurface(ro, rd, model, nrm, solid)) {
                    bool planeHit = !ed.planeOn || ed.planePart < 0 || ed.partPlane ||
                        (solid == ed.planePart && faceOfNorm(nrm) == ed.planeFace);
                    if ((ed.partAdd || ed.partMeasure) && planeHit) {
                        auto snap = [&](float v) { return std::round(v / partG) * partG; };
                        float p[3] = { snap(model.x), snap(model.y), snap(model.z) };
                        if (ed.planeOn && ed.planePart >= 0 && ed.planePart < (int)editModel.solids.size()) {
                            const mat::Solid& ps = editModel.solids[ed.planePart];
                            int ax = ed.planeFace / 2;
                            float plane = (ed.planeFace % 2 == 0) ? (ps.c[ax] - ps.h[ax]) : (ps.c[ax] + ps.h[ax]);
                            p[ax] = snap(plane);
                        }
                        ed.partAddVis = true;
                        if (!ed.partAnchor) {
                            for (int k = 0; k < 3; k++) { ed.partMn[k] = p[k]; ed.partMx[k] = p[k] + partG; }
                        } else {
                            for (int k = 0; k < 3; k++) {
                                ed.partMn[k] = std::min(ed.partA[k], p[k]);
                                ed.partMx[k] = std::max(ed.partA[k], p[k]);
                                if (ed.partMx[k] - ed.partMn[k] < partG * 0.5f)
                                    ed.partMx[k] = ed.partMn[k] + partG;
                            }
                        }
                        if (lmb && !prevLmb) {
                            if (!ed.partAnchor) {
                                ed.partAnchor = true;
                                ed.partA[0] = p[0]; ed.partA[1] = p[1]; ed.partA[2] = p[2];
                            } else if (ed.partMeasure) {
                                ed.measureOn = true;
                                for (int k = 0; k < 3; k++) {
                                    ed.measureA[k] = ed.partA[k];
                                    ed.measureB[k] = p[k];
                                }
                                ed.partAnchor = false;
                            } else {
                                addSolidBox(ed.partMn, ed.partMx, true, 0, faceOfNorm(nrm));
                                ed.partAnchor = false;
                            }
                        }
                    } else if ((ed.hairPaint || ed.hairCard) && planeHit) {
                        float mn[3], mx[3];
                        cellOnFace(model, nrm, true, paintCell, mn, mx);
                        for (int k = 0; k < 3; k++) { ed.partMn[k] = mn[k]; ed.partMx[k] = mx[k]; }
                        ed.partAddVis = true;
                        if (lmb) {
                            int cell[3];
                            for (int k = 0; k < 3; k++)
                                cell[k] = (int)std::floor(((mn[k] + mx[k]) * 0.5f) / paintCell + 1e-4f);
                            bool place = true;
                            if (havePaintCell) {
                                int d[3] = { cell[0] - lastPaintCell[0], cell[1] - lastPaintCell[1], cell[2] - lastPaintCell[2] };
                                if (d[0] == 0 && d[1] == 0 && d[2] == 0) place = false;
                                else {
                                    int ax = partDom(nrm);
                                    int along = d[ax];
                                    float nv = ax == 0 ? nrm.x : ax == 1 ? nrm.y : nrm.z;
                                    int sgn = nv >= 0.0f ? 1 : -1;
                                    int t1 = d[(ax + 1) % 3], t2 = d[(ax + 2) % 3];
                                    if (along * sgn > 0 && t1 == 0 && t2 == 0) place = false;
                                }
                            }
                            if (place) {
                                if (!partStroke) { pushModelUndo(); partStroke = true; }
                                addSolidBox(mn, mx, false, ed.hairCard ? 1 : 0, faceOfNorm(nrm));
                                lastPaintCell[0] = cell[0]; lastPaintCell[1] = cell[1]; lastPaintCell[2] = cell[2];
                                havePaintCell = true;
                            }
                        }
                    } else if (ed.partPlane && solid >= 0 && lmb && !prevLmb) {
                        int face = faceOfNorm(nrm);
                        if (ed.planeOn && ed.planePart == solid && ed.planeFace == face) ed.planeOn = false;
                        else { ed.planeOn = true; ed.planePart = solid; ed.planeFace = face; }
                    } else if ((ed.hairErase || ed.partCut) && solid >= 0 && !mat::solidRotated(editModel.solids[solid])) {
                        const mat::Solid& s = editModel.solids[solid];
                        int ax = partDom(nrm);
                        float sign = ax == 0 ? nrm.x : ax == 1 ? nrm.y : nrm.z;
                        float smin = s.c[ax] - s.h[ax], smax = s.c[ax] + s.h[ax];
                        float q[3] = { model.x, model.y, model.z };
                        float mn[3], mx[3];
                        for (int k = 0; k < 3; k++) {
                            if (k == ax) {
                                if (sign >= 0.0f) { mx[k] = smax; mn[k] = std::max(smin, smax - partG); }
                                else { mn[k] = smin; mx[k] = std::min(smax, smin + partG); }
                            } else {
                                float o = std::floor(q[k] / partG + 1e-4f) * partG;
                                mn[k] = std::max(s.c[k] - s.h[k], o);
                                mx[k] = std::min(s.c[k] + s.h[k], o + partG);
                            }
                        }
                        for (int k = 0; k < 3; k++) { ed.partMn[k] = mn[k]; ed.partMx[k] = mx[k]; }
                        ed.partAddVis = (mx[0] > mn[0] && mx[1] > mn[1] && mx[2] > mn[2]);
                        bool click = ed.partCut ? (lmb && !prevLmb) : lmb;
                        if (click && ed.partAddVis) {
                            if (!partStroke) { pushModelUndo(); partStroke = true; partCutAny = false; }
                            if (!mat::subtractSolid(editModel.solids, solid, mn, mx)) {
                                if (!partCutAny && !modelUndo.empty()) { modelUndo.pop_back(); partStroke = false; }
                            } else {
                                partCutAny = true;
                                solidMark.clear();
                                ed.selSolid = editModel.solids.empty() ? -1 : (int)editModel.solids.size() - 1;
                                ed.dirty = true;
                            }
                        }
                    }
                }
            }
            if (ed.hairSelect && inLeft3d && partToolHover < 0 && !faceHov && !blkHov && !smHov && !tinyHov) {
                if (lmb && !prevLmb) {
                    ed.hairSelDrag = true;
                    ed.hairSelX0 = ed.hairSelX1 = mx;
                    ed.hairSelY0 = ed.hairSelY1 = my;
                }
            }
            if (ed.hairSelDrag && ed.modelMode) {
                if (lmb) {
                    ed.hairSelX1 = mx;
                    ed.hairSelY1 = my;
                } else {
                    float x0 = std::min(ed.hairSelX0, ed.hairSelX1), x1 = std::max(ed.hairSelX0, ed.hairSelX1);
                    float y0 = std::min(ed.hairSelY0, ed.hairSelY1), y1 = std::max(ed.hairSelY0, ed.hairSelY1);
                    bool add = keyDown(VK_SHIFT) || keyDown(VK_CONTROL);
                    if (!add) {
                        ensureSolidMark();
                        std::fill(solidMark.begin(), solidMark.end(), 0);
                        ed.selSolid = -1;
                    }
                    ensureSolidMark();
                    for (int i = 0; i < (int)editModel.solids.size(); i++) {
                        const mat::Solid& s = editModel.solids[i];
                        float sx, sy;
                        if (!projectView(s.c[0] - 0.5f, s.c[1], s.c[2] - 0.5f, sx, sy)) continue;
                        if (sx < x0 || sx > x1 || sy < y0 || sy > y1) continue;
                        solidMark[i] = 1;
                        ed.selSolid = i;
                    }
                    if (ed.selSolid >= 0) {
                        ensureSelMark();
                        std::fill(selMark.begin(), selMark.end(), 0);
                    }
                    ed.hairSelDrag = false;
                }
            }
            if (lmb && !prevLmb && inLeft3d && ed.modelToolHover < 0 && ed.matHover < 0 && partToolHover < 0 &&
                !partBrush && !ed.hairSelect &&
                !faceHov && !blkHov && !smHov && !tinyHov && !midSaveHov && !midUndoHov) {
                bool addSel = keyDown(VK_SHIFT) || keyDown(VK_CONTROL);
                if (ed.modelTool == 6) {
                    Vec3 ro, rd;
                    leftRay(mx, my, ro, rd);
                    float ox, oy, oz, texS;
                    if (pickTinyOrigin(ro, rd, ox, oy, oz, texS)) addTinyAt(ox, oy, oz, texS);
                } else if (!addSel && ed.gizmoHover != 0 &&
                    (!editModel.quads.empty() || (ed.selSolid >= 0 && ed.selSolid < (int)editModel.solids.size())) &&
                    (ed.modelTool == 0 || clickOnlyTool() ||
                     ((ed.modelTool == 1 || ed.modelTool == 2) && gizmoIsDeform(ed.gizmoHover)) ||
                     ((solidStretchOn() || partStretchOn()) && !gizmoIsDeform(ed.gizmoHover)))) {
                    gzDrag = ed.gizmoHover;
                    gzPressMx = mx; gzPressMy = my;
                    gzDidDrag = false; gzUndoPushed = false;
                    if (clickOnlyTool()) {
                        // click-only: 45 / Mirror / UV — no drag setup
                    } else if (gizmoIsDeform(gzDrag) && ed.selQuad >= 0 && ed.selQuad < (int)editModel.quads.size()) {
                        mat::Quad& q = editModel.quads[ed.selQuad];
                        for (int c = 0; c < 4; c++)
                            for (int k = 0; k < 3; k++) dfGrabP[c][k] = q.p[c][k];
                        Vec3 corner[4], mid[4], cOut[4], eOut[4], n;
                        deformGeom(ed.selQuad, corner, mid, cOut, eOut, n);
                        dfFaceN = n;
                        dfPlaneP = viewOfModel(q.p[0]);
                        if (gzDrag >= 40 && gzDrag <= 43) {
                            dfOut = cOut[gzDrag - 40];
                            ed.selCorner = gzDrag - 40;
                        } else {
                            dfOut = eOut[gzDrag - 50];
                        }
                        Vec3 ro, rd;
                        leftRay(mx, my, ro, rd);
                        if (!rayPlaneHit(ro, rd, dfPlaneP, dfFaceN, dfGrabHit))
                            dfGrabHit = dfPlaneP;
                    } else {
                        gizmoCenterView(gzGrabC);
                        Vec3 ro, rd;
                        leftRay(mx, my, ro, rd);
                        int axis = gizmoAxisOf(gzDrag);
                        if (gizmoIsRot(gzDrag)) gzAppliedAng = rotAngleAt(axis, gzGrabC, ro, rd);
                        else gzAppliedT = closestAxisT(gzGrabC, kAxis[axis], ro, rd);
                        if (solidStretchOn()) captureSolidGrab();
                        if (partStretchOn()) {
                            partGrabI = solidIds();
                            partGrabH.clear();
                            partGrabC.clear();
                            for (int i : partGrabI) {
                                for (int k = 0; k < 3; k++) {
                                    partGrabH.push_back(editModel.solids[i].h[k]);
                                    partGrabC.push_back(editModel.solids[i].c[k]);
                                }
                            }
                            partGrabT0 = gzAppliedT;
                        }
                    }
                } else {
                    Vec3 ro, rd;
                    leftRay(mx, my, ro, rd);
                    float tSolid;
                    Vec3 nSolid;
                    int si = pickItemSolid(ro, rd, tSolid, nSolid);
                    float best = 1e9f;
                    int hit = -1;
                    for (int i = 0; i < (int)editModel.quads.size(); i++) {
                        Vec3 p[4];
                        for (int c = 0; c < 4; c++) p[c] = viewOfModel(editModel.quads[i].p[c]);
                        float t;
                        if (rayHitTri(ro, rd, p[0], p[1], p[2], t) && t < best) { best = t; hit = i; }
                        if (rayHitTri(ro, rd, p[0], p[2], p[3], t) && t < best) { best = t; hit = i; }
                    }
                    if (si >= 0 && (hit < 0 || tSolid <= best)) {
                        if (addSel) {
                            ensureSolidMark();
                            if (si < (int)solidMark.size()) solidMark[si] = 1;
                            ed.selSolid = si;
                            ensureSelMark();
                            std::fill(selMark.begin(), selMark.end(), 0);
                        } else selectOnlySolid(si);
                    } else if (hit >= 0) {
                        markObject(hit, addSel);
                        if (ed.modelTool == 2) snapshotFillBase();
                    }
                }
            }
            static bool tileDrag = false;
            static float tileDragMy = 0, tileDragS = 1;
            static bool tileUndoPushed = false;
            if (ed.modelTool == 7 && inLeft3d && ed.modelToolHover < 0 && ed.matHover < 0 &&
                !faceHov && !blkHov && !smHov && !tinyHov && !midSaveHov && !midUndoHov) {
                if (lmb && !prevLmb) {
                    tileDrag = true;
                    tileDragMy = my;
                    auto idx = selectedIndices();
                    if (!idx.empty()) tileDragS = mat::quadTexScale(editModel.quads[idx[0]]);
                    else {
                        auto ss = solidIds();
                        tileDragS = ss.empty() ? 1.0f : editModel.solids[ss[0]].texScale;
                        if (tileDragS < 1e-4f) tileDragS = 1.0f;
                    }
                    tileUndoPushed = false;
                }
                if (lmb && tileDrag) {
                    float dy = tileDragMy - my;
                    if (std::fabs(dy) > 8.0f) {
                        if (!tileUndoPushed) { pushModelUndo(); tileUndoPushed = true; }
                        applyTexScaleSel(tileDragS * std::exp(dy * 0.012f));
                    }
                }
            }
            if (!lmb) tileDrag = false;
            if (!lmb && gzDrag != 0) {
                if (!editModel.quads.empty() || (ed.selSolid >= 0 && ed.selSolid < (int)editModel.solids.size())) {
                    if (ed.modelTool == 3) {
                        int axis = gizmoAxisOf(gzDrag);
                        float s = gizmoIsHead(gzDrag) ? gizmoHeadSign(gzDrag) : 1.0f;
                        rotateSel(axis, s * 0.78539816f);
                    } else if (ed.modelTool == 4) {
                        mirrorSel(gizmoAxisOf(gzDrag));
                    } else if (ed.modelTool == 5) {
                        flipSelUV(gzDrag == 61 ? 1 : 0);
                    } else if (!gzDidDrag) {
                        if (gizmoIsDeform(gzDrag) && ed.selQuad >= 0 && ed.selQuad < (int)editModel.quads.size()) {
                            pushModelUndo();
                            float s = keyDown(VK_SHIFT) ? -0.05f : 0.05f;
                            Vec3 d = dfOut * s;
                            mat::Quad& q = editModel.quads[ed.selQuad];
                            if (gzDrag >= 40 && gzDrag <= 43) {
                                int c = gzDrag - 40;
                                q.p[c][0] += d.x; q.p[c][1] += d.y; q.p[c][2] += d.z;
                            } else {
                                int e = gzDrag - 50;
                                int c0 = e, c1 = (e + 1) % 4;
                                q.p[c0][0] += d.x; q.p[c0][1] += d.y; q.p[c0][2] += d.z;
                                q.p[c1][0] += d.x; q.p[c1][1] += d.y; q.p[c1][2] += d.z;
                            }
                            applyDeformUV(q);
                            ed.dirty = true;
                        } else if (gizmoIsHead(gzDrag)) {
                            int axis = gizmoAxisOf(gzDrag);
                            float s = gizmoHeadSign(gzDrag);
                            if (gizmoIsRot(gzDrag)) rotateSel(axis, s * 1.5707963f);
                            else if (solidStretchOn()) {
                                pushModelUndo();
                                stretchSolidSel(axis, s * 0.0625f, true, s);
                            } else {
                                pushModelUndo();
                                translateSel(kAxis[axis].x * s * 0.25f, kAxis[axis].y * s * 0.25f, kAxis[axis].z * s * 0.25f);
                            }
                        }
                    }
                }
                gzDrag = 0;
            }

            static int dragSlider = -1;
            if (showSliders) {
                if (lmb && !prevLmb && overSliders) {
                    for (int i = 0; i < kNRandSliders; i++) {
                        float lx, ly, tx, ty, tw, th, vx;
                        sliderTrack(i, lx, ly, tx, ty, tw, th, vx);
                        if (mx >= tx - 6 && mx < tx + tw + 6 && my >= ty - 6 && my < ty + th + 6)
                            dragSlider = i;
                    }
                }
                if (lmb && dragSlider >= 0 && dragSlider < kNRandSliders) {
                    const RandSlider& spec = kRandSliders[dragSlider];
                    float lx, ly, tx, ty, tw, th, vx;
                    sliderTrack(dragSlider, lx, ly, tx, ty, tw, th, vx);
                    float t = (tw > 1.0f) ? (mx - tx) / tw : 0.0f;
                    if (t < 0) t = 0;
                    if (t > 1) t = 1;
                    float v = spec.minv + t * (spec.maxv - spec.minv);
                    if (spec.integer) v = (float)(int)(v + 0.5f);
                    editRand.f[spec.key] = v;
                    ed.dirty = true;
                }
                if (!lmb) dragSlider = -1;
            } else {
                dragSlider = -1;
            }

            if (keyDown('Q') && !prevLbrack && !editModel.quads.empty()) {
                auto reps = objectReps();
                if (!reps.empty()) {
                    int cur = 0;
                    for (int r = 0; r < (int)reps.size(); r++) if (objectId(reps[r]) == objectId(ed.selQuad)) cur = r;
                    int nxt = (cur + (int)reps.size() - 1) % (int)reps.size();
                    markObject(reps[nxt], keyDown(VK_SHIFT) || keyDown(VK_CONTROL));
                    if (ed.modelTool == 2) snapshotFillBase();
                }
            }
            if (keyDown('E') && !prevRbrack && !editModel.quads.empty()) {
                auto reps = objectReps();
                if (!reps.empty()) {
                    int cur = 0;
                    for (int r = 0; r < (int)reps.size(); r++) if (objectId(reps[r]) == objectId(ed.selQuad)) cur = r;
                    int nxt = (cur + 1) % (int)reps.size();
                    markObject(reps[nxt], keyDown(VK_SHIFT) || keyDown(VK_CONTROL));
                    if (ed.modelTool == 2) snapshotFillBase();
                }
            }
            if (keyDown('1')) ed.selCorner = 0;
            if (keyDown('2')) ed.selCorner = 1;
            if (keyDown('3')) ed.selCorner = 2;
            if (keyDown('4')) ed.selCorner = 3;
            static bool prevN = false, prevDel = false, prevD = false;
            if (keyDown('N') && !prevN) addFace();
            if (keyDown(VK_DELETE) && !prevDel) {
                auto idx = selectedIndices();
                if (idx.empty()) eraseSolids(solidIds());
                else if (!idx.empty()) {
                    pushModelUndo();
                    std::vector<uint8_t> kill(editModel.quads.size(), 0);
                    for (int i : idx) kill[i] = 1;
                    mat::Model kept = editModel;
                    kept.quads.clear();
                    for (int i = 0; i < (int)editModel.quads.size(); i++)
                        if (!kill[i]) kept.quads.push_back(editModel.quads[i]);
                    editModel = std::move(kept);
                    selMark.clear();
                    if (ed.selQuad >= (int)editModel.quads.size()) ed.selQuad = (int)editModel.quads.size() - 1;
                    if (ed.selQuad < 0) ed.selQuad = 0;
                    ed.dirty = true;
                }
            }
            if (keyDown('D') && !prevD && !editModel.quads.empty()) {
                pushModelUndo();
                for (int i : selectedIndices())
                    editModel.quads[i].doubleSided = !editModel.quads[i].doubleSided;
                ed.dirty = true;
            }
            prevN = keyDown('N'); prevDel = keyDown(VK_DELETE); prevD = keyDown('D');

            float step = keyDown(VK_SHIFT) ? 0.1f : 0.02f;
            static bool prevMoved = false;
            bool wantMove = !editModel.quads.empty() &&
                (keyDown(VK_LEFT) || keyDown(VK_RIGHT) || keyDown(VK_UP) || keyDown(VK_DOWN) ||
                 keyDown(VK_NEXT) || keyDown(VK_PRIOR));
            if (wantMove && !prevMoved) pushModelUndo();
            if (wantMove && !editModel.quads.empty() && ed.modelTool != 0) {
                bool deform = (ed.modelTool == 1 || ed.modelTool == 2)
                    && isDeformableFace(ed.selQuad);
                float dx = 0, dy = 0, dz = 0;
                if (keyDown(VK_LEFT)) dx -= step;
                if (keyDown(VK_RIGHT)) dx += step;
                if (keyDown(VK_UP)) dy += step;
                if (keyDown(VK_DOWN)) dy -= step;
                if (keyDown(VK_NEXT)) dz += step;
                if (keyDown(VK_PRIOR)) dz -= step;
                if (deform) {
                    mat::Quad& q = editModel.quads[ed.selQuad];
                    q.p[ed.selCorner][0] += dx;
                    q.p[ed.selCorner][1] += dy;
                    q.p[ed.selCorner][2] += dz;
                    if (ed.modelTool == 2) applyFillBySize(q);
                    ed.dirty = true;
                } else {
                    translateSel(dx, dy, dz);
                }
            }
            prevMoved = wantMove;
            // free orbit: right-drag rotates the camera under the cursor (left = model, right = rand)
            static int orbitPane = 0; // 0 none, 1 left model, 2 right rand
            if (rmb && !prevRmb) {
                lastMx = mx; lastMy = my;
                if (applyRandHov || saveHov || cancelHov || overExit || overBlockPanel || overSliders || overRightHdr ||
                    overMidHdr || overMatBar || paintSlideHov || colorSlideHov || faceHov || blkHov || smHov || tinyHov || midSaveHov || midUndoHov ||
                    ed.modelToolHover >= 0 || ed.gizmoHover != 0 || gzDrag != 0) orbitPane = 0;
                else if (inRight3d) orbitPane = 2;
                else if (inLeft3d) orbitPane = 1;
                else orbitPane = 0;
            }
            if (rmb && orbitPane != 0) {
                float& yaw = (orbitPane == 2) ? ed.rrotY : ed.brotY;
                float& pitch = (orbitPane == 2) ? ed.rrotX : ed.brotX;
                yaw += (mx - lastMx) * 0.008f;
                pitch += (my - lastMy) * 0.008f;
                if (pitch > 1.55f) pitch = 1.55f;
                if (pitch < -1.55f) pitch = -1.55f;
                lastMx = mx; lastMy = my;
            }
            if (!rmb) orbitPane = 0;
        } else {
            // texture painting
            const int T = tex::TILE;
            mat::Image& img = mat::g_tileImages[ed.tile];

            // tool / brush-size / undo shortcuts
            static bool prevB = false, prevN = false, prevG = false, prevI = false, prevX = false, prevR = false, prevL = false, prevO = false, prevU = false, prevPlus = false, prevMinus = false, prevA = false, prevC = false;
            if (keyDown('B') && !prevB) { ed.tool = 0; ed.picker = false; ed.brushMod = false; }   // paintbrush
            if (keyDown('N') && !prevN) { ed.tool = 1; ed.picker = false; ed.brushMod = false; }   // brush (multi-color)
            if (keyDown('G') && !prevG) { ed.tool = 2; ed.picker = false; ed.brushMod = false; }   // bucket
            if (keyDown('I') && !prevI) { ed.picker = !ed.picker; if (ed.picker) ed.brushMod = false; } // picker
            if (keyDown('X') && !prevX) { ed.tool = 4; ed.picker = false; ed.brushMod = false; }   // eraser
            if (keyDown('R') && !prevR) { ed.tool = 6; ed.picker = false; ed.brushMod = false; }   // rectangle
            if (keyDown('L') && !prevL) { ed.tool = 7; ed.picker = false; ed.brushMod = false; }   // line
            if (keyDown('O') && !prevO) { ed.tool = 8; ed.picker = false; ed.brushMod = false; }   // circle
            prevB = keyDown('B'); prevN = keyDown('N'); prevG = keyDown('G'); prevI = keyDown('I'); prevX = keyDown('X');
            // add current color to the multi-color brush / clear it
            if (keyDown('A') && !prevA) {
                addBrushColor({ (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) });
            }
            if (keyDown('C') && !prevC) { ed.brushColors.assign(16, Color4{}); ed.brushSet.assign(16, 0); ed.brushSel.assign(16, 0); }
            prevA = keyDown('A'); prevC = keyDown('C');
            if (keyDown(VK_OEM_PLUS) && !prevPlus && ed.brushSize < 4) ed.brushSize++;
            if (keyDown(VK_OEM_MINUS) && !prevMinus && ed.brushSize > 1) ed.brushSize--;
            prevPlus = keyDown(VK_OEM_PLUS); prevMinus = keyDown(VK_OEM_MINUS);
            bool undo = (keyDown(VK_CONTROL) && keyDown('Z')) || (keyDown('U') && !prevU);
            prevU = keyDown('U');
            if (undo && !ed.undoStack.empty()) {
                img.rgba = ed.undoStack.back();
                ed.undoStack.pop_back();
                ed.dirty = true;
            }

            // toolbar button clicks
            if (lmb && !prevLmb && ed.toolHover >= 0) {
                int i = ed.toolHover;
                if (i == 0) { ed.tool = (ed.tool == 0) ? -1 : 0; ed.picker = false; ed.brushMod = false; }        // paintbrush
                else if (i == 1) {   // brush: stack on bucket if bucket is selected, else toggle
                    if (ed.tool == 2) { ed.brushMod = !ed.brushMod; if (ed.brushMod) ed.picker = false; }
                    else { ed.tool = (ed.tool == 1) ? -1 : 1; ed.picker = false; ed.brushMod = false; }
                }
                else if (i == 2) { ed.tool = (ed.tool == 2) ? -1 : 2; ed.picker = false; ed.brushMod = false; }   // bucket
                else if (i == 3) { ed.picker = !ed.picker; if (ed.picker) ed.brushMod = false; } // picker
                else if (i == 4) { ed.tool = (ed.tool == 4) ? -1 : 4; ed.picker = false; ed.brushMod = false; }   // eraser
                else if (i == 5) { ed.tool = (ed.tool == 5) ? -1 : 5; ed.picker = false; ed.brushMod = false; }   // rect bucket
                else if (i == 6) { ed.tool = (ed.tool == 6) ? -1 : 6; ed.picker = false; ed.brushMod = false; }   // rectangle
                else if (i == 7) { ed.tool = (ed.tool == 7) ? -1 : 7; ed.picker = false; ed.brushMod = false; }   // line
                else if (i == 8) { ed.tool = (ed.tool == 8) ? -1 : 8; ed.picker = false; ed.brushMod = false; }   // circle
                else if (i == 9) {
                    if (!ed.undoStack.empty()) { img.rgba = ed.undoStack.back(); ed.undoStack.pop_back(); ed.dirty = true; }
                } else if (i == 10) { if (ed.brushSize > 1) ed.brushSize--; }
                else if (i == 11) { if (ed.brushSize < 4) ed.brushSize++; }
                else if (i == 12) {
                    if (img.ok()) {
                        std::string p = pack::tilePng(mat::tileName(ed.tile));
                        mat::savePNG(p.c_str(), img.w, img.h, img.rgba.data());
                        ed.dirty = false;
                    }
                    syncTilesToGL();
                }
            }

            // SV rectangle + hue ring clicks
            if (lmb) {
                if (mx >= ed.svRect[0] && mx < ed.svRect[0] + ed.svRect[2] && my >= ed.svRect[1] && my < ed.svRect[1] + ed.svRect[3]) {
                    float s = (mx - ed.svRect[0]) / ed.svRect[2];
                    float v = 1.0f - (my - ed.svRect[1]) / ed.svRect[3];
                    if (s < 0) s = 0; if (s > 1) s = 1;
                    if (v < 0) v = 0; if (v > 1) v = 1;
                    float hh, ss, vv; rgbToHsv(ed.r, ed.g, ed.b, hh, ss, vv);
                    hsvToRgb(hh, s, v, ed.r, ed.g, ed.b);
                }
                float ddx = mx - ed.hueCX, ddy = my - ed.hueCY;
                float dist = std::sqrt(ddx * ddx + ddy * ddy);
                if (dist >= ed.hueRI - 6.0f && dist <= ed.hueRO + 6.0f) {
                    float h = std::atan2(ddy, ddx) / 6.2831853f;
                    if (h < 0) h += 1.0f;
                    float hh, ss, vv; rgbToHsv(ed.r, ed.g, ed.b, hh, ss, vv);
                    hsvToRgb(h, ss, vv, ed.r, ed.g, ed.b);
                }
            }

            ed.hoverX = -1; ed.hoverY = -1;
            if (ed.tileSize > 0) {
                int hx = (int)((mx - ed.tileX) / (ed.tileSize / T));
                int hy = (int)((my - ed.tileY) / (ed.tileSize / T));
                if (hx >= 0 && hx < T && hy >= 0 && hy < T) { ed.hoverX = hx; ed.hoverY = hy; }
            }

            if (lmb && !prevLmb && ed.hoverTile >= 0 && ed.hoverTile < TEX_COUNT) {
                ed.tile = ed.hoverTile;               // tile selection panel
            } else if (img.ok() && ed.hoverX >= 0) {
                auto pushRecent = [&](uint8_t rr, uint8_t gg, uint8_t bb, uint8_t aa) {
                    for (size_t k = 0; k < ed.recent.size(); k++) {
                        if (ed.recent[k].r == rr && ed.recent[k].g == gg && ed.recent[k].b == bb && ed.recent[k].a == aa) {
                            ed.recent.erase(ed.recent.begin() + k);
                            break;
                        }
                    }
                    ed.recent.push_back({ rr, gg, bb, aa });
                    if (ed.recent.size() > 8) ed.recent.erase(ed.recent.begin());
                };
                int eff = ed.picker ? 3 : ed.tool;
                if (eff == 3) {                       // picker / eyedropper (temporary)
                    if (lmb || (rmb && !prevRmb)) {
                        auto samplePx = [&](int px, int py, Color4& out) -> bool {
                            if (px < 0 || px >= T || py < 0 || py >= T) return false;
                            size_t i = ((size_t)py * T + px) * 4;
                            out = { img.rgba[i + 0], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3] };
                            return true;
                        };
                        Color4 center{};
                        samplePx(ed.hoverX, ed.hoverY, center);
                        ed.r = center.r / 255.0f; ed.g = center.g / 255.0f;
                        ed.b = center.b / 255.0f; ed.a = center.a / 255.0f;
                        if (ed.tool == 1) {
                            int half = ed.brushSize / 2;
                            for (int dy = 0; dy < ed.brushSize; dy++) {
                                for (int dx = 0; dx < ed.brushSize; dx++) {
                                    Color4 c;
                                    if (!samplePx(ed.hoverX + dx - half, ed.hoverY + dy - half, c)) continue;
                                    addBrushColor(c);
                                }
                            }
                        } else if (ed.brushSize > 1) {
                            long sr = 0, sg = 0, sb = 0, sa = 0; int cnt = 0;
                            int half = ed.brushSize / 2;
                            for (int dy = 0; dy < ed.brushSize; dy++) {
                                for (int dx = 0; dx < ed.brushSize; dx++) {
                                    Color4 c;
                                    if (!samplePx(ed.hoverX + dx - half, ed.hoverY + dy - half, c)) continue;
                                    if (!ed.pickAlpha0 && c.a == 0) continue;
                                    sr += c.r; sg += c.g; sb += c.b; sa += c.a; cnt++;
                                }
                            }
                            if (cnt > 0) {
                                ed.r = (float)(sr / cnt) / 255.0f; ed.g = (float)(sg / cnt) / 255.0f;
                                ed.b = (float)(sb / cnt) / 255.0f; ed.a = (float)(sa / cnt) / 255.0f;
                            }
                        }
                        ed.picker = false;
                        ed.justPicked = true; // suppress paint/fill until the mouse is released
                    }
                } else if (!ed.justPicked && eff == 2) {  // bucket / flood fill
                    if (lmb && !prevLmb) {
                        ed.undoStack.push_back(img.rgba);
                        if (ed.undoStack.size() > 32) ed.undoStack.erase(ed.undoStack.begin());
                        if (ed.brushMod) {
                            std::vector<Color4> valid;
                            for (size_t k = 0; k < ed.brushColors.size(); k++) {
                                if (k < ed.brushSet.size() && ed.brushSet[k]) valid.push_back(ed.brushColors[k]);
                            }
                            if (valid.empty()) floodFill(img, ed.hoverX, ed.hoverY, (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                            else floodFillRandom(img, ed.hoverX, ed.hoverY, valid);
                        } else {
                            floodFill(img, ed.hoverX, ed.hoverY, (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                        }
                        pushRecent((uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                        ed.dirty = true;
                    }
                } else if (!ed.justPicked && (eff == 5 || eff == 6 || eff == 7 || eff == 8)) {
                    // shape tools (drag)
                    if (lmb && !prevLmb) {
                        ed.dragging = true;
                        ed.dragX0 = ed.hoverX; ed.dragY0 = ed.hoverY;
                        ed.dragX1 = ed.hoverX; ed.dragY1 = ed.hoverY;
                        ed.undoStack.push_back(img.rgba);
                        if (ed.undoStack.size() > 32) ed.undoStack.erase(ed.undoStack.begin());
                    }
                    if (ed.dragging) { ed.dragX1 = ed.hoverX; ed.dragY1 = ed.hoverY; }
                } else if (!ed.justPicked && eff == 4) {            // eraser
                    if (lmb) {
                        if (lmb && !prevLmb) {
                            ed.undoStack.push_back(img.rgba);
                            if (ed.undoStack.size() > 32) ed.undoStack.erase(ed.undoStack.begin());
                        }
                        int half = ed.brushSize / 2;
                        for (int dy = 0; dy < ed.brushSize; dy++) {
                            for (int dx = 0; dx < ed.brushSize; dx++) {
                                int pxx = ed.hoverX + dx - half, pyy = ed.hoverY + dy - half;
                                if (pxx < 0 || pxx >= T || pyy < 0 || pyy >= T) continue;
                                size_t i = ((size_t)pyy * T + pxx) * 4;
                                img.rgba[i + 0] = 0; img.rgba[i + 1] = 0; img.rgba[i + 2] = 0; img.rgba[i + 3] = 0;
                            }
                        }
                        ed.dirty = true;
                    }
                } else if (!ed.justPicked && eff == 1) {            // multi-color brush (random from list)
                    if (lmb) {
                        if (lmb && !prevLmb) {
                            ed.undoStack.push_back(img.rgba);
                            if (ed.undoStack.size() > 32) ed.undoStack.erase(ed.undoStack.begin());
                        }
                        int half = ed.brushSize / 2;
                        for (int dy = 0; dy < ed.brushSize; dy++) {
                            for (int dx = 0; dx < ed.brushSize; dx++) {
                                int pxx = ed.hoverX + dx - half, pyy = ed.hoverY + dy - half;
                                if (pxx < 0 || pxx >= T || pyy < 0 || pyy >= T) continue;
                                size_t i = ((size_t)pyy * T + pxx) * 4;
                                if (ed.brushColors.empty()) {
                                    img.rgba[i + 0] = (uint8_t)(ed.r * 255); img.rgba[i + 1] = (uint8_t)(ed.g * 255);
                                    img.rgba[i + 2] = (uint8_t)(ed.b * 255); img.rgba[i + 3] = (uint8_t)(ed.a * 255);
                                } else {
                                    std::vector<int> valid;
                                    for (size_t k = 0; k < ed.brushColors.size(); k++) {
                                        if (k < ed.brushSet.size() && ed.brushSet[k]) valid.push_back((int)k);
                                    }
                                    Color4 c;
                                    if (valid.empty()) c = { (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                                    else c = ed.brushColors[valid[rand() % valid.size()]];
                                    img.rgba[i + 0] = c.r; img.rgba[i + 1] = c.g; img.rgba[i + 2] = c.b; img.rgba[i + 3] = c.a;
                                }
                            }
                        }
                        ed.dirty = true;
                    }
                } else if (!ed.justPicked && eff == 0) {                              // paintbrush (single color)
                    if (lmb) {
                        if (lmb && !prevLmb) {
                            ed.undoStack.push_back(img.rgba);
                            if (ed.undoStack.size() > 32) ed.undoStack.erase(ed.undoStack.begin());
                        }
                        int half = ed.brushSize / 2;
                        for (int dy = 0; dy < ed.brushSize; dy++) {
                            for (int dx = 0; dx < ed.brushSize; dx++) {
                                int pxx = ed.hoverX + dx - half, pyy = ed.hoverY + dy - half;
                                if (pxx < 0 || pxx >= T || pyy < 0 || pyy >= T) continue;
                                size_t i = ((size_t)pyy * T + pxx) * 4;
                                img.rgba[i + 0] = (uint8_t)(ed.r * 255); img.rgba[i + 1] = (uint8_t)(ed.g * 255);
                                img.rgba[i + 2] = (uint8_t)(ed.b * 255); img.rgba[i + 3] = (uint8_t)(ed.a * 255);
                            }
                        }
                        pushRecent((uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255));
                        ed.dirty = true;
                    }
                }
            }
            // clear the pick-suppression flag once the mouse is released
            if (ed.justPicked && !lmb && !rmb) ed.justPicked = false;
            // apply shape on mouse release
            if (ed.dragging && !lmb) {
                Color4 c = { (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                int x0 = ed.dragX0, y0 = ed.dragY0, x1 = ed.dragX1, y1 = ed.dragY1;
                switch (ed.tool) {
                    case 5: fillRectImg(img, x0, y0, x1, y1, c); break;
                    case 6:
                        drawLineImg(img, x0, y0, x1, y0, c, 1);
                        drawLineImg(img, x1, y0, x1, y1, c, 1);
                        drawLineImg(img, x1, y1, x0, y1, c, 1);
                        drawLineImg(img, x0, y1, x0, y0, c, 1);
                        break;
                    case 7: drawLineImg(img, x0, y0, x1, y1, c, ed.brushSize); break;
                    case 8: {
                        int r = (int)std::sqrt((float)((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)));
                        drawCircleImg(img, x0, y0, r, c);
                        break;
                    }
                }
                ed.dragging = false;
                ed.dirty = true;
            }
            if (keyDown(VK_OEM_4) && !prevLbrack) ed.tile = (ed.tile + TEX_COUNT - 1) % TEX_COUNT;
            if (keyDown(VK_OEM_6) && !prevRbrack) ed.tile = (ed.tile + 1) % TEX_COUNT;
        }

        // EXIT button (bottom-right): return to the mode chooser.
        if (startMode >= 0) {
            const float exW = 120.0f, exH = 44.0f, exM = 20.0f;
            float exX = g_winW - exW - exM, exY = g_winH - exH - exM;
            if (lmb && !prevLmb && mx >= exX && mx < exX + exW && my >= exY && my < exY + exH) {
                if (ed.modelMode) bakeFillIfLeaving();
                startMode = -1;
                ed.entityMode = false;
                ed.modelMode = false;
                ed.animMode = false;
                ed.animPlaying = false;
                ed.toolHover = -1;
                clearEditorTools();
            }
        }

        // save
        bool f5 = keyDown(VK_F5);
        if (f5 && !prevF5 && startMode >= 0) {
            if (ed.animMode) {
                saveAnimClip();
            } else if (ed.entityMode) {
                saveEntity();
            } else if (ed.modelMode) {
                saveRandFile();
            } else {
                mat::Image& img = mat::g_tileImages[ed.tile];
                if (img.ok()) {
                    std::string p = pack::tilePng(mat::tileName(ed.tile));
                    mat::savePNG(p.c_str(), img.w, img.h, img.rgba.data());
                }
                syncTilesToGL();
            }
            ed.dirty = false;
        }
        prevF5 = f5;

        // ---- render ----
        gl::Viewport(0, 0, g_winW, g_winH);
        gl::ClearColor(0.08f, 0.08f, 0.11f, 1.0f);
        gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        gl::Enable(GL_BLEND);
        gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl::Disable(GL_DEPTH_TEST);
        gl::UseProgram(prog);

        // orthographic MVP for 2D
        Mat4 ortho;
        ortho.m[0] = 2.0f / g_winW; ortho.m[5] = -2.0f / g_winH; ortho.m[10] = 1.0f;
        ortho.m[12] = -1.0f; ortho.m[13] = 1.0f; ortho.m[15] = 1.0f;
        Mat4 mvp = ortho;      // 2D UI (triangles) — always orthographic
        Mat4 mvp3d = ortho;    // 3D wireframe (lines) — perspective in model modes
        Mat4 mvpRand = ortho;  // right-pane rand preview

        std::vector<float> verts;      // 2D UI triangles
        std::vector<float> lineVerts;  // 3D wireframe line segments (GL_LINES)
        std::vector<float> texVerts;   // 3D textured quads (pos3 + color4 + uv2)
        std::vector<float> randLineVerts;
        std::vector<float> randTexVerts;
        std::vector<float> gizmoOverlay; // 2D overlay lines/tris for move gizmos
        std::vector<float> entSolid;     // entity cuboid fill (pos+color, untextured)
        std::vector<float> entTex;       // entity textured cutouts (pos+color+uv)
        struct TexBatch { unsigned tex = 0; int wrap = 0; std::vector<float> verts; };
        std::vector<TexBatch> leftBatches;
        std::vector<float> itemSolid;
        std::vector<float> itemSolidRand;
        std::vector<TexBatch> entBatches;
        std::vector<float> entRightSolid;
        std::vector<TexBatch> entRightBatches;
        auto rect = [&](float x, float y, float w, float h, float r, float g, float b, float a) {
            float v[6][7] = {
                {x,y,0,r,g,b,a},{x+w,y,0,r,g,b,a},{x+w,y+h,0,r,g,b,a},
                {x,y,0,r,g,b,a},{x+w,y+h,0,r,g,b,a},{x,y+h,0,r,g,b,a},
            };
            for (auto& e : v) for (int i = 0; i < 7; i++) verts.push_back(e[i]);
        };
        auto line = [&](float x0, float y0, float z0, float x1, float y1, float z1, float r, float g, float b, float a) {
            float v[2][7] = { {x0,y0,z0,r,g,b,a},{x1,y1,z1,r,g,b,a} };
            for (auto& e : v) for (int i = 0; i < 7; i++) lineVerts.push_back(e[i]);
        };
        auto rline = [&](float x0, float y0, float z0, float x1, float y1, float z1, float r, float g, float b, float a) {
            float v[2][7] = { {x0,y0,z0,r,g,b,a},{x1,y1,z1,r,g,b,a} };
            for (auto& e : v) for (int i = 0; i < 7; i++) randLineVerts.push_back(e[i]);
        };
        auto appendMesh = [](std::vector<float>& dst, const std::vector<Vertex>& mesh) {
            for (const Vertex& v : mesh) {
                dst.push_back(v.px); dst.push_back(v.py); dst.push_back(v.pz);
                float s = v.faceShade;
                dst.push_back(s); dst.push_back(s); dst.push_back(s); dst.push_back(1.0f);
                dst.push_back(v.u); dst.push_back(v.v);
            }
        };
        auto meshPivot = [](const std::vector<Vertex>& mesh, float& cx, float& cy, float& cz) {
            cx = 0.0f; cy = 0.5f; cz = 0.0f;
            if (mesh.empty()) return;
            float mnX = mesh[0].px, mxX = mnX, mnY = mesh[0].py, mxY = mnY, mnZ = mesh[0].pz, mxZ = mnZ;
            for (const Vertex& v : mesh) {
                if (v.px < mnX) mnX = v.px;
                if (v.px > mxX) mxX = v.px;
                if (v.py < mnY) mnY = v.py;
                if (v.py > mxY) mxY = v.py;
                if (v.pz < mnZ) mnZ = v.pz;
                if (v.pz > mxZ) mxZ = v.pz;
            }
            cx = 0.5f * (mnX + mxX); cy = 0.5f * (mnY + mxY); cz = 0.5f * (mnZ + mxZ);
        };
        auto orbitMvp = [](float aspect, float yaw, float pitch, float zoom, float px, float py, float pz) {
            if (aspect < 0.05f) aspect = 0.05f;
            Mat4 proj = Mat4::perspective(45.0f, aspect, 0.05f, 100.0f);
            float eyex = std::sin(yaw) * std::cos(pitch) * zoom + px;
            float eyey = std::sin(pitch) * zoom + py;
            float eyez = std::cos(yaw) * std::cos(pitch) * zoom + pz;
            return proj * Mat4::lookAt({eyex, eyey, eyez}, {px, py, pz}, {0, 1, 0});
        };
        auto tri = [&](float ax, float ay, float bx, float by, float cx, float cy, float r, float g, float b, float a) {
            float v[3][7] = { {ax,ay,0,r,g,b,a},{bx,by,0,r,g,b,a},{cx,cy,0,r,g,b,a} };
            for (auto& e : v) for (int i = 0; i < 7; i++) verts.push_back(e[i]);
        };
        // Draw an 8x8 pixel icon ('Q' = filled, 'O' = empty) inside a square.
        auto drawIcon = [&](float bx, float by, float size, const char* const rows[8], float r, float g, float b) {
            float cell = size / 8.0f;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                if (rows[y][x] == 'Q') rect(bx + x * cell, by + y * cell, cell + 0.5f, cell + 0.5f, r, g, b, 1);
            }
        };

        // Draw a cached text texture (textured pass) at (tx, ty).
        auto drawTextTex = [&](TextTex& tt, float tx, float ty) {
            if (!tt.tex) return;
            float vtx[6][9] = {
                {tx,ty,0, 1,1,1,1, 0,0}, {tx+tt.w,ty,0, 1,1,1,1, 1,0}, {tx+tt.w,ty+tt.h,0, 1,1,1,1, 1,1},
                {tx,ty,0, 1,1,1,1, 0,0}, {tx+tt.w,ty+tt.h,0, 1,1,1,1, 1,1}, {tx,ty+tt.h,0, 1,1,1,1, 0,1},
            };
            gl::BindVertexArray(tvao);
            gl::BindBuffer(GL_ARRAY_BUFFER, tvbo);
            gl::BufferData(GL_ARRAY_BUFFER, sizeof(vtx), vtx, GL_STREAM_DRAW);
            gl::UseProgram(tprog);
            gl::UniformMatrix4fv(tuMVP, 1, GL_FALSE, mvp.m);
            gl::BindTexture(GL_TEXTURE_2D, tt.tex);
            gl::DrawArrays(GL_TRIANGLES, 0, 6);
            gl::UseProgram(prog);
            gl::BindVertexArray(0);
        };
        // Scale a text texture so it stays inside a button. Entity tool rows shrink
        // with the window; the 20px font otherwise spills out and disappears.
        auto drawTextFit = [&](TextTex& tt, float x, float y, float w, float h) {
            if (!tt.tex || w < 4.0f || h < 4.0f) return;
            float aw = w - 6.0f, ah = h - 4.0f;
            if (aw < 2.0f) aw = 2.0f;
            if (ah < 2.0f) ah = 2.0f;
            float s = aw / (float)tt.w;
            float sh = ah / (float)tt.h;
            if (sh < s) s = sh;
            if (s > 1.0f) s = 1.0f;
            float dw = (float)tt.w * s, dh = (float)tt.h * s;
            float tx = x + (w - dw) * 0.5f, ty = y + (h - dh) * 0.5f;
            float vtx[6][9] = {
                {tx,ty,0, 1,1,1,1, 0,0}, {tx+dw,ty,0, 1,1,1,1, 1,0}, {tx+dw,ty+dh,0, 1,1,1,1, 1,1},
                {tx,ty,0, 1,1,1,1, 0,0}, {tx+dw,ty+dh,0, 1,1,1,1, 1,1}, {tx,ty+dh,0, 1,1,1,1, 0,1},
            };
            gl::BindVertexArray(tvao);
            gl::BindBuffer(GL_ARRAY_BUFFER, tvbo);
            gl::BufferData(GL_ARRAY_BUFFER, sizeof(vtx), vtx, GL_STREAM_DRAW);
            gl::UseProgram(tprog);
            gl::UniformMatrix4fv(tuMVP, 1, GL_FALSE, mvp.m);
            gl::BindTexture(GL_TEXTURE_2D, tt.tex);
            gl::DrawArrays(GL_TRIANGLES, 0, 6);
            gl::UseProgram(prog);
            gl::BindVertexArray(0);
        };

        // Draw one atlas tile as a 2D quad (ortho) — used for the block list icons.
        auto drawAtlasTile = [&](int tile, float x, float y, float w, float h) {
            if (!atlasTex) return;
            float u0, v0, u1, v1;
            tex::tileUV(tile, u0, v0, u1, v1);
            float vtx[6][9] = {
                {x,y,0, 1,1,1,1, u0,v0}, {x+w,y,0, 1,1,1,1, u1,v0}, {x+w,y+h,0, 1,1,1,1, u1,v1},
                {x,y,0, 1,1,1,1, u0,v0}, {x+w,y+h,0, 1,1,1,1, u1,v1}, {x,y+h,0, 1,1,1,1, u0,v1},
            };
            gl::BindVertexArray(tvao);
            gl::BindBuffer(GL_ARRAY_BUFFER, tvbo);
            gl::BufferData(GL_ARRAY_BUFFER, sizeof(vtx), vtx, GL_STREAM_DRAW);
            gl::UseProgram(tprog);
            gl::UniformMatrix4fv(tuMVP, 1, GL_FALSE, mvp.m);
            gl::BindTexture(GL_TEXTURE_2D, atlasTex);
            gl::DrawArrays(GL_TRIANGLES, 0, 6);
            gl::UseProgram(prog);
            gl::BindVertexArray(0);
        };

        auto drawGlTex = [&](unsigned tex, float x, float y, float w, float h) {
            if (!tex) return;
            float vtx[6][9] = {
                {x,y,0, 1,1,1,1, 0,0}, {x+w,y,0, 1,1,1,1, 1,0}, {x+w,y+h,0, 1,1,1,1, 1,1},
                {x,y,0, 1,1,1,1, 0,0}, {x+w,y+h,0, 1,1,1,1, 1,1}, {x,y+h,0, 1,1,1,1, 0,1},
            };
            gl::BindVertexArray(tvao);
            gl::BindBuffer(GL_ARRAY_BUFFER, tvbo);
            gl::BufferData(GL_ARRAY_BUFFER, sizeof(vtx), vtx, GL_STREAM_DRAW);
            gl::UseProgram(tprog);
            gl::UniformMatrix4fv(tuMVP, 1, GL_FALSE, mvp.m);
            gl::BindTexture(GL_TEXTURE_2D, tex);
            gl::DrawArrays(GL_TRIANGLES, 0, 6);
            gl::UseProgram(prog);
            gl::BindVertexArray(0);
        };

        if (startMode < 0) {
            // ---- mode chooser (entry screen): buttons only; text drawn later ----
            rect(0, 0, (float)g_winW, (float)g_winH, 0.05f, 0.05f, 0.08f, 1.0f);
            const float bw = 380.0f, bh = 64.0f, gap = 20.0f;
            const int nOpts = 6;
            const float totalH = nOpts * bh + (nOpts - 1) * gap;
            const float bx = (g_winW - bw) * 0.5f;
            const float y0 = (g_winH - totalH) * 0.5f - 50.0f;
            for (int i = 0; i < nOpts; i++) {
                float by = y0 + i * (bh + gap);
                bool hov = mx >= bx && mx < bx + bw && my >= by && my < by + bh;
                rect(bx, by, bw, bh, hov ? 0.32f : 0.18f, hov ? 0.40f : 0.18f, hov ? 0.22f : 0.18f, 1.0f);
                if (hov) rect(bx - 2, by - 2, bw + 4, bh + 4, 0.95f, 0.85f, 0.30f, 1.0f);
            }
        } else if (ed.animMode) {
            rect(0, 0, (float)g_winW, (float)g_winH, 0.07f, 0.07f, 0.09f, 1.0f);
            rect(0, 0, animLeftW, (float)g_winH, 0.10f, 0.11f, 0.13f, 1);
            rect((float)g_winW - animRightW, 0, animRightW, (float)g_winH, 0.10f, 0.11f, 0.13f, 1);
            rect(0, 0, (float)g_winW, kTopH + kHdrH, 0.12f, 0.13f, 0.16f, 1);
            float ty = animBtnRect[0][1] - 8.0f;
            rect(0, ty, (float)g_winW, (float)g_winH - ty, 0.11f, 0.12f, 0.14f, 1);
            rect(animVX, animVY, animVW, animVH, 0.16f, 0.17f, 0.20f, 1);
            auto fillR = [&](const float r[4], float cr, float cg, float cb, bool hov, bool on) {
                if (r[2] < 1.0f) return;
                float r0 = on ? 0.42f : (hov ? 0.28f : cr);
                float g0 = on ? 0.38f : (hov ? 0.30f : cg);
                float b0 = on ? 0.22f : (hov ? 0.22f : cb);
                rect(r[0], r[1], r[2], r[3], r0, g0, b0, 1);
                if (hov) rect(r[0] - 1, r[1] - 1, r[2] + 2, r[3] + 2, 1, 1, 1, 1);
            };
            auto hitRA = [&](const float r[4]) {
                return r[2] > 1.0f && mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3];
            };
            auto beginPaneScissor = [&](float x, float w) {
                int sx = (int)std::floor(x);
                int sy = (int)std::floor((float)g_winH - animPaneBot);
                int sw = (int)std::ceil(w);
                int sh = (int)std::ceil(animPaneBot - animPaneTop);
                if (sx < 0) { sw += sx; sx = 0; }
                if (sy < 0) { sh += sy; sy = 0; }
                if (sw < 0) sw = 0;
                if (sh < 0) sh = 0;
                gl::Enable(GL_SCISSOR_TEST);
                gl::Scissor(sx, sy, sw, sh);
            };
            beginPaneScissor(0.0f, animLeftW);
            for (int i = 0; i < animClipN && i < (int)animNames.size(); i++) {
                bool on = (animNames[i] == editClip.name);
                fillR(animClipRect[i], 0.16f, 0.16f, 0.18f, ed.animClipHover == i, on);
            }
            fillR(animNewRect, 0.18f, 0.22f, 0.18f, hitRA(animNewRect), false);
            fillR(animDupRect, 0.18f, 0.20f, 0.22f, hitRA(animDupRect), false);
            fillR(animDelRect, 0.28f, 0.16f, 0.16f, hitRA(animDelRect), false);
            for (int i = 0; i < animBoneN; i++)
                fillR(animBoneRect[i], 0.15f, 0.15f, 0.17f, ed.animBoneHover == i, i == ed.animSelBone);
            gl::Disable(GL_SCISSOR_TEST);
            beginPaneScissor((float)g_winW - animRightW, animRightW);
            for (int i = 0; i < animBindN && i < (int)entNames.size(); i++)
                fillR(animBindRect[i], 0.16f, 0.16f, 0.18f, ed.animBindHover == i, entNames[i] == animBindName);
            fillR(animRebuildRect, 0.20f, 0.22f, 0.18f, hitRA(animRebuildRect), false);
            fillR(animModelSave, 0.28f, 0.20f, 0.12f, hitRA(animModelSave), ed.animModelDirty);
            fillR(animHoldPrev, 0.18f, 0.18f, 0.20f, hitRA(animHoldPrev), false);
            fillR(animHoldNext, 0.18f, 0.18f, 0.20f, hitRA(animHoldNext), false);
            fillR(animHoldL, 0.16f, 0.16f, 0.18f, hitRA(animHoldL), ed.animHoldSide < 0);
            fillR(animHoldR, 0.16f, 0.16f, 0.18f, hitRA(animHoldR), ed.animHoldSide > 0);
            fillR(animHoldGrip, 0.18f, 0.16f, 0.14f, hitRA(animHoldGrip), ed.animHoldEdit);
            fillR(animHoldGrasp, 0.12f, 0.22f, 0.24f, hitRA(animHoldGrasp), ed.animGrasp);
            fillR(animHoldLower, 0.18f, 0.18f, 0.20f, hitRA(animHoldLower), false);
            fillR(animHoldHigher, 0.18f, 0.18f, 0.20f, hitRA(animHoldHigher), false);
            fillR(animLockDir, 0.22f, 0.20f, 0.12f, hitRA(animLockDir), ed.animLockDir);
            fillR(animLockPos, 0.16f, 0.22f, 0.20f, hitRA(animLockPos), ed.animLockPos);
            fillR(animHoldBone, 0.16f, 0.18f, 0.16f, hitRA(animHoldBone), false);
            fillR(animHoldSave, 0.22f, 0.28f, 0.18f, hitRA(animHoldSave), ed.animHoldDirty);
            gl::Disable(GL_SCISSOR_TEST);
            auto drawScroll = [&](float x, float scroll, float maxScroll) {
                if (maxScroll <= 1.0f || animVH < 8.0f) return;
                float thumb = animVH * animVH / (animVH + maxScroll);
                if (thumb < 16.0f) thumb = 16.0f;
                if (thumb > animVH) thumb = animVH;
                float travel = animVH - thumb;
                float ty = animPaneTop + travel * (scroll / maxScroll);
                rect(x, animPaneTop, 4.0f, animVH, 0.16f, 0.16f, 0.18f, 1);
                rect(x, ty, 4.0f, thumb, 0.72f, 0.74f, 0.78f, 1);
            };
            drawScroll(animLeftW - 6.0f, ed.animScrollL, animScrollMaxL);
            drawScroll((float)g_winW - 6.0f, ed.animScrollR, animScrollMaxR);
            const char* kPlayOn[6] = {};
            (void)kPlayOn;
            for (int i = 0; i < 6; i++) {
                bool on = (i == 1 && ed.animPlaying);
                fillR(animBtnRect[i], 0.18f, 0.18f, 0.20f, ed.animBtnHover == i, on);
            }
            fillR(animBtnRect[6], 0.16f, 0.16f, 0.18f, ed.animBtnHover == 6, ed.animView == 0);
            fillR(animBtnRect[7], 0.16f, 0.16f, 0.18f, ed.animBtnHover == 7, ed.animView == 1);
            fillR(animBtnRect[8], 0.16f, 0.18f, 0.20f, ed.animBtnHover == 8, ed.animTool == 0);
            fillR(animBtnRect[9], 0.16f, 0.18f, 0.20f, ed.animBtnHover == 9, ed.animTool == 1);
            fillR(animBtnRect[10], 0.16f, 0.18f, 0.20f, ed.animBtnHover == 10, ed.animTool == 2 || keyDown(VK_SHIFT));
            fillR(animBtnRect[12], 0.22f, 0.16f, 0.12f, ed.animBtnHover == 12, ed.animTool == 3);
            fillR(animBtnRect[11], 0.22f, 0.18f, 0.16f, ed.animBtnHover == 11, ed.animMask);
            {
                const float kSw[6][3] = {
                    { 0.90f, 0.25f, 0.22f }, { 0.25f, 0.75f, 0.35f }, { 0.28f, 0.45f, 0.95f },
                    { 0.95f, 0.78f, 0.18f }, { 0.82f, 0.32f, 0.82f }, { 0.20f, 0.78f, 0.82f },
                };
                for (int i = 0; i < 6; i++)
                    fillR(animMaskSwatch[i], kSw[i][0], kSw[i][1], kSw[i][2], false, i == ed.animMaskColor);
            }
            fillR(animSaveRect, 0.22f, 0.28f, 0.18f, hitRA(animSaveRect), ed.animDirty);
            fillR(animUndoRect, 0.22f, 0.24f, 0.30f, hitRA(animUndoRect), !animUndo.empty());
            fillR(animLenMinus, 0.18f, 0.18f, 0.20f, hitRA(animLenMinus), false);
            fillR(animLenPlus, 0.18f, 0.18f, 0.20f, hitRA(animLenPlus), false);
            for (int i = 0; i < 4; i++)
                fillR(animScaleRect[i], 0.16f, 0.22f, 0.20f, hitRA(animScaleRect[i]), false);
            fillR(animKeyOp[0], 0.18f, 0.20f, 0.18f, hitRA(animKeyOp[0]), false);
            fillR(animKeyOp[1], 0.16f, 0.18f, 0.22f, hitRA(animKeyOp[1]), poseCopy.has);
            fillR(animKeyOp[2], 0.16f, 0.18f, 0.22f, hitRA(animKeyOp[2]), false);
            fillR(animKeyOp[3], 0.22f, 0.20f, 0.14f, hitRA(animKeyOp[3]), false);
            fillR(animKeyOp[4], 0.28f, 0.16f, 0.16f, hitRA(animKeyOp[4]), false);
            fillR(animKeyOp[5], 0.18f, 0.18f, 0.20f, hitRA(animKeyOp[5]), false);
            fillR(animKeyOp[6], 0.18f, 0.18f, 0.20f, hitRA(animKeyOp[6]), false);
            fillR(animKeyOp[7], 0.20f, 0.18f, 0.12f, hitRA(animKeyOp[7]), ed.animSpanA >= 0);
            fillR(animKeyOp[8], 0.20f, 0.18f, 0.12f, hitRA(animKeyOp[8]), ed.animSpanB >= 0);
            fillR(animKeyOp[9], 0.14f, 0.24f, 0.20f, hitRA(animKeyOp[9]), false);
            fillR(animTurnRect[0], 0.16f, 0.22f, 0.18f, hitRA(animTurnRect[0]), false);
            fillR(animTurnRect[1], 0.18f, 0.18f, 0.20f, hitRA(animTurnRect[1]), false);
            fillR(animTurnRect[2], 0.18f, 0.18f, 0.20f, hitRA(animTurnRect[2]), false);
            fillR(animFlipRect[0], 0.42f, 0.16f, 0.14f, hitRA(animFlipRect[0]), false);
            fillR(animFlipRect[1], 0.42f, 0.36f, 0.12f, hitRA(animFlipRect[1]), false);
            fillR(animFlipRect[2], 0.14f, 0.24f, 0.42f, hitRA(animFlipRect[2]), false);
            rect(animTimeRect[0], animTimeRect[1], animTimeRect[2], animTimeRect[3], 0.08f, 0.08f, 0.10f, 1);
            if (editClip.length > 1 && ed.animSpanA >= 0 && ed.animSpanB >= 0 && ed.animSpanA != ed.animSpanB) {
                int lo = ed.animSpanA < ed.animSpanB ? ed.animSpanA : ed.animSpanB;
                int hi = ed.animSpanA < ed.animSpanB ? ed.animSpanB : ed.animSpanA;
                float pad = animBarPitch * 0.5f;
                float xL = frameToX((float)lo) - pad;
                float xR = frameToX((float)hi) + pad;
                if (xL < animTimeRect[0]) xL = animTimeRect[0];
                if (xR > animTimeRect[0] + animTimeRect[2]) xR = animTimeRect[0] + animTimeRect[2];
                if (xR > xL)
                    rect(xL, animTimeRect[1], xR - xL, animTimeRect[3], 0.35f, 0.55f, 0.85f, 0.38f);
            }
            gl::Enable(GL_SCISSOR_TEST);
            gl::Scissor((int)std::floor(animTimeRect[0]),
                        (int)std::floor((float)g_winH - (animTimeRect[1] + animTimeRect[3])),
                        (int)std::ceil(animTimeRect[2]), (int)std::ceil(animTimeRect[3]));
            if (editClip.length > 1) {
                for (const anim::Track& tr : editClip.tracks) {
                    bool sel = (ed.animSelBone >= 0 && ed.animSelBone < (int)editClip.bones.size()
                                && tr.bone == viewBoneName(editClip.bones[ed.animSelBone].name));
                    bool gripTr = (tr.bone == anim::kGripTrack);
                    bool faceTr = (tr.bone == anim::kFaceTrack);
                    for (const anim::Key& k : tr.keys) {
                        float kx = frameToX((float)k.frame);
                        float kw = (gripTr || faceTr) ? 8.0f : 6.0f, kh = (sel || gripTr || faceTr) ? 18.0f : 10.0f;
                        float kr = faceTr ? 0.95f : (gripTr ? 0.20f : (sel ? 1.0f : 0.75f));
                        float kg = faceTr ? 0.55f : (gripTr ? 0.85f : (sel ? 0.82f : 0.55f));
                        float kb = faceTr ? 0.15f : (gripTr ? 0.95f : (sel ? 0.2f : 0.35f));
                        rect(kx - kw * 0.5f, animTimeRect[1] + (animTimeRect[3] - kh) * 0.5f, kw, kh,
                             kr, kg, kb, 1);
                    }
                }
                float px = frameToX(ed.animClock);
                rect(px - 2.0f, animTimeRect[1], 4.0f, animTimeRect[3], 1, 1, 1, 1);
            }
            gl::Disable(GL_SCISSOR_TEST);
            rect(animBarSlider[0], animBarSlider[1], animBarSlider[2], animBarSlider[3], 0.10f, 0.10f, 0.12f, 1);
            {
                float content = animTimeRect[2] + animBarScrollMax;
                float tw = (content > 1.0f) ? animBarSlider[2] * animTimeRect[2] / content : animBarSlider[2];
                if (tw < 16.0f) tw = 16.0f;
                if (tw > animBarSlider[2]) tw = animBarSlider[2];
                float travel = animBarSlider[2] - tw;
                float u = (animBarScrollMax > 0.0f) ? ed.animBarScroll / animBarScrollMax : 0.0f;
                rect(animBarSlider[0] + travel * u, animBarSlider[1], tw, animBarSlider[3], 0.72f, 0.74f, 0.78f, 1);
            }
            // posed model / skeleton into entSolid + lineVerts
            mvp3d = animMvp;
            anim::Clip viewClip = previewClipNow();
            auto pose = poseShown(viewClip, ed.animClock);
            auto pline = [&](const Vec3& a, const Vec3& b, float r, float g, float bl, float a0) {
                float v[2][7] = { {a.x,a.y,a.z,r,g,bl,a0},{b.x,b.y,b.z,r,g,bl,a0} };
                for (auto& e : v) for (int i = 0; i < 7; i++) lineVerts.push_back(e[i]);
            };
            auto pquad = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float r, float g, float bl) {
                float v[6][7] = {
                    {a.x,a.y,a.z,r,g,bl,1},{b.x,b.y,b.z,r,g,bl,1},{c.x,c.y,c.z,r,g,bl,1},
                    {a.x,a.y,a.z,r,g,bl,1},{c.x,c.y,c.z,r,g,bl,1},{d.x,d.y,d.z,r,g,bl,1},
                };
                for (auto& e : v) for (int i = 0; i < 7; i++) entSolid.push_back(e[i]);
            };
            for (float gx = -0.6f; gx <= 0.6f + 1e-4f; gx += 0.2f) {
                pline({ gx, 0, -0.6f }, { gx, 0, 0.6f }, 0.28f, 0.28f, 0.30f, 0.7f);
                pline({ -0.6f, 0, gx }, { 0.6f, 0, gx }, 0.28f, 0.28f, 0.30f, 0.7f);
            }
            if (ed.animView == 1 && !animBindParts.empty()) {
                std::vector<pm::Part> posed = anim::poseParts(animBindParts, viewClip, pose);
                auto hasNamed = [&](const std::string& name) -> bool {
                    if (name.empty()) return false;
                    auto it = overlayGL.find(name);
                    if (it != overlayGL.end() && it->second) return true;
                    for (int i = 0; i < (int)entMouthNames.size() && i < 3; i++)
                        if (name == entMouthNames[i] && mouthGL[i]) return true;
                    int slot = pm::overlayFileSlotOf(name, entMouthNames);
                    if (slot == 0 && leafGL) return true;
                    if (slot == 1 && eyeGL) return true;
                    if (slot >= 2 && slot < 5 && mouthGL[slot - 2]) return true;
                    if (slot == 5 && eyelidGL) return true;
                    for (const ExtraMat& e : extraMats) if (e.tex && e.name == name) return true;
                    return false;
                };
                pdraw::Mesh mesh;
                pdraw::build(posed, pdraw::identXform, hasNamed, skinGL != 0, false, mesh);
                auto addBatch = [&](unsigned tex, const std::vector<float>& v) {
                    if (!tex || v.empty()) return;
                    for (TexBatch& b : entBatches) {
                        if (b.tex == tex) {
                            b.verts.insert(b.verts.end(), v.begin(), v.end());
                            return;
                        }
                    }
                    entBatches.push_back({ tex, 0, v });
                };
                addBatch(skinGL, mesh.skin);
                addBatch(atlasTex, mesh.atlas);
                for (auto& kv : mesh.named) addBatch(glTexForName(kv.first), kv.second);
                entSolid.insert(entSolid.end(), mesh.solid.begin(), mesh.solid.end());
                if (!ed.faceMarks.empty()) {
                    const float kSw[6][3] = {
                        { 0.90f, 0.25f, 0.22f }, { 0.25f, 0.75f, 0.35f }, { 0.28f, 0.45f, 0.95f },
                        { 0.95f, 0.78f, 0.18f }, { 0.82f, 0.32f, 0.82f }, { 0.20f, 0.78f, 0.82f },
                    };
                    for (const Editor::FaceMark& m : ed.faceMarks) {
                        const pm::Part* part = nullptr;
                        for (const pm::Part& p : posed)
                            if (pm::partName(p) == m.part) { part = &p; break; }
                        if (!part || m.face < 0 || m.face > 5 || m.color < 0 || m.color > 5) continue;
                        int axis = (m.face < 2) ? 1 : (m.face < 4) ? 0 : 2;
                        float sign = (m.face % 2 == 0) ? 1.0f : -1.0f;
                        int a0 = (axis + 1) % 3;
                        int a1 = (axis + 2) % 3;
                        float h[3] = { part->half.x, part->half.y, part->half.z };
                        Vec3 c[4];
                        for (int k = 0; k < 4; k++) {
                            float u = (k == 1 || k == 2) ? 1.0f : -1.0f;
                            float v = (k >= 2) ? 1.0f : -1.0f;
                            float o[3] = {};
                            o[axis] = sign * h[axis];
                            o[a0] = u * h[a0];
                            o[a1] = v * h[a1];
                            Vec3 local{ o[0], o[1], o[2] };
                            Vec3 nrm{ axis == 0 ? sign : 0.0f, axis == 1 ? sign : 0.0f, axis == 2 ? sign : 0.0f };
                            c[k] = pm::partWorldOffset(*part, local + nrm * 0.02f);
                        }
                        float r = kSw[m.color][0], g = kSw[m.color][1], b = kSw[m.color][2];
                        auto pushTri = [&](const Vec3& a, const Vec3& b0, const Vec3& c0) {
                            float v[3][7] = {
                                { a.x, a.y, a.z, r, g, b, 0.55f },
                                { b0.x, b0.y, b0.z, r, g, b, 0.55f },
                                { c0.x, c0.y, c0.z, r, g, b, 0.55f },
                            };
                            for (auto& e : v) for (int k = 0; k < 7; k++) entSolid.push_back(e[k]);
                        };
                        pushTri(c[0], c[1], c[2]);
                        pushTri(c[0], c[2], c[3]);
                    }
                }
            }
            {
                // Left / Right chooses which palm previews the tool. The other palm stays a marker.
                uint8_t hb = ed.animHoldItem ? ed.animHoldItem : (uint8_t)HAND_AXE;
                auto pushHeld = [&](const hold::Spec& spec) {
                    anim::BoneXform xf{};
                    if (!anim::boneXformOf(viewClip, pose, spec.bone, xf)) return;
                    std::vector<Vertex> hmesh;
                    auto hx = [&](float x, float y, float z) { return hold::pointOnBone(spec, xf, x, y, z); };
                    const mat::Material* tm = mat::toolMaterial(hb);
                    if (tm && tm->model.ok() && !tm->model.cube)
                        mat::emitModelMesh(tm->model, hmesh, hx, blockOf(hb).icon);
                    else {
                        const BlockInfo& info = blockOf(hb);
                        for (int f = 0; f < 6; f++) {
                            const geo::FaceDef& F = geo::kFaces[f];
                            uint8_t tile = (f == 0) ? info.texTop : (f == 1 ? info.texBottom : info.texSide);
                            float u0, v0, u1, v1;
                            tex::tileUV(tile, u0, v0, u1, v1);
                            Vertex vv[4];
                            Vec3 n{ (float)F.n[0], (float)F.n[1], (float)F.n[2] };
                            for (int c = 0; c < 4; c++) {
                                Vec3 p = hx(F.p[c][0], F.p[c][1], F.p[c][2]);
                                vv[c] = { p.x, p.y, p.z,
                                          u0 + (u1 - u0) * F.t[c][0], v0 + (v1 - v0) * F.t[c][1],
                                          n.x, n.y, n.z, F.shade, 1.0f, 1.0f };
                            }
                            hmesh.push_back(vv[0]); hmesh.push_back(vv[1]); hmesh.push_back(vv[2]);
                            hmesh.push_back(vv[0]); hmesh.push_back(vv[2]); hmesh.push_back(vv[3]);
                        }
                    }
                    std::vector<float> tv;
                    tv.reserve(hmesh.size() * 9);
                    for (const Vertex& v : hmesh) {
                        float s = v.faceShade;
                        float row[9] = { v.px, v.py, v.pz, s, s, s, 1.0f, v.u, v.v };
                        for (int i = 0; i < 9; i++) tv.push_back(row[i]);
                    }
                    if (tv.empty()) return;
                    for (TexBatch& b : entBatches) {
                        if (b.tex == atlasTex) {
                            b.verts.insert(b.verts.end(), tv.begin(), tv.end());
                            return;
                        }
                    }
                    entBatches.push_back({ atlasTex, 0, tv });
                };
                {
                    hold::Spec spec = toolSpecAt(viewClip, ed.animClock);
                    spec.grip = anim::evalGrip(viewClip, ed.animClock, spec.grip);
                    pushHeld(spec);
                    anim::BoneXform txf{};
                    if (anim::boneXformOf(viewClip, pose, spec.bone, txf)) {
                        auto seg = [&](float x0, float y0, float z0, float x1, float y1, float z1,
                                       float r, float g, float b) {
                            pline(hold::pointOnBone(spec, txf, x0, y0, z0),
                                  hold::pointOnBone(spec, txf, x1, y1, z1), r, g, b, 1.0f);
                        };
                        auto arrow = [&](float tx, float ty, float tz, float bx, float by, float bz,
                                         float ux, float uy, float uz, float r, float g, float b) {
                            seg(tx, ty, tz, tx - bx + ux, ty - by + uy, tz - bz + uz, r, g, b);
                            seg(tx, ty, tz, tx - bx - ux, ty - by - uy, tz - bz - uz, r, g, b);
                        };
                        const float ox = 0.5f, oy = 0.85f, oz = 0.5f;
                        seg(ox, -1.05f, oz, ox, 1.85f, oz, 1.0f, 0.92f, 0.15f);
                        arrow(ox, 1.85f, oz, 0.0f, 0.22f, 0.0f, 0.16f, 0.0f, 0.0f, 1.0f, 0.92f, 0.15f);
                        arrow(ox, 1.85f, oz, 0.0f, 0.22f, 0.0f, 0.0f, 0.0f, 0.16f, 1.0f, 0.92f, 0.15f);
                        seg(ox - 1.05f, oy, oz, ox + 1.15f, oy, oz, 1.0f, 0.28f, 0.22f);
                        arrow(ox + 1.15f, oy, oz, 0.22f, 0.0f, 0.0f, 0.0f, 0.16f, 0.0f, 1.0f, 0.28f, 0.22f);
                        arrow(ox + 1.15f, oy, oz, 0.22f, 0.0f, 0.0f, 0.0f, 0.0f, 0.16f, 1.0f, 0.28f, 0.22f);
                        seg(ox, oy, oz - 1.45f, ox, oy, oz + 2.15f, 0.35f, 0.55f, 1.0f);
                        arrow(ox, oy, oz + 2.15f, 0.0f, 0.0f, 0.22f, 0.16f, 0.0f, 0.0f, 0.35f, 0.55f, 1.0f);
                        arrow(ox, oy, oz + 2.15f, 0.0f, 0.0f, 0.22f, 0.0f, 0.16f, 0.0f, 0.35f, 0.55f, 1.0f);
                        if (ed.animLockDir || ed.animLockPos) {
                            Vec3 g = hold::pointOnBone(spec, txf, spec.grip.x, spec.grip.y, spec.grip.z);
                            const float s = 0.06f;
                            pline(g + Vec3{ -s, 0, 0 }, g + Vec3{ s, 0, 0 }, 1.0f, 0.85f, 0.2f, 1.0f);
                            pline(g + Vec3{ 0, -s, 0 }, g + Vec3{ 0, s, 0 }, 1.0f, 0.85f, 0.2f, 1.0f);
                            pline(g + Vec3{ 0, 0, -s }, g + Vec3{ 0, 0, s }, 1.0f, 0.85f, 0.2f, 1.0f);
                        }
                    }
                }
                {
                    std::string off = (holdSideName() == "left") ? "right" : "left";
                    const char* offPalm = (off == "left") ? "arm_l_palm" : "arm_r_palm";
                    const char* offHand = (off == "left") ? "arm_l_hand" : "arm_r_hand";
                    anim::BoneXform xf{};
                    bool ok = anim::boneXformOf(editClip, pose, offPalm, xf)
                           || anim::boneXformOf(editClip, pose, offHand, xf);
                    if (ok) {
                        Vec3 o = xf.pivot;
                        float s = 0.055f;
                        Vec3 a{ o.x - s, o.y - s, o.z - s }, b{ o.x + s, o.y - s, o.z - s };
                        Vec3 c{ o.x + s, o.y + s, o.z - s }, d{ o.x - s, o.y + s, o.z - s };
                        Vec3 e{ o.x - s, o.y - s, o.z + s }, f{ o.x + s, o.y - s, o.z + s };
                        Vec3 g{ o.x + s, o.y + s, o.z + s }, h{ o.x - s, o.y + s, o.z + s };
                        pquad(b, c, g, f, 0.06f, 0.12f, 0.32f);
                        pquad(a, e, h, d, 0.04f, 0.08f, 0.22f);
                        pquad(d, c, g, h, 0.08f, 0.16f, 0.42f);
                        pquad(a, b, f, e, 0.03f, 0.06f, 0.16f);
                        pquad(e, f, g, h, 0.05f, 0.10f, 0.28f);
                        pquad(a, d, c, b, 0.05f, 0.09f, 0.24f);
                    }
                }
            }
            for (int i = 0; i < (int)editClip.bones.size() && i < (int)pose.size(); i++) {
                int p = anim::parentIndex(editClip, i);
                if (p >= 0 && p < (int)pose.size())
                    pline(pose[p].pivot, pose[i].pivot, 0.95f, 0.85f, 0.25f, 1);
                Vec3 o = pose[i].pivot;
                float s = (i == ed.animSelBone) ? 0.035f : 0.022f;
                const char* offPrefix = (ed.animHoldSide < 0) ? "arm_r_" : "arm_l_";
                bool offHand = editClip.bones[i].name.rfind(offPrefix, 0) == 0;
                float cr = (i == ed.animSelBone) ? 1.0f : (offHand ? 0.25f : 0.9f);
                float cg = (i == ed.animSelBone) ? 0.45f : (offHand ? 0.82f : 0.9f);
                float cb = (i == ed.animSelBone) ? 0.15f : (offHand ? 0.95f : 0.35f);
                Vec3 a{ o.x - s, o.y - s, o.z - s }, b{ o.x + s, o.y - s, o.z - s };
                Vec3 c{ o.x + s, o.y + s, o.z - s }, d{ o.x - s, o.y + s, o.z - s };
                Vec3 e{ o.x - s, o.y - s, o.z + s }, f{ o.x + s, o.y - s, o.z + s };
                Vec3 g{ o.x + s, o.y + s, o.z + s }, h{ o.x - s, o.y + s, o.z + s };
                pquad(b, c, g, f, cr, cg, cb);
                pquad(a, e, h, d, cr * 0.7f, cg * 0.7f, cb * 0.7f);
                pquad(d, c, g, h, cr, cg, cb);
                pquad(a, b, f, e, cr * 0.55f, cg * 0.55f, cb * 0.55f);
                pquad(e, f, g, h, cr * 0.85f, cg * 0.85f, cb * 0.85f);
                pquad(a, d, c, b, cr * 0.65f, cg * 0.65f, cb * 0.65f);
            }
            {
                Vec3 gz;
                if (animGizmoCenter(gz)) {
                    auto pushGz = [&](float x, float y, float r, float gcol, float b, float a) {
                        gizmoOverlay.push_back(x); gizmoOverlay.push_back(y); gizmoOverlay.push_back(0);
                        gizmoOverlay.push_back(r); gizmoOverlay.push_back(gcol); gizmoOverlay.push_back(b); gizmoOverlay.push_back(a);
                    };
                    auto gzLine2 = [&](float x0, float y0, float x1, float y1, float r, float gcol, float b) {
                        pushGz(x0, y0, r, gcol, b, 1); pushGz(x1, y1, r, gcol, b, 1);
                    };
                    auto gzHead = [&](float tx, float ty, float fx, float fy, float sz, float r, float gcol, float b) {
                        float dx = tx - fx, dy = ty - fy;
                        float len = std::hypot(dx, dy);
                        if (len < 1.0f) return;
                        dx /= len; dy /= len;
                        float px = -dy, py = dx;
                        float bx = tx - dx * sz, by = ty - dy * sz;
                        gzLine2(tx, ty, bx + px * sz * 0.55f, by + py * sz * 0.55f, r, gcol, b);
                        gzLine2(tx, ty, bx - px * sz * 0.55f, by - py * sz * 0.55f, r, gcol, b);
                        gzLine2(bx + px * sz * 0.55f, by + py * sz * 0.55f, bx - px * sz * 0.55f, by - py * sz * 0.55f, r, gcol, b);
                    };
                    float L = animGizmoSize();
                    float Rarc = L * 0.72f;
                    const float col[3][3] = { {1.0f,0.28f,0.28f},{0.28f,0.92f,0.32f},{0.32f,0.45f,1.0f} };
                    int hov = ed.gizmoHover;
                    bool drawTrans = !animBonePick && (ed.animGrasp || ed.animTool == 0);
                    bool drawRot = !animBonePick && !ed.animGrasp && (ed.animTool == 1);
                    Vec3 axes[3] = { kAxis[0], kAxis[1], kAxis[2] };
                    if (ed.animGrasp) itemModelAxes(axes);
                    for (int a = 0; a < 3; a++) {
                        bool hotT = (hov == 1 + a) || (hov == 10 + a * 2) || (hov == 11 + a * 2);
                        bool hotR = (hov == 4 + a) || (hov == 20 + a * 2) || (hov == 21 + a * 2);
                        if (drawTrans) {
                            float r = col[a][0], gc = col[a][1], b = col[a][2];
                            if (hotT) { r = 1; gc = 1; b = 0.35f; }
                            Vec3 p0 = { gz.x - axes[a].x * L, gz.y - axes[a].y * L, gz.z - axes[a].z * L };
                            Vec3 p1 = { gz.x + axes[a].x * L, gz.y + axes[a].y * L, gz.z + axes[a].z * L };
                            float sx0, sy0, sx1, sy1;
                            if (projectAnim(p0.x, p0.y, p0.z, sx0, sy0) && projectAnim(p1.x, p1.y, p1.z, sx1, sy1)) {
                                gzLine2(sx0, sy0, sx1, sy1, r, gc, b);
                                gzHead(sx1, sy1, sx0, sy0, hotT ? 16.0f : 13.0f, r, gc, b);
                                gzHead(sx0, sy0, sx1, sy1, hotT ? 16.0f : 13.0f, r, gc, b);
                            }
                        }
                        if (!drawRot) continue;
                        float rr = hotR ? 1.0f : col[a][0], rg = hotR ? 1.0f : col[a][1], rb = hotR ? 0.35f : col[a][2];
                        Vec3 u, vv; rotBasis(a, u, vv);
                        const int segs = 28;
                        float a0 = -2.3f, a1 = 2.3f;
                        float px, py;
                        bool prevOk = false;
                        for (int i = 0; i <= segs; i++) {
                            float ang = a0 + (a1 - a0) * (float)i / (float)segs;
                            Vec3 q = { gz.x + (u.x * std::cos(ang) + vv.x * std::sin(ang)) * Rarc,
                                       gz.y + (u.y * std::cos(ang) + vv.y * std::sin(ang)) * Rarc,
                                       gz.z + (u.z * std::cos(ang) + vv.z * std::sin(ang)) * Rarc };
                            float sx, sy;
                            bool ok = projectAnim(q.x, q.y, q.z, sx, sy);
                            if (ok && prevOk) gzLine2(px, py, sx, sy, rr, rg, rb);
                            if (ok) { px = sx; py = sy; }
                            prevOk = ok;
                        }
                        auto rotHead = [&](float ang, float dir) {
                            Vec3 hp = { gz.x + (u.x * std::cos(ang) + vv.x * std::sin(ang)) * Rarc,
                                        gz.y + (u.y * std::cos(ang) + vv.y * std::sin(ang)) * Rarc,
                                        gz.z + (u.z * std::cos(ang) + vv.z * std::sin(ang)) * Rarc };
                            float dang = 0.22f * dir;
                            Vec3 from = { gz.x + (u.x * std::cos(ang - dang) + vv.x * std::sin(ang - dang)) * Rarc,
                                          gz.y + (u.y * std::cos(ang - dang) + vv.y * std::sin(ang - dang)) * Rarc,
                                          gz.z + (u.z * std::cos(ang - dang) + vv.z * std::sin(ang - dang)) * Rarc };
                            float hx, hy, fx, fy;
                            if (projectAnim(hp.x, hp.y, hp.z, hx, hy) && projectAnim(from.x, from.y, from.z, fx, fy))
                                gzHead(hx, hy, fx, fy, hotR ? 16.0f : 13.0f, rr, rg, rb);
                        };
                        rotHead(a0, -1.0f);
                        rotHead(a1, 1.0f);
                    }
                }
            }
            if (ed.animTool == 3 && !animBonePick && !ed.animHoldEdit && !ed.animGrasp) {
                Vec3 g, axis, u, v;
                if (boneRollFrame(g, axis, u, v)) {
                    Vec3 restO, restD;
                    if (!anim::boneSegmentAxis(editClip, ed.animSelBone, restO, restD)) restD = axis;
                    Vec3 wo = pose[ed.animSelBone].pivot;
                    int ch = anim::distalBone(editClip, ed.animSelBone);
                    Vec3 we = (ch >= 0 && ch < (int)pose.size()) ? pose[ch].pivot : (wo + anim::mul9(pose[ed.animSelBone].R, restD) * 0.25f);
                    pline(wo, we, 1.0f, 0.55f, 0.12f, 1.0f);
                    auto pushGz = [&](float x, float y, float r, float gcol, float b, float a) {
                        gizmoOverlay.push_back(x); gizmoOverlay.push_back(y); gizmoOverlay.push_back(0);
                        gizmoOverlay.push_back(r); gizmoOverlay.push_back(gcol); gizmoOverlay.push_back(b); gizmoOverlay.push_back(a);
                    };
                    auto gzLine2 = [&](float x0, float y0, float x1, float y1, float r, float gcol, float b) {
                        pushGz(x0, y0, r, gcol, b, 1); pushGz(x1, y1, r, gcol, b, 1);
                    };
                    float Rarc = animGizmoSize() * 0.95f;
                    bool hot = ed.gizmoHover == 30;
                    float rr = hot ? 1.0f : 1.0f, rg = hot ? 0.85f : 0.55f, rb = hot ? 0.2f : 0.12f;
                    const int segs = 32;
                    float px, py;
                    bool prevOk = false;
                    for (int i = 0; i <= segs; i++) {
                        float ang = 6.2831853f * (float)i / (float)segs;
                        Vec3 q = g + (u * std::cos(ang) + v * std::sin(ang)) * Rarc;
                        float sx, sy;
                        bool ok = projectAnim(q.x, q.y, q.z, sx, sy);
                        if (ok && prevOk) gzLine2(px, py, sx, sy, rr, rg, rb);
                        if (ok) { px = sx; py = sy; }
                        prevOk = ok;
                    }
                }
            }
        } else if (!ed.modelMode && !ed.entityMode) {
            // ---- toolbar (top): visual tool buttons ----
            const float tbY = 8.0f, tbS = 40.0f, tbGap = 6.0f;
            ed.toolHover = -1;
            for (int i = 0; i < 13; i++) {
                float bx = 8.0f + i * (tbS + tbGap);
                bool hov = mx >= bx && mx < bx + tbS && my >= tbY && my < tbY + tbS;
                if (hov) ed.toolHover = i;
                ed.tbRect[i][0] = bx; ed.tbRect[i][1] = tbY; ed.tbRect[i][2] = tbS; ed.tbRect[i][3] = tbS;
                bool active = (i == 3) ? ed.picker : (i == 1) ? (ed.tool == 1 || ed.brushMod) : (i == ed.tool && i < 9);
                rect(bx, tbY, tbS, tbS, active ? 0.30f : 0.15f, active ? 0.45f : 0.15f, active ? 0.18f : 0.15f, 1);
                if (hov) rect(bx, tbY, tbS, tbS, 0.35f, 0.35f, 0.35f, 1);
                float bc = active ? 1.0f : 0.4f;
                rect(bx, tbY, tbS, 1, bc, active ? 0.9f : bc, 0.2f, 1);
                rect(bx, tbY + tbS - 1, tbS, 1, bc, active ? 0.9f : bc, 0.2f, 1);
                rect(bx, tbY, 1, tbS, bc, active ? 0.9f : bc, 0.2f, 1);
                rect(bx + tbS - 1, tbY, 1, tbS, bc, active ? 0.9f : bc, 0.2f, 1);
            }

            // ---- tile selection panel (left) ----
            const float panelW = 172.0f, thumb = 36.0f;
            const int cols = 4;
            ed.hoverTile = -1;
            for (int t = 0; t < TEX_COUNT; t++) {
                int cx = t % cols, cy = t / cols;
                float bx = 8.0f + cx * (thumb + 6.0f);
                float by = 60.0f + cy * (thumb + 6.0f);
                bool hov = mx >= bx && mx < bx + thumb && my >= by && my < by + thumb;
                if (hov) ed.hoverTile = t;
                rect(bx - 1, by - 1, thumb + 2, thumb + 2, 0.25f, 0.25f, 0.25f, 1.0f);
                if (t == ed.tile) { // selected: border only
                    rect(bx - 2, by - 2, thumb + 4, 2, 1.0f, 0.9f, 0.2f, 1);
                    rect(bx - 2, by + thumb, thumb + 4, 2, 1.0f, 0.9f, 0.2f, 1);
                    rect(bx - 2, by - 2, 2, thumb + 4, 1.0f, 0.9f, 0.2f, 1);
                    rect(bx + thumb, by - 2, 2, thumb + 4, 1.0f, 0.9f, 0.2f, 1);
                }
                mat::Image& ti = mat::g_tileImages[t];
                const int TH = 16;
                for (int py = 0; py < TH; py++) for (int pxx = 0; pxx < TH; pxx++) {
                    int sx = (int)((float)pxx * ((float)tex::TILE / TH));
                    int sy = (int)((float)py * ((float)tex::TILE / TH));
                    size_t i = ((size_t)sy * tex::TILE + sx) * 4;
                    rect(bx + pxx * (thumb / TH), by + py * (thumb / TH), thumb / TH + 0.5f, thumb / TH + 0.5f,
                         ti.rgba[i + 0] / 255.0f, ti.rgba[i + 1] / 255.0f, ti.rgba[i + 2] / 255.0f, ti.rgba[i + 3] / 255.0f);
                }
            }

            // ---- texture editor ----
            mat::Image& img = mat::g_tileImages[ed.tile];
            const int T = tex::TILE;
            const float rightW = 200.0f; // reserve space for the right color picker
            const float centerW = (float)g_winW - panelW - rightW - 40.0f;
            const float baseTs = std::min(centerW, (float)g_winH - 180.0f);
            float ts = baseTs * ed.canvasZoom;
            if (ts < 64.0f) ts = 64.0f;
            if (ts > baseTs * 3.0f) ts = baseTs * 3.0f;
            const float x0 = panelW + 20.0f + (centerW - ts) * 0.5f;
            const float y0 = 60.0f;
            const float px = ts / T;
            ed.tileX = x0; ed.tileY = y0; ed.tileSize = ts;

            for (int py = 0; py < T; py++) for (int pxx = 0; pxx < T; pxx++) {
                size_t i = ((size_t)py * T + pxx) * 4;
                bool check = (((pxx/4)+(py/4)) & 1) != 0;
                float cr = check ? 0.45f : 0.6f;
                rect(x0+pxx*px, y0+py*px, px, px, cr, cr, cr, 1.0f);
                rect(x0+pxx*px, y0+py*px, px, px,
                     img.rgba[i+0]/255.0f, img.rgba[i+1]/255.0f, img.rgba[i+2]/255.0f, img.rgba[i+3]/255.0f);
            }
            if (ed.hoverX >= 0) {
                // highlight matches the brush size (centered on the hovered pixel)
                int half = ed.brushSize / 2;
                int ax = ed.hoverX - half, ay = ed.hoverY - half;
                float hx = x0 + ax * px, hy = y0 + ay * px, hw = ed.brushSize * px;
                rect(hx, hy, hw, 2, 1, 1, 0, 1);
                rect(hx, hy, 2, hw, 1, 1, 0, 1);
                rect(hx + hw - 2, hy, 2, hw, 1, 1, 0, 1);
                rect(hx, hy + hw - 2, hw, 2, 1, 1, 0, 1);
            }
            // shape drag preview (outline of the drag region)
            if (ed.dragging && ed.tool >= 5 && ed.tool <= 8) {
                int ax0 = std::min(ed.dragX0, ed.dragX1), ay0 = std::min(ed.dragY0, ed.dragY1);
                int ax1 = std::max(ed.dragX0, ed.dragX1), ay1 = std::max(ed.dragY0, ed.dragY1);
                float sx0 = x0 + ax0 * px, sy0 = y0 + ay0 * px;
                float sx1 = x0 + (ax1 + 1) * px, sy1 = y0 + (ay1 + 1) * px;
                if (ed.tool == 7) { // line: draw a thin quad along the segment
                    float x0p = sx0 + px * 0.5f, y0p = sy0 + px * 0.5f;
                    float x1p = sx1 - px * 0.5f, y1p = sy1 - px * 0.5f;
                    float dx = x1p - x0p, dy = y1p - y0p;
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.001f) {
                        float nx = -dy / len * 1.5f, ny = dx / len * 1.5f;
                        float c[4][2] = { {x0p + nx, y0p + ny}, {x0p - nx, y0p - ny}, {x1p - nx, y1p - ny}, {x1p + nx, y1p + ny} };
                        float v[6][7] = {
                            {c[0][0], c[0][1], 0, 1, 1, 0, 1}, {c[1][0], c[1][1], 0, 1, 1, 0, 1}, {c[2][0], c[2][1], 0, 1, 1, 0, 1},
                            {c[0][0], c[0][1], 0, 1, 1, 0, 1}, {c[2][0], c[2][1], 0, 1, 1, 0, 1}, {c[3][0], c[3][1], 0, 1, 1, 0, 1},
                        };
                        for (auto& e : v) for (int i = 0; i < 7; i++) verts.push_back(e[i]);
                    }
                } else { // rect / circle bounding box
                    rect(sx0, sy0, sx1 - sx0, 2, 1, 1, 0, 1);
                    rect(sx0, sy1 - 2, sx1 - sx0, 2, 1, 1, 0, 1);
                    rect(sx0, sy0, 2, sy1 - sy0, 1, 1, 0, 1);
                    rect(sx1 - 2, sy0, 2, sy1 - sy0, 1, 1, 0, 1);
                }
            }
            // palette
            static const float kPal[8][3] = {
                {0,1,0},{0.55f,0.3f,0.1f},{0.5f,0.5f,0.5f},{1,1,1},{0,0,0},{1,0,0},{0,0.3f,1},{1,0.85f,0},
            };
            const float pc = 34, py0 = y0 + ts + 20;
            ed.paletteHover = -1;
            for (int i = 0; i < 8; i++) {
                float cx = x0 + i*(pc+8);
                bool hov = mx >= cx && mx < cx+pc && my >= py0 && my < py0+pc;
                if (hov) ed.paletteHover = i;
                rect(cx, py0, pc, pc, kPal[i][0], kPal[i][1], kPal[i][2], 1);
                if (hov) rect(cx-3, py0-3, pc+6, pc+6, 1, 1, 1, 1);
            }
            {
                float cx = x0 + 8*(pc+8);
                bool hov = mx >= cx && mx < cx+pc && my >= py0 && my < py0+pc;
                if (hov) ed.paletteHover = 8;
                rect(cx, py0, pc, pc, 0.3f, 0.3f, 0.3f, 1);
                rect(cx+6, py0+pc*0.5f-1, pc-12, 2, 1, 0, 0, 1);
                if (hov) rect(cx-3, py0-3, pc+6, pc+6, 1, 1, 1, 1);
            }
            // palette click sets color
            if (lmb && !prevLmb && ed.paletteHover >= 0) {
                if (ed.paletteHover < 8) { ed.r = kPal[ed.paletteHover][0]; ed.g = kPal[ed.paletteHover][1]; ed.b = kPal[ed.paletteHover][2]; ed.a = 1; }
                else ed.a = 0;
            }

            // ---- recent colors (FIFO) ----
            const float rcY = py0 + 50.0f, rcS = 26.0f;
            for (size_t k = 0; k < ed.recent.size(); k++) {
                float rx = x0 + k * (rcS + 4);
                rect(rx, rcY, rcS, rcS, ed.recent[k].r / 255.0f, ed.recent[k].g / 255.0f, ed.recent[k].b / 255.0f, ed.recent[k].a / 255.0f);
                if (lmb && !prevLmb && mx >= rx && mx < rx + rcS && my >= rcY && my < rcY + rcS) {
                    ed.r = ed.recent[k].r / 255.0f; ed.g = ed.recent[k].g / 255.0f;
                    ed.b = ed.recent[k].b / 255.0f; ed.a = ed.recent[k].a / 255.0f;
                }
            }

            // ---- color picker (right): SV rectangle + hue ring ----
            float ch, cs, cv;
            rgbToHsv(ed.r, ed.g, ed.b, ch, cs, cv);
            const float svW = 160.0f;
            float svX = (float)g_winW - svW - 30.0f, svY = 80.0f;
            ed.svRect[0] = svX; ed.svRect[1] = svY; ed.svRect[2] = svW; ed.svRect[3] = svW;
            for (int j = 0; j < 16; j++) for (int i = 0; i < 16; i++) {
                float s = (float)i / 15.0f, v = 1.0f - (float)j / 15.0f;
                float rr, gg, bb; hsvToRgb(ch, s, v, rr, gg, bb);
                rect(svX + i * (svW / 16), svY + j * (svW / 16), svW / 16 + 0.5f, svW / 16 + 0.5f, rr, gg, bb, 1);
            }
            rect(svX + cs * svW - 2, svY + (1 - cv) * svW - 2, 4, 4, 1, 1, 1, 1);

            ed.hueCX = (float)g_winW - 110.0f;   // aligned with the SV rect center
            ed.hueCY = svY + svW + 95.0f;
            ed.hueRO = 72.0f; ed.hueRI = 48.0f;
            const int HS = 48;
            for (int k = 0; k < HS; k++) {
                float a0 = (float)k / HS * 6.2831853f, a1 = (float)(k + 1) / HS * 6.2831853f;
                float rr, gg, bb; hsvToRgb((float)k / HS, 1, 1, rr, gg, bb);
                float x0 = ed.hueCX + std::cos(a0) * ed.hueRI, y0 = ed.hueCY + std::sin(a0) * ed.hueRI;
                float x1 = ed.hueCX + std::cos(a1) * ed.hueRI, y1 = ed.hueCY + std::sin(a1) * ed.hueRI;
                float x2 = ed.hueCX + std::cos(a1) * ed.hueRO, y2 = ed.hueCY + std::sin(a1) * ed.hueRO;
                float x3 = ed.hueCX + std::cos(a0) * ed.hueRO, y3 = ed.hueCY + std::sin(a0) * ed.hueRO;
                float v6[6][7] = { {x0,y0,0,rr,gg,bb,1},{x1,y1,0,rr,gg,bb,1},{x2,y2,0,rr,gg,bb,1},
                                    {x0,y0,0,rr,gg,bb,1},{x2,y2,0,rr,gg,bb,1},{x3,y3,0,rr,gg,bb,1} };
                for (auto& e : v6) for (int ii = 0; ii < 7; ii++) verts.push_back(e[ii]);
            }
            {
                float ca = ch * 6.2831853f;
                float hx = ed.hueCX + std::cos(ca) * (ed.hueRI + ed.hueRO) * 0.5f;
                float hy = ed.hueCY + std::sin(ca) * (ed.hueRI + ed.hueRO) * 0.5f;
                rect(hx - 3, hy - 3, 6, 6, 1, 1, 1, 1);
            }

            // ---- alpha (transparency) slider, below the hue ring ----
            const float rcx = (float)g_winW - 110.0f;
            const float alX = rcx - 80.0f, alY = ed.hueCY + ed.hueRO + 24.0f, alW = 160.0f, alH = 14.0f;
            ed.alphaRect[0] = alX; ed.alphaRect[1] = alY; ed.alphaRect[2] = alW; ed.alphaRect[3] = alH;
            for (int i = 0; i < 16; i++) {
                bool ck = (i % 2) != 0;
                rect(alX + i * (alW / 16), alY, alW / 16, alH, ck ? 0.45f : 0.65f, ck ? 0.45f : 0.65f, ck ? 0.45f : 0.65f, 1);
            }
            rect(alX, alY, alW * ed.a, alH, 1.0f, 1.0f, 1.0f, 0.85f);
            rect(alX + alW * ed.a - 2, alY - 3, 4, alH + 6, 1, 1, 1, 1);
            if (lmb && mx >= alX - 6 && mx < alX + alW + 6 && my >= alY - 6 && my < alY + alH + 6) {
                ed.a = (mx - alX) / alW;
                if (ed.a < 0) ed.a = 0; if (ed.a > 1) ed.a = 1;
            }

            // ---- brush palette matrix (16 blocks), only for the multi-color brush ----
            if (ed.tool == 1) {
                ensureBrushMat();
                const float bmS = 34.0f;
                const float bmX = rcx - 80.0f, bmY = alY + 40.0f;
                const float mw = bmS * 4 + 12;
                ed.brushMat[0] = bmX; ed.brushMat[1] = bmY; ed.brushMat[2] = mw; ed.brushMat[3] = mw;
                auto drawChecker = [&](float x, float y, float s) {
                    const int n = 4;
                    float cs = s / (float)n;
                    for (int cy = 0; cy < n; cy++) for (int cx = 0; cx < n; cx++) {
                        bool ck = ((cx + cy) & 1) != 0;
                        float g = ck ? 0.55f : 0.72f;
                        rect(x + cx * cs, y + cy * cs, cs + 0.4f, cs + 0.4f, g, g, g, 1);
                    }
                };
                for (int k = 0; k < 16; k++) {
                    int cxi = k % 4, cyi = k / 4;
                    float bx2 = bmX + cxi * (bmS + 4), by2 = bmY + cyi * (bmS + 4);
                    bool set = ed.brushSet[k] != 0;
                    bool sel = ed.brushSel[k] != 0;
                    if (set) {
                        drawChecker(bx2, by2, bmS);
                        rect(bx2, by2, bmS, bmS, ed.brushColors[k].r / 255.0f, ed.brushColors[k].g / 255.0f, ed.brushColors[k].b / 255.0f, ed.brushColors[k].a / 255.0f);
                    } else if (sel) {
                        drawChecker(bx2, by2, bmS);
                        rect(bx2, by2, bmS, bmS, ed.r, ed.g, ed.b, ed.a);
                    } else {
                        rect(bx2, by2, bmS, bmS, 0.12f, 0.12f, 0.12f, 1);
                    }
                    if (sel) {
                        rect(bx2 - 2, by2 - 2, bmS + 4, 2, 1, 1, 0, 1);
                        rect(bx2 - 2, by2 + bmS, bmS + 4, 2, 1, 1, 0, 1);
                        rect(bx2 - 2, by2 - 2, 2, bmS + 4, 1, 1, 0, 1);
                        rect(bx2 + bmS, by2 - 2, 2, bmS + 4, 1, 1, 0, 1);
                    }
                    if (lmb && !prevLmb && mx >= bx2 && mx < bx2 + bmS && my >= by2 && my < by2 + bmS)
                        ed.brushSel[k] ^= 1;
                }
                const float btnY = bmY + mw + 10.0f;
                const float ckS = 16.0f, ckX = bmX, ckY = btnY + 4.0f;
                const float ckHitW = 58.0f; // box + "A=0" label, stays left of the apply/clear buttons
                bool ckHov = mx >= ckX && mx < ckX + ckHitW && my >= ckY && my < ckY + ckS;
                rect(ckX, ckY, ckS, ckS, 0.18f, 0.18f, 0.18f, 1);
                rect(ckX, ckY, ckS, 1, 0.7f, 0.7f, 0.7f, 1);
                rect(ckX, ckY + ckS - 1, ckS, 1, 0.7f, 0.7f, 0.7f, 1);
                rect(ckX, ckY, 1, ckS, 0.7f, 0.7f, 0.7f, 1);
                rect(ckX + ckS - 1, ckY, 1, ckS, 0.7f, 0.7f, 0.7f, 1);
                if (ed.pickAlpha0) {
                    rect(ckX + 3, ckY + 3, ckS - 6, ckS - 6, 0.35f, 0.85f, 0.4f, 1);
                }
                if (ckHov) rect(ckX - 1, ckY - 1, ckS + 2, ckS + 2, 1, 1, 1, 1);
                if (lmb && !prevLmb && ckHov) ed.pickAlpha0 = !ed.pickAlpha0;

                bool anySel = false;
                for (int k = 0; k < 16; k++) if (ed.brushSel[k]) { anySel = true; break; }
                if (anySel) {
                    const float mRight = bmX + mw;
                    float crx = mRight - 52, chkx = mRight - 24;
                    bool crsHov = mx >= crx && mx < crx + 24 && my >= btnY && my < btnY + 24;
                    bool chkHov = mx >= chkx && mx < chkx + 24 && my >= btnY && my < btnY + 24;
                    rect(crx, btnY, 24, 24, 0.7f, 0.2f, 0.2f, 1);
                    if (crsHov) rect(crx - 1, btnY - 1, 26, 26, 1, 1, 1, 1);
                    rect(chkx, btnY, 24, 24, 0.2f, 0.7f, 0.2f, 1);
                    if (chkHov) rect(chkx - 1, btnY - 1, 26, 26, 1, 1, 1, 1);
                    if (lmb && !prevLmb && chkHov) {
                        Color4 c = { (uint8_t)(ed.r * 255), (uint8_t)(ed.g * 255), (uint8_t)(ed.b * 255), (uint8_t)(ed.a * 255) };
                        for (int k = 0; k < 16; k++) if (ed.brushSel[k]) {
                            ed.brushColors[k] = c;
                            ed.brushSet[k] = 1;
                        }
                        for (uint8_t& s : ed.brushSel) s = 0;
                    }
                    if (lmb && !prevLmb && crsHov) {
                        for (int k = 0; k < 16; k++) if (ed.brushSel[k]) {
                            ed.brushColors[k] = Color4{};
                            ed.brushSet[k] = 0;
                        }
                        compactBrushMat();
                    }
                }
            }
        } else if (ed.entityMode) {
            // ---- entity model editor (player cuboids) ----
            if (entIsCloth) {
                ed.entSkinView = false;
                uint64_t sig = clothGeomSig();
                if (sig != clothSheetSig) rebuildClothSheet();
            }
            mvp3d = leftMvp;
            ensureEntMark();

            std::vector<float>* dstSolid = &entSolid;
            std::vector<TexBatch>* dstBatches = &entBatches;
            std::vector<float>* lineDst = &lineVerts;
            auto pushTri = [&](const Vec3& a, const Vec3& b, const Vec3& c, float r, float g, float bl, float a0) {
                float v[3][7] = {
                    { a.x, a.y, a.z, r, g, bl, a0 }, { b.x, b.y, b.z, r, g, bl, a0 }, { c.x, c.y, c.z, r, g, bl, a0 }
                };
                for (auto& e : v) for (int i = 0; i < 7; i++) dstSolid->push_back(e[i]);
            };
            auto pushQuad = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float r, float g, float bl) {
                pushTri(a, b, c, r, g, bl, 1);
                pushTri(a, c, d, r, g, bl, 1);
            };
            std::vector<float>* texDst = &entTex;
            auto batchVerts = [&](unsigned t) -> std::vector<float>& {
                for (TexBatch& b : *dstBatches) if (b.tex == t) return b.verts;
                dstBatches->push_back({});
                dstBatches->back().tex = t;
                return dstBatches->back().verts;
            };
            auto pushTexTri = [&](const Vec3& a, const Vec3& b, const Vec3& c,
                                  float ua, float va, float ub, float vb, float uc, float vc,
                                  float r, float g, float bl) {
                float v[3][9] = {
                    { a.x, a.y, a.z, r, g, bl, 1, ua, va },
                    { b.x, b.y, b.z, r, g, bl, 1, ub, vb },
                    { c.x, c.y, c.z, r, g, bl, 1, uc, vc },
                };
                for (auto& e : v) for (int i = 0; i < 9; i++) texDst->push_back(e[i]);
            };
            auto pushTexQuad = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                                   float ua, float va, float ub, float vb, float uc, float vc, float ud, float vd,
                                   float r, float g, float bl) {
                pushTexTri(a, b, c, ua, va, ub, vb, uc, vc, r, g, bl);
                pushTexTri(a, c, d, ua, va, uc, vc, ud, vd, r, g, bl);
            };
            auto eline = [&](float x0, float y0, float z0, float x1, float y1, float z1, float r, float g, float b, float a) {
                float v[2][7] = { {x0,y0,z0,r,g,b,a},{x1,y1,z1,r,g,b,a} };
                for (auto& e : v) for (int i = 0; i < 7; i++) lineDst->push_back(e[i]);
            };

            const float kFaceSh[6] = { 0.82f, 0.62f, 1.0f, 0.48f, 0.90f, 0.70f };
            auto emitPart = [&](int qi) {
                const pm::Part& p = ed.entityParts[qi];
                bool sel = (qi < (int)entMark.size() && entMark[qi]);
                bool prim = (qi == ed.entitySel);
                auto corner = [&](float sx, float sy, float sz) {
                    return pm::partWorldOffset(p, { sx * p.half.x, sy * p.half.y, sz * p.half.z });
                };
                Vec3 wc[8] = {
                    corner(-1, -1, -1), corner(1, -1, -1), corner(1, -1, 1), corner(-1, -1, 1),
                    corner(-1, 1, -1), corner(1, 1, -1), corner(1, 1, 1), corner(-1, 1, 1)
                };
                if (pm::isHairPart(p)) {
                    auto emitHairFace = [&](int f, bool tex, float tu0, float tv0, float tu1, float tv1) {
                        std::vector<pm::HairRect> rs;
                        pm::hairFaceRemainders(p, f, ed.entityParts, rs);
                        float s = kFaceSh[f] * (prim ? 1.08f : (sel ? 1.04f : 1.0f));
                        float cr = std::min(1.0f, p.color.x * s);
                        float cg = std::min(1.0f, p.color.y * s);
                        float cb = std::min(1.0f, p.color.z * s);
                        for (const pm::HairRect& hr : rs) {
                            Vec3 q[4];
                            pm::hairFaceCorners(p, f, hr, q);
                            if (tex) pushTexQuad(q[0], q[1], q[2], q[3], tu0, tv1, tu1, tv1, tu1, tv0, tu0, tv0, cr, cg, cb);
                            else pushQuad(q[0], q[1], q[2], q[3], cr, cg, cb);
                        }
                    };
                    if (!p.tex.empty() && !pm::isCutoutOverlay(p.tex)) {
                        unsigned ttex = glTexForName(p.tex);
                        int ti = mat::tileIndex(p.tex.c_str());
                        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
                        if (ti >= 0 && ttex == atlasTex) tex::tileUV(ti, u0, v0, u1, v1);
                        pm::applyPartUvFlip(p, u0, v0, u1, v1);
                        texDst = &batchVerts(ttex);
                        for (int f = 0; f < 6; f++) emitHairFace(f, true, u0, v0, u1, v1);
                    } else {
                        for (int f = 0; f < 6; f++) emitHairFace(f, false, 0, 0, 1, 1);
                    }
                } else if (pm::isHairCardPart(p)) {
                    Vec3 q[4];
                    pm::texQuadLocal(p, q);
                    float cr = p.color.x, cg = p.color.y, cb = p.color.z;
                    if (p.tex.empty()) {
                        pushQuad(q[0], q[1], q[2], q[3], cr, cg, cb);
                        pushQuad(q[1], q[0], q[3], q[2], cr, cg, cb);
                    } else {
                        unsigned ttex = glTexForName(p.tex);
                        int ti = mat::tileIndex(p.tex.c_str());
                        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                        if (ti >= 0 && ttex == atlasTex) tex::tileUV(ti, u0, v0, u1, v1);
                        pm::applyPartUvFlip(p, u0, v0, u1, v1);
                        texDst = &batchVerts(ttex);
                        pushTexQuad(q[0], q[1], q[2], q[3], u0, v1, u1, v1, u1, v0, u0, v0, cr, cg, cb);
                        pushTexQuad(q[1], q[0], q[3], q[2], u1, v1, u0, v1, u0, v0, u1, v0, cr, cg, cb);
                    }
                } else if (pm::isDecalPart(p)) {
                    unsigned ttex = glTexForName(p.tex);
                    int ti = mat::tileIndex(p.tex.c_str());
                    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                    if (ti >= 0 && ttex == atlasTex) tex::tileUV(ti, u0, v0, u1, v1);
                    if (p.side < 0) { float t = u0; u0 = u1; u1 = t; }
                    pm::applyPartUvFlip(p, u0, v0, u1, v1);
                    texDst = &batchVerts(ttex);
                    Vec3 q[4];
                    pm::texQuadLocal(p, q);
                    float cr = p.color.x, cg = p.color.y, cb = p.color.z;
                    pushTexQuad(q[0], q[1], q[2], q[3], u0, v1, u1, v1, u1, v0, u0, v0, cr, cg, cb);
                    pushTexQuad(q[1], q[0], q[3], q[2], u1, v1, u0, v1, u0, v0, u1, v0, cr, cg, cb);
                } else if (!p.tex.empty() && !pm::isCutoutOverlay(p.tex)) {
                    unsigned ttex = glTexForName(p.tex);
                    int ti = mat::tileIndex(p.tex.c_str());
                    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                    if (ti >= 0 && ttex == atlasTex) tex::tileUV(ti, u0, v0, u1, v1);
                    pm::applyPartUvFlip(p, u0, v0, u1, v1);
                    texDst = &batchVerts(ttex);
                    for (int f = 0; f < 6; f++) {
                        Vec3 q[4];
                        pm::cuboidFaceCorners(p, f, q);
                        float s = kFaceSh[f] * (prim ? 1.08f : (sel ? 1.04f : 1.0f));
                        float cr = std::min(1.0f, p.color.x * s);
                        float cg = std::min(1.0f, p.color.y * s);
                        float cb = std::min(1.0f, p.color.z * s);
                        pushTexQuad(q[0], q[1], q[2], q[3], u0, v1, u1, v1, u1, v0, u0, v0, cr, cg, cb);
                    }
                } else if (pm::partHasBox(p) && skinGL && entSheetW > 0 && entSheetH > 0) {
                    texDst = &batchVerts(skinGL);
                    for (int f = 0; f < 6; f++) {
                        float u0, v0, u1, v1;
                        pm::modelBoxUV(p, f, entSheetW, entSheetH, u0, v0, u1, v1);
                        float s = kFaceSh[f] * (prim ? 1.08f : (sel ? 1.04f : 1.0f));
                        std::vector<pm::HairRect> rs;
                        pm::hairFaceRemainders(p, f, ed.entityParts, rs);
                        for (const pm::HairRect& hr : rs) {
                            Vec3 q[4];
                            pm::hairFaceCorners(p, f, hr, q);
                            pushTexQuad(q[0], q[1], q[2], q[3], u0, v1, u1, v1, u1, v0, u0, v0, s, s, s);
                        }
                    }
                } else if (pm::isGridHairPart(p)) {
                    for (int f = 0; f < 6; f++) {
                        std::vector<pm::HairRect> rs;
                        pm::hairFaceRemainders(p, f, ed.entityParts, rs);
                        float s = kFaceSh[f] * (prim ? 1.08f : (sel ? 1.04f : 1.0f));
                        float cr = std::min(1.0f, p.color.x * s);
                        float cg = std::min(1.0f, p.color.y * s);
                        float cb = std::min(1.0f, p.color.z * s);
                        for (const pm::HairRect& hr : rs) {
                            Vec3 q[4];
                            pm::hairFaceCorners(p, f, hr, q);
                            pushQuad(q[0], q[1], q[2], q[3], cr, cg, cb);
                        }
                    }
                } else if (p.kind == "cloth" || entIsCloth || entSkin.empty() || !skinGL) {
                    Vec3* c = wc;
                    auto sh = [&](int f) {
                        float s = kFaceSh[f] * (prim ? 1.08f : (sel ? 1.04f : 1.0f));
                        return Vec3{ std::min(1.0f, p.color.x * s), std::min(1.0f, p.color.y * s), std::min(1.0f, p.color.z * s) };
                    };
                    Vec3 sx = sh(0), sxn = sh(1), sy = sh(2), syn = sh(3), sz = sh(4), szn = sh(5);
                    pushQuad(c[1], c[2], c[6], c[5], sx.x, sx.y, sx.z);
                    pushQuad(c[0], c[4], c[7], c[3], sxn.x, sxn.y, sxn.z);
                    pushQuad(c[4], c[5], c[6], c[7], sy.x, sy.y, sy.z);
                    pushQuad(c[0], c[3], c[2], c[1], syn.x, syn.y, syn.z);
                    pushQuad(c[3], c[7], c[6], c[2], sz.x, sz.y, sz.z);
                    pushQuad(c[0], c[1], c[5], c[4], szn.x, szn.y, szn.z);
                } else if (skinGL) {
                    texDst = &batchVerts(skinGL);
                    for (int f = 0; f < 6; f++) {
                        Vec3 q[4];
                        pm::cuboidFaceCorners(p, f, q);
                        float u0, v0, u1, v1;
                        pm::skinFaceUV(p, f, u0, v0, u1, v1);
                        float s = kFaceSh[f] * (prim ? 1.08f : (sel ? 1.04f : 1.0f));
                        pushTexQuad(q[0], q[1], q[2], q[3], u0, v1, u1, v1, u1, v0, u0, v0, s, s, s);
                    }
                }
                float wr = prim ? 1.0f : (sel ? 1.0f : 0.45f);
                float wg = prim ? 0.9f : (sel ? 0.75f : 0.45f);
                float wb = prim ? 0.2f : (sel ? 0.25f : 0.45f);
                auto wline = [&](int a, int b) {
                    if (pm::isGridHairPart(p) && p.bind >= 0) return;
                    eline(wc[a].x, wc[a].y, wc[a].z, wc[b].x, wc[b].y, wc[b].z, wr, wg, wb, 1);
                };
                wline(0,1); wline(0,4); wline(0,3);
                wline(6,7); wline(6,2); wline(6,5);
                wline(1,5); wline(1,2);
                wline(4,7); wline(3,2);
                wline(7,6); wline(4,5);
            };
            auto emitClumpWires = [&](bool onlySel) {
                std::vector<int> done;
                auto seen = [&](int b) {
                    for (int x : done) if (x == b) return true;
                    return false;
                };
                for (int i = 0; i < (int)ed.entityParts.size(); i++) {
                    const pm::Part& p = ed.entityParts[i];
                    if (!pm::isGridHairPart(p) || p.bind < 0) continue;
                    if (onlySel && (i >= (int)entMark.size() || !entMark[i])) continue;
                    if (seen(p.bind)) continue;
                    done.push_back(p.bind);
                    bool sel = false, prim = false;
                    for (int j = 0; j < (int)ed.entityParts.size(); j++) {
                        if (ed.entityParts[j].bind != p.bind) continue;
                        if (j < (int)entMark.size() && entMark[j]) sel = true;
                        if (j == ed.entitySel) prim = true;
                    }
                    float wr = prim ? 1.0f : (sel ? 1.0f : 0.45f);
                    float wg = prim ? 0.9f : (sel ? 0.75f : 0.45f);
                    float wb = prim ? 0.2f : (sel ? 0.25f : 0.45f);
                    std::vector<pm::HairOutlineSeg> segs;
                    pm::hairClumpOutlines(ed.entityParts, p.bind, segs);
                    for (const pm::HairOutlineSeg& s : segs)
                        eline(s.a.x, s.a.y, s.a.z, s.b.x, s.b.y, s.b.z, wr, wg, wb, 1);
                }
            };

            bool anySel = false;
            for (int i = 0; i < (int)ed.entityParts.size(); i++)
                if (i < (int)entMark.size() && entMark[i]) { anySel = true; break; }

            if (!ed.entSkinView) {
                eline(leftPivX, leftPivY, leftPivZ, leftPivX + 1, leftPivY, leftPivZ, 1, 0.2f, 0.2f, 1);
                eline(leftPivX, leftPivY, leftPivZ, leftPivX, leftPivY + 1, leftPivZ, 0.2f, 1, 0.2f, 1);
                eline(leftPivX, leftPivY, leftPivZ, leftPivX, leftPivY, leftPivZ + 1, 0.2f, 0.3f, 1, 1);
                bool localView = !entIsCloth || ed.clothLocal;
                for (int qi = 0; qi < (int)ed.entityParts.size(); qi++) {
                    if (localView && anySel && (qi >= (int)entMark.size() || !entMark[qi])) continue;
                    emitPart(qi);
                }
                emitClumpWires(localView && anySel);
            }

            dstSolid = &entRightSolid;
            dstBatches = &entRightBatches;
            lineDst = &randLineVerts;
            eline(rightPivX, rightPivY, rightPivZ, rightPivX + 0.4f, rightPivY, rightPivZ, 1, 0.2f, 0.2f, 1);
            eline(rightPivX, rightPivY, rightPivZ, rightPivX, rightPivY + 0.4f, rightPivZ, 0.2f, 1, 0.2f, 1);
            eline(rightPivX, rightPivY, rightPivZ, rightPivX, rightPivY, rightPivZ + 0.4f, 0.2f, 0.3f, 1, 1);
            // pivot height marker
            eline(rightPivX - 0.12f, rightPivY, rightPivZ, rightPivX + 0.12f, rightPivY, rightPivZ, 1, 0.85f, 0.2f, 1);
            eline(rightPivX, rightPivY, rightPivZ - 0.12f, rightPivX, rightPivY, rightPivZ + 0.12f, 1, 0.85f, 0.2f, 1);
            for (int qi = 0; qi < (int)ed.entityParts.size(); qi++) emitPart(qi);
            emitClumpWires(false);
            if (entIsCloth && ed.clothBody) {
                EntThumb body;
                fillEntThumb(body, pm::buildPlayerModel(), bodySkinGL, 0, 0, 0);
                auto pourBody = [&](std::vector<float>& solid, std::vector<TexBatch>& batches) {
                    solid.insert(solid.end(), body.solid.begin(), body.solid.end());
                    for (const EntThumb::Batch& b : body.batches) {
                        if (!b.tex || b.verts.empty()) continue;
                        bool found = false;
                        for (TexBatch& dst : batches) {
                            if (dst.tex != b.tex) continue;
                            dst.verts.insert(dst.verts.end(), b.verts.begin(), b.verts.end());
                            found = true;
                            break;
                        }
                        if (!found) batches.push_back({ b.tex, 0, b.verts });
                    }
                };
                pourBody(entRightSolid, entRightBatches);
            }
            if (ed.partCut && ed.cutVis) {
                float x0 = ed.cutMn[0], y0 = ed.cutMn[1], z0 = ed.cutMn[2];
                float x1 = ed.cutMx[0], y1 = ed.cutMx[1], z1 = ed.cutMx[2];
                auto boxEdge = [&](float ax, float ay, float az, float bx, float by, float bz) {
                    eline(ax, ay, az, bx, by, bz, 1.0f, 0.85f, 0.15f, 1);
                };
                boxEdge(x0,y0,z0, x1,y0,z0); boxEdge(x1,y0,z0, x1,y0,z1); boxEdge(x1,y0,z1, x0,y0,z1); boxEdge(x0,y0,z1, x0,y0,z0);
                boxEdge(x0,y1,z0, x1,y1,z0); boxEdge(x1,y1,z0, x1,y1,z1); boxEdge(x1,y1,z1, x0,y1,z1); boxEdge(x0,y1,z1, x0,y1,z0);
                boxEdge(x0,y0,z0, x0,y1,z0); boxEdge(x1,y0,z0, x1,y1,z0); boxEdge(x1,y0,z1, x1,y1,z1); boxEdge(x0,y0,z1, x0,y1,z1);
            }
            if ((ed.partAdd || ed.partMeasure || ed.partPlane) && (ed.partAddVis || ed.partAnchor)) {
                const float h = 0.0015f;
                auto boxEdge = [&](float ax, float ay, float az, float bx, float by, float bz, float r, float g, float b) {
                    eline(ax, ay, az, bx, by, bz, r, g, b, 1);
                };
                auto tiny = [&](float cx, float cy, float cz, float r, float g, float b) {
                    float x0 = cx - h, y0 = cy - h, z0 = cz - h;
                    float x1 = cx + h, y1 = cy + h, z1 = cz + h;
                    boxEdge(x0,y0,z0, x1,y0,z0, r,g,b); boxEdge(x1,y0,z0, x1,y0,z1, r,g,b); boxEdge(x1,y0,z1, x0,y0,z1, r,g,b); boxEdge(x0,y0,z1, x0,y0,z0, r,g,b);
                    boxEdge(x0,y1,z0, x1,y1,z0, r,g,b); boxEdge(x1,y1,z0, x1,y1,z1, r,g,b); boxEdge(x1,y1,z1, x0,y1,z1, r,g,b); boxEdge(x0,y1,z1, x0,y1,z0, r,g,b);
                    boxEdge(x0,y0,z0, x0,y1,z0, r,g,b); boxEdge(x1,y0,z0, x1,y1,z0, r,g,b); boxEdge(x1,y0,z1, x1,y1,z1, r,g,b); boxEdge(x0,y0,z1, x0,y1,z1, r,g,b);
                };
                if (ed.partAnchor) tiny(ed.partA[0], ed.partA[1], ed.partA[2], 0.95f, 0.85f, 0.25f);
                if (ed.partAddVis) tiny(ed.partB[0], ed.partB[1], ed.partB[2], 0.35f, 0.95f, 0.45f);
                if (ed.partMeasure && ed.partAnchor && ed.partAddVis) {
                    float oa[3], ob[3], len = 0.0f;
                    guideOuterSpan(ed.partA, ed.partB, h, oa, ob, len);
                    eline(oa[0], oa[1], oa[2], ob[0], ob[1], ob[2], 0.35f, 0.85f, 1.0f, 1);
                }
                if (ed.partAdd && ed.partAnchor && ed.partAddVis) {
                    float x0 = ed.partMn[0], y0 = ed.partMn[1], z0 = ed.partMn[2];
                    float x1 = ed.partMx[0], y1 = ed.partMx[1], z1 = ed.partMx[2];
                    boxEdge(x0,y0,z0, x1,y0,z0, 0.35f,0.95f,0.45f); boxEdge(x1,y0,z0, x1,y0,z1, 0.35f,0.95f,0.45f); boxEdge(x1,y0,z1, x0,y0,z1, 0.35f,0.95f,0.45f); boxEdge(x0,y0,z1, x0,y0,z0, 0.35f,0.95f,0.45f);
                    boxEdge(x0,y1,z0, x1,y1,z0, 0.35f,0.95f,0.45f); boxEdge(x1,y1,z0, x1,y1,z1, 0.35f,0.95f,0.45f); boxEdge(x1,y1,z1, x0,y1,z1, 0.35f,0.95f,0.45f); boxEdge(x0,y1,z1, x0,y1,z0, 0.35f,0.95f,0.45f);
                    boxEdge(x0,y0,z0, x0,y1,z0, 0.35f,0.95f,0.45f); boxEdge(x1,y0,z0, x1,y1,z0, 0.35f,0.95f,0.45f); boxEdge(x1,y0,z1, x1,y1,z1, 0.35f,0.95f,0.45f); boxEdge(x0,y0,z1, x0,y1,z1, 0.35f,0.95f,0.45f);
                }
            }
            if (ed.measureOn) {
                const float h = 0.0015f;
                auto boxEdge = [&](float ax, float ay, float az, float bx, float by, float bz) {
                    eline(ax, ay, az, bx, by, bz, 0.35f, 0.85f, 1.0f, 1);
                };
                auto tiny = [&](float cx, float cy, float cz) {
                    float x0 = cx - h, y0 = cy - h, z0 = cz - h;
                    float x1 = cx + h, y1 = cy + h, z1 = cz + h;
                    boxEdge(x0,y0,z0, x1,y0,z0); boxEdge(x1,y0,z0, x1,y0,z1); boxEdge(x1,y0,z1, x0,y0,z1); boxEdge(x0,y0,z1, x0,y0,z0);
                    boxEdge(x0,y1,z0, x1,y1,z0); boxEdge(x1,y1,z0, x1,y1,z1); boxEdge(x1,y1,z1, x0,y1,z1); boxEdge(x0,y1,z1, x0,y1,z0);
                    boxEdge(x0,y0,z0, x0,y1,z0); boxEdge(x1,y0,z0, x1,y1,z0); boxEdge(x1,y0,z1, x1,y1,z1); boxEdge(x0,y0,z1, x0,y1,z1);
                };
                tiny(ed.measureA[0], ed.measureA[1], ed.measureA[2]);
                tiny(ed.measureB[0], ed.measureB[1], ed.measureB[2]);
                float oa[3], ob[3], len = 0.0f;
                guideOuterSpan(ed.measureA, ed.measureB, h, oa, ob, len);
                eline(oa[0], oa[1], oa[2], ob[0], ob[1], ob[2], 0.35f, 0.85f, 1.0f, 1);
            }
            if (ed.planeOn) {
                pm::Part bp;
                if (copyBoundPart(bp)) {
                    Vec3 q[4];
                    pm::cuboidFaceCorners(bp, ed.planeFace, q);
                    Vec3 nrm = (q[1] - q[0]).cross(q[3] - q[0]);
                    Vec3 mid = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
                    if (nrm.dot(mid - bp.center) < 0.0f) nrm = nrm * -1.0f;
                    if (nrm.lengthSq() > 1e-12f) nrm = nrm.normalized();
                    const float lift = 0.0004f;
                    Vec3 c0 = q[0] + nrm * lift, c1 = q[1] + nrm * lift;
                    Vec3 c2 = q[2] + nrm * lift, c3 = q[3] + nrm * lift;
                    auto paintMask = [&](std::vector<float>& solid) {
                        auto tri = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
                            float v[3][7] = {
                                { a.x, a.y, a.z, 0.72f, 0.35f, 1.0f, 0.45f },
                                { b.x, b.y, b.z, 0.72f, 0.35f, 1.0f, 0.45f },
                                { c.x, c.y, c.z, 0.72f, 0.35f, 1.0f, 0.45f },
                            };
                            for (auto& e : v) for (int k = 0; k < 7; k++) solid.push_back(e[k]);
                        };
                        tri(c0, c1, c2);
                        tri(c0, c2, c3);
                    };
                    paintMask(entRightSolid);
                    paintMask(entSolid);
                    eline(c0.x, c0.y, c0.z, c1.x, c1.y, c1.z, 0.72f, 0.35f, 1.0f, 1);
                    eline(c1.x, c1.y, c1.z, c2.x, c2.y, c2.z, 0.72f, 0.35f, 1.0f, 1);
                    eline(c2.x, c2.y, c2.z, c3.x, c3.y, c3.z, 0.72f, 0.35f, 1.0f, 1);
                    eline(c3.x, c3.y, c3.z, c0.x, c0.y, c0.z, 0.72f, 0.35f, 1.0f, 1);
                }
            }
            if (ed.hairSelect) {
                auto emitHairSel = [&](const std::vector<pm::HairAtom>& sel) {
                    for (const pm::HairAtom& a : sel) {
                        Vec3 c[8] = {
                            { a.mn.x, a.mn.y, a.mn.z }, { a.mx.x, a.mn.y, a.mn.z },
                            { a.mx.x, a.mn.y, a.mx.z }, { a.mn.x, a.mn.y, a.mx.z },
                            { a.mn.x, a.mx.y, a.mn.z }, { a.mx.x, a.mx.y, a.mn.z },
                            { a.mx.x, a.mx.y, a.mx.z }, { a.mn.x, a.mx.y, a.mx.z }
                        };
                        auto yl = [&](int i, int j) {
                            eline(c[i].x, c[i].y, c[i].z, c[j].x, c[j].y, c[j].z, 1.0f, 0.92f, 0.15f, 1);
                        };
                        yl(0, 1); yl(1, 2); yl(2, 3); yl(3, 0);
                        yl(4, 5); yl(5, 6); yl(6, 7); yl(7, 4);
                        yl(0, 4); yl(1, 5); yl(2, 6); yl(3, 7);
                    }
                };
                emitHairSel(ed.hairVoxSel);
                emitHairSel(ed.hairCardSel);
            }

            if ((entGzMove || entGzStretch || entGzFill || entGzRot || entGzMirror || entGzUV) &&
                canEntGizmo && !ed.hairPaint && !ed.hairErase && !ed.hairSelect && !ed.hairCard) {
                auto pushGz = [&](float x, float y, float r, float gcol, float b, float a) {
                    gizmoOverlay.push_back(x); gizmoOverlay.push_back(y); gizmoOverlay.push_back(0);
                    gizmoOverlay.push_back(r); gizmoOverlay.push_back(gcol); gizmoOverlay.push_back(b); gizmoOverlay.push_back(a);
                };
                auto gzLine2 = [&](float x0, float y0, float x1, float y1, float r, float gcol, float b) {
                    pushGz(x0, y0, r, gcol, b, 1); pushGz(x1, y1, r, gcol, b, 1);
                };
                auto gzHead = [&](float tx, float ty, float fx, float fy, float sz, float r, float gcol, float b) {
                    float dx = tx - fx, dy = ty - fy;
                    float len = std::hypot(dx, dy);
                    if (len < 1.0f) return;
                    dx /= len; dy /= len;
                    float px = -dy, py = dx;
                    float bx = tx - dx * sz, by = ty - dy * sz;
                    gzLine2(tx, ty, bx + px * sz * 0.55f, by + py * sz * 0.55f, r, gcol, b);
                    gzLine2(tx, ty, bx - px * sz * 0.55f, by - py * sz * 0.55f, r, gcol, b);
                    gzLine2(bx + px * sz * 0.55f, by + py * sz * 0.55f, bx - px * sz * 0.55f, by - py * sz * 0.55f, r, gcol, b);
                };
                auto gzArrow = [&](const Vec3& pos, const Vec3& out, float alen, bool hot, float r, float gc, float b) {
                    if (hot) { r = 1; gc = 1; b = 0.35f; }
                    Vec3 a0 = pos - out * alen, a1 = pos + out * alen;
                    float sx0, sy0, sx1, sy1;
                    if (!projectRight(a0.x, a0.y, a0.z, sx0, sy0) || !projectRight(a1.x, a1.y, a1.z, sx1, sy1)) return;
                    gzLine2(sx0, sy0, sx1, sy1, r, gc, b);
                    gzHead(sx1, sy1, sx0, sy0, hot ? 14.0f : 11.0f, r, gc, b);
                    gzHead(sx0, sy0, sx1, sy1, hot ? 14.0f : 11.0f, r, gc, b);
                };
                Vec3 g; gizmoCenterView(g);
                float L = gizmoSize();
                float R = L * 0.72f;
                const float col[3][3] = { {1.0f,0.28f,0.28f},{0.28f,0.92f,0.32f},{0.32f,0.45f,1.0f} };
                int hov = ed.gizmoHover;
                bool drawTrans = entGzMove || entGzStretch || entGzFill || entGzMirror;
                bool drawRot = entGzMove || entGzRot;
                if (drawTrans || drawRot) {
                    for (int a = 0; a < 3; a++) {
                        bool hotT = (hov == 1 + a) || (hov == 10 + a * 2) || (hov == 11 + a * 2);
                        bool hotR = (hov == 4 + a) || (hov == 20 + a * 2) || (hov == 21 + a * 2);
                        if (drawTrans) {
                            float r = col[a][0], gc = col[a][1], b = col[a][2];
                            if (hotT) { r = 1; gc = 1; b = 0.35f; }
                            Vec3 p0 = { g.x - kAxis[a].x * L, g.y - kAxis[a].y * L, g.z - kAxis[a].z * L };
                            Vec3 p1 = { g.x + kAxis[a].x * L, g.y + kAxis[a].y * L, g.z + kAxis[a].z * L };
                            float sx0, sy0, sx1, sy1;
                            if (projectRight(p0.x, p0.y, p0.z, sx0, sy0) && projectRight(p1.x, p1.y, p1.z, sx1, sy1)) {
                                gzLine2(sx0, sy0, sx1, sy1, r, gc, b);
                                gzHead(sx1, sy1, sx0, sy0, hotT ? 16.0f : 13.0f, r, gc, b);
                                gzHead(sx0, sy0, sx1, sy1, hotT ? 16.0f : 13.0f, r, gc, b);
                            }
                        }
                        if (!drawRot) continue;
                        float rr = hotR ? 1.0f : col[a][0], rg = hotR ? 1.0f : col[a][1], rb = hotR ? 0.35f : col[a][2];
                        Vec3 u, vv; rotBasis(a, u, vv);
                        const int segs = 28;
                        float a0 = -2.3f, a1 = 2.3f;
                        float px, py;
                        bool prevOk = false;
                        for (int i = 0; i <= segs; i++) {
                            float ang = a0 + (a1 - a0) * (float)i / (float)segs;
                            Vec3 q = { g.x + (u.x * std::cos(ang) + vv.x * std::sin(ang)) * R,
                                       g.y + (u.y * std::cos(ang) + vv.y * std::sin(ang)) * R,
                                       g.z + (u.z * std::cos(ang) + vv.z * std::sin(ang)) * R };
                            float sx, sy;
                            bool ok = projectRight(q.x, q.y, q.z, sx, sy);
                            if (ok && prevOk) gzLine2(px, py, sx, sy, rr, rg, rb);
                            if (ok) { px = sx; py = sy; }
                            prevOk = ok;
                        }
                        auto rotHead = [&](float ang, float dir) {
                            Vec3 hp = { g.x + (u.x * std::cos(ang) + vv.x * std::sin(ang)) * R,
                                        g.y + (u.y * std::cos(ang) + vv.y * std::sin(ang)) * R,
                                        g.z + (u.z * std::cos(ang) + vv.z * std::sin(ang)) * R };
                            float dang = 0.22f * dir;
                            Vec3 from = { g.x + (u.x * std::cos(ang - dang) + vv.x * std::sin(ang - dang)) * R,
                                          g.y + (u.y * std::cos(ang - dang) + vv.y * std::sin(ang - dang)) * R,
                                          g.z + (u.z * std::cos(ang - dang) + vv.z * std::sin(ang - dang)) * R };
                            float hx, hy, fx, fy;
                            if (projectRight(hp.x, hp.y, hp.z, hx, hy) && projectRight(from.x, from.y, from.z, fx, fy))
                                gzHead(hx, hy, fx, fy, hotR ? 16.0f : 13.0f, rr, rg, rb);
                        };
                        rotHead(a0, -1.0f);
                        rotHead(a1, 1.0f);
                    }
                } else if (entGzUV) {
                    auto idx = geomSelected();
                    if (!idx.empty()) {
                        int pi = ed.entitySel;
                        if (pi < 0 || pi >= (int)ed.entityParts.size() ||
                            std::find(idx.begin(), idx.end(), pi) == idx.end())
                            pi = idx[0];
                        const pm::Part& p = ed.entityParts[pi];
                        Vec3 o, u, v;
                        bool ok = false;
                        if (!p.tex.empty() && !pm::isHairPart(p)) {
                            Vec3 q[4];
                            pm::texQuadLocal(p, q);
                            o = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
                            u = q[1] - q[0];
                            v = q[3] - q[0];
                            ok = u.lengthSq() > 1e-10f && v.lengthSq() > 1e-10f;
                            if (ok) { u = u.normalized(); v = v.normalized(); }
                        } else {
                            o = p.center; u = { 1, 0, 0 }; v = { 0, 1, 0 }; ok = true;
                        }
                        if (ok) {
                            float alen = gizmoSize() * 0.55f;
                            gzArrow(o, u, alen, hov == 60, 1.0f, 0.55f, 0.2f);
                            gzArrow(o, v, alen, hov == 61, 0.3f, 0.85f, 1.0f);
                        }
                    }
                }
            }

            if (ed.refTool && ed.refDecalIdx >= 0 && ed.refDecalIdx < (int)ed.entityParts.size() &&
                pm::isDecalPart(ed.entityParts[ed.refDecalIdx])) {
                auto pushGz = [&](float x, float y, float r, float gcol, float b, float a) {
                    gizmoOverlay.push_back(x); gizmoOverlay.push_back(y); gizmoOverlay.push_back(0);
                    gizmoOverlay.push_back(r); gizmoOverlay.push_back(gcol); gizmoOverlay.push_back(b); gizmoOverlay.push_back(a);
                };
                auto gzLine2 = [&](float x0, float y0, float x1, float y1, float r, float gcol, float b) {
                    pushGz(x0, y0, r, gcol, b, 1); pushGz(x1, y1, r, gcol, b, 1);
                };
                Vec3 q[4];
                pm::texQuadLocal(ed.entityParts[ed.refDecalIdx], q);
                float extend = std::max(1.2f, gizmoSize() * 5.0f);
                int e0 = 0, e1 = 4;
                if (ed.refEdge >= 0) { e0 = ed.refEdge; e1 = ed.refEdge + 1; }
                for (int e = e0; e < e1; e++) {
                    Vec3 a, b;
                    decalEdgeExtended(q, e, extend, a, b);
                    float sx0, sy0, sx1, sy1;
                    if (!projectRight(a.x, a.y, a.z, sx0, sy0) || !projectRight(b.x, b.y, b.z, sx1, sy1)) continue;
                    bool hot = (e == ed.refLineHover) || (ed.refEdge == e);
                    float r = hot ? 1.0f : 0.95f, gc = hot ? 0.9f : 0.72f, bc = hot ? 0.15f : 0.22f;
                    gzLine2(sx0, sy0, sx1, sy1, r, gc, bc);
                }
            }

            rect(paneRX, kTopH, paneRW, matBarY - kTopH, 0.07f, 0.08f, 0.10f, 1);
            rect(palColX, kTopH, palColW, matBarY - kTopH, 0.09f, 0.09f, 0.11f, 1);
            rect(0, kTopH, leftBarW, (float)g_winH - kTopH, 0.08f, 0.08f, 0.10f, 1);
            rect(leftBarW - 1, kTopH, 2, (float)g_winH - kTopH, 0.25f, 0.25f, 0.28f, 1);
            rect(splitX - 1, kTopH, 2, matBarY - kTopH, 0.25f, 0.25f, 0.28f, 1);
            rect(paneRX - 1, kTopH, 2, matBarY - kTopH, 0.25f, 0.25f, 0.28f, 1);
            rect(paneLX, kTopH, paneLW, kHdrH, 0.10f, 0.11f, 0.13f, 1);
            if (toolBandH > 1.0f)
                rect(paneLX, kTopH + kHdrH, paneLW, toolBandH, 0.09f, 0.10f, 0.12f, 1);
            rect(palColX, kTopH, palColW, kHdrH, 0.10f, 0.11f, 0.13f, 1);
            rect(paneRX, kTopH, paneRW, kHdrH, 0.10f, 0.11f, 0.13f, 1);
            if (entToolColW > 1.0f) {
                rect(entToolX, kTopH, entToolColW, matBarY - kTopH, 0.08f, 0.09f, 0.11f, 1);
                rect(entToolX - 1, kTopH, 2, matBarY - kTopH, 0.25f, 0.25f, 0.28f, 1);
                rect(entToolX, kTopH, entToolColW, kHdrH, 0.10f, 0.11f, 0.13f, 1);
            }
            rect(paneLX, matBarY, (float)g_winW - paneLX, kEntBarH, 0.10f, 0.11f, 0.13f, 1);
            if (ed.entSkinView || paintKind() >= 0 || hairToolsOn)
                rect(paneLX, paneLY, paneLW, paneLH, 0.06f, 0.06f, 0.08f, 1);
            // 3D / planar toggle (center pane top-right)
            auto stroke2 = [&](float x, float y, float w, float h, float t, float r, float g, float b) {
                rect(x, y, w, t, r, g, b, 1);
                rect(x, y + h - t, w, t, r, g, b, 1);
                rect(x, y, t, h, r, g, b, 1);
                rect(x + w - t, y, t, h, r, g, b, 1);
            };
            auto drawEntToggle = [&](bool right) {
                rect(togX, togY, togW, togH, togHov ? 0.22f : 0.14f, togHov ? 0.24f : 0.15f, togHov ? 0.28f : 0.18f, 1);
                float knobW = togW * 0.48f;
                float knobX = right ? (togX + togW - knobW - 3.0f) : (togX + 3.0f);
                rect(knobX, togY + 3.0f, knobW, togH - 6.0f, 0.38f, 0.52f, 0.32f, 1);
                if (togHov) stroke2(togX - 1, togY - 1, togW + 2, togH + 2, 1, 1, 1, 1);
            };
            drawEntToggle(entIsCloth ? !ed.clothLocal : ed.entSkinView);
            if (entIsCloth) {
                auto clothBtn = [&](const float r[4], bool on) {
                    bool hov = mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3];
                    rect(r[0], r[1], r[2], r[3], on ? 0.28f : 0.14f, on ? 0.36f : 0.15f, on ? 0.24f : 0.18f, 1);
                    if (hov || on) stroke2(r[0] - 1, r[1] - 1, r[2] + 2, r[3] + 2, 1, on ? 0.9f : 1.0f, on ? 0.9f : 1.0f, on ? 0.4f : 1.0f);
                };
                clothBtn(cBody, ed.clothBody);
            }
            // vertical pivot-height slider on the far right of the overview
            {
                float tx = entSlideX + kEntSlideW * 0.5f - 3.0f;
                rect(tx, entSlideY, 6.0f, entSlideH, 0.18f, 0.18f, 0.20f, 1);
                float t = ed.entPivotY / 1.80f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float thY = entSlideY + (1.0f - t) * entSlideH - 8.0f;
                rect(entSlideX, thY, kEntSlideW, 16.0f, slideHov ? 0.95f : 0.80f, slideHov ? 0.85f : 0.70f, 0.25f, 1);
                if (slideHov) stroke2(entSlideX - 1, thY - 1, kEntSlideW + 2, 18.0f, 1, 1, 1, 1);
            }
            if (ed.hairPaint && paintSlideW > 1.0f) {
                float trackY = paintSlideY + 5.0f;
                rect(paintSlideX, trackY, paintSlideW, 6.0f, 0.18f, 0.18f, 0.20f, 1);
                for (int i = 0; i < 4; i++) {
                    float x = paintSlideX + paintSlideW * ((float)i / 3.0f);
                    rect(x - 1.0f, trackY - 3.0f, 2.0f, 12.0f, 0.45f, 0.45f, 0.48f, 1);
                }
                float kx = paintSlideX + paintSlideW * ((float)ed.paintDiv / 3.0f) - 7.0f;
                rect(kx, trackY - 5.0f, 14.0f, 16.0f, paintSlideHov ? 0.95f : 0.80f, paintSlideHov ? 0.85f : 0.70f, 0.25f, 1);
            }
            for (int i = 0; i < kEntTools; i++) {
                float bx = etoolRect[i][0], by = etoolRect[i][1], bw = etoolRect[i][2], bh = etoolRect[i][3];
                if (bw < 1.0f || bh < 1.0f) continue;
                bool hov = (ed.modelToolHover == i);
                bool on = (i == 2 && ed.modelTool == 0) || (i == 5 && ed.modelTool == 3)
                    || (i == 6 && ed.modelTool == 1) || (i == 7 && ed.modelTool == 2)
                    || (i == 8 && ed.modelTool == 4) || (i == 9 && ed.modelTool == 5)
                    || (i == 10 && ed.refTool);
                float r = on ? 0.38f : (hov ? 0.28f : 0.16f);
                float g = on ? 0.48f : (hov ? 0.32f : 0.17f);
                float b = on ? 0.28f : (hov ? 0.22f : 0.16f);
                rect(bx, by, bw, bh, r, g, b, 1);
                if (hov) rect(bx - 2, by - 2, bw + 4, bh + 4, 1, 1, 1, 1);
            }
            if (eactRect[0][2] > 1.0f) {
                float divY = eactRect[0][1] - 8.0f;
                rect(entToolX + 8.0f, divY, entToolColW - 16.0f, 1.0f, 0.28f, 0.28f, 0.32f, 1);
            }
            for (int i = 0; i < kEActs; i++) {
                float bx = eactRect[i][0], by = eactRect[i][1], bw = eactRect[i][2], bh = eactRect[i][3];
                if (bw < 1.0f) continue;
                bool hov = (i == 0) ? entPartHov : (i == 1) ? entSelHov : (i == 2) ? entPaintHov
                    : (i == 3) ? entCardHov : (i == 4) ? entEraseHov : (i == 5) ? entMergeHov
                    : (i == 6) ? entSplitHov : (i == 7) ? entTexHov : (i == 8) ? entDupHov : (i == 9) ? entDelHov
                    : (i == 10) ? entCutHov : (i == 11) ? entMeasureHov : entPlaneHov;
                bool del = (i == 9);
                bool stickyOn = (i == 0 && entIsCloth && ed.partAdd) || (i == 1 && ed.hairSelect) || (i == 2 && ed.hairPaint)
                    || (i == 3 && ed.hairCard) || (i == 4 && ed.hairErase) || (i == 10 && ed.partCut) || (i == 11 && ed.partMeasure) || (i == 12 && ed.partPlane);
                float r = del ? (hov ? 0.50f : 0.24f) : stickyOn ? 0.38f : (hov ? 0.32f : 0.18f);
                float g = del ? (hov ? 0.22f : 0.16f) : stickyOn ? 0.48f : (hov ? 0.40f : 0.20f);
                float b = del ? (hov ? 0.20f : 0.16f) : stickyOn ? 0.28f : (hov ? 0.28f : 0.18f);
                rect(bx, by, bw, bh, r, g, b, 1);
                if (hov) rect(bx - 2, by - 2, bw + 4, bh + 4, 1, 1, 1, 1);
            }
            rect(entCancelX, midBtnY, kCancelW, kBtnH, entCancelHov ? 0.45f : 0.22f, entCancelHov ? 0.28f : 0.18f, entCancelHov ? 0.22f : 0.18f, 1);
            if (entCancelHov) rect(entCancelX - 2, midBtnY - 2, kCancelW + 4, kBtnH + 4, 1, 1, 1, 1);
            rect(midSaveX, midBtnY, kSaveS, kSaveS, midSaveHov ? 0.30f : 0.15f, midSaveHov ? 0.45f : 0.15f, midSaveHov ? 0.18f : 0.15f, 1);
            if (midSaveHov) rect(midSaveX - 2, midBtnY - 2, kSaveS + 4, kSaveS + 4, 1, 1, 1, 1);
            rect(midUndoX, midBtnY, kSaveS, kSaveS, midUndoHov ? 0.30f : 0.15f, midUndoHov ? 0.45f : 0.15f, midUndoHov ? 0.18f : 0.15f, 1);
            if (midUndoHov) rect(midUndoX - 2, midBtnY - 2, kSaveS + 4, kSaveS + 4, 1, 1, 1, 1);

            int curMouth = -1;
            std::string curHairTex;
            if (!ed.entityParts.empty() && ed.entitySel >= 0 && ed.entitySel < (int)ed.entityParts.size()) {
                if (pm::isMouthPart(ed.entityParts[ed.entitySel])) {
                    curMouth = -1;
                    const std::string& mt = ed.entityParts[ed.entitySel].tex;
                    for (int i = 0; i < (int)entMouthNames.size(); i++)
                        if (entMouthNames[i] == mt) { curMouth = i; break; }
                    if (curMouth < 0) curMouth = pm::mouthIndex(mt);
                }
                if (pm::isHairPart(ed.entityParts[ed.entitySel]) || pm::isHairCardPart(ed.entityParts[ed.entitySel]) ||
                    (pm::isDecalPart(ed.entityParts[ed.entitySel]) && !pm::isEyePart(ed.entityParts[ed.entitySel])
                     && !pm::isEyelidPart(ed.entityParts[ed.entitySel]) && !pm::isMouthPart(ed.entityParts[ed.entitySel])))
                    curHairTex = ed.entityParts[ed.entitySel].tex;
            }
            if (ed.hairCard && !ed.hairCardTex.empty()) curHairTex = ed.hairCardTex;
            rect(0, kTopH, leftBarW, 36.0f, 0.10f, 0.11f, 0.13f, 1);
            if (selEyeKind) {
                for (int i = 0; i < nFaceChip; i++) {
                    bool hov = (faceChipHov == i);
                    int pi = findPartTex(faceNameAt(i));
                    bool on = (pi >= 0 && pi == ed.entitySel);
                    float r = on ? 0.38f : (hov ? 0.28f : 0.16f);
                    float g = on ? 0.48f : (hov ? 0.32f : 0.17f);
                    float b = on ? 0.28f : (hov ? 0.22f : 0.16f);
                    rect(faceChip[i][0], faceChip[i][1], faceChip[i][2], faceChip[i][3], r, g, b, 1);
                    if (hov || on) rect(faceChip[i][0] - 2, faceChip[i][1] - 2, faceChip[i][2] + 4, faceChip[i][3] + 4,
                                        on ? 1.0f : 1.0f, on ? 0.85f : 1.0f, on ? 0.2f : 1.0f, 1);
                }
            }
            if (selMouthKind) {
                for (int i = 0; i < nMouthChip; i++) {
                    bool hov = (mouthChipHov == i), on = (curMouth == i);
                    float r = on ? 0.38f : (hov ? 0.28f : 0.16f);
                    float g = on ? 0.48f : (hov ? 0.32f : 0.17f);
                    float b = on ? 0.28f : (hov ? 0.22f : 0.16f);
                    rect(mouthChip[i][0], mouthChip[i][1], mouthChip[i][2], mouthChip[i][3], r, g, b, 1);
                    if (hov || on) rect(mouthChip[i][0] - 2, mouthChip[i][1] - 2, mouthChip[i][2] + 4, mouthChip[i][3] + 4,
                                        on ? 1.0f : 1.0f, on ? 0.85f : 1.0f, on ? 0.2f : 1.0f, 1);
                }
            }
            if (hairToolsOn) {
                rect(paneLX, entMatY - 6.0f, paneLW, (paneLY + paneLH) - (entMatY - 6.0f), 0.08f, 0.08f, 0.10f, 0.92f);
                for (int i = 0; i < nMatItems; i++) {
                    float mx0, my0;
                    matItemRect(i, mx0, my0);
                    bool hov = (ed.matHover == i);
                    bool addSlot = (i == nMatItems - 1);
                    bool sel = false;
                    if (!addSlot) {
                        std::string nm = (i < TEX_COUNT) ? mat::tileName(i) : extraMats[i - TEX_COUNT].name;
                        sel = (nm == curHairTex);
                    }
                    rect(mx0, my0, kMatThumb, kMatThumb, addSlot ? 0.18f : 0.08f, addSlot ? 0.18f : 0.08f, addSlot ? 0.20f : 0.10f, 1);
                    if (sel) {
                        rect(mx0 - 2, my0 - 2, kMatThumb + 4, 2, 1.0f, 0.85f, 0.2f, 1);
                        rect(mx0 - 2, my0 + kMatThumb, kMatThumb + 4, 2, 1.0f, 0.85f, 0.2f, 1);
                        rect(mx0 - 2, my0 - 2, 2, kMatThumb + 4, 1.0f, 0.85f, 0.2f, 1);
                        rect(mx0 + kMatThumb, my0 - 2, 2, kMatThumb + 4, 1.0f, 0.85f, 0.2f, 1);
                    }
                    if (hov) rect(mx0 - 1, my0 - 1, kMatThumb + 2, kMatThumb + 2, 1, 1, 1, 1);
                }
            }
            for (int i = 0; i < 13; i++) {
                float bx = ed.tbRect[i][0], by = ed.tbRect[i][1], bs = ed.tbRect[i][2];
                bool hov = (ed.toolHover == i);
                bool active = hairToolsOn
                    ? ((i == 0 && ed.hairPaint) || (i == 4 && ed.hairErase) ||
                       (i == 3 && ed.picker) || (i == 1 && (ed.tool == 1 || ed.brushMod)) ||
                       (i == ed.tool && i < 9 && i != 0 && i != 4))
                    : ((i == 3) ? ed.picker : (i == 1) ? (ed.tool == 1 || ed.brushMod) : (i == ed.tool && i < 9));
                rect(bx, by, bs, bs, active ? 0.30f : 0.15f, active ? 0.45f : 0.15f, active ? 0.18f : 0.15f, 1);
                if (hov) rect(bx, by, bs, bs, 0.35f, 0.35f, 0.35f, 1);
                float bc = active ? 1.0f : 0.4f;
                rect(bx, by, bs, 1, bc, active ? 0.9f : bc, 0.2f, 1);
                rect(bx, by + bs - 1, bs, 1, bc, active ? 0.9f : bc, 0.2f, 1);
                rect(bx, by, 1, bs, bc, active ? 0.9f : bc, 0.2f, 1);
                rect(bx + bs - 1, by, 1, bs, bc, active ? 0.9f : bc, 0.2f, 1);
            }
            for (int i = 0; i < 8; i++) {
                bool hov = (ed.paletteHover == i);
                rect(palRect[i][0], palRect[i][1], palRect[i][2], palRect[i][3],
                     kPaintPal[i][0], kPaintPal[i][1], kPaintPal[i][2], 1);
                if (hov) rect(palRect[i][0] - 2, palRect[i][1] - 2, palRect[i][2] + 4, palRect[i][3] + 4, 1, 1, 1, 1);
            }
            {
                bool hov = (ed.paletteHover == 8);
                rect(palRect[8][0], palRect[8][1], palRect[8][2], palRect[8][3], 0.3f, 0.3f, 0.3f, 1);
                rect(palRect[8][0] + 6, palRect[8][1] + palRect[8][3] * 0.5f - 1, palRect[8][2] - 12, 2, 1, 0, 0, 1);
                if (hov) rect(palRect[8][0] - 2, palRect[8][1] - 2, palRect[8][2] + 4, palRect[8][3] + 4, 1, 1, 1, 1);
            }
            rect(curX, curY, curW, curH, ed.r, ed.g, ed.b, ed.a);
            {
                const int n = 4;
                float cs = curH / (float)n;
                for (int cy = 0; cy < n; cy++) for (int cx = 0; cx < n; cx++) {
                    bool ck = ((cx + cy) & 1) != 0;
                    float g = ck ? 0.45f : 0.62f;
                    rect(curX + cx * (curW / n), curY + cy * cs, curW / n + 0.4f, cs + 0.4f, g, g, g, 1);
                }
                rect(curX, curY, curW, curH, ed.r, ed.g, ed.b, ed.a);
            }
            {
                for (size_t k = 0; k < ed.recent.size() && k < 8; k++) {
                    float rx = recX0 + k * (recS + 4.0f);
                    float ry = recY;
                    rect(rx, ry, recS, recS, ed.recent[k].r / 255.0f, ed.recent[k].g / 255.0f,
                         ed.recent[k].b / 255.0f, ed.recent[k].a / 255.0f);
                    if (lmb && !prevLmb && mx >= rx && mx < rx + recS && my >= ry && my < ry + recS) {
                        ed.r = ed.recent[k].r / 255.0f; ed.g = ed.recent[k].g / 255.0f;
                        ed.b = ed.recent[k].b / 255.0f; ed.a = ed.recent[k].a / 255.0f;
                    }
                }
            }
            {
                float alX = ed.alphaRect[0], alY = ed.alphaRect[1], alW = ed.alphaRect[2], alH = ed.alphaRect[3];
                for (int i = 0; i < 16; i++) {
                    bool ck = (i % 2) != 0;
                    rect(alX + i * (alW / 16), alY, alW / 16, alH, ck ? 0.45f : 0.65f, ck ? 0.45f : 0.65f, ck ? 0.45f : 0.65f, 1);
                }
                rect(alX, alY, alW * ed.a, alH, 1.0f, 1.0f, 1.0f, 0.85f);
                rect(alX + alW * ed.a - 2, alY - 2, 4, alH + 4, 1, 1, 1, 1);
            }
            {
                float ch, cs, cv;
                rgbToHsv(ed.r, ed.g, ed.b, ch, cs, cv);
                float svX = ed.svRect[0], svY = ed.svRect[1], svW = ed.svRect[2];
                for (int j = 0; j < 16; j++) for (int i = 0; i < 16; i++) {
                    float s = (float)i / 15.0f, v = 1.0f - (float)j / 15.0f;
                    float rr, gg, bb; hsvToRgb(ch, s, v, rr, gg, bb);
                    rect(svX + i * (svW / 16), svY + j * (svW / 16), svW / 16 + 0.5f, svW / 16 + 0.5f, rr, gg, bb, 1);
                }
                rect(svX + cs * svW - 2, svY + (1 - cv) * svW - 2, 4, 4, 1, 1, 1, 1);
                const int HS = 32;
                for (int k = 0; k < HS; k++) {
                    float a0 = (float)k / HS * 6.2831853f, a1 = (float)(k + 1) / HS * 6.2831853f;
                    float rr, gg, bb; hsvToRgb((float)k / HS, 1, 1, rr, gg, bb);
                    float x0 = ed.hueCX + std::cos(a0) * ed.hueRI, y0 = ed.hueCY + std::sin(a0) * ed.hueRI;
                    float x1 = ed.hueCX + std::cos(a1) * ed.hueRI, y1 = ed.hueCY + std::sin(a1) * ed.hueRI;
                    float x2 = ed.hueCX + std::cos(a1) * ed.hueRO, y2 = ed.hueCY + std::sin(a1) * ed.hueRO;
                    float x3 = ed.hueCX + std::cos(a0) * ed.hueRO, y3 = ed.hueCY + std::sin(a0) * ed.hueRO;
                    float v6[6][7] = { {x0,y0,0,rr,gg,bb,1},{x1,y1,0,rr,gg,bb,1},{x2,y2,0,rr,gg,bb,1},
                                        {x0,y0,0,rr,gg,bb,1},{x2,y2,0,rr,gg,bb,1},{x3,y3,0,rr,gg,bb,1} };
                    for (auto& e : v6) for (int ii = 0; ii < 7; ii++) verts.push_back(e[ii]);
                }
                float ca = ch * 6.2831853f;
                float hx = ed.hueCX + std::cos(ca) * (ed.hueRI + ed.hueRO) * 0.5f;
                float hy = ed.hueCY + std::sin(ca) * (ed.hueRI + ed.hueRO) * 0.5f;
                rect(hx - 2, hy - 2, 4, 4, 1, 1, 1, 1);
            }
            if (ed.tool == 1) {
                ensureBrushMat();
                float bmX = ed.brushMat[0], bmY = ed.brushMat[1];
                float mw = ed.brushMat[2];
                auto drawChecker = [&](float x, float y, float s) {
                    const int n = 4;
                    float cs = s / (float)n;
                    for (int cy = 0; cy < n; cy++) for (int cx = 0; cx < n; cx++) {
                        bool ck = ((cx + cy) & 1) != 0;
                        float g = ck ? 0.55f : 0.72f;
                        rect(x + cx * cs, y + cy * cs, cs + 0.4f, cs + 0.4f, g, g, g, 1);
                    }
                };
                for (int k = 0; k < 16; k++) {
                    int cxi = k % 4, cyi = k / 4;
                    float bx2 = bmX + cxi * (entBmS + 4.0f), by2 = bmY + cyi * (entBmS + 4.0f);
                    bool set = ed.brushSet[k] != 0;
                    bool sel = ed.brushSel[k] != 0;
                    if (set) {
                        drawChecker(bx2, by2, entBmS);
                        rect(bx2, by2, entBmS, entBmS, ed.brushColors[k].r / 255.0f, ed.brushColors[k].g / 255.0f,
                             ed.brushColors[k].b / 255.0f, ed.brushColors[k].a / 255.0f);
                    } else if (sel) {
                        drawChecker(bx2, by2, entBmS);
                        rect(bx2, by2, entBmS, entBmS, ed.r, ed.g, ed.b, ed.a);
                    } else {
                        rect(bx2, by2, entBmS, entBmS, 0.12f, 0.12f, 0.12f, 1);
                    }
                    if (sel) {
                        rect(bx2 - 1, by2 - 1, entBmS + 2, 1, 1, 1, 0, 1);
                        rect(bx2 - 1, by2 + entBmS, entBmS + 2, 1, 1, 1, 0, 1);
                        rect(bx2 - 1, by2 - 1, 1, entBmS + 2, 1, 1, 0, 1);
                        rect(bx2 + entBmS, by2 - 1, 1, entBmS + 2, 1, 1, 0, 1);
                    }
                }
                const float btnY = bmY + mw + 10.0f;
                const float ckS = 16.0f, ckX = bmX, ckY = btnY + 4.0f;
                const float ckHitW = 58.0f;
                bool ckHov = mx >= ckX && mx < ckX + ckHitW && my >= ckY && my < ckY + ckS;
                rect(ckX, ckY, ckS, ckS, 0.18f, 0.18f, 0.18f, 1);
                rect(ckX, ckY, ckS, 1, 0.7f, 0.7f, 0.7f, 1);
                rect(ckX, ckY + ckS - 1, ckS, 1, 0.7f, 0.7f, 0.7f, 1);
                rect(ckX, ckY, 1, ckS, 0.7f, 0.7f, 0.7f, 1);
                rect(ckX + ckS - 1, ckY, 1, ckS, 0.7f, 0.7f, 0.7f, 1);
                if (ed.pickAlpha0) {
                    rect(ckX + 3, ckY + 3, ckS - 6, ckS - 6, 0.35f, 0.85f, 0.4f, 1);
                }
                if (ckHov) rect(ckX - 1, ckY - 1, ckS + 2, ckS + 2, 1, 1, 1, 1);
                bool anySel = false;
                for (int k = 0; k < 16; k++) if (ed.brushSel[k]) { anySel = true; break; }
                if (anySel) {
                    float crx = bmX + mw - 52.0f, chkx = bmX + mw - 24.0f;
                    bool crsHov = mx >= crx && mx < crx + 24.0f && my >= btnY && my < btnY + 24.0f;
                    bool chkHov = mx >= chkx && mx < chkx + 24.0f && my >= btnY && my < btnY + 24.0f;
                    rect(crx, btnY, 24, 24, 0.7f, 0.2f, 0.2f, 1);
                    if (crsHov) rect(crx - 1, btnY - 1, 26, 26, 1, 1, 1, 1);
                    rect(chkx, btnY, 24, 24, 0.2f, 0.7f, 0.2f, 1);
                    if (chkHov) rect(chkx - 1, btnY - 1, 26, 26, 1, 1, 1, 1);
                }
            }
        } else {
            // ---- item model editor: left = editable model, right = apply-rand result ----
            std::vector<Vertex> previewMesh;
            auto appendBatchVert = [](std::vector<float>& dst, const Vertex& v) {
                dst.push_back(v.px); dst.push_back(v.py); dst.push_back(v.pz);
                float s = v.faceShade;
                dst.push_back(s); dst.push_back(s); dst.push_back(s); dst.push_back(1.0f);
                dst.push_back(v.u); dst.push_back(v.v);
            };
            auto emitEditQuad = [&](const mat::Quad& q) {
                unsigned tex = glTexForName(q.tex.empty() ? defaultTexFor(ed.modelBlock) : q.tex);
                int wrap = (q.uvMode == 1 || q.crop) ? 1 : 0;
                TexBatch* batch = nullptr;
                for (TexBatch& it : leftBatches) if (it.tex == tex && it.wrap == wrap) { batch = &it; break; }
                if (!batch) {
                    leftBatches.push_back({});
                    batch = &leftBatches.back();
                    batch->tex = tex;
                    batch->wrap = wrap;
                }
                float shade = (q.face >= 0 && q.face < 6) ? geo::kFaces[q.face].shade : 1.0f;
                Vertex vv[4];
                for (int c = 0; c < 4; c++) {
                    vv[c] = { q.p[c][0] - 0.5f, q.p[c][1], q.p[c][2] - 0.5f,
                              q.uv[c][0], q.uv[c][1], 0, 1, 0, shade, 1, 1 };
                    previewMesh.push_back(vv[c]);
                }
                int tris[] = { 0,1,2, 0,2,3 };
                for (int t = 0; t < 6; t++) appendBatchVert(batch->verts, vv[tris[t]]);
                if (q.doubleSided) {
                    int back[] = { 0,2,1, 0,3,2 };
                    for (int t = 0; t < 6; t++) appendBatchVert(batch->verts, vv[back[t]]);
                }
            };
            if (editModel.ok()) {
                for (const mat::Quad& q : editModel.quads) emitEditQuad(q);
            } else if (editModel.cube || (editModel.solids.empty() && !mat::modelHasSolidTex(editModel))) {
                mat::buildBlockPreviewMesh((uint8_t)ed.modelBlock, editModel, previewMesh);
                TexBatch b;
                b.tex = atlasTex;
                appendMesh(b.verts, previewMesh);
                leftBatches.push_back(std::move(b));
            }
            if (editModel.ok() || !editModel.cube) {
                std::vector<mat::Quad> solidQuads;
                for (const mat::Solid& s : editModel.solids) mat::appendSolidQuads(s, solidQuads);
                for (const mat::Quad& q : solidQuads) emitEditQuad(q);
            }
            float pivX = leftPivX, pivY = leftPivY, pivZ = leftPivZ;
            mvp3d = leftMvp;

            line(pivX, pivY, pivZ, pivX + 1, pivY, pivZ, 1, 0.2f, 0.2f, 1);
            line(pivX, pivY, pivZ, pivX, pivY + 1, pivZ, 0.2f, 1, 0.2f, 1);
            line(pivX, pivY, pivZ, pivX, pivY, pivZ + 1, 0.2f, 0.3f, 1, 1);
            float gstep = (ed.modelTool == 6) ? kTinyS : 0.25f;
            for (float gx = -0.5f; gx <= 0.5f + 1e-4f; gx += gstep) {
                line(gx, 0, -0.5f, gx, 0, 0.5f, 0.35f, 0.35f, 0.35f, 0.5f);
                line(-0.5f, 0, gx, 0.5f, 0, gx, 0.35f, 0.35f, 0.35f, 0.5f);
            }
            if (ed.modelTool == 6 && inLeft3d) {
                Vec3 ro, rd;
                leftRay(mx, my, ro, rd);
                float ox, oy, oz, texS;
                if (pickTinyOrigin(ro, rd, ox, oy, oz, texS)) {
                    float x0 = ox - 0.5f, y0 = oy, z0 = oz - 0.5f, s = kTinyS;
                    float x1 = x0 + s, y1 = y0 + s, z1 = z0 + s;
                    const float pr = 0.35f, pg = 1.0f, pb = 0.55f;
                    line(x0, y0, z0, x1, y0, z0, pr, pg, pb, 1);
                    line(x1, y0, z0, x1, y0, z1, pr, pg, pb, 1);
                    line(x1, y0, z1, x0, y0, z1, pr, pg, pb, 1);
                    line(x0, y0, z1, x0, y0, z0, pr, pg, pb, 1);
                    line(x0, y1, z0, x1, y1, z0, pr, pg, pb, 1);
                    line(x1, y1, z0, x1, y1, z1, pr, pg, pb, 1);
                    line(x1, y1, z1, x0, y1, z1, pr, pg, pb, 1);
                    line(x0, y1, z1, x0, y1, z0, pr, pg, pb, 1);
                    line(x0, y0, z0, x0, y1, z0, pr, pg, pb, 1);
                    line(x1, y0, z0, x1, y1, z0, pr, pg, pb, 1);
                    line(x1, y0, z1, x1, y1, z1, pr, pg, pb, 1);
                    line(x0, y0, z1, x0, y1, z1, pr, pg, pb, 1);
                }
            }

            if (editModel.ok()) {
                selectedIndices();
                for (int qi = 0; qi < (int)editModel.quads.size(); qi++) {
                    mat::Quad& q = editModel.quads[qi];
                    bool inSel = (qi < (int)selMark.size() && selMark[qi]);
                    bool prim = (qi == ed.selQuad);
                    float cr = prim ? 1.0f : (inSel ? 1.0f : 0.55f);
                    float cg = prim ? 0.9f : (inSel ? 0.75f : 0.55f);
                    float cb = prim ? 0.2f : (inSel ? 0.25f : 0.55f);
                    float X[4], Y[4], Z[4];
                    for (int c = 0; c < 4; c++) {
                        X[c] = q.p[c][0] - 0.5f;
                        Y[c] = q.p[c][1];
                        Z[c] = q.p[c][2] - 0.5f;
                    }
                    for (int c = 0; c < 4; c++) line(X[c], Y[c], Z[c], X[(c + 1) % 4], Y[(c + 1) % 4], Z[(c + 1) % 4], cr, cg, cb, 1);
                }
            } else if (editModel.cube || editModel.solids.empty()) {
                float c[8][3] = { {-0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f},{0.5f,-0.5f,0.5f},{-0.5f,-0.5f,0.5f},
                                  {-0.5f,0.5f,-0.5f},{0.5f,0.5f,-0.5f},{0.5f,0.5f,0.5f},{-0.5f,0.5f,0.5f} };
                int e[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7} };
                for (int i = 0; i < 12; i++) line(c[e[i][0]][0],c[e[i][0]][1],c[e[i][0]][2], c[e[i][1]][0],c[e[i][1]][1],c[e[i][1]][2], 0.5f,0.5f,0.5f,1);
            }

            mat::emitSolidMesh(editModel.solids, itemSolid, [](float x, float y, float z) {
                return Vec3{ x - 0.5f, y, z - 0.5f };
            }, true, 1.0f, true);
            if (itemSheet.ok() && itemSheetGL) {
                TexBatch sheet;
                sheet.tex = itemSheetGL;
                auto pushSheet = [&](std::vector<float>& dst, auto&& xform) {
                    const float SW = (float)itemSheet.w, SH = (float)itemSheet.h;
                    for (const mat::Solid& s : editModel.solids) {
                        if (!mat::solidHasBox(s)) continue;
                        for (int f = 0; f < 6; f++) {
                            float x0 = -s.h[0], x1 = s.h[0], y0 = -s.h[1], y1 = s.h[1], z0 = -s.h[2], z1 = s.h[2];
                            Vec3 ql[4];
                            switch (f) {
                                case 0: ql[0] = {x1,y0,z1}; ql[1] = {x1,y0,z0}; ql[2] = {x1,y1,z0}; ql[3] = {x1,y1,z1}; break;
                                case 1: ql[0] = {x0,y0,z0}; ql[1] = {x0,y0,z1}; ql[2] = {x0,y1,z1}; ql[3] = {x0,y1,z0}; break;
                                case 2: ql[0] = {x0,y1,z0}; ql[1] = {x1,y1,z0}; ql[2] = {x1,y1,z1}; ql[3] = {x0,y1,z1}; break;
                                case 3: ql[0] = {x0,y0,z1}; ql[1] = {x1,y0,z1}; ql[2] = {x1,y0,z0}; ql[3] = {x0,y0,z0}; break;
                                case 4: ql[0] = {x0,y0,z1}; ql[1] = {x1,y0,z1}; ql[2] = {x1,y1,z1}; ql[3] = {x0,y1,z1}; break;
                                default:ql[0] = {x1,y0,z0}; ql[1] = {x0,y0,z0}; ql[2] = {x0,y1,z0}; ql[3] = {x1,y1,z0}; break;
                            }
                            int px, py, fw, fh;
                            pm::boxFacePx(s.boxX, s.boxY, s.boxW, s.boxH, s.boxD, f, px, py, fw, fh);
                            float u0 = (float)px / SW, v0 = (float)py / SH;
                            float u1 = (float)(px + fw) / SW, v1 = (float)(py + fh) / SH;
                            float uv[4][2] = { {u0, v1}, {u1, v1}, {u1, v0}, {u0, v0} };
                            Vec3 q[4];
                            for (int c = 0; c < 4; c++) {
                                Vec3 w = mat::solidEuler(ql[c], s.rot);
                                q[c] = xform(s.c[0] + w.x, s.c[1] + w.y, s.c[2] + w.z);
                            }
                            int tris[6] = { 0, 1, 2, 0, 2, 3 };
                            for (int t = 0; t < 6; t++) {
                                int c = tris[t];
                                dst.push_back(q[c].x); dst.push_back(q[c].y); dst.push_back(q[c].z);
                                dst.push_back(1); dst.push_back(1); dst.push_back(1); dst.push_back(1);
                                dst.push_back(uv[c][0]); dst.push_back(uv[c][1]);
                            }
                        }
                    }
                };
                pushSheet(sheet.verts, [](float x, float y, float z) { return Vec3{ x - 0.5f, y, z - 0.5f }; });
                if (!sheet.verts.empty()) leftBatches.push_back(std::move(sheet));
            }
            auto boxLine = [&](const float mn[3], const float mx[3], float r, float g, float b) {
                float x0 = mn[0] - 0.5f, y0 = mn[1], z0 = mn[2] - 0.5f;
                float x1 = mx[0] - 0.5f, y1 = mx[1], z1 = mx[2] - 0.5f;
                line(x0, y0, z0, x1, y0, z0, r, g, b, 1);
                line(x1, y0, z0, x1, y0, z1, r, g, b, 1);
                line(x1, y0, z1, x0, y0, z1, r, g, b, 1);
                line(x0, y0, z1, x0, y0, z0, r, g, b, 1);
                line(x0, y1, z0, x1, y1, z0, r, g, b, 1);
                line(x1, y1, z0, x1, y1, z1, r, g, b, 1);
                line(x1, y1, z1, x0, y1, z1, r, g, b, 1);
                line(x0, y1, z1, x0, y1, z0, r, g, b, 1);
                line(x0, y0, z0, x0, y1, z0, r, g, b, 1);
                line(x1, y0, z0, x1, y1, z0, r, g, b, 1);
                line(x1, y0, z1, x1, y1, z1, r, g, b, 1);
                line(x0, y0, z1, x0, y1, z1, r, g, b, 1);
            };
            for (int i = 0; i < (int)editModel.solids.size(); i++) {
                const mat::Solid& s = editModel.solids[i];
                float mn[3] = { s.c[0] - s.h[0], s.c[1] - s.h[1], s.c[2] - s.h[2] };
                float mx[3] = { s.c[0] + s.h[0], s.c[1] + s.h[1], s.c[2] + s.h[2] };
                bool sel = (i == ed.selSolid) || (i < (int)solidMark.size() && solidMark[i]);
                boxLine(mn, mx, sel ? 1.0f : 0.75f, sel ? 0.85f : 0.65f, sel ? 0.25f : 0.45f);
            }
            if (ed.partAddVis && (ed.partAdd || ed.partMeasure || ed.hairPaint || ed.hairCard || ed.hairErase || ed.partCut))
                boxLine(ed.partMn, ed.partMx, 0.35f, 1.0f, 0.55f);
            if (ed.measureOn) {
                line(ed.measureA[0] - 0.5f, ed.measureA[1], ed.measureA[2] - 0.5f,
                     ed.measureB[0] - 0.5f, ed.measureB[1], ed.measureB[2] - 0.5f,
                     1.0f, 0.92f, 0.25f, 1);
            }

            if (ed.modelTool >= 0 && ed.modelTool <= 5 &&
                (editModel.ok() || (ed.selSolid >= 0 && ed.selSolid < (int)editModel.solids.size() && selectedIndices().empty()))) {
                auto pushGz = [&](float x, float y, float r, float gcol, float b, float a) {
                    gizmoOverlay.push_back(x); gizmoOverlay.push_back(y); gizmoOverlay.push_back(0);
                    gizmoOverlay.push_back(r); gizmoOverlay.push_back(gcol); gizmoOverlay.push_back(b); gizmoOverlay.push_back(a);
                };
                auto gzLine2 = [&](float x0, float y0, float x1, float y1, float r, float gcol, float b) {
                    pushGz(x0, y0, r, gcol, b, 1); pushGz(x1, y1, r, gcol, b, 1);
                };
                auto gzHead = [&](float tx, float ty, float fx, float fy, float sz, float r, float gcol, float b) {
                    float dx = tx - fx, dy = ty - fy;
                    float len = std::hypot(dx, dy);
                    if (len < 1.0f) return;
                    dx /= len; dy /= len;
                    float px = -dy, py = dx;
                    float bx = tx - dx * sz, by = ty - dy * sz;
                    gzLine2(tx, ty, bx + px * sz * 0.55f, by + py * sz * 0.55f, r, gcol, b);
                    gzLine2(tx, ty, bx - px * sz * 0.55f, by - py * sz * 0.55f, r, gcol, b);
                    gzLine2(bx + px * sz * 0.55f, by + py * sz * 0.55f, bx - px * sz * 0.55f, by - py * sz * 0.55f, r, gcol, b);
                };
                auto gzArrow = [&](const Vec3& pos, const Vec3& out, float alen, bool hot, float r, float gc, float b) {
                    if (hot) { r = 1; gc = 1; b = 0.35f; }
                    Vec3 a0 = pos - out * alen, a1 = pos + out * alen;
                    float sx0, sy0, sx1, sy1;
                    if (!projectView(a0.x, a0.y, a0.z, sx0, sy0) || !projectView(a1.x, a1.y, a1.z, sx1, sy1)) return;
                    gzLine2(sx0, sy0, sx1, sy1, r, gc, b);
                    gzHead(sx1, sy1, sx0, sy0, hot ? 14.0f : 11.0f, r, gc, b);
                    gzHead(sx0, sy0, sx1, sy1, hot ? 14.0f : 11.0f, r, gc, b);
                };
                bool solidStretchDraw = (ed.modelTool == 1 && isSolidGroup(ed.selQuad));
                bool partStretchDraw = ed.modelTool == 1 && selectedIndices().empty() &&
                    ed.selSolid >= 0 && ed.selSolid < (int)editModel.solids.size();
                if (ed.modelTool == 0 || ed.modelTool == 3 || ed.modelTool == 4 || solidStretchDraw || partStretchDraw) {
                Vec3 g; gizmoCenterView(g);
                float L = gizmoSize();
                float R = L * 0.72f;
                const float col[3][3] = { {1.0f,0.28f,0.28f},{0.28f,0.92f,0.32f},{0.32f,0.45f,1.0f} };
                int hov = ed.gizmoHover;
                bool drawTrans = (ed.modelTool == 0 || ed.modelTool == 4 || solidStretchDraw || partStretchDraw);
                bool drawRot = (ed.modelTool == 0 || ed.modelTool == 3);
                for (int a = 0; a < 3; a++) {
                    bool hotT = (hov == 1 + a) || (hov == 10 + a * 2) || (hov == 11 + a * 2);
                    bool hotR = (hov == 4 + a) || (hov == 20 + a * 2) || (hov == 21 + a * 2);
                    if (drawTrans) {
                    float r = col[a][0], gc = col[a][1], b = col[a][2];
                    if (hotT) { r = 1; gc = 1; b = 0.35f; }
                    Vec3 p0 = { g.x - kAxis[a].x * L, g.y - kAxis[a].y * L, g.z - kAxis[a].z * L };
                    Vec3 p1 = { g.x + kAxis[a].x * L, g.y + kAxis[a].y * L, g.z + kAxis[a].z * L };
                    float sx0, sy0, sx1, sy1;
                    if (projectView(p0.x, p0.y, p0.z, sx0, sy0) && projectView(p1.x, p1.y, p1.z, sx1, sy1)) {
                        gzLine2(sx0, sy0, sx1, sy1, r, gc, b);
                        gzHead(sx1, sy1, sx0, sy0, hotT ? 16.0f : 13.0f, r, gc, b);
                        gzHead(sx0, sy0, sx1, sy1, hotT ? 16.0f : 13.0f, r, gc, b);
                    }
                    }
                    if (!drawRot) continue;
                    float rr = hotR ? 1.0f : col[a][0], rg = hotR ? 1.0f : col[a][1], rb = hotR ? 0.35f : col[a][2];
                    Vec3 u, vv; rotBasis(a, u, vv);
                    const int segs = 28;
                    float a0 = -2.3f, a1 = 2.3f;
                    float px, py;
                    bool prevOk = false;
                    for (int i = 0; i <= segs; i++) {
                        float ang = a0 + (a1 - a0) * (float)i / (float)segs;
                        Vec3 q = { g.x + (u.x * std::cos(ang) + vv.x * std::sin(ang)) * R,
                                   g.y + (u.y * std::cos(ang) + vv.y * std::sin(ang)) * R,
                                   g.z + (u.z * std::cos(ang) + vv.z * std::sin(ang)) * R };
                        float sx, sy;
                        bool ok = projectView(q.x, q.y, q.z, sx, sy);
                        if (ok && prevOk) gzLine2(px, py, sx, sy, rr, rg, rb);
                        if (ok) { px = sx; py = sy; }
                        prevOk = ok;
                    }
                    auto rotHead = [&](float ang, float dir) {
                        Vec3 hp = { g.x + (u.x * std::cos(ang) + vv.x * std::sin(ang)) * R,
                                    g.y + (u.y * std::cos(ang) + vv.y * std::sin(ang)) * R,
                                    g.z + (u.z * std::cos(ang) + vv.z * std::sin(ang)) * R };
                        float dang = 0.22f * dir;
                        Vec3 from = { g.x + (u.x * std::cos(ang - dang) + vv.x * std::sin(ang - dang)) * R,
                                      g.y + (u.y * std::cos(ang - dang) + vv.y * std::sin(ang - dang)) * R,
                                      g.z + (u.z * std::cos(ang - dang) + vv.z * std::sin(ang - dang)) * R };
                        float hx, hy, fx, fy;
                        if (projectView(hp.x, hp.y, hp.z, hx, hy) && projectView(from.x, from.y, from.z, fx, fy))
                            gzHead(hx, hy, fx, fy, hotR ? 16.0f : 13.0f, rr, rg, rb);
                    };
                    rotHead(a0, -1.0f);
                    rotHead(a1, 1.0f);
                }
                } else if (ed.modelTool == 1 || ed.modelTool == 2) {
                    Vec3 corner[4], mid[4], cOut[4], eOut[4], n;
                    if (deformGeom(ed.selQuad, corner, mid, cOut, eOut, n)) {
                        float alen = ed.bzoom * 0.07f;
                        if (alen < 0.06f) alen = 0.06f;
                        if (alen > 0.18f) alen = 0.18f;
                        int hov = ed.gizmoHover;
                        for (int c = 0; c < 4; c++) {
                            gzArrow(corner[c], cOut[c], alen, hov == 40 + c, 1.0f, 0.62f, 0.18f);
                            gzArrow(mid[c], eOut[c], alen, hov == 50 + c, 0.25f, 0.85f, 1.0f);
                        }
                    }
                } else if (ed.modelTool == 5) {
                    Vec3 o, u, v;
                    if (uvGeom(ed.selQuad, o, u, v)) {
                        float L = gizmoSize() * 0.55f;
                        int hov = ed.gizmoHover;
                        gzArrow(o, u, L, hov == 60, 1.0f, 0.55f, 0.2f);
                        gzArrow(o, v, L, hov == 61, 0.3f, 0.85f, 1.0f);
                    }
                }
            }

            // Right pane: game-identical mesh after applying .rand (R / button re-rolls seed).
            std::vector<Vertex> randMesh;
            mat::buildRandAppliedMesh((uint8_t)ed.modelBlock, editModel, editRand, ed.randWx, ed.randWz, randMesh);
            float rpX, rpY, rpZ;
            meshPivot(randMesh, rpX, rpY, rpZ);
            if (randMesh.empty() && !editModel.solids.empty()) {
                rpX = rpY = rpZ = 0.0f;
                for (const mat::Solid& s : editModel.solids) {
                    rpX += s.c[0] - 0.5f; rpY += s.c[1] - 0.5f; rpZ += s.c[2] - 0.5f;
                }
                float n = (float)editModel.solids.size();
                rpX /= n; rpY /= n; rpZ /= n;
            }
            float aspectR = (paneRH > 1.0f) ? (paneRW / paneRH) : 1.0f;
            mvpRand = orbitMvp(aspectR, ed.rrotY, ed.rrotX, ed.rzoom, rpX, rpY, rpZ);
            appendMesh(randTexVerts, randMesh);
            mat::emitSolidMesh(editModel.solids, itemSolidRand, [](float x, float y, float z) {
                return Vec3{ x - 0.5f, y - 0.5f, z - 0.5f };
            }, true);
            rline(rpX, rpY, rpZ, rpX + 1, rpY, rpZ, 1, 0.2f, 0.2f, 1);
            rline(rpX, rpY, rpZ, rpX, rpY + 1, rpZ, 0.2f, 1, 0.2f, 1);
            rline(rpX, rpY, rpZ, rpX, rpY, rpZ + 1, 0.2f, 0.3f, 1, 1);
            // unit-cell footprint so the applied result reads as one block
            rline(0,0,0, 1,0,0, 0.4f,0.4f,0.4f,0.7f); rline(1,0,0, 1,0,1, 0.4f,0.4f,0.4f,0.7f);
            rline(1,0,1, 0,0,1, 0.4f,0.4f,0.4f,0.7f); rline(0,0,1, 0,0,0, 0.4f,0.4f,0.4f,0.7f);

            // split backgrounds + divider + headers (middle Model tools, right Apply Rand)
            rect(paneRX, kTopH, paneRW, (float)g_winH - kTopH, 0.07f, 0.08f, 0.10f, 1);
            rect(splitX - 1, kTopH, 2, (float)g_winH - kTopH, 0.25f, 0.25f, 0.28f, 1);
            rect(paneLX, kTopH, paneLW, kHdrH, 0.10f, 0.11f, 0.13f, 1);
            rect(paneLX, kTopH + kHdrH, paneLW, kToolH, 0.09f, 0.10f, 0.12f, 1);
            if (partColW > 1.0f)
                rect(partColX, paneLY, partColW, paneLH, 0.08f, 0.09f, 0.11f, 1);
            for (int i = 0; i < 13; i++) {
                float bx = partToolRect[i][0], by = partToolRect[i][1], bw = partToolRect[i][2], bh = partToolRect[i][3];
                if (bw < 1.0f) continue;
                bool hov = (partToolHover == i);
                bool on = (i == 0 && ed.partAdd) || (i == 1 && ed.hairSelect) || (i == 2 && ed.hairPaint) ||
                    (i == 3 && ed.hairCard) || (i == 4 && ed.hairErase) || (i == 10 && ed.partCut) ||
                    (i == 11 && ed.partMeasure) || (i == 12 && ed.partPlane);
                rect(bx, by, bw, bh, on ? 0.38f : (hov ? 0.28f : 0.16f), on ? 0.48f : (hov ? 0.32f : 0.17f), on ? 0.28f : (hov ? 0.22f : 0.16f), 1);
                if (hov) rect(bx - 2, by - 2, bw + 4, bh + 4, 1, 1, 1, 1);
            }
            for (int i = 0; i < 8; i++) {
                float bx = partChipRect[i][0], by = partChipRect[i][1], bw = partChipRect[i][2], bh = partChipRect[i][3];
                if (bw < 1.0f) continue;
                rect(bx, by, bw, bh, kPaintPal[i][0], kPaintPal[i][1], kPaintPal[i][2], 1);
                bool on = std::fabs(ed.r - kPaintPal[i][0]) < 0.02f && std::fabs(ed.g - kPaintPal[i][1]) < 0.02f && std::fabs(ed.b - kPaintPal[i][2]) < 0.02f;
                if (on || partToolHover == 20 + i) {
                    float fr = on ? 1.0f : 0.85f, fg = on ? 0.85f : 0.85f, fb = on ? 0.2f : 0.85f;
                    rect(bx - 2, by - 2, bw + 4, 2, fr, fg, fb, 1);
                    rect(bx - 2, by + bh, bw + 4, 2, fr, fg, fb, 1);
                    rect(bx - 2, by - 2, 2, bh + 4, fr, fg, fb, 1);
                    rect(bx + bw, by - 2, 2, bh + 4, fr, fg, fb, 1);
                }
            }
            for (int i = 0; i < kNMTools; i++) {
                float bx = mtoolRect[i][0], by = mtoolRect[i][1], bw = mtoolRect[i][2], bh = mtoolRect[i][3];
                bool hov = (ed.modelToolHover == i);
                bool on = (i == 2 && ed.modelTool == 0) || (i == 5 && ed.modelTool == 3)
                    || (i == 6 && ed.modelTool == 1) || (i == 7 && ed.modelTool == 2)
                    || (i == 8 && ed.modelTool == 4) || (i == 9 && ed.modelTool == 5)
                    || (i == 10 && ed.modelTool == 7);
                float r = on ? 0.38f : (hov ? 0.28f : 0.16f);
                float g = on ? 0.48f : (hov ? 0.32f : 0.17f);
                float b = on ? 0.28f : (hov ? 0.22f : 0.16f);
                rect(bx, by, bw, bh, r, g, b, 1);
                if (hov) rect(bx - 2, by - 2, bw + 4, bh + 4, 1, 1, 1, 1);
            }
            rect(paneLX, matBarY, paneLW, kMatBarH, 0.10f, 0.11f, 0.13f, 1);
            rect(faceX, midBtnY, kFaceW, kBtnH, faceHov ? 0.32f : 0.18f, faceHov ? 0.40f : 0.20f, faceHov ? 0.28f : 0.18f, 1);
            if (faceHov) rect(faceX - 2, midBtnY - 2, kFaceW + 4, kBtnH + 4, 1, 1, 1, 1);
            rect(blkX, midBtnY, kBlkW, kBtnH, blkHov ? 0.32f : 0.18f, blkHov ? 0.40f : 0.20f, blkHov ? 0.28f : 0.18f, 1);
            if (blkHov) rect(blkX - 2, midBtnY - 2, kBlkW + 4, kBtnH + 4, 1, 1, 1, 1);
            rect(smX, midBtnY, kSmW, kBtnH, smHov ? 0.32f : 0.18f, smHov ? 0.40f : 0.20f, smHov ? 0.28f : 0.18f, 1);
            if (smHov) rect(smX - 2, midBtnY - 2, kSmW + 4, kBtnH + 4, 1, 1, 1, 1);
            bool tinyOn = (ed.modelTool == 6);
            rect(tinyX, midBtnY, kTinyW, kBtnH,
                tinyOn ? 0.38f : (tinyHov ? 0.32f : 0.18f),
                tinyOn ? 0.48f : (tinyHov ? 0.40f : 0.20f),
                tinyOn ? 0.28f : (tinyHov ? 0.28f : 0.18f), 1);
            if (tinyHov) rect(tinyX - 2, midBtnY - 2, kTinyW + 4, kBtnH + 4, 1, 1, 1, 1);
            rect(midSaveX, midBtnY, kSaveS, kSaveS, midSaveHov ? 0.30f : 0.15f, midSaveHov ? 0.45f : 0.15f, midSaveHov ? 0.18f : 0.15f, 1);
            if (midSaveHov) rect(midSaveX - 2, midBtnY - 2, kSaveS + 4, kSaveS + 4, 1, 1, 1, 1);
            rect(midUndoX, midBtnY, kSaveS, kSaveS, midUndoHov ? 0.30f : 0.15f, midUndoHov ? 0.45f : 0.15f, midUndoHov ? 0.18f : 0.15f, 1);
            if (midUndoHov) rect(midUndoX - 2, midBtnY - 2, kSaveS + 4, kSaveS + 4, 1, 1, 1, 1);
            std::string selTexName = ed.selMat;
            if (!editModel.quads.empty()) selTexName = editModel.quads[ed.selQuad].tex.empty() ? ed.selMat : editModel.quads[ed.selQuad].tex;
            for (int i = 0; i < nMatItems; i++) {
                float mx0, my0;
                matItemRect(i, mx0, my0);
                bool hov = (ed.matHover == i);
                bool addSlot = (i == nMatItems - 1);
                bool sel = false;
                if (!addSlot) {
                    std::string nm = (i < TEX_COUNT) ? mat::tileName(i) : extraMats[i - TEX_COUNT].name;
                    sel = (nm == selTexName);
                }
                rect(mx0, my0, kMatThumb, kMatThumb, addSlot ? 0.18f : 0.08f, addSlot ? 0.18f : 0.08f, addSlot ? 0.20f : 0.10f, 1);
                if (sel) {
                    rect(mx0 - 2, my0 - 2, kMatThumb + 4, 2, 1.0f, 0.85f, 0.2f, 1);
                    rect(mx0 - 2, my0 + kMatThumb, kMatThumb + 4, 2, 1.0f, 0.85f, 0.2f, 1);
                    rect(mx0 - 2, my0 - 2, 2, kMatThumb + 4, 1.0f, 0.85f, 0.2f, 1);
                    rect(mx0 + kMatThumb, my0 - 2, 2, kMatThumb + 4, 1.0f, 0.85f, 0.2f, 1);
                }
                if (hov) rect(mx0 - 1, my0 - 1, kMatThumb + 2, kMatThumb + 2, 1, 1, 1, 1);
            }
            rect(paneRX, kTopH, paneRW, kHdrH, 0.10f, 0.11f, 0.13f, 1);
            rect(arX, arY, kArW, kArH, applyRandHov ? 0.35f : 0.22f, applyRandHov ? 0.50f : 0.32f, applyRandHov ? 0.28f : 0.18f, 1);
            if (applyRandHov) rect(arX - 2, arY - 2, kArW + 4, kArH + 4, 1, 1, 1, 1);
            rect(saveX, saveY, kSaveS, kSaveS, saveHov ? 0.30f : 0.15f, saveHov ? 0.45f : 0.15f, saveHov ? 0.18f : 0.15f, 1);
            if (saveHov) rect(saveX - 2, saveY - 2, kSaveS + 4, kSaveS + 4, 1, 1, 1, 1);
            rect(cancelX, cancelY, kCancelW, kCancelH, cancelHov ? 0.45f : 0.22f, cancelHov ? 0.28f : 0.18f, cancelHov ? 0.22f : 0.18f, 1);
            if (cancelHov) rect(cancelX - 2, cancelY - 2, kCancelW + 4, kCancelH + 4, 1, 1, 1, 1);
            for (int i = 0; i < 2; i++) {
                float x = rightTab[i][0], y = rightTab[i][1], w = rightTab[i][2], h = rightTab[i][3];
                if (w < 1.0f) continue;
                bool on = ed.modelRightTab == i;
                bool hov = rightTabHover == i;
                if (on) rect(x, y, w, h + 2.0f, 0.07f, 0.08f, 0.10f, 1);
                else rect(x, y + 4.0f, w, h - 4.0f, hov ? 0.20f : 0.14f, hov ? 0.21f : 0.15f, hov ? 0.24f : 0.17f, 1);
            }
            if (ed.modelRightTab == 1) {
                for (int i = 0; i < 9; i++) {
                    float x = colorTool[i][0], y = colorTool[i][1], w = colorTool[i][2], h = colorTool[i][3];
                    if (w < 1.0f) continue;
                    bool on = (i == 3) ? ed.picker : (!ed.picker && ((i == 1) ? (ed.tool == 1 || ed.brushMod) : (ed.tool < 0 ? i == 0 : ed.tool == i)));
                    bool hov = colorToolHover == i;
                    rect(x, y, w, h, on ? 0.32f : (hov ? 0.24f : 0.16f), on ? 0.42f : (hov ? 0.26f : 0.16f), on ? 0.24f : (hov ? 0.20f : 0.16f), 1);
                }
                {
                    float x = colorCur[0], y = colorCur[1], w = colorCur[2], h = colorCur[3];
                    for (int i = 0; i < 8; i++) {
                        bool ck = (i & 1) != 0;
                        rect(x + i * (w / 8.0f), y, w / 8.0f + 0.4f, h, ck ? 0.45f : 0.62f, ck ? 0.45f : 0.62f, ck ? 0.45f : 0.62f, 1);
                    }
                    rect(x, y, w, h, ed.r, ed.g, ed.b, ed.a);
                }
                {
                    float ch, cs, cv;
                    rgbToHsv(ed.r, ed.g, ed.b, ch, cs, cv);
                    float svX = colorSv[0], svY = colorSv[1], svW = colorSv[2];
                    for (int j = 0; j < 16; j++) for (int i = 0; i < 16; i++) {
                        float s = (float)i / 15.0f, v = 1.0f - (float)j / 15.0f;
                        float rr, gg, bb; hsvToRgb(ch, s, v, rr, gg, bb);
                        rect(svX + i * (svW / 16.0f), svY + j * (svW / 16.0f), svW / 16.0f + 0.5f, svW / 16.0f + 0.5f, rr, gg, bb, 1);
                    }
                    rect(svX + cs * svW - 3, svY + (1.0f - cv) * svW - 3, 6, 6, 1, 1, 1, 1);
                    const int HS = 28;
                    for (int k = 0; k < HS; k++) {
                        float a0 = (float)k / HS * 6.2831853f, a1 = (float)(k + 1) / HS * 6.2831853f;
                        float rr, gg, bb; hsvToRgb((float)k / HS, 1, 1, rr, gg, bb);
                        float x0 = colorHueCX + std::cos(a0) * colorHueRI, y0 = colorHueCY + std::sin(a0) * colorHueRI;
                        float x1 = colorHueCX + std::cos(a1) * colorHueRI, y1 = colorHueCY + std::sin(a1) * colorHueRI;
                        float x2 = colorHueCX + std::cos(a1) * colorHueRO, y2 = colorHueCY + std::sin(a1) * colorHueRO;
                        float x3 = colorHueCX + std::cos(a0) * colorHueRO, y3 = colorHueCY + std::sin(a0) * colorHueRO;
                        float v6[6][7] = { {x0,y0,0,rr,gg,bb,1},{x1,y1,0,rr,gg,bb,1},{x2,y2,0,rr,gg,bb,1},
                                           {x0,y0,0,rr,gg,bb,1},{x2,y2,0,rr,gg,bb,1},{x3,y3,0,rr,gg,bb,1} };
                        for (auto& e : v6) for (int ii = 0; ii < 7; ii++) verts.push_back(e[ii]);
                    }
                    float ca = ch * 6.2831853f;
                    float hx = colorHueCX + std::cos(ca) * (colorHueRI + colorHueRO) * 0.5f;
                    float hy = colorHueCY + std::sin(ca) * (colorHueRI + colorHueRO) * 0.5f;
                    rect(hx - 3, hy - 3, 6, 6, 1, 1, 1, 1);
                }
                {
                    float alX = colorAlpha[0], alY = colorAlpha[1], alW = colorAlpha[2], alH = colorAlpha[3];
                    for (int i = 0; i < 12; i++) {
                        bool ck = (i % 2) != 0;
                        rect(alX + i * (alW / 12.0f), alY, alW / 12.0f, alH, ck ? 0.45f : 0.65f, ck ? 0.45f : 0.65f, ck ? 0.45f : 0.65f, 1);
                    }
                    rect(alX, alY, alW * ed.a, alH, 1, 1, 1, 0.85f);
                    rect(alX + alW * ed.a - 2, alY - 2, 4, alH + 4, 1, 1, 1, 1);
                }
                for (size_t k = 0; k < ed.recent.size() && k < 8; k++) {
                    float x = colorRecent[k][0], y = colorRecent[k][1], w = colorRecent[k][2], h = colorRecent[k][3];
                    rect(x, y, w, h, ed.recent[k].r / 255.0f, ed.recent[k].g / 255.0f, ed.recent[k].b / 255.0f, ed.recent[k].a / 255.0f);
                    if (colorRecentHover == (int)k) rect(x - 2, y - 2, w + 4, h + 4, 1, 1, 1, 1);
                }
            }

            if (showSliders) {
                rect(paneRX, sliderTop, paneRW, kSliderH, 0.11f, 0.12f, 0.14f, 1);
                for (int i = 0; i < kNRandSliders; i++) {
                    float lx, ly, tx, ty, tw, th, vx;
                    sliderTrack(i, lx, ly, tx, ty, tw, th, vx);
                    const RandSlider& spec = kRandSliders[i];
                    float v = editRand.get(spec.key, spec.def);
                    float t = (spec.maxv > spec.minv) ? (v - spec.minv) / (spec.maxv - spec.minv) : 0.0f;
                    if (t < 0) t = 0;
                    if (t > 1) t = 1;
                    rect(tx, ty, tw, th, 0.22f, 0.22f, 0.24f, 1);
                    rect(tx, ty, tw * t, th, 0.35f, 0.55f, 0.30f, 1);
                    rect(tx + tw * t - 2, ty - 3, 4, th + 6, 1, 1, 1, 1);
                }
            }

            // left selection panel background (icons drawn in the overlay)
            const float pX = 8.0f, pY = 52.0f, thumb = 36.0f, gap = 6.0f;
            const int cols = 4;
            int nRows = (liveBlockCount() - 1 + cols - 1) / cols;
            rect(pX - 4, pY - 4, cols * thumb + (cols - 1) * gap + 8, nRows * thumb + (nRows - 1) * gap + 8, 0.12f, 0.12f, 0.14f, 0.85f);
            for (int b = 1; b < liveBlockCount(); b++) {
                int i = b - 1;
                float cx = pX + (i % cols) * (thumb + gap);
                float cy = pY + (i / cols) * (thumb + gap);
                bool hov = (ed.modelHover == b);
                bool sel = (b == ed.modelBlock);
                if (sel) { // yellow selection border
                    rect(cx - 2, cy - 2, thumb + 4, 2, 1.0f, 0.85f, 0.2f, 1);
                    rect(cx - 2, cy + thumb, thumb + 4, 2, 1.0f, 0.85f, 0.2f, 1);
                    rect(cx - 2, cy - 2, 2, thumb + 4, 1.0f, 0.85f, 0.2f, 1);
                    rect(cx + thumb, cy - 2, 2, thumb + 4, 1.0f, 0.85f, 0.2f, 1);
                }
                if (hov) rect(cx - 1, cy - 1, thumb + 2, thumb + 2, 0.6f, 0.6f, 0.6f, 1);
            }
        }

        // Model-page overlay: top hint bar (ortho) drawn over the 3D viewport.
        if (startMode >= 0 && (ed.entityMode || ed.modelMode)) {
            rect(0, 0, (float)g_winW, 40, 0, 0, 0, 0.78f);
        }

        // EXIT button (bottom-right) background, shown in every active mode.
        if (startMode >= 0) {
            const float exW = 120.0f, exH = 44.0f, exM = 20.0f;
            float exX = g_winW - exW - exM, exY = g_winH - exH - exM;
            bool exHov = mx >= exX && mx < exX + exW && my >= exY && my < exY + exH;
            rect(exX, exY, exW, exH, exHov ? 0.55f : 0.30f, exHov ? 0.20f : 0.15f, exHov ? 0.18f : 0.15f, 1.0f);
            if (exHov) rect(exX - 2, exY - 2, exW + 4, exH + 4, 1, 1, 1, 1);
        }

        // upload + draw: 2D triangles (ortho, no depth writes), textured 3D mesh
        // (perspective, depth-tested), then 3D wireframe lines (perspective).
        gl::DepthMask(GL_FALSE);
        gl::BindVertexArray(vao);
        gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
        if (!verts.empty()) {
            gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size()*sizeof(float)), verts.data(), GL_STREAM_DRAW);
            gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
            gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 7));
        }
        gl::BindVertexArray(0);
        gl::DepthMask(GL_TRUE);

        auto drawPane3D = [&](float x, float y, float w, float h, const Mat4& mvpP,
                              const std::vector<float>* tverts, unsigned defaultTex,
                              const std::vector<TexBatch>* batches,
                              const std::vector<float>& lverts, bool depth,
                              const std::vector<float>* sverts = nullptr) {
            int vw = (int)w, vh = (int)h;
            int vx = (int)x, vy = g_winH - (int)(y + h);
            if (vw < 2 || vh < 2) return;
            gl::Viewport(vx, vy, vw, vh);
            gl::Enable(GL_DEPTH_TEST);
            gl::DepthMask(GL_TRUE);
            gl::Clear(GL_DEPTH_BUFFER_BIT);
            if (!depth) gl::Disable(GL_DEPTH_TEST);
            auto drawTV = [&](const std::vector<float>& tv, unsigned tex, int wrap) {
                if (tv.empty()) return;
                gl::BindVertexArray(tvao);
                gl::BindBuffer(GL_ARRAY_BUFFER, tvbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tv.size() * sizeof(float)), tv.data(), GL_STREAM_DRAW);
                gl::UseProgram(cprog);
                gl::UniformMatrix4fv(cuMVP, 1, GL_FALSE, mvpP.m);
                unsigned t = tex ? tex : atlasTex;
                gl::BindTexture(GL_TEXTURE_2D, t);
                int wmode = wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE;
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wmode);
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wmode);
                gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(tv.size() / 9));
                gl::UseProgram(prog);
                gl::BindVertexArray(0);
            };
            if (batches) {
                for (const TexBatch& b : *batches) drawTV(b.verts, b.tex, b.wrap);
            } else if (tverts) {
                drawTV(*tverts, defaultTex, 0);
            }
            if (sverts && !sverts->empty()) {
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sverts->size() * sizeof(float)), sverts->data(), GL_STREAM_DRAW);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvpP.m);
                gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(sverts->size() / 7));
                gl::BindVertexArray(0);
            }
            if (!lverts.empty()) {
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lverts.size() * sizeof(float)), lverts.data(), GL_STREAM_DRAW);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvpP.m);
                gl::DrawArrays(GL_LINES, 0, (GLsizei)(lverts.size() / 7));
                gl::BindVertexArray(0);
            }
            gl::Disable(GL_DEPTH_TEST);
            gl::Viewport(0, 0, g_winW, g_winH);
        };

        if (startMode >= 0 && ed.modelMode) {
            if (ed.modelRightTab == 1 && colorFaceVis) {
                Vec3 e1 = colorFaceQ[1] - colorFaceQ[0];
                Vec3 e2 = colorFaceQ[3] - colorFaceQ[0];
                Vec3 n = e1.cross(e2);
                float nl = n.length();
                if (nl > 1e-8f) n = n * (0.008f / nl);
                Vec3 q0 = colorFaceQ[0] + n, q1 = colorFaceQ[1] + n, q2 = colorFaceQ[2] + n, q3 = colorFaceQ[3] + n;
                line(q0.x, q0.y, q0.z, q1.x, q1.y, q1.z, 1.0f, 0.9f, 0.2f, 1);
                line(q1.x, q1.y, q1.z, q2.x, q2.y, q2.z, 1.0f, 0.9f, 0.2f, 1);
                line(q2.x, q2.y, q2.z, q3.x, q3.y, q3.z, 1.0f, 0.9f, 0.2f, 1);
                line(q3.x, q3.y, q3.z, q0.x, q0.y, q0.z, 1.0f, 0.9f, 0.2f, 1);
            }
            drawPane3D(paneLX, paneLY, viewLW, paneLH, mvp3d, nullptr, 0, &leftBatches, lineVerts, true, &itemSolid);
            if (ed.modelRightTab == 1)
                drawPane3D(paneRX, paneRY, colorViewW, paneRH, colorMvp, nullptr, 0, &leftBatches, lineVerts, true, &itemSolid);
            else
                drawPane3D(paneRX, paneRY, paneRW, paneRH, mvpRand, &randTexVerts, atlasTex, nullptr, randLineVerts, true, &itemSolidRand);
            if ((ed.hairPaint && paintSlideW > 1.0f) || (ed.modelRightTab == 1 && colorSlideW > 1.0f)) {
                verts.clear();
                auto drawStops = [&](float sx, float sy, float sw, bool hot) {
                    float trackY = sy + 5.0f;
                    rect(sx, trackY, sw, 6.0f, 0.18f, 0.18f, 0.20f, 1);
                    for (int i = 0; i < 4; i++) {
                        float x = sx + sw * ((float)i / 3.0f);
                        rect(x - 1.0f, trackY - 3.0f, 2.0f, 12.0f, 0.45f, 0.45f, 0.48f, 1);
                    }
                    float kx = sx + sw * ((float)ed.paintDiv / 3.0f) - 7.0f;
                    rect(kx, trackY - 5.0f, 14.0f, 16.0f, hot ? 0.95f : 0.80f, hot ? 0.85f : 0.70f, 0.25f, 1);
                };
                if (ed.hairPaint && paintSlideW > 1.0f) drawStops(paintSlideX, paintSlideY, paintSlideW, paintSlideHov);
                if (ed.modelRightTab == 1 && colorSlideW > 1.0f) drawStops(colorSlideX, colorSlideY, colorSlideW, colorSlideHov);
                gl::Disable(GL_DEPTH_TEST);
                gl::DepthMask(GL_FALSE);
                gl::Viewport(0, 0, g_winW, g_winH);
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STREAM_DRAW);
                gl::UseProgram(prog);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
                gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 7));
                gl::BindVertexArray(0);
                gl::DepthMask(GL_TRUE);
                verts.clear();
            }
            if (!gizmoOverlay.empty()) {
                gl::Disable(GL_DEPTH_TEST);
                gl::DepthMask(GL_FALSE);
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gizmoOverlay.size() * sizeof(float)), gizmoOverlay.data(), GL_STREAM_DRAW);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
                gl::DrawArrays(GL_LINES, 0, (GLsizei)(gizmoOverlay.size() / 7));
                gl::BindVertexArray(0);
                gl::DepthMask(GL_TRUE);
            }
        } else if (startMode >= 0 && ed.animMode) {
            drawPane3D(animVX, animVY, animVW, animVH, animMvp, nullptr, 0, &entBatches, lineVerts, true, &entSolid);
            if (!gizmoOverlay.empty()) {
                gl::Disable(GL_DEPTH_TEST);
                gl::DepthMask(GL_FALSE);
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gizmoOverlay.size() * sizeof(float)), gizmoOverlay.data(), GL_STREAM_DRAW);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
                gl::DrawArrays(GL_LINES, 0, (GLsizei)(gizmoOverlay.size() / 7));
                gl::BindVertexArray(0);
                gl::DepthMask(GL_TRUE);
            }
        } else if (startMode >= 0 && ed.entityMode) {
            if (!ed.entSkinView) {
                float entLeftH = paneLH;
                if (hairToolsOn && entMatY > paneLY + 40.0f) {
                    entLeftH = entMatY - 6.0f - paneLY;
                    if (entLeftH < 40.0f) entLeftH = 40.0f;
                }
                drawPane3D(paneLX, paneLY, paneLW, entLeftH, mvp3d, nullptr, 0, &entBatches, lineVerts, true, &entSolid);
            }
            drawPane3D(paneRX, paneRY, entViewRW, paneRH, rightEntMvp, nullptr, 0, &entRightBatches, randLineVerts, true, &entRightSolid);
            verts.clear();
            auto flushUi = [&]() {
                if (verts.empty()) return;
                gl::Disable(GL_DEPTH_TEST);
                gl::DepthMask(GL_FALSE);
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STREAM_DRAW);
                gl::UseProgram(prog);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
                gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 7));
                gl::BindVertexArray(0);
                gl::DepthMask(GL_TRUE);
                verts.clear();
            };
            if (paintSlideW > 1.0f) {
                float trackY = paintSlideY + 5.0f;
                rect(paintSlideX, trackY, paintSlideW, 6.0f, 0.18f, 0.18f, 0.20f, 1);
                for (int i = 0; i < 4; i++) {
                    float x = paintSlideX + paintSlideW * ((float)i / 3.0f);
                    rect(x - 1.0f, trackY - 3.0f, 2.0f, 12.0f, 0.45f, 0.45f, 0.48f, 1);
                }
                float kx = paintSlideX + paintSlideW * ((float)ed.paintDiv / 3.0f) - 7.0f;
                rect(kx, trackY - 5.0f, 14.0f, 16.0f, paintSlideHov ? 0.95f : 0.80f, paintSlideHov ? 0.85f : 0.70f, 0.25f, 1);
                flushUi();
            }
            int lsx = 0, lsy = g_winH - (int)entListBot, lsw = (int)leftBarW, lsh = (int)(entListBot - entListTop);
            if (lsw < 1) lsw = 1;
            if (lsh < 1) lsh = 1;
            gl::Enable(GL_SCISSOR_TEST);
            gl::Scissor(lsx, lsy, lsw, lsh);
            for (int i = 0; i < entCardN; i++) {
                float cx = entCardRect[i].x, cy = entCardRect[i].y, cw = entCardRect[i].w, ch = entCardRect[i].h;
                if (cy + ch < entListTop || cy > entListBot) continue;
                bool hov = (ed.entityHover == i);
                const std::string* cardName = entityListName(i);
                bool sel = cardName && *cardName == ed.entityName;
                rect(cx, cy, cw, ch, 0.10f, 0.10f, 0.12f, 1);
                rect(cx + 2, cy + 2, cw - 4, entCardPrevH - 2, 0.05f, 0.05f, 0.07f, 1);
                rect(cx, cy + entCardPrevH, cw, kEntCardNameH, sel ? 0.28f : 0.14f, sel ? 0.32f : 0.14f, sel ? 0.18f : 0.16f, 1);
                if (sel) {
                    rect(cx - 2, cy - 2, cw + 4, 2, 1.0f, 0.85f, 0.2f, 1);
                    rect(cx - 2, cy + ch, cw + 4, 2, 1.0f, 0.85f, 0.2f, 1);
                    rect(cx - 2, cy - 2, 2, ch + 4, 1.0f, 0.85f, 0.2f, 1);
                    rect(cx + cw, cy - 2, 2, ch + 4, 1.0f, 0.85f, 0.2f, 1);
                } else if (hov) {
                    rect(cx - 1, cy - 1, cw + 2, ch + 2, 1, 1, 1, 1);
                }
            }
            flushUi();
            auto thumbZoom = [&](float aspect, float ext) {
                if (ext < 0.2f) ext = 0.2f;
                float halfFov = 22.5f * 3.14159265f / 180.0f;
                float fit = ext * 0.65f / std::tan(halfFov);
                if (aspect < 1.0f) fit /= aspect;
                return fit;
            };
            std::vector<float> noLines;
            for (int i = 0; i < entCardN; i++) {
                float cx = entCardRect[i].x + 4.0f;
                float cy = entCardRect[i].y + 4.0f;
                float cw = entCardRect[i].w - 8.0f;
                float ch = entCardPrevH - 6.0f;
                if (cw < 8.0f || ch < 8.0f) continue;
                if (cy + ch < entListTop || cy > entListBot) continue;
                const std::string* cardName = entityListName(i);
                bool live = cardName && *cardName == ed.entityName;
                float aspect = cw / ch;
                std::vector<TexBatch> cachedBatches;
                EntThumb liveThumb;
                const EntThumb* thumb = nullptr;
                if (live) {
                    if (entIsCloth) {
                        fillEntThumb(liveThumb, ed.entityParts, 0, skinGL, entSheetW, entSheetH);
                    } else {
                        fillEntThumb(liveThumb, ed.entityParts, skinGL, 0, 0, 0);
                    }
                    thumb = &liveThumb;
                } else {
                    thumb = ensureEntThumb(i);
                }
                if (!thumb) continue;
                cachedBatches.reserve(thumb->batches.size());
                for (const EntThumb::Batch& b : thumb->batches)
                    cachedBatches.push_back({ b.tex, 0, b.verts });
                Mat4 thumbMvp = orbitMvp(aspect, 0.62f, 0.16f, thumbZoom(aspect, thumb->ext), thumb->px, thumb->py, thumb->pz);
                drawPane3D(cx, cy, cw, ch, thumbMvp, nullptr, 0, &cachedBatches, noLines, true, &thumb->solid);
                gl::Enable(GL_SCISSOR_TEST);
                gl::Scissor(lsx, lsy, lsw, lsh);
            }
            gl::Disable(GL_SCISSOR_TEST);
            if (entScrollMax > 1.0f) {
                rect(entScrollTrack[0], entScrollTrack[1], entScrollTrack[2], entScrollTrack[3], 0.14f, 0.14f, 0.16f, 1);
                rect(entScrollTrack[0], entScrollThumbY, entScrollTrack[2], entScrollThumbH, 0.62f, 0.64f, 0.68f, 1);
                flushUi();
            }
            if (!gizmoOverlay.empty()) {
                gl::Disable(GL_DEPTH_TEST);
                gl::DepthMask(GL_FALSE);
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gizmoOverlay.size() * sizeof(float)), gizmoOverlay.data(), GL_STREAM_DRAW);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
                gl::DrawArrays(GL_LINES, 0, (GLsizei)(gizmoOverlay.size() / 7));
                gl::BindVertexArray(0);
                gl::DepthMask(GL_TRUE);
            }
            auto flushOrthoUi = [&]() {
                if (verts.empty()) return;
                gl::Disable(GL_DEPTH_TEST);
                gl::DepthMask(GL_FALSE);
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STREAM_DRAW);
                gl::UseProgram(prog);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
                gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 7));
                gl::BindVertexArray(0);
                gl::DepthMask(GL_TRUE);
                verts.clear();
            };
            auto stroke2 = [&](float x, float y, float w, float h, float t, float r, float g, float b) {
                rect(x, y, w, t, r, g, b, 1);
                rect(x, y + h - t, w, t, r, g, b, 1);
                rect(x, y, t, h, r, g, b, 1);
                rect(x + w - t, y, t, h, r, g, b, 1);
            };
            verts.clear();
            bool showPaint = !hairToolsOn && paintGL && paintDispW > 1.0f && (paintKind() >= 0 || ed.entSkinView);
            if (showPaint) {
                gl::Disable(GL_DEPTH_TEST);
                gl::DepthMask(GL_FALSE);
                gl::Enable(GL_SCISSOR_TEST);
                int scx = (int)paneLX, scw = (int)paneLW, sch = (int)paneLH;
                int scy = g_winH - (int)(paneLY + paneLH);
                if (scw < 1) scw = 1;
                if (sch < 1) sch = 1;
                gl::Scissor(scx, scy, scw, sch);
                gl::BindTexture(GL_TEXTURE_2D, paintGL);
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                drawGlTex(paintGL, skinImgX, skinImgY, paintDispW, paintDispH);
                auto uvBox = [&](float u0, float v0, float u1, float v1, float t, float r, float g, float b) {
                    float a = (u0 < u1) ? u0 : u1, bb = (u0 < u1) ? u1 : u0;
                    float c = (v0 < v1) ? v0 : v1, d = (v0 < v1) ? v1 : v0;
                    float x = skinImgX + a * paintDispW;
                    float y = skinImgY + c * paintDispH;
                    float w = (bb - a) * paintDispW;
                    float h = (d - c) * paintDispH;
                    if (w < 1.0f) w = 1.0f;
                    if (h < 1.0f) h = 1.0f;
                    stroke2(x, y, w, h, t, r, g, b);
                };
                stroke2(skinImgX - 1, skinImgY - 1, paintDispW + 2, paintDispH + 2, 1, 0.55f, 0.55f, 0.58f);
                if (paintKind() < 0 && entIsCloth) {
                    const float SW = (float)paintTw, SH = (float)paintTh;
                    for (int qi = 0; qi < (int)ed.entityParts.size(); qi++) {
                        const pm::Part& p = ed.entityParts[qi];
                        if (!pm::partHasBox(p)) continue;
                        bool sel = qi < (int)entMark.size() && entMark[qi];
                        for (int f = 0; f < 6; f++) {
                            int x, y, fw, fh;
                            pm::boxFacePx(p.boxX, p.boxY, p.boxW, p.boxH, p.boxD, f, x, y, fw, fh);
                            float cr = sel ? 1.0f : 0.92f;
                            float cg = sel ? 0.82f : 0.92f;
                            float cb = sel ? 0.18f : 0.95f;
                            uvBox((float)x / SW, (float)y / SH, (float)(x + fw) / SW, (float)(y + fh) / SH,
                                  sel ? 2.0f : 1.0f, cr, cg, cb);
                        }
                    }
                } else if (paintKind() < 0) {
                    const float SW = (float)paintTw, SH = (float)paintTh;
                    for (int bi = 0; bi < pm::kSkinBoxCount; bi++) {
                        const pm::SkinBox& box = pm::kSkinBoxes[bi];
                        for (int f = 0; f < 6; f++) {
                            int x, y, fw, fh;
                            pm::boxFacePx(box.bx, box.by, box.w, box.h, box.d, f, x, y, fw, fh);
                            uvBox((float)x / SW, (float)y / SH, (float)(x + fw) / SW, (float)(y + fh) / SH,
                                  1.0f, 0.92f, 0.92f, 0.95f);
                        }
                    }
                    for (int i = 0; i < pm::kSkinLimbCount; i++) {
                        const pm::SkinLimbSlot& sl = pm::kSkinLimbs[i];
                        uvBox((float)sl.ox / SW, (float)sl.oy / SH,
                              (float)(sl.ox + 16) / SW, (float)(sl.oy + 16) / SH,
                              1.0f, 0.42f, 0.68f, 0.90f);
                    }
                    for (int i = 0; i < pm::kSkinExtraCapCount; i++) {
                        pm::SkinExtraCap c{};
                        if (!pm::extraCapByIndex(i, c)) continue;
                        uvBox((float)c.x / SW, (float)c.y / SH,
                              (float)(c.x + c.w) / SW, (float)(c.y + c.h) / SH,
                              1.0f, 0.92f, 0.92f, 0.95f);
                    }
                    for (int qi = 0; qi < (int)ed.entityParts.size(); qi++) {
                        if (qi >= (int)entMark.size() || !entMark[qi]) continue;
                        const pm::Part& p = ed.entityParts[qi];
                        if (pm::isDecalPart(p) || pm::isHairPart(p) || pm::isHairCardPart(p)) continue;
                        for (int f = 0; f < 6; f++) {
                            float u0, v0, u1, v1;
                            pm::skinFaceUV(p, f, u0, v0, u1, v1);
                            uvBox(u0, v0, u1, v1, 2.0f, 1.0f, 0.82f, 0.18f);
                        }
                    }
                }
                if (ed.hoverX >= 0) {
                    float px = paintDispW / (float)paintTw;
                    float py = paintDispH / (float)paintTh;
                    int half = ed.brushSize / 2;
                    float hx = skinImgX + (ed.hoverX - half) * px;
                    float hy = skinImgY + (ed.hoverY - half) * py;
                    stroke2(hx, hy, ed.brushSize * px, ed.brushSize * py, 2.0f, 1, 1, 0);
                }
                if (ed.dragging && ed.tool >= 5 && ed.tool <= 8) {
                    int ax0 = std::min(ed.dragX0, ed.dragX1), ay0 = std::min(ed.dragY0, ed.dragY1);
                    int ax1 = std::max(ed.dragX0, ed.dragX1), ay1 = std::max(ed.dragY0, ed.dragY1);
                    float px = paintDispW / (float)paintTw;
                    float py = paintDispH / (float)paintTh;
                    float sx0 = skinImgX + ax0 * px, sy0 = skinImgY + ay0 * py;
                    float sx1 = skinImgX + (ax1 + 1) * px, sy1 = skinImgY + (ay1 + 1) * py;
                    stroke2(sx0, sy0, sx1 - sx0, sy1 - sy0, 2.0f, 1, 1, 0);
                }
                flushOrthoUi();
                gl::Disable(GL_SCISSOR_TEST);
            }
            {
                bool right = entIsCloth ? !ed.clothLocal : ed.entSkinView;
                rect(togX, togY, togW, togH, togHov ? 0.22f : 0.14f, togHov ? 0.24f : 0.15f, togHov ? 0.28f : 0.18f, 1);
                float knobW = togW * 0.48f;
                float knobX = right ? (togX + togW - knobW - 3.0f) : (togX + 3.0f);
                rect(knobX, togY + 3.0f, knobW, togH - 6.0f, 0.38f, 0.52f, 0.32f, 1);
                if (togHov) stroke2(togX - 1, togY - 1, togW + 2, togH + 2, 1, 1, 1, 1);
            }
            if (ed.hairSelDrag) {
                float x0 = std::min(ed.hairSelX0, ed.hairSelX1);
                float y0 = std::min(ed.hairSelY0, ed.hairSelY1);
                float w = std::fabs(ed.hairSelX1 - ed.hairSelX0);
                float h = std::fabs(ed.hairSelY1 - ed.hairSelY0);
                if (w < 1.0f) w = 1.0f;
                if (h < 1.0f) h = 1.0f;
                stroke2(x0, y0, w, h, 1.5f, 1.0f, 0.92f, 0.18f);
            }
            flushOrthoUi();
        } else {
            if (!texVerts.empty()) {
                bool depthTest = (ed.modelBlock != GRASS_TUFT);
                if (depthTest) gl::Enable(GL_DEPTH_TEST);
                gl::BindVertexArray(tvao);
                gl::BindBuffer(GL_ARRAY_BUFFER, tvbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(texVerts.size()*sizeof(float)), texVerts.data(), GL_STREAM_DRAW);
                gl::UseProgram(cprog);
                gl::UniformMatrix4fv(cuMVP, 1, GL_FALSE, mvp3d.m);
                gl::BindTexture(GL_TEXTURE_2D, atlasTex);
                gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(texVerts.size() / 9));
                gl::UseProgram(prog);
                gl::BindVertexArray(0);
                if (depthTest) gl::Disable(GL_DEPTH_TEST);
            }
            if (!lineVerts.empty()) {
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lineVerts.size()*sizeof(float)), lineVerts.data(), GL_STREAM_DRAW);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp3d.m);
                gl::DrawArrays(GL_LINES, 0, (GLsizei)(lineVerts.size() / 7));
                gl::BindVertexArray(0);
            }
        }

        // ---- text overlay (white English), drawn after geometry so it stays on top ----
        if (startMode < 0) {
            const float bw = 380.0f, bh = 64.0f, gap = 20.0f;
            const float totalH = 6 * bh + 5 * gap;
            const float bx = (g_winW - bw) * 0.5f;
            const float y0 = (g_winH - totalH) * 0.5f - 50.0f;
            const char* labels[6] = {
                "Texture Editor", "Item Model Editor", "Entity Model Editor",
                "Animation Editor", "Data Pack Editor", "Structure Editor"
            };
            for (int i = 0; i < 6; i++) {
                TextTex& tt = getTextTex(labels[i]);
                if (tt.tex) drawTextTex(tt, bx + (bw - tt.w) * 0.5f, y0 + i * (bh + gap) + (bh - tt.h) * 0.5f);
            }
            TextTex& hd = getTextTex("Select Editor Mode");
            if (hd.tex) drawTextTex(hd, (g_winW - hd.w) * 0.5f, y0 - 70.0f);
        }
        if (startMode >= 0 && ed.modelMode) {
            // item selection list icons (3D display models on the panel background)
            const float pX = 8.0f, pY = 52.0f, thumb = 36.0f, gap = 6.0f;
            const int cols = 4;
            std::vector<float> iconNoLines;
            // Same isometric direction as the game icons, but the projection is fit
            // to each mesh so thin tools (axe, pick, shears) fill the thumbnail
            // instead of sitting inside a unit-cube framing.
            const Vec3 iconDir = Vec3{ 1.05f, 0.77f, 1.05f }.normalized();
            const Vec3 iconUp{ 0.0f, 1.0f, 0.0f };
            for (int b = 1; b < liveBlockCount(); b++) {
                int i = b - 1;
                float cx = pX + (i % cols) * (thumb + gap);
                float cy = pY + (i / cols) * (thumb + gap);
                const mat::Model& mdl = (b == ed.modelBlock) ? editModel : mat::itemModel((uint8_t)b);
                std::vector<Vertex> iconMesh;
                mat::buildItemDisplayMesh((uint8_t)b, mdl, iconMesh);
                std::vector<float> iconSolid;
                if (!mdl.solids.empty()) {
                    mat::emitSolidMesh(mdl.solids, iconSolid, [](float x, float y, float z) {
                        return Vec3{ x, y, z };
                    }, true);
                }
                if (iconMesh.empty() && iconSolid.empty()) continue;
                Vec3 ctr{ 0.0f, 0.0f, 0.0f };
                int nctr = 0;
                for (const Vertex& v : iconMesh) { ctr += Vec3{ v.px, v.py, v.pz }; nctr++; }
                for (const mat::Solid& s : mdl.solids) { ctr += Vec3{ s.c[0], s.c[1], s.c[2] }; nctr++; }
                if (nctr > 0) ctr = ctr / (float)nctr;
                Mat4 iconView = Mat4::lookAt(ctr + iconDir * 8.0f, ctr, iconUp);
                float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f, minZ = 1e9f, maxZ = -1e9f;
                auto expandIcon = [&](const Vec3& w) {
                    Vec4 p = iconView * Vec4{ w.x, w.y, w.z, 1.0f };
                    if (p.x < minX) minX = p.x;
                    if (p.x > maxX) maxX = p.x;
                    if (p.y < minY) minY = p.y;
                    if (p.y > maxY) maxY = p.y;
                    if (p.z < minZ) minZ = p.z;
                    if (p.z > maxZ) maxZ = p.z;
                };
                for (const Vertex& v : iconMesh) expandIcon({ v.px, v.py, v.pz });
                for (const mat::Solid& s : mdl.solids) {
                    for (int c = 0; c < 8; c++) {
                        expandIcon({
                            s.c[0] + ((c & 1) ? s.h[0] : -s.h[0]),
                            s.c[1] + ((c & 2) ? s.h[1] : -s.h[1]),
                            s.c[2] + ((c & 4) ? s.h[2] : -s.h[2])
                        });
                    }
                }
                float span = std::max(maxX - minX, maxY - minY) * 0.58f;
                if (span < 0.02f) span = 0.02f;
                float mx2 = 0.5f * (minX + maxX), my2 = 0.5f * (minY + maxY);
                // lookAt stores view Z negative in front of the camera.
                float znear = -maxZ - 0.5f;
                float zfar = -minZ + 0.5f;
                if (znear < 0.05f) znear = 0.05f;
                if (zfar < znear + 0.2f) zfar = znear + 0.2f;
                Mat4 iconMvp = Mat4::ortho(mx2 - span, mx2 + span, my2 - span, my2 + span, znear, zfar) * iconView;
                std::vector<float> iconVerts;
                appendMesh(iconVerts, iconMesh);
                drawPane3D(cx + 3, cy + 3, thumb - 6, thumb - 6, iconMvp, &iconVerts, atlasTex, nullptr, iconNoLines, true, &iconSolid);
            }
            TextTex& ph = getTextTex("Items");
            if (ph.tex) drawTextTex(ph, pX, 22);
            TextTex& ar = getTextTex("Apply Rand");
            if (ar.tex) drawTextTex(ar, arX + (kArW - ar.w) * 0.5f, arY + (kArH - ar.h) * 0.5f);
            TextTex& cl = getTextTex("Cancel");
            if (cl.tex) drawTextTex(cl, cancelX + (kCancelW - cl.w) * 0.5f, cancelY + (kCancelH - cl.h) * 0.5f);
            const char* tabLab[2] = { "Rand", "Color" };
            for (int i = 0; i < 2; i++) {
                if (rightTab[i][2] < 1.0f) continue;
                TextTex& t = getTextTex(tabLab[i]);
                drawTextFit(t, rightTab[i][0], rightTab[i][1], rightTab[i][2], rightTab[i][3]);
            }
            TextTex& md = getTextTex("Model");
            if (md.tex) drawTextTex(md, paneLX + 10.0f, midBtnY + (kBtnH - md.h) * 0.5f);
            auto drawBtnLabel = [&](const char* s, float x, float w) {
                TextTex& t = getTextTex(s);
                if (t.tex) drawTextTex(t, x + (w - t.w) * 0.5f, midBtnY + (kBtnH - t.h) * 0.5f);
            };
            drawBtnLabel("Face", faceX, kFaceW);
            drawBtnLabel("Block", blkX, kBlkW);
            drawBtnLabel("Small", smX, kSmW);
            drawBtnLabel("Tiny", tinyX, kTinyW);
            for (int i = 0; i < 13; i++) {
                TextTex& t = getTextTex(partLab[i]);
                if (!t.tex || partToolRect[i][2] < 1.0f) continue;
                drawTextFit(t, partToolRect[i][0], partToolRect[i][1], partToolRect[i][2], partToolRect[i][3]);
            }
            if (ed.modelMode && ed.modelRightTab == 1) {
                for (int i = 0; i < 9; i++) {
                    if (colorTool[i][2] < 1.0f) continue;
                    TextTex& t = getTextTex(colorLab[i]);
                    drawTextFit(t, colorTool[i][0], colorTool[i][1], colorTool[i][2], colorTool[i][3]);
                }
                if (!itemPaintName.empty()) {
                    TextTex& t = getTextTex(itemPaintName);
                    if (t.tex) drawTextTex(t, paneRX + 12.0f, paneRY + 4.0f);
                }
            }
            if (ed.modelMode && ed.measureOn) {
                float dx = ed.measureB[0] - ed.measureA[0];
                float dy = ed.measureB[1] - ed.measureA[1];
                float dz = ed.measureB[2] - ed.measureA[2];
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.3f", std::sqrt(dx * dx + dy * dy + dz * dz));
                TextTex& t = getTextTex(buf);
                if (t.tex) drawTextTex(t, paneLX + 8.0f, paneLY + 8.0f);
            }
            for (int i = 0; i < kNMTools; i++) {
                TextTex& t = getTextTex(kMToolLab[i]);
                if (t.tex) {
                    float bx = mtoolRect[i][0], by = mtoolRect[i][1], bw = mtoolRect[i][2], bh = mtoolRect[i][3];
                    drawTextTex(t, bx + (bw - t.w) * 0.5f, by + (bh - t.h) * 0.5f);
                }
            }
            for (int i = 0; i < nMatItems; i++) {
                float mx0, my0;
                matItemRect(i, mx0, my0);
                float pad = 2.0f;
                if (i == nMatItems - 1) {
                    TextTex& pl = getTextTex("+");
                    if (pl.tex) drawTextTex(pl, mx0 + (kMatThumb - pl.w) * 0.5f, my0 + (kMatThumb - pl.h) * 0.5f);
                } else if (i < TEX_COUNT) {
                    if (tileGL[i]) drawGlTex(tileGL[i], mx0 + pad, my0 + pad, kMatThumb - pad * 2, kMatThumb - pad * 2);
                    else drawAtlasTile(i, mx0 + pad, my0 + pad, kMatThumb - pad * 2, kMatThumb - pad * 2);
                } else {
                    int ei = i - TEX_COUNT;
                    if (ei >= 0 && ei < (int)extraMats.size() && extraMats[ei].tex)
                        drawGlTex(extraMats[ei].tex, mx0 + pad, my0 + pad, kMatThumb - pad * 2, kMatThumb - pad * 2);
                }
            }
            if (showSliders) {
                for (int i = 0; i < kNRandSliders; i++) {
                    float lx, ly, tx, ty, tw, th, vx;
                    sliderTrack(i, lx, ly, tx, ty, tw, th, vx);
                    const RandSlider& spec = kRandSliders[i];
                    float v = editRand.get(spec.key, spec.def);
                    TextTex& lb = getTextTex(spec.label);
                    if (lb.tex) drawTextTex(lb, lx, ly + 5.0f);
                    char buf[32];
                    if (spec.integer) snprintf(buf, sizeof(buf), "%d", (int)(v + 0.5f));
                    else snprintf(buf, sizeof(buf), "%.2f", v);
                    TextTex& vt = getTextTex(buf);
                    if (vt.tex) {
                        float vxDraw = vx + slValueW - (float)vt.w - 4.0f;
                        if (vxDraw < vx) vxDraw = vx;
                        drawTextTex(vt, vxDraw, ly + 5.0f);
                    }
                }
            }
        }
        if (startMode >= 0 && ed.entityMode) {
            TextTex& el = getTextTex("Entities");
            if (el.tex) drawTextTex(el, 10.0f, kTopH + 8.0f);
            gl::Enable(GL_SCISSOR_TEST);
            gl::Scissor(0, g_winH - (int)entListBot, (int)leftBarW > 1 ? (int)leftBarW : 1,
                        (int)(entListBot - entListTop) > 1 ? (int)(entListBot - entListTop) : 1);
            for (int i = 0; i < entCardN; i++) {
                float cx = entCardRect[i].x, cy = entCardRect[i].y, cw = entCardRect[i].w;
                if (cy + entCardRect[i].h < entListTop || cy > entListBot) continue;
                const char* label = "";
                if (const std::string* cardName = entityListName(i)) label = cardName->c_str();
                if (i < (int)entNames.size()) {
                    if (const plugin::EntityModule* m = plugin::findEntity(label))
                        if (!m->title.empty()) label = m->title.c_str();
                }
                TextTex& nm = getTextTex(label);
                if (nm.tex) drawTextTex(nm, cx + (cw - nm.w) * 0.5f,
                                        cy + entCardPrevH + (kEntCardNameH - nm.h) * 0.5f);
            }
            gl::Disable(GL_SCISSOR_TEST);
            TextTex& md = getTextTex("Entity");
            if (md.tex) drawTextTex(md, paneLX + 10.0f, midBtnY + (kBtnH - md.h) * 0.5f);
            TextTex& wh = getTextTex("Whole");
            if (wh.tex) drawTextTex(wh, paneRX + 10.0f, midBtnY + (kBtnH - wh.h) * 0.5f);
            if (entToolColW > 1.0f) {
                TextTex& tl = getTextTex("Tools");
                if (tl.tex) drawTextTex(tl, entToolX + (entToolColW - tl.w) * 0.5f, midBtnY + (kBtnH - tl.h) * 0.5f);
            }
            TextTex& clab = getTextTex("Color");
            if (clab.tex) drawTextTex(clab, palColX + 10.0f, midBtnY + (kBtnH - clab.h) * 0.5f);
            {
                TextTex& t3 = getTextTex(entIsCloth ? "Local" : "3D");
                TextTex& ts = getTextTex(entIsCloth ? "All" : "Skin");
                float ty = togY + (togH - (t3.tex ? t3.h : 12)) * 0.5f;
                if (t3.tex) drawTextTex(t3, togX + togW * 0.25f - t3.w * 0.5f, ty);
                if (ts.tex) drawTextTex(ts, togX + togW * 0.75f - ts.w * 0.5f, ty);
            }
            if (entIsCloth) {
                TextTex& t = getTextTex("Body");
                if (t.tex) drawTextTex(t, cBody[0] + (cBody[2] - t.w) * 0.5f, cBody[1] + (cBody[3] - t.h) * 0.5f);
            }
            auto drawBtnLabel = [&](const char* s, float x, float w) {
                TextTex& t = getTextTex(s);
                if (t.tex) drawTextTex(t, x + (w - t.w) * 0.5f, midBtnY + (kBtnH - t.h) * 0.5f);
            };
            drawBtnLabel("Cancel", entCancelX, kCancelW);
            {
                const char* kActLab[13] = { "Hair", "Select", "Paint", "Card", "Erase", "Merge", "Split", "Tex", "Dup", "Del", "Cut", "Measure", "Plane" };
                for (int i = 0; i < kEActs; i++) {
                    const char* lab = (entIsCloth && i == 0) ? "Part" : kActLab[i];
                    TextTex& t = getTextTex(lab);
                    if (!t.tex) continue;
                    float bx = eactRect[i][0], by = eactRect[i][1], bw = eactRect[i][2], bh = eactRect[i][3];
                    if (bw < 1.0f) continue;
                    drawTextFit(t, bx, by, bw, bh);
                }
            }
            for (int i = 0; i < kEntTools; i++) {
                TextTex& t = getTextTex(kEntToolLab[i]);
                if (t.tex) {
                    float bx = etoolRect[i][0], by = etoolRect[i][1], bw = etoolRect[i][2], bh = etoolRect[i][3];
                    if (bw < 1.0f || bh < 1.0f) continue;
                    drawTextFit(t, bx, by, bw, bh);
                }
            }
            if (selEyeKind) {
                for (int i = 0; i < nFaceChip; i++) {
                    const char* fn = faceNameAt(i);
                    int pi = findPartTex(fn);
                    unsigned t = 0;
                    if (pi >= 0 && !ed.entityParts[pi].tex.empty()) t = glTexForName(ed.entityParts[pi].tex);
                    if (!t) t = glTexForName(fn);
                    float x = faceChip[i][0] + 4.0f, y = faceChip[i][1] + 4.0f;
                    float w = 18.0f, h = faceChip[i][3] - 8.0f;
                    if (t) drawGlTex(t, x, y, w, h);
                    const char* lab = (i < 4 && std::strcmp(fn, kFaceChipTex[i]) == 0) ? kFaceChipLab[i] : fn;
                    TextTex& lb = getTextTex(lab);
                    if (lb.tex) drawTextTex(lb, x + w + 4.0f, faceChip[i][1] + (faceChip[i][3] - lb.h) * 0.5f);
                }
            }
            if (selMouthKind) {
                for (int i = 0; i < nMouthChip; i++) {
                    unsigned t = mouthGL[i];
                    float x = mouthChip[i][0] + 4.0f, y = mouthChip[i][1] + 8.0f;
                    float w = 28.0f, h = 14.0f;
                    if (t) drawGlTex(t, x, y, w, h);
                    const char* lab = pm::kMouthLabs[i < pm::kMouthVariantCount ? i : 0];
                    if (i < (int)entMouthNames.size() &&
                        (i >= pm::kMouthVariantCount || entMouthNames[i] != pm::kMouthNames[i]))
                        lab = entMouthNames[i].c_str();
                    TextTex& lb = getTextTex(lab);
                    if (lb.tex) drawTextTex(lb, x + w + 4.0f, mouthChip[i][1] + (mouthChip[i][3] - lb.h) * 0.5f);
                }
            }
            if (hairToolsOn) {
                for (int i = 0; i < nMatItems; i++) {
                    float mx0, my0;
                    matItemRect(i, mx0, my0);
                    float pad = 2.0f;
                    if (i == nMatItems - 1) {
                        TextTex& pl = getTextTex("+");
                        if (pl.tex) drawTextTex(pl, mx0 + (kMatThumb - pl.w) * 0.5f, my0 + (kMatThumb - pl.h) * 0.5f);
                    } else if (i < TEX_COUNT) {
                        if (tileGL[i]) drawGlTex(tileGL[i], mx0 + pad, my0 + pad, kMatThumb - pad * 2, kMatThumb - pad * 2);
                        else drawAtlasTile(i, mx0 + pad, my0 + pad, kMatThumb - pad * 2, kMatThumb - pad * 2);
                    } else {
                        int ei = i - TEX_COUNT;
                        if (ei >= 0 && ei < (int)extraMats.size() && extraMats[ei].tex)
                            drawGlTex(extraMats[ei].tex, mx0 + pad, my0 + pad, kMatThumb - pad * 2, kMatThumb - pad * 2);
                    }
                }
            }
        }
        if (startMode >= 0 && ed.animMode) {
            auto labelR = [&](const char* s, const float r[4]) {
                if (r[2] < 1.0f || !s) return;
                TextTex& t = getTextTex(s);
                if (!t.tex) return;
                float tx = r[0] + (r[2] - (float)t.w) * 0.5f;
                float ty = r[1] + (r[3] - (float)t.h) * 0.5f;
                drawTextTex(t, tx, ty);
            };
            TextTex& clips = getTextTex("Animations");
            if (clips.tex) drawTextTex(clips, 10.0f, 8.0f);
            gl::Enable(GL_SCISSOR_TEST);
            gl::Scissor(0, (int)std::floor((float)g_winH - animPaneBot), (int)std::ceil(animLeftW),
                        (int)std::ceil(animPaneBot - animPaneTop));
            for (int i = 0; i < animClipN && i < (int)animNames.size(); i++)
                labelR(animNames[i].c_str(), animClipRect[i]);
            labelR("New", animNewRect);
            labelR("Dup", animDupRect);
            labelR("Del", animDelRect);
            TextTex& bones = getTextTex("Bones");
            if (bones.tex) drawTextTex(bones, 10.0f, animNewRect[1] + animNewRect[3] + 6.0f);
            for (int i = 0; i < animBoneN && i < (int)editClip.bones.size(); i++)
                labelR(editClip.bones[i].name.c_str(), animBoneRect[i]);
            gl::Disable(GL_SCISSOR_TEST);
            TextTex& bind = getTextTex("Bind Model");
            if (bind.tex) drawTextTex(bind, (float)g_winW - animRightW + 10.0f, 8.0f);
            gl::Enable(GL_SCISSOR_TEST);
            {
                int sx = (int)std::floor((float)g_winW - animRightW);
                int sy = (int)std::floor((float)g_winH - animPaneBot);
                int sw = (int)std::ceil(animRightW);
                int sh = (int)std::ceil(animPaneBot - animPaneTop);
                if (sx < 0) { sw += sx; sx = 0; }
                if (sy < 0) { sh += sy; sy = 0; }
                if (sw < 0) sw = 0;
                if (sh < 0) sh = 0;
                gl::Scissor(sx, sy, sw, sh);
            }
            for (int i = 0; i < animBindN && i < (int)entNames.size(); i++) {
                const char* lab = entNames[i].c_str();
                if (const plugin::EntityModule* m = plugin::findEntity(lab))
                    if (!m->title.empty()) lab = m->title.c_str();
                labelR(lab, animBindRect[i]);
            }
            labelR("Rebuild Rig", animRebuildRect);
            labelR(ed.animModelDirty ? "Save Model*" : "Save Model", animModelSave);
            int matched = 0;
            for (const anim::Bone& b : editClip.bones) {
                for (const pm::Part& p : animBindParts)
                    if (pm::partName(p) == b.name) { matched++; break; }
            }
            char st[64];
            snprintf(st, sizeof(st), "Channels %d / %d", matched, (int)editClip.bones.size());
            TextTex& stt = getTextTex(st);
            if (stt.tex) drawTextTex(stt, (float)g_winW - animRightW + 10.0f, animRebuildRect[1] + animRebuildRect[3] + 4.0f);
            TextTex& holdLab = getTextTex("Hold Bind");
            if (holdLab.tex) drawTextTex(holdLab, (float)g_winW - animRightW + 10.0f, animHoldPrev[1] - 22.0f);
            labelR("<", animHoldPrev);
            labelR(">", animHoldNext);
            {
                std::string nm = holdItemName();
                TextTex& it = getTextTex(nm.c_str());
                if (it.tex) {
                    float nameL = animHoldPrev[0] + animHoldPrev[2] + 6.0f;
                    float nameR = animHoldNext[0] - 6.0f;
                    float cx = nameL + (nameR - nameL - (float)it.w) * 0.5f;
                    if (cx < nameL) cx = nameL;
                    drawTextTex(it, cx, animHoldPrev[1] + (animHoldPrev[3] - (float)it.h) * 0.5f);
                }
            }
            labelR("Main L", animHoldL);
            labelR("Main R", animHoldR);
            labelR(ed.animHoldEdit ? "Bind ON" : "Bind", animHoldGrip);
            labelR(ed.animGrasp ? "Grasp ON" : "Grasp", animHoldGrasp);
            labelR("Lower", animHoldLower);
            labelR("Higher", animHoldHigher);
            labelR(ed.animLockDir ? "Dir ON" : "Dir", animLockDir);
            labelR(ed.animLockPos ? "Pos ON" : "Pos", animLockPos);
            {
                anim::Clip view = previewClipNow();
                Vec3 g = anim::evalGrip(view, (float)ed.animFrame, previewHoldSpec().grip);
                char gy[64];
                snprintf(gy, sizeof(gy), "grip  %.2f   %.2f   %.2f", g.x, g.y, g.z);
                TextTex& gt = getTextTex(gy);
                if (gt.tex) {
                    float rowY = animHoldLower[1] + animHoldLower[3] + 2.0f;
                    drawTextTex(gt, animHoldLower[0], rowY);
                }
            }
            labelR("Use Bone", animHoldBone);
            labelR(ed.animHoldDirty ? "Save Hold*" : "Save Hold", animHoldSave);
            {
                hold::Spec spec = previewHoldSpec();
                char hb[96];
                snprintf(hb, sizeof(hb), "%s", spec.bone.c_str());
                TextTex& bn = getTextTex(hb);
                if (bn.tex) drawTextTex(bn, (float)g_winW - animRightW + 10.0f, animHoldSave[1] + animHoldSave[3] + 8.0f);
                TextTex& off = getTextTex("Dark blue = other palm");
                if (off.tex) drawTextTex(off, (float)g_winW - animRightW + 10.0f, animHoldSave[1] + animHoldSave[3] + 28.0f);
            }
            gl::Disable(GL_SCISSOR_TEST);
            const char* kBtn[6] = { "|<", ed.animPlaying ? "||" : ">", ">|", "[]", "+K", "-K" };
            for (int i = 0; i < 6; i++) labelR(kBtn[i], animBtnRect[i]);
            labelR("Blank", animKeyOp[0]);
            labelR("Copy", animKeyOp[1]);
            labelR("Paste", animKeyOp[2]);
            labelR("To Key", animKeyOp[3]);
            labelR("Del", animKeyOp[4]);
            labelR("<", animKeyOp[5]);
            labelR(">", animKeyOp[6]);
            char aLab[24], bLab[24];
            if (ed.animSpanA >= 0) snprintf(aLab, sizeof(aLab), "A %d", ed.animSpanA);
            else snprintf(aLab, sizeof(aLab), "A");
            if (ed.animSpanB >= 0) snprintf(bLab, sizeof(bLab), "B %d", ed.animSpanB);
            else snprintf(bLab, sizeof(bLab), "B");
            labelR(aLab, animKeyOp[7]);
            labelR(bLab, animKeyOp[8]);
            labelR("Unwrap", animKeyOp[9]);
            {
                int sense = 1, laps = 0;
                bool mixed = false, any = false;
                anim::Clip view = previewClipNow();
                std::vector<int> marks;
                anim::armKeyFrames(view, previewToolSpec().bone, marks);
                int lo = 0, hi = 0;
                bool span = ed.animSpanA >= 0 && ed.animSpanB >= 0 && ed.animSpanA != ed.animSpanB;
                if (span) {
                    lo = ed.animSpanA < ed.animSpanB ? ed.animSpanA : ed.animSpanB;
                    hi = ed.animSpanA < ed.animSpanB ? ed.animSpanB : ed.animSpanA;
                }
                for (int i = 0; i + 1 < (int)marks.size(); i++) {
                    int a = marks[i], b = marks[i + 1];
                    bool use = span ? (a >= lo && b <= hi) : (ed.animFrame > a && ed.animFrame < b);
                    if (!use) continue;
                    int ss = 1, ll = 0;
                    anim::toolTurnOf(view, a, b, ss, ll);
                    if (!any) { sense = ss; laps = ll; any = true; }
                    else if (ss != sense || ll != laps) mixed = true;
                }
                labelR(sense < 0 ? "Far" : "Near", animTurnRect[0]);
                labelR("k-", animTurnRect[1]);
                labelR("k+", animTurnRect[2]);
                char kb[32] = "k 0";
                if (mixed) snprintf(kb, sizeof(kb), "k *");
                else snprintf(kb, sizeof(kb), "k %d", laps);
                TextTex& kt = getTextTex(kb);
                if (kt.tex) drawTextTex(kt, animTurnRect[2][0] + animTurnRect[2][2] + 8.0f,
                                        animTurnRect[2][1] + 4.0f);
            }
            labelR("X", animFlipRect[0]);
            labelR("Y", animFlipRect[1]);
            labelR("Z", animFlipRect[2]);
            labelR("Move", animBtnRect[8]);
            labelR("Rot", animBtnRect[9]);
            labelR(ed.animRollChain ? "Roll+" : "Only", animBtnRect[12]);
            labelR("Select", animBtnRect[10]);
            labelR(ed.animMask ? "Mask ON" : "Mask", animBtnRect[11]);
            labelR("Skeleton", animBtnRect[6]);
            labelR("Bound", animBtnRect[7]);
            labelR(ed.animDirty ? "Save*" : "Save", animSaveRect);
            labelR("Undo", animUndoRect);
            labelR("-", animLenMinus);
            labelR("+", animLenPlus);
            labelR("1/2", animScaleRect[0]);
            labelR("s-", animScaleRect[1]);
            labelR("s+", animScaleRect[2]);
            labelR("x2", animScaleRect[3]);
            char lenb[48];
            snprintf(lenb, sizeof(lenb), "%d f", editClip.length);
            TextTex& lf = getTextTex(lenb);
            if (lf.tex) drawTextTex(lf, animLenMinus[0] + 28.0f, animLenMinus[1] + 4.0f);
            char frb[32];
            snprintf(frb, sizeof(frb), "frame %d", ed.animFrame);
            TextTex& frt = getTextTex(frb);
            if (frt.tex) drawTextTex(frt, animTimeRect[0] + animTimeRect[2] - 90.0f, animTimeRect[1] - 22.0f);
            char title[96];
            snprintf(title, sizeof(title), "%s   %.0f fps   %s",
                     editClip.name.c_str(), editClip.fps, editClip.loop ? "loop" : "once");
            TextTex& tit = getTextTex(title);
            if (tit.tex) drawTextTex(tit, animVX + 12.0f, 10.0f);
            TextTex& ah = getTextTex("Roll+ turns the limb below too. 1/2 and x2 scale the bar; s- s+ step it. Wheel on the bar zooms; drag the slider to pan.");
            if (ah.tex) drawTextTex(ah, 12.0f, animBarSlider[1] + animBarSlider[3] + 4.0f);
        }
        if (startMode >= 0 && (ed.entityMode || ed.modelMode)) {
            std::string hint = ed.entityMode
                ? std::string("Entity: Paint draw  Shift+Paint straight line  hold E erase  Shift/Ctrl+click multi-select then Bind  wheel zoom  Ctrl+Z")
                : (ed.modelTool == 6
                    ? std::string("Tiny: click to place  same-size face: snap flush  other faces: grid  click Tiny again to exit")
                    : (ed.modelTool == 7
                        ? ([&]() {
                            float ts = 1.0f;
                            auto idx = selectedIndices();
                            if (!idx.empty()) ts = mat::quadTexScale(editModel.quads[idx[0]]);
                            char buf[160];
                            snprintf(buf, sizeof(buf),
                                     "Tile: period %.4g  wheel/drag up = larger tex  down = finer  click Tile to exit",
                                     ts);
                            return std::string(buf);
                        }())
                        : std::string("Item: ") + blockOf(ed.modelBlock).name +
                          "   Click to select  Move: arrows+arcs  45: click 90  Mirror/UV: arrows  Stretch Tiny/Small: RGB  unbound: edges  Ctrl+Z"));
            TextTex& ht = getTextTex(hint);
            if (ht.tex) drawTextTex(ht, 216, 9);
        }
        if (startMode >= 0) {
            const float exW = 120.0f, exH = 44.0f, exM = 20.0f;
            float exX = g_winW - exW - exM, exY = g_winH - exH - exM;
            TextTex& et = getTextTex("EXIT");
            if (et.tex) drawTextTex(et, exX + (exW - et.w) * 0.5f, exY + (exH - et.h) * 0.5f);
        }

        if (startMode >= 0 && ed.tool == 1 && !ed.modelMode && !ed.animMode) {
            float bmX = ed.brushMat[0], bmY = ed.brushMat[1], mw = ed.brushMat[2];
            if (mw > 1.0f) {
                const float btnY = bmY + mw + 10.0f;
                const float ckS = 16.0f, ckX = bmX, ckY = btnY + 4.0f;
                TextTex& a0 = getTextTex("A=0");
                if (a0.tex) drawTextTex(a0, ckX + ckS + 6.0f, ckY + (ckS - a0.h) * 0.5f);
                char cnt[16];
                snprintf(cnt, sizeof(cnt), "%d/16", brushFilledCount());
                TextTex& ct = getTextTex(cnt);
                if (ct.tex) {
                    if (ed.entityMode)
                        drawTextTex(ct, bmX + mw - (float)ct.w, bmY - (float)ct.h - 4.0f);
                    else
                        drawTextTex(ct, bmX + mw - (float)ct.w, bmY - (float)ct.h - 6.0f);
                }
            }
        }
        // toolbar icons (PNG textures), drawn in the textured pass.
        // Texture editor and the entity material painter share this toolbar;
        // item-model mode uses the model-tool row instead.
        if (startMode >= 0 && !ed.modelMode && !ed.animMode) {
            gl::Disable(GL_SCISSOR_TEST);
            gl::Disable(GL_DEPTH_TEST);
            gl::Viewport(0, 0, g_winW, g_winH);
            for (int k = 0; k < 13; k++) {
                if (!iconTex[k]) continue;
                float bx = ed.tbRect[k][0], by = ed.tbRect[k][1];
                float bw = ed.tbRect[k][2], bh = ed.tbRect[k][3];
                if (bw < 2.0f || bh < 2.0f) continue;
                float is = (bw < bh ? bw : bh) * 0.78f;
                float ix = bx + (bw - is) * 0.5f, iy = by + (bh - is) * 0.5f;
                drawGlTex(iconTex[k], ix, iy, is, is);
            }
        }
        if (startMode >= 0 && (ed.modelMode || ed.entityMode)) {
            auto drawSaveIcon = [&](float bx, float by, int icon) {
                if (!iconTex[icon]) return;
                float is = kSaveS * 0.72f;
                float ix = bx + (kSaveS - is) * 0.5f, iy = by + (kSaveS - is) * 0.5f;
                drawGlTex(iconTex[icon], ix, iy, is, is);
            };
            if (ed.modelMode) {
                drawSaveIcon(saveX, saveY, 12);
                drawSaveIcon(midSaveX, midBtnY, 12);
                drawSaveIcon(midUndoX, midBtnY, 9);
            } else {
                drawSaveIcon(midSaveX, midBtnY, 12);
                drawSaveIcon(midUndoX, midBtnY, 9);
            }
        }

        // tooltip popup for hovered toolbar button
        if (ed.toolHover >= 0 && ed.toolHover < 13) {
            const char* tbTip[13] = { "Paintbrush", "Brush (multi-color)", "Bucket (flood fill)", "Picker (eyedropper)", "Eraser", "Rect bucket", "Rectangle", "Line", "Circle", "Undo", "Brush size -", "Brush size +", "Save PNG" };
            TextTex& tt = getTextTex(tbTip[ed.toolHover]);
            if (tt.tex) {
                float px2 = ed.tbRect[ed.toolHover][0] + ed.tbRect[ed.toolHover][2] * 0.5f;
                float py2 = ed.tbRect[ed.toolHover][1] + ed.tbRect[ed.toolHover][3] + 6.0f;
                float tw2 = (float)tt.w, th2 = (float)tt.h;
                float bx2 = px2 - tw2 * 0.5f - 6.0f, by2 = py2;
                if (bx2 < 4.0f) bx2 = 4.0f;
                if (bx2 + tw2 + 12.0f > g_winW) bx2 = g_winW - tw2 - 12.0f;
                if (by2 + th2 + 12.0f > g_winH) by2 = g_winH - th2 - 12.0f;
                if (by2 < 4.0f) by2 = 4.0f;
                // background
                gl::BindVertexArray(vao);
                gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
                float bg[6][7] = {
                    {bx2-3,by2-3,0, 0.05f,0.05f,0.05f,0.95f}, {bx2+tw2+9,by2-3,0, 0.05f,0.05f,0.05f,0.95f}, {bx2+tw2+9,by2+th2+9,0, 0.05f,0.05f,0.05f,0.95f},
                    {bx2-3,by2-3,0, 0.05f,0.05f,0.05f,0.95f}, {bx2+tw2+9,by2+th2+9,0, 0.05f,0.05f,0.05f,0.95f}, {bx2-3,by2+th2+9,0, 0.05f,0.05f,0.05f,0.95f},
                };
                gl::BufferData(GL_ARRAY_BUFFER, sizeof(bg), bg, GL_STREAM_DRAW);
                gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
                gl::DrawArrays(GL_TRIANGLES, 0, 6);
                // text
                gl::BindVertexArray(tvao);
                gl::BindBuffer(GL_ARRAY_BUFFER, tvbo);
                float vtx[6][9] = {
                    {bx2,by2,0, 1,1,1,1, 0,0}, {bx2+tw2,by2,0, 1,1,1,1, 1,0}, {bx2+tw2,by2+th2,0, 1,1,1,1, 1,1},
                    {bx2,by2,0, 1,1,1,1, 0,0}, {bx2+tw2,by2+th2,0, 1,1,1,1, 1,1}, {bx2,by2+th2,0, 1,1,1,1, 0,1},
                };
                gl::BufferData(GL_ARRAY_BUFFER, sizeof(vtx), vtx, GL_STREAM_DRAW);
                gl::UseProgram(tprog);
                gl::UniformMatrix4fv(tuMVP, 1, GL_FALSE, mvp.m);
                gl::BindTexture(GL_TEXTURE_2D, tt.tex);
                gl::DrawArrays(GL_TRIANGLES, 0, 6);
                gl::UseProgram(prog);
                gl::BindVertexArray(0);
            }
        }

        // window title = status
        char title[300];
        const char* toolNames[9] = { "paintbrush", "brush", "bucket", "picker", "eraser", "rect-bucket", "rectangle", "line", "circle" };
        const char* tbTip[13] = { "Paintbrush", "Brush (multi-color)", "Bucket (flood fill)", "Picker (eyedropper)", "Eraser", "Rect bucket", "Rectangle", "Line", "Circle", "Undo", "Brush size -", "Brush size +", "Save PNG" };
        if (startMode < 0) {
            snprintf(title, sizeof(title), "%s - select editor mode (or pass --texture / --item / --model / --data)", kAppName);
        } else if (ed.toolHover >= 0 && ed.toolHover < 13) {
            snprintf(title, sizeof(title), "%s - %s", kAppName, tbTip[ed.toolHover]);
        } else {
            char modeName[64];
            if (ed.entityMode) snprintf(modeName, sizeof(modeName), "entity");
            else if (ed.modelMode) snprintf(modeName, sizeof(modeName), "model:%s", blockOf(ed.modelBlock).name);
            else snprintf(modeName, sizeof(modeName), "%s", mat::tileName(ed.tile));
            const char* effName = ed.picker ? "picker" : (ed.tool >= 0 ? toolNames[ed.tool] : "none");
            snprintf(title, sizeof(title), "%s [%s] tool=%s size=%dx%d | TAB mode, F5 save%s",
                     kAppName, modeName, ed.entityMode || ed.modelMode ? "-" : effName, ed.brushSize, ed.brushSize,
                     ed.dirty ? " *UNSAVED*" : "");
        }
        SetWindowTextA(g_hwnd, title);

        prevLmb = lmb; prevRmb = rmb; prevMmb = mmb; // update after rendering so UI clicks see the edge

        SwapBuffers(g_hdc);
    }

    return 0;
}
