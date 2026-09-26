#include "tree_sim.hpp"
#include "core/gl.hpp"
#include "core/math.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace gl;

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
static int g_winW = 1600, g_winH = 900;
static bool g_resized = false, g_running = true;
static int g_wheel = 0;
static float g_mouseDX = 0, g_mouseDY = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            g_winW = LOWORD(l); g_winH = HIWORD(l); g_resized = true;
            return 0;
        case WM_MOUSEWHEEL:
            g_wheel += (int)(short)HIWORD(w);
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static void loadWgl() {
    HINSTANCE inst = GetModuleHandle(nullptr);
    HWND hw = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hw) return;
    HDC dc = GetDC(hw);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 24; pfd.cDepthBits = 24;
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

static bool createGL(HWND& hwnd, HDC& hdc, HGLRC& ctx, int w, int h) {
    loadWgl();
    HINSTANCE inst = GetModuleHandle(nullptr);
    WNDCLASSW wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TreeLabWnd";
    if (!RegisterClassW(&wc)) return false;
    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExW(0, L"TreeLabWnd", L"Tree Lab — 逐帧 / 公式 / 力源",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           nullptr, nullptr, inst, nullptr);
    if (!hwnd) return false;
    hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 24; pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(hdc, &pfd);
    SetPixelFormat(hdc, pf, &pfd);
    HGLRC ctxTmp = nullptr;
    if (gl::WglCreateContextAttribsARB) {
        const int cattrs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0
        };
        ctxTmp = gl::WglCreateContextAttribsARB(hdc, nullptr, cattrs);
    }
    if (!ctxTmp) return false;
    wglMakeCurrent(hdc, ctxTmp);
    ctx = ctxTmp;
    return true;
}

static GLuint compile(GLenum type, const char* src) {
    GLuint s = gl::CreateShader(type);
    gl::ShaderSource(s, 1, &src, nullptr);
    gl::CompileShader(s);
    GLint ok = 0;
    gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; gl::GetShaderInfoLog(s, 1024, nullptr, log);
        fprintf(stderr, "[shader] %s\n", log);
    }
    return s;
}
static GLuint link(const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = gl::CreateProgram();
    gl::AttachShader(p, v); gl::AttachShader(p, f); gl::LinkProgram(p);
    gl::DeleteShader(v); gl::DeleteShader(f);
    return p;
}

// ---------------------------------------------------------------------------
// Text (GDI → GL texture)
// ---------------------------------------------------------------------------
struct TextTex { unsigned tex = 0; int w = 0, h = 0; };
static std::unordered_map<std::string, TextTex> g_text;

static TextTex& textTex(const std::string& utf8) {
    auto it = g_text.find(utf8);
    if (it != g_text.end()) return it->second;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(wlen > 0 ? wlen : 1, L'?');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wide[0], wlen);
    HDC hdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(hdc);
    HFONT font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    SelectObject(mem, font);
    SIZE sz{ 8, 16 };
    GetTextExtentPoint32W(mem, wide.c_str(), (int)wide.size(), &sz);
    int w = std::max(2, (int)sz.cx + 6), h = std::max(2, (int)sz.cy + 4);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, bmp);
    RECT r{ 0, 0, w, h };
    FillRect(mem, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    TextOutW(mem, 3, 1, wide.c_str(), (int)wide.size());
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    uint8_t* src = (uint8_t*)bits;
    for (int i = 0; i < w * h; i++) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], rr = src[i * 4 + 2];
        uint8_t a = rr;
        if (g > a) a = g;
        if (b > a) a = b;
        rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = a;
    }
    unsigned tex = 0;
    gl::GenTextures(1, &tex);
    gl::BindTexture(GL_TEXTURE_2D, tex);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    DeleteObject(bmp); DeleteObject(font); DeleteDC(mem); ReleaseDC(nullptr, hdc);
    TextTex t{ tex, w, h };
    g_text[utf8] = t;
    return g_text[utf8];
}

// ---------------------------------------------------------------------------
// Cube geometry
// ---------------------------------------------------------------------------
static void pushBox(std::vector<float>& v, float x, float y, float z,
                    float sx, float sy, float sz, float r, float g, float b) {
    const float hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
    const float n[6][3] = { {0,1,0},{0,-1,0},{1,0,0},{-1,0,0},{0,0,1},{0,0,-1} };
    const float f[6][4][3] = {
        { {-hx,hy,-hz},{hx,hy,-hz},{hx,hy,hz},{-hx,hy,hz} },
        { {-hx,-hy,hz},{hx,-hy,hz},{hx,-hy,-hz},{-hx,-hy,-hz} },
        { {hx,-hy,-hz},{hx,-hy,hz},{hx,hy,hz},{hx,hy,-hz} },
        { {-hx,-hy,hz},{-hx,-hy,-hz},{-hx,hy,-hz},{-hx,hy,hz} },
        { {hx,-hy,hz},{-hx,-hy,hz},{-hx,hy,hz},{hx,hy,hz} },
        { {-hx,-hy,-hz},{hx,-hy,-hz},{hx,hy,-hz},{-hx,hy,-hz} },
    };
    const int idx[6] = { 0,1,2, 0,2,3 };
    for (int face = 0; face < 6; face++) {
        for (int k = 0; k < 6; k++) {
            const float* p = f[face][idx[k]];
            v.push_back(x + p[0]); v.push_back(y + p[1]); v.push_back(z + p[2]);
            v.push_back(n[face][0]); v.push_back(n[face][1]); v.push_back(n[face][2]);
            v.push_back(r); v.push_back(g); v.push_back(b);
        }
    }
}
static void pushCube(std::vector<float>& v, float x, float y, float z, float s,
                     float r, float g, float b) {
    pushBox(v, x, y, z, s, s, s, r, g, b);
}

static const char* VS_WORLD = R"(#version 330 core
uniform mat4 uMVP;
uniform vec3 uSun;
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec3 aCol;
out vec3 vCol;
void main() {
    float l = 0.32 + 0.68 * max(0.0, dot(normalize(aNrm), uSun));
    vCol = aCol * l;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
static const char* FS_WORLD = R"(#version 330 core
in vec3 vCol;
out vec4 frag;
void main() { frag = vec4(vCol, 1.0); }
)";
static const char* VS_LINE = R"(#version 330 core
uniform mat4 uMVP;
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aCol;
out vec3 vCol;
void main() {
    vCol = aCol;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
static const char* FS_LINE = R"(#version 330 core
in vec3 vCol;
out vec4 frag;
void main() { frag = vec4(vCol, 1.0); }
)";
static const char* VS_UI = R"(#version 330 core
uniform vec2 uScreen;
layout(location=0) in vec2 aPos;
layout(location=1) in vec4 aCol;
out vec4 vCol;
void main() {
    vCol = aCol;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
)";
static const char* FS_UI = R"(#version 330 core
in vec4 vCol;
out vec4 frag;
void main() { frag = vCol; }
)";
static const char* VS_TEXT = R"(#version 330 core
uniform vec2 uScreen;
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
)";
static const char* FS_TEXT = R"(#version 330 core
uniform sampler2D uTex;
in vec2 vUV;
out vec4 frag;
void main() { frag = texture(uTex, vUV); }
)";

