#include "game/hud_snapshot.h"
#include "core/logging.h"
#include <Windows.h>
#include <mutex>
#include <atomic>
#include <string>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif
namespace jc3vr {
namespace {
std::mutex mutex;
HudSnapshot snapshot;
std::atomic<bool> installed=false;
std::atomic<bool> suppressDamage=true;
std::atomic<ULONGLONG> vrFrameAt=0;
bool Read(void* out,const void* source,std::size_t size) {
    __try {std::memcpy(out,source,size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ReadName(const char* source,char(&name)[192]) {
    for(unsigned i=0;i<sizeof(name);++i) {
        if(!Read(name+i,source+i,1))return false;
        if(!name[i])return true;
    }
    return false;
}
using InvokeFn=bool(__fastcall*)(void*,const char*,const HudValue*,unsigned,void*,float);
InvokeFn original=nullptr;
#if JC3VR_HAVE_MINHOOK
using DamageApplyFn=unsigned(__fastcall*)(void*,void*,void*,void*,unsigned);
DamageApplyFn originalDamage=nullptr;
bool FreshVrDamageFrame() {const auto at=vrFrameAt.load();return at && GetTickCount64()-at<250;}
void DamageReport(const char* name) {
    static std::atomic<unsigned> reports=0;
    if(reports.fetch_add(1)<8) TraceRuntime(std::string("VR comfort: suppressed ")+name+"; health/gameplay unchanged");
}
unsigned __fastcall DamageDetour(void* self,void* context,void* effects,void* targets,unsigned input) {
    if(SuppressVrDamage(suppressDamage.load(),FreshVrDamageFrame(),true)) {
        DamagePresentationParameters parameters{};
        if(effects && Read(&parameters,effects,sizeof(parameters))) {
            auto quiet=ZeroDamagePresentation(parameters);
            if(std::isfinite(parameters.intensity) && parameters.intensity>.001f)
                DamageReport("PlayerDamageEffect red vignette (native zero-intensity path)");
            // ALWAYS execute original. It initializes self+0x40 and retains
            // native no-effect semantics; do not bypass its state machinery.
            return originalDamage(self,context,&quiet,targets,input);
        }
    }
    return originalDamage(self,context,effects,targets,input);
}
#endif
bool __fastcall InvokeDetour(void* hud,const char* method,const HudValue* args,unsigned count,void* result,float timeout) {
    std::uintptr_t table=0,localHud=0;char name[192]{};HudValue values[9]{};
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    bool observed=Read(&localHud,reinterpret_cast<void*>(base+0x2F38820),sizeof(localHud)) &&
        hud && reinterpret_cast<std::uintptr_t>(hud)==localHud && Read(&table,hud,sizeof(table)) && table==base+0x24BDF78 &&
        method && count<=9 && ReadName(method,name);
    if(observed) {
        const std::string_view path(name);
        const auto dot=path.find_last_of('.');const auto leaf=path.substr(dot==path.npos?0:dot+1);
#if JC3VR_HAVE_MINHOOK
        // Omit incoming-damage art BEFORE Scaleform renders it and before
        // either wrist relay samples it. Native event routines, health/audio,
        // screen-grab timer and caller argument cleanup remain unchanged.
        if(SuppressHudDamageVisual(leaf,suppressDamage.load(),FreshVrDamageFrame(),observed)) {
            DamageReport(leaf=="UpdateHealth"?"local HUD health damage paint/flare":
                leaf=="UpdateCharacterDmgIndicators"?"local HUD incoming damage direction indicators":
                "local HUD OnOmniDamage flash (UI invocation only)");
            return true;
        }
#endif
        observed=leaf=="OnSelectWeapon" || leaf=="OnSetSelectedMagazineAmmo" ||
            leaf=="OnSetSelectedInventoryAmmo" || leaf=="UpdateHealth";
        if(observed && count)observed=args && Read(values,args,count*sizeof(HudValue));
    }
    const bool okay=original(hud,method,args,count,result,timeout); // never suppress/alter game UI
    if(observed && okay) {
        bool changed=false;
        {std::lock_guard lock(mutex);changed=ApplyHudEvent(snapshot,name,values,count);}
        static std::atomic<unsigned> reports=0;
        if(reports.fetch_add(1)<32) {
            std::string message="HUD data callback: "+std::string(name)+" argc="+std::to_string(count)+" accepted="+std::to_string(changed);
            for(unsigned i=0;i<count;++i) {
                double n=0;
                if(HudNumber(values[i],n))message+=" arg"+std::to_string(i)+"="+std::to_string(n);
            }
            TraceRuntime(message);
        }
    }
    return okay;
}
}
HudSnapshot GetHudSnapshot() {std::lock_guard lock(mutex);return snapshot;}
void ConfigureVrDamageSuppression(bool enabled) {suppressDamage=enabled;}
void PublishHudVrFrame(bool active) {vrFrameAt=active?GetTickCount64():0;}
bool InstallVrDamageHooks() {
#if JC3VR_HAVE_MINHOOK
    if(!suppressDamage.load()) return false;
    static bool damageInstalled=false;
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
    // Guard the parameter layout and original zero-intensity branch, not
    // merely the entry point. A different effect layout must fail closed.
    const unsigned char loadBytes[]{0xf3,0x41,0x0f,0x10,0x80,0x40,0x01,0,0};
    const unsigned char branchBytes[]{0xf3,0x0f,0x11,0x41,0x40,0x0f,0x2f,0xc6,0x0f,0x86,0xe7,0x01,0,0};
    unsigned char load[sizeof(loadBytes)]{},branch[sizeof(branchBytes)]{};
    if(!Read(load,reinterpret_cast<void*>(base+0xF7725),sizeof(load)) ||
       !Read(branch,reinterpret_cast<void*>(base+0xF774B),sizeof(branch)) ||
       std::memcmp(load,loadBytes,sizeof(load)) || std::memcmp(branch,branchBytes,sizeof(branch))) {
        TraceRuntime("VR damage comfort disabled: parameter/zero-effect branch mismatch");return false;
    }
    // Exact installed Apply reads R8+140 only, initializes self+40, then
    // branches to its own native zero-effect path. Preserve that execution.
    const unsigned char damageBytes[]{0x40,0x55,0x56,0x57,0x48,0x81,0xec,0x80,0,0,0,0x48,0xc7,0x44,0x24,0x38,0xfe,0xff,0xff,0xff};
    const auto hook=[&](std::uintptr_t rva,const unsigned char* expected,unsigned size,void* detour,void** originalFn,bool& ready) {
        if(ready)return;
        auto* target=reinterpret_cast<void*>(base+rva);unsigned char bytes[32]{};
        if(size>sizeof(bytes) || !Read(bytes,target,size) || std::memcmp(bytes,expected,size)) {
            TraceRuntime("VR damage hook disabled: exact executable bytes mismatch at "+std::to_string(rva));return;
        }
        if(MH_CreateHook(target,detour,originalFn)!=MH_OK)return;
        if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);return;}
        ready=true;TraceRuntime("VR damage presentation hook installed at "+std::to_string(rva)+"; inactive/stale XR forwards original");
    };
    hook(0xF76E0,damageBytes,sizeof(damageBytes),reinterpret_cast<void*>(&DamageDetour),reinterpret_cast<void**>(&originalDamage),damageInstalled);
    // No E2CDB0 detour: its original side effects now stay intact.
    return damageInstalled;
#else
    return false;
#endif
}
bool InstallHudSnapshotHook() {
    if(installed.load())return true;
#if JC3VR_HAVE_MINHOOK
    // Live CHUDUI vtable+0x70 = CUIBase::Invoke. It dispatches loaded HUD
    // display-object methods through ObjectInterface::Invoke (1C4D380), NOT
    // MovieRoot::Invoke. Read the producer values; never touch AS3 ownership.
    // Its six-argument signature and 0x30 Value stride are confirmed in exe.
    constexpr unsigned char expected[]{0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xa8,0x02,0x00,0x00};
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target=reinterpret_cast<void*>(base+0xFF8300);unsigned char bytes[sizeof(expected)]{};
    if(!Read(bytes,target,sizeof(bytes)) || std::memcmp(bytes,expected,sizeof(bytes))) {
        TraceRuntime("HUD data tap disabled: Invoke bytes mismatch; original HUD retained");return false;
    }
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
    if(MH_CreateHook(target,&InvokeDetour,reinterpret_cast<void**>(&original))!=MH_OK)return false;
    if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);return false;}
    installed=true;TraceRuntime("HUD read-only local CHUDUI Invoke data tap installed; native UI and capture ownership unchanged");return true;
#else
    return false;
#endif
}
}
