#undef NDEBUG
#include "game/helicopter_pull.h"
#include <cassert>
#include <iostream>
using namespace jc3vr;
HelicopterPullInput Ready(HelicopterPullState& s) {
    HelicopterPullInput i{};i.active=true;i.vehicle=123;i.now=100;
    i.hand[2]=-0.6f;i.head[1]=i.hand[1]=1.5f;
    UpdateHelicopterPull(s,i);i.modifier=true;i.grip=1;i.now=110;
    const auto r=UpdateHelicopterPull(s,i);assert(r.grabbed && r.consume && !r.enter);return i;
}
int main() {
    {
        HelicopterPullState s{};auto i=Ready(s);i.now=250;i.hand[2]=-0.38f;
        auto r=UpdateHelicopterPull(s,i);assert(r.fired && r.enter && r.consume);
        i.now=260;r=UpdateHelicopterPull(s,i);assert(!r.fired && r.enter);
        i.now=450;r=UpdateHelicopterPull(s,i);assert(!r.enter && !r.fired && r.consume);
        i.now=460;i.modifier=false;UpdateHelicopterPull(s,i);
        i.now=470;i.modifier=true;r=UpdateHelicopterPull(s,i);assert(!r.grabbed && !r.fired);
        i.now=480;i.grip=0;UpdateHelicopterPull(s,i);i.now=490;i.grip=1;
        assert(UpdateHelicopterPull(s,i).grabbed);
    }
    {HelicopterPullState s{};auto i=Ready(s);i.now=250;i.head[2]=-0.4f;assert(!UpdateHelicopterPull(s,i).fired);} // head-only motion
    {HelicopterPullState s{};auto i=Ready(s);i.now=250;i.hand[2]=-0.38f;assert(UpdateHelicopterPull(s,i).fired);
     i.now=260;i.modifier=false;UpdateHelicopterPull(s,i);i.now=270;
     assert(UpdateHelicopterPull(s,i).enter);i.now=280;i.active=false;assert(!UpdateHelicopterPull(s,i).enter);}
    {HelicopterPullState s{};auto i=Ready(s);i.now=250;i.hand[2]=-0.75f;assert(!UpdateHelicopterPull(s,i).fired);} // wrong direction
    {HelicopterPullState s{};auto i=Ready(s);i.now=250;i.hand[0]=0.3f;assert(!UpdateHelicopterPull(s,i).fired && s.spent);}
    {HelicopterPullState s{};auto i=Ready(s);i.now=250;i.hand[2]=-0.5f;assert(!UpdateHelicopterPull(s,i).fired);} // short pull
    for(int failure=0;failure<6;++failure) {
        HelicopterPullState s{};auto i=Ready(s);i.hand[2]=-0.38f;i.now=250;
        if(failure==0)i.active=false;
        if(failure==1)i.vehicle=456;
        if(failure==2)i.hand[0]=NAN;
        if(failure==3)i.now=500;
        if(failure==4)i.now=50;
        if(failure==5)i.vehicle=0;
        auto r=UpdateHelicopterPull(s,i);assert(!r.fired && !r.enter && !s.grabbed);
    }
    {HelicopterPullState s{};auto i=Ready(s);i.now=250;i.hand[2]=0.0f;assert(!UpdateHelicopterPull(s,i).fired && s.spent);} // tracking jump
    {HelicopterPullState s{};auto i=Ready(s);for(int n=1;n<=16;++n){i.now=110+n*200;assert(!UpdateHelicopterPull(s,i).fired);}assert(s.spent);}
    {HelicopterPullState s{};auto i=Ready(s);i.now=200;i.grip=0;assert(!UpdateHelicopterPull(s,i).fired && !s.grabbed);}
    {HelicopterPullState s{};auto i=Ready(s);i.now=150;i.modifier=false;UpdateHelicopterPull(s,i);i.now=170;i.modifier=true;assert(!UpdateHelicopterPull(s,i).grabbed);}
    {HelicopterPullState s{};HelicopterPullInput i{};i.active=true;i.vehicle=123;i.modifier=true;i.grip=1;i.now=100;assert(!UpdateHelicopterPull(s,i).grabbed);} // held on reconnect
    std::cout<<"Helicopter pull gesture/cancellation/latching PASS; native hijack animation not validated.\n";
}
