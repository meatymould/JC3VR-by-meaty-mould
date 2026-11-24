#include "game/vehicle_camera.h"
#include "game/render_character_root.h"
#include "core/logging.h"
#include <Windows.h>
#include <mutex>
#include <cstring>
#include <atomic>
namespace jc3vr {
namespace {
std::mutex mutex; VehicleCameraAnchor current; ULONGLONG tick=0; std::uintptr_t owner=0;
float renderFraction=1; ULONGLONG renderTick=0;
bool ReadRoots(std::uintptr_t character, float roots[32]) {
    __try { std::memcpy(roots,reinterpret_cast<const void*>(character+0x27E8),128); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadAircraftWorld(std::uintptr_t vehicle, AircraftKind kind, float world[16]) {
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto expected=kind==AircraftKind::Helicopter?0x24E4348:kind==AircraftKind::Plane?0x24E7B18:0;
    if(!vehicle || !expected) return false;
    __try {
        if(*reinterpret_cast<const std::uintptr_t*>(vehicle+8)!=image+expected) return false;
        // Verified CGameObject::GetTransform implementation F232B0 copies
        // exactly this matrix from its secondary (+8) object.
        std::memcpy(world,reinterpret_cast<const void*>(vehicle+0x4B8),64);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
void PublishVehicleCameraAnchor(const VehicleCameraAnchor& anchor, std::uintptr_t character) {
    std::lock_guard lock(mutex); current=anchor; owner=character; tick=GetTickCount64();
}
void SetVehicleRenderInterpolation(float fraction) {
    if (!std::isfinite(fraction) || fraction<0 || fraction>1) return;
    std::lock_guard lock(mutex); renderFraction=fraction; renderTick=GetTickCount64();
    static std::atomic<bool> traced{false};
    if (!traced.exchange(true)) TraceRuntime("movement anchor: native render interpolation fraction observed (existing shadow callback); seated root T0/T1 smoothing active");
}
VehicleCameraAnchor GetFirstPersonCameraAnchor(bool renderInterpolated) {
    std::lock_guard lock(mutex);
    if (GetTickCount64()-tick>500 || !current.valid) return {};
    if (!owner) return current;
    // The skull offset belongs to Rico's model, not a world position frozen
    // at the previous Present. Recompose against his current native T1 root.
    float roots[32]{},world[16]{};
    if (!ReadRoots(owner,roots)) return {};
    VehicleCameraAnchor result;
    if (renderInterpolated && (GetCharacterRenderWorld(owner,world) ||
        (GetTickCount64()-renderTick<=100 && InterpolateVehicleRoot(roots,roots+16,renderFraction,world))))
        result=BuildVehicleCameraAnchor(world,current.eyeModel);
    else result=BuildVehicleCameraAnchor(roots+16,current.eyeModel);
    result.traversal=current.traversal;
    if(result.traversal) {
        if(!SetTraversalYawFrame(result,result.basis)) return {};
        return result;
    }
    float aircraftWorld[16]{};
    if(ReadAircraftWorld(current.vehicle,current.aircraft,aircraftWorld) && ApplyAircraftBank(result,aircraftWorld,current.seatYaw)) {
        result.aircraft=current.aircraft;result.driver=current.driver;
        result.vehicle=current.vehicle;result.seatYaw=current.seatYaw;
    }
    return result;
}
VehicleCameraAnchor GetVehicleCameraAnchor(bool renderInterpolated) {
    auto anchor=GetFirstPersonCameraAnchor(renderInterpolated);
    return anchor.traversal?VehicleCameraAnchor{}:anchor;
}
bool GetCharacterRenderPosition(std::uintptr_t character,float position[3]) {
    float roots[32]{},world[16]{};
    if(GetCharacterRenderWorld(character,world)){std::copy(world+12,world+15,position);return true;}
    if(!character || !ReadRoots(character,roots)) return false;
    float fraction=1;bool fresh=false;
    {std::lock_guard lock(mutex);fraction=renderFraction;fresh=GetTickCount64()-renderTick<=100;}
    const float zero[3]{};
    if(fresh && InterpolateVehicleRoot(roots,roots+16,fraction,world)) {
        std::copy(world+12,world+15,position);return true;
    }
    if(!BuildVehicleCameraAnchor(roots+16,zero).valid) return false;
    std::copy(roots+28,roots+31,position);return true;
}
}
