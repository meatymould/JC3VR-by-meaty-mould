#pragma once
#include <cmath>

namespace jc3vr {
enum class PocketAction { None, Holster, Draw };
struct PocketHolster {
    bool hands = false;
    bool released = false;
};

inline bool InRightPocket(float right, float down, float forward) {
    return right >= 0.12f && right <= 0.43f &&
        down >= 0.38f && down <= 0.78f && std::abs(forward) <= 0.23f;
}

inline PocketAction UpdatePocketHolster(PocketHolster& state, bool valid, bool inPocket, float squeeze) {
    if (!valid || !std::isfinite(squeeze)) {
        state = {};
        return PocketAction::None;
    }
    if (squeeze <= 0.25f) state.released = true;
    if (squeeze < 0.72f || !state.released) return PocketAction::None;
    state.released = false;
    if (!inPocket) return PocketAction::None;
    state.hands = !state.hands;
    return state.hands ? PocketAction::Holster : PocketAction::Draw;
}
}
