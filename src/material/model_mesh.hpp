#pragma once
#include "registry.hpp"
#include "../world/world.hpp"
#include "../world/blocks.hpp"
#include "../render/textures.hpp"
#include "../render/block_geo.hpp"
#include "../core/math.hpp"
#include <cmath>
#include <vector>

namespace mat {

inline uint8_t quadTile(const Quad& q, uint8_t fallback) {
    if (q.tex.empty()) return fallback;
    int i = tileIndex(q.tex.c_str());
    return i >= 0 ? (uint8_t)i : fallback;
}

inline float quadTexScale(const Quad& q) {
    return (q.texScale > 1e-4f) ? q.texScale : 1.0f;
}
inline bool quadTiles(const Quad& q) {
    return q.uvMode == 1 || q.crop;
}

inline bool modelHasSolidTex(const Model& model) {
    for (const Solid& s : model.solids) if (solidTextured(s)) return true;
    return false;
}

inline Vec3 solidEuler(const Vec3& v, const float e[3]) {
    float m = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
    if (m < 1e-12f) return v;
    float cx = std::cos(e[0]), sx = std::sin(e[0]);
    float cy = std::cos(e[1]), sy = std::sin(e[1]);
    float cz = std::cos(e[2]), sz = std::sin(e[2]);
    Vec3 p{ v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx };
    p = { p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy };
    return { p.x * cz - p.y * sz, p.x * sz + p.y * cz, p.z };
}

inline Vec3 solidUnEuler(const Vec3& v, const float e[3]) {
    float m = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
    if (m < 1e-12f) return v;
    float cx = std::cos(e[0]), sx = std::sin(e[0]);
    float cy = std::cos(e[1]), sy = std::sin(e[1]);
    float cz = std::cos(e[2]), sz = std::sin(e[2]);
    Vec3 p{ v.x * cz + v.y * sz, -v.x * sz + v.y * cz, v.z };
    p = { p.x * cy - p.z * sy, p.y, p.x * sy + p.z * cy };
    return { p.x, p.y * cx + p.z * sx, -p.y * sx + p.z * cx };
}

inline void bakeSolidUV(Quad& q) {
    float s = quadTexScale(q);
    auto put = [&](int c, float u, float v) {
        q.uv[c][0] = u;
        q.uv[c][1] = v;
    };
    if (q.crop) {
        int f = q.face;
        for (int c = 0; c < 4; c++) {
            float x = q.p[c][0] / s, y = q.p[c][1] / s, z = q.p[c][2] / s;
            if (f == 0 || f == 1) put(c, x, z);
            else if (f == 2 || f == 3) put(c, z, 1.0f / s - y);
            else put(c, x, 1.0f / s - y);
        }
    } else {
        q.uvMode = 1;
        Vec3 p0{ q.p[0][0], q.p[0][1], q.p[0][2] };
        Vec3 p1{ q.p[1][0], q.p[1][1], q.p[1][2] };
        Vec3 p3{ q.p[3][0], q.p[3][1], q.p[3][2] };
        Vec3 n = (p1 - p0).cross(p3 - p0);
        float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        for (int c = 0; c < 4; c++) {
            float x = q.p[c][0] / s, y = q.p[c][1] / s, z = q.p[c][2] / s;
            if (ay >= ax && ay >= az) put(c, x, z);
            else if (ax >= az) put(c, z, 1.0f / s - y);
            else put(c, x, 1.0f / s - y);
        }
    }
}

// Atlas faces for one textured part. Color-only parts are skipped.
inline void appendSolidQuads(const Solid& s, std::vector<Quad>& out) {
    if (!solidTextured(s) || solidHasBox(s)) return;
    auto finish = [&](Quad q) {
        bakeSolidUV(q);
        if (s.uvFlip & 1) for (int c = 0; c < 4; c++) q.uv[c][0] = -q.uv[c][0];
        if (s.uvFlip & 2) for (int c = 0; c < 4; c++) q.uv[c][1] = -q.uv[c][1];
        out.push_back(std::move(q));
    };
    auto corner = [&](Quad& q, int c, float lx, float ly, float lz) {
        Vec3 w = solidEuler({ lx, ly, lz }, s.rot);
        q.p[c][0] = s.c[0] + w.x;
        q.p[c][1] = s.c[1] + w.y;
        q.p[c][2] = s.c[2] + w.z;
    };
    if (s.kind == 1) {
        Quad q{};
        q.tex = s.tex;
        q.doubleSided = true;
        q.texScale = (s.texScale > 1e-4f) ? s.texScale : 1.0f;
        q.crop = s.crop;
        q.uvMode = s.crop ? 2 : 1;
        int thin = 0;
        if (s.h[1] <= s.h[thin]) thin = 1;
        if (s.h[2] <= s.h[thin]) thin = 2;
        q.face = (s.face >= 0 && s.face < 6) ? s.face : (thin == 0 ? 2 : thin == 1 ? 0 : 4);
        int ua = (thin + 1) % 3, va = (thin + 2) % 3;
        const float su[4] = { -1, 1, 1, -1 };
        const float sv[4] = { -1, -1, 1, 1 };
        for (int c = 0; c < 4; c++) {
            float o[3] = {};
            o[ua] = su[c] * s.h[ua];
            o[va] = sv[c] * s.h[va];
            corner(q, c, o[0], o[1], o[2]);
        }
        finish(std::move(q));
        return;
    }
    for (int f = 0; f < 6; f++) {
        Quad q{};
        q.tex = s.tex;
        q.face = f;
        q.texScale = (s.texScale > 1e-4f) ? s.texScale : 1.0f;
        q.crop = s.crop;
        q.uvMode = s.crop ? 2 : 1;
        const geo::FaceDef& F = geo::kFaces[f];
        for (int c = 0; c < 4; c++) {
            corner(q, c,
                   (F.p[c][0] - 0.5f) * 2.0f * s.h[0],
                   (F.p[c][1] - 0.5f) * 2.0f * s.h[1],
                   (F.p[c][2] - 0.5f) * 2.0f * s.h[2]);
        }
        finish(std::move(q));
    }
}

// Emit a .model's quads into a Vertex mesh. `xform` maps model-space (x,y,z in [0,1]) to world.
// Tiled faces (uvMode fill / crop cubes) are split so atlas UVs stay inside one tile.
template <typename Xform>
void emitModelMesh(const Model& model, std::vector<Vertex>& out, Xform&& xform, uint8_t fallbackTile) {
    struct Cell { Vec3 p[4]; float uv[4][2]; int depth; };
    auto lerpP = [](const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; };
    auto lerpUV = [](const float a[2], const float b[2], float t, float o[2]) {
        o[0] = a[0] + (b[0] - a[0]) * t;
        o[1] = a[1] + (b[1] - a[1]) * t;
    };
    std::vector<Quad> drawn = model.quads;
    for (const Solid& s : model.solids) appendSolidQuads(s, drawn);
    for (const Quad& q : drawn) {
        uint8_t tile = quadTile(q, fallbackTile);
        float tu0, tv0, tu1, tv1;
        tex::tileUV(tile, tu0, tv0, tu1, tv1);
        Vec3 wp[4];
        for (int c = 0; c < 4; c++)
            wp[c] = xform(q.p[c][0], q.p[c][1], q.p[c][2]);
        Vec3 e0 = wp[1] - wp[0], e1 = wp[2] - wp[0];
        Vec3 n = e0.cross(e1);
        float nl = n.length();
        if (nl > 1e-8f) n = n * (1.0f / nl);
        else n = { 0, 1, 0 };
        float shade = 0.55f + 0.45f * std::fabs(n.y);
        auto emitPiece = [&](const Vec3 p[4], const float uv[4][2]) {
            Vertex vv[4];
            float umn = uv[0][0], vmn = uv[0][1];
            for (int c = 1; c < 4; c++) {
                if (uv[c][0] < umn) umn = uv[c][0];
                if (uv[c][1] < vmn) vmn = uv[c][1];
            }
            float uo = std::floor(umn + 1e-5f);
            float vo = std::floor(vmn + 1e-5f);
            for (int c = 0; c < 4; c++) {
                float lu = uv[c][0] - uo;
                float lv = uv[c][1] - vo;
                if (lu < 0.0f) lu = 0.0f;
                if (lu > 1.0f) lu = 1.0f;
                if (lv < 0.0f) lv = 0.0f;
                if (lv > 1.0f) lv = 1.0f;
                vv[c] = { p[c].x, p[c].y, p[c].z,
                          tu0 + (tu1 - tu0) * lu, tv0 + (tv1 - tv0) * lv,
                          n.x, n.y, n.z, shade, 1.0f, 1.0f };
            }
            out.push_back(vv[0]); out.push_back(vv[1]); out.push_back(vv[2]);
            out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[3]);
            if (q.doubleSided) {
                out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[1]);
                out.push_back(vv[0]); out.push_back(vv[3]); out.push_back(vv[2]);
            }
        };
        Cell root{};
        for (int c = 0; c < 4; c++) {
            root.p[c] = wp[c];
            root.uv[c][0] = q.uv[c][0];
            root.uv[c][1] = q.uv[c][1];
        }
        root.depth = 0;
        if (!quadTiles(q)) {
            emitPiece(root.p, root.uv);
            continue;
        }
        std::vector<Cell> stack;
        stack.push_back(root);
        while (!stack.empty()) {
            Cell cell = stack.back();
            stack.pop_back();
            float umn = cell.uv[0][0], umx = cell.uv[0][0], vmn = cell.uv[0][1], vmx = cell.uv[0][1];
            for (int c = 1; c < 4; c++) {
                if (cell.uv[c][0] < umn) umn = cell.uv[c][0];
                if (cell.uv[c][0] > umx) umx = cell.uv[c][0];
                if (cell.uv[c][1] < vmn) vmn = cell.uv[c][1];
                if (cell.uv[c][1] > vmx) vmx = cell.uv[c][1];
            }
            bool uSplit = std::floor(umx - 1e-4f) > std::floor(umn + 1e-4f);
            bool vSplit = std::floor(vmx - 1e-4f) > std::floor(vmn + 1e-4f);
            if ((!uSplit && !vSplit) || cell.depth >= 8) {
                emitPiece(cell.p, cell.uv);
                continue;
            }
            Cell a = cell, b = cell;
            a.depth = b.depth = cell.depth + 1;
            if (uSplit && (!vSplit || (umx - umn) >= (vmx - vmn))) {
                a.p[1] = lerpP(cell.p[0], cell.p[1], 0.5f);
                a.p[2] = lerpP(cell.p[3], cell.p[2], 0.5f);
                lerpUV(cell.uv[0], cell.uv[1], 0.5f, a.uv[1]);
                lerpUV(cell.uv[3], cell.uv[2], 0.5f, a.uv[2]);
                b.p[0] = a.p[1];
                b.p[3] = a.p[2];
                b.uv[0][0] = a.uv[1][0]; b.uv[0][1] = a.uv[1][1];
                b.uv[3][0] = a.uv[2][0]; b.uv[3][1] = a.uv[2][1];
            } else {
                a.p[3] = lerpP(cell.p[0], cell.p[3], 0.5f);
                a.p[2] = lerpP(cell.p[1], cell.p[2], 0.5f);
                lerpUV(cell.uv[0], cell.uv[3], 0.5f, a.uv[3]);
                lerpUV(cell.uv[1], cell.uv[2], 0.5f, a.uv[2]);
                b.p[0] = a.p[3];
                b.p[1] = a.p[2];
                b.uv[0][0] = a.uv[3][0]; b.uv[0][1] = a.uv[3][1];
                b.uv[1][0] = a.uv[2][0]; b.uv[1][1] = a.uv[2][1];
            }
            stack.push_back(b);
            stack.push_back(a);
        }
    }
}

