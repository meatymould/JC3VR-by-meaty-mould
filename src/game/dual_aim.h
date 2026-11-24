#pragma once
#include <algorithm>
#include <cmath>

namespace jc3vr {
// Only a target for JC3's existing shot builder: this does not spawn bullets,
// alter weapon range, remove scatter, or write character/camera/grapple aim.
inline bool BuildDualAimTarget(const float muzzle[3], const float forward[3], float target[3]) {
    float lengthSquared=0;
    for(int i=0;i<3;++i) {
        if(!std::isfinite(muzzle[i]) || std::abs(muzzle[i])>1.0e7f || !std::isfinite(forward[i])) return false;
        lengthSquared+=forward[i]*forward[i];
    }
    if(!std::isfinite(lengthSquared) || lengthSquared<0.25f || lengthSquared>4.0f) return false;
    const float distance=1000.0f/std::sqrt(lengthSquared);
    // Start at the native muzzle, not the grip: no artificial convergence or
    // sideways muzzle-to-grip correction. Native projectile tuning limits range.
    for(int i=0;i<3;++i) target[i]=muzzle[i]+forward[i]*distance;
    return true;
}

// POD transaction, usable in the SEH island. No heap allocation or ownership
// of a native object; the caller serializes the complete pair on the game path.
struct DualAimBackup { float target[3]{}; int mode=-1; };
inline DualAimBackup ApplyDualAim(float sharedTarget[3],int& sharedMode,const float target[3]) {
    DualAimBackup previous{};
    std::copy(sharedTarget,sharedTarget+3,previous.target);
    previous.mode=sharedMode;
    std::copy(target,target+3,sharedTarget);
    sharedMode=0; // verified native AIM_POS; cone check and shot builder agree
    return previous;
}
inline void RestoreDualAim(float sharedTarget[3],int& sharedMode,const DualAimBackup& previous) {
    std::copy(previous.target,previous.target+3,sharedTarget);
    sharedMode=previous.mode;
}
}
