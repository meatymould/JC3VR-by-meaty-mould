#pragma once
#include <cmath>
#include <cstdint>
namespace jc3vr {
// Gesture-only prototype. No inferred handle location, door writes or physics.
struct HelicopterPullInput {
    std::uintptr_t vehicle=0;
    std::uint64_t now=0;
    bool active=false,modifier=false;
    float grip=0,hand[3]{},head[3]{}; // same OpenXR LOCAL space, metres
};
struct HelicopterPullState {
    std::uintptr_t vehicle=0;
    std::uint64_t last=0,began=0,pulseUntil=0;
    bool grabbed=false,spent=false,requireRelease=true;
    float start[3]{},previous[3]{},toward[3]{};
};
struct HelicopterPullResult { bool consume=false,grabbed=false,fired=false,enter=false; };
inline HelicopterPullResult UpdateHelicopterPull(HelicopterPullState& s,const HelicopterPullInput& i) {
    bool finite=std::isfinite(i.grip);
    for(int c=0;c<3;++c) finite=finite && std::isfinite(i.hand[c]) && std::isfinite(i.head[c]);
    if(!i.active || !i.vehicle || !finite || i.now<s.last || (s.last && i.now-s.last>250) ||
       (s.vehicle && s.vehicle!=i.vehicle)) {
        s={};s.last=i.now;return {};
    }
    s.last=i.now;s.vehicle=i.vehicle;
    if(s.requireRelease) {
        if(i.grip<0.25f)s.requireRelease=false;
        return {i.modifier && i.grip>=0.25f,false,false,i.now<s.pulseUntil};
    }
    HelicopterPullResult r{};
    r.enter=i.now<s.pulseUntil;
    // Captured grab owns X/LB until release, including after a timeout/firing.
    r.consume=s.grabbed || s.spent;
    if(i.grip<0.25f || !i.modifier) {
        if(i.grip>=0.25f && (s.grabbed || s.spent))s.requireRelease=true;
        s.grabbed=s.spent=false;return r;
    }
    if(!s.grabbed && !s.spent && i.grip>0.65f) {
        float length=0;
        for(int c=0;c<3;++c) {s.toward[c]=i.head[c]-i.hand[c];length+=s.toward[c]*s.toward[c];}
        // Require deliberate reach, not a squeeze at the chest or at the feet.
        if(length<0.35f*0.35f || length>1.0f || i.hand[1]-i.head[1]<-0.65f || i.hand[1]-i.head[1]>0.35f)return r;
        length=std::sqrt(length);
        for(int c=0;c<3;++c) {s.toward[c]/=length;s.start[c]=s.previous[c]=i.hand[c];}
        s.grabbed=true;s.began=i.now;r.grabbed=true;r.consume=true;return r;
    }
    if(!s.grabbed)return r;
    float step=0,travel=0,pull=0;
    for(int c=0;c<3;++c) {
        const float d=i.hand[c]-s.start[c],p=i.hand[c]-s.previous[c];
        travel+=d*d;step+=p*p;pull+=d*s.toward[c];s.previous[c]=i.hand[c];
    }
    if(i.now-s.began>3000 || step>0.35f*0.35f || travel-pull*pull>0.25f*0.25f) {
        s.grabbed=false;s.spent=true;return r;
    }
    if(pull>=0.20f && i.now-s.began>=120) {
        s.grabbed=false;s.spent=true;s.pulseUntil=i.now+180;
        r.fired=r.enter=true;
    }
    return r;
}
}