struct Slider {
    const char* name;
    float* f = nullptr;
    int* i = nullptr;
    float minv, maxv;
    float x = 0, y = 0, w = 0;
};

struct Btn {
    const char* id;
    std::string label;
    float x = 0, y = 0, w = 0, h = 0;
};

int main() {
    SetProcessDPIAware();
    HWND hwnd; HDC hdc; HGLRC ctx;
    if (!createGL(hwnd, hdc, ctx, g_winW, g_winH)) {
        fprintf(stderr, "Failed to create GL context\n");
        return 1;
    }
    if (!gl::loadAll()) {
        fprintf(stderr, "Failed to load GL\n");
        return 1;
    }
    if (gl::WglSwapIntervalEXT) gl::WglSwapIntervalEXT(1);

    GLuint progW = link(VS_WORLD, FS_WORLD);
    GLuint progL = link(VS_LINE, FS_LINE);
    GLuint progUI = link(VS_UI, FS_UI);
    GLuint progT = link(VS_TEXT, FS_TEXT);
    int uMVP = gl::GetUniformLocation(progW, "uMVP");
    int uSun = gl::GetUniformLocation(progW, "uSun");
    int uMVPLine = gl::GetUniformLocation(progL, "uMVP");
    int uScrUI = gl::GetUniformLocation(progUI, "uScreen");
    int uScrT = gl::GetUniformLocation(progT, "uScreen");
    int uTex = gl::GetUniformLocation(progT, "uTex");

    GLuint vaoW = 0, vboW = 0, vaoL = 0, vboL = 0, vaoUI = 0, vboUI = 0, vaoT = 0, vboT = 0;
    gl::GenVertexArrays(1, &vaoW); gl::GenBuffers(1, &vboW);
    gl::GenVertexArrays(1, &vaoL); gl::GenBuffers(1, &vboL);
    gl::GenVertexArrays(1, &vaoUI); gl::GenBuffers(1, &vboUI);
    gl::GenVertexArrays(1, &vaoT); gl::GenBuffers(1, &vboT);

    TreeSim sim;
    applyTreeKind(sim.params, TreeKind::Thick);
    sim.reset();
    sim.bakeHistory();

    bool playing = false;
    bool showLeaves = true;
    bool showSources = true;
    bool useThinFork = false;
    float playHz = 8.0f;
    float playAcc = 0.0f;
    float camYaw = 0.7f, camPitch = 0.45f, camDist = 36.0f;
    int viewFrame = (int)sim.history.size() - 1;
    if (viewFrame < 0) viewFrame = 0;
    int selected = 0;
    float panelScroll = 0.0f;
    float dummyF = 0.0f;
    int dummyI = 0;

    enum DragKind { DragNone, DragParam, DragSrc, DragFrame, DragOrbit };
    DragKind drag = DragNone;
    int dragIndex = -1;
    POINT lastMouse{ 0, 0 };
    Mat4 lastMvp;

    auto dump = [&]() {
        const TreeParams& p = sim.params;
        printf("seed=%u K=%.3f v0=%.2f wood=%.2f trunkW=%d thick=%.0f%% sourceS=%.2f N=%d r=%.1f..%.1f y=%.1f..%.1f off=(%.1f,%.1f) frames=%d\n",
               p.seed, p.K, p.v0, p.woodStr, p.trunkW, p.trunkWFrac * 100.0f, p.sourceS, p.sourceN,
               p.srcRMin, p.srcRMax, p.srcYMin, p.srcYMax, p.srcOffX, p.srcOffZ, (int)sim.history.size());
        printf("  leaf rounds=%d t=%d unsat=%d%% top=%.2f+%.2f u2 side=%.2f+%.2f u2 R=%d..%d exp=%d..%d step=%.1f punch=%d cinch=%.2f trunkKeep=%d\n",
               p.leaf.rounds, p.leaf.tStep, p.leaf.unsatPct,
               p.leaf.topP0, p.leaf.topP1, p.leaf.sideP0, p.leaf.sideP1,
               p.leaf.rMin, p.leaf.rMax, p.leaf.expMin, p.leaf.expMax, p.leaf.expStep,
               p.leaf.punchDiv, p.leaf.cinchU, p.leaf.trunkKeep);
        fflush(stdout);
    };
    dump();

    auto sourceCount = [&]() -> int {
        return (int)sim.sources.size();
    };
    auto sourceAt = [&](int id) -> TreeSource* {
        if (id < 0 || id >= (int)sim.sources.size()) return nullptr;
        return &sim.sources[id];
    };
    auto applyNow = [&]() {
        sim.bakeHistory();
        viewFrame = (int)sim.history.size() - 1;
        if (viewFrame < 0) viewFrame = 0;
        playing = false;
        playAcc = 0;
        dump();
    };
    auto clampFrame = [&]() {
        int n = (int)sim.history.size();
        if (n <= 0) { viewFrame = 0; return; }
        if (viewFrame < 0) viewFrame = 0;
        if (viewFrame > n - 1) viewFrame = n - 1;
    };
    auto sliderValue = [&](const Slider& s) -> float {
        return s.f ? *s.f : (float)*s.i;
    };
    auto setSlider = [&](Slider& s, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        float v = s.minv + t * (s.maxv - s.minv);
        if (s.f) *s.f = v;
        else *s.i = (int)std::round(v);
    };

    auto tPrev = std::chrono::steady_clock::now();
    bool prevSpace = false, prevA = false, prevN = false, prevL = false, prevF = false;
    bool prevLeft = false, prevRight = false, prevHome = false, prevEnd = false;
    bool prevLmb = false;

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            else { TranslateMessage(&msg); DispatchMessage(&msg); }
        }
        if (!g_running) break;

        auto tNow = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(tNow - tPrev).count();
        tPrev = tNow;
        if (dt > 0.1f) dt = 0.1f;

        RECT crect{};
        GetClientRect(hwnd, &crect);
        g_winW = std::max(1, (int)(crect.right - crect.left));
        g_winH = std::max(1, (int)(crect.bottom - crect.top));

        const int rightW = 440;
        const int barH = 58;
        int viewW = std::max(1, g_winW - rightW);
        int viewH = std::max(1, g_winH - barH);
        float panelX = (float)viewW;
        float slX = panelX + 16.0f, slW = (float)rightW - 32.0f, slH = 11.0f;

        POINT mp;
        GetCursorPos(&mp);
        ScreenToClient(hwnd, &mp);
        float mx = (float)mp.x, my = (float)mp.y;
        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        bool inPanel = mx >= panelX;
        bool inView = mx < panelX && my < (float)viewH;
        bool inBar = mx < panelX && my >= (float)viewH;

        bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        if (space && !prevSpace) playing = !playing;
        prevSpace = space;
        bool ka = (GetAsyncKeyState('A') & 0x8000) != 0;
        if (ka && !prevA) applyNow();
        prevA = ka;
        bool kn = (GetAsyncKeyState('N') & 0x8000) != 0;
        if (kn && !prevN) { sim.params.seed += 1; sim.scatterSources(); }
        prevN = kn;
        bool kl = (GetAsyncKeyState('L') & 0x8000) != 0;
        if (kl && !prevL) showLeaves = !showLeaves;
        prevL = kl;
        bool kf = (GetAsyncKeyState('F') & 0x8000) != 0;
        if (kf && !prevF) showSources = !showSources;
        prevF = kf;
        bool kLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        if (kLeft && !prevLeft) { playing = false; viewFrame--; clampFrame(); }
        prevLeft = kLeft;
        bool kRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        if (kRight && !prevRight) { playing = false; viewFrame++; clampFrame(); }
        prevRight = kRight;
        bool kHome = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (kHome && !prevHome) { playing = false; viewFrame = 0; }
        prevHome = kHome;
        bool kEnd = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (kEnd && !prevEnd) {
            playing = false;
            viewFrame = std::max(0, (int)sim.history.size() - 1);
        }
        prevEnd = kEnd;

        TreeSource* sel = sourceAt(selected);
        if (!sel && !sim.sources.empty()) { selected = 0; sel = &sim.sources[0]; }

        Slider formulaSliders[] = {
            { "V0  速率（转向在单位方向上）", &sim.params.v0, nullptr, 0.5f, 10.0f },
            { "K  源对轨迹的力倍率",         &sim.params.K, nullptr, 0.0f, 2.0f },
            { "MAXLEN  主干可走步数",       nullptr, &sim.params.trunkH, 4.0f, 22.0f },
            { "α  能量对数系数  E=α·log(step+1)", &sim.params.energyAlpha, nullptr, 0.0f, 12.0f },
            { "MAXE  能量上限",            &sim.params.maxE, nullptr, 0.0f, 20.0f },
            { "S木  自避让（0=关闭）",       &sim.params.woodStr, nullptr, 0.0f, 2.0f },
        };
        Slider extraSliders[] = {
            { "分叉朝源偏°",               &sim.params.forkAngle, nullptr, 0.0f, 70.0f },
            { "主干最大倾角°  2上1外≈28",  &sim.params.maxTilt, nullptr, 0.0f, 70.0f },
            { "分叉最大倾角°",             &sim.params.maxTiltFork, nullptr, 0.0f, 80.0f },
            { "BRANCH_PROTECT  子枝前N格不跳", nullptr, &sim.params.branchProtect, 0.0f, 8.0f },
            { "MAX_BRANCH  子枝起跳系数",  nullptr, &sim.params.maxBranch, 1.0f, 20.0f },
            { "lean  出发时的水平偏置",     &sim.params.lean, nullptr, 0.0f, 0.8f },
            { "上天S  高处引力（远≈+Y）",   &sim.params.upS, nullptr, 0.0f, 3.0f },
            { "上天Y  高处源高度",         &sim.params.upY, nullptr, 12.0f, 48.0f },
            { "maxWood  木头格数上限",     nullptr, &sim.params.maxWood, 32.0f, 400.0f },
            { "maxDepth  最多分叉层数",    nullptr, &sim.params.maxDepth, 1.0f, 5.0f },
            { "playHz  逐帧回放速度",      &playHz, nullptr, 0.5f, 30.0f },
        };
        Slider scatterSliders[] = {
            { "空间源数量 N  拖动即增删",  nullptr, &sim.params.sourceN, 0.0f, 24.0f },
            { "布源：每个源的S",           &sim.params.sourceS, nullptr, 0.0f, 6.0f },
            { "范围中心偏移 X",            &sim.params.srcOffX, nullptr, -12.0f, 12.0f },
            { "范围中心偏移 Z",            &sim.params.srcOffZ, nullptr, -12.0f, 12.0f },
            { "高度下限 Ymin",             &sim.params.srcYMin, nullptr, 1.0f, 24.0f },
            { "高度上限 Ymax",             &sim.params.srcYMax, nullptr, 2.0f, 28.0f },
            { "水平内径 Rmin  禁正上",     &sim.params.srcRMin, nullptr, 0.5f, 18.0f },
            { "水平外径 Rmax  有多远",     &sim.params.srcRMax, nullptr, 1.0f, 22.0f },
            { "方位起点°  0=+X",          &sim.params.srcAzi0, nullptr, 0.0f, 360.0f },
            { "方位跨度°  360=一圈",      &sim.params.srcAziSpan, nullptr, 20.0f, 360.0f },
        };
        Slider leafSliders[] = {
            { "轮数  判定+外扩",           nullptr, &sim.params.leaf.rounds, 1.0f, 6.0f },
            { "t  每轮上探缩短",           nullptr, &sim.params.leaf.tStep, 0.0f, 8.0f },
            { "挡阈值％  ≥则外扩",         nullptr, &sim.params.leaf.unsatPct, 5.0f, 90.0f },
            { "顶面底P  p=底+高·u²",      &sim.params.leaf.topP0, nullptr, 0.0f, 1.0f },
            { "顶面高P",                   &sim.params.leaf.topP1, nullptr, 0.0f, 1.0f },
            { "侧面底P",                   &sim.params.leaf.sideP0, nullptr, 0.0f, 1.0f },
            { "侧面高P",                   &sim.params.leaf.sideP1, nullptr, 0.0f, 1.0f },
            { "团半径 min",                nullptr, &sim.params.leaf.rMin, 1.0f, 6.0f },
            { "团半径 max",                nullptr, &sim.params.leaf.rMax, 1.0f, 8.0f },
            { "外扩次数 min",              nullptr, &sim.params.leaf.expMin, 0.0f, 4.0f },
            { "外扩次数 max",              nullptr, &sim.params.leaf.expMax, 0.0f, 4.0f },
            { "外扩步长",                  &sim.params.leaf.expStep, nullptr, 0.5f, 8.0f },
            { "挖洞间距  0=关",            nullptr, &sim.params.leaf.punchDiv, 0.0f, 40.0f },
            { "收底高度  0=关",            &sim.params.leaf.cinchU, nullptr, 0.0f, 0.80f },
            { "主干可贴叶（距顶格数）",     nullptr, &sim.params.leaf.trunkKeep, 0.0f, 8.0f },
        };
        Slider srcSliders[] = {
            { "e.x  源位置", sel ? &sel->x : &dummyF, nullptr, -20.0f, 20.0f },
            { "e.y  源位置", sel ? &sel->y : &dummyF, nullptr, 0.0f, 28.0f },
            { "e.z  源位置", sel ? &sel->z : &dummyF, nullptr, -20.0f, 20.0f },
            { "S  该源强度（进公式）", sel ? &sel->strength : &dummyF, nullptr, 0.0f, 6.0f },
        };
        const int nFormula = (int)(sizeof(formulaSliders) / sizeof(formulaSliders[0]));
        const int nExtra = (int)(sizeof(extraSliders) / sizeof(extraSliders[0]));
        const int nScatter = (int)(sizeof(scatterSliders) / sizeof(scatterSliders[0]));
        const int nLeaf = (int)(sizeof(leafSliders) / sizeof(leafSliders[0]));
        const int nSrcSl = (int)(sizeof(srcSliders) / sizeof(srcSliders[0]));
        (void)dummyI;

        std::vector<std::string> formulaLines;
        {
            char b[160];
            formulaLines.push_back("同一组空间源：主干被推开，分叉被拉过去。");
            formulaLines.push_back("初始  v = (lean, V0, lean)   |v|→V0");
            formulaLines.push_back("每步  F = Σ σ rhat K E S / r^2");
            formulaLines.push_back("      v̂ ← normalize(v̂+F)   再限制 ∠(v̂,+Y)≤θmax");
            formulaLines.push_back("      v ← V0·v̂    p ← p + v̂");
            formulaLines.push_back("σ = +1 主干斥力     σ = -1 分叉引力");
            formulaLines.push_back("另有高处引力源（始终吸引）；越高 θmax 越小");
            formulaLines.push_back("E = min(α·log(step+1), MAXE)    r^2 = max(|p-e|^2, 1)");
            snprintf(b, sizeof(b), "当前  V0=%.2f  K=%.2f  θ干=%.0f°  θ枝=%.0f°  源数=%d",
                     sim.params.v0, sim.params.K, sim.params.maxTilt, sim.params.maxTiltFork,
                     (int)sim.sources.size());
            formulaLines.push_back(b);
            snprintf(b, sizeof(b), "子枝  前%d格不跳；第%d格起 p=min(20%%,(MAX_BRANCH-i)*2%%)",
                     sim.params.branchProtect, sim.params.branchProtect + 1);
            formulaLines.push_back(b);
            snprintf(b, sizeof(b), "      MAX_BRANCH=%d  朝源偏 %.0f°；主干分叉仍每格+5%%、后×1/4、+2格",
                     sim.params.maxBranch, sim.params.forkAngle);
            formulaLines.push_back(b);
            formulaLines.push_back("孙枝  同一套朝源偏转后再取反，斥力、不再分枝");
            snprintf(b, sizeof(b), "树冠  %d轮  t=%d  挡≥%d%%  外扩%d–%d次  拖叶子滑条即时重算",
                     sim.params.leaf.rounds, sim.params.leaf.tStep, sim.params.leaf.unsatPct,
                     sim.params.leaf.expMin, sim.params.leaf.expMax);
            formulaLines.push_back(b);
        }

        std::vector<Btn> btns;
        auto addBtn = [&](const char* id, const std::string& label, float x, float y, float w, float h) {
            btns.push_back({ id, label, x, y, w, h });
        };

        float cyLayout = 10.0f + panelScroll;
        auto skip = [&](float h) { cyLayout += h; };
        skip(26);
        const float formulaBoxH = 18.0f * (float)formulaLines.size() + 8.0f;
        skip(formulaBoxH);
        skip(8);
        addBtn("apply", "Apply  用当前公式+源 重新生成  (A)", slX, cyLayout, slW, 36);
        skip(44);
        addBtn("scatter", "随机布源", slX, cyLayout, 100, 26);
        addBtn("seed", "新种子 N", slX + 108, cyLayout, 92, 26);
        addBtn("leaves", showLeaves ? "叶子 开" : "叶子 关", slX + 208, cyLayout, 92, 26);
        addBtn("fields", showSources ? "力源 开" : "力源 关", slX + 308, cyLayout, 92, 26);
        skip(32);
        addBtn("onefork", useThinFork ? "细干分叉 开" : "细干分叉 关", slX, cyLayout, 160, 26);
        skip(36);
        float yGrowH = cyLayout;
        skip(20);
        auto placeSliders = [&](Slider* arr, int n) {
            for (int i = 0; i < n; i++) {
                cyLayout += 18;
                arr[i].x = slX; arr[i].y = cyLayout; arr[i].w = slW;
                cyLayout += 16;
            }
        };
        placeSliders(formulaSliders, nFormula);
        skip(10);
        float yExtraH = cyLayout;
        skip(20);
        placeSliders(extraSliders, nExtra);
        skip(10);
        float yLeafH = cyLayout;
        skip(20);
        placeSliders(leafSliders, nLeaf);
        skip(10);
        float yScatH = cyLayout;
        skip(20);
        placeSliders(scatterSliders, nScatter);
        skip(10);
        float ySrcH = cyLayout;
        skip(20);
        addBtn("addSrc", "+空间源", slX, cyLayout, 110, 24);
        addBtn("delSrc", "删除选中", slX + 118, cyLayout, 110, 24);
        skip(30);

        struct SrcRow { int id; float y, h; std::string label; };
        std::vector<SrcRow> srcRows;
        int nSrc = sourceCount();
        for (int i = 0; i < nSrc; i++) {
            TreeSource* s = sourceAt(i);
            char buf[96];
            snprintf(buf, sizeof(buf), "空间源%d  e=(%.1f, %.1f, %.1f)  S=%.2f",
                     i, s->x, s->y, s->z, s->strength);
            srcRows.push_back({ i, cyLayout, 22.0f, buf });
            cyLayout += 22;
        }
        skip(10);
        float ySelH = cyLayout;
        skip(20);
        placeSliders(srcSliders, nSrcSl);
        skip(16);
        float contentH = cyLayout - panelScroll;
        float maxScroll = std::min(0.0f, (float)g_winH - 8.0f - contentH);
        panelScroll = std::clamp(panelScroll, maxScroll, 0.0f);

        float frameTrackX = 168.0f;
        float frameTrackW = (float)viewW - 280.0f;
        if (frameTrackW < 80.0f) frameTrackW = 80.0f;
        float frameTrackY = (float)viewH + 16.0f;
        addBtn("prev", "<<", 12, (float)viewH + 8, 44, 28);
        addBtn("play", playing ? "暂停" : "播放", 62, (float)viewH + 8, 56, 28);
        addBtn("next", ">>", 124, (float)viewH + 8, 36, 28);

        if (g_wheel) {
            if (inPanel) {
                panelScroll += (g_wheel > 0) ? 48.0f : -48.0f;
                panelScroll = std::clamp(panelScroll, maxScroll, 0.0f);
            } else if (inView) {
                camDist *= (g_wheel > 0) ? 0.9f : 1.11f;
                camDist = std::clamp(camDist, 8.0f, 80.0f);
            } else if (inBar && (int)sim.history.size() > 1) {
                viewFrame += (g_wheel > 0) ? -1 : 1;
                playing = false;
                clampFrame();
            }
            g_wheel = 0;
        }

        auto hitSlider = [&](Slider* arr, int n, float hx, float hy) -> int {
            for (int i = 0; i < n; i++) {
                if (hx >= arr[i].x - 8 && hx <= arr[i].x + arr[i].w + 8 &&
                    hy >= arr[i].y - 8 && hy <= arr[i].y + slH + 8)
                    return i;
            }
            return -1;
        };

        if (lmb && !prevLmb) {
            drag = DragNone;
            dragIndex = -1;
            bool consumed = false;
            for (const Btn& b : btns) {
                if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) {
                    consumed = true;
                    if (strcmp(b.id, "apply") == 0) applyNow();
                    else if (strcmp(b.id, "scatter") == 0) sim.scatterSources();
                    else if (strcmp(b.id, "seed") == 0) { sim.params.seed += 1; sim.scatterSources(); }
                    else if (strcmp(b.id, "leaves") == 0) showLeaves = !showLeaves;
                    else if (strcmp(b.id, "fields") == 0) showSources = !showSources;
                    else if (strcmp(b.id, "onefork") == 0) {
                        useThinFork = !useThinFork;
                        if (useThinFork) {
                            sim.params.maxDepth = 1;
                            sim.params.maxPrimary = 1;
                        } else {
                            sim.params.maxDepth = 2;
                            sim.params.maxPrimary = 0;
                        }
                        applyNow();
                    }
                    else if (strcmp(b.id, "addSrc") == 0) {
                        sim.resizeSources((int)sim.sources.size() + 1);
                        selected = (int)sim.sources.size() - 1;
                    } else if (strcmp(b.id, "delSrc") == 0) {
                        if (selected >= 0 && selected < (int)sim.sources.size()) {
                            sim.sources.erase(sim.sources.begin() + selected);
                            sim.params.sourceN = (int)sim.sources.size();
                            if (selected >= (int)sim.sources.size())
                                selected = (int)sim.sources.size() - 1;
                            if (selected < 0) selected = 0;
                        }
                    } else if (strcmp(b.id, "prev") == 0) { playing = false; viewFrame--; clampFrame(); }
                    else if (strcmp(b.id, "next") == 0) { playing = false; viewFrame++; clampFrame(); }
                    else if (strcmp(b.id, "play") == 0) playing = !playing;
                }
            }
            if (!consumed) {
                for (const SrcRow& r : srcRows) {
                    if (mx >= slX && mx <= slX + slW && my >= r.y && my < r.y + r.h) {
                        selected = r.id;
                        consumed = true;
                    }
                }
            }
            if (!consumed) {
                int gi = hitSlider(formulaSliders, nFormula, mx, my);
                int ei = hitSlider(extraSliders, nExtra, mx, my);
                int li = hitSlider(leafSliders, nLeaf, mx, my);
                int si = hitSlider(scatterSliders, nScatter, mx, my);
                int xi = hitSlider(srcSliders, nSrcSl, mx, my);
                if (gi >= 0) { drag = DragParam; dragIndex = gi; consumed = true; }
                else if (ei >= 0) { drag = DragParam; dragIndex = 200 + ei; consumed = true; }
                else if (li >= 0) { drag = DragParam; dragIndex = 300 + li; consumed = true; }
                else if (si >= 0) { drag = DragParam; dragIndex = 100 + si; consumed = true; }
                else if (xi >= 0) { drag = DragSrc; dragIndex = xi; consumed = true; }
            }
            if (!consumed && inBar && mx >= frameTrackX && mx <= frameTrackX + frameTrackW &&
                my >= frameTrackY - 10 && my <= frameTrackY + slH + 10) {
                drag = DragFrame;
                consumed = true;
            }
            if (!consumed && inView) {
                int picked = -1;
                float best = 18.0f * 18.0f;
                auto tryPick = [&](float px, float py, float pz, int id) {
                    Vec4 clip = lastMvp * Vec4{ px, py, pz, 1.0f };
                    if (clip.w <= 1e-4f) return;
                    float ndcX = clip.x / clip.w, ndcY = clip.y / clip.w;
                    if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f) return;
                    float sx = (ndcX * 0.5f + 0.5f) * (float)viewW;
                    float sy = (0.5f - ndcY * 0.5f) * (float)viewH;
                    float dx = sx - mx, dy = sy - my;
                    float d2 = dx * dx + dy * dy;
                    if (d2 < best) { best = d2; picked = id; }
                };
                for (int i = 0; i < (int)sim.sources.size(); i++) {
                    const TreeSource& e = sim.sources[i];
                    tryPick(e.x, e.y, e.z, i);
                }
                if (picked >= 0) selected = picked;
                else { drag = DragOrbit; lastMouse = mp; }
            }
        }
        if (!lmb) { drag = DragNone; dragIndex = -1; }

        if (drag == DragParam || drag == DragSrc) {
            Slider* s = nullptr;
            if (drag == DragSrc && dragIndex >= 0 && dragIndex < nSrcSl) s = &srcSliders[dragIndex];
            else if (dragIndex >= 0 && dragIndex < nFormula) s = &formulaSliders[dragIndex];
            else if (dragIndex >= 100 && dragIndex < 100 + nScatter) s = &scatterSliders[dragIndex - 100];
            else if (dragIndex >= 200 && dragIndex < 200 + nExtra) s = &extraSliders[dragIndex - 200];
            else if (dragIndex >= 300 && dragIndex < 300 + nLeaf) s = &leafSliders[dragIndex - 300];
            if (s) {
                setSlider(*s, (mx - s->x) / s->w);
                if (drag == DragParam && dragIndex >= 300 && dragIndex < 300 + nLeaf) {
                    sim.rebuildLeaves();
                    viewFrame = std::max(0, (int)sim.history.size() - 1);
                }
            }
        }
        if (drag == DragFrame) {
            int n = (int)sim.history.size();
            if (n > 1) {
                float t = (mx - frameTrackX) / frameTrackW;
                t = std::clamp(t, 0.0f, 1.0f);
                viewFrame = (int)std::round(t * (float)(n - 1));
                playing = false;
            }
        }
        if (drag == DragOrbit && lmb) {
            camYaw += (mp.x - lastMouse.x) * 0.008f;
            camPitch += (mp.y - lastMouse.y) * 0.008f;
            camPitch = std::clamp(camPitch, 0.05f, 1.45f);
            lastMouse = mp;
        }
        {
            static bool rDown = false;
            static POINT rPrev{ 0, 0 };
            if (rmb && inView) {
                if (!rDown) { rPrev = mp; rDown = true; }
                camYaw += (mp.x - rPrev.x) * 0.008f;
                camPitch += (mp.y - rPrev.y) * 0.008f;
                camPitch = std::clamp(camPitch, 0.05f, 1.45f);
                rPrev = mp;
            } else {
                rDown = false;
            }
        }
        prevLmb = lmb;

        if ((int)sim.sources.size() != sim.params.sourceN)
            sim.resizeSources(sim.params.sourceN);
        if (selected >= (int)sim.sources.size())
            selected = std::max(0, (int)sim.sources.size() - 1);

        if (playing) {
            playAcc += dt * playHz;
            int n = (int)playAcc;
            if (n > 8) n = 8;
            playAcc -= (float)n;
            int last = std::max(0, (int)sim.history.size() - 1);
            for (int i = 0; i < n; i++) {
                if (viewFrame >= last) { playing = false; break; }
                viewFrame++;
            }
        }
        clampFrame();

        const TreeSim::Frame* fr = nullptr;
        if (!sim.history.empty())
            fr = &sim.history[viewFrame];
        const std::vector<IVec3>& woods = fr ? fr->woods : sim.woods;
        const std::vector<IVec3>& leaves = fr ? fr->leaves : sim.leaves;
        const std::vector<TreeShoot>& tips = fr ? fr->tips : sim.tips;

        std::vector<float> mesh;
        mesh.reserve((woods.size() + leaves.size() + 40) * 36 * 9);
        pushBox(mesh, 0.0f, -0.06f, 0.0f, 40.0f, 0.12f, 40.0f, 0.76f, 0.82f, 0.70f);
        for (const IVec3& w : woods)
            pushCube(mesh, (float)w.x + 0.5f, (float)w.y + 0.5f, (float)w.z + 0.5f, 0.92f, 0.50f, 0.30f, 0.12f);
        if (showLeaves)
            for (const IVec3& L : leaves)
                pushCube(mesh, (float)L.x + 0.5f, (float)L.y + 0.5f, (float)L.z + 0.5f, 0.88f, 0.28f, 0.62f, 0.18f);
        for (const TreeShoot& t : tips) {
            pushCube(mesh, t.x, t.y, t.z, 0.55f, 0.25f, 0.75f, 1.0f);
            float vl = std::sqrt(t.vx * t.vx + t.vy * t.vy + t.vz * t.vz);
            if (vl > 1e-4f) {
                float sz = 0.32f + std::min(0.45f, vl * 0.07f);
                // trunk heading = pale yellow, fork heading = orange
                if (t.attract)
                    pushCube(mesh, t.x + t.vx / vl * 0.7f, t.y + t.vy / vl * 0.7f, t.z + t.vz / vl * 0.7f,
                             sz, 1.0f, 0.55f, 0.15f);
                else
                    pushCube(mesh, t.x + t.vx / vl * 0.7f, t.y + t.vy / vl * 0.7f, t.z + t.vz / vl * 0.7f,
                             sz, 0.95f, 0.92f, 0.35f);
            }
        }
        if (showSources) {
            auto drawSrc = [&](const TreeSource& s, int id, float r, float g, float b, float baseSz) {
                bool hi = (id == selected);
                float sz = hi ? baseSz * 1.55f : baseSz;
                float k = hi ? 1.15f : 1.0f;
                pushCube(mesh, s.x, s.y, s.z, sz, std::min(1.0f, r * k), std::min(1.0f, g * k), std::min(1.0f, b * k));
            };
            for (int i = 0; i < (int)sim.sources.size(); i++)
                drawSrc(sim.sources[i], i, 0.20f, 0.85f, 0.95f, 0.36f);
            if (sim.params.upS > 1e-5f) {
                TreeSource up{
                    sim.params.originXZ() + sim.params.srcOffX,
                    sim.params.upY,
                    sim.params.originXZ() + sim.params.srcOffZ,
                    sim.params.upS
                };
                drawSrc(up, -1, 0.95f, 0.72f, 0.22f, 0.42f);
            }
        }

        std::vector<float> lines;
        if (showSources) {
            const TreeParams& rp = sim.params;
            float cx = rp.originXZ() + rp.srcOffX;
            float cz = rp.originXZ() + rp.srcOffZ;
            float yLo = std::min(rp.srcYMin, rp.srcYMax);
            float yHi = std::max(rp.srcYMin, rp.srcYMax);
            float rLo = std::max(0.4f, std::min(rp.srcRMin, rp.srcRMax));
            float rHi = std::max(rLo, std::max(rp.srcRMin, rp.srcRMax));
            float span = std::clamp(rp.srcAziSpan, 8.0f, 360.0f) * kDeg2Rad;
            float a0 = rp.srcAzi0 * kDeg2Rad;
            const float dashOn = 0.38f, dashOff = 0.26f;
            const float gr = 0.22f, gg = 0.22f, gb = 0.24f;
            auto emit = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
                lines.push_back(x0); lines.push_back(y0); lines.push_back(z0);
                lines.push_back(gr); lines.push_back(gg); lines.push_back(gb);
                lines.push_back(x1); lines.push_back(y1); lines.push_back(z1);
                lines.push_back(gr); lines.push_back(gg); lines.push_back(gb);
            };
            auto dashLine = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
                float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
                float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (len < 1e-4f) return;
                dx /= len; dy /= len; dz /= len;
                float t = 0.0f;
                bool on = true;
                while (t < len) {
                    float step = on ? dashOn : dashOff;
                    float t1 = std::min(len, t + step);
                    if (on)
                        emit(x0 + dx * t, y0 + dy * t, z0 + dz * t,
                             x0 + dx * t1, y0 + dy * t1, z0 + dz * t1);
                    t = t1;
                    on = !on;
                }
            };
            auto dashArc = [&](float y, float r) {
                float arc = r * span;
                float s = 0.0f;
                bool on = true;
                while (s < arc) {
                    float step = on ? dashOn : dashOff;
                    float s1 = std::min(arc, s + step);
                    if (on) {
                        float th0 = a0 + s / r, th1 = a0 + s1 / r;
                        emit(cx + r * std::cos(th0), y, cz + r * std::sin(th0),
                             cx + r * std::cos(th1), y, cz + r * std::sin(th1));
                    }
                    s = s1;
                    on = !on;
                }
            };
            dashArc(yLo, rLo); dashArc(yLo, rHi);
            dashArc(yHi, rLo); dashArc(yHi, rHi);
            int posts = (rp.srcAziSpan >= 350.0f) ? 8 : 4;
            for (int i = 0; i < posts; i++) {
                float t = (rp.srcAziSpan >= 350.0f)
                    ? (float)i / (float)posts
                    : (posts <= 1 ? 0.0f : (float)i / (float)(posts - 1));
                float a = a0 + t * span;
                float ca = std::cos(a), sa = std::sin(a);
                float xL = cx + rLo * ca, zL = cz + rLo * sa;
                float xH = cx + rHi * ca, zH = cz + rHi * sa;
                dashLine(xL, yLo, zL, xL, yHi, zL);
                dashLine(xH, yLo, zH, xH, yHi, zH);
                dashLine(xL, yLo, zL, xH, yLo, zH);
                dashLine(xL, yHi, zL, xH, yHi, zH);
            }
        }

        float cy = 9.0f;
        Vec3 eye{
            std::cos(camPitch) * std::sin(camYaw) * camDist,
            std::sin(camPitch) * camDist + cy,
            std::cos(camPitch) * std::cos(camYaw) * camDist
        };
        Mat4 proj = Mat4::perspective(55.0f, (float)viewW / (float)std::max(1, viewH), 0.1f, 200.0f);
        Mat4 viewM = Mat4::lookAt(eye, Vec3{ 0, cy, 0 }, Vec3{ 0, 1, 0 });
        Mat4 mvp = proj * viewM;
        lastMvp = mvp;

        gl::Viewport(0, 0, g_winW, g_winH);
        gl::Disable(GL_SCISSOR_TEST);
        gl::Enable(GL_DEPTH_TEST);
        gl::Enable(GL_CULL_FACE);
        gl::ClearColor(0.86f, 0.88f, 0.90f, 1);
        gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        gl::Viewport(0, barH, viewW, viewH);
        gl::Enable(GL_SCISSOR_TEST);
        gl::Scissor(0, barH, viewW, viewH);
        gl::ClearColor(0.90f, 0.92f, 0.94f, 1);
        gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        gl::UseProgram(progW);
        gl::UniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
        gl::Uniform3f(uSun, 0.45f, 0.82f, 0.35f);
        gl::BindVertexArray(vaoW);
        gl::BindBuffer(GL_ARRAY_BUFFER, vboW);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh.size() * sizeof(float)),
                       mesh.empty() ? nullptr : mesh.data(), GL_DYNAMIC_DRAW);
        gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)0);
        gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(3 * sizeof(float)));
        gl::VertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void*)(6 * sizeof(float)));
        gl::EnableVertexAttribArray(0);
        gl::EnableVertexAttribArray(1);
        gl::EnableVertexAttribArray(2);
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(mesh.size() / 9));
        if (!lines.empty()) {
            gl::Disable(GL_CULL_FACE);
            gl::UseProgram(progL);
            gl::UniformMatrix4fv(uMVPLine, 1, GL_FALSE, mvp.m);
            gl::BindVertexArray(vaoL);
            gl::BindBuffer(GL_ARRAY_BUFFER, vboL);
            gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lines.size() * sizeof(float)),
                           lines.data(), GL_DYNAMIC_DRAW);
            gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
            gl::EnableVertexAttribArray(0);
            gl::EnableVertexAttribArray(1);
            gl::DrawArrays(GL_LINES, 0, (GLsizei)(lines.size() / 6));
        }
        gl::Disable(GL_SCISSOR_TEST);

        gl::Viewport(0, 0, g_winW, g_winH);
        gl::Disable(GL_DEPTH_TEST);
        gl::Disable(GL_CULL_FACE);
        gl::Disable(GL_SCISSOR_TEST);
        gl::Enable(GL_BLEND);
        gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        std::vector<float> ui;
        auto quad = [&](float x, float y, float w, float h, float r, float g, float b, float a) {
            // Window Y-down → NDC Y-up: this order is CCW in clip space.
            float q[6][2] = { {x,y+h},{x+w,y+h},{x+w,y}, {x,y+h},{x+w,y},{x,y} };
            for (int i = 0; i < 6; i++) {
                ui.push_back(q[i][0]); ui.push_back(q[i][1]);
                ui.push_back(r); ui.push_back(g); ui.push_back(b); ui.push_back(a);
            }
        };
        quad(panelX, 0, (float)rightW, (float)g_winH, 0.14f, 0.15f, 0.18f, 1);
        quad(panelX, 0, 3, (float)g_winH, 0.42f, 0.78f, 0.36f, 1);
        quad(0, (float)viewH, (float)viewW, (float)barH, 0.10f, 0.11f, 0.14f, 1);

        float formulaTop = 10.0f + panelScroll;
        quad(slX - 4, formulaTop + 22, slW + 8, formulaBoxH, 0.10f, 0.12f, 0.16f, 1);

        for (const Btn& b : btns) {
            bool hov = mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h;
            float r = 0.24f, g = 0.24f, bl = 0.24f;
            if (strcmp(b.id, "apply") == 0) { r = hov ? 0.28f : 0.18f; g = hov ? 0.62f : 0.48f; bl = 0.20f; }
            else if (hov) { r = g = bl = 0.38f; }
            quad(b.x, b.y, b.w, b.h, r, g, bl, 1);
        }
        for (const SrcRow& row : srcRows) {
            bool hi = row.id == selected;
            float r = hi ? 0.16f : 0.10f, g = hi ? 0.32f : 0.16f, b = hi ? 0.36f : 0.18f;
            quad(slX, row.y, slW, row.h - 1, r, g, b, 1);
        }
        auto drawSlider = [&](const Slider& s) {
            float val = sliderValue(s);
            float t = (val - s.minv) / (s.maxv - s.minv);
            t = std::clamp(t, 0.0f, 1.0f);
            quad(s.x, s.y, s.w, slH, 0.15f, 0.15f, 0.15f, 1);
            quad(s.x, s.y, s.w * t, slH, 0.45f, 0.72f, 0.32f, 1);
            quad(s.x + s.w * t - 5, s.y - 3, 10, slH + 6, 0.9f, 0.9f, 0.9f, 1);
        };
        for (int i = 0; i < nFormula; i++) drawSlider(formulaSliders[i]);
        for (int i = 0; i < nExtra; i++) drawSlider(extraSliders[i]);
        for (int i = 0; i < nLeaf; i++) drawSlider(leafSliders[i]);
        for (int i = 0; i < nScatter; i++) drawSlider(scatterSliders[i]);
        for (int i = 0; i < nSrcSl; i++) drawSlider(srcSliders[i]);

        int nHist = (int)sim.history.size();
        float ft = (nHist > 1) ? (float)viewFrame / (float)(nHist - 1) : 0.0f;
        quad(frameTrackX, frameTrackY, frameTrackW, slH, 0.18f, 0.18f, 0.18f, 1);
        quad(frameTrackX, frameTrackY, frameTrackW * ft, slH, 0.35f, 0.62f, 0.90f, 1);
        quad(frameTrackX + frameTrackW * ft - 5, frameTrackY - 4, 10, slH + 8, 0.95f, 0.95f, 0.95f, 1);

        gl::UseProgram(progUI);
        gl::Uniform2f(uScrUI, (float)g_winW, (float)g_winH);
        gl::BindVertexArray(vaoUI);
        gl::BindBuffer(GL_ARRAY_BUFFER, vboUI);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(ui.size() * sizeof(float)), ui.data(), GL_DYNAMIC_DRAW);
        gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
        gl::EnableVertexAttribArray(0);
        gl::EnableVertexAttribArray(1);
        gl::DrawArrays(GL_TRIANGLES, 0, (GLsizei)(ui.size() / 6));

        auto drawText = [&](const std::string& s, float x, float y, float scale = 1.0f) {
            TextTex& tt = textTex(s);
            float tw = tt.w * scale, th = tt.h * scale;
            float qv[] = {
                x, y + th, 0, 1,  x + tw, y + th, 1, 1,  x + tw, y, 1, 0,
                x, y + th, 0, 1,  x + tw, y, 1, 0,  x, y, 0, 0
            };
            gl::UseProgram(progT);
            gl::Uniform2f(uScrT, (float)g_winW, (float)g_winH);
            gl::Uniform1i(uTex, 0);
            gl::BindTexture(GL_TEXTURE_2D, tt.tex);
            gl::BindVertexArray(vaoT);
            gl::BindBuffer(GL_ARRAY_BUFFER, vboT);
            gl::BufferData(GL_ARRAY_BUFFER, sizeof(qv), qv, GL_DYNAMIC_DRAW);
            gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
            gl::EnableVertexAttribArray(0);
            gl::EnableVertexAttribArray(1);
            gl::DrawArrays(GL_TRIANGLES, 0, 6);
        };

        gl::Enable(GL_SCISSOR_TEST);
        gl::Scissor(viewW, 0, rightW, g_winH);
        drawText("公式与符号", slX, formulaTop, 0.95f);
        for (int i = 0; i < (int)formulaLines.size(); i++)
            drawText(formulaLines[i], slX, formulaTop + 26 + i * 18, 0.66f);

        drawText("公式里的量  (Apply 后生效)", slX, yGrowH, 0.82f);
        drawText("不在力公式里", slX, yExtraH, 0.82f);
        drawText("树冠叶子  （拖动即时重算，不用 Apply）", slX, yLeafH, 0.82f);
        drawText("生成范围（拖滑条看线框，点「随机布源」才写入点）", slX, yScatH, 0.76f);
        drawText("空间源  主干=斥力  分叉=引力", slX, ySrcH, 0.82f);
        drawText("选中源 = 公式中的 e 和 S", slX, ySelH, 0.82f);

        auto drawSliderLabel = [&](Slider* arr, int n) {
            for (int i = 0; i < n; i++) {
                char buf[96];
                if (arr[i].i) snprintf(buf, sizeof(buf), "%s  %d", arr[i].name, *arr[i].i);
                else snprintf(buf, sizeof(buf), "%s  %.2f", arr[i].name, sliderValue(arr[i]));
                drawText(buf, arr[i].x, arr[i].y - 17, 0.76f);
            }
        };
        drawSliderLabel(formulaSliders, nFormula);
        drawSliderLabel(extraSliders, nExtra);
        drawSliderLabel(leafSliders, nLeaf);
        for (int i = 0; i < nScatter; i++) {
            char buf[96];
            if (scatterSliders[i].i) snprintf(buf, sizeof(buf), "%s  %d", scatterSliders[i].name, *scatterSliders[i].i);
            else snprintf(buf, sizeof(buf), "%s  %.2f", scatterSliders[i].name, sliderValue(scatterSliders[i]));
            drawText(buf, scatterSliders[i].x, scatterSliders[i].y - 17, 0.78f);
        }
        for (int i = 0; i < nSrcSl; i++) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s  %.2f", srcSliders[i].name, sliderValue(srcSliders[i]));
            drawText(buf, srcSliders[i].x, srcSliders[i].y - 17, 0.78f);
        }
        for (const SrcRow& row : srcRows)
            drawText(row.label, slX + 6, row.y + 1, 0.72f);
        for (const Btn& b : btns) {
            if (b.x >= panelX)
                drawText(b.label, b.x + 8, b.y + (b.h > 30 ? 8 : 3), b.h > 30 ? 0.92f : 0.78f);
        }
        gl::Disable(GL_SCISSOR_TEST);

        gl::Enable(GL_SCISSOR_TEST);
        gl::Scissor(0, 0, viewW, barH);
        for (const Btn& b : btns) {
            if (b.x < panelX)
                drawText(b.label, b.x + 8, b.y + 5, 0.80f);
        }
        char st[160];
        snprintf(st, sizeof(st), "帧 %d / %d   木头 %d   叶子 %d   活梢 %d   seed %u",
                 nHist ? viewFrame : 0, std::max(0, nHist - 1),
                 (int)woods.size(), (int)leaves.size(), (int)tips.size(), sim.params.seed);
        drawText(st, frameTrackX + frameTrackW + 10, (float)viewH + 8, 0.78f);
        drawText("左右键逐帧  Home/End  空格播放  点选力源", 12, (float)viewH + 40, 0.68f);
        gl::Disable(GL_SCISSOR_TEST);

        SwapBuffers(hdc);
        (void)g_mouseDX; (void)g_mouseDY;
    }
    return 0;
}
