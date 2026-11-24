#undef NDEBUG
#include "game/dual_aim.h"
#include "game/dual_wield.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <iostream>

namespace {
bool Near(float a,float b) { return std::abs(a-b)<0.001f; }
void TargetGeometry() {
    const float muzzle[3]{2184,1244,-1591};
    float target[3]{};
    for(const auto& direction : {std::array<float,3>{-1,0,0},{1,0,0},{0,1,0},{0,0,-1},{0.6f,0,0.8f}}) {
        assert(jc3vr::BuildDualAimTarget(muzzle,direction.data(),target));
        for(int i=0;i<3;++i) assert(Near(target[i]-muzzle[i],direction[i]*1000));
    }
    const float scaled[3]{0,0,-2};
    assert(jc3vr::BuildDualAimTarget(muzzle,scaled,target));
    assert(Near(target[2],muzzle[2]-1000));
    // No state change on rejected input.
    const std::array<float,3> before{target[0],target[1],target[2]};
    for(float invalid : {0.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        const float bad[3]{invalid,0,0};
        assert(!jc3vr::BuildDualAimTarget(muzzle,bad,target));
        assert(!std::memcmp(before.data(),target,sizeof(target)));
    }
    const float badOrigin[3]{0,std::numeric_limits<float>::quiet_NaN(),0};
    const float forward[3]{1,0,0};
    assert(!jc3vr::BuildDualAimTarget(badOrigin,forward,target));
}
void FireMechanics() {
    assert(jc3vr::SupportedDualFireMechanics(false,false,false,0,0,0));
    // Actual +976F00 base constructor: idle forced-shot count is -1, not 0.
    assert(jc3vr::SupportedDualFireMechanics(false,false,false,0,-1,0));
    // Fail closed on unknown negative values; the native idle sentinel is -1.
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,0,-2,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,0,INT32_MIN,0));
    assert(!jc3vr::SupportedDualFireMechanics(true,false,false,0,-1,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,true,false,0,-1,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,true,0,-1,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,.1f,-1,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,0,-1,.1f));
    assert(!jc3vr::SupportedDualFireMechanics(true,false,false,0,0,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,true,false,0,0,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,true,0,0,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,.1f,0,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,0,1,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,0,0,.1f));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,NAN,0,0));
    assert(!jc3vr::SupportedDualFireMechanics(false,false,false,0,0,INFINITY));
}
void ExpiredBlockIsNotCorruption() {
    jc3vr::DualFireState s{};
    s.blocked=0.001f-1.f/30;
    assert(s.blocked< -0.001f && jc3vr::SaneDualFireState(s));
    s.buffer=-0.1f;assert(!jc3vr::SaneDualFireState(s));s.buffer=0;
    s.sinceShot=-0.1f;assert(!jc3vr::SaneDualFireState(s));s.sinceShot=0;
    s.blocked=NAN;assert(!jc3vr::SaneDualFireState(s));
    s.blocked=INFINITY;assert(!jc3vr::SaneDualFireState(s));
    s.blocked=-3601;assert(!jc3vr::SaneDualFireState(s));
    s.blocked=0;s.flags[0]=2;assert(!jc3vr::SaneDualFireState(s));
}
void PlacementOwnership() {
    jc3vr::DualGunPlacements placements;
    placements.Record(10,0,1000);placements.Record(20,1,1001);
    placements.Record(30,1,1002); // later attachment on right does NOT erase pistol20
    assert(placements.HandFor(10,1100)==0 && placements.HandFor(20,1100)==1);
    assert(placements.HandFor(30,1100)==1);
    assert(placements.HandFor(10,1250)==0 && placements.HandFor(10,1251)==-1);
    assert(placements.HandFor(20,900)==-1 && placements.HandFor(0,1100)==-1);
    placements.Record(20,0,1100); // same component moved: latest authoritative hand wins
    assert(placements.HandFor(20,1100)==0);
    placements.Record(20,9,1101);placements.Record(0,0,1101);
    assert(placements.HandFor(20,1101)==0);
    for(int i=0;i<32;++i) placements.Record(100+i,i%2,1200+i);
    assert(placements.HandFor(10,1250)==-1 && placements.HandFor(20,1250)==-1);
    for(int i=0;i<32;++i) assert(placements.HandFor(100+i,1250)==i%2);
}
void ScopedState() {
    float shared[3]{30,40,50}; int mode=1;
    const float left[3]{-900,2,3},right[3]{1100,2,3};
    const auto a=jc3vr::ApplyDualAim(shared,mode,left);
    assert(mode==0 && shared[0]==-900);
    const auto b=jc3vr::ApplyDualAim(shared,mode,right);
    assert(mode==0 && shared[0]==1100);
    jc3vr::RestoreDualAim(shared,mode,b);
    assert(mode==0 && shared[0]==-900);
    jc3vr::RestoreDualAim(shared,mode,a);
    assert(mode==1 && shared[0]==30 && shared[1]==40 && shared[2]==50);
    // Byte-for-byte preservation of unset mode and signed zero.
    mode=-1;shared[1]=-0.0f;float original[3];std::memcpy(original,shared,sizeof(shared));
    const auto c=jc3vr::ApplyDualAim(shared,mode,left);
    jc3vr::RestoreDualAim(shared,mode,c);
    assert(mode==-1 && !std::memcmp(original,shared,sizeof(shared)));
}
void NativeFaultIsland(float* shared,int& mode,const float* target) {
    const auto previous=jc3vr::ApplyDualAim(shared,mode,target);
    __try {
        RaiseException(0xE0420001,0,0,nullptr); // synthetic native-call fault
    } __finally {
        jc3vr::RestoreDualAim(shared,mode,previous);
    }
}
void FaultRestoration() {
    float shared[3]{5,6,7};int mode=-1;const float target[3]{-999,6,7};
    bool caught=false;
    __try { NativeFaultIsland(shared,mode,target); }
    __except(GetExceptionCode()==0xE0420001?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {caught=true;}
    assert(caught && mode==-1 && shared[0]==5 && shared[1]==6 && shared[2]==7);
}
void SerializedNativeContract() {
    // Mock native shot consumer, NOT a simulation of JC3 ballistics. Uses the
    // production target/transaction helpers and existing trigger veto helper.
    std::mutex fireMutex;float shared[3]{1,2,3};int mode=-1;
    std::atomic<int> calls[2]{};
    const auto worker=[&](int hand) {
        const float forward[3]{hand?1.0f:-1.0f,0,0};
        const float muzzle[3]{hand?0.3f:-0.3f,1.5f,0};
        float target[3]{};assert(jc3vr::BuildDualAimTarget(muzzle,forward,target));
        for(int i=0;i<1000;++i) {
            std::lock_guard lock(fireMutex);
            const auto old=jc3vr::ApplyDualAim(shared,mode,target);
            if(jc3vr::PermittedDualTrigger(1,1)>0) {
                assert(mode==0 && (shared[0]-muzzle[0])*forward[0]>999);
                std::this_thread::yield();
                assert((shared[0]-muzzle[0])*forward[0]>999);
                ++calls[hand];
            }
            jc3vr::RestoreDualAim(shared,mode,old);
            assert(mode==-1 && shared[0]==1 && shared[1]==2 && shared[2]==3);
        }
    };
    std::thread left(worker,0),right(worker,1);left.join();right.join();
    assert(calls[0]==1000 && calls[1]==1000);
    assert(jc3vr::PermittedDualTrigger(1,0)==0);
    assert(jc3vr::PermittedDualTrigger(0,1)==0);
}
}
int main() {TargetGeometry();FireMechanics();ExpiredBlockIsNotCorruption();PlacementOwnership();ScopedState();FaultRestoration();SerializedNativeContract();std::cout<<"dual aim geometry / mechanics / placement ownership / SEH restoration / serialized dispatch contract passed\n";}
