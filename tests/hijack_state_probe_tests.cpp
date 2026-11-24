#undef NDEBUG
#include "../src/game/hijack_state_probe.cpp"
#include <cassert>
#include <iostream>
#include <thread>
namespace lab {
std::array<std::byte,0x3000> character{},other{};
std::array<std::byte,0x1600> vehicle{};
std::array<std::byte,0x100> rule{},machine{},state{};
std::uintptr_t rules[2]{};unsigned reports=0;
template<class T>void Put(std::uintptr_t p,const T& v){std::memcpy(reinterpret_cast<void*>(p),&v,sizeof(v));}
std::uintptr_t C(){return reinterpret_cast<std::uintptr_t>(character.data());}
std::uintptr_t V(){return reinterpret_cast<std::uintptr_t>(vehicle.data());}
void Init() {
    using namespace jc3vr;
    character={};vehicle={};other={};rule={};machine={};state={};
    const auto r=reinterpret_cast<std::uintptr_t>(rule.data()),m=reinterpret_cast<std::uintptr_t>(machine.data());
    const auto s=reinterpret_cast<std::uintptr_t>(state.data());rules[0]=r;
    Put(C()+0x1818,reinterpret_cast<std::uintptr_t>(rules));Put(C()+0x1820,reinterpret_cast<std::uintptr_t>(rules)+16);
    Put(r+0x48,m);Put(m+0x18,s);Put(s+0x10,0x12345678u);
    Put(C()+0x1A08+0x10,std::uintptr_t{0xABCD});Put(C()+0x1A08+0x18,std::uintptr_t{0x5678});
    Put(C()+0x1A08+0x188,0.25f);Put(C()+0x1A08+0x18C,0.75f);
    Put(V()+8,reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+0x24E4348);
    Put(V()+0xB28+10*0x30,std::uintptr_t{0x5678});
    busy.clear();count=overflow=0;characterOwner=targetVehicle=0;ConfigureHijackStateProbe(false);reports=0;
}
}
namespace jc3vr {void TraceRuntime(std::string_view){++lab::reports;}}
int main() {
    using namespace jc3vr;using namespace lab;Init();
    ObserveHijackState(C(),V());assert(count==0 && reports==0);
    auto s=Take(C(),V());assert(s.valid==31 && s.stateHash==0x12345678 && s.seat==10);
    assert(s.root==0xABCD && s.node==0x5678 && s.lineFraction==0.25f && s.moveFraction==0.75f);
    const auto oldCharacter=character;const auto oldVehicle=vehicle;
    ConfigureHijackStateProbe(true);until=GetTickCount64()+12000;
    ObserveHijackState(C(),0);assert(count==0); // won't start outside helicopter
    ObserveHijackState(C(),V());assert(count==1 && characterOwner==C());
    ObserveHijackState(C(),0);assert(count==2 && samples[1].vehicle==0 && samples[1].seat==-1);
    ObserveHijackState(reinterpret_cast<std::uintptr_t>(other.data()),V());assert(count==2);
    assert(character==oldCharacter && vehicle==oldVehicle);
    for(int i=0;i<600;++i)ObserveHijackState(C(),V());assert(count==512 && overflow==90);
    until=0;ObserveHijackState(C(),V());assert(count==512);
    busy.test_and_set();assert(!Export() && count==512);busy.clear();assert(Export() && count==0 && reports==514);
    Put(V()+8,std::uintptr_t{1});assert(Take(C(),V()).vehicle==0);
    assert(!Read(s.state,1));
    const auto page=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_NOACCESS);assert(page);
    assert(!Read(s.state,reinterpret_cast<std::uintptr_t>(page)));VirtualFree(page,0,MEM_RELEASE);
    Init();ConfigureHijackStateProbe(true);until=GetTickCount64()+12000;
    std::array<std::thread,4> workers;
    for(auto& worker:workers)worker=std::thread([]{for(int i=0;i<200;++i)ObserveHijackState(C(),V());});
    for(auto& worker:workers)worker.join();
    assert(count>0 && count<=512 && count+overflow<=800);
    for(unsigned i=1;i<count;++i)assert(samples[i].qpc.QuadPart>=samples[i-1].qpc.QuadPart);
    std::cout<<"Hijack read-only chain/seat/ownership/bounds/export PASS; not a live hijack test.\n";
}
