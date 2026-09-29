// Six orthographic views of an entity .model, for checking cuboid silhouettes.
// One shared scale across the sheet, so a wide model stays wide in every panel.
#include "material/image.hpp"
#include "world/player_model.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct View {
    const char* name;
    Vec3 right;
    Vec3 up;
    Vec3 closer;
    bool ground;
};

struct Rgba {
    uint8_t r, g, b, a;
};

constexpr int kSs = 2;
constexpr Rgba kBg{ 236, 232, 224, 255 };
constexpr Rgba kCaption{ 42, 40, 38, 255 };
constexpr Rgba kGround{ 186, 180, 168, 255 };

const View kViews[6] = {
    { "front",  { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },  true },
    { "right",  { 0, 0,-1 }, { 0, 1, 0 }, { 1, 0, 0 },  true },
    { "back",   {-1, 0, 0 }, { 0, 1, 0 }, { 0, 0,-1 },  true },
    { "left",   { 0, 0, 1 }, { 0, 1, 0 }, {-1, 0, 0 },  true },
    { "top",    { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 },  false },
    { "bottom", { 1, 0, 0 }, { 0, 0,-1 }, { 0,-1, 0 },  false },
};

struct Glyph {
    char c;
    uint8_t row[7];
};

const Glyph kFont[] = {
    { 'a', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'b', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'c', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'e', { 0x0E, 0x11, 0x1F, 0x10, 0x10, 0x11, 0x0E } },
    { 'f', { 0x0E, 0x04, 0x1F, 0x04, 0x04, 0x04, 0x04 } },
    { 'g', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E } },
    { 'h', { 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x11 } },
    { 'i', { 0x04, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'k', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'l', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'm', { 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11 } },
    { 'n', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'o', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'p', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'r', { 0x16, 0x19, 0x10, 0x10, 0x10, 0x10, 0x10 } },
    { 't', { 0x04, 0x04, 0x1F, 0x04, 0x04, 0x04, 0x0C } },
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

const uint8_t* glyphRows(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (const Glyph& g : kFont)
        if (g.c == c) return g.row;
    return kFont[sizeof(kFont) / sizeof(kFont[0]) - 1].row;
}

int textWidth(const char* s) {
    int n = 0;
    for (const char* p = s; *p; ++p) n++;
    if (n <= 0) return 0;
    return n * 6 - 1;
}

struct Buffer {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba;
    std::vector<float> depth;

    void reset(int W, int H, Rgba bg) {
        w = W;
        h = H;
        rgba.assign((size_t)W * H * 4, 0);
        depth.assign((size_t)W * H, -1.0e30f);
        for (int i = 0; i < W * H; i++) {
            rgba[(size_t)i * 4 + 0] = bg.r;
            rgba[(size_t)i * 4 + 1] = bg.g;
            rgba[(size_t)i * 4 + 2] = bg.b;
            rgba[(size_t)i * 4 + 3] = bg.a;
        }
    }

    void put(int x, int y, float z, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        size_t i = (size_t)y * w + x;
        if (z < depth[i]) return;
        depth[i] = z;
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = 255;
    }

    void darken(int x, int y, float z, float extent) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        size_t i = (size_t)y * w + x;
        if (depth[i] < -1.0e20f) return;
        if (z < depth[i] - extent * 0.02f) return;
        uint8_t* p = &rgba[i * 4];
        p[0] = (uint8_t)(p[0] * 0.42f);
        p[1] = (uint8_t)(p[1] * 0.42f);
        p[2] = (uint8_t)(p[2] * 0.42f);
    }
};

Vec3 viewOf(const Vec3& p, const View& v) {
    return { p.dot(v.right), p.dot(v.up), p.dot(v.closer) };
}

float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

uint8_t clampByte(float c) {
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    return (uint8_t)(c * 255.0f + 0.5f);
}

