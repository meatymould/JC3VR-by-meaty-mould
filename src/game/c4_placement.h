#pragma once
#include <cmath>
#include <cstdint>

namespace jc3vr {
// Physics adapter must supply a fresh hand-origin surface query. This core
// never substitutes a camera crosshair hit or consumes inventory itself.
struct C4SurfaceContact {
    bool hit = false;
    float point[3]{}, normal[3]{};
    std::uintptr_t object = 0; // zero is valid for static world geometry
    std::uint64_t frame = 0;
};
struct C4PlacementInput {
    bool equipped = false, trackingValid = false, gameplay = false;
    bool placePressed = false;
    float palm[3]{};
    std::uint64_t frame = 0;
    C4SurfaceContact contact{};
};
struct C4PlacementDecision {
    bool showHeldCharge = false, contactReady = false, requestNativePlace = false;
    C4SurfaceContact contact{};
};
struct C4PlacementState { bool latched = false, needsRelease = true; };

inline bool C4NativeHitFraction(float fraction,float reach,float originalMaximum,float& out) {
    if(!std::isfinite(fraction) || !std::isfinite(reach) || !std::isfinite(originalMaximum) ||
       fraction<0 || fraction>=1 || reach<=0 || originalMaximum<reach) return false;
    out=fraction*reach/originalMaximum;return true;
}

inline C4PlacementDecision UpdateC4Placement(C4PlacementState& state, const C4PlacementInput& input) {
    C4PlacementDecision out{};
    if (!input.equipped || !input.trackingValid || !input.gameplay) {
        state = {}; // require a release after tracking/equip/menu transitions
        return out;
    }
    out.showHeldCharge = true;
    if (!input.placePressed) { state.needsRelease = false; state.latched = false; }
    float distanceSquared = 0, normalSquared = 0;
    bool valid = input.contact.hit && input.contact.frame == input.frame;
    for (int i = 0; i != 3; ++i) {
        valid = valid && std::isfinite(input.palm[i]) && std::isfinite(input.contact.point[i]) && std::isfinite(input.contact.normal[i]);
        const float delta = input.contact.point[i]-input.palm[i];
        distanceSquared += delta*delta;
        normalSquared += input.contact.normal[i]*input.contact.normal[i];
    }
    // Physical reach only: 12cm acquisition, 16cm release hysteresis.
    const float reach = state.latched ? 0.16f : 0.12f;
    out.contactReady = valid && normalSquared >= 0.25f && normalSquared <= 4.0f && distanceSquared <= reach*reach;
    if (!out.contactReady) { state.latched = false; return out; }
    out.contact = input.contact;
    const float inverseNormal = 1.0f/std::sqrt(normalSquared);
    for (float& component : out.contact.normal) component *= inverseNormal;
    state.latched = true;
    if (input.placePressed && !state.needsRelease) {
        out.requestNativePlace = true;
        state.needsRelease = true; // exactly one request per physical press
    }
    return out;
}
}
