#pragma once
#include "../core/math.hpp"
#include <cmath>

// Torch mesh and flame VFX share this so wall mounts stay aligned.
namespace torch_appear {

inline Vec3 localPoint(float mx, float my, float mz, int attach) {
    const float baseY = 0.02f;
    float px = mx - 0.50f;
    float py = my - baseY;
    float pz = mz - 0.50f;
    if (attach <= 1)
        return { 0.50f + px, baseY + py, 0.50f + pz };

    const float tilt = 0.70f;
    float c = std::cos(tilt), s = std::sin(tilt);
    float rx = px, ry = py, rz = pz;
    float ox = 0.50f, oy = 0.42f, oz = 0.50f;
    switch (attach) {
    case 3:
        rx = px * c + py * s;
        ry = -px * s + py * c;
        ox = 0.10f;
        break;
    case 2:
        rx = px * c - py * s;
        ry = px * s + py * c;
        ox = 0.90f;
        break;
    case 5:
        rz = pz * c + py * s;
        ry = -pz * s + py * c;
        oz = 0.10f;
        break;
    case 4:
        rz = pz * c - py * s;
        ry = pz * s + py * c;
        oz = 0.90f;
        break;
    default:
        break;
    }
    return { ox + rx, oy + ry, oz + rz };
}

// Hot tip above the stick, in cell-local [0,1] space.
inline Vec3 flameTip(int attach) {
    return localPoint(0.50f, 0.78f, 0.50f, attach);
}

} // namespace torch_appear
