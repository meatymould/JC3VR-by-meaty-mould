#include "game/hijack_state_probe.h"
#include "core/logging.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
namespace jc3vr {
namespace {
struct Sample {
    LARGE_INTEGER qpc{};DWORD tid=0;unsigned valid=0,stateHash=0;
    std::uintptr_t character=0,vehicle=0,root=0,node=0,state=0;
    float lineFraction=0,moveFraction=0;int seat=-1;
};
std::array<Sample,512> samples;
unsigned count=0,overflow=0;
std::uintptr_t characterOwner=0,targetVehicle=0;
std::atomic<bool> enabled=false;
std::atomic<ULONGLONG> until=0;
std::atomic_flag busy=ATOMIC_FLAG_INIT;
bool exportPending=false;
bool Read(void* out,std::uintptr_t p,std::size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if(!p || p+n<p || VirtualQuery(reinterpret_cast<void*>(p),&m,sizeof(m))!=sizeof(m) ||
       m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
       p+n>reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize)return false;
    __try {std::memcpy(out,reinterpret_cast<const void*>(p),n);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T>bool Read(T& v,std::uintptr_t p){return Read(&v,p,sizeof(v));}
Sample Take(std::uintptr_t character,std::uintptr_t vehicle) {
    Sample s{};s.character=character;s.vehicle=vehicle;
    // Offsets from Character base P (NOT our discovered Rico O=P+8).
    // Chains independently checked in native77F080/F24FB0/7D5D90.
    std::uintptr_t begin=0,end=0,rule=0,machine=0;
    if(Read(begin,character+0x1818) && Read(end,character+0x1820) && begin &&
       end>begin && end-begin<=16*64 && (end-begin)%16==0 &&
       Read(rule,begin) && rule && Read(machine,rule+0x48) && machine &&
       Read(s.state,machine+0x18) && s.state && Read(s.stateHash,s.state+0x10))s.valid|=1;
    if(Read(s.root,character+0x1A08+0x10))s.valid|=2;
    if(Read(s.node,character+0x1A08+0x18))s.valid|=4;
    // Raw candidate progress values, not a phase classifier or write target.
    if(Read(s.lineFraction,character+0x1A08+0x188) && Read(s.moveFraction,character+0x1A08+0x18C))s.valid|=8;
    if(vehicle) {
        std::uintptr_t table=0;
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::array<std::byte,42*0x30> seats{};
        if(Read(table,vehicle+8) && table==base+0x24E4348 && Read(seats.data(),vehicle+0xB28,seats.size())) {
            s.valid|=16;
            if(s.node) for(int i=0;i<42;++i) {
                std::uintptr_t node=0;std::memcpy(&node,seats.data()+i*0x30,sizeof(node));
                if(node==s.node){s.seat=i;break;}
            }
        } else s.vehicle=0;
    }
    QueryPerformanceCounter(&s.qpc);s.tid=GetCurrentThreadId();return s;
}
bool Export() {
    if(busy.test_and_set(std::memory_order_acquire))return false;
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    TraceRuntime("hijack state BEGIN count="+std::to_string(count)+" overflow="+std::to_string(overflow)+
        " qpc_frequency="+std::to_string(frequency.QuadPart)+"; READ ONLY; fractions are candidates; flags1=state2=root4=node8=fractions16=vehicle-seats");
    for(unsigned i=0;i<count;++i) {
        const auto& s=samples[i];char line[600]{};
        std::snprintf(line,sizeof(line),"hijack state sample=%u qpc=%lld tid=%lu flags=%u character=%llX vehicle=%llX root=%llX node=%llX state=%llX hash=%08X seat=%d line_candidate=%.9g move_candidate=%.9g",
            i,s.qpc.QuadPart,s.tid,s.valid,static_cast<unsigned long long>(s.character),static_cast<unsigned long long>(s.vehicle),
            static_cast<unsigned long long>(s.root),static_cast<unsigned long long>(s.node),static_cast<unsigned long long>(s.state),s.stateHash,s.seat,s.lineFraction,s.moveFraction);
        TraceRuntime(line);
    }
    TraceRuntime("hijack state END; no native calls replayed, no game data changed");
    count=overflow=0;characterOwner=targetVehicle=0;busy.clear(std::memory_order_release);return true;
}
}
bool HijackStateProbeActive(){const auto end=until.load(std::memory_order_acquire);return enabled.load() && end && GetTickCount64()<end;}
void ConfigureHijackStateProbe(bool value){enabled=value;until=0;}
void ObserveHijackState(std::uintptr_t character,std::uintptr_t helicopter) {
    if(!HijackStateProbeActive() || !character)return;
    if(busy.test_and_set(std::memory_order_acquire))return;
    if(!HijackStateProbeActive()){busy.clear(std::memory_order_release);return;}
    // Serialize snapshot timestamps as well as insertion. Concurrent callbacks
    // must not append an older pre-lock sample after a newer one.
    const auto sample=Take(character,helicopter);
    // Start only from a validated helicopter. Afterwards retain detachment and
    // seat transition samples; never follow a new unrelated character.
    if(!targetVehicle && sample.vehicle && (sample.valid&16)) {targetVehicle=sample.vehicle;characterOwner=character;}
    if(targetVehicle && character==characterOwner) {
        if(count<samples.size())samples[count++]=sample;else ++overflow;
    }
    busy.clear(std::memory_order_release);
}
void HijackStateProbePresent() {
    if(!enabled.load())return;
    static bool wasF3=false;
    const bool f3=(GetAsyncKeyState(VK_F3)&0x8000)!=0;const auto now=GetTickCount64();
    if(until.load() && now>=until.load()){until=0;exportPending=true;}
    if(exportPending){if(Export())exportPending=false;else return;}
    if(f3 && !wasF3 && !until.load() && !busy.test_and_set(std::memory_order_acquire)) {
        count=overflow=0;characterOwner=targetVehicle=0;until.store(now+12000,std::memory_order_release);
        busy.clear(std::memory_order_release);
        TraceRuntime("hijack state armed by F3 for12s; perform NORMAL native Y hijack; observation only");
    }
    wasF3=f3;
}
}