// Colored cuboids into a pos3+rgba mesh (7 floats). `faceShade` tints each face
// for an unlit viewer; a lit shader should pass false and shade from the normal.
template <typename Xform>
void emitSolidMesh(const std::vector<Solid>& solids, std::vector<float>& out, Xform&& xform,
                   bool faceShade, float alpha = 1.0f, bool skipBoxed = false) {
    const float sh[6] = { 1.0f, 0.48f, 0.82f, 0.62f, 0.90f, 0.70f }; // +Y -Y +X -X +Z -Z
    for (const Solid& s : solids) {
        if (solidTextured(s) || (skipBoxed && solidHasBox(s))) continue;
        const float lx[8] = { -1, 1, 1, -1, -1, 1, 1, -1 };
        const float ly[8] = { -1, -1, -1, -1, 1, 1, 1, 1 };
        const float lz[8] = { -1, -1, 1, 1, -1, -1, 1, 1 };
        Vec3 p[8];
        for (int i = 0; i < 8; i++) {
            Vec3 o = solidEuler({ lx[i] * s.h[0], ly[i] * s.h[1], lz[i] * s.h[2] }, s.rot);
            p[i] = xform(s.c[0] + o.x, s.c[1] + o.y, s.c[2] + o.z);
        }
        const int faces[6][4] = {
            { 4, 5, 6, 7 }, { 0, 3, 2, 1 },
            { 1, 2, 6, 5 }, { 0, 4, 7, 3 },
            { 3, 7, 6, 2 }, { 0, 1, 5, 4 }
        };
        for (int i = 0; i < 6; i++) {
            float m = faceShade ? sh[i] : 1.0f;
            float cr = s.rgb[0] * m, cg = s.rgb[1] * m, cb = s.rgb[2] * m;
            int a = faces[i][0], b = faces[i][1], c = faces[i][2], d = faces[i][3];
            const int tri[2][3] = { { a, b, c }, { a, c, d } };
            for (int t = 0; t < 2; t++) {
                for (int k = 0; k < 3; k++) {
                    const Vec3& q = p[tri[t][k]];
                    out.push_back(q.x); out.push_back(q.y); out.push_back(q.z);
                    out.push_back(cr); out.push_back(cg); out.push_back(cb); out.push_back(alpha);
                }
            }
        }
    }
}

