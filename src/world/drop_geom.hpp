#pragma once
#include "../core/config.hpp"
#include "../material/registry.hpp"
#include "loot.hpp"
#include "wear.hpp"
#include <cmath>

// Ground-drop shape. Placeable blocks stay a cube the size of one voxel.
// Tools, worn equipment, and props use their own mesh: block-space models are
// scaled by BLOCK_SCALE, garment parts stay in player space. The collision
// box is that mesh's local AABB, expanded by the drop's full orientation.
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

inline void worldHalf(const Shape& s, const Vec3& ax, const Vec3& ay, const Vec3& az,
                      float& ex, float& ey, float& ez) {
    auto absf = [](float v) { return v < 0.0f ? -v : v; };
    ex = absf(ax.x) * s.half.x + absf(ay.x) * s.half.y + absf(az.x) * s.half.z;
    ey = absf(ax.y) * s.half.x + absf(ay.y) * s.half.y + absf(az.y) * s.half.z;
    ez = absf(ax.z) * s.half.x + absf(ay.z) * s.half.y + absf(az.z) * s.half.z;
    const float m = 0.02f;
    if (ex < m) ex = m;
    if (ey < m) ey = m;
    if (ez < m) ez = m;
}

inline Vec3 rotateAbout(const Vec3& v, const Vec3& axis, float ang) {
    float len = std::sqrt(axis.lengthSq());
    if (len < 1e-8f || std::fabs(ang) < 1e-8f) return v;
    Vec3 u = axis * (1.0f / len);
    float c = std::cos(ang), s = std::sin(ang);
    return v * c + u.cross(v) * s + u * (u.dot(v) * (1.0f - c));
}

inline Vec3 nearestUpAxis(const Vec3& ax, const Vec3& ay, const Vec3& az) {
    Vec3 up = ay;
    if (std::fabs(ax.y) > std::fabs(up.y)) up = ax;
    if (std::fabs(az.y) > std::fabs(up.y)) up = az;
    if (up.y < 0.0f) up = -up;
    return up;
}

// Rotate the closest box face onto the ground without changing heading.
inline void plantFace(Vec3& ax, Vec3& ay, Vec3& az) {
    Vec3 up = nearestUpAxis(ax, ay, az);
    Vec3 axis = up.cross(Vec3{ 0.0f, 1.0f, 0.0f });
    float s = std::sqrt(axis.lengthSq());
    float c = up.y;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    if (s >= 1.0e-5f) {
        float ang = std::atan2(s, c);
        ax = rotateAbout(ax, axis, ang);
        ay = rotateAbout(ay, axis, ang);
        az = rotateAbout(az, axis, ang);
    }
    float lx = std::sqrt(ax.lengthSq());
    ax = (lx < 1.0e-6f) ? Vec3{ 1, 0, 0 } : ax * (1.0f / lx);
    ay = ay - ax * ay.dot(ax);
    float ly = std::sqrt(ay.lengthSq());
    if (ly < 1.0e-6f) {
        Vec3 t = (std::fabs(ax.y) < 0.9f) ? Vec3{ 0, 1, 0 } : Vec3{ 1, 0, 0 };
        ay = t - ax * t.dot(ax);
        ly = std::sqrt(ay.lengthSq());
        if (ly < 1.0e-6f) ly = 1.0f;
    }
    ay = ay * (1.0f / ly);
    az = ax.cross(ay);
}

// How high the center must rise to roll over an edge of half-width `halfWidth`
// while the center sits `halfHeight` above that face.
inline float faceCrestRise(float halfWidth, float halfHeight) {
    return std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight) - halfHeight;
}

// 1 for a square face. Longer than it is thick means a taller crest, so the
// same friction adds less tip. A log still rolls easily about its long axis.
inline float tipEase(float halfWidth, float halfHeight) {
    float rise = faceCrestRise(halfWidth, halfHeight);
    float square = halfHeight * 0.41421356f;
    if (rise < 1.0e-6f) return 1.0f;
    float e = square / rise;
    if (e < 0.05f) e = 0.05f;
    if (e > 1.0f) e = 1.0f;
    return e;
}

