#pragma once
#include "../core/config.hpp"
#include "../material/registry.hpp"
#include "loot.hpp"
#include "wear.hpp"
#include <cmath>

// Ground-drop shape. Placeable blocks stay a cube the size of one voxel.
// Tools, worn equipment, and props use their own mesh: block-space models are
// scaled by BLOCK_SCALE, garment parts stay in player space. The collision
// box is that mesh's local AABB (yaw expands it on XZ).
namespace dropgeom {

enum class Form : uint8_t { Cube = 0, Model = 1, Garment = 2 };

struct Shape {
    Form form = Form::Cube;
    Vec3 center{ 0.5f, 0.5f, 0.5f }; // point in mesh space that sits on the drop origin
    Vec3 half{ 0.25f, 0.25f, 0.25f }; // local half extents, world units
};

inline void growBox(Vec3& mn, Vec3& mx, const Vec3& p) {
    if (p.x < mn.x) mn.x = p.x;
    if (p.y < mn.y) mn.y = p.y;
    if (p.z < mn.z) mn.z = p.z;
    if (p.x > mx.x) mx.x = p.x;
    if (p.y > mx.y) mx.y = p.y;
    if (p.z > mx.z) mx.z = p.z;
}

inline Vec3 clampHalf(Vec3 h) {
    const float m = 0.02f;
    if (h.x < m) h.x = m;
    if (h.y < m) h.y = m;
    if (h.z < m) h.z = m;
    return h;
}

inline Shape cubeShape() {
    Shape s;
    float h = cfg::DROP_SIZE * 0.5f;
    s.form = Form::Cube;
    s.center = { 0.5f, 0.5f, 0.5f };
    s.half = { h, h, h };
    return s;
}

inline Shape shapeOf(uint8_t item) {
    if (item == AIR || !validBlock(item) || loot::itemDef(item).kind == loot::Kind::Block)
        return cubeShape();

    const std::vector<pm::Part>& garment = wear::garment(item);
    if (!garment.empty()) {
        Vec3 mn{ 1e9f, 1e9f, 1e9f };
        Vec3 mx{ -1e9f, -1e9f, -1e9f };
        for (const pm::Part& p : garment) {
            Vec3 c[8];
            pm::partWorldCorners(p, c);
            for (int i = 0; i < 8; i++) growBox(mn, mx, c[i]);
        }
        if (mx.x > mn.x) {
            Shape s;
            s.form = Form::Garment;
            s.center = (mn + mx) * 0.5f;
            s.half = clampHalf((mx - mn) * 0.5f);
            return s;
        }
    }

    const mat::Model& model = mat::itemModel(item);
    if (!model.cube && (!model.quads.empty() || !model.solids.empty())) {
        Vec3 mn{ 1e9f, 1e9f, 1e9f };
        Vec3 mx{ -1e9f, -1e9f, -1e9f };
        for (const mat::Quad& q : model.quads) {
            for (int c = 0; c < 4; c++)
                growBox(mn, mx, { q.p[c][0], q.p[c][1], q.p[c][2] });
        }
        for (const mat::Solid& solid : model.solids) {
            growBox(mn, mx, { solid.c[0] - solid.h[0], solid.c[1] - solid.h[1], solid.c[2] - solid.h[2] });
            growBox(mn, mx, { solid.c[0] + solid.h[0], solid.c[1] + solid.h[1], solid.c[2] + solid.h[2] });
        }
        if (mx.x > mn.x) {
            Shape s;
            s.form = Form::Model;
            s.center = (mn + mx) * 0.5f;
            s.half = clampHalf((mx - mn) * (0.5f * cfg::BLOCK_SCALE));
            return s;
        }
    }
    return cubeShape();
}

inline const Shape& cached(uint8_t item) {
    static Shape cache[256];
    static uint8_t ready[256]{};
    if (ready[item]) return cache[item];
    Shape s = shapeOf(item);
    const mat::Model& m = mat::itemModel(item);
    bool settled = s.form != Form::Cube
        || loot::itemDef(item).kind == loot::Kind::Block
        || m.cube || !m.quads.empty() || !m.solids.empty();
    cache[item] = s;
    if (settled) ready[item] = 1;
    return cache[item];
}

inline void worldHalf(uint8_t item, float yaw, float& ex, float& ey, float& ez) {
    const Shape& s = cached(item);
    float c = std::fabs(std::cos(yaw));
    float sn = std::fabs(std::sin(yaw));
    ex = c * s.half.x + sn * s.half.z;
    ey = s.half.y;
    ez = sn * s.half.x + c * s.half.z;
}

} // namespace dropgeom
