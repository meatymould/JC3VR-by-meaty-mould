#include "game/render_character_root.h"
#include "game/vr_body_rig.h"
#include "core/logging.h"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <intrin.h>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif
namespace jc3vr { namespace {
// Retail A53C...: Character::UpdateRender +7FA3CE..7FA3DA passes
// RCX=Rico-8 base subobject, XMM1=context.alpha, R8=completed interpolated 4x4.
// Callee consumes no fourth argument. Hook observes, then forwards exactly once.
using RootConsumer=void(__fastcall*)(void*,float,const float*);
RootConsumer originalRoot{};
std::uintptr_t image{};std::atomic<bool> active{false};
RenderRootCache cache;std::mutex cacheMutex;
std::atomic<std::uint64_t> captures{0},hits{0},misses{0},rejected{0};
bool Read(void* dst,std::uintptr_t src,std::size_t n){
    MEMORY_BASIC_INFORMATION m{};
    if(!src || src+n<src || VirtualQuery(reinterpret_cast<void*>(src),&m,sizeof(m))!=sizeof(m) ||
       m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || src+n>reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize)return false;
    __try{std::memcpy(dst,reinterpret_cast<void*>(src),n);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void Observe(void* character,float alpha,const float* world,std::uintptr_t caller){
    if(!active.load(std::memory_order_acquire) || !character)return;
    if(caller!=image+0x7FA3DF){++rejected;return;}
    const auto baseObject=reinterpret_cast<std::uintptr_t>(character);
    if(baseObject+8<baseObject || baseObject+8!=VrBodyCharacter())return;
    const auto rico=baseObject+8; // caller's lea rcx,[r12-8], not a guessed owner field
    float copy[16]{};std::uint32_t counter{};
    if(!Read(copy,reinterpret_cast<std::uintptr_t>(world),sizeof(copy)) || !Read(&counter,image+0x2D3A6AC,sizeof(counter))){++rejected;return;}
    std::lock_guard lock(cacheMutex);
    if(cache.Publish(rico,counter,GetTickCount64(),alpha,copy))++captures;else ++rejected;
}
void __fastcall RootDetour(void* character,float alpha,const float* world){
    Observe(character,alpha,world,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    originalRoot(character,alpha,world);
}
} // namespace
bool InstallRenderCharacterRoot(bool enabled){
    if(!enabled){active.store(false);return false;}
#if JC3VR_HAVE_MINHOOK
    if(active.load())return true;
    image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // Whole instructions; guard producer's register assignments and the engine
    // counter->FrameIndex publication too. No new engine offset is inferred.
    constexpr unsigned char entry[]{0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x78,0x10,0x55,0x48,0x8D,0x68,0xA1,0x48,0x81,0xEC,0xB0,0,0,0};
    constexpr unsigned char caller[]{0x4C,0x8D,0x45,0,0x0F,0x28,0xCE,0x49,0x8D,0x4C,0x24,0xF8,0xE8,0xD1,0x80,0xFC,0xFF};
    constexpr unsigned char stamp[]{0x44,0x8B,0x05,0xD9,0x64,0xC4,0x02,0x45,0x8B,0xC8,0x44,0x89,0x05,0xD3,0x64,0xC4,0x02,0x41,0xFF,0xC0,0x44,0x89,0x05,0xC5,0x64,0xC4,0x02};
    const auto matches=[](std::uintptr_t p,const auto& expected){unsigned char bytes[32]{};return Read(bytes,p,sizeof(expected)) && !std::memcmp(bytes,expected,sizeof(expected));};
    std::uintptr_t updateTarget{};
    if(!Read(&updateTarget,image+0x23D16D8+0x70,sizeof(updateTarget)) || updateTarget!=image+0x7FA220 ||
       !matches(image+0x7C24B0,entry) || !matches(image+0x7FA3CE,caller) || !matches(image+0xF41CC,stamp)){
        TraceRuntime("attachment render root: signature rejected; original paths retained");return false;
    }
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return false;
    auto* target=reinterpret_cast<void*>(image+0x7C24B0);
    if(MH_CreateHook(target,reinterpret_cast<void*>(&RootDetour),reinterpret_cast<void**>(&originalRoot))!=MH_OK)return false;
    if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);return false;}
    active.store(true,std::memory_order_release);
    TraceRuntime("attachment render root: read-only native root consumer installed; no interpolation approximation or timing change");return true;
#else
    return false;
#endif
}
bool GetCharacterRenderWorld(std::uintptr_t character,float world[16]){
    if(!active.load(std::memory_order_acquire) || !world || !character || character!=VrBodyCharacter())return false;
    std::uint32_t frame{};
    if(!Read(&frame,image+0x2D3A6B0,sizeof(frame))){++misses;return false;}
    RenderRootSample sample;
    {std::lock_guard lock(cacheMutex);if(!cache.Get(character,frame,GetTickCount64(),sample)){++misses;return false;}}
    std::copy(sample.world.begin(),sample.world.end(),world);++hits;return true;
}
void ReportRenderCharacterRoot(){
    RenderRootMeta meta;
    {std::lock_guard lock(cacheMutex);meta=cache.DiagnosticMetadata();}
    std::uint32_t currentFrame{};const bool frameReadable=active.load() && Read(&currentFrame,image+0x2D3A6B0,sizeof(currentFrame));
    TraceRuntime("attachment render root: enabled="+std::to_string(active.load())+" captures="+std::to_string(captures.load())+
        " current_frame_hits="+std::to_string(hits.load())+" stale_or_missing="+std::to_string(misses.load())+" rejected="+std::to_string(rejected.load())+
        " captured_frame="+std::to_string(meta.frame)+" draw_frame="+std::to_string(currentFrame)+" frame_readable="+std::to_string(frameReadable)+
        " alpha="+std::to_string(meta.alpha)+" age_ms="+std::to_string(GetTickCount64()-meta.tick)+
        "; counters only; real headset alignment unverified");
    const auto character=VrBodyCharacter();float native[16]{},legacy[16]{};
    if(GetCharacterRenderWorld(character,native) && Read(legacy,character+0x27E8,sizeof(legacy)) && ValidRenderRoot(legacy)){
        float squared=0;for(unsigned i=12;i<15;++i){const auto delta=native[i]-legacy[i];squared+=delta*delta;}
        float rotationTrace=0;
        for(unsigned axis=0;axis<3;++axis)for(unsigned c=0;c<3;++c)rotationTrace+=native[axis*4+c]*legacy[axis*4+c];
        const float angle=std::acos(std::clamp((rotationTrace-1)*.5f,-1.0f,1.0f))*57.295779513f;
        TraceRuntime("attachment render root F3: native_position="+std::to_string(native[12])+","+std::to_string(native[13])+","+std::to_string(native[14])+
            " raw_position="+std::to_string(legacy[12])+","+std::to_string(legacy[13])+","+std::to_string(legacy[14])+
            " root_difference_world_units="+std::to_string(std::sqrt(squared))+" root_angle_difference_degrees="+std::to_string(angle)+
            "; not a measured controller/weapon residual");
    }
}
}