// Ground contact for a rigid box. Friction at the low corners turns a slide
// into rotation. While the box is up on a corner or an edge, its weight keeps
// turning it toward a face. A face that is nearly down, and is not being
// rolled off by a fast slide, is planted flat — a tilted pose is not a rest.
// Friction may tip a face only as easily as that face's own crest allows.
inline void applyGroundContact(Vec3& vel, Vec3& angVel,
                               const Shape& shape, Vec3& ax, Vec3& ay, Vec3& az,
                               float mu, float dt) {
    if (dt <= 1e-8f) return;
    if (mu < 0.0f) mu = 0.0f;
    if (mu > 2.0f) mu = 2.0f;
    const float hx = shape.half.x, hy = shape.half.y, hz = shape.half.z;
    const float g = cfg::GRAVITY;

    Vec3 low[8];
    int nLow = 0;
    float minY = 1.0e9f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                Vec3 c = ax * (sx * hx) + ay * (sy * hy) + az * (sz * hz);
                if (c.y < minY) minY = c.y;
                low[nLow++] = c;
            }
    float span = hx < hy ? hx : hy;
    if (hz < span) span = hz;
    float eps = span * 0.08f;
    if (eps < 0.003f) eps = 0.003f;
    Vec3 r{ 0, 0, 0 };
    int n = 0;
    for (int i = 0; i < nLow; i++) {
        if (low[i].y > minY + eps) continue;
        r += low[i];
        low[n++] = low[i];
    }
    nLow = n;
    if (nLow <= 0) return;
    r = r * (1.0f / (float)nLow);

    auto invI = [&](const Vec3& t) {
        float bx = t.dot(ax), by = t.dot(ay), bz = t.dot(az);
        float ix = hy * hy + hz * hz;
        float iy = hx * hx + hz * hz;
        float iz = hx * hx + hy * hy;
        if (ix < 1.0e-6f) ix = 1.0e-6f;
        if (iy < 1.0e-6f) iy = 1.0e-6f;
        if (iz < 1.0e-6f) iz = 1.0e-6f;
        bx *= 3.0f / ix;
        by *= 3.0f / iy;
        bz *= 3.0f / iz;
        return ax * bx + ay * by + az * bz;
    };

    // Bottom face: the outward normal closest to straight down. The center is
    // still over that face while a vertical through it hits the rectangle.
    // Rolling off the long side has to clear a higher crest than rolling off
    // the short side, or off a square face of the same thickness.
    Vec3 dirs[3] = { ax, ay, az };
    float halves[3] = { hx, hy, hz };
    int faceAxis = 0;
    float faceSign = 1.0f;
    float faceDownY = 1.0f;
    for (int i = 0; i < 3; i++) {
        for (int s = -1; s <= 1; s += 2) {
            float downY = dirs[i].y * (float)s;
            if (downY < faceDownY) {
                faceDownY = downY;
                faceAxis = i;
                faceSign = (float)s;
            }
        }
    }
    Vec3 faceN = dirs[faceAxis] * faceSign;
    float b = halves[faceAxis];
    Vec3 u = dirs[(faceAxis + 1) % 3];
    Vec3 v = dirs[(faceAxis + 2) % 3];
    float au = halves[(faceAxis + 1) % 3];
    float av = halves[(faceAxis + 2) % 3];
    bool supported = false;
    if (faceN.y < -0.2f) {
        Vec3 hit{ 0.0f, b / faceN.y, 0.0f };
        Vec3 rel = hit - faceN * b;
        supported = std::fabs(rel.dot(u)) <= au && std::fabs(rel.dot(v)) <= av;
    }
    const float faceCos = 0.9781f; // about 12 degrees off a face
    Vec3 up = nearestUpAxis(ax, ay, az);
    const bool nearFace = up.y > faceCos;
    const float lever = std::sqrt(r.x * r.x + r.z * r.z);
    // Weight pulls a box that is up on a corner or edge onto a face.
    // A face already under it has no moment.
    if (!nearFace && lever > 1.0e-5f) {
        Vec3 torque{ -g * r.z, 0.0f, g * r.x };
        float tmag = torque.length();
        Vec3 u = torque * (1.0f / tmag);
        float bx = u.dot(ax), by = u.dot(ay), bz = u.dot(az);
        float iCm = (bx * bx * (hy * hy + hz * hz)
                   + by * by * (hx * hx + hz * hz)
                   + bz * bz * (hx * hx + hy * hy)) / 3.0f;
        float along = r.dot(u);
        float d2 = r.lengthSq() - along * along;
        if (d2 < 0.0f) d2 = 0.0f;
        float iPivot = iCm + d2;
        if (iPivot < 1.0e-6f) iPivot = 1.0e-6f;
        float dw = (tmag / iPivot) * dt;
        if (dw > 1.2f) dw = 1.2f;
        angVel += u * dw;
    }

    Vec3 spinBefore = angVel;
    if (mu > 0.0f) {
        const float cap = mu * g / (float)nLow;
        for (int i = 0; i < nLow; i++) {
            Vec3 at = angVel.cross(low[i]);
            float slipX = vel.x + at.x;
            float slipZ = vel.z + at.z;
            float slip = std::sqrt(slipX * slipX + slipZ * slipZ);
            if (slip <= 1.0e-5f) continue;
            Vec3 uhat{ -slipX / slip, 0.0f, -slipZ / slip };
            Vec3 alphaPer = invI(low[i].cross(uhat));
            float k = uhat.dot(uhat + alphaPer.cross(low[i]));
            if (k < 0.25f) k = 0.25f;
            float lambda = slip / (dt * k);
            if (lambda > cap) lambda = cap;
            Vec3 a = uhat * lambda;
            vel.x += a.x * dt;
            vel.z += a.z * dt;
            angVel += invI(low[i].cross(a)) * dt;
        }
    }
    if (supported) {
        // Spin about u rolls over the edges at ±v, and the reverse.
        Vec3 added = angVel - spinBefore;
        float cu = added.dot(u);
        float cv = added.dot(v);
        float eu = tipEase(av, b);
        float ev = tipEase(au, b);
        added = added - u * cu - v * cv + u * (cu * eu) + v * (cv * ev);
        angVel = spinBefore + added;
    }

    float w2 = angVel.lengthSq();
    if (w2 > 40.0f * 40.0f) angVel *= 40.0f / std::sqrt(w2);

    Vec3 inward = supported ? (faceN * -1.0f) : nearestUpAxis(ax, ay, az);
    float rising = angVel.cross(inward).y; // positive: rotating onto that face
    float horiz = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    bool rollingOff = rising < -0.4f && horiz > 0.5f;
    bool almostDown = supported && inward.y > faceCos;
    // Land the face. A fast slide that is still rolling off is left alone so
    // friction can start the tumble; everything else that is nearly flat is
    // set down instead of being frozen on a corner.
    if (almostDown && !rollingOff && (horiz < 0.55f || rising > 0.2f)) {
        if (horiz > 0.4f || w2 > 1.0f) {
            vel.x *= 0.45f;
            vel.z *= 0.45f;
        } else {
            vel.x = 0.0f;
            vel.z = 0.0f;
        }
        angVel = { 0, 0, 0 };
        plantFace(ax, ay, az);
    }
}

// Turn the local axes by a world-space angular velocity and keep them orthonormal.
inline void spinBasis(Vec3& ax, Vec3& ay, Vec3& az, const Vec3& angVel, float dt) {
    float w2 = angVel.lengthSq();
    if (w2 < 1e-10f || dt <= 0.0f) return;
    float ang = std::sqrt(w2) * dt;
    ax = rotateAbout(ax, angVel, ang);
    ay = rotateAbout(ay, angVel, ang);
    float lx = std::sqrt(ax.lengthSq());
    ax = (lx < 1e-6f) ? Vec3{ 1, 0, 0 } : ax * (1.0f / lx);
    ay = ay - ax * ay.dot(ax);
    float ly = std::sqrt(ay.lengthSq());
    if (ly < 1e-6f) {
        Vec3 t = (std::fabs(ax.y) < 0.9f) ? Vec3{ 0, 1, 0 } : Vec3{ 1, 0, 0 };
        ay = t - ax * t.dot(ax);
        ly = std::sqrt(ay.lengthSq());
        if (ly < 1e-6f) ly = 1.0f;
    }
    ay = ay * (1.0f / ly);
    az = ax.cross(ay);
}

} // namespace dropgeom
