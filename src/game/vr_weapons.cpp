#include "game/vr_weapons.h"
#include "game/vr_c4.h"
#include "game/vr_body_rig.h"
#include "game/vr_grapple.h"
#include "game/weapon_pose.h"
#include "game/weapon_watch.h"
#include "game/weapon_attachment_probe.h"
#include "game/hijack_state_probe.h"
#include "game/physical_hijack.h"
#include "game/vehicle_camera.h"
#include "game/camera_capture.h"
#include "game/native_signature.h"
#include "game/dual_wield.h"
#include "render/weapon_selector.h"
#include "core/logging.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif

namespace jc3vr {
namespace {
// Character::UpdateWeaponAiming(this, const Vector3* target):
//   mov eax,[rdx]; mov [rcx+26BCh],eax; ... mov byte [rcx+2714h],1; ret
// (ferrobrew/pyxis-defs, build Steam 20206564 == ours). `this` is the
// Character base, 8 bytes before the address our vtable scan finds.
constexpr std::uintptr_t kUpdateWeaponAimingRva = 0x7602C0;
constexpr std::uint8_t kUpdateWeaponAimingBytes[]{0x8B, 0x02, 0x89, 0x81, 0xBC, 0x26, 0x00, 0x00, 0x8B, 0x42, 0x04, 0x89};
constexpr std::uintptr_t kCharacterBaseAdjust = 8;
constexpr std::uintptr_t kPocketDrawRva = 0x7608A0, kPocketHolsterRva = 0x7F2D80;
using PocketDrawFn = void(__fastcall*)(void*, bool, bool);
using PocketHolsterFn = void(__fastcall*)(void*);
PocketDrawFn g_pocketDraw = nullptr;
PocketHolsterFn g_pocketHolster = nullptr;
std::uintptr_t g_pocketImage = 0;
std::atomic<bool> g_pocketWanted = false, g_pocketInstalled = false, g_pocketFault = false;
std::atomic<ULONGLONG> g_pocketHandsUntil = 0, g_pocketRequestAt = 0;
std::atomic<PocketAction> g_pocketAction = PocketAction::None;

void __fastcall PocketDrawDetour(void* self, bool immediate, bool second) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_pocketImage;
    if ((caller == 0x933148 || caller == 0x93324C) && !g_pocketFault.load() &&
        GetTickCount64() < g_pocketHandsUntil.load() &&
        reinterpret_cast<std::uintptr_t>(self) + kCharacterBaseAdjust == VrBodyCharacter()) return;
    g_pocketDraw(self, immediate, second);
}

void ApplyPocketRequest(void* self, bool inVehicleOrTraversal) {
    if (!RicoPocketHolsterAvailable()) return;
    const auto action = g_pocketAction.exchange(PocketAction::None);
    if (inVehicleOrTraversal || !HasNativeGameplayCamera()) {
        g_pocketHandsUntil.store(0);
        return;
    }
    if (action == PocketAction::None || GetTickCount64() - g_pocketRequestAt.load() > 250) return;
    __try {
        if (action == PocketAction::Holster) {
            if (!RicoInventoryIsHolstered()) g_pocketHolster(self);
        } else {
            g_pocketDraw(self, false, false);
        }
        TraceRuntime(action == PocketAction::Holster ? "Rico pocket: native holster requested" :
            "Rico pocket: native draw requested");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pocketFault = true;
        g_pocketHandsUntil = 0;
    }
}
constexpr std::uintptr_t kSetAimModeRva = 0xC3CEB0;
constexpr std::uint8_t kSetAimModeBytes[]{0x41,0x54,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF};
constexpr std::uintptr_t kCanPrecisionAimRva=0x7D9200;
constexpr std::uint8_t kCanPrecisionAimBytes[]{0x40,0x57,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF};
using SetAimModeFn = void(__fastcall*)(void*, int, bool);
using CanPrecisionAimFn = bool(__fastcall*)(void*);
SetAimModeFn g_setAimMode=nullptr;
CanPrecisionAimFn g_canPrecisionAim=nullptr;
constexpr std::uintptr_t kIsRidingRva=0x77EA60;
constexpr std::uint8_t kIsRidingBytes[]{0x48,0x8B,0x81,0x20,0x18,0x00,0x00,0x48,0x2B,0x81,0x18,0x18,0x00,0x00,0x48};
using IsRidingFn=bool(__fastcall*)(void*);
IsRidingFn g_isRiding=nullptr;
std::atomic<bool> g_vehicleFault=false;
bool g_traversalStateLayoutReady=false;
constexpr std::uintptr_t kGetVehicleRva=0x7D5E30,kVehicleAimRva=0xF24B00;
constexpr std::uint8_t kGetVehicleBytes[]{0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x30,0x48,0x8D,0x54,0x24,0x20};
constexpr std::uint8_t kVehicleAimBytes[]{0x83,0xB9,0xDC,0x15,0x00,0x00,0x00,0x8B,0x02,0x89,0x81,0x34,0x4C,0x00,0x00};
constexpr std::uint8_t kVehicleTransformBytes[]{0x48,0x83,0xEC,0x28,0x48,0x8B,0xC1,0x48,0x8B,0xCA,0x41,0xB8,0x40,0x00,0x00};
using GetVehicleFn=void*(__fastcall*)(void*);
using VehicleAimFn=void(__fastcall*)(void*,const float*);
GetVehicleFn g_getVehicle=nullptr;
std::atomic<bool> g_pullHijackEnabled=false;
std::mutex g_pullContextMutex;
std::uintptr_t g_pullHelicopter=0;
ULONGLONG g_pullContextAt=0;
// Called on the existing local-character callback thread; no new engine hook.
std::uintptr_t ObserveExteriorPullHelicopter(void* self) {
    std::uintptr_t vehicle=0;
    __try {
        if(g_getVehicle && g_isRiding && !g_vehicleFault.load() && !g_isRiding(self)) {
            const auto candidate=reinterpret_cast<std::uintptr_t>(g_getVehicle(self));
            std::uintptr_t table=0;
            if(candidate && (table=*reinterpret_cast<const std::uintptr_t*>(candidate+8)) &&
               table==reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+0x24E4348)vehicle=candidate;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {vehicle=0;}
    return vehicle;
}
void PublishExteriorPullHelicopter(void* self) {
    if(!g_pullHijackEnabled.load())return;
    const auto vehicle=ObserveExteriorPullHelicopter(self);
    std::lock_guard lock(g_pullContextMutex);
    g_pullHelicopter=vehicle;g_pullContextAt=GetTickCount64();
}
void CaptureHijackState(void* self) {
    if(!HijackStateProbeActive())return;
    std::uintptr_t vehicle=0;
    __try {
        if(g_getVehicle) {
            const auto candidate=reinterpret_cast<std::uintptr_t>(g_getVehicle(self));
            if(candidate && *reinterpret_cast<const std::uintptr_t*>(candidate+8)==
               reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+0x24E4348)vehicle=candidate;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){vehicle=0;}
    ObserveHijackState(reinterpret_cast<std::uintptr_t>(self),vehicle);
}
VehicleAimFn g_originalVehicleAim=nullptr;
bool g_vehicleAimInstalled=false;
std::atomic<bool> g_aircraftFault=false;
std::mutex g_airGazeMutex;
std::uintptr_t g_airGazeVehicle=0;
float g_airGazeTarget[3]{};
ULONGLONG g_airGazeAt=0;
std::mutex g_supportMutex;
bool g_supportWanted=false;
std::uintptr_t g_supportWeapon=0;
ULONGLONG g_supportAt=0;
std::atomic<bool> g_supportFault=false;
constexpr std::uintptr_t kSniperVtableRva=0x23F0A48,kSniperAimRva=0x97F120;
constexpr std::uint8_t kSniperAimBytes[]{0x40,0x57,0x48,0x83,0xEC,0x50,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF};
using SniperAimFn=void(__fastcall*)(void*,int);
SniperAimFn g_originalSniperAim=nullptr;
SetAimModeFn g_originalScopePlayerAim=nullptr;
void __fastcall ScopePlayerAimDetour(void* self,int mode,bool cameraHint);
bool g_sniperHookInstalled=false;
std::atomic<std::uintptr_t> g_scopeWeapon=0;
std::atomic<ULONGLONG> g_scopeAt=0;
std::mutex g_opticMutex;
std::uintptr_t g_opticWeapon=0;
SniperOpticMount g_opticMount;
ULONGLONG g_opticAt=0;
void __fastcall SniperAimDetour(void* self,int mode) {
    const auto weapon=reinterpret_cast<std::uintptr_t>(self);
    // The original NORMAL branch removes the overlay and restores the holder.
    // NPC/desktop/tracking-lost paths remain native; no FOV or camera writes.
    if(mode==1 && VrScopeReady(weapon) && weapon==GetVrSniperWeapon()) {
        mode=0;
        static std::atomic<bool> traced=false;
        if(!traced.exchange(true)) TraceRuntime("VR scope: local sniper flat overlay/hide suppressed via native normal aim mode");
    }
    g_originalSniperAim(self,mode);
}
constexpr std::uintptr_t kAimTargetOffset = 0x26BC - 8;  // m_AimTargetPositionWeapons, from O

// Pose finalisation and the joint API (pyxis character.pyxis). Each is
// byte-guarded against our exe before use.
constexpr std::uintptr_t kUpdatePropEffectsRva = 0x7C2380;  // Character::UpdatePropEffects(this, float dt)
constexpr std::uint8_t kUpdatePropEffectsBytes[]{0x40, 0x53, 0x48, 0x83, 0xEC, 0x70, 0x0F, 0x29, 0x74, 0x24, 0x60};
constexpr std::uintptr_t kGetSafeIndexRva = 0x79AB30;  // Character::GetSafeIndex(this, SafeBoneIndex) -> bone
constexpr std::uint8_t kGetSafeIndexBytes[]{0x44, 0x0F, 0xB7, 0x81, 0xC8, 0x21, 0x00, 0x00};
constexpr std::uintptr_t kGetJointRva = 0x43FF90;  // AnimationController::GetJoint(this, index, Joint*)
constexpr std::uint8_t kGetJointBytes[]{0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x05};
constexpr std::uintptr_t kSetJointRva = 0x43FFF0;  // AnimationController::SetJoint(this, index, Joint*)
constexpr std::uint8_t kSetJointBytes[]{0x48, 0x8B, 0x89, 0xB8, 0x00, 0x00, 0x00, 0xE9};

// Character fields, from the pyxis Character base (= the hook's `this`).
constexpr std::uintptr_t kAnimationControllerOffset = 0x1780 + 0x178;  // m_AnimatedModel.m_AnimationController
constexpr std::uintptr_t kIsLocalCharacterOffset = 0x262A;
constexpr std::uintptr_t kWorldMatrixT1Offset = 0x2830;  // row-major, translation in row 3

// NBone::ESafeBoneIndex name hashes.
constexpr std::uint32_t kSafeLeftHand = 0x57C83F95, kSafeRightHand = 0x69E77FA6;
constexpr std::uint32_t kSafeAttachLeft = 0x4190BFF7, kSafeAttachRight = 0x65C5D2EB;
// Skeleton bones of the same rig vr_body_rig uses (hkaSkeleton "Reference"):
// the hands, and index / middle / pinky roots that give the hand's frame.
constexpr std::uint32_t kHandBone[2]{25, 65};
constexpr std::uint32_t kFingerBones[2][3]{{30, 33, 41}, {70, 73, 81}};
constexpr float kWristToPalm = 0.08f;  // matches vr_body_rig

// hkQsTransform as the joint API takes it: model space, 16-byte vectors.
struct alignas(16) Joint {
    float t[4];
    float q[4];  // x, y, z, w
    float s[4];
};

using UpdateWeaponAimingFn = void(__fastcall*)(void* self, const float* target);
using UpdatePropEffectsFn = void(__fastcall*)(void* self, float dt);
using GetSafeIndexFn = std::uint32_t(__fastcall*)(void* self, std::uint32_t safeIndex);
using JointFn = void(__fastcall*)(void* controller, std::uint32_t index, Joint* joint);

UpdateWeaponAimingFn g_original = nullptr;
UpdatePropEffectsFn g_originalPropEffects = nullptr;
GetSafeIndexFn g_getSafeIndex = nullptr;
JointFn g_getJoint = nullptr;
JointFn g_setJoint = nullptr;
bool g_installed = false;
bool g_handsInstalled = false;
VrWeaponOptions g_options;
std::atomic<bool> g_active = true;       // Shift+F7 flips this for A/B tests
std::atomic<bool> g_handsActive = true;  // Shift+F8

std::mutex g_rayMutex;
bool g_rayValid = false;
float g_rayOrigin[3]{}, g_rayDirection[3]{};
ULONGLONG g_rayAt = 0;
std::atomic<std::uint64_t> g_replaced = 0, g_untouched = 0;
std::atomic<std::uint64_t> g_handWrites[2]{}, g_handSkips = 0;
std::atomic<std::uint64_t> g_nativeHeadHides = 0;
std::atomic<bool> g_headHideFault = false;
std::atomic<bool> g_rigChecked = false, g_rigMismatch = false, g_handFault = false;

bool SafeRead(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void __fastcall ScopePlayerAimDetour(void* self,int mode,bool cameraHint) {
    const auto weapon=GetVrSniperWeapon();
    std::uintptr_t character=0,table=0;
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if(mode==1 && VrScopeReady(weapon) &&
       SafeRead(&table,self,sizeof(table)) && table==image+0x2468868 &&
       SafeRead(&character,reinterpret_cast<const char*>(self)+0x110,sizeof(character)) && character==VrBodyCharacter()-8) {
        mode=0;cameraHint=false;
    }
    g_originalScopePlayerAim(self,mode,cameraHint);
}

bool BytesMatch(std::uintptr_t address, const std::uint8_t* expected, std::size_t size) {
    return MatchNativeSignature(address,expected,size,SafeRead);
}
void __fastcall VehicleAimDetour(void* self,const float* target) {
    float gaze[3]{}; bool wanted=false;
    {
        std::lock_guard lock(g_airGazeMutex);
        wanted=g_airGazeVehicle && g_airGazeVehicle==reinterpret_cast<std::uintptr_t>(self) && GetTickCount64()-g_airGazeAt<=150;
        if(wanted) std::memcpy(gaze,g_airGazeTarget,sizeof(gaze));
    }
    std::uintptr_t table=0;int seat=-1;
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto vehicle=reinterpret_cast<std::uintptr_t>(self);
    if(wanted && SafeRead(&table,reinterpret_cast<void*>(vehicle+8),sizeof(table)) && table==image+0x24E4348 &&
       SafeRead(&seat,reinterpret_cast<void*>(vehicle+0x15DC),sizeof(seat)) && seat==0) {
        target=gaze;
        static std::atomic<bool> traced=false;
        if(!traced.exchange(true)) TraceRuntime("helicopter: fresh local-driver gaze target used by native vehicle aim setter");
    }
    g_originalVehicleAim(self,target);
}
VehicleCameraAnchor AddAircraftFrame(void* character,VehicleCameraAnchor seat) {
    if(!seat.valid || !g_getVehicle || g_aircraftFault.load()) return seat;
    // Called only by the existing local-character simulation/seat hook.
    // Fail separately: never disable the proven seated camera on air failure.
    __try {
        const auto vehicle=reinterpret_cast<std::uintptr_t>(g_getVehicle(character));
        const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if(!vehicle) return seat;
        const auto table=*reinterpret_cast<const std::uintptr_t*>(vehicle+8);
        const auto kind=table==image+0x24E4348?AircraftKind::Helicopter:
            table==image+0x24E7B18?AircraftKind::Plane:AircraftKind::None;
        if(kind==AircraftKind::None) return seat;
        float world[16]{};std::memcpy(world,reinterpret_cast<void*>(vehicle+0x4B8),sizeof(world));
        static std::uintptr_t previous=0;static float yaw=0;
        if(previous!=vehicle) {
            if(!AircraftSeatYaw(world,seat.basis,yaw)) return seat;
            previous=vehicle;
        }
        if(!ApplyAircraftBank(seat,world,yaw)) return seat;
        seat.aircraft=kind;seat.vehicle=vehicle;seat.seatYaw=yaw;
        seat.driver=*reinterpret_cast<const int*>(vehicle+0x15DC)==0;
        return seat;
    } __except(EXCEPTION_EXECUTE_HANDLER) { g_aircraftFault=true;return seat; }
}

// --- the game's gun where we draw it ----------------------------------------
//
// Found live with the weapon probe (2026-10-01, our own RTTI lookup): Rico's
// held bullet weapon (CBulletWeaponBase, vtable RVA 0x23E3038, slots O+0x998 /
// O+0x9A8) keeps a vector of CWeaponData* at +0x568..+0x570 (one per gun, so
// two for dual wield). CWeaponData (vtable RVA 0x23F4750) holds the gun's
// world matrix at +0x158 (previous step) and +0x198 (current step): +0x198 at
// one read equals +0x158 at the next. The drawn gun sits exactly on the
// hand-attach bone. Bullets come from the game's gun, not ours, so each sim
// step any of those matrices sitting on an attach bone is replaced with where
// JC3VR draws that bone. Matching and placement are in Rico's model space
// (his next-step world matrix O+0x2828) so the result holds at any speed.
//
// When: a hardware watch on T1 (weapon_watch, 2D 2026-10-01) showed the game
// rebuilding it every update inside exe+0x986F80 (CWeaponData placement:
// T0 = T1, T1 = attach bone via Character exe+0x79AEB0, times world via
// exe+0x34530), so an earlier write is simply overwritten. The move runs right
// after that function returns. Its arguments (from its disassembly): rcx =
// CWeaponData, rdx = owner (our O for Rico), three stack arguments, no float
// arguments.
constexpr std::uintptr_t kBulletWeaponSlots[2]{0x998, 0x9A8};
constexpr std::uintptr_t kBulletWeaponVtableRva = 0x23E3038;
constexpr std::uintptr_t kWeaponDataVtableRva = 0x23F4750;
constexpr std::uintptr_t kWeaponDataListOffset = 0x568;
constexpr std::uintptr_t kWeaponWorldT0 = 0x158, kWeaponWorldT1 = 0x198;
constexpr std::uintptr_t kRicoWorldT1 = 0x2828;  // from O

std::atomic<bool> g_gunsActive = true;  // Shift+F9
std::atomic<std::uint64_t> g_gunsMoved = 0, g_gunsUnmatched = 0;
std::atomic<bool> g_gunDump = false;  // jc3vr_guns_dump

// Row-major 4x4 product (p * a * b convention: a first, then b).
void MulRowMajor(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r != 4; ++r)
        for (int c = 0; c != 4; ++c) out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

// Inverse of a rigid row-major 4x4.
void InverseRigid(const float m[16], float out[16]) {
    for (int i = 0; i != 3; ++i)
        for (int j = 0; j != 3; ++j) out[i * 4 + j] = m[j * 4 + i];
    for (int j = 0; j != 3; ++j) out[12 + j] = -(m[12] * out[j] + m[13] * out[4 + j] + m[14] * out[8 + j]);
    out[3] = out[7] = out[11] = 0.0f;
    out[15] = 1.0f;
}

bool WriteMatrix(std::uintptr_t address, const float m[16]) {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), m, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct V3 {
    float x, y, z;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Normalize(V3 v) {
    const float l = std::sqrt(Dot(v, v));
    return l > 1e-6f ? V3{v.x / l, v.y / l, v.z / l} : V3{0.0f, 0.0f, 0.0f};
}

// The game's placement branch reads data+0xD0+slot*4 and passes that safe
// bone hash to Character::GetJointTransform (exe+0x79AEB0). This is the
// authoritative mount; geometric matching against a render-side pose races
// animation refresh and can leave a firing gun on invisible Rico.
bool ResolveWeaponMount(std::uintptr_t rico, std::uintptr_t data, std::uint32_t slot, int& mount) {
    if (!g_getSafeIndex || slot > 7) return false;
    std::uint32_t hash = 0;
    if (!SafeRead(&hash, reinterpret_cast<const void*>(data + 0xD0 + slot * 4), sizeof(hash))) return false;
    std::uint32_t bone = ~0u;
    __try { bone = g_getSafeIndex(reinterpret_cast<void*>(rico - kCharacterBaseAdjust), hash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    switch (bone) {
    case 26: mount = 0; break;
    case 44: mount = 1; break;
    case 66: mount = 2; break;
    case 84: mount = 3; break;
    default: return false;  // holsters, back mounts, vehicle/physics placement
    }
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 8) {
        char text[192];
        std::snprintf(text, sizeof(text), "weapon authoritative mount: data=0x%llX slot=%u hash=0x%08X bone=%u controller=%s",
            static_cast<unsigned long long>(data), slot, hash, bone, mount < 2 ? "left" : "right");
        TraceRuntime(text);
    }
    return true;
}

struct WeaponMountBinding {
    std::uintptr_t data = 0, parent = 0, rico = 0;
    int mount = -1;
    float local[16]{};
    bool measured = false;
    ULONGLONG tick = 0;
};
std::mutex g_mountMutex;
WeaponMountBinding g_mountBindings[16]{};
std::atomic<std::uint64_t> g_mountFallbacks{0};

void StableMountLocal(std::uintptr_t rico, std::uintptr_t data, int mount,
                      const float model[16], const float gameMount[16], float local[16]) {
    std::uintptr_t parent = 0;
    SafeRead(&parent, reinterpret_cast<const void*>(data + 0xC0), sizeof(parent));
    const auto now = GetTickCount64();
    std::lock_guard lock(g_mountMutex);
    WeaponMountBinding* binding = &g_mountBindings[0];
    bool found = false;
    for (auto& entry : g_mountBindings) {
        if (entry.data == data) { binding = &entry; found = true; break; }
        if (entry.tick < binding->tick) binding = &entry;
    }
    if (!found || binding->parent != parent || binding->rico != rico || binding->mount != mount || now - binding->tick > 2000) {
        *binding = {};
        binding->data = data; binding->parent = parent; binding->rico = rico; binding->mount = mount;
        binding->local[0] = binding->local[5] = binding->local[10] = binding->local[15] = 1.0f;
    }
    binding->tick = now;
    if (!binding->measured) {
        float distanceSquared = 0, trace = 0;
        for (int i = 0; i != 3; ++i) {
            const float delta = model[12+i] - gameMount[12+i]; distanceSquared += delta * delta;
            for (int j = 0; j != 3; ++j) trace += model[i*4+j] * gameMount[i*4+j];
        }
        // Capture the gun's mount-relative offset only when these two pose
        // snapshots agree. Keep it through animation changes; never derive
        // a new offset by subtracting unrelated simulation/render poses.
        if (distanceSquared < 0.06f*0.06f && (trace-1)*0.5f > 0.98f) {
            float inverse[16]; InverseRigid(gameMount, inverse);
            MulRowMajor(model, inverse, binding->local);
            binding->measured = true;
        } else ++g_mountFallbacks;
    }
    std::memcpy(local, binding->local, sizeof(binding->local));
}



// Moves one of Rico's guns (a CWeaponData just placed by the game) from the
// game's hand mount to where JC3VR draws that mount.
void MoveGunToHand(std::uintptr_t rico, std::uintptr_t data, std::uint32_t placementSlot) {
    if (!g_options.gunsToController || (!g_gunsActive.load() && !g_gunDump.load())) return;
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t vtable = 0;
    if (!SafeRead(&vtable, reinterpret_cast<const void*>(data), sizeof(vtable)) || vtable != image + kWeaponDataVtableRva) return;
    int mount = -1;
    if (!ResolveWeaponMount(rico, data, placementSlot, mount)) { ++g_gunsUnmatched; return; }
    float game[4][16], carried[4][16];
    bool valid[4]{};
    unsigned long long tick = 0;
    if (!GetVrBodyAttachMounts(game, carried, valid, tick) || GetTickCount64() - tick > 200) return;
    if (!valid[mount]) return;
    float ricoWorld[16], toModel[16];
    if (!SafeRead(ricoWorld, reinterpret_cast<const void*>(rico + kRicoWorldT1), sizeof(ricoWorld)) || std::abs(ricoWorld[15] - 1.0f) > 1e-3f) return;
    for (const float value : ricoWorld) if (!std::isfinite(value)) return;
    InverseRigid(ricoWorld, toModel);
    {
        {
            float world[16], model[16];
            if (!SafeRead(world, reinterpret_cast<const void*>(data + kWeaponWorldT1), sizeof(world)) || std::abs(world[15] - 1.0f) > 1e-3f) return;
            for (const float value : world) if (!std::isfinite(value)) return;
            MulRowMajor(world, toModel, model);
            // Keep the gun's offset from its mount (identity while firing).
            float local[16], placedModel[16], placed[16];
            StableMountLocal(rico, data, mount, model, game[mount], local);
            MulRowMajor(local, carried[mount], placedModel);
            MulRowMajor(placedModel, ricoWorld, placed);
            // The gun's -Z is the barrel. Use the tracked aim frame for both
            // pointing and roll; the prior minimal rotation preserved animated
            // wrist roll (seen rotating the gun while its barrel stayed aligned).
            {
                float grip[3], fingers[3], down[3];
                if (GetVrBodyHandTarget(mount < 2 ? 0 : 1, grip, fingers, down)) {
                    if (g_options.controllerRoll) {
                        // Reject incoherent pivot samples without losing the
                        // previous stable controller-orientation behaviour.
                        if (!AlignWeaponAboutGrip(grip, fingers, down, placed) &&
                            !AlignWeaponToController(fingers, down, placed)) return;
                    } else {
                        const V3 b = Normalize({-placed[8], -placed[9], -placed[10]});
                        const V3 f = Normalize({fingers[0], fingers[1], fingers[2]});
                        const V3 axis = Cross(b, f);
                        const float s = std::sqrt(Dot(axis, axis)), c = Dot(b, f);
                        if (s > 1e-5f) {
                            const V3 k{axis.x / s, axis.y / s, axis.z / s};
                            for (int row = 0; row != 3; ++row) {
                                const V3 v{placed[row * 4], placed[row * 4 + 1], placed[row * 4 + 2]};
                                const V3 kv = Cross(k, v);
                                const float kd = Dot(k, v) * (1.0f - c);
                                placed[row * 4] = v.x * c + kv.x * s + k.x * kd;
                                placed[row * 4 + 1] = v.y * c + kv.y * s + k.y * kd;
                                placed[row * 4 + 2] = v.z * c + kv.z * s + k.z * kd;
                            }
                        }
                    }
                }
            }
            // Self-timed dumps too: every 2 s while a gun is matched, 20 in all.
            static ULONGLONG nextAutoDump = 0;
            static int autoDumps = 0;
            if (autoDumps < 20 && GetTickCount64() >= nextAutoDump) {
                nextAutoDump = GetTickCount64() + 2000;
                ++autoDumps;
                g_gunDump = true;
            }
            if (g_gunDump.exchange(false)) {
                // One-shot numbers for checking the placement (jc3vr_guns_dump).
                char text[512];
                std::snprintf(text, sizeof(text),
                    "guns dump: data=0x%llX mount=%d gunModel t(%.3f %.3f %.3f) mountGame t(%.3f %.3f %.3f) local t(%.4f %.4f %.4f) diag(%.4f %.4f %.4f) "
                    "mountDrawn t(%.3f %.3f %.3f) placed world t(%.3f %.3f %.3f) game world t(%.3f %.3f %.3f)",
                    static_cast<unsigned long long>(data), mount, model[12], model[13], model[14], game[mount][12], game[mount][13], game[mount][14], local[12],
                    local[13], local[14], local[0], local[5], local[10], carried[mount][12], carried[mount][13], carried[mount][14], placed[12], placed[13],
                    placed[14], world[12], world[13], world[14]);
                TraceRuntime(text);
                std::snprintf(text, sizeof(text), "guns dump: rows game x(%.3f %.3f %.3f) y(%.3f %.3f %.3f) z(%.3f %.3f %.3f) placed x(%.3f %.3f %.3f) y(%.3f %.3f %.3f) z(%.3f %.3f %.3f)",
                    world[0], world[1], world[2], world[4], world[5], world[6], world[8], world[9], world[10], placed[0], placed[1], placed[2], placed[4], placed[5],
                    placed[6], placed[8], placed[9], placed[10]);
                TraceRuntime(text);
                float grip[3], fingers[3], down[3];
                if (GetVrBodyHandTarget(mount < 2 ? 0 : 1, grip, fingers, down)) {
                    const float gx = placed[12] - grip[0], gy = placed[13] - grip[1], gz = placed[14] - grip[2];
                    std::snprintf(text, sizeof(text), "guns dump: controller grip (%.3f %.3f %.3f) fingers (%.3f %.3f %.3f); placed gun is %.3f m from the grip",
                        grip[0], grip[1], grip[2], fingers[0], fingers[1], fingers[2], std::sqrt(gx * gx + gy * gy + gz * gz));
                    TraceRuntime(text);
                    const V3 forward = Normalize({fingers[0], fingers[1], fingers[2]});
                    const V3 handleDown = Normalize({down[0], down[1], down[2]});
                    const float barrelDot = Dot({-placed[8], -placed[9], -placed[10]}, forward);
                    const float topDot = Dot({placed[4], placed[5], placed[6]}, {-handleDown.x, -handleDown.y, -handleDown.z});
                    std::snprintf(text, sizeof(text), "guns dump: barrel_dot_aim=%.6f top_dot_controller_up=%.6f controller_roll=%d",
                        barrelDot, topDot, g_options.controllerRoll ? 1 : 0);
                    TraceRuntime(text);
                }
                float lastOriginal[16], lastCarried[16];
                int lastBone = -1;
                unsigned long long lastTick = 0;
                if (GetVrBodyLastCarried(1, lastOriginal, lastCarried, lastBone, lastTick)) {
                    std::snprintf(text, sizeof(text), "guns dump: render carry last moved a gun on bone %d %llu ms ago: from (%.3f %.3f %.3f) to (%.3f %.3f %.3f); move active=%d",
                        lastBone, static_cast<unsigned long long>(GetTickCount64() - lastTick), lastOriginal[12], lastOriginal[13], lastOriginal[14], lastCarried[12],
                        lastCarried[13], lastCarried[14], g_gunsActive.load() ? 1 : 0);
                    TraceRuntime(text);
                }
            }
            if (!g_gunsActive.load()) return;
            if (WriteMatrix(data + kWeaponWorldT1, placed) && WriteMatrix(data + kWeaponWorldT0, placed)) {
                RegisterVrBodyPlacedWeapon(data, placed,g_options.controllerRoll?(mount<2?0:1):-1);
                RecordDualGunPlacement(data,mount<2?0:1);
                // Only the primary local sniper body, never magazine/effects.
                std::uintptr_t parent=0,table=0,first=0,begin=0;std::int16_t icon=0;
                if(mount>=2 && g_options.controllerRoll &&
                   SafeRead(&parent,reinterpret_cast<void*>(data+0xC0),sizeof(parent)) && parent &&
                   SafeRead(&table,reinterpret_cast<void*>(parent),sizeof(table)) && table==image+kSniperVtableRva &&
                   SafeRead(&begin,reinterpret_cast<void*>(parent+0x568),sizeof(begin)) && begin &&
                   SafeRead(&first,reinterpret_cast<void*>(begin),sizeof(first)) && first==data &&
                   SafeRead(&icon,reinterpret_cast<void*>(parent+0x41C),sizeof(icon))) {
                    auto optic=SniperModelOptic(icon);float grip[3]{},forward[3]{},down[3]{};
                    if(optic.valid && GetVrBodyHandTarget(1,grip,forward,down)) {
                        // Retain the actual model-origin/palm offset in gun
                        // axes. Later XR poses rotate this same offset once.
                        for(int axis=0;axis<3;++axis) for(int c=0;c<3;++c)
                            optic.fromGrip[axis]+=(placed[12+c]-grip[c])*placed[axis*4+c];
                        std::lock_guard lock(g_opticMutex);
                        g_opticWeapon=parent;g_opticMount=optic;g_opticAt=GetTickCount64();
                    }
                }
                ++g_gunsMoved;
            }
        }
    }
}

// CWeaponData placement (exe+0x986F80): the game's own gun follows its hand
// bone here; Rico's guns then move to the drawn hands.
constexpr std::uintptr_t kPlaceWeaponDataRva = 0x986F80;
constexpr std::uint8_t kPlaceWeaponDataBytes[]{0x48, 0x8B, 0xC4, 0x55, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0xA8, 0x98, 0xFD, 0xFF, 0xFF};
using PlaceWeaponDataFn = void(__fastcall*)(void* data, void* owner, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
PlaceWeaponDataFn g_originalPlace = nullptr;
bool g_placeInstalled = false;

// When the game last placed guns (every character, every simulation update).
// While the map is open placements still trickle in now and then (live
// 2026-10-02: menu mode flickered), so "running" means a sustained rate: the
// last 8 placements all within 400 ms. A ring of recent placement times.
constexpr int kPlacementRing = 8;
std::atomic<ULONGLONG> g_placementTimes[kPlacementRing]{};
std::atomic<unsigned> g_placementIndex{0};

void __fastcall PlaceWeaponDataDetour(void* data, void* owner, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7) {
    g_originalPlace(data, owner, a3, a4, a5, a6, a7);
    g_placementTimes[g_placementIndex.fetch_add(1, std::memory_order_relaxed) % kPlacementRing].store(GetTickCount64(), std::memory_order_relaxed);
    const std::uintptr_t rico = VrBodyCharacter();
    if (rico && data && reinterpret_cast<std::uintptr_t>(owner) == rico) {
        float nativeWorld[32];
        const auto address=reinterpret_cast<std::uintptr_t>(data);
        const bool observe=WeaponAttachmentProbeActive() &&
            SafeRead(nativeWorld,reinterpret_cast<const void*>(address+kWeaponWorldT0),64) &&
            SafeRead(nativeWorld+16,reinterpret_cast<const void*>(address+kWeaponWorldT1),64);
        MoveGunToHand(rico, reinterpret_cast<std::uintptr_t>(data), static_cast<std::uint32_t>(a5));
        if(observe)ObserveWeaponAttachmentPlacement(reinterpret_cast<std::uintptr_t>(data),rico,static_cast<unsigned>(a5),nativeWorld);
    }
}

// The CWeaponData of the gun currently on a hand mount (game pose), or the
// first one held, or 0. Used by the diagnostics.
std::uintptr_t FindHeldWeaponData(std::uintptr_t rico) {
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t first = 0;
    for (const std::uintptr_t slot : kBulletWeaponSlots) {
        std::uintptr_t weapon = 0, vtable = 0, list[2]{};
        if (!SafeRead(&weapon, reinterpret_cast<const void*>(rico + slot), sizeof(weapon)) || !weapon) continue;
        if (!SafeRead(&vtable, reinterpret_cast<const void*>(weapon), sizeof(vtable)) || vtable != image + kBulletWeaponVtableRva) continue;
        if (!SafeRead(list, reinterpret_cast<const void*>(weapon + kWeaponDataListOffset), sizeof(list)) || list[1] <= list[0]) continue;
        std::uintptr_t data = 0;
        if (!SafeRead(&data, reinterpret_cast<const void*>(list[0]), sizeof(data)) || !data) continue;
        if (!SafeRead(&vtable, reinterpret_cast<const void*>(data), sizeof(vtable)) || vtable != image + kWeaponDataVtableRva) continue;
        if (!first) first = data;
        // Prefer the one whose matrix is near Rico's right hand (held, not holstered).
        float world[16], rico16[16];
        if (SafeRead(world, reinterpret_cast<const void*>(data + kWeaponWorldT1), sizeof(world)) &&
            SafeRead(rico16, reinterpret_cast<const void*>(rico + kRicoWorldT1), sizeof(rico16))) {
            const float dx = world[12] - rico16[12], dy = world[13] - rico16[13], dz = world[14] - rico16[14];
            if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f && dy > 0.8f) return data;
        }
    }
    return first;
}

// The player's weapon aim point, replaced by a point along the controller ray.
// Distance: the game's own target distance (its raycast along the view), but
// at least 20 m, so the muzzle-to-point direction is essentially the
// controller's direction even if the muzzle sits a little off the hand.
void __fastcall UpdateWeaponAimingDetour(void* self, const float* target) {
    const std::uintptr_t rico = VrBodyCharacter();
    if (!rico || !self || reinterpret_cast<std::uintptr_t>(self) != rico - kCharacterBaseAdjust) return g_original(self, target);
    if (!g_active.load() || !g_options.aimFromController || !target) return g_original(self, target);
    float origin[3], direction[3];
    {
        std::lock_guard lock(g_rayMutex);
        if (!g_rayValid || GetTickCount64() - g_rayAt > 200) {
            ++g_untouched;
            return g_original(self, target);
        }
        std::memcpy(origin, g_rayOrigin, sizeof(origin));
        std::memcpy(direction, g_rayDirection, sizeof(direction));
    }
    float game[3];
    if (!SafeRead(game, target, sizeof(game))) return g_original(self, target);
    const float dx = game[0] - origin[0], dy = game[1] - origin[1], dz = game[2] - origin[2];
    const float distance = std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz), 20.0f, 1000.0f);
    const float point[3]{origin[0] + direction[0] * distance, origin[1] + direction[1] * distance, origin[2] + direction[2] * distance};
    ++g_replaced;
    g_original(self, point);
}

// --- small vector / quaternion helpers for the hand write -------------------

// The rotation taking frame (a1, a2) onto frame (b1, b2): first axis exact,
// second axis projected (the same construction as vr_body_rig's AlignFrames,
// which drives the render-side hands the user has already signed off on).
void AlignFrames(V3 a1, V3 a2, V3 b1, V3 b2, float r[3][3]) {
    const auto frame = [](V3 x, V3 y, V3 out[3]) {
        out[0] = Normalize(x);
        const float d = Dot(y, out[0]);
        out[1] = Normalize({y.x - out[0].x * d, y.y - out[0].y * d, y.z - out[0].z * d});
        out[2] = Cross(out[0], out[1]);
    };
    V3 a[3], b[3];
    frame(a1, a2, a);
    frame(b1, b2, b);
    const auto c = [](V3 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; };
    for (int i = 0; i != 3; ++i)
        for (int j = 0; j != 3; ++j) r[i][j] = c(b[0], i) * c(a[0], j) + c(b[1], i) * c(a[1], j) + c(b[2], i) * c(a[2], j);
}

// Rotation matrix (column-vector convention, r[row][col]) -> quaternion xyzw.
void MatrixToQuat(const float r[3][3], float q[4]) {
    const float trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (r[2][1] - r[1][2]) / s;
        q[1] = (r[0][2] - r[2][0]) / s;
        q[2] = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q[3] = (r[2][1] - r[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (r[0][1] + r[1][0]) / s;
        q[2] = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q[3] = (r[0][2] - r[2][0]) / s;
        q[0] = (r[0][1] + r[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (r[1][2] + r[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q[3] = (r[1][0] - r[0][1]) / s;
        q[0] = (r[0][2] + r[2][0]) / s;
        q[1] = (r[1][2] + r[2][1]) / s;
        q[2] = 0.25f * s;
    }
}

// a * b (apply b, then a), xyzw.
void QuatMultiply(const float a[4], const float b[4], float out[4]) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    const float l = std::sqrt(x * x + y * y + z * z + w * w);
    out[0] = x / l, out[1] = y / l, out[2] = z / l, out[3] = w / l;
}

// World -> Rico model space through his row-major world matrix
// (p_world = p_model * R + t, so p_model[k] = (p_world - t) . row_k).
V3 ToModel(const float world[16], const float p[3], bool point) {
    const V3 d = point ? V3{p[0] - world[12], p[1] - world[13], p[2] - world[14]} : V3{p[0], p[1], p[2]};
    return {Dot(d, {world[0], world[1], world[2]}), Dot(d, {world[4], world[5], world[6]}), Dot(d, {world[8], world[9], world[10]})};
}

// One-time evidence for the log: the safe-bone lookups land on the bones we
// assume (so the finger indices are valid too), and the gun attach bone is a
// child of the hand (SetJoint on the hand carried it along).
void CheckRig(void* character, void* controller) {
    if (g_rigChecked.exchange(true)) return;
    const std::uint32_t left = g_getSafeIndex(character, kSafeLeftHand), right = g_getSafeIndex(character, kSafeRightHand);
    const std::uint32_t attachLeft = g_getSafeIndex(character, kSafeAttachLeft), attachRight = g_getSafeIndex(character, kSafeAttachRight);
    g_rigMismatch = left != kHandBone[0] || right != kHandBone[1];
    TraceRuntime("VR hands: safe bones LEFT_HAND=" + std::to_string(left) + " RIGHT_HAND=" + std::to_string(right) + " ATTACH_HAND_LEFT=" +
        std::to_string(attachLeft) + " ATTACH_HAND_RIGHT=" + std::to_string(attachRight) + (g_rigMismatch ? " MISMATCH: hand writes disabled" : " (as expected)"));
    if (g_rigMismatch) return;
    // Nudge the right hand 10 cm up and read the attach bone back, then restore.
    Joint hand{}, before{}, after{};
    g_getJoint(controller, right, &hand);
    g_getJoint(controller, attachRight, &before);
    Joint moved = hand;
    moved.t[1] += 0.1f;
    g_setJoint(controller, right, &moved);
    g_getJoint(controller, attachRight, &after);
    g_setJoint(controller, right, &hand);
    TraceRuntime("VR hands: ATTACH_HAND_RIGHT moved " + std::to_string(after.t[1] - before.t[1]) + " m when RIGHT_HAND moved 0.1 m (0.1 = child of the hand)");
}

// Put one hand on its controller: the wrist 8 cm behind the grip point, the
// fingers / index->pinky axes on the controller's aim frame. Children (the
// fingers, the ATTACH_HAND bones and the gun on them) follow.
bool WriteHand(void* controller, const float world[16], int side) {
    float grip[3], fingers[3], down[3];
    if (!GetVrBodyHandTarget(side, grip, fingers, down)) return false;
    const V3 palm = ToModel(world, grip, true);
    const V3 fingerDir = Normalize(ToModel(world, fingers, false));
    const V3 downDir = Normalize(ToModel(world, down, false));
    Joint hand{}, index{}, middle{}, pinky{};
    g_getJoint(controller, kHandBone[side], &hand);
    g_getJoint(controller, kFingerBones[side][0], &index);
    g_getJoint(controller, kFingerBones[side][1], &middle);
    g_getJoint(controller, kFingerBones[side][2], &pinky);
    const V3 wrist{hand.t[0], hand.t[1], hand.t[2]};
    const V3 target{palm.x - fingerDir.x * kWristToPalm, palm.y - fingerDir.y * kWristToPalm, palm.z - fingerDir.z * kWristToPalm};
    // A target far from the animated hand means tracking or mapping is off
    // (menus, teleports, loading): leave the animation alone.
    const V3 reach = Sub(target, wrist);
    if (Dot(reach, reach) > 1.5f * 1.5f) return false;
    float r[3][3], q[4], composed[4];
    AlignFrames(Sub({middle.t[0], middle.t[1], middle.t[2]}, wrist), Sub({pinky.t[0], pinky.t[1], pinky.t[2]}, {index.t[0], index.t[1], index.t[2]}),
        fingerDir, downDir, r);
    MatrixToQuat(r, q);
    QuatMultiply(q, hand.q, composed);
    if (!std::isfinite(composed[0]) || !std::isfinite(composed[3])) return false;
    std::memcpy(hand.q, composed, sizeof(composed));
    hand.t[0] = target.x, hand.t[1] = target.y, hand.t[2] = target.z;
    g_setJoint(controller, kHandBone[side], &hand);
    return true;
}

// --- weapon probe (read-only, trigger file jc3vr_weapon_probe) --------------
//
// Where does the game keep the transform it fires from? The carry sees the
// gun's own world matrix in the draw uploads (exactly RightHandAttach while
// firing); this looks for that matrix inside the objects hanging off Rico's
// inventory slots (O+0x978..+0xA60), following pointers three levels deep,
// and logs every place it is stored, row-major 4x4 or 3x4 column-vector.

struct ProbeHit {
    std::string path;
    float error;
};

bool PlausiblePointer(std::uintptr_t value) { return value > 0x10000 && value < 0x7FF000000000ull && (value & 7) == 0; }

// Max abs difference between the 12 meaningful floats at p and the gun
// matrix g (row-major 4x4), read in the two layouts the engine uses.
float MatrixError(const float* p, const float g[16], bool columnVector) {
    // NaN / garbage must never compare as a match (NaN slips through max/abs).
    for (int k = 0; k != 16; ++k)
        if (!std::isfinite(p[k]) || std::abs(p[k]) > 1e6f) return 1e9f;
    float error = 0.0f;
    for (int i = 0; i != 3; ++i)
        for (int j = 0; j != 3; ++j) error = std::max(error, std::abs((columnVector ? p[i * 4 + j] : p[j * 4 + i]) - g[j * 4 + i]));
    for (int i = 0; i != 3; ++i) error = std::max(error, std::abs((columnVector ? p[i * 4 + 3] : p[12 + i]) - g[12 + i]) * 0.5f);
    return error;
}

void ScanBlock(std::uintptr_t address, std::size_t size, const std::string& path, const float gun[16], std::vector<ProbeHit>& hits) {
    std::vector<float> block(size / 4);
    if (!SafeRead(block.data(), reinterpret_cast<const void*>(address), size)) return;
    for (std::size_t i = 0; i + 16 <= block.size(); ++i) {
        const float* p = block.data() + i;
        for (const bool column : {false, true}) {
            if (!column && std::abs(p[15] - 1.0f) > 1e-3f) continue;
            const float error = MatrixError(p, gun, column);
            if (error < 0.05f) {
                char text[64];
                std::snprintf(text, sizeof(text), " +0x%zX %s", i * 4, column ? "3x4" : "4x4");
                hits.push_back({path + text, error});
            }
        }
    }
}

void RunWeaponProbe() {
    float original[16], carried[16];
    int bone = -1;
    unsigned long long tick = 0;
    if (!GetVrBodyLastCarried(1, original, carried, bone, tick) || GetTickCount64() - tick > 150) {
        TraceRuntime("weapon probe: no right-hand gun carried in the last 150 ms (fire first); not run");
        return;
    }
    const std::uintptr_t rico = VrBodyCharacter();
    if (!rico) return;
    char text[256];
    std::snprintf(text, sizeof(text), "weapon probe: gun on bone %d, game pos (%.3f %.3f %.3f) drawn pos (%.3f %.3f %.3f); Rico O=0x%llX", bone,
        original[12], original[13], original[14], carried[12], carried[13], carried[14], static_cast<unsigned long long>(rico));
    TraceRuntime(text);
    std::snprintf(text, sizeof(text), "weapon probe: game rows x(%.3f %.3f %.3f) y(%.3f %.3f %.3f) z(%.3f %.3f %.3f)", original[0], original[1], original[2],
        original[4], original[5], original[6], original[8], original[9], original[10]);
    TraceRuntime(text);
    {
        float aim[3]{};
        if (SafeRead(aim, reinterpret_cast<const void*>(rico + kAimTargetOffset), sizeof(aim))) {
            const float to[3]{aim[0] - original[12], aim[1] - original[13], aim[2] - original[14]};
            const float length = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
            if (length > 1e-3f) {
                const auto dot = [&](const float* m, int row) { return (m[row * 4] * to[0] + m[row * 4 + 1] * to[1] + m[row * 4 + 2] * to[2]) / length; };
                std::snprintf(text, sizeof(text),
                    "weapon probe: aim point (%.2f %.2f %.2f) %.1f m away; game gun rows . aim dir: x=%.3f y=%.3f z=%.3f; drawn gun rows . aim dir: x=%.3f y=%.3f z=%.3f",
                    aim[0], aim[1], aim[2], length, dot(original, 0), dot(original, 1), dot(original, 2), dot(carried, 0), dot(carried, 1), dot(carried, 2));
                TraceRuntime(text);
            }
        }
        std::snprintf(text, sizeof(text), "weapon probe: drawn rows x(%.3f %.3f %.3f) y(%.3f %.3f %.3f) z(%.3f %.3f %.3f)", carried[0], carried[1], carried[2],
            carried[4], carried[5], carried[6], carried[8], carried[9], carried[10]);
        TraceRuntime(text);
    }
    std::vector<ProbeHit> hits;
    std::size_t scanned = 0;
    for (std::uintptr_t slot = 0x978; slot < 0xA60; slot += 8) {
        std::uintptr_t object = 0;
        if (!SafeRead(&object, reinterpret_cast<const void*>(rico + slot), sizeof(object)) || !PlausiblePointer(object)) continue;
        char root[32];
        std::snprintf(root, sizeof(root), "O+0x%llX", static_cast<unsigned long long>(slot));
        ScanBlock(object, 0x1000, root, original, hits);
        std::vector<std::uintptr_t> level1(0x800 / 8);
        if (!SafeRead(level1.data(), reinterpret_cast<const void*>(object), 0x800)) continue;
        for (std::size_t a = 0; a != level1.size(); ++a) {
            if (!PlausiblePointer(level1[a]) || level1[a] == object) continue;
            char path1[64];
            std::snprintf(path1, sizeof(path1), "%s->+0x%zX", root, a * 8);
            ScanBlock(level1[a], 0x800, path1, original, hits);
            std::vector<std::uintptr_t> level2(0x100 / 8);
            if (!SafeRead(level2.data(), reinterpret_cast<const void*>(level1[a]), 0x100)) continue;
            for (std::size_t b = 0; b != level2.size(); ++b) {
                if (!PlausiblePointer(level2[b]) || level2[b] == object || level2[b] == level1[a]) continue;
                char path2[96];
                std::snprintf(path2, sizeof(path2), "%s->+0x%zX", path1, b * 8);
                ScanBlock(level2[b], 0x800, path2, original, hits);
                ++scanned;
            }
        }
        if (hits.size() > 200) break;
    }
    std::sort(hits.begin(), hits.end(), [](const ProbeHit& a, const ProbeHit& b) { return a.error < b.error; });
    TraceRuntime("weapon probe: " + std::to_string(hits.size()) + " matches (" + std::to_string(scanned) + " level-2 blocks scanned)");
    for (std::size_t i = 0; i < hits.size() && i < 40; ++i) TraceRuntime("weapon probe:   " + hits[i].path + " err=" + std::to_string(hits[i].error));
}

bool CallPrecisionMode(std::uintptr_t player,int mode) {
    __try { g_setAimMode(reinterpret_cast<void*>(player),mode,false); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { g_supportFault=true; return false; }
}
bool CanSupportPrecision(std::uintptr_t rico) {
    __try { return g_canPrecisionAim && g_canPrecisionAim(reinterpret_cast<void*>(rico-8)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { g_supportFault=true; return false; }
}

// Explicit mode changes on the simulation thread, never from Present/XR.
// Only release precision that this grab actually enabled; preserve manual aim.
void ApplySupportPrecision(std::uintptr_t rico) {
    if (!g_setAimMode || !g_canPrecisionAim || g_supportFault.load()) return;
    bool wanted=false;
    std::uintptr_t weapon=0;
    {
        std::lock_guard lock(g_supportMutex);
        wanted=g_supportWanted && GetTickCount64()-g_supportAt<=250;
        weapon=g_supportWeapon;
    }
    wanted=wanted && weapon && weapon==GetSupportGripWeapon() && GetSupportGripProfile(weapon).nativePrecision;
    std::uintptr_t avatar=0, character=0, table=0;
    if (!SafeRead(&avatar,reinterpret_cast<void*>(rico+0x29A8),sizeof(avatar)) || avatar<0x10010) return;
    const auto player=avatar-0x10;
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    int mode=-1;
    if (!SafeRead(&table,reinterpret_cast<void*>(player),sizeof(table)) || table!=image+0x2468868 ||
        !SafeRead(&character,reinterpret_cast<void*>(player+0x110),sizeof(character)) || character!=rico-8 ||
        !SafeRead(&mode,reinterpret_cast<void*>(player+0x140C),sizeof(mode)) || mode<0 || mode>1) return;
    static std::uintptr_t lastPlayer=0,lastRico=0,lastWeapon=0;
    static bool lastWanted=false,owned=false;
    if (player!=lastPlayer || rico!=lastRico) { lastPlayer=player; lastRico=rico; lastWeapon=0; lastWanted=owned=false; }
    if (lastWanted && (!wanted || weapon!=lastWeapon)) {
        if (owned && mode==1 && CallPrecisionMode(player,0)) {
            TraceRuntime("support grip: native precision released"); mode=0;
        }
        owned=false;
    }
    if (wanted && (!lastWanted || weapon!=lastWeapon) && mode==0) {
        if (CanSupportPrecision(rico) && CallPrecisionMode(player,1)) {
            int after=0;
            owned=SafeRead(&after,reinterpret_cast<void*>(player+0x140C),sizeof(after)) && after==1;
            TraceRuntime(owned?"support grip: native precision accepted (camera hint disabled)":"support grip: native precision refused by game state");
        } else TraceRuntime("support grip: native precision unavailable for current character state");
    }
    lastWanted=wanted; lastWeapon=weapon;
}

// The game's pose finalisation for every character; for the local player
// (Rico) both hands are then rewritten onto the controllers.
// Use the current seated skeleton, before head hiding, not the vehicle chase
// camera. The predicate distinguishes seated driving/riding from roof stunts.
VehicleCameraAnchor ReadSeatedCamera(void* self,void* controller,const float* world) {
    if (!g_isRiding || g_vehicleFault.load()) return {};
    __try {
        if (!g_isRiding(self) || g_getSafeIndex(self,0xA877D9CC)!=8 ||
            g_getSafeIndex(self,0xA1C96158)!=7) return {};
        Joint head{},neck{};
        g_getJoint(controller,8,&head);
        g_getJoint(controller,7,&neck);
        // A previous pose-finalization pass may already have hidden the head.
        // The neck is untouched and is the fallback skull-base reference.
        if (head.s[0]<0.1f || head.s[1]<0.1f || head.s[2]<0.1f) {
            std::memcpy(head.t,neck.t,sizeof(head.t));
            head.t[1]+=0.12f;
        }
        return AddAircraftFrame(self,BuildVehicleCameraAnchor(world,head.t));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_vehicleFault=true;
        return {};
    }
}

// Read only the state bits verified against native getter instructions.
// No new detour, state-machine edits or render-thread native getter calls.
VehicleCameraAnchor ReadTraversalCamera(void* self,void* controller,const float* world) {
    if(!g_traversalStateLayoutReady || g_vehicleFault.load()) return {};
    __try {
        const auto character=reinterpret_cast<std::uintptr_t>(self);
        std::uintptr_t vm=0;std::uint64_t flags=0;std::uint32_t high=0;
        if(!SafeRead(&vm,reinterpret_cast<void*>(character+0x2718),sizeof(vm)) || !vm ||
           !SafeRead(&flags,reinterpret_cast<void*>(vm+0xE8),sizeof(flags)) ||
           !SafeRead(&high,reinterpret_cast<void*>(vm+0xF8),sizeof(high))) return {};
        bool helicopter=false;
        if(g_getVehicle) {
            const auto vehicle=reinterpret_cast<std::uintptr_t>(g_getVehicle(self));
            std::uintptr_t table=0;
            helicopter=vehicle && SafeRead(&table,reinterpret_cast<void*>(vehicle+8),sizeof(table)) &&
                table==reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+0x24E4348;
        }
        if(!NeedsTraversalCamera(false,(flags&(1ull<<5))!=0,(flags&(1ull<<33))!=0,
            (high&(1u<<7))!=0,(flags&((1ull<<32)|(1ull<<42)))!=0,helicopter)) return {};
        if(g_getSafeIndex(self,0xA877D9CC)!=8 || g_getSafeIndex(self,0xA1C96158)!=7) return {};
        Joint head{},neck{};g_getJoint(controller,8,&head);g_getJoint(controller,7,&neck);
        if(head.s[0]<0.1f || head.s[1]<0.1f || head.s[2]<0.1f) {
            std::memcpy(head.t,neck.t,sizeof(head.t));head.t[1]+=0.12f;
        }
        auto anchor=BuildVehicleCameraAnchor(world,head.t);
        if(!anchor.valid) return {};
        anchor.traversal=true;
        if(!SetTraversalYawFrame(anchor,anchor.basis)) return {};
        return anchor;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return {}; }
}

void __fastcall UpdatePropEffectsDetour(void* self, float dt) {
    g_originalPropEffects(self, dt);
    if (!self) return;
    const auto base = reinterpret_cast<std::uintptr_t>(self);
    bool local = false;
    void* controller = nullptr;
    float world[16]{};
    if (!SafeRead(&local, reinterpret_cast<const void*>(base + kIsLocalCharacterOffset), sizeof(local)) || !local) return;
    if (!SafeRead(&controller, reinterpret_cast<const void*>(base + kAnimationControllerOffset), sizeof(controller)) || !controller) return;
    if (!SafeRead(world, reinterpret_cast<const void*>(base + kWorldMatrixT1Offset), sizeof(world)) || std::abs(world[15] - 1.0f) > 1e-3f) return;
    // Rico only (the vtable-matched rig), and only once vr_body found him.
    const std::uintptr_t rico = VrBodyCharacter();
    if (!rico || base != rico - kCharacterBaseAdjust) return;
    PublishExteriorPullHelicopter(self);
    CaptureHijackState(self);
    PhysicalHijackCharacterTick(base);
    const auto seated=ReadSeatedCamera(self,controller,world);
    const auto anchor=seated.valid?seated:ReadTraversalCamera(self,controller,world);
    PublishVehicleCameraAnchor(anchor, rico);
    ApplyPocketRequest(self, anchor.valid);
    static bool wasTraversal=false;
    if(anchor.traversal!=wasTraversal) {
        TraceRuntime(anchor.traversal?"traversal first-person: native head anchor active (freefall/grapple/helicopter exterior)":
            "traversal first-person: released to seated/on-foot camera");
        wasTraversal=anchor.traversal;
    }
    static bool wasSeated=false;
    if (seated.valid!=wasSeated) {
        TraceRuntime(seated.valid?"vehicle first-person: seated head anchor active":"vehicle first-person: seated anchor released; normal on-foot camera restored");
        wasSeated=seated.valid;
    }
    ApplySupportPrecision(rico);
    // The render-side palette is not the only source of skinning uploads.
    // Hide at pose finalization too, before alternate-frame copies are made.
    // This changes only Rico's validated head subtree; controller hand writes
    // remain separately gated and off in the production configuration.
    std::uint32_t headBones[32]{};
    const auto headCount = !g_headHideFault.load() ? GetVrBodyHeadBones(headBones, 32) : 0;
    if (headCount) {
        __try {
            if (g_getSafeIndex(self, 0xA877D9CC) == 8 && g_getSafeIndex(self, 0xA1C96158) == 7) {
                Joint neck{};
                g_getJoint(controller, 7, &neck);
                bool sane = true;
                for (int i = 0; i != 3; ++i) sane = sane && std::isfinite(neck.t[i]) && std::abs(neck.t[i]) < 5.0f;
                if (sane) {
                    for (std::size_t i = 0; i != headCount; ++i) {
                        Joint head{};
                        g_getJoint(controller, headBones[i], &head);
                        std::memcpy(head.t, neck.t, sizeof(head.t));
                        // Nonzero scale avoids a singular transform in any
                        // later model/local conversion. Geometry is sub-mm.
                        head.s[0] = head.s[1] = head.s[2] = 0.0001f;
                        g_setJoint(controller, headBones[i], &head);
                    }
                    ++g_nativeHeadHides;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_headHideFault = true;
        }
    }
    if (!g_options.handsInGame || !g_handsActive.load() || g_rigMismatch.load()) return;
    __try {
        CheckRig(self, controller);
        if (g_rigMismatch.load()) return;
        for (int side = 0; side != 2; ++side) {
            if (WriteHand(controller, world, side)) ++g_handWrites[side];
            else ++g_handSkips;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_handsActive = false;
        g_handFault = true;  // logged from the present tick (no unwinding here)
    }
}
}

void ConfigureVrWeapons(const VrWeaponOptions& options) {
    g_options = options;
    TraceRuntime(std::string("VR weapons: aim point from the right controller: ") + (options.aimFromController ? "on" : "off") +
        ", hands in the game pose: " + (options.handsInGame ? "on" : "off") + ", game's guns to the drawn hands: " + (options.gunsToController ? "on" : "off") +
        ", controller weapon roll: " + (options.controllerRoll ? "on" : "off (animated roll)"));
}

bool InstallVrWeaponHooks() {
#if JC3VR_HAVE_MINHOOK
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // Guard complete bit accessors, including shift and return, not merely
    // their shared prologue. These prove the VM pointer/word/bit layout.
    const std::uint8_t fall[]{0x48,0x8B,0x81,0x18,0x27,0,0,0x8B,0x88,0xE8,0,0,0,0x48,0xC1,0xE9,5,0x83,0xE1,1,0x0F,0xB6,0xC1,0xC3};
    const std::uint8_t fast[]{0x48,0x8B,0x81,0x18,0x27,0,0,0x48,0x8B,0x80,0xE8,0,0,0,0x48,0xC1,0xE8,0x21,0x83,0xE0,1,0xC3};
    const std::uint8_t hang[]{0x48,0x8B,0x81,0x18,0x27,0,0,0x8B,0x88,0xF8,0,0,0,0x48,0xC1,0xE9,7,0x83,0xE1,1,0x0F,0xB6,0xC1,0xC3};
    g_traversalStateLayoutReady=BytesMatch(base+0x75F570,fall,sizeof(fall)) &&
        BytesMatch(base+0x75F910,fast,sizeof(fast)) && BytesMatch(base+0x75F790,hang,sizeof(hang));
    TraceRuntime(g_traversalStateLayoutReady?"traversal first-person: native state layout byte guards passed":
        "traversal first-person: state layout rejected; existing camera retained");
    if(!g_getVehicle && BytesMatch(base+kGetVehicleRva,kGetVehicleBytes,sizeof(kGetVehicleBytes)) &&
       BytesMatch(base+0xF232B0,kVehicleTransformBytes,sizeof(kVehicleTransformBytes))) {
        g_getVehicle=reinterpret_cast<GetVehicleFn>(base+kGetVehicleRva);
        TraceRuntime("aircraft: native vehicle getter/transform storage byte guards passed");
    }
    if (!g_isRiding && BytesMatch(base+kIsRidingRva,kIsRidingBytes,sizeof(kIsRidingBytes))) {
        g_isRiding=reinterpret_cast<IsRidingFn>(base+kIsRidingRva);
        TraceRuntime("vehicle first-person: native seated-state predicate byte guard passed");
    }
    if (!g_setAimMode && BytesMatch(base+kSetAimModeRva,kSetAimModeBytes,sizeof(kSetAimModeBytes)) &&
        BytesMatch(base+kCanPrecisionAimRva,kCanPrecisionAimBytes,sizeof(kCanPrecisionAimBytes))) {
        g_setAimMode=reinterpret_cast<SetAimModeFn>(base+kSetAimModeRva);
        g_canPrecisionAim=reinterpret_cast<CanPrecisionAimFn>(base+kCanPrecisionAimRva);
        TraceRuntime("support grip: precision setter/eligibility byte guards passed");
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (g_pocketWanted.load() && !g_pocketInstalled.load()) {
        constexpr std::uint8_t draw[]{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18};
        constexpr std::uint8_t holster[]{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20};
        constexpr std::uint8_t call1[]{0xE8,0x58,0xD7,0xE2,0xFF}, call2[]{0xE8,0x54,0xD6,0xE2,0xFF};
        if (BytesMatch(base + kPocketDrawRva, draw, sizeof(draw)) &&
            BytesMatch(base + kPocketHolsterRva, holster, sizeof(holster)) &&
            BytesMatch(base + 0x933143, call1, sizeof(call1)) && BytesMatch(base + 0x933247, call2, sizeof(call2))) {
            void* target = reinterpret_cast<void*>(base + kPocketDrawRva);
            g_pocketImage = base;
            g_pocketHolster = reinterpret_cast<PocketHolsterFn>(base + kPocketHolsterRva);
            if (MH_CreateHook(target, reinterpret_cast<void*>(&PocketDrawDetour), reinterpret_cast<void**>(&g_pocketDraw)) == MH_OK) {
                if (MH_EnableHook(target) == MH_OK) g_pocketInstalled = true;
                else MH_RemoveHook(target);
            }
        }
        TraceRuntime(g_pocketInstalled ? "Rico pocket: native equipment entry points armed" :
                     "Rico pocket: native equipment guards failed; input unchanged");
    }
    if(!g_vehicleAimInstalled && g_getVehicle && BytesMatch(base+kVehicleAimRva,kVehicleAimBytes,sizeof(kVehicleAimBytes))) {
        auto* target=reinterpret_cast<void*>(base+kVehicleAimRva);
        auto status=MH_CreateHook(target,reinterpret_cast<void*>(&VehicleAimDetour),reinterpret_cast<void**>(&g_originalVehicleAim));
        const bool created=status==MH_OK;
        if(created) status=MH_EnableHook(target);
        g_vehicleAimInstalled=status==MH_OK;
        if(created && !g_vehicleAimInstalled) MH_RemoveHook(target);
        TraceRuntime("helicopter: guarded native aim hook result="+std::to_string(static_cast<int>(status)));
    }
    if(!g_sniperHookInstalled && BytesMatch(base+kSniperAimRva,kSniperAimBytes,sizeof(kSniperAimBytes)) &&
       BytesMatch(base+kSetAimModeRva,kSetAimModeBytes,sizeof(kSetAimModeBytes))) {
        auto* target=reinterpret_cast<void*>(base+kSniperAimRva);
        auto* playerTarget=reinterpret_cast<void*>(base+kSetAimModeRva);
        MH_STATUS status=MH_CreateHook(target,reinterpret_cast<void*>(&SniperAimDetour),reinterpret_cast<void**>(&g_originalSniperAim));
        const bool sniperCreated=status==MH_OK;
        bool playerCreated=false;
        if(status==MH_OK) {
            status=MH_CreateHook(playerTarget,reinterpret_cast<void*>(&ScopePlayerAimDetour),reinterpret_cast<void**>(&g_originalScopePlayerAim));
            playerCreated=status==MH_OK;
        }
        if(status==MH_OK) status=MH_EnableHook(playerTarget);
        if(status==MH_OK) status=MH_EnableHook(target);
        g_sniperHookInstalled=status==MH_OK;
        if(!g_sniperHookInstalled) {
            if(sniperCreated) { MH_DisableHook(target);MH_RemoveHook(target); }
            if(playerCreated) { MH_DisableHook(playerTarget);MH_RemoveHook(playerTarget); }
        }
        TraceRuntime("VR scope: guarded sniper aim hook result="+std::to_string(static_cast<int>(status)));
    }
    InstallVrGrappleHooks();
    if (!g_installed && g_options.aimFromController) {
        auto* target = reinterpret_cast<void*>(base + kUpdateWeaponAimingRva);
        if (!BytesMatch(base + kUpdateWeaponAimingRva, kUpdateWeaponAimingBytes, sizeof(kUpdateWeaponAimingBytes))) {
            TraceRuntime("VR weapons: UpdateWeaponAiming byte guard failed; controller aim not installed");
        } else {
            MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&UpdateWeaponAimingDetour), reinterpret_cast<void**>(&g_original));
            if (status == MH_OK) status = MH_EnableHook(target);
            g_installed = status == MH_OK;
            TraceRuntime("VR weapons hooks: UpdateWeaponAiming=" + std::to_string(static_cast<int>(status)) + " (0 = MH_OK)");
        }
    }
    if (!g_placeInstalled && g_options.gunsToController) {
        if (!BytesMatch(base + kPlaceWeaponDataRva, kPlaceWeaponDataBytes, sizeof(kPlaceWeaponDataBytes)) ||
            !BytesMatch(base + kGetSafeIndexRva, kGetSafeIndexBytes, sizeof(kGetSafeIndexBytes))) {
            TraceRuntime("VR weapons: CWeaponData placement byte guard failed; game guns stay on the animated hands");
        } else {
            g_getSafeIndex = reinterpret_cast<GetSafeIndexFn>(base + kGetSafeIndexRva);
            auto* target = reinterpret_cast<void*>(base + kPlaceWeaponDataRva);
            MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&PlaceWeaponDataDetour), reinterpret_cast<void**>(&g_originalPlace));
            if (status == MH_OK) status = MH_EnableHook(target);
            g_placeInstalled = status == MH_OK;
            TraceRuntime("VR weapons hooks: CWeaponData placement=" + std::to_string(static_cast<int>(status)) + " (0 = MH_OK)");
        }
    }
    if (!g_handsInstalled && (g_options.handsInGame || VrBodyHeadHideEnabled())) {
        const bool guards = BytesMatch(base + kUpdatePropEffectsRva, kUpdatePropEffectsBytes, sizeof(kUpdatePropEffectsBytes)) &&
            BytesMatch(base + kGetSafeIndexRva, kGetSafeIndexBytes, sizeof(kGetSafeIndexBytes)) &&
            BytesMatch(base + kGetJointRva, kGetJointBytes, sizeof(kGetJointBytes)) &&
            BytesMatch(base + kSetJointRva, kSetJointBytes, sizeof(kSetJointBytes));
        if (!guards) {
            TraceRuntime("VR hands: byte guard failed (UpdatePropEffects / GetSafeIndex / GetJoint / SetJoint); hands stay render-side");
        } else {
            g_getSafeIndex = reinterpret_cast<GetSafeIndexFn>(base + kGetSafeIndexRva);
            g_getJoint = reinterpret_cast<JointFn>(base + kGetJointRva);
            g_setJoint = reinterpret_cast<JointFn>(base + kSetJointRva);
            auto* target = reinterpret_cast<void*>(base + kUpdatePropEffectsRva);
            MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&UpdatePropEffectsDetour), reinterpret_cast<void**>(&g_originalPropEffects));
            if (status == MH_OK) status = MH_EnableHook(target);
            g_handsInstalled = status == MH_OK;
            TraceRuntime("VR pose hooks: UpdatePropEffects=" + std::to_string(static_cast<int>(status)) + " (0 = MH_OK); native head hide=" + (VrBodyHeadHideEnabled() ? "on" : "off") + "; game hands=" + (g_options.handsInGame ? "on" : "off"));
        }
    }
    if(g_placeInstalled && g_options.controllerRoll) InstallDualWieldHooks();
    if(g_placeInstalled) InstallVrC4Hooks();
    if(g_handsInstalled) InstallPhysicalHijackHooks();
    return g_installed || g_handsInstalled || g_placeInstalled;
#else
    return false;
#endif
}

void PublishHelicopterGaze(std::uintptr_t vehicle,const float target[3]) {
    std::lock_guard lock(g_airGazeMutex);
    g_airGazeVehicle=0;
    if(!vehicle || !target) return;
    for(int c=0;c<3;++c) if(!std::isfinite(target[c]) || std::abs(target[c])>1e7f) return;
    std::memcpy(g_airGazeTarget,target,sizeof(g_airGazeTarget));
    g_airGazeVehicle=vehicle;g_airGazeAt=GetTickCount64();
}
void SetWeaponAimRay(bool valid, const float origin[3], const float direction[3]) {
    std::lock_guard lock(g_rayMutex);
    g_rayValid = valid;
    if (!valid) return;
    std::memcpy(g_rayOrigin, origin, sizeof(g_rayOrigin));
    std::memcpy(g_rayDirection, direction, sizeof(g_rayDirection));
    g_rayAt = GetTickCount64();
}

bool GameSimulationPaused() {
    // Unknown (hook not installed, or too few placements yet): never paused.
    if (!g_placeInstalled || g_placementIndex.load(std::memory_order_relaxed) < kPlacementRing) return false;
    ULONGLONG oldest = ~0ull;
    for (const auto& time : g_placementTimes) oldest = std::min(oldest, time.load(std::memory_order_relaxed));
    return GetTickCount64() - oldest > 400;
}

bool WeaponAimFromController() { return g_installed && g_active.load() && g_options.aimFromController; }

std::array<int, 4> GetWeaponSelectorIconIds() {
    std::array<int, 4> ids{-1,-1,-1,-1};
    const auto rico = VrBodyCharacter();
    if (!rico || !g_installed) return ids;
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // Installed settings/keymap_gamepad.bin: UP dual, DOWN explosive,
    // LEFT two-handed-special, RIGHT two-handed. No input behavior changed.
    const auto& slots=kWeaponSelectorSlots;
    for (int direction=0; direction<4; ++direction) {
        std::uintptr_t weapon=0, table=0;
        int slot=-1;
        std::int16_t icon=-1;
        if (!SafeRead(&weapon,reinterpret_cast<void*>(rico+0x998+16*slots[direction]),sizeof(weapon)) || !weapon ||
            !SafeRead(&table,reinterpret_cast<void*>(weapon),sizeof(table)) || table<image+0x2000000 || table>=image+0x3000000 ||
            !SafeRead(&slot,reinterpret_cast<void*>(weapon+0x420),sizeof(slot)) || slot!=slots[direction] ||
            !SafeRead(&icon,reinterpret_cast<void*>(weapon+0x41C),sizeof(icon)) || icon<=0 || icon>4096) continue;
        // Native UI call at exe+991250 reads +414 from its embedded game-object
        // base (weapon+8). Live inventory pointers instead require +41C.
        ids[direction]=icon;
    }
    return ids;
}

std::uintptr_t GetSupportGripWeapon() {
    const auto rico=VrBodyCharacter();
    if (!rico || !g_options.gunsToController || !g_gunsActive.load() || !WeaponAimFromController() ||
        GetVehicleCameraAnchor().valid) return 0;
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t found=0;
    // Reject ambiguous snapshots during a weapon switch. Dual-wield, C4,
    // vehicles and holstered inventory entries cannot become a support grab.
    for (const int slot:{0,1,4,6}) {
        std::uintptr_t weapon=0,table=0;
        int equip=-1,state=-1;
        if (!SafeRead(&weapon,reinterpret_cast<void*>(rico+0x998+16*slot),sizeof(weapon)) || !weapon) continue;
        if (!SafeRead(&table,reinterpret_cast<void*>(weapon),sizeof(table)) || table<image+0x2000000 || table>=image+0x3000000 ||
            !SafeRead(&equip,reinterpret_cast<void*>(weapon+0x420),sizeof(equip)) || equip!=slot ||
            !SafeRead(&state,reinterpret_cast<void*>(weapon+0x424),sizeof(state)) || state<0 || state>10) return 0;
        if (state!=1 && state!=2) continue;
        if (slot!=1 && slot!=6) return 0;
        // A sniper may use the physical support grip, but never its native
        // fullscreen precision mode (ApplySupportPrecision excludes it).
        if (!SupportProfileForTable(table-image).eligible) return 0;
        if (found) return 0;
        bool disabled=true;
        if (!SafeRead(&disabled,reinterpret_cast<void*>(weapon+0x466),sizeof(disabled)) || disabled) return 0;
        found=weapon;
    }
    return found;
}

int GetEquippedWeaponIconId() {
    const auto rico=VrBodyCharacter();if(!rico || !g_installed) return -1;
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for(const int slot:{0,1,4,6}) {
        std::uintptr_t weapon=0,table=0;int equip=-1,state=-1;std::int16_t icon=-1;
        if(!SafeRead(&weapon,reinterpret_cast<void*>(rico+0x998+16*slot),sizeof(weapon)) || !weapon) continue;
        if(!SafeRead(&table,reinterpret_cast<void*>(weapon),sizeof(table)) || table<image+0x2000000 || table>=image+0x3000000 ||
           !SafeRead(&equip,reinterpret_cast<void*>(weapon+0x420),sizeof(equip)) || equip!=slot ||
           !SafeRead(&state,reinterpret_cast<void*>(weapon+0x424),sizeof(state)) || (state!=1 && state!=2) ||
           !SafeRead(&icon,reinterpret_cast<void*>(weapon+0x41C),sizeof(icon)) || icon<=0 || icon>4096) continue;
        return icon;
    }
    return -1;
}

bool RicoInventoryIsHolstered() {
    const auto rico = VrBodyCharacter();
    if (!rico || !g_installed || GetVehicleCameraAnchor().valid) return false;
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (const int slot : kWeaponSelectorSlots) {
        std::uintptr_t weapon = 0, table = 0;
        int equip = -1, state = -1;
        if (!SafeRead(&weapon, reinterpret_cast<void*>(rico + 0x998 + 16 * slot), sizeof(weapon))) return false;
        if (!weapon) continue;
        if (!SafeRead(&table, reinterpret_cast<void*>(weapon), sizeof(table)) ||
            table < image + 0x2000000 || table >= image + 0x3000000 ||
            !SafeRead(&equip, reinterpret_cast<void*>(weapon + 0x420), sizeof(equip)) || equip != slot ||
            !SafeRead(&state, reinterpret_cast<void*>(weapon + 0x424), sizeof(state)) ||
            !RicoWeaponIsHolstered(state)) return false;
    }
    return true;
}

void ConfigureRicoPocketHolster(bool enabled) { g_pocketWanted = enabled; }

bool RicoPocketHolsterAvailable() {
    return g_pocketInstalled.load() && !g_pocketFault.load();
}

void UpdateRicoPocketRequest(bool hands, PocketAction action) {
    if (!RicoPocketHolsterAvailable()) return;
    const auto now = GetTickCount64();
    g_pocketHandsUntil = hands ? now + 250 : 0;
    if (action != PocketAction::None) {
        g_pocketRequestAt = now;
        g_pocketAction = action;
    }
}

SupportGripProfile GetSupportGripProfile(std::uintptr_t weapon) {
    std::uintptr_t table=0;
    if(!weapon || !SafeRead(&table,reinterpret_cast<void*>(weapon),sizeof(table))) return {};
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    return table>=image?SupportProfileForTable(table-image):SupportGripProfile{};
}

void RequestSupportGripPrecision(bool enabled,std::uintptr_t weapon) {
    std::lock_guard lock(g_supportMutex);
    g_supportWanted=enabled; g_supportWeapon=weapon; g_supportAt=GetTickCount64();
}

std::uintptr_t GetVrSniperWeapon() {
    if(!g_sniperHookInstalled || !WeaponAimFromController() || !g_options.gunsToController || !g_gunsActive.load() ||
       GetVehicleCameraAnchor().valid) return 0;
    const auto rico=VrBodyCharacter();
    if(!rico) return 0;
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t found=0;
    for(int slot:{0,1,4,6}) {
        std::uintptr_t weapon=0,table=0;int equip=-1,state=-1;
        if(!SafeRead(&weapon,reinterpret_cast<void*>(rico+0x998+slot*16),sizeof(weapon)) || !weapon) continue;
        if(!SafeRead(&table,reinterpret_cast<void*>(weapon),sizeof(table)) ||
           !SafeRead(&equip,reinterpret_cast<void*>(weapon+0x420),sizeof(equip)) || equip!=slot ||
           !SafeRead(&state,reinterpret_cast<void*>(weapon+0x424),sizeof(state))) return 0;
        if(state!=1 && state!=2) continue;
        if(found || table!=image+kSniperVtableRva) return 0;
        bool disabled=true;
        if(!SafeRead(&disabled,reinterpret_cast<void*>(weapon+0x466),sizeof(disabled)) || disabled) return 0;
        found=weapon;
    }
    return found;
}
void PublishVrScopeReady(std::uintptr_t weapon) { g_scopeWeapon=weapon;g_scopeAt=GetTickCount64(); }
bool VrScopeReady(std::uintptr_t weapon) { return weapon && g_scopeWeapon.load()==weapon && GetTickCount64()-g_scopeAt.load()<250; }
SniperOpticMount GetVrSniperOpticMount(std::uintptr_t weapon) {
    if(!weapon || weapon!=GetVrSniperWeapon()) return {};
    std::lock_guard lock(g_opticMutex);
    if(g_opticWeapon!=weapon || GetTickCount64()-g_opticAt>200) return {};
    return g_opticMount;
}

void ConfigureHelicopterPullHijack(bool enabled) {
    g_pullHijackEnabled.store(enabled);
    std::lock_guard lock(g_pullContextMutex);g_pullHelicopter=0;g_pullContextAt=0;
}
std::uintptr_t GetExteriorPullHelicopter() {
    if(!g_pullHijackEnabled.load())return 0;
    std::lock_guard lock(g_pullContextMutex);
    return GetTickCount64()-g_pullContextAt<=200?g_pullHelicopter:0;
}
void VrWeaponsPresentTick() {
    HijackStateProbePresent();
    WeaponAttachmentProbePresent();
    DualWieldPresentTick();
    static bool vehicleFaultReported=false;
    if(g_vehicleFault.load() && !vehicleFaultReported) {
        vehicleFaultReported=true;
        TraceRuntime("vehicle first-person: seated pose read fault; anchor disabled for this process");
    }
    VrGrapplePresentTick();
    static bool supportFaultReported=false;
    if (g_supportFault.load() && !supportFaultReported) {
        supportFaultReported=true;
        TraceRuntime("support grip: native precision call faulted; support precision disabled for this process");
    }
    // Shift+F7: controller aim on/off (plain F7 pauses head-follow).
    // Shift+F8: hands in the game pose on/off (plain F8 swaps the eye phase).
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    static bool f7WasDown = false, f8WasDown = false;
    const bool f7Down = shift && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool f8Down = shift && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f7Down && !f7WasDown) {
        g_active.store(!g_active.load());
        TraceRuntime(std::string("Shift+F7: weapon aim from controller ") + (g_active.load() ? "ON" : "OFF (engine camera steering)"));
    }
    static bool f9WasDown = false;
    const bool f9Down = shift && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9Down && !f9WasDown) {
        g_gunsActive.store(!g_gunsActive.load());
        TraceRuntime(std::string("Shift+F9: game's guns moved to the drawn hands ") + (g_gunsActive.load() ? "ON" : "OFF"));
    }
    f9WasDown = f9Down;
    if (f8Down && !f8WasDown) {
        g_handsActive.store(!g_handsActive.load());
        TraceRuntime(std::string("Shift+F8: hands in the game pose ") + (g_handsActive.load() ? "ON" : "OFF (render-side carry only)"));
    }
    f7WasDown = f7Down;
    f8WasDown = f8Down;
    SetVrBodyGunsPlacedInGame(g_placeInstalled && g_options.gunsToController && g_gunsActive.load());
    static std::uint64_t probePresents = 0;
    if (++probePresents % 10 == 0) {
        static const std::wstring trigger = [] {
            wchar_t base[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
            return length && length < MAX_PATH ? std::wstring(base) + L"\\JC3VR\\jc3vr_weapon_probe" : std::wstring();
        }();
        if (!trigger.empty() && GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(trigger.c_str());
            RunWeaponProbe();
        }
        static const std::wstring dump = trigger.empty() ? std::wstring() : trigger.substr(0, trigger.size() - std::wcslen(L"jc3vr_weapon_probe")) + L"jc3vr_guns_dump";
        if (!dump.empty() && GetFileAttributesW(dump.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(dump.c_str());
            g_gunDump = true;
        }
        // Unattended A/B of the gun move (same as Shift+F9): jc3vr_guns_toggle.
        static const std::wstring toggle = trigger.empty() ? std::wstring() : trigger.substr(0, trigger.size() - std::wcslen(L"jc3vr_weapon_probe")) + L"jc3vr_guns_toggle";
        if (!toggle.empty() && GetFileAttributesW(toggle.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(toggle.c_str());
            g_gunsActive.store(!g_gunsActive.load());
            TraceRuntime(std::string("jc3vr_guns_toggle: game's guns moved to the drawn hands ") + (g_gunsActive.load() ? "ON" : "OFF") +
                " moved=" + std::to_string(g_gunsMoved.load()) + " unmatched=" + std::to_string(g_gunsUnmatched.load()));
        }
    }
    {
        const std::uintptr_t rico = VrBodyCharacter();
        WeaponWatchTick(rico ? FindHeldWeaponData(rico) : 0);
    }
    if (g_handFault.exchange(false)) TraceRuntime("VR hands: exception in the hand write; disabled (Shift+F8 re-enables)");
    static std::uint64_t presents = 0;
    if (++presents % 1800 == 0)
        TraceRuntime("VR weapons: aim points replaced=" + std::to_string(g_replaced.load()) + " untouched=" + std::to_string(g_untouched.load()) +
            " active=" + (g_active.load() ? "1" : "0") + "; hand writes L=" + std::to_string(g_handWrites[0].load()) + " R=" +
            std::to_string(g_handWrites[1].load()) + " skipped=" + std::to_string(g_handSkips.load()) + " active=" + (g_handsActive.load() ? "1" : "0") +
            "; guns moved=" + std::to_string(g_gunsMoved.load()) + " non_hand_placements=" + std::to_string(g_gunsUnmatched.load()) +
            " mount_offset_fallbacks=" + std::to_string(g_mountFallbacks.load()) + " active=" + (g_gunsActive.load() ? "1" : "0"));
    if (presents % 1800 == 0)
        TraceRuntime("VR native head hide: finalized_poses=" + std::to_string(g_nativeHeadHides.load()) + " fault=" + (g_headHideFault.load() ? "1" : "0"));
}
}
