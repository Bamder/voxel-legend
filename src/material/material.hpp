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

// A block model = a list of quads.
struct Model {
    std::vector<Quad> quads;
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