void fillTri(Buffer& buf, const Vec3& a, const Vec3& b, const Vec3& c, uint8_t r, uint8_t g, uint8_t bl) {
    float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    Vec3 A = a, B = b, C = c;
    if (area < 0.0f) {
        B = c;
        C = b;
        area = -area;
    }
    if (area < 1e-4f) return;
    int minx = (int)std::floor(std::min(A.x, std::min(B.x, C.x)));
    int maxx = (int)std::ceil(std::max(A.x, std::max(B.x, C.x)));
    int miny = (int)std::floor(std::min(A.y, std::min(B.y, C.y)));
    int maxy = (int)std::ceil(std::max(A.y, std::max(B.y, C.y)));
    minx = std::max(minx, 0);
    miny = std::max(miny, 0);
    maxx = std::min(maxx, buf.w - 1);
    maxy = std::min(maxy, buf.h - 1);
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = edge(B.x, B.y, C.x, C.y, px, py);
            float w1 = edge(C.x, C.y, A.x, A.y, px, py);
            float w2 = edge(A.x, A.y, B.x, B.y, px, py);
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            float z = (w0 * A.z + w1 * B.z + w2 * C.z) / area;
            buf.put(x, y, z, r, g, bl);
        }
    }
}

void strokeEdge(Buffer& buf, const Vec3& a, const Vec3& b, float extent) {
    float dx = b.x - a.x, dy = b.y - a.y;
    int steps = (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)));
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float x = a.x + dx * t;
        float y = a.y + dy * t;
        float z = a.z + (b.z - a.z) * t;
        int ix = (int)std::lround(x);
        int iy = (int)std::lround(y);
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++)
                buf.darken(ix + ox, iy + oy, z, extent);
    }
}

bool overlaps(const pm::Part& a, const pm::Part& b, float pad) {
    Vec3 a0 = a.center - a.half - Vec3{ pad, pad, pad };
    Vec3 a1 = a.center + a.half + Vec3{ pad, pad, pad };
    Vec3 b0 = b.center - b.half - Vec3{ pad, pad, pad };
    Vec3 b1 = b.center + b.half + Vec3{ pad, pad, pad };
    return a0.x <= b1.x && a1.x >= b0.x && a0.y <= b1.y && a1.y >= b0.y && a0.z <= b1.z && a1.z >= b0.z;
}

void reportParts(const std::string& stem, const std::vector<pm::Part>& parts) {
    Vec3 mn{ 1e9f, 1e9f, 1e9f }, mx{ -1e9f, -1e9f, -1e9f };
    for (const pm::Part& p : parts) {
        Vec3 c[8];
        pm::partWorldCorners(p, c);
        for (const Vec3& v : c) {
            mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
            mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
        }
    }
    std::cout << stem << "  parts=" << parts.size()
              << "  y " << mn.y << ".." << mx.y
              << "  size " << (mx.x - mn.x) << " x " << (mx.y - mn.y) << " x " << (mx.z - mn.z);
    std::string isolated;
    for (size_t i = 0; i < parts.size(); i++) {
        bool hit = false;
        for (size_t j = 0; j < parts.size(); j++) {
            if (i == j) continue;
            if (overlaps(parts[i], parts[j], 0.02f)) { hit = true; break; }
        }
        if (!hit) {
            if (!isolated.empty()) isolated += ",";
            isolated += parts[i].name.empty() ? "?" : parts[i].name;
        }
    }
    if (!isolated.empty()) std::cout << "  isolated=" << isolated;
    std::cout << "\n";
}

void drawText(std::vector<uint8_t>& img, int imgW, int imgH, int x, int y, const char* text, int scale) {
    int cx = x;
    for (const char* p = text; *p; ++p) {
        const uint8_t* rows = glyphRows(*p);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if ((rows[row] & (1 << (4 - col))) == 0) continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = cx + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px < 0 || py < 0 || px >= imgW || py >= imgH) continue;
                        size_t i = ((size_t)py * imgW + px) * 4;
                        img[i] = 236; img[i + 1] = 230; img[i + 2] = 214; img[i + 3] = 255;
                    }
                }
            }
        }
        cx += 6 * scale;
    }
}

