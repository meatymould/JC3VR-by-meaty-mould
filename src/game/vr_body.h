#pragma once

// Engine-independent VR body math: the pieces that decide where Rico's body
// and arms go given the headset and controllers. Kept free of game memory so
// it is unit-tested offline; wiring to JC3's skeleton happens only once the
// player's pose buffer is identified (see game/camera_source_scan skeleton
// probe and docs/DEVELOPMENT_NOTES.md).
namespace jc3vr {
struct BodyVec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Two-bone IK (shoulder -> elbow -> wrist). Bone lengths come from the
// animated pose and are preserved; the reach is clamped just short of full
// extension, so arms never stretch and never snap straight. The elbow bends
// towards `pole` (a direction, e.g. down/back/out from the shoulder).
struct TwoBoneIkResult {
    BodyVec3 elbow;
    BodyVec3 wrist;
    bool reached = false;  // target within reach (else wrist stops short)
};
TwoBoneIkResult SolveTwoBoneIk(const BodyVec3& shoulder, const BodyVec3& elbow, const BodyVec3& wrist,
                               const BodyVec3& target, const BodyVec3& pole);

// Default elbow pole for a hand target: down and slightly out/back from the
// shoulder, in a y-up frame with `bodyRight`/`bodyForward` horizontal axes.
BodyVec3 ElbowPole(bool leftArm, const BodyVec3& bodyRight, const BodyVec3& bodyForward);

// Neck pivot below and behind the eyes (the point a real head rotates
// about). Torso placement anchors here so looking down moves the eyes
// forward over the chest instead of rotating the whole body.
BodyVec3 NeckFromEyes(const BodyVec3& eyeCentre, const BodyVec3& headForward, const BodyVec3& headUp);

// Body yaw that does not copy the head one-to-one: inside the comfort
// zone the body holds still; beyond it the body turns until the head is
// back within `settleDegrees`; while moving it follows the head (locomotion
// direction). Angles in radians, yaw about world up.
struct BodyYawParams {
    float comfortDegrees = 40.0f;
    float settleDegrees = 12.0f;
    float turnRateDegreesPerSecond = 220.0f;
    float moveFollowRateDegreesPerSecond = 360.0f;
    float moveThreshold = 0.2f;  // stick magnitude 0..1
};
struct BodyYawState {
    float bodyYaw = 0.0f;
    bool initialized = false;
    bool turning = false;
};
float UpdateBodyYaw(BodyYawState& state, float headYaw, float moveAmount, float dtSeconds, const BodyYawParams& params = {});

// Wrapped difference a - b in (-pi, pi].
float WrapAngle(float radians);
}
