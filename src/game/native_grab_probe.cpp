#include "game/native_grab_probe.h"
#include "game/camera_capture.h"
#include "game/pocket_holster.h"
#include "game/vehicle_camera.h"
#include "game/vr_body_rig.h"
#include "game/vr_weapons.h"
#include "core/logging.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace jc3vr {
namespace {
constexpr std::uintptr_t kNativeAimReturn = 0xCE599A;
constexpr std::uintptr_t kPhysicsSingleton = 0x2EDC120;
constexpr std::uintptr_t kRayInfoVtable = 0x23452C8;
constexpr std::uintptr_t kRigidBodyVtable = 0x2350E38;
constexpr std::uint32_t kInvalidBody = 0x7fffffff;
using RayInfoConstruct = void* (__fastcall*)(void*);
using RayInfoDestroy = void* (__fastcall*)(void*, unsigned);

struct Hit {
    std::uint32_t body = kInvalidBody, flags = 0, motion = 0;
    std::uintptr_t instance = 0, instanceTable = 0;
    float fraction = 1.0f, normal[3]{}, velocity[3]{};
    bool physicalBody = false, velocityValid = false, hit = false;
};
std::atomic<bool> g_enabled = false, g_fault = false;
std::atomic<unsigned> g_samples = 0;
std::atomic<ULONGLONG> g_pending[2]{};
std::once_flag g_configure;
std::uintptr_t g_image = 0;
float g_reach = 0.15f;
RayInfoConstruct g_construct = nullptr;
RayInfoDestroy g_destroy = nullptr;
std::filesystem::path g_armFile;

bool ReadBytes(std::uintptr_t address, void* out, std::size_t length) {
    if (address < 0x10000) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), length);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template<class T> bool Read(std::uintptr_t address, T& out) { return ReadBytes(address, &out, sizeof(out)); }
bool Match(std::uintptr_t rva, const unsigned char* expected, std::size_t length) {
    unsigned char actual[16]{};
    return length <= sizeof(actual) && ReadBytes(g_image + rva, actual, length) &&
           std::memcmp(actual, expected, length) == 0;
}

void ReadNativeBody(std::uintptr_t world, Hit& hit) {
    std::uintptr_t bodies = 0, motions = 0;
    std::uint32_t bodyCapacity = 0;
    // Same allocation/flags gate as the native WireEnd ray consumer.
    if (hit.body == kInvalidBody || !Read(world + 0x2C, bodyCapacity) ||
        hit.body >= (bodyCapacity & 0x3fffffff) || !Read(world + 0x20, bodies) || !bodies) return;
    const auto body = bodies + static_cast<std::uintptr_t>(hit.body) * 0x90;
    if (!Read(body + 0x40, hit.flags) || !(hit.flags & 3) || !Read(body + 0x68, hit.motion)) return;
    hit.physicalBody = true;
    // The native velocity accessors resolve the motion index directly from
    // this live body. Keep an additional sanity bound on index arithmetic.
    if (!(hit.flags & 2) || !hit.motion || hit.motion > 0x100000 ||
        !Read(world + 0xE0, motions) || !motions) return;
    hit.velocityValid = Read(motions + static_cast<std::uintptr_t>(hit.motion) * 0x80 + 0x40, hit.velocity) &&
        std::all_of(hit.velocity, hit.velocity + 3, [](float value) { return std::isfinite(value); });
}

bool RunQuery(void* physics, std::uintptr_t world, NativeGrabCast cast,
              const NativeGrabRay& ray, float reach, Hit& hit) {
    // The engine constructs this stack record in its aim, wire and C4 paths.
    // Reserve extra stack space, call the actual constructor and non-deleting
    // destructor, and keep all borrowed pointers inside this callback.
    alignas(16) unsigned char result[0x50]{};
    const auto address = reinterpret_cast<std::uintptr_t>(result);
    bool prepared = false;
    __try {
        prepared = g_construct(result) == result;
        if (!prepared || *reinterpret_cast<std::uintptr_t*>(result) != g_image + kRayInfoVtable) return false;
        hit.hit = cast(physics, &ray, 0, reach, result, nullptr, 0, 21);
        if (!hit.hit) return true;
        if (!Read(address + 0x14, hit.fraction) || !Read(address + 0x08, hit.normal) ||
            !Read(address + 0x30, hit.body) || !Read(address + 0x38, hit.instance) ||
            !std::isfinite(hit.fraction) || hit.fraction < 0 || hit.fraction > 1) return false;
        if (hit.instance) Read(hit.instance, hit.instanceTable);
        ReadNativeBody(world, hit);
        return true;
    } __finally {
        if (prepared) g_destroy(result, 0);
    }
}

