#pragma once
#include "game/physical_hijack.h"
#include <cmath>
namespace jc3vr {
// Pure input policy. Native ownership/phase validation stays in the adapter.
struct PhysicalHijackGesture {
    bool sawRelease=false,grabbed=false,complete=false;
    std::uint64_t began=0,last=0;
    float start[3]{},previous[3]{},toward[3]{};
    void Reset(){*this={};}
    bool Fresh(const PhysicalHijackInput& i,std::uint64_t now)const {
        if(!i.active || now<i.now || now-i.now>250 || !std::isfinite(i.grip))return false;
        for(int c=0;c<3;++c)if(!std::isfinite(i.hand[c]) || !std::isfinite(i.head[c]))return false;
        return i.grip>=0 && i.grip<=1;
    }
    PhysicalHijackFeedback Update(const PhysicalHijackInput& i) {
        PhysicalHijackFeedback r{};r.consume=true;
        if(!Fresh(i,i.now) || (last && (i.now<last || i.now-last>250))) {Reset();return r;}
        last=i.now;
        if(complete)return r;
        if(i.grip<0.25f){sawRelease=true;grabbed=false;return r;}
        if(!sawRelease)return r;
        if(!grabbed && i.grip>0.65f) {
            float reach=0;
            for(int c=0;c<3;++c){toward[c]=i.head[c]-i.hand[c];reach+=toward[c]*toward[c];}
            if(reach<0.25f*0.25f || reach>1.1f*1.1f || i.hand[1]-i.head[1]<-0.75f || i.hand[1]-i.head[1]>0.4f)return r;
            reach=std::sqrt(reach);
            for(int c=0;c<3;++c){toward[c]/=reach;start[c]=previous[c]=i.hand[c];}
            grabbed=true;began=i.now;r.grabbed=true;return r;
        }
        if(!grabbed)return r;
        float step=0,travel=0,pull=0;
        for(int c=0;c<3;++c){const float d=i.hand[c]-start[c],s=i.hand[c]-previous[c];travel+=d*d;step+=s*s;pull+=d*toward[c];previous[c]=i.hand[c];}
        if(step>0.35f*0.35f || travel-pull*pull>0.25f*0.25f || i.now-began>4000){grabbed=false;sawRelease=false;return r;}
        if(pull>=0.20f && i.now-began>=120){complete=true;grabbed=false;r.opened=true;}
        return r;
    }
};
}
