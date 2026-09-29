#pragma once
#include "../core/math.hpp"
#include "vitals.hpp"

// A standing practice body. It uses the player rig and the same limb vitals;
// it does not walk, wear clothes, or keep a face.
struct TrainingTarget {
    Vec3 feet{ 0, 0, 0 };
    float yaw = 0.0f;
    vitals::Vitals vitals{};
};
