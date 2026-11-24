#pragma once
#include "game/weapon_pose.h"
#include <cstddef>

namespace jc3vr {
// Live attachment landmark: the local gauntlet uses a 4096-byte upload
// with its absolute affine world matrix at +0. Size alone is NOT ownership.
inline bool IsLocalGauntletUpload(std::size_t bytes, const float world[16],
                                 bool recentPlacedMatch, const float* animatedForearm) {
    if (bytes != 4096 || !world || !MatchesPlacedWeapon(world, world)) return false;
    return recentPlacedMatch || (animatedForearm && MatchesPlacedWeapon(world, animatedForearm));
}
}
