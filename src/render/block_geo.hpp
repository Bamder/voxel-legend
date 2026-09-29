#pragma once
// Shared cube-face geometry used by both the game's chunk mesher and the
// model editor's preview, so the two stay in sync by construction.
namespace geo {

struct FaceDef {
    int n[3];
    float shade;
    float p[4][3];   // corner offsets from block min, CCW seen from outside
    float t[4][2];   // uv grid coords (0 or 1); final uv = u0+(u1-u0)*tu, v0+(v1-v0)*tv
};

// Face order: 0:+Y(top) 1:-Y(bottom) 2:+X(east) 3:-X(west) 4:+Z(south) 5:-Z(north)
inline const FaceDef kFaces[6] = {
    { { 0, 1, 0}, 1.0f,
      { {0,1,0},{0,1,1},{1,1,1},{1,1,0} },
      { {0,0},{0,1},{1,1},{1,0} } },
    { { 0,-1, 0}, 0.5f,
      { {0,0,0},{1,0,0},{1,0,1},{0,0,1} },
      { {0,0},{1,0},{1,1},{0,1} } },
    { { 1, 0, 0}, 0.6f,
      { {1,0,0},{1,1,0},{1,1,1},{1,0,1} },
      { {0,1},{0,0},{1,0},{1,1} } },
    { {-1, 0, 0}, 0.6f,
      { {0,0,0},{0,0,1},{0,1,1},{0,1,0} },
      { {0,1},{1,1},{1,0},{0,0} } },
    { { 0, 0, 1}, 0.8f,
      { {0,0,1},{1,0,1},{1,1,1},{0,1,1} },
      { {0,1},{1,1},{1,0},{0,0} } },
    { { 0, 0,-1}, 0.8f,
      { {1,0,0},{0,0,0},{0,1,0},{1,1,0} },
      { {1,1},{0,1},{0,0},{1,0} } },
};

} // namespace geo
