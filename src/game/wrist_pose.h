#pragma once
#include "game/weapon_pose.h"

namespace jc3vr {
// Attach a rest-pose forearm socket to the tracked wrist. Unlike an animated
// mount, this offset does not change when Rico runs, fires or grapples.
inline bool BuildWristAttachment(const float restWrist[3], const float restForward[3],
                                 const float restDown[3], const float restBone[16],
                                 const float grip[3], const float forward[3], const float down[3], float out[16]) {
    float source[16]{}, target[16]{};
    if (!AlignWeaponToController(restForward, restDown, source) ||
        !AlignWeaponToController(forward, down, target)) return false;
    for (int i = 0; i != 16; ++i) if (!std::isfinite(restBone[i])) return false;
    float rotation[3][3]{};
    for (int r = 0; r != 3; ++r)
        for (int c = 0; c != 3; ++c)
            for (int axis = 0; axis != 3; ++axis)
                rotation[r][c] += target[axis * 4 + r] * source[axis * 4 + c];
    float displacement[3]{};
    for (int i = 0; i != 3; ++i) {
        if (!std::isfinite(grip[i]) || !std::isfinite(restWrist[i])) return false;
        displacement[i] = restBone[12 + i] - restWrist[i];
    }
    if (displacement[0]*displacement[0] + displacement[1]*displacement[1] + displacement[2]*displacement[2] > 0.8f*0.8f) return false;
    for (int axis = 0; axis != 3; ++axis) {
        for (int component = 0; component != 3; ++component) {
            out[axis*4+component] = 0.0f;
            for (int k = 0; k != 3; ++k) out[axis*4+component] += rotation[component][k] * restBone[axis*4+k];
        }
        out[axis*4+3] = 0.0f;
    }
    for (int i = 0; i != 3; ++i) {
        out[12+i] = grip[i] + target[8+i] * 0.08f;  // -finger * wrist-to-palm
        for (int k = 0; k != 3; ++k) out[12+i] += rotation[i][k] * displacement[k];
    }
    out[15] = 1.0f;
    return true;
}
}