bool GuardedQuery(void* physics, std::uintptr_t world, NativeGrabCast cast,
                  NativeGrabRay ray, float reach, Hit& hit) {
    __try { return RunQuery(physics, world, cast, ray, reach, hit); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Report(const char* label, void* physics, std::uintptr_t world, NativeGrabCast cast,
            NativeGrabRay ray, float reach) {
    if (g_fault || g_samples.fetch_add(1) >= 16) return;
    const float length = std::hypot(ray.direction[0], ray.direction[1], ray.direction[2]);
    if (!std::isfinite(length) || length < 0.5f || length > 1.5f ||
        !std::all_of(ray.origin, ray.origin + 3, [](float v) { return std::isfinite(v); })) return;
    for (float& value : ray.direction) value /= length;
    Hit hit;
    if (!GuardedQuery(physics, world, cast, ray, reach, hit)) {
        g_fault = true;
        TraceRuntime("native grab probe: query/record guard failed; further samples disabled");
        return;
    }
    const auto type = hit.instanceTable >= g_image ? hit.instanceTable - g_image : 0;
    const char* reason = !hit.hit ? "miss" : !hit.physicalBody ? "no-live-body" :
        !(hit.flags & 2) || !hit.motion ? "no-dynamic-motion" : "entity-allowlist-unverified";
    float point[3]{};
    if (hit.hit) for (int i = 0; i != 3; ++i) point[i] = ray.origin[i] + ray.direction[i] * reach * hit.fraction;
    char line[560]{};
    std::snprintf(line, sizeof(line),
        "native grab probe: source=%s tid=%lu reason=%s body=%08X flags=%08X motion=%u instance=%p vtable_rva=%llX exact_rigid=%d distance=%.4f point=[%.3f,%.3f,%.3f] normal=[%.3f,%.3f,%.3f] velocity_valid=%d vel=[%.3f,%.3f,%.3f]",
        label, GetCurrentThreadId(), reason, hit.body, hit.flags, hit.motion, reinterpret_cast<void*>(hit.instance),
        static_cast<unsigned long long>(type), type == kRigidBodyVtable, hit.fraction * reach,
        point[0], point[1], point[2], hit.normal[0], hit.normal[1], hit.normal[2], hit.velocityValid,
        hit.velocity[0], hit.velocity[1], hit.velocity[2]);
    TraceRuntime(line);
}
}

void ConfigureNativeGrabProbe(bool enabled, float reachMeters, float unitsPerMeter) {
    if (!enabled) return;
    std::call_once(g_configure, [&] {
        g_image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        constexpr unsigned char aimCall[]{0xE8,0xA6,0x0D,0x5A,0xFF};
        constexpr unsigned char construct[]{0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8D,0x05,0x5B,0xA9,0x16,0x02};
        constexpr unsigned char destroy[]{0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8D,0x05,0xDB,0xA8,0x16,0x02};
        if (!Match(kNativeAimReturn - 5, aimCall, sizeof(aimCall)) ||
            !Match(0x1D95A0, construct, sizeof(construct)) || !Match(0x1D9620, destroy, sizeof(destroy))) {
            TraceRuntime("native grab probe: executable guard failed");
            return;
        }
        if (!std::isfinite(reachMeters) || !std::isfinite(unitsPerMeter) ||
            unitsPerMeter <= 0 || unitsPerMeter > 5) return;
        g_reach = std::clamp(reachMeters, 0.06f, 0.30f) * unitsPerMeter;
        g_construct = reinterpret_cast<RayInfoConstruct>(g_image + 0x1D95A0);
        g_destroy = reinterpret_cast<RayInfoDestroy>(g_image + 0x1D9620);
        wchar_t local[MAX_PATH]{};
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (length && length < MAX_PATH)
            g_armFile = std::filesystem::path(local) / L"JC3VR/native_grab_probe_arm";
        g_enabled = true;
        TraceRuntime("native grab probe: read-only contacts armed by fresh grip or flat-screen marker, max 16 samples");
    });
}

void PublishNativeGrabProbeGrips(bool handsMode, float left, float right) {
    if (!g_enabled || g_fault) return;
    static bool released[2]{};
    const float grip[]{left, right};
    for (int side = 0; side < 2; ++side) {
        if (!handsMode || !std::isfinite(grip[side])) {
            released[side] = false;
            g_pending[side] = 0;
        } else if (grip[side] <= 0.25f) {
            released[side] = true;
        } else if (released[side] && grip[side] >= 0.72f) {
            released[side] = false;
            g_pending[side] = GetTickCount64();
        }
    }
}

void SampleNativeGrabProbe(void* physics, std::uintptr_t caller, NativeGrabCast cast) {
    if (!g_enabled || g_fault || caller != g_image + kNativeAimReturn || g_samples.load() >= 16 ||
        !HasNativeGameplayCamera() || GetVehicleCameraAnchor().valid) return;
    const auto now = GetTickCount64();
    std::uintptr_t singleton = 0, world = 0;
    if (!Read(g_image + kPhysicsSingleton, singleton) || singleton != reinterpret_cast<std::uintptr_t>(physics) ||
        !Read(singleton + 0x30, world) || !world) return;
    for (int hand = 0; hand < 2; ++hand) {
        const auto time = g_pending[hand].exchange(0);
        if (!time || now - time > 160) continue;
        if (!RicoInventoryIsHolstered()) {
            TraceRuntime("native grab probe: grip rejected while inventory is armed or unresolved");
            continue;
        }
        NativeGrabRay ray{};
        float down[3]{};
        if (GetVrBodyHandTarget(hand, ray.origin, ray.direction, down))
            Report(hand ? "right-grip" : "left-grip", physics, world, cast, ray, g_reach);
        else TraceRuntime("native grab probe: grip did not have a fresh world pose");
    }
    // A single camera-centre diagnostic ray for a flat-screen run. Poll the
    // marker at most once per second and consume only our exact filename.
    static std::atomic<ULONGLONG> nextPoll = 0;
    auto previous = nextPoll.load();
    if (g_armFile.empty() || now < previous || !nextPoll.compare_exchange_strong(previous, now + 1000)) return;
    if (GetFileAttributesW(g_armFile.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    float basis[9]{};
    NativeGrabRay ray{};
    if (!GetEngineCameraBasis(basis) || !GetEngineCameraPosition(ray.origin) ||
        !DeleteFileW(g_armFile.c_str())) return;
    for (int i = 0; i != 3; ++i) ray.direction[i] = -basis[i + 6];
    Report("flat-camera", physics, world, cast, ray, 2.0f);
}
}