// True when the atlas mesh should be the six block faces. A model that is only
// colored cuboids (no quads, not a symbolic cube) draws those cuboids alone.
inline bool modelUsesBlockFaces(const Model& model) {
    return model.quads.empty() && (model.cube || model.solids.empty());
}

// GUI / inventory display mesh in model space [0,1]. Custom-quad items keep
// their authored geometry; cube items (and missing models) use the six block
// faces so blocks get the same 3D icon treatment as tools.
inline void buildItemDisplayMesh(uint8_t block, const Model& model, std::vector<Vertex>& out) {
    out.clear();
    if (!validBlock(block)) return;
    bool custom = !model.quads.empty() || modelHasSolidTex(model);
    if (custom) {
        emitModelMesh(model, out, [](float x, float y, float z) {
            return Vec3{ x, y, z };
        }, blockOf(block).icon);
        if (!model.cube) return;
    }
    if (!model.cube && !modelUsesBlockFaces(model)) return;
    const BlockInfo& info = blockOf(block);
    float alpha = (block == GLASS) ? 0.45f : 1.0f;
    for (int f = 0; f < 6; f++) {
        const geo::FaceDef& F = geo::kFaces[f];
        uint8_t tile = (f == 0) ? info.texTop : (f == 1 ? info.texBottom : info.texSide);
        float u0, v0, u1, v1;
        tex::tileUV(tile, u0, v0, u1, v1);
        Vertex vv[4];
        for (int c = 0; c < 4; c++) {
            vv[c] = { F.p[c][0], F.p[c][1], F.p[c][2],
                      u0 + (u1 - u0) * F.t[c][0], v0 + (v1 - v0) * F.t[c][1],
                      (float)F.n[0], (float)F.n[1], (float)F.n[2], F.shade, 1.0f, alpha };
        }
        out.push_back(vv[0]); out.push_back(vv[1]); out.push_back(vv[2]);
        out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[3]);
    }
}

} // namespace mat
