#undef NDEBUG
#include "../src/game/physical_hijack.cpp"
#include "core/config.h"
#include <array>
#include <cassert>
#include <iostream>
namespace lab {
std::array<std::byte,0x3000> character{};
std::array<std::byte,0x1800> vehicle{};
std::array<std::byte,0x100> machine{},state{},rule{},node{},context{},tree{},header{},record{};
std::uintptr_t rules[2]{};
unsigned calls=0,logs=0;bool ruleFinished=true,reentrant=false,throwEvent=false,probeActive=false;
std::string lastTrace;
template<class T>void Put(std::uintptr_t p,const T& v){std::memcpy(reinterpret_cast<void*>(p),&v,sizeof(v));}
template<class T>std::uintptr_t P(T& x){return reinterpret_cast<std::uintptr_t>(x.data());}
void* __fastcall Vehicle(void*){return vehicle.data();}
bool __fastcall Finish(void*,void*){return ruleFinished;}
void __fastcall Event(void* self,const void* e) {
    assert(*static_cast<const unsigned*>(e)==jc3vr::kOpenEvent || *static_cast<const unsigned*>(e)==0);
    ++calls;
    if(throwEvent)RaiseException(0xE0424242,0,0,nullptr);
    if(reentrant) {
        assert(self==character.data());
        // Native re-entry cannot repeat delivery or escape the hold early.
        assert(!jc3vr::FinishDetour(nullptr,context.data()));
        auto in=jc3vr::input;jc3vr::SubmitPhysicalHijackInput(in);
    }
}
void Init(unsigned hash=0x56F1DBBB) {
    using namespace jc3vr;
    character={};vehicle={};machine={};state={};rule={};node={};context={};tree={};header={};record={};
    Put(P(character)+0x1818,reinterpret_cast<std::uintptr_t>(rules));
    Put(P(character)+0x1820,reinterpret_cast<std::uintptr_t>(rules)+16);rules[0]=P(rule);
    Put(P(rule)+0x48,P(machine));Put(P(machine)+0x18,P(state));Put(P(state)+0x10,hash);
    Put(P(character)+0x1A18,std::uintptr_t{0xABCD});Put(P(character)+0x1A20,P(node));Put(P(node)+0x34,1);
    Put(P(context)+0x20,P(character)+8);
    Put(P(vehicle)+8,reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+0x24E4348);
    Put(P(vehicle)+0x1670,P(header));Put(P(header)+8,P(tree));Put(P(header)+0x41,static_cast<unsigned char>(1));
    Put(P(tree),P(header));Put(P(tree)+0x10,P(header));Put(P(tree)+0x18,1);
    Put(P(tree)+0x20,P(record));Put(P(tree)+0x28,P(record)+0x50);
    originalEvent=Event;originalFinish=Finish;getVehicle=Vehicle;enabled=ready=true;Clear();
    input={true,GetTickCount64(),0,{0,0,-0.5f},{0,0,0}};
    calls=logs=0;ruleFinished=true;reentrant=throwEvent=probeActive=false;activationRecords=lastObservedPhase=0;lastTrace.clear();
}
void Arm() {unsigned event=jc3vr::kOpenEvent;jc3vr::EventDetour(character.data(),&event);assert(calls==0 && jc3vr::holding);}
void Pull() {
    using namespace jc3vr;auto i=input;i.now=GetTickCount64();i.grip=0;
    auto r=SubmitPhysicalHijackInput(i);assert(r.consume);
    i.grip=1;r=SubmitPhysicalHijackInput(i);assert(r.grabbed);
    gesture.began=i.now-150;i.hand[2]=-0.28f;
    r=SubmitPhysicalHijackInput(i);assert(r.opened && gesture.complete && calls==0);
}
}
namespace jc3vr {
std::uintptr_t VrBodyCharacter(){return lab::P(lab::character)+8;}
void TraceRuntime(std::string_view text){++lab::logs;lab::lastTrace=text;}
bool HijackStateProbeActive(){return lab::probeActive;}
}
int main() {
    using namespace jc3vr;using namespace lab;
    // Default-off and non-local/NPC/unrelated event paths remain native.
    Init();enabled=false;unsigned event=kOpenEvent;EventDetour(character.data(),&event);assert(calls==1 && !holding);
    Init();EventDetour(vehicle.data(),&event);assert(calls==1 && !holding);
    Init(0x1234);EventDetour(character.data(),&event);assert(calls==1 && !holding);
    Init();input.active=false;EventDetour(character.data(),&event);assert(calls==1 && !holding);
    Init();unsigned unrelated=0;EventDetour(character.data(),&unrelated);assert(calls==1 && !holding);
    Init();Put(P(record)+4,3u);EventDetour(character.data(),&event);assert(calls==1 && !holding); // destroyed/no door
    Init();Put(P(tree),P(tree));EventDetour(character.data(),&event);assert(calls==1 && !holding); // bounded corrupt tree
    assert(lastTrace.find("native-door-record-unavailable")!=std::string::npos);
    Init();probeActive=true;input.active=false;PhysicalHijackCharacterTick(P(character));
    assert(calls==0 && !holding && lastTrace.find("input_active=0")!=std::string::npos);
    Init();input.active=false;EventDetour(character.data(),&event);
    assert(calls==1 && !holding && lastTrace.find("tracked-input-unavailable")!=std::string::npos);
    // Actual state-gate adapter, not a second copy of its policy.
    Init();const auto oldCharacter=character;const auto oldVehicle=vehicle;Arm();
    EventDetour(character.data(),&event);assert(calls==0); // duplicates swallowed
    assert(!FinishDetour(nullptr,context.data())); // native finished, hand has not pulled
    Pull();reentrant=true;PhysicalHijackCharacterTick(P(character));assert(calls==1 && hold.eventDelivered);
    assert(FinishDetour(nullptr,context.data()));PhysicalHijackCharacterTick(P(character));assert(calls==1);
    assert(character==oldCharacter && vehicle==oldVehicle); // no native-memory writes
    // Native unfinished animation remains unfinished even after successful pull.
    Init();Arm();Pull();ruleFinished=false;assert(!FinishDetour(nullptr,context.data()));assert(calls==0);
    PhysicalHijackCharacterTick(P(character));assert(calls==1);assert(!FinishDetour(nullptr,context.data()));
    // Driver variant is scoped identically. No request is forced into another state.
    Init(0x86386B15);Arm();Pull();assert(FinishDetour(nullptr,context.data()) && calls==1);
    Init();Arm();hold.began=GetTickCount64()-12001;assert(FinishDetour(nullptr,context.data()) && calls==1);
    Init();Arm();input.active=false;PhysicalHijackCharacterTick(P(character));assert(calls==1);
    Init();Arm();input.now=GetTickCount64()-251;assert(FinishDetour(nullptr,context.data()) && calls==1);
    Init();Arm();Put(P(state)+0x10,0x1234u);PhysicalHijackCharacterTick(P(character));assert(!holding && calls==0);
    Init();Arm();Put(P(node)+0x34,2);PhysicalHijackCharacterTick(P(character));assert(!holding && calls==0);
    Init();Arm();Put(P(vehicle)+8,std::uintptr_t{1});PhysicalHijackCharacterTick(P(character));assert(!holding && calls==0);
    Init();Arm();Put(P(context)+0x20,std::uintptr_t{1});assert(FinishDetour(nullptr,context.data()) && calls==0);
    Init();Arm();Pull();throwEvent=true;PhysicalHijackCharacterTick(P(character));assert(calls==1 && !enabled); // never retry
    // Input-only rejection: held grip, sideways motion, tracking jumps/head motion.
    PhysicalHijackGesture g;PhysicalHijackInput i{true,1000,1,{0,0,-0.5f},{0,0,0}};
    assert(!g.Update(i).grabbed);i.grip=0;g.Update(i);i.grip=1;assert(g.Update(i).grabbed);
    i.now+=150;i.head[2]=0.5f;assert(!g.Update(i).opened); // moving head alone is not a pull
    i.hand[0]=0.3f;assert(!g.Update(i).opened && !g.grabbed);
    i.grip=0;g.Update(i);i.grip=1;i.hand[0]=0;g.Update(i);i.hand[2]=0.5f;assert(!g.Update(i).opened);
    assert(!Config{}.physicalHelicopterHijack);
    std::cout<<"Physical hijack scoped native event/gate/gesture/cancellation tests PASS; not a live VR result.\n";
}
