#include "game/dual_wield.h"
#include "game/dual_aim.h"
#include "game/vr_body_rig.h"
#include "game/vehicle_camera.h"
#include "game/vr_gamepad.h"
#include "game/haptic_feedback.h"
#include "core/logging.h"
#include <Windows.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <intrin.h>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif
namespace jc3vr {
namespace {
using UpdateFn=bool(__fastcall*)(void*,float,void*,const float*);
using GunFn=void(__fastcall*)(void*,void*);
using FireFromFn=float*(__fastcall*)(void*,float*,void*);
UpdateFn originalUpdate=nullptr;
GunFn originalStop=nullptr,originalProjectile=nullptr;
FireFromFn nativeFireFrom=nullptr;
std::atomic<bool> installed=false,fault=false;
std::atomic<std::uintptr_t> requestedWeapon=0;
std::atomic<std::uint64_t> updates=0,shots[2]{};
// Fixed-size observations only; worker hooks never format or write log lines.
// Present exports at most 12 summaries, no disk writes per update or shot.
std::atomic<std::uint64_t> requestedCalls=0,wrongCaller=0,staleRequest=0,pairFailures=0,inputFailures=0;
std::atomic<std::uint64_t> activeRequests=0;
std::atomic<std::uintptr_t> lastCaller=0;
std::atomic<std::uint64_t> handCalls[2]{},triggerCalls[2]{},nativeVetoes=0;
std::atomic<float> lastTrigger[2]{},lastNativeInput=0;
std::atomic<unsigned> lastTrying[2]{},lastFiring[2]{};
struct Request { std::uintptr_t weapon=0; bool active=false; float trigger[2]{}; ULONGLONG tick=0; };
std::mutex inputMutex,fireMutex;
// Keep ownership by component identity, not just the last object placed on a
// hand. An effect/secondary attachment must not erase the other gun's mapping.
DualGunPlacements placements; Request request;
thread_local std::uintptr_t scopedWeapon=0,scopedGun=0;
thread_local int scopedHand=-1;
bool Read(void* dst,std::uintptr_t address,std::size_t size) {
    __try { std::memcpy(dst,reinterpret_cast<void*>(address),size); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool Read(T& dst,std::uintptr_t address) { return Read(&dst,address,sizeof(dst)); }
bool Guard(std::uintptr_t at,const std::uint8_t* expected,std::size_t n) {
    std::uint8_t bytes[32]{};
    return n<=sizeof(bytes) && Read(bytes,at,n) && !std::memcmp(bytes,expected,n);
}
bool Writable(std::uintptr_t p,std::size_t size) {
    MEMORY_BASIC_INFORMATION m{};
    return VirtualQuery(reinterpret_cast<void*>(p),&m,sizeof(m))==sizeof(m) && m.State==MEM_COMMIT &&
        !(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) && (m.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) &&
        p+size>=p && p+size<=reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize;
}
struct Pair { std::uintptr_t weapon=0,gun[2]{}; int hand[2]{-1,-1}; float forward[2][3]{}; };
enum class PairReject {None,Local,Owner,State,Mechanics,List,Component,Placement,Hands,Direction};
struct PairObservation {
    int slot=-1,state=-1,forceCount=0;
    bool heat=false,forced=false,alt=false;
    float forceTimer=0,charge=0;
    bool mechanicsRead=false;
};
const char* RejectName(PairReject reason) {
    switch(reason) {
    case PairReject::None:return "none";case PairReject::Local:return "local character/vehicle";
    case PairReject::Owner:return "inventory/vtable";case PairReject::State:return "slot/equip state";
    case PairReject::Mechanics:return "unsupported fire mechanics";case PairReject::List:return "two-component list";
    case PairReject::Component:return "component ownership";case PairReject::Placement:return "fresh component placement";
    case PairReject::Hands:return "distinct hands";case PairReject::Direction:return "fresh hand directions";
    }
    return "unknown";
}
bool ReadPair(std::uintptr_t weapon,Pair& pair,PairReject* rejection=nullptr,PairObservation* observation=nullptr) {
    const auto reject=[&](PairReject reason) {if(rejection)*rejection=reason;return false;};
    const auto rico=VrBodyCharacter();
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t held=0,table=0,begin=0,end=0;
    int slot=-1,state=-1; bool heat=true,forced=true,alt=true;
    float forceTimer=0,charge=0; int forceCount=0;
    if(!rico || !weapon || GetVehicleCameraAnchor().valid) return reject(PairReject::Local);
    if(!Read(held,rico+0x998) || held!=weapon || !Read(table,weapon) || table!=image+0x23E3038)
        return reject(PairReject::Owner);
    const bool stateRead=Read(slot,weapon+0x420) && Read(state,weapon+0x424);
    if(observation) {observation->slot=slot;observation->state=state;}
    if(!stateRead || slot!=0 || (state!=1 && state!=2))
        return reject(PairReject::State);
    // +466 is m_IsPrecisionAimDisabled (zoom), NOT a native fire veto. In
    // particular, a pistol must not need rifle precision mode to enable LT.
    const bool mechanicsRead=Read(heat,weapon+0x38C) && Read(forced,weapon+0x178) && Read(alt,weapon+0x164) &&
       Read(forceTimer,weapon+0x17C) && Read(forceCount,weapon+0x180) && Read(charge,weapon+0x194);
    if(observation) {
        observation->heat=heat;observation->forced=forced;observation->alt=alt;
        observation->forceTimer=forceTimer;observation->forceCount=forceCount;
        observation->charge=charge;observation->mechanicsRead=mechanicsRead;
    }
    if(!mechanicsRead || !SupportedDualFireMechanics(heat,forced,alt,forceTimer,forceCount,charge))
        return reject(PairReject::Mechanics);
    if(!Read(begin,weapon+0x568) || !Read(end,weapon+0x570) || !begin || end<begin || end-begin!=16 ||
       !Read(pair.gun,begin,sizeof(pair.gun)) || !pair.gun[0] || pair.gun[0]==pair.gun[1]) return reject(PairReject::List);
    DualGunPlacements seen;
    { std::lock_guard lock(inputMutex); seen=placements; }
    const auto now=GetTickCount64();
    for(int index=0;index<2;++index) {
        std::uintptr_t parent=0;
        if(!Read(table,pair.gun[index]) || table!=image+0x23F4750 ||
            !Read(parent,pair.gun[index]+0xC0) || parent!=weapon) return reject(PairReject::Component);
        pair.hand[index]=seen.HandFor(pair.gun[index],now);
        if(pair.hand[index]<0) return reject(PairReject::Placement);
    }
    if(pair.hand[0]==pair.hand[1]) return reject(PairReject::Hands);
    // Reuse the same existing world-space hand targets as gun placement. Take
    // both directions before firing either gun, not a mutable shared aim ray.
    for(int index=0;index<2;++index) {
        float grip[3]{},down[3]{},test[3]{},zero[3]{};
        if(!GetVrBodyHandTarget(pair.hand[index],grip,pair.forward[index],down) ||
           !BuildDualAimTarget(zero,pair.forward[index],test)) return reject(PairReject::Direction);
    }
    pair.weapon=weapon; return true;
}
bool ReadFireState(std::uintptr_t w,DualFireState& s) {
    return Read(s.flags,w+0x158,sizeof(s.flags)) && Read(s.blocked,w+0x174) && Read(s.buffer,w+0x184) &&
        Read(s.pullTime,w+0x18C) && Read(s.sinceShot,w+0x198) && Read(s.firingTime,w+0x19C) && SaneDualFireState(s);
}
void WriteFireState(std::uintptr_t w,const DualFireState& s) {
    std::memcpy(reinterpret_cast<void*>(w+0x158),s.flags,sizeof(s.flags));
    *reinterpret_cast<float*>(w+0x174)=s.blocked; *reinterpret_cast<float*>(w+0x184)=s.buffer;
    *reinterpret_cast<float*>(w+0x18C)=s.pullTime; *reinterpret_cast<float*>(w+0x198)=s.sinceShot;
    *reinterpret_cast<float*>(w+0x19C)=s.firingTime;
}
struct Cache { std::uintptr_t weapon=0,gun[2]{}; DualFireState hand[2]; ULONGLONG tick=0; } cache;
void __fastcall StopDetour(void* self,void* data) {
    // Native release loops all guns; releasing left must not stop right effects.
    if(scopedWeapon==reinterpret_cast<std::uintptr_t>(self) && scopedGun!=reinterpret_cast<std::uintptr_t>(data)) return;
    originalStop(self,data);
}
int ProjectileHapticHand(std::uintptr_t weapon,std::uintptr_t data) {
    if(!VrHapticsEnabled() || !weapon || !data) return -1;
    const auto rico=VrBodyCharacter();
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    int slot=-1,state=-1;std::uintptr_t held=0,parent=0,table=0,begin=0,end=0,first=0;
    if(!rico || !Read(slot,weapon+0x420) || (slot!=0 && slot!=1 && slot!=4 && slot!=6) ||
       !Read(held,rico+0x998+16*slot) || held!=weapon ||
       !Read(state,weapon+0x424) || (state!=1 && state!=2) ||
       !Read(table,data) || table!=image+0x23F4750 || !Read(parent,data+0xC0) || parent!=weapon) return -1;
    const bool scoped=scopedWeapon==weapon && scopedGun==data;
    int hand=-1;
    {
        std::lock_guard lock(inputMutex);
        const auto now=GetTickCount64();
        hand=placements.HandFor(data,now);
    }
    const bool single=Read(begin,weapon+0x568) && Read(end,weapon+0x570) && begin &&
        end>=begin && end-begin==sizeof(std::uintptr_t) && Read(first,begin) && first==data;
    return ShotHapticHand(true,true,scoped?scopedHand:-1,hand,single);
}
void __fastcall ProjectileDetour(void* self,void* data) {
    if(scopedWeapon==reinterpret_cast<std::uintptr_t>(self) && scopedGun!=reinterpret_cast<std::uintptr_t>(data)) return;
    originalProjectile(self,data);
    // Observe the existing native projectile dispatch, not the trigger or HUD.
    // No additional hook or modification of the original fire/reload path.
    const int hapticHand=ProjectileHapticHand(reinterpret_cast<std::uintptr_t>(self),reinterpret_cast<std::uintptr_t>(data));
    if(hapticHand>=0) {
        RequestVrHapticPulse(static_cast<unsigned>(hapticHand),0.6f);
        static std::atomic<unsigned> reports=0;
        if(reports.fetch_add(1)<6) TraceRuntime("VR haptics: local native projectile pulse hand="+std::to_string(hapticHand));
    }
    if(scopedWeapon==reinterpret_cast<std::uintptr_t>(self) && scopedGun==reinterpret_cast<std::uintptr_t>(data) && scopedHand>=0)
        ++shots[scopedHand];
}
// No C++ destructors in this SEH island. Never fall through and fire again
// after a partially completed native call; restore scoped fields and disable.
bool RunPair(const Pair& pair,const Request& wanted,float dt,const float* nativeInput) {
    const auto w=pair.weapon;
    DualFireState before{};
    int mode=-1; std::uint8_t cursor=0; float beforeAim[3]{};
    if(!nativeFireFrom || !Writable(w+0x158,0x48) || !Writable(w+0x3FC,sizeof(beforeAim)) ||
       !ReadFireState(w,before) || !Read(mode,w+0x16C) || !Read(cursor,w+0x160) || cursor>1 ||
       !Read(beforeAim,w+0x3FC,sizeof(beforeAim))) {
        fault=true;return false;
    }
    if(cache.weapon!=w || cache.gun[0]!=pair.gun[0] || cache.gun[1]!=pair.gun[1] || GetTickCount64()-cache.tick>250) {
        cache={}; cache.weapon=w; std::memcpy(cache.gun,pair.gun,sizeof(pair.gun));
        for(auto& hand:cache.hand) { hand.sinceShot=before.sinceShot; hand.blocked=before.blocked; }
    } else {
        // Native +198 is a shared clock advanced outside this per-gun call.
        // Advance each cached gun once per simulation update, not per XR frame.
        for(auto& hand:cache.hand) {
            AdvanceDualFireClock(hand,dt);
        }
    }
    // Preserve cancellation/cooldown newly set by the enclosing native update,
    // rather than overwriting those restrictions with last frame's hand state.
    const auto combined=CombinedDualFireState(cache.hand[0],cache.hand[1]);
    for(auto& hand:cache.hand) {
        if(before.flags[4] && !combined.flags[4]) hand.flags[4]=1;
        if(before.blocked>combined.blocked+0.001f) hand.blocked=before.blocked;
    }
    bool result=false;
    __try {
        for(int index=0;index<2;++index) {
            const int hand=pair.hand[index];
            WriteFireState(w,cache.hand[hand]);
            *reinterpret_cast<std::uint8_t*>(w+0x160)=static_cast<std::uint8_t>(index);
            scopedWeapon=w;scopedGun=pair.gun[index];scopedHand=hand;
            // Input has exactly two floats at this native call site. Keep the
            // parent's main-fire permission (reload/menu/state masks), no altfire.
            bool reloading=false;
            if(!Read(reloading,w+0x188)) { fault=true;scopedWeapon=scopedGun=0;scopedHand=-1;break; }
            float input[2]{reloading?0:PermittedDualTrigger(wanted.trigger[hand],nativeInput[0]),0};
            ++handCalls[hand];
            lastTrigger[hand]=wanted.trigger[hand];lastNativeInput=nativeInput[0];
            if(input[0]>0.05f) ++triggerCalls[hand];
            if(wanted.trigger[hand]>0.05f && input[0]<=0.05f) ++nativeVetoes;
            alignas(16) float muzzle[4]{}; float target[3]{};
            // Offline verified vtable+C8 == 966940, ABI (weapon,out,data).
            // This getter composes the selected component's fire point and world
            // transform. Native recoil/scatter/projectile/damage code stays live.
            if(nativeFireFrom(reinterpret_cast<void*>(w),muzzle,reinterpret_cast<void*>(pair.gun[index]))!=muzzle ||
               !BuildDualAimTarget(muzzle,pair.forward[index],target)) {
                fault=true;scopedWeapon=scopedGun=0;scopedHand=-1;break;
            }
            auto* aim=reinterpret_cast<float*>(w+0x3FC);
            auto& directionMode=*reinterpret_cast<int*>(w+0x16C);
            const auto aimBackup=ApplyDualAim(aim,directionMode,target);
            // Finally restores even if a native call raises SEH. The outer
            // exception path disables this feature and NEVER retries a shot.
            __try {
                result=originalUpdate(reinterpret_cast<void*>(w),dt,reinterpret_cast<void*>(pair.gun[index]),input) || result;
            } __finally {
                RestoreDualAim(aim,directionMode,aimBackup);
            }
            scopedWeapon=scopedGun=0;scopedHand=-1;
            if(!ReadFireState(w,cache.hand[hand])) { fault=true; break; }
            lastTrying[hand]=cache.hand[hand].flags[2];
            lastFiring[hand]=cache.hand[hand].flags[1];
        }
        WriteFireState(w,fault.load()?before:CombinedDualFireState(cache.hand[0],cache.hand[1]));
        *reinterpret_cast<int*>(w+0x16C)=mode;
        *reinterpret_cast<std::uint8_t*>(w+0x160)=cursor;
        cache.tick=GetTickCount64(); ++updates;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fault=true;scopedWeapon=scopedGun=0;scopedHand=-1;
        __try {
            WriteFireState(w,before); *reinterpret_cast<int*>(w+0x16C)=mode;
            *reinterpret_cast<std::uint8_t*>(w+0x160)=cursor;
            std::memcpy(reinterpret_cast<void*>(w+0x3FC),beforeAim,sizeof(beforeAim));
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    return result;
}
bool __fastcall UpdateDetour(void* self,float dt,void* data,const float* input) {
    const auto w=reinterpret_cast<std::uintptr_t>(self);
    // NPCs and other update paths do not take our mutex or touch their state.
    if(w!=requestedWeapon.load(std::memory_order_acquire) || !w) return originalUpdate(self,dt,data,input);
    ++requestedCalls;
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    lastCaller=caller-image;
    if(caller!=image+0x9A419F) {++wrongCaller;return originalUpdate(self,dt,data,input);}
    Request wanted;
    { std::lock_guard lock(inputMutex); wanted=request; }
    if(!wanted.active || w!=wanted.weapon || GetTickCount64()-wanted.tick>250 || fault.load() || scopedWeapon ||
        !std::isfinite(dt) || dt<0 || dt>0.1f) {++staleRequest;return originalUpdate(self,dt,data,input);}
    // Revalidate ownership and hand directions while owning the transaction.
    // Neither pistol may overwrite the other's target/trigger state in here.
    std::lock_guard lock(fireMutex);
    Pair pair{}; float native[2]{};
    if(!ReadPair(w,pair) || (reinterpret_cast<std::uintptr_t>(data)!=pair.gun[0] && reinterpret_cast<std::uintptr_t>(data)!=pair.gun[1])) {
        ++pairFailures;return originalUpdate(self,dt,data,input);
    }
    if(!Read(native,reinterpret_cast<std::uintptr_t>(input),sizeof(native)) || !std::isfinite(native[0]) || native[0]<0 || native[0]>1) {
        ++inputFailures;return originalUpdate(self,dt,data,input);
    }
    return RunPair(pair,wanted,dt,native);
}
}
bool InstallDualWieldHooks() {
#if JC3VR_HAVE_MINHOOK
    if(installed.load()) return true;
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    constexpr std::uint8_t updateBytes[]{0x48,0x8B,0xC4,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0x80,0,0,0};
    constexpr std::uint8_t stopBytes[]{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20};
    constexpr std::uint8_t projectileBytes[]{0x48,0x8B,0xC4,0x48,0x89,0x50,0x10,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55};
    constexpr std::uint8_t fireFromBytes[]{0x40,0x53,0x48,0x81,0xEC,0xB0,0,0,0,0x83,0xB9,0x24,0x04,0,0,0x0A};
    constexpr std::uint8_t updateCallBytes[]{0xE8,0x61,0xE9,0xFE,0xFF};
    constexpr std::uint8_t aimReadBytes[]{0x8B,0x83,0xFC,0x03,0,0};
    constexpr std::uint8_t modeReadBytes[]{0x8B,0x8B,0x6C,0x01,0,0,0x83,0xF9,0xFF};
    void* sites[]{reinterpret_cast<void*>(base+0x992B00),reinterpret_cast<void*>(base+0x965820),reinterpret_cast<void*>(base+0x927550)};
    if(!Guard(base+0x992B00,updateBytes,sizeof(updateBytes)) || !Guard(base+0x965820,stopBytes,sizeof(stopBytes)) ||
        !Guard(base+0x927550,projectileBytes,sizeof(projectileBytes)) ||
        !Guard(base+0x966940,fireFromBytes,sizeof(fireFromBytes)) ||
        !Guard(base+0x9A419A,updateCallBytes,sizeof(updateCallBytes)) ||
        !Guard(base+0x985AE4,aimReadBytes,sizeof(aimReadBytes)) ||
        !Guard(base+0x985C22,modeReadBytes,sizeof(modeReadBytes))) {
        TraceRuntime("dual wield: entry byte guard failed; native firing unchanged");return false;
    }
    void* detours[]{reinterpret_cast<void*>(&UpdateDetour),reinterpret_cast<void*>(&StopDetour),reinterpret_cast<void*>(&ProjectileDetour)};
    void** originals[]{reinterpret_cast<void**>(&originalUpdate),reinterpret_cast<void**>(&originalStop),reinterpret_cast<void**>(&originalProjectile)};
    int created=0; bool okay=true;
    for(;created<3;++created) if(MH_CreateHook(sites[created],detours[created],originals[created])!=MH_OK) { okay=false;break; }
    if(okay) for(int i=0;i<3;++i) if(MH_EnableHook(sites[i])!=MH_OK) { okay=false;break; }
    if(!okay) {
        for(int i=0;i<created;++i) { MH_DisableHook(sites[i]);MH_RemoveHook(sites[i]); }
        TraceRuntime("dual wield: hook transaction failed; native firing unchanged");return false;
    }
    nativeFireFrom=reinterpret_cast<FireFromFn>(base+0x966940);
    installed=true;
    TraceRuntime("dual wield: guarded per-gun update/stop/projectile hooks installed; scoped per-hand native aim targets; active only for fresh tracked local dual pair");
    return true;
#else
    return false;
#endif
}
std::uintptr_t GetDualWieldWeapon() {
    if(!installed.load() || fault.load()) return 0;
    const auto rico=VrBodyCharacter();std::uintptr_t weapon=0;Pair pair{};PairReject rejection=PairReject::Local;
    PairObservation observation{};
    const bool eligible=rico && Read(weapon,rico+0x998) && ReadPair(weapon,pair,&rejection,&observation);
    // At most one message per rejection category per process. This makes a
    // silent inactive prototype distinguishable from a failed firing hook.
    if(!eligible && weapon) {
        static std::atomic<unsigned> reported{0};
        const auto bit=1u<<static_cast<unsigned>(rejection);
        if(!(reported.fetch_or(bit)&bit)) {
            char detail[320]{};
            std::snprintf(detail,sizeof(detail),"; slot=%d state=%d mechanics_read=%d heat=%d release_flag=%d alt=%d force_timer=%.6g force_count=%d charge=%.6g",
                observation.slot,observation.state,observation.mechanicsRead,observation.heat,observation.forced,observation.alt,
                observation.forceTimer,observation.forceCount,observation.charge);
            TraceRuntime(std::string("dual wield: eligibility rejected: ")+RejectName(rejection)+detail);
        }
    }
    static bool wasEligible=false;
    if(eligible!=wasEligible) {
        TraceRuntime(eligible?"dual wield: fresh LEFT/RIGHT tracked gun pair eligible; independent dispatch available, hits unverified":"dual wield: pair/tracking/state unavailable; normal inputs restored");
        wasEligible=eligible;
    }
    return eligible?weapon:0;
}
void RecordDualGunPlacement(std::uintptr_t data,int hand) {
    if(hand<0 || hand>1) return;
    std::lock_guard lock(inputMutex);
    placements.Record(data,hand,GetTickCount64());
}
void RequestDualFire(std::uintptr_t weapon,bool active,float left,float right) {
    Request next{};next.weapon=weapon;next.active=active && weapon && std::isfinite(left) && std::isfinite(right);
    next.trigger[0]=std::clamp(left,0.0f,1.0f);next.trigger[1]=std::clamp(right,0.0f,1.0f);next.tick=GetTickCount64();
    std::lock_guard lock(inputMutex);request=next;
    requestedWeapon.store(next.active?weapon:0,std::memory_order_release);
    if(next.active) ++activeRequests;
}
void DualWieldPresentTick() {
    static ULONGLONG last=0;static bool reported=false;
    if(fault.load() && !reported) { reported=true;TraceRuntime("dual wield: transient-state fault; independent firing disabled for this process"); }
    static unsigned summaries=0;
    static std::uint64_t lastActivity=0;
    const auto activity=triggerCalls[0].load()+triggerCalls[1].load()+nativeVetoes.load();
    if(activeRequests.load() && summaries<12 && (summaries==0 || activity!=lastActivity) && GetTickCount64()-last>5000) {
        last=GetTickCount64();
        lastActivity=activity;
        ++summaries;
        TraceRuntime("dual wield: native paired updates="+std::to_string(updates.load())+" projectile calls left="+std::to_string(shots[0].load())+" right="+std::to_string(shots[1].load()));
        char text[640]{};
        std::snprintf(text,sizeof(text),"dual wield: dispatch requests=%llu calls=%llu caller_rva=%llX caller_reject=%llu stale_or_fault=%llu pair_reject=%llu input_reject=%llu hand_calls=%llu/%llu trigger_calls=%llu/%llu native_veto=%llu last_trigger=%.3f/%.3f native=%.3f trying=%u/%u firing=%u/%u",
            static_cast<unsigned long long>(activeRequests.load()),static_cast<unsigned long long>(requestedCalls.load()),
            static_cast<unsigned long long>(lastCaller.load()),static_cast<unsigned long long>(wrongCaller.load()),
            static_cast<unsigned long long>(staleRequest.load()),static_cast<unsigned long long>(pairFailures.load()),
            static_cast<unsigned long long>(inputFailures.load()),static_cast<unsigned long long>(handCalls[0].load()),
            static_cast<unsigned long long>(handCalls[1].load()),static_cast<unsigned long long>(triggerCalls[0].load()),
            static_cast<unsigned long long>(triggerCalls[1].load()),static_cast<unsigned long long>(nativeVetoes.load()),
            lastTrigger[0].load(),lastTrigger[1].load(),lastNativeInput.load(),lastTrying[0].load(),lastTrying[1].load(),
            lastFiring[0].load(),lastFiring[1].load());
        TraceRuntime(text);
    }
}
}
