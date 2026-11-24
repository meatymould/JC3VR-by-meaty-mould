#include "game/physical_hijack.h"
#include "game/physical_hijack_policy.h"
#include "game/hijack_state_probe.h"
#include "game/vr_body_rig.h"
#include "game/native_signature.h"
#include "core/logging.h"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif
namespace jc3vr {
namespace {
constexpr std::uint32_t kOpenEvent=0x9272AEFA;
constexpr std::uintptr_t kEventRva=0x7FADD0,kFinishRva=0x78E540,kVehicleRva=0x7D5E30;
constexpr std::uint8_t kEventBytes[]{0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0xF8,0xF7,0xFF,0xFF};
constexpr std::uint8_t kFinishBytes[]{0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xCA,0xE8,0x8F,0xDD,0xC2,0xFF};
constexpr std::uint8_t kVehicleBytes[]{0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x30,0x48,0x8D,0x54,0x24,0x20};
using EventFn=void(__fastcall*)(void*,const void*);
using FinishFn=bool(__fastcall*)(void*,void*);
using VehicleFn=void*(__fastcall*)(void*);
EventFn originalEvent=nullptr;FinishFn originalFinish=nullptr;VehicleFn getVehicle=nullptr;
std::atomic<bool> enabled=false,ready=false,holding=false;
std::mutex mutex;
PhysicalHijackInput input{};
PhysicalHijackGesture gesture{};
struct Scope {
    std::uintptr_t character=0,vehicle=0,root=0,node=0;
    std::uint32_t state=0;int key=-1;
    bool operator==(const Scope&)const=default;
};
struct Hold {Scope scope{};std::uint64_t began=0;bool eventDelivered=false,delivering=false,announced=false;};
Hold hold{};
std::atomic<unsigned> activationRecords=0,lastObservedPhase=0;
void ActivationTrace(const char* reason,const Scope& scope,const PhysicalHijackInput& sample) {
    // At most32 lines per process, only a local open event or F3 opening phase.
    // No extra native calls and no change to the activation decision.
    if(activationRecords.fetch_add(1)>=32)return;
    const auto now=GetTickCount64();
    TraceRuntime(std::string("physical hijack activation: reason=")+reason+" state="+std::to_string(scope.state)+
        " key="+std::to_string(scope.key)+" input_active="+(sample.active?"1":"0")+
        " input_fresh="+(gesture.Fresh(sample,now)?"1":"0")+" input_age_ms="+std::to_string(now>=sample.now?now-sample.now:0));
}
bool Read(void* dst,const void* src,std::size_t n) {
    const auto p=reinterpret_cast<std::uintptr_t>(src);MEMORY_BASIC_INFORMATION m{};
    if(!p || p+n<p || VirtualQuery(src,&m,sizeof(m))!=sizeof(m) || m.State!=MEM_COMMIT ||
       (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || p+n>reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize)return false;
    __try {std::memcpy(dst,src,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T>bool Read(T& x,std::uintptr_t p){return Read(&x,reinterpret_cast<void*>(p),sizeof(x));}
bool Opening(std::uint32_t state){return state==0x56F1DBBB || state==0x86386B15;}
bool DoorExists(std::uintptr_t vehicle,int key) {
    // Bounded read-only equivalent of verified FC7540 -> FACB30. Native
    // returns true if a keyed group has at least one record not in state3.
    std::uintptr_t header=0,node=0,best=0;
    if(!Read(header,vehicle+0x1668+8) || !header || !Read(node,header+8))return false;
    best=header;bool terminal=false;
    for(int i=0;i<64;++i) {
        unsigned char nil=0;int nodeKey=0;
        if(!Read(nil,node+0x41))return false;
        if(nil){terminal=true;break;}
        if(!Read(nodeKey,node+0x18))return false;
        if(nodeKey>=key){best=node;if(!Read(node,node))return false;}
        else if(!Read(node,node+0x10))return false;
    }
    if(!terminal || best==header)return false;
    int found=-1;std::uintptr_t begin=0,end=0;
    if(!Read(found,best+0x18) || found!=key || !Read(begin,best+0x20) || !Read(end,best+0x28) ||
       !begin || end<=begin || (end-begin)%0x50 || (end-begin)/0x50>32)return false;
    for(auto p=begin;p<end;p+=0x50){unsigned state=3;if(!Read(state,p+4))return false;if(state!=3)return true;}
    return false;
}
std::uintptr_t Vehicle(std::uintptr_t p) {
    __try{return getVehicle?reinterpret_cast<std::uintptr_t>(getVehicle(reinterpret_cast<void*>(p))):0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
bool Current(std::uintptr_t p,Scope& s,const char** reason=nullptr) {
    auto why=[&](const char* text){if(reason)*reason=text;};
    why("not-local-character");
    const auto rico=VrBodyCharacter();if(!rico || p!=rico-8)return false;
    std::uintptr_t begin=0,end=0,rule=0,machine=0,state=0,table=0;
    s.character=p;
    why("rule-state-unreadable");
    if(!Read(begin,p+0x1818) || !Read(end,p+0x1820) || !begin || end<=begin || end-begin>16*64 || (end-begin)%16 ||
       !Read(rule,begin) || !rule || !Read(machine,rule+0x48) || !machine || !Read(state,machine+0x18) || !state ||
       !Read(s.state,state+0x10))return false;
    why("outside-opening-phase");if(!Opening(s.state))return false;
    why("graph-root-or-node-unavailable");
    if(!Read(s.root,p+0x1A18) || !s.root || !Read(s.node,p+0x1A20) || !s.node)return false;
    s.vehicle=Vehicle(p);
    why("helicopter-association-invalid");
    if(!s.vehicle || !Read(table,s.vehicle+8) || table!=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+0x24E4348)return false;
    // Exact priority/offsets from native open-event branch7FBBB6..7FBBFC.
    const std::uintptr_t slots[]{0x1A30,0x1A28,0x1A38,0x1A20};
    const std::uintptr_t offsets[]{0x2C,0x28,0x24,0x34};
    why("native-door-key-unreadable");
    for(int i=0;i<4;++i) {
        std::uintptr_t candidate=0;if(!Read(candidate,p+slots[i]))return false;
        if(candidate){if(!Read(s.key,candidate+offsets[i]))return false;break;}
    }
    why("native-door-key-out-of-range");if(s.key<0 || s.key>6)return false;
    why("native-door-record-unavailable");if(!DoorExists(s.vehicle,s.key))return false;
    why("context-valid");return true;
}
void Clear(){hold={};gesture.Reset();holding=false;}
bool ReplayOpen(std::uintptr_t p) {
    // The pinned open-event branch reads only its leading uint32 hash. Never
    // retain the original transient event pointer or replay any other event.
    struct {std::uint32_t hash=kOpenEvent;std::uint32_t unused[3]{};} event;
    __try{originalEvent(reinterpret_cast<void*>(p),&event);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void Deliver(const Scope& current,const char* reason,std::unique_lock<std::mutex>& lock) {
    // No project mutex held across native calls; a re-entrant condition must
    // remain held until the one dispatch has actually returned.
    hold.delivering=true;lock.unlock();
    Scope live{};const bool valid=Current(current.character,live) && live==current;
    const bool ok=valid && ReplayOpen(current.character);
    lock.lock();
    if(hold.scope==current){if(valid){hold.delivering=false;hold.eventDelivered=true;}else Clear();}
    if(!valid){TraceRuntime("physical hijack: scope changed before native dispatch; no stale replay");return;}
    TraceRuntime(std::string("physical hijack: native door event delivered reason=")+reason+" success="+(ok?"1":"0")+" key="+std::to_string(current.key));
    if(!ok)enabled=false; // no retry of a potentially partially executed event
}
void __fastcall EventDetour(void* self,const void* event) {
    std::uint32_t hash=0;
    if(enabled.load() && ready.load() && Read(hash,reinterpret_cast<std::uintptr_t>(event)) && hash==kOpenEvent) {
        Scope current{};const char* reason="unknown";
        if(Current(reinterpret_cast<std::uintptr_t>(self),current,&reason)) {
            std::lock_guard lock(mutex);const auto now=GetTickCount64();
            if(hold.scope.character && hold.scope==current)return; // duplicate native open event
            if(gesture.Fresh(input,now)) {
                Clear();hold.scope=current;hold.began=now;
                holding=true;
                TraceRuntime("physical hijack: deferred native open event; left grip/release then reach and pull20cm; state="+std::to_string(current.state)+" key="+std::to_string(current.key));
                return;
            }
            ActivationTrace("tracked-input-unavailable",current,input);
        } else if(current.character) {
            std::lock_guard lock(mutex);ActivationTrace(reason,current,input);
        }
    }
    originalEvent(self,event);
}
bool __fastcall FinishDetour(void* condition,void* context) {
    // Preserve native completion result; never force an unfinished rule true.
    const bool result=originalFinish(condition,context);
    if(!result || !enabled.load() || !ready.load() || !holding.load())return result;
    std::uintptr_t object=0;
    if(!Read(object,reinterpret_cast<std::uintptr_t>(context)+0x20) || !object || object!=VrBodyCharacter())return result;
    Scope current{};if(!Current(object-8,current))return result;
    std::unique_lock lock(mutex);
    if(!hold.scope.character || !(hold.scope==current) || hold.eventDelivered)return result;
    if(hold.delivering)return false;
    const auto now=GetTickCount64();
    if(now<hold.began || now-hold.began>=12000 || !gesture.Fresh(input,now)) {
        Deliver(current,"timeout-or-tracking-loss",lock);return result;
    }
    if(gesture.complete){Deliver(current,"physical-pull",lock);return result;}
    return false;
}
}
void ConfigurePhysicalHijack(bool value){enabled=value;}
PhysicalHijackFeedback SubmitPhysicalHijackInput(const PhysicalHijackInput& value) {
    if(!enabled.load() || !ready.load())return {};
    std::lock_guard lock(mutex);input=value;
    if(!hold.scope.character || hold.eventDelivered)return {};
    auto r=gesture.Update(value);r.ready=!hold.announced;hold.announced=true;return r;
}
void PhysicalHijackCharacterTick(std::uintptr_t character) {
    if(!enabled.load() || !ready.load())return;
    const bool observe=HijackStateProbeActive();
    if(!holding.load() && !observe)return;
    Scope current{};const char* reason="unknown";const bool valid=Current(character,current,&reason);
    std::unique_lock lock(mutex);
    if(observe && current.character) {
        const auto previous=lastObservedPhase.exchange(current.state);
        if(previous!=current.state && Opening(current.state))ActivationTrace(reason,current,input);
    }
    if(!hold.scope.character || character!=hold.scope.character)return;
    if(!valid || !(hold.scope==current)) {
        TraceRuntime("physical hijack: context changed; hold discarded without stale event replay");Clear();return;
    }
    if(hold.eventDelivered || hold.delivering)return;
    const auto now=GetTickCount64();
    if(gesture.complete)Deliver(current,"physical-pull",lock);
    else if(now<hold.began || now-hold.began>=12000 || !gesture.Fresh(input,now))Deliver(current,"timeout-or-tracking-loss",lock);
}
bool InstallPhysicalHijackHooks() {
    if(!enabled.load())return true;
    if(ready.load())return true;
#if JC3VR_HAVE_MINHOOK
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto match=[&](std::uintptr_t rva,const auto& bytes){return MatchNativeSignature(base+rva,bytes,sizeof(bytes),
        [](void* dst,const void* src,std::size_t n){return Read(dst,src,n);});};
    // Also bind the context getter and exact native open-event branch/caller,
    // not merely two plausible function prologues.
    constexpr std::uint8_t context[]{0x48,0x8B,0x41,0x20,0xC3};
    constexpr std::uint8_t caller[]{0x48,0x8B,0xD6,0x49,0x8B,0xCC,0xE8,0x1A,0xE5,0xFF,0xFF};
    constexpr std::uint8_t openCall[]{0xF3,0x0F,0x10,0x1D,0x3D,0x26,0xB0,0x01,0x41,0xB0,0x01,0x48,0x8B,0xCB,0xE8,0xFE,0xB6,0x7C,0x00};
    constexpr std::uint8_t openCase[]{0x3D,0xFA,0xAE,0x72,0x92,0x0F,0x87,0x4E,0x03,0x00,0x00,0x0F,0x84,0xB2,0x02,0x00,0x00};
    constexpr std::uint8_t doorExists[]{0x48,0x81,0xC1,0x68,0x16,0x00,0x00,0xE9,0xE4,0x55,0xFE,0xFF};
    if(!match(kEventRva,kEventBytes) || !match(kFinishRva,kFinishBytes) || !match(kVehicleRva,kVehicleBytes) ||
       !match(0x3BC2E0,context) || !match(0x7FC8AB,caller) || !match(0x7FBC0F,openCall) || !match(0x7FB8CE,openCase) || !match(0xFC7540,doorExists)) {
        enabled=false;TraceRuntime("physical hijack disabled: pinned native signature mismatch");return false;
    }
    getVehicle=reinterpret_cast<VehicleFn>(base+kVehicleRva);
    auto* eventTarget=reinterpret_cast<void*>(base+kEventRva);auto* finishTarget=reinterpret_cast<void*>(base+kFinishRva);
    bool eventCreated=false,finishCreated=false;
    auto status=MH_CreateHook(eventTarget,reinterpret_cast<void*>(&EventDetour),reinterpret_cast<void**>(&originalEvent));
    eventCreated=status==MH_OK;
    if(eventCreated){status=MH_CreateHook(finishTarget,reinterpret_cast<void*>(&FinishDetour),reinterpret_cast<void**>(&originalFinish));finishCreated=status==MH_OK;}
    if(finishCreated)status=MH_EnableHook(eventTarget);
    if(status==MH_OK)status=MH_EnableHook(finishTarget);
    if(status!=MH_OK) {
        if(eventCreated){MH_DisableHook(eventTarget);MH_RemoveHook(eventTarget);}
        if(finishCreated){MH_DisableHook(finishTarget);MH_RemoveHook(finishTarget);}
        enabled=false;TraceRuntime("physical hijack disabled: hook installation failed status="+std::to_string(status));return false;
    }
    ready=true;TraceRuntime("physical hijack: scoped door-event/animation-finish hooks ready; prototype, no continuous hinge control");return true;
#else
    enabled=false;return false;
#endif
}
}
