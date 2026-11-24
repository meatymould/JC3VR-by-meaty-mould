#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace jc3vr {
// Ownership must be established before assigning a native projectile to a
// hand. Unknown dual ownership is silent, never guessed as right-handed.
inline int ShotHapticHand(bool local,bool parentMatches,int scoped,int placed,bool singleGun) {
    if(!local || !parentMatches) return -1;
    if(scoped>=0 && scoped<2) return scoped;
    if(placed>=0 && placed<2) return placed;
    return singleGun?1:-1;
}
// Gamepad motors are frequency bands, NOT left/right physical hands.
// Mirror native game feedback to both hands; explicit interaction pulses
// retain their own hand. Short leases prevent stuck buzz after XR stalls.
struct HapticFeedback {
    float rumble=0;std::uint64_t rumbleAt=0;
    std::array<float,2> pulse{};std::array<std::uint64_t,2> pulseUntil{};
    void Rumble(std::uint16_t low,std::uint16_t high,std::uint64_t now) {
        rumble=std::max(low,high)/65535.f;rumbleAt=now;
    }
    void Pulse(unsigned hand,float amplitude,std::uint64_t now) {
        if(hand>=2 || !std::isfinite(amplitude)) return;
        pulse[hand]=std::clamp(amplitude,0.f,1.f);pulseUntil[hand]=now+60;
    }
    std::array<float,2> Sample(std::uint64_t now,float scale) const {
        if(!std::isfinite(scale)) return {};
        scale=std::clamp(scale,0.f,1.f);
        const float game=now>=rumbleAt && now-rumbleAt<=250?rumble:0;
        return {scale*std::max(game,now<pulseUntil[0]?pulse[0]:0),
                scale*std::max(game,now<pulseUntil[1]?pulse[1]:0)};
    }
};
}
