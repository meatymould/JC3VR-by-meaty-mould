#pragma once
#include <cmath>
namespace jc3vr {
// Follow measured actor displacement since sampling, not velocity prediction.
// Tracking/head motion stays in the target; actor locomotion is applied once.
inline bool RebaseTrackedPoint(const float sampled[3], const float oldRoot[3], const float currentRoot[3], float out[3]) {
    float result[3]{},distance=0;
    for (int i=0;i<3;++i) {
        if (!std::isfinite(sampled[i]) || !std::isfinite(oldRoot[i]) || !std::isfinite(currentRoot[i])) return false;
        const float delta=currentRoot[i]-oldRoot[i]; distance+=delta*delta;
        result[i]=sampled[i]+delta;
    }
    if (!std::isfinite(distance) || distance>100.0f*100.0f) return false;
    for (int i=0;i<3;++i) out[i]=result[i];
    return true;
}
}
