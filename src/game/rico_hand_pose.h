#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace jc3vr {
struct RicoHandTuning {
    std::array<float, 3> offset{}; // wrist-local right, up, back in JC3 units
    std::array<float, 3> degrees{}; // wrist-local X, Y, Z
};

struct RicoFingerInput {
    bool unarmed = false;
    float grip = 0;
    float trigger = 0;
};

inline bool RicoWeaponIsHolstered(int state) {
    return state == 3 || state == 4;
}

inline float HandAnalog(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

inline float RicoFingerFlex(int leftBone, const RicoFingerInput& input) {
    if (!input.unarmed || leftBone < 27 || leftBone > 43) return 0;
    const float grip = HandAnalog(input.grip);
    const float trigger = HandAnalog(input.trigger);
    if (leftBone < 30) return 0.35f * grip;
    if (leftBone <= 32) return 0.72f * std::max(grip, trigger);
    return 0.82f * grip;
}
}
