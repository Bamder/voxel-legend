#pragma once
// Shared single-block mesh builder for the model editor preview. It reuses the
// exact same geometry + atlas UV conventions as the game's chunk mesher, so the
// editor preview is pixel-consistent with the in-game rendering.
#include "../world/world.hpp"      // Vertex
#include "../world/blocks.hpp"     // BLOCKS, Block/ TexId enums
#include "../render/block_geo.hpp" // geo::kFaces
#include "../render/textures.hpp"  // tex::tileUV
#include "material.hpp"
#include "blocks/grass_tuft_mat.hpp"
#include "model_mesh.hpp"

namespace mat {

// Build one representative block's mesh, centered at the origin:
//   - GRASS_TUFT (or any block with explicit quads): a single crossed-X blade
//     using the same quad->vertex mapping as buildGrassTuftMesh, at a fixed
//     visible preview scale.
//   - otherwise: the six cube faces (top/side/bottom textures).
// UVs always come from the game atlas via tex::tileUV.
inline void buildBlockPreviewMesh(uint8_t block, const Model& model, std::vector<Vertex>& out) {
    out.clear();
    if (block == GRASS_TUFT && model.ok()) {
        float u0, v0, u1, v1;
        tex::tileUV(TEX_GRASS_TUFT, u0, v0, u1, v1);
        // Representative single blade: half-width 0.2, height 0.85 in the unit
        // cell (game blades are 1/16 wide; a preview scale keeps them editable).
        const float hw = 0.2f, hgt = 0.85f;
        for (const Quad& q : model.quads) {
            Vertex vv[4];
            for (int c = 0; c < 4; c++) {
                float lx = (q.p[c][0] - 0.5f) * 2.0f * hw;
                float ly = q.p[c][1] * hgt;
                float lz = (q.p[c][2] - 0.5f) * 2.0f * hw;
                vv[c] = { lx, ly, lz,
                          u0 + (u1 - u0) * q.uv[c][0],
                          v0 + (v1 - v0) * q.uv[c][1],
                          0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f };
            }
            out.push_back(vv[0]); out.push_back(vv[1]); out.push_back(vv[2]);
            out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[3]);
            if (q.doubleSided) {
                out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[1]);
                out.push_back(vv[0]); out.push_back(vv[3]); out.push_back(vv[2]);
            }
        }
        return;
    }
    if (model.ok() && !model.cube) {
        const BlockInfo& info = blockOf(block);
        emitModelMesh(model, out, [](float x, float y, float z) {
            return Vec3{ x - 0.5f, y - 0.5f, z - 0.5f };
        }, info.icon);
        return;
    }

    const BlockInfo& info = blockOf(block);
    for (int f = 0; f < 6; f++) {
        const geo::FaceDef& F = geo::kFaces[f];
        uint8_t tile = (f == 0) ? info.texTop : (f == 1 ? info.texBottom : info.texSide);
        float u0, v0, u1, v1;
        tex::tileUV(tile, u0, v0, u1, v1);
        Vertex vv[4];
        for (int c = 0; c < 4; c++) {
            vv[c] = { F.p[c][0] - 0.5f, F.p[c][1] - 0.5f, F.p[c][2] - 0.5f,
                      u0 + (u1 - u0) * F.t[c][0], v0 + (v1 - v0) * F.t[c][1],
                      (float)F.n[0], (float)F.n[1], (float)F.n[2], F.shade, 1.0f, 1.0f };
        }
        out.push_back(vv[0]); out.push_back(vv[1]); out.push_back(vv[2]);
        out.push_back(vv[0]); out.push_back(vv[2]); out.push_back(vv[3]);
    }
}

// Same geometry the game would emit for one block after applying .rand
// (grass tuft: many crossed blades; other blocks: the cube preview).
inline void buildRandAppliedMesh(uint8_t block, const Model& model, const RandParams& rand,
                                 int wx, int wz, std::vector<Vertex>& out) {
    out.clear();
    if (block == GRASS_TUFT && model.ok()) {
        Material m;
        m.model = model;
        m.rand = rand;
        float u0, v0, u1, v1;
        tex::tileUV(TEX_GRASS_TUFT, u0, v0, u1, v1);
        buildGrassTuftMesh(m, out, 0, 0, 0, wx, wz, u0, v0, u1, v1);
        return;
    }
    buildBlockPreviewMesh(block, model, out);
}

} // namespace mat
