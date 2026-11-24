#pragma once
#include <algorithm>
#include <cmath>

namespace jc3vr {
// The weapon's local -Z is its barrel and +Y its top. Rebuild all three
// axes from the tracked aim frame; aligning only -Z leaves animation roll.
// Translation is deliberately preserved until each weapon's grip is measured.
inline bool AlignWeaponToController(const float forward[3], const float down[3], float matrix[16]) {
    float f[3]{}, u[3]{}, r[3]{};
    float length = 0.0f;
    for (int i = 0; i != 3; ++i) {
        if (!std::isfinite(forward[i]) || !std::isfinite(down[i])) return false;
        length += forward[i] * forward[i];
    }
    if (!std::isfinite(length) || length < 1e-8f) return false;
    length = std::sqrt(length);
    for (int i = 0; i != 3; ++i) f[i] = forward[i] / length;
    float dot = 0.0f;
    for (int i = 0; i != 3; ++i) dot += -down[i] * f[i];
    length = 0.0f;
    for (int i = 0; i != 3; ++i) { u[i] = -down[i] - dot * f[i]; length += u[i] * u[i]; }
    if (!std::isfinite(length) || length < 1e-8f) return false;
    length = std::sqrt(length);
    for (float& component : u) component /= length;
    r[0] = f[1] * u[2] - f[2] * u[1];
    r[1] = f[2] * u[0] - f[0] * u[2];
    r[2] = f[0] * u[1] - f[1] * u[0];
    for (int i = 0; i != 3; ++i) {
        matrix[i] = r[i]; matrix[4 + i] = u[i]; matrix[8 + i] = -f[i];
    }
    return true;
}

// Rotate about the tracked grip, not the model origin. Native weapon meshes
// have an authored mount offset; rotating just their axes leaves that offset
// in the animated wrist frame (particularly visible on long rifles).
inline bool AlignWeaponAboutGrip(const float grip[3], const float forward[3], const float down[3], float matrix[16]) {
    float pivot[3]{};
    for (int axis = 0; axis < 3; ++axis) {
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(grip[c]) || !std::isfinite(matrix[12+c]) || !std::isfinite(matrix[axis*4+c])) return false;
            pivot[axis] += (grip[c]-matrix[12+c])*matrix[axis*4+c];
        }
    }
    // Reject an unrelated mount snapshot rather than making a huge correction.
    if (pivot[0]*pivot[0]+pivot[1]*pivot[1]+pivot[2]*pivot[2] > 0.5f*0.5f) return false;
    float aligned[16]; std::copy(matrix,matrix+16,aligned);
    if (!AlignWeaponToController(forward,down,aligned)) return false;
    for (int c=0;c<3;++c) {
        aligned[12+c]=grip[c];
        for (int axis=0;axis<3;++axis) aligned[12+c]-=pivot[axis]*aligned[axis*4+c];
    }
    std::copy(aligned,aligned+16,matrix);
    return true;
}

inline bool RetargetPlacedWeapon(const float source[16], const float oldGrip[3], const float newGrip[3],
                                 const float forward[3], const float down[3], float out[16]) {
    float m[16];std::copy(source,source+16,m);
    float distance=0;
    for(int i=0;i<3;++i) {
        if(!std::isfinite(newGrip[i]) || !std::isfinite(oldGrip[i])) return false;
        const float delta=newGrip[i]-oldGrip[i];distance+=delta*delta;
    }
    if(distance>100.0f*100.0f || !std::isfinite(distance) || !AlignWeaponAboutGrip(oldGrip,forward,down,m)) return false;
    for(int i=0;i<3;++i) m[12+i]+=newGrip[i]-oldGrip[i];
    std::copy(m,m+16,out);return true;
}

// Match the actual matrix written by the weapon hook, rather than assuming
// every object near an animated hand bone is an uncorrected weapon.
inline bool MatchesPlacedWeapon(const float matrix[16], const float placed[16]) {
    float distanceSquared = 0.0f;
    for (int i = 0; i != 16; ++i)
        if (!std::isfinite(matrix[i]) || !std::isfinite(placed[i])) return false;
    // Bone palettes are packed 3x4, not affine world 4x4. Never claim a
    // shifted/packed window just because its axes and origin happen to match.
    for (int i : {3, 7, 11, 15}) {
        const float expected = i == 15 ? 1.0f : 0.0f;
        if (std::abs(matrix[i] - expected) > 1e-4f || std::abs(placed[i] - expected) > 1e-4f) return false;
    }
    for (int i = 12; i != 15; ++i) {
        const float delta = matrix[i] - placed[i]; distanceSquared += delta * delta;
    }
    if (distanceSquared > 0.01f * 0.01f) return false;
    for (int row = 0; row != 3; ++row)
        for (int column = 0; column != 3; ++column)
            if (std::abs(matrix[row * 4 + column] - placed[row * 4 + column]) > 0.01f) return false;
    return true;
}
}
