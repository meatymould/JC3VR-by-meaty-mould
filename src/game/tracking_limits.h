#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace jc3vr {
inline bool ClampRoomOffset(float xyz[3],float horizontal,float vertical) {
    for(int c=0;c<3;++c) if(!std::isfinite(xyz[c])) return false;
    if(!std::isfinite(horizontal)||!std::isfinite(vertical)) return false;
    const float length=std::hypot(xyz[0],xyz[2]);
    if(horizontal>0 && length>horizontal) {xyz[0]*=horizontal/length;xyz[2]*=horizontal/length;}
    if(vertical>0) xyz[1]=std::clamp(xyz[1],-vertical,vertical);
    return true;
}
struct RecenterTap {bool wasDown=false;};
inline bool TapRecenter(RecenterTap& state,bool combo) {
    const bool tapped=combo && !state.wasDown;
    state.wasDown=combo;
    return tapped; // one recenter per press, regardless of grips/triggers
}
inline bool LegacyAimGate(bool handAim,bool controllerAim,bool seated) {return handAim && !controllerAim && !seated;}
// Native gun rays do not steer the engine camera. Grapple targeting still
// does: use the left controller after support-grip input has been consumed.
inline int NativeGrappleAimHand(bool handAim,bool seated,bool inputActive,bool leftTracked,float leftGrip) {
    return handAim && !seated && inputActive && leftTracked && std::isfinite(leftGrip) && leftGrip>0.3f ? 0 : -1;
}
}
