#pragma once
#include <cstdint>

namespace jc3vr {
struct PauseChord {
    std::uint64_t began = 0, pulseUntil = 0, lastTime = 0;
    bool latched = false, spent = false, requireRelease = false;
};
struct PauseChordResult { bool consume = false, pause = false, fired = false; };
inline void CancelPauseChord(PauseChord& state) {
    state = {};
    state.requireRelease = true;
}

// Individual face buttons stay immediate. A recognised X+Y chord consumes
// both buttons through complete release, and fires only once per 1s hold.
inline PauseChordResult UpdatePauseChord(PauseChord& state, bool active,
                                        bool x, bool y, std::uint64_t now) {
    if (!active || now < state.lastTime) {
        state = {};
        state.requireRelease = x || y;
        state.lastTime = now;
        return {x || y, false, false};
    }
    state.lastTime = now;
    if (state.requireRelease) {
        if (!x && !y) state.requireRelease = false;
        return {x || y, false, false};
    }
    if (!state.latched && x && y) {
        state.latched = true;
        state.spent = false;
        state.began = now;
    }
    PauseChordResult result{};
    result.consume = state.latched;
    if (state.latched) {
        if (!x || !y) state.spent = true;
        if (!state.spent && now - state.began >= 1000) {
            state.spent = true;
            state.pulseUntil = now + 150;
            result.fired = true;
        }
        if (!x && !y) { state.latched = false; state.spent = false; }
    }
    result.pause = now < state.pulseUntil;
    return result;
}
}
