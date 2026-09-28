#pragma once
#include "image.hpp"
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mat {

// One quad of a material model. Corners are in normalized block space [0,1],
// UVs in [0,1]. `doubleSided` renders both windings.
struct Quad {
    float p[4][3] = {}; // xyz of the 4 corners, block space [0,1]
    float uv[4][2] = {}; // uv of the 4 corners
    bool doubleSided = false;
    bool crop = false;       // small-cube faces: UV from world position (crop), not stretched
    bool solid = false;      // small-block: native group cannot be unbound
    int group = -1;          // >=0 links the 6 faces of one block / small-block
    int bind = -1;           // >=0 user combination (Bind tool); Unbind clears this first
    int face = -1;           // 0..5 cube-face index (for crop UV / block faces)
    int uvMode = 0;          // 0 stretch, 1 fill-repeat, 2 fill-crop (planar, no stretch)
    bool uvLock = false;     // after crop/fill bake: UV rides with the face
    float texScale = 1.0f;   // world units per one texture tile (tiling period)
    std::string tex;         // material image stem (tile name or extras/*.png)
    std::vector<std::pair<std::string, std::string>> extra; // unknown flags/keys, round-tripped
};

// Cuboid in the same block space as quads. Empty `tex` is a solid color.
// A material stem draws the block atlas (Face / Block / Tex). `kind` 1 is a
// thin double-sided card. `rot` is Euler XYZ radians around the center.
inline constexpr float kSolidGrid = 0.04f;

struct Solid {
    float c[3] = { 0.5f, 0.5f, 0.5f }; // center
    float h[3] = { 0.02f, 0.02f, 0.02f }; // half extents
    float rgb[3] = { 0.55f, 0.42f, 0.28f };
    float rot[3] = {};
    int bind = -1;
    int kind = 0;            // 0 box, 1 card
    int face = 5;            // card face index
    int uvFlip = 0;          // bit0 mirror U, bit1 mirror V
    float texScale = 1.0f;
    bool crop = false;
    std::string tex;
    int boxX = -1, boxY = -1, boxW = 0, boxH = 0, boxD = 0; // unwrap island on this model's sheet
};

inline bool solidTextured(const Solid& s) { return !s.tex.empty(); }
inline bool solidHasBox(const Solid& s) {
    return s.boxX >= 0 && s.boxW > 0 && s.boxH > 0 && s.boxD > 0;
}
inline bool solidRotated(const Solid& s) {
    return s.rot[0] * s.rot[0] + s.rot[1] * s.rot[1] + s.rot[2] * s.rot[2] > 1e-8f;
}

// Remove the overlap of box [cmn, cmx] from one cuboid. Remainder is up to six slabs.
bool subtractSolid(std::vector<Solid>& parts, int index, const float cmn[3], const float cmx[3]);

// A block model = quads (atlas faces) plus optional colored cuboids.
struct Model {
    std::vector<Quad> quads;
    std::vector<Solid> solids;
    bool cube = false; // symbolic cube (six faces from block tiles); not expanded here
    std::vector<std::string> extraLines; // unknown record types, round-tripped
    bool ok() const { return !quads.empty(); }
};

// Key/value float parameters used by a block's material handler.
struct RandParams {
    std::unordered_map<std::string, float> f;
    std::vector<std::string> extraLines; // unknown / non-float records, round-tripped
    float get(const char* k, float def) const {
        auto it = f.find(k);
        return it == f.end() ? def : it->second;
    }
};

// The three files that together describe one material unit:
//   image  -> the visible texture
//   model  -> how the texture is mapped onto geometry
//   rand   -> randomized variation parameters
struct Material {
    Image image;
    Model model;
    RandParams rand;
    bool loaded() const { return image.ok() && model.ok(); }
};

// Load a model text file (one `quad` per line).
Model loadModel(const char* path);

// Save a model back to a text file (same format as loadModel).
bool saveModel(const char* path, const Model& m);

// Load a randomization text file (one `key value` per line).
RandParams loadRand(const char* path);

// Save randomization parameters (sorted keys, same format as loadRand).
bool saveRand(const char* path, const RandParams& r);

} // namespace mat
