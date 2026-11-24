#include "game/vr_c4.h"
#include "game/native_grab_probe.h"
#include "game/c4_placement.h"
#include "game/vr_body_rig.h"
#include "game/vehicle_camera.h"
#include "game/camera_capture.h"
#include "game/weapon_pose.h"
#include "core/logging.h"
#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
namespace jc3vr {
namespace {
using UpdateFn=bool(__fastcall*)(void*,float,void*,const float*);
using CameraFn=void*(__fastcall*)(void*,float*);
using Ray=NativeGrabRay;
using CastFn=NativeGrabCast;
UpdateFn update=nullptr; CameraFn camera=nullptr; CastFn cast=nullptr;
std::uintptr_t image=0;
std::atomic<bool> installed{false},fault{false};
std::mutex requestMutex;
std::uintptr_t requested=0; bool pressed=false; ULONGLONG requestAt=0,permitUntil=0,contactUntil=0;
float unitsPerMeter=1;
struct Scope {std::uintptr_t weapon=0; std::uint64_t frame=0; float units=1; float palm[3]{},forward[3]{},down[3]{}; C4SurfaceContact contact{}; bool queried=false;};
thread_local Scope* scope=nullptr;
C4PlacementState placement;
std::uintptr_t stateWeapon=0;
std::atomic<std::uint64_t> serial{0};
template<class T> bool Read(std::uintptr_t p,T& out) {
    __try {std::memcpy(&out,reinterpret_cast<const void*>(p),sizeof(out));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Write(std::uintptr_t p,const void* value,std::size_t size) {
    __try {std::memcpy(reinterpret_cast<void*>(p),value,size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void* __fastcall CameraDetour(void* self,float* out) {
    void* result=camera(self,out);
    // Only the C4 geometry producer's exact getter call. Never global view.
    if(scope && out && reinterpret_cast<std::uintptr_t>(_ReturnAddress())==image+0x98F526)
        if(!Write(reinterpret_cast<std::uintptr_t>(out+12),scope->palm,12)) fault=true;
    return result;
}
bool __fastcall CastDetour(void* self,const Ray* ray,float minimum,float maximum,void* info,void* filter,unsigned flags,int layer) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if(!scope || !info || caller!=image+0x98F6CC) {
        const bool result = cast(self,ray,minimum,maximum,info,filter,flags,layer);
        if (!scope && caller == image + 0xCE599A) SampleNativeGrabProbe(self, caller, cast);
        return result;
    }
    scope->queried=true;
    scope->contact.frame=scope->frame;
    Ray hand{}; std::copy(scope->palm,scope->palm+3,hand.origin);std::copy(scope->forward,scope->forward+3,hand.direction);
    const float reach=0.12f*scope->units;
    if(!std::isfinite(maximum) || maximum<reach || minimum!=0 || layer!=21) {fault=true; return cast(self,ray,minimum,maximum,info,filter,flags,layer);}
    // Let the original filter/result object retain native body references.
    const bool hit=cast(self,&hand,0,reach,info,filter,flags,layer);
    float fraction=1,normal[3]{};
    if(!Read(reinterpret_cast<std::uintptr_t>(info)+0x14,fraction) ||
       !Read(reinterpret_cast<std::uintptr_t>(info)+8,normal)) {fault=true;return hit;}
    if(hit && std::isfinite(fraction) && fraction>=0 && fraction<1) {
        scope->contact.hit=true;
        for(int i=0;i<3;++i) {
            scope->contact.point[i]=hand.origin[i]+hand.direction[i]*reach*fraction;
            scope->contact.normal[i]=normal[i];
        }
        // Caller reconstructs its point with the ORIGINAL max-distance register.
        // Rescale the fraction, otherwise a 10cm hit becomes a distant plant.
        if(!C4NativeHitFraction(fraction,reach,maximum,fraction)) {fault=true;return hit;}
        if(!Write(reinterpret_cast<std::uintptr_t>(info)+0x14,&fraction,sizeof(fraction))) fault=true;
    }
    return hit;
}
bool __fastcall UpdateDetour(void* self,float dt,void* skip,const float* target) {
    const auto weapon=reinterpret_cast<std::uintptr_t>(self);
    if(scope || fault || weapon!=GetVrC4Weapon()) return update(self,dt,skip,target);
    Scope local; local.weapon=weapon;
    {std::lock_guard lock(requestMutex);local.units=unitsPerMeter;}
    if(!GetVrBodyHandTarget(1,local.palm,local.forward,local.down)) return update(self,dt,skip,target);
    float length=0;for(float f:local.forward) length+=f*f;
    if(!std::isfinite(length) || length<0.25f) return update(self,dt,skip,target);
    for(float& f:local.forward) f/=std::sqrt(length);
    float handTarget[3];for(int i=0;i<3;++i) handTarget[i]=local.palm[i]+local.forward[i];
    local.frame=++serial; scope=&local;
    const bool result=update(self,dt,skip,handTarget);
    scope=nullptr;
    static std::atomic<bool> queryTraced{false};
    if(local.queried && !queryTraced.exchange(true)) {
        TraceRuntime("C4: native local geometry update used guarded hand-origin ray (12cm, original body filter/result retained)");
    }
    C4PlacementInput input;
    input.equipped=true;input.trackingValid=true;input.gameplay=HasNativeGameplayCamera();
    input.frame=local.frame;input.contact=local.contact;std::copy(local.palm,local.palm+3,input.palm);
    for(int i=0;i<3;++i) {input.palm[i]/=local.units;input.contact.point[i]/=local.units;}
    C4PlacementDecision decision;
    {
        std::lock_guard lock(requestMutex);
        if(stateWeapon!=weapon) {placement={};stateWeapon=weapon;permitUntil=0;contactUntil=0;}
        input.placePressed=requested==weapon && GetTickCount64()-requestAt<150 && pressed;
        decision=UpdateC4Placement(placement,input);
        contactUntil=decision.contactReady && !fault?GetTickCount64()+100:0;
        if(decision.requestNativePlace && !fault) permitUntil=GetTickCount64()+100;
        if(!local.queried || fault) permitUntil=0;
    }
    static std::atomic<bool> ready{false};
    if(decision.contactReady!=ready.exchange(decision.contactReady)) {
        TraceRuntime(decision.contactReady?"C4: fresh hand surface within 12cm; trigger can request plant":"C4: hand contact lost; remote planting blocked");
    }
    if(decision.requestNativePlace) TraceRuntime("C4: physical contact requested native trigger pulse (inventory/attachment stay native)");
    return result;
}
}
std::uintptr_t GetVrC4Weapon() {
    if(!installed || fault || !HasNativeGameplayCamera() || GetVehicleCameraAnchor().valid) return 0;
    const auto rico=VrBodyCharacter(); std::uintptr_t weapon=0,table=0;int slot=-1,state=-1;
    if(!rico || !Read(rico+0x998+16*4,weapon) || !weapon || !Read(weapon,table) || table!=image+0x23F8F88 ||
       !Read(weapon+0x420,slot) || slot!=4 || !Read(weapon+0x424,state) || (state!=1 && state!=2)) return 0;
    return weapon;
}
void RequestVrC4Trigger(std::uintptr_t weapon,bool trigger,float worldUnitsPerMeter) {
    std::lock_guard lock(requestMutex);
    if(!std::isfinite(worldUnitsPerMeter) || worldUnitsPerMeter<=0 || worldUnitsPerMeter>100) weapon=0;
    else unitsPerMeter=worldUnitsPerMeter;
    if(requested!=weapon || !weapon) {placement={};stateWeapon=0;permitUntil=0;contactUntil=0;}
    requested=weapon;pressed=trigger;requestAt=GetTickCount64();
}
bool VrC4NativePress(std::uintptr_t weapon) {
    std::lock_guard lock(requestMutex);
    return weapon && !fault && requested==weapon && GetTickCount64()-requestAt<150 && GetTickCount64()<permitUntil;
}
bool VrC4ContactReady(std::uintptr_t weapon) {
    std::lock_guard lock(requestMutex);
    return weapon && !fault && stateWeapon==weapon && GetTickCount64()<contactUntil;
}
bool InstallVrC4Hooks() {
    if(installed) return true;
    image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const std::uintptr_t offsets[]{0x98EBE0,0x75C7C0,0x286740};
    const unsigned char guards[3][15]{
        {0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48},
        {0x48,0x8B,0xC1,0x48,0x8B,0xCA,0x41,0xB8,0x40,0,0,0,0x48,0x8D,0x90},
        {0x48,0x8B,0xC4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x8D}};
    for(int i=0;i<3;++i) {unsigned char bytes[15];if(!Read(image+offsets[i],bytes) || std::memcmp(bytes,guards[i],15)) {TraceRuntime("C4: entry guard failed; native equipment unchanged");return false;}}
    void* detours[]{reinterpret_cast<void*>(&UpdateDetour),reinterpret_cast<void*>(&CameraDetour),reinterpret_cast<void*>(&CastDetour)};
    void** originals[]{reinterpret_cast<void**>(&update),reinterpret_cast<void**>(&camera),reinterpret_cast<void**>(&cast)};
    int created=0; bool okay=true;
    for(int i=0;i<3;++i) {
        void* site=reinterpret_cast<void*>(image+offsets[i]);
        if(MH_CreateHook(site,detours[i],originals[i])!=MH_OK){okay=false;break;}
        ++created;
    }
    if(okay) for(int i=0;i<3;++i) if(MH_EnableHook(reinterpret_cast<void*>(image+offsets[i]))!=MH_OK){okay=false;break;}
    if(!okay) {for(int i=0;i<created;++i){MH_DisableHook(reinterpret_cast<void*>(image+offsets[i]));MH_RemoveHook(reinterpret_cast<void*>(image+offsets[i]));}TraceRuntime("C4: hook transaction failed; native equipment unchanged");return false;}
    installed=true;TraceRuntime("C4: guarded local hand-origin contact adapter installed; native inventory/detonation unchanged");return true;
}
}
