#include "game/vr_body.h"
#include <algorithm>
#include <cmath>

namespace jc3vr {
namespace {
constexpr float kPi = 3.14159265358979f;
BodyVec3 operator+(BodyVec3 a, BodyVec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
BodyVec3 operator-(BodyVec3 a, BodyVec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
BodyVec3 operator*(BodyVec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(BodyVec3 a, BodyVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(BodyVec3 a) { return std::sqrt(Dot(a, a)); }
BodyVec3 Normalize(BodyVec3 a, BodyVec3 fallback) {
    const float length = Length(a);
    return length > 1e-6f ? a * (1.0f / length) : fallback;
}
}

float WrapAngle(float radians) {
    while (radians > kPi) radians -= 2.0f * kPi;
    while (radians <= -kPi) radians += 2.0f * kPi;
    return radians;
}

TwoBoneIkResult SolveTwoBoneIk(const BodyVec3& shoulder, const BodyVec3& elbow, const BodyVec3& wrist,
                               const BodyVec3& target, const BodyVec3& pole) {
    TwoBoneIkResult result;
    const float upper = Length(elbow - shoulder);
    const float lower = Length(wrist - elbow);
    if (upper < 1e-4f || lower < 1e-4f) {
        result.elbow = elbow;
        result.wrist = wrist;
        return result;
    }
    const BodyVec3 toTarget = target - shoulder;
    const float distance = Length(toTarget);
    // Never fully straight (avoids the snap/lock at full extension) and never
    // folded through the shoulder.
    const float maxReach = (upper + lower) * 0.995f;
    const float minReach = std::abs(upper - lower) + 1e-3f;
    const float reach = std::clamp(distance, minReach, maxReach);
    result.reached = distance <= maxReach && distance >= minReach;
    const BodyVec3 axis = Normalize(toTarget, Normalize(wrist - shoulder, {0.0f, -1.0f, 0.0f}));
    // Law of cosines: distance from the shoulder, along the axis, to the
    // elbow's projection, and the elbow's offset from the axis.
    const float along = (upper * upper - lower * lower + reach * reach) / (2.0f * reach);
    const float height = std::sqrt(std::max(0.0f, upper * upper - along * along));
    // Bend direction: the pole with its component along the axis removed; if
    // the pole is (nearly) parallel to the reach, keep the animated elbow side.
    BodyVec3 bend = pole - axis * Dot(pole, axis);
    if (Length(bend) < 1e-4f) {
        const BodyVec3 animated = elbow - shoulder;
        bend = animated - axis * Dot(animated, axis);
    }
    bend = Normalize(bend, {0.0f, -1.0f, 0.0f});
    result.elbow = shoulder + axis * along + bend * height;
    result.wrist = shoulder + axis * reach;
    return result;
}

BodyVec3 ElbowPole(bool leftArm, const BodyVec3& bodyRight, const BodyVec3& bodyForward) {
    const float side = leftArm ? -1.0f : 1.0f;
    // Mostly down, a little outwards and back: elbows hang naturally.
    return Normalize(BodyVec3{0.0f, -1.0f, 0.0f} + bodyRight * (0.35f * side) - bodyForward * 0.25f, {0.0f, -1.0f, 0.0f});
}

BodyVec3 NeckFromEyes(const BodyVec3& eyeCentre, const BodyVec3& headForward, const BodyVec3& headUp) {
    // Typical adult: the head's rotation pivot (upper neck) sits ~8 cm
    // behind and ~11 cm below the eye midpoint.
    return eyeCentre - headForward * 0.08f - headUp * 0.11f;
}

float UpdateBodyYaw(BodyYawState& state, float headYaw, float moveAmount, float dtSeconds, const BodyYawParams& params) {
    if (!state.initialized) {
        state.bodyYaw = headYaw;
        state.initialized = true;
        return state.bodyYaw;
    }
    const float degree = kPi / 180.0f;
    const float difference = WrapAngle(headYaw - state.bodyYaw);
    const bool moving = moveAmount > params.moveThreshold;
    float rate = 0.0f;
    if (moving) {
        rate = params.moveFollowRateDegreesPerSecond * degree;
        state.turning = std::abs(difference) > params.settleDegrees * degree;
    } else {
        if (std::abs(difference) > params.comfortDegrees * degree) state.turning = true;
        if (state.turning && std::abs(difference) <= params.settleDegrees * degree) state.turning = false;
        rate = state.turning ? params.turnRateDegreesPerSecond * degree : 0.0f;
    }
    if (rate > 0.0f) {
        // Turn towards the head; an idle turn stops once the head is back
        // inside the settle band (turning clears above), leaving a small,
        // natural neck twist rather than snapping the body square.
        const float step = std::clamp(difference, -rate * dtSeconds, rate * dtSeconds);
        state.bodyYaw = WrapAngle(state.bodyYaw + step);
    }
    return state.bodyYaw;
}
}
