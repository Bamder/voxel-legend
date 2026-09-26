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
    for (const Quad& q : model.quads) {
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

// GUI / inventory display mesh in model space [0,1]. Custom-quad items keep
// their authored geometry; cube items (and missing models) use the six block
// faces so blocks get the same 3D icon treatment as tools.
inline void buildItemDisplayMesh(uint8_t block, const Model& model, std::vector<Vertex>& out) {
    out.clear();
    if (!validBlock(block)) return;
    if (model.ok()) {
        emitModelMesh(model, out, [](float x, float y, float z) {
            return Vec3{ x, y, z };
        }, blockOf(block).icon);
        return;
    }
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
