#include "game/weapon_attachment_probe.h"
#include "game/vr_body_rig.h"
#include "core/logging.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif
namespace jc3vr {
namespace {
constexpr unsigned kCapacity=96;
constexpr ULONGLONG kDuration=8000;
struct Snapshot {
    std::uintptr_t data=0,parent=0,animated=0,model=0,rules[4]{},models[8]{};
    unsigned ruleCount=0,valid=0; int state=-1;
    float t0[16]{},t1[16]{};
};
struct Record { Snapshot before,after;LARGE_INTEGER qpc{};DWORD thread=0;unsigned phase=0,slot=~0u; };
std::array<Record,kCapacity> records;
unsigned count=0,dropped=0;
std::atomic<ULONGLONG> until=0;
std::atomic_flag busy=ATOMIC_FLAG_INIT;
struct Lease {std::uintptr_t data=0;unsigned phase=0;ULONGLONG at=0;};
std::array<Lease,16> leases;
using RenderFn=void(__fastcall*)(void*,void*);
RenderFn originalRender=nullptr;
bool renderInstalled=false,pending=false,exportPending=false;
ULONGLONG pendingUntil=0;

bool Read(void* out,std::uintptr_t p,std::size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if(!p || p+n<p || VirtualQuery(reinterpret_cast<void*>(p),&m,sizeof(m))!=sizeof(m) ||
       m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
       p+n>reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize) return false;
    __try {std::memcpy(out,reinterpret_cast<void*>(p),n);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
template<class T> bool Read(T& out,std::uintptr_t p) {return Read(&out,p,sizeof(out));}
bool LocalWeapon(std::uintptr_t w) {
    const auto rico=VrBodyCharacter();if(!rico || !w)return false;
    for(int slot:{0,1,6}) {
        std::uintptr_t held=0;
        if(Read(held,rico+0x998+slot*16) && held==w)return true;
    }
    return false;
}
bool Take(std::uintptr_t data,Snapshot& s) {
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t table=0;
    if(!Read(table,data) || table!=base+0x23F4750 || !Read(s.parent,data+0xC0) || !LocalWeapon(s.parent) ||
       !Read(s.state,s.parent+0x424) || (s.state!=1 && s.state!=2))return false;
    s.data=data;
    if(Read(s.t0,data+0x158,64))s.valid|=1;
    if(Read(s.t1,data+0x198,64))s.valid|=2;
    Read(s.animated,data+0x128);Read(s.model,data+0x130);
    if(s.animated) {
        // +494690 walks animation RULE systems at +98..+A0, not child models.
        // Model slots +200 are separately verified by native +4941C1.
        std::uintptr_t begin=0,end=0;
        if(Read(begin,s.animated+0x98) && Read(end,s.animated+0xA0) && begin && end>=begin &&
           (end-begin)%16==0 && end-begin<=64*16) {
            s.ruleCount=static_cast<unsigned>((end-begin)/16);
            for(unsigned i=0;i<s.ruleCount && i<4;++i)Read(s.rules[i],begin+i*16);
            s.valid|=4;
        }
        if(Read(s.models,s.animated+0x200,sizeof(s.models)))s.valid|=8;
    }
    return (s.valid&3)==3;
}
void Publish(const Snapshot& before,const Snapshot& after,unsigned phase,unsigned slot) {
    if(!WeaponAttachmentProbeActive() || busy.test_and_set(std::memory_order_acquire))return;
    if(!WeaponAttachmentProbeActive()) {busy.clear(std::memory_order_release);return;}
    const auto now=GetTickCount64();
    Lease* lease=&leases[0];bool found=false;
    for(auto& entry:leases) {
        if(entry.data==after.data && entry.phase==phase) {lease=&entry;found=true;break;}
        if(entry.at<lease->at)lease=&entry;
    }
    if(!found || now-lease->at>=400) {
        *lease={after.data,phase,now};
        if(count<records.size()) {
            auto& r=records[count++];r={};r.before=before;r.after=after;r.phase=phase;r.slot=slot;
            QueryPerformanceCounter(&r.qpc);r.thread=GetCurrentThreadId();
        } else ++dropped;
    }
    busy.clear(std::memory_order_release);
}
void __fastcall RenderDetour(void* gameObject,void* context) {
    // Verified native ABI: game-object subobject in RCX, render context in RDX;
    // self is weapon+8. Original called exactly once, no render state changed.
    if(!WeaponAttachmentProbeActive()) {originalRender(gameObject,context);return;}
    std::array<Snapshot,4> before{};unsigned n=0;
    const auto self=reinterpret_cast<std::uintptr_t>(gameObject);
    if(WeaponAttachmentProbeActive() && self>=8 && LocalWeapon(self-8)) {
        std::uintptr_t begin=0,end=0;
        if(Read(begin,self-8+0x568) && Read(end,self-8+0x570) && begin && end>=begin &&
           (end-begin)%8==0 && end-begin<=4*8) {
            for(auto p=begin;p<end;++n,p+=8) {
                std::uintptr_t data=0;
                if(!Read(data,p) || !Take(data,before[n]))before[n]={};
            }
        }
    }
    originalRender(gameObject,context);
    for(unsigned i=0;i<n;++i) if(before[i].data) {
        Snapshot after{};
        if(Take(before[i].data,after))Publish(before[i],after,1,~0u);
    }
}
bool InstallObserver() {
#if JC3VR_HAVE_MINHOOK
    if(renderInstalled)return true;
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    constexpr unsigned char prologue[]{0x4C,0x8B,0xDC,0x55,0x53,0x56,0x41,0x54,0x48,0x8D,0x6C,0x24,0x88,0x48,0x81,0xEC,0x78,0x01,0,0};
    unsigned char actual[sizeof(prologue)]{};
    std::uintptr_t virtualTarget=0;
    if(!Read(actual,base+0x966380,sizeof(actual)) || std::memcmp(actual,prologue,sizeof(actual)) ||
       !Read(virtualTarget,base+0x23E2EC8+0x70) || virtualTarget!=base+0x966380) {
        TraceRuntime("weapon attachment probe: native UpdateRender guard rejected; no observer installed");return false;
    }
    auto target=reinterpret_cast<void*>(base+0x966380);
    const auto created=MH_CreateHook(target,reinterpret_cast<void*>(&RenderDetour),reinterpret_cast<void**>(&originalRender));
    if(created!=MH_OK)return false;
    if(MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);return false;}
    renderInstalled=true;return true;
#else
    return false;
#endif
}
bool Export() {
    if(busy.test_and_set(std::memory_order_acquire))return false;
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    TraceRuntime("weapon attachment probe BEGIN records="+std::to_string(count)+" overflow="+std::to_string(dropped)+
        " qpc_frequency="+std::to_string(frequency.QuadPart)+"; phase0=native-placement/VR-placement (component metadata sampled AFTER placement) phase1=native-weapon-UpdateRender; observation ONLY");
    for(unsigned i=0;i<count;++i) {
        const auto& r=records[i];char line[1400]{};
        std::snprintf(line,sizeof(line),"weapon attachment sample phase=%u slot=%u qpc=%lld tid=%lu data=%llX parent=%llX state=%d anim=%llX model=%llX rule_count=%u rule_ids=%llX/%llX/%llX/%llX before_flags=%u after_flags=%u before_t=(%.5f,%.5f,%.5f) after_t=(%.5f,%.5f,%.5f) before_z=(%.5f,%.5f,%.5f) after_z=(%.5f,%.5f,%.5f)",
            r.phase,r.slot,r.qpc.QuadPart,r.thread,static_cast<unsigned long long>(r.after.data),static_cast<unsigned long long>(r.after.parent),r.after.state,
            static_cast<unsigned long long>(r.after.animated),static_cast<unsigned long long>(r.after.model),r.after.ruleCount,
            static_cast<unsigned long long>(r.after.rules[0]),static_cast<unsigned long long>(r.after.rules[1]),
            static_cast<unsigned long long>(r.after.rules[2]),static_cast<unsigned long long>(r.after.rules[3]),r.before.valid,r.after.valid,
            r.before.t1[12],r.before.t1[13],r.before.t1[14],r.after.t1[12],r.after.t1[13],r.after.t1[14],
            r.before.t1[8],r.before.t1[9],r.before.t1[10],r.after.t1[8],r.after.t1[9],r.after.t1[10]);
        TraceRuntime(line);
        const auto matrix=[&](const char* name,const float* m) {
            std::snprintf(line,sizeof(line),"weapon attachment matrix record=%u %s %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g",
                i,name,m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]);
            TraceRuntime(line);
        };
        matrix("before_t0",r.before.t0);matrix("before_t1",r.before.t1);
        matrix("after_t0",r.after.t0);matrix("after_t1",r.after.t1);
        std::snprintf(line,sizeof(line),"weapon attachment model_slots record=%u before=%llX/%llX/%llX/%llX/%llX/%llX/%llX/%llX after=%llX/%llX/%llX/%llX/%llX/%llX/%llX/%llX",
            i,static_cast<unsigned long long>(r.before.models[0]),static_cast<unsigned long long>(r.before.models[1]),
            static_cast<unsigned long long>(r.before.models[2]),static_cast<unsigned long long>(r.before.models[3]),
            static_cast<unsigned long long>(r.before.models[4]),static_cast<unsigned long long>(r.before.models[5]),
            static_cast<unsigned long long>(r.before.models[6]),static_cast<unsigned long long>(r.before.models[7]),
            static_cast<unsigned long long>(r.after.models[0]),static_cast<unsigned long long>(r.after.models[1]),
            static_cast<unsigned long long>(r.after.models[2]),static_cast<unsigned long long>(r.after.models[3]),
            static_cast<unsigned long long>(r.after.models[4]),static_cast<unsigned long long>(r.after.models[5]),
            static_cast<unsigned long long>(r.after.models[6]),static_cast<unsigned long long>(r.after.models[7]));
        TraceRuntime(line);
    }
    TraceRuntime("weapon attachment probe END; no transforms/animations modified");
    count=dropped=0;busy.clear(std::memory_order_release);return true;
}
}
bool WeaponAttachmentProbeActive() {const auto end=until.load(std::memory_order_acquire);return end && GetTickCount64()<end;}
void ObserveWeaponAttachmentPlacement(std::uintptr_t data,std::uintptr_t owner,unsigned slot,const float native[32]) {
    if(owner!=VrBodyCharacter() || !WeaponAttachmentProbeActive())return;
    Snapshot after{};if(!Take(data,after))return;
    auto before=after;std::memcpy(before.t0,native,64);std::memcpy(before.t1,native+16,64);Publish(before,after,0,slot);
}
void WeaponAttachmentProbePresent() {
    static bool f3Was=false;const bool f3=(GetAsyncKeyState(VK_F3)&0x8000)!=0;
    if(f3 && !f3Was) {pending=true;pendingUntil=GetTickCount64()+10000;}
    f3Was=f3;
    const auto now=GetTickCount64();
    if(until.load() && now>=until.load()) {until=0;exportPending=true;}
    if(exportPending) {if(Export())exportPending=false;else return;}
    // Never install at startup or during a menu based on a blind pointer.
    if(!pending || until.load())return;
    if(now>=pendingUntil) {pending=false;TraceRuntime("weapon attachment probe: no ready tracked local world weapon; capture not armed");return;}
    float grip[3]{},forward[3]{},down[3]{};
    if(!VrBodyCharacter() || (!GetVrBodyHandTarget(1,grip,forward,down) && !GetVrBodyHandTarget(0,grip,forward,down)))return;
    bool ready=false;const auto rico=VrBodyCharacter();
    for(int slot:{0,1,6}) {
        std::uintptr_t held=0,begin=0,end=0,data=0;Snapshot snapshot{};
        if(Read(held,rico+0x998+slot*16) && held && Read(begin,held+0x568) && Read(end,held+0x570) &&
           begin && end>begin && (end-begin)%8==0 && end-begin<=4*8 && Read(data,begin) && Take(data,snapshot))ready=true;
    }
    if(!ready)return;
    if(!InstallObserver()) {pending=false;TraceRuntime("weapon attachment probe: observer install failed; native path unchanged");return;}
    if(busy.test_and_set(std::memory_order_acquire))return;
    count=dropped=0;leases={};until.store(now+kDuration,std::memory_order_release);pending=false;
    busy.clear(std::memory_order_release);
    TraceRuntime("weapon attachment probe armed for8s by F3; read-only, max96 records; repeat F3 for a new capture");
}
}
