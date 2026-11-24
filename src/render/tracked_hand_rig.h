#pragma once
#include <cmath>

namespace jc3vr {
constexpr float kTrackedWristToPalm = 0.08f;
// Authoring frame has fingers along -Y; authoritative aim frame along -Z.
// Translation is ONLY the current OpenXR grip position. No player velocity,
// native world transform, smoothing or eye-dependent offset is involved.
inline bool BuildTrackedHandFrame(const float grip[3], const float aimQuaternion[4], float position[3], float orientation[4]) {
    float lengthSquared = 0;
    for (int i = 0; i != 3; ++i) if (!std::isfinite(grip[i])) return false;
    for (int i = 0; i != 4; ++i) {
        if (!std::isfinite(aimQuaternion[i])) return false;
        lengthSquared += aimQuaternion[i]*aimQuaternion[i];
    }
    if (lengthSquared < 0.25f || lengthSquared > 4) return false;
    const float scale = 0.70710678118f/std::sqrt(lengthSquared);
    const auto* q = aimQuaternion;
    orientation[0] = (q[3]+q[0])*scale;
    orientation[1] = (q[1]+q[2])*scale;
    orientation[2] = (q[2]-q[1])*scale;
    orientation[3] = (q[3]-q[0])*scale;
    for (int i = 0; i != 3; ++i) position[i] = grip[i];
    return true;
}
}