bool renderOne(const std::filesystem::path& modelPath, const std::filesystem::path& outDir, int cell) {
    pm::EntityFile ef = pm::loadEntity(modelPath.string().c_str());
    if (ef.parts.empty()) {
        std::cerr << "no parts: " << modelPath.string() << "\n";
        return false;
    }
    const std::string stem = modelPath.stem().string();
    reportParts(stem, ef.parts);

    Vec3 light = Vec3{ 0.30f, 0.88f, 0.36f }.normalized();
    const int faces[6][4] = {
        { 1, 3, 7, 5 },
        { 0, 4, 6, 2 },
        { 2, 3, 7, 6 },
        { 0, 1, 5, 4 },
        { 4, 5, 7, 6 },
        { 0, 1, 3, 2 },
    };

    Vec3 worldMn{ 1e9f, 1e9f, 1e9f }, worldMx{ -1e9f, -1e9f, -1e9f };
    for (const pm::Part& p : ef.parts) {
        Vec3 c[8];
        pm::partWorldCorners(p, c);
        for (const Vec3& v : c) {
            worldMn.x = std::min(worldMn.x, v.x); worldMn.y = std::min(worldMn.y, v.y); worldMn.z = std::min(worldMn.z, v.z);
            worldMx.x = std::max(worldMx.x, v.x); worldMx.y = std::max(worldMx.y, v.y); worldMx.z = std::max(worldMx.z, v.z);
        }
    }
    float extent = std::max(worldMx.x - worldMn.x, std::max(worldMx.y - worldMn.y, worldMx.z - worldMn.z));
    if (extent < 1e-3f) extent = 1.0f;

    const int caption = 28;
    const int gap = 10;
    const int margin = 8;
    const int sheetW = margin * 2 + cell * 3 + gap * 2;
    const int sheetH = margin * 2 + (cell + caption) * 2 + gap;
    std::vector<uint8_t> sheet((size_t)sheetW * sheetH * 4, 0);
    for (int i = 0; i < sheetW * sheetH; i++) {
        sheet[(size_t)i * 4 + 0] = 28;
        sheet[(size_t)i * 4 + 1] = 27;
        sheet[(size_t)i * 4 + 2] = 26;
        sheet[(size_t)i * 4 + 3] = 255;
    }

    const int hi = cell * kSs;
    Buffer buf;
    for (int vi = 0; vi < 6; vi++) {
        const View& view = kViews[vi];
        buf.reset(hi, hi, kBg);

        float minS = 1e9f, maxS = -1e9f, minU = 1e9f, maxU = -1e9f;
        for (const pm::Part& p : ef.parts) {
            Vec3 c[8];
            pm::partWorldCorners(p, c);
            for (const Vec3& w : c) {
                Vec3 s = viewOf(w, view);
                minS = std::min(minS, s.x); maxS = std::max(maxS, s.x);
                minU = std::min(minU, s.y); maxU = std::max(maxU, s.y);
            }
        }
        float cx = (minS + maxS) * 0.5f;
        float cy = (minU + maxU) * 0.5f;
        float scale = (hi * 0.88f) / extent;
        auto project = [&](const Vec3& w) {
            Vec3 s = viewOf(w, view);
            return Vec3{ hi * 0.5f + (s.x - cx) * scale, hi * 0.5f - (s.y - cy) * scale, s.z };
        };

        if (view.ground) {
            Vec3 g0 = project({ -8.0f, 0.0f, 0.0f });
            Vec3 g1 = project({ 8.0f, 0.0f, 0.0f });
            int gy = (int)std::lround(g0.y);
            for (int x = 0; x < hi; x++) buf.put(x, gy, -1.0e20f, kGround.r, kGround.g, kGround.b);
            (void)g1;
        }

        struct Edge { Vec3 a, b; };
        std::vector<Edge> edges;
        for (const pm::Part& p : ef.parts) {
            Vec3 c[8];
            pm::partWorldCorners(p, c);
            Vec3 s[8];
            for (int i = 0; i < 8; i++) s[i] = project(c[i]);
            for (int f = 0; f < 6; f++) {
                const Vec3& a = s[faces[f][0]];
                const Vec3& b = s[faces[f][1]];
                const Vec3& d = s[faces[f][2]];
                const Vec3& e = s[faces[f][3]];
                Vec3 n = (c[faces[f][1]] - c[faces[f][0]]).cross(c[faces[f][3]] - c[faces[f][0]]).normalized();
                float nd = n.dot(light);
                float shade = nd > 0.0f ? 0.40f + 0.60f * nd : 0.22f + 0.10f * (1.0f + nd);
                uint8_t r = clampByte(p.color.x * shade);
                uint8_t g = clampByte(p.color.y * shade);
                uint8_t bch = clampByte(p.color.z * shade);
                fillTri(buf, a, b, d, r, g, bch);
                fillTri(buf, a, d, e, r, g, bch);
                edges.push_back({ a, b });
                edges.push_back({ b, d });
                edges.push_back({ d, e });
                edges.push_back({ e, a });
            }
        }
        for (const Edge& e : edges) strokeEdge(buf, e.a, e.b, extent);

        std::vector<uint8_t> panel((size_t)cell * cell * 4, 255);
        for (int y = 0; y < cell; y++) {
            for (int x = 0; x < cell; x++) {
                int r = 0, g = 0, b = 0, n = 0;
                for (int oy = 0; oy < kSs; oy++) {
                    for (int ox = 0; ox < kSs; ox++) {
                        size_t si = ((size_t)(y * kSs + oy) * hi + (x * kSs + ox)) * 4;
                        r += buf.rgba[si]; g += buf.rgba[si + 1]; b += buf.rgba[si + 2];
                        n++;
                    }
                }
                size_t di = ((size_t)y * cell + x) * 4;
                panel[di] = (uint8_t)(r / n);
                panel[di + 1] = (uint8_t)(g / n);
                panel[di + 2] = (uint8_t)(b / n);
                panel[di + 3] = 255;
            }
        }

        int col = vi % 3;
        int row = vi / 3;
        int ox = margin + col * (cell + gap);
        int oy = margin + row * (cell + caption + gap);
        for (int y = 0; y < cell; y++) {
            for (int x = 0; x < cell; x++) {
                size_t s = ((size_t)y * cell + x) * 4;
                size_t d = ((size_t)(oy + y) * sheetW + (ox + x)) * 4;
                sheet[d] = panel[s];
                sheet[d + 1] = panel[s + 1];
                sheet[d + 2] = panel[s + 2];
                sheet[d + 3] = 255;
            }
        }
        for (int y = 0; y < caption; y++) {
            for (int x = 0; x < cell; x++) {
                size_t d = ((size_t)(oy + cell + y) * sheetW + (ox + x)) * 4;
                sheet[d] = kCaption.r;
                sheet[d + 1] = kCaption.g;
                sheet[d + 2] = kCaption.b;
                sheet[d + 3] = 255;
            }
        }
        int tw = textWidth(view.name) * 2;
        int tx = ox + (cell - tw) / 2;
        int ty = oy + cell + (caption - 14) / 2;
        drawText(sheet, sheetW, sheetH, tx, ty, view.name, 2);

        std::filesystem::path one = outDir / (stem + "_" + view.name + ".png");
        if (!mat::savePNG(one.string().c_str(), cell, cell, panel.data())) {
            std::cerr << "save failed: " << one.string() << "\n";
            return false;
        }
    }

    std::filesystem::path sheetPath = outDir / (stem + "_sheet.png");
    if (!mat::savePNG(sheetPath.string().c_str(), sheetW, sheetH, sheet.data())) {
        std::cerr << "save failed: " << sheetPath.string() << "\n";
        return false;
    }
    std::cout << "  sheet " << sheetPath.string() << "\n";
    return true;
}

void usage() {
    std::cerr << "usage: model_views [--out dir] [--size px] [--dir folder] model.model ...\n";
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path outDir = "tools/model_views/out";
    int cell = 320;
    std::vector<std::filesystem::path> files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--size" && i + 1 < argc) cell = std::max(64, std::atoi(argv[++i]));
        else if (a == "--dir" && i + 1 < argc) {
            std::filesystem::path dir = argv[++i];
            std::error_code ec;
            for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
                if (ent.is_regular_file() && ent.path().extension() == ".model")
                    files.push_back(ent.path());
            }
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) {
        usage();
        return 1;
    }
    std::sort(files.begin(), files.end());
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "cannot create " << outDir.string() << "\n";
        return 1;
    }
    int failed = 0;
    for (const auto& f : files)
        if (!renderOne(f, outDir, cell)) failed++;
    return failed ? 1 : 0;
}
