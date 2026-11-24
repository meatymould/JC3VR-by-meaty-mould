#include "render/vr_hands.h"
#include <algorithm>
#include <cmath>

namespace jc3vr {
namespace {
struct Vec3 {
    float x, y, z;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 Normalize(Vec3 v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 1e-6f ? v * (1.0f / length) : Vec3{0.0f, 0.0f, 0.0f};
}
// Rotate v about unit axis k by angle (Rodrigues).
Vec3 Rotate(Vec3 v, Vec3 k, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float d = k.x * v.x + k.y * v.y + k.z * v.z;
    return v * c + Cross(k, v) * s + k * (d * (1.0f - c));
}

constexpr Vec3 kSkin{0.76f, 0.55f, 0.42f};
constexpr Vec3 kKnuckle{0.70f, 0.50f, 0.38f};
constexpr Vec3 kSleeve{0.16f, 0.17f, 0.15f};
constexpr Vec3 kGauntlet{0.22f, 0.22f, 0.24f};
constexpr Vec3 kGauntletLight{0.95f, 0.45f, 0.10f};

// Oriented box: origin at the centre of its base face, extending `length`
// along `along`, `width` along `across`, `thickness` along `up`.
void Box(std::vector<HandMeshVertex>& out, Vec3 base, Vec3 along, Vec3 across, Vec3 up,
         float length, float width, float thickness, Vec3 color) {
    const Vec3 a = along * length;
    const Vec3 w = across * (width * 0.5f);
    const Vec3 t = up * (thickness * 0.5f);
    const Vec3 corners[8] = {
        base - w - t, base + w - t, base + w + t, base - w + t,
        base - w - t + a, base + w - t + a, base + w + t + a, base - w + t + a,
    };
    // Faces as corner quads (outward winding does not matter: no culling).
    const int faces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {3, 2, 6, 7}, {0, 3, 7, 4}, {1, 5, 6, 2}};
    const Vec3 normals[6] = {along * -1.0f, along, up * -1.0f, up, across * -1.0f, across};
    for (int face = 0; face != 6; ++face) {
        const int* q = faces[face];
        for (const int index : {q[0], q[1], q[2], q[0], q[2], q[3]}) {
            HandMeshVertex vertex{};
            vertex.position[0] = corners[index].x;
            vertex.position[1] = corners[index].y;
            vertex.position[2] = corners[index].z;
            vertex.normal[0] = normals[face].x;
            vertex.normal[1] = normals[face].y;
            vertex.normal[2] = normals[face].z;
            vertex.color[0] = color.x;
            vertex.color[1] = color.y;
            vertex.color[2] = color.z;
            out.push_back(vertex);
        }
    }
}

// A finger chain starting at `base`, initially pointing along `direction`,
void RoundedSegment(std::vector<HandMeshVertex>& out, Vec3 base, Vec3 along, Vec3 across, Vec3 up,
                    float length, float width, float thickness, Vec3 color) {
    constexpr int sides = 8;
    const auto add = [&](Vec3 p, Vec3 n) {
        out.push_back({{p.x,p.y,p.z},{n.x,n.y,n.z},{color.x,color.y,color.z}});
    };
    for (int side = 0; side != sides; ++side) {
        const float a = side*6.283185307f/sides, b = (side+1)*6.283185307f/sides;
        const Vec3 offsetA = across*(std::cos(a)*width*0.5f)+up*(std::sin(a)*thickness*0.5f);
        const Vec3 offsetB = across*(std::cos(b)*width*0.5f)+up*(std::sin(b)*thickness*0.5f);
        const Vec3 normalA = Normalize(across*(std::cos(a)/width)+up*(std::sin(a)/thickness));
        const Vec3 normalB = Normalize(across*(std::cos(b)/width)+up*(std::sin(b)/thickness));
        const Vec3 p0=base+offsetA, p1=base+offsetB, p2=p0+along*length, p3=p1+along*length;
        add(p0,normalA); add(p2,normalA); add(p1,normalB);
        add(p1,normalB); add(p2,normalA); add(p3,normalB);
        add(base,along*-1); add(p1,along*-1); add(p0,along*-1);
        add(base+along*length,along); add(p2,along); add(p3,along);
    }
}

// A finger chain starting at `base`, initially pointing along `direction`,
// bending by `bends[i]` about `hinge` at each joint.
void Finger(std::vector<HandMeshVertex>& out, Vec3 base, Vec3 direction, Vec3 hinge, Vec3 palmNormal,
            const float* lengths, const float* bends, int segments, float width, float thickness) {
    Vec3 along = Normalize(direction);
    Vec3 up = Normalize(Cross(hinge, along));
    // Keep "up" on the back-of-hand side.
    if (up.x * palmNormal.x + up.y * palmNormal.y + up.z * palmNormal.z > 0.0f) up = up * -1.0f;
    Vec3 joint = base;
    for (int segment = 0; segment != segments; ++segment) {
        along = Normalize(Rotate(along, hinge, bends[segment]));
        up = Normalize(Rotate(up, hinge, bends[segment]));
        const float taper = 1.0f - 0.12f * static_cast<float>(segment);
        RoundedSegment(out, joint, along, hinge, up, lengths[segment], width * taper, thickness * taper,
            segment == 0 ? kKnuckle : kSkin);
        joint = joint + along * lengths[segment];
    }
}
}

void BuildHandMesh(bool leftHand, const HandCurl& curl, std::vector<HandMeshVertex>& out) {
    const float s = leftHand ? 1.0f : -1.0f;
    const Vec3 palmNormal{s, 0.0f, 0.0f};          // palm faces the midline
    const Vec3 forward{0.0f, -1.0f, 0.0f};         // extended fingers
    const Vec3 thumbward{0.0f, 0.0f, -1.0f};       // little finger -> thumb
    // Fingers curl toward the palm: rotating "forward" toward +palmNormal.
    const Vec3 hinge = Normalize(Cross(forward, palmNormal));
    const float index = std::clamp(curl.index, 0.0f, 1.0f);
    const float grip = std::clamp(curl.grip, 0.0f, 1.0f);
    const float thumb = std::clamp(curl.thumb, 0.0f, 1.0f);

    // The handle sits in front of the palm, so the palm lies behind the grip
    // origin (toward -palmNormal). Knuckles at y=-0.045, wrist at y=+0.050.
    const float palmX = -s * 0.028f;
    Box(out, {palmX, 0.050f, 0.004f}, forward, thumbward, palmNormal, 0.095f, 0.086f, 0.026f, kSkin);
    // Forearm/sleeve toward the user.
    RoundedSegment(out, {palmX, 0.050f, 0.004f}, Vec3{0.0f, 1.0f, 0.0f}, thumbward, palmNormal, 0.16f, 0.062f, 0.052f, kSleeve);
    if (leftHand) {
        // Rico's grappling gauntlet on the left wrist, with its orange light.
        Box(out, {palmX - s * 0.004f, 0.060f, 0.004f}, Vec3{0.0f, 1.0f, 0.0f}, thumbward, palmNormal, 0.075f, 0.074f, 0.066f, kGauntlet);
        Box(out, {palmX - s * 0.036f, 0.080f, 0.004f}, Vec3{0.0f, 1.0f, 0.0f}, thumbward, palmNormal, 0.028f, 0.022f, 0.006f, kGauntletLight);
    }
    // Fingers: z from index (toward the thumb, -Z) to little finger (+Z).
    const float fingerZ[4] = {-0.030f, -0.009f, 0.012f, 0.032f};
    const float fingerLengths[4][3] = {{0.044f, 0.026f, 0.021f}, {0.048f, 0.029f, 0.022f}, {0.045f, 0.027f, 0.021f}, {0.036f, 0.021f, 0.018f}};
    for (int finger = 0; finger != 4; ++finger) {
        const float amount = finger == 0 ? index : grip;
        const float bends[3] = {amount * 1.25f, amount * 1.55f, amount * 1.10f};
        Finger(out, {palmX, -0.045f, fingerZ[finger]}, forward, hinge, palmNormal,
               fingerLengths[finger], bends, 3, finger == 3 ? 0.015f : 0.017f, 0.016f);
    }
    // Thumb: from the index side of the palm, up along the handle and
    // forward; it folds down toward the palm/buttons when pressed.
    const Vec3 thumbDirection = Normalize(thumbward + forward * 0.55f + palmNormal * 0.25f);
    const Vec3 thumbHinge = Normalize(Cross(thumbDirection, palmNormal));
    const float thumbLengths[2] = {0.040f, 0.030f};
    const float thumbBends[2] = {thumb * 0.55f, thumb * 0.75f};
    Finger(out, {palmX + s * 0.006f, 0.010f, -0.040f}, thumbDirection, thumbHinge, palmNormal,
           thumbLengths, thumbBends, 2, 0.020f, 0.018f);
}

void BuildAimBeam(float length, float width, const float color[3], std::vector<HandMeshVertex>& out) {
    Box(out, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        length, width, width, {color[0], color[1], color[2]});
}

void BuildControllerMesh(bool leftHand, const HandCurl& curl, std::vector<HandMeshVertex>& out) {
    constexpr Vec3 kBody{0.10f, 0.10f, 0.11f};
    constexpr Vec3 kFace{0.15f, 0.15f, 0.16f};
    constexpr Vec3 kButton{0.42f, 0.42f, 0.45f};
    constexpr Vec3 kStick{0.22f, 0.22f, 0.24f};
    const float midline = leftHand ? 1.0f : -1.0f;  // +X is the left palm's side
    const Vec3 x{1.0f, 0.0f, 0.0f}, y{0.0f, 1.0f, 0.0f}, z{0.0f, 0.0f, 1.0f};
    const Vec3 up{0.0f, 0.0f, -1.0f};  // toward the controller head
    // Handle: 10 cm along Z, held in the fist around the grip origin.
    Box(out, {0.0f, 0.0f, 0.05f}, up, x, y, 0.10f, 0.030f, 0.036f, kBody);
    // Head: a wider block above the fist, leaning back toward the user (+Y).
    const Vec3 headUp = Normalize(up * 0.85f + y * 0.35f);
    const Vec3 headBack = Normalize(Cross(headUp, x) * -1.0f);
    Box(out, {0.0f, 0.004f, -0.045f}, headUp, x, headBack, 0.024f, 0.050f, 0.056f, kFace);
    // Face controls on top of the head: thumbstick on the midline side,
    // two buttons on the outer side.
    const Vec3 top = Vec3{0.0f, 0.004f, -0.045f} + headUp * 0.024f;
    Box(out, top + x * (0.010f * midline) + headBack * 0.004f, headUp, x, headBack, 0.006f, 0.016f, 0.016f, kFace);
    Box(out, top + x * (0.010f * midline) + headBack * 0.004f + headUp * 0.006f, headUp, x, headBack, 0.009f, 0.007f, 0.007f, kStick);
    Box(out, top - x * (0.012f * midline) - headBack * 0.008f, headUp, x, headBack, 0.003f, 0.010f, 0.010f, kButton);
    Box(out, top - x * (0.012f * midline) + headBack * 0.012f, headUp, x, headBack, 0.003f, 0.010f, 0.010f, kButton);
    // Trigger on the finger side under the head; pulled in by the trigger value.
    const float pull = 0.008f * std::clamp(curl.index, 0.0f, 1.0f);
    Box(out, {0.0f, -0.024f + pull, -0.030f}, up, x, y, 0.028f, 0.014f, 0.010f, kStick);
    // Grip button on the midline side of the handle.
    const float squeeze = 0.004f * std::clamp(curl.grip, 0.0f, 1.0f);
    Box(out, {midline * (0.016f - squeeze), -0.004f, 0.010f}, up, y, x, 0.040f, 0.020f, 0.006f, kStick);
    (void)z;
}
}
