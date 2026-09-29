#pragma once
#include <vector>
#include "../material.hpp"

struct Vertex;

namespace mat {

// Build the grass-tuft mesh for one block (block-local bx,by,bz; world wx,wz).
// The atlas UV range of the grass-tuft tile is passed so the material image is
// sampled from the shared atlas. The material's model + rand params drive the
// tower placement, height, shade, and density.
void buildGrassTuftMesh(const Material& m, std::vector<Vertex>& out,
                        int bx, int by, int bz, int wx, int wz,
                        float u0, float v0, float u1, float v1,
                        float heightScale = 1.0f);

} // namespace mat
