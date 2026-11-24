#include "game/engine_lighting.h"
#include "game/camera_capture.h"
#include "game/engine_culling.h"
#include "game/vehicle_camera.h"
#include "core/logging.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif

namespace jc3vr {
namespace {
// Matrix4::PerspectiveFovInverse(this = out, fov, aspect, far, near) -> out,
// from ferrobrew/pyxis-defs (MIT), build "JustCause3 Steam 20206564".
constexpr std::uintptr_t kPerspectiveFovInverseRva = 0x390E0;
constexpr std::uint8_t kPerspectiveFovInverseBytes[]{0x48, 0x8B, 0xC4, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
// The active camera's clip planes identify the main view's passes (a
// reflection or other auxiliary camera has its own near/far and keeps the
// engine's symmetric rebuild, which is right for it).
constexpr std::uintptr_t kCameraManagerRva = 0x2ED0E20;  // -> CameraManager*
constexpr std::size_t kActiveCamera = 0x5C0, kNear = 0x594, kFar = 0x598;

using PerspectiveFovInverseFn = float*(__fastcall*)(float* out, float fov, float aspect, float zFar, float zNear);
// ShadowManager::UpdateRender(this, dt, dtf): the sim-side sun-shadow update
// that fits the scheduled cascades to the active camera.
constexpr std::uintptr_t kShadowUpdateRenderRva = 0x1C7370;
constexpr std::uint8_t kShadowUpdateRenderBytes[]{0x40, 0x55, 0x56, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x10, 0xFE, 0xFF};
constexpr std::size_t kCascadeUpdateLevels = 0x2468;  // ShadowManager: int[6], 2^level frames between refreshes
constexpr std::size_t kProjectionF = 0x154, kTransformT1 = 0xD4;  // Camera
using ShadowUpdateRenderFn = std::uint64_t(__fastcall*)(void* self, float dt, float dtf);
ShadowUpdateRenderFn g_originalShadowUpdate = nullptr;
std::atomic<std::uint64_t> g_shadowUpdates = 0, g_levelsZeroed = 0;
std::uintptr_t g_imageBase = 0;
bool g_installed = false;
PerspectiveFovInverseFn g_originalInverse = nullptr;
EngineLightingOptions g_options;
std::atomic<bool> g_active = true;  // Shift+F6 flips this for A/B tests
// Set once the first symmetric call proves our inverse matches the engine's
// layout; until then (or if it never does) nothing is substituted.
std::atomic<int> g_layoutCheck = 0;  // 0 untested, 1 matches, -1 mismatch
std::atomic<std::uint64_t> g_substituted = 0, g_passedThrough = 0;

bool SafeWrite(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeRead(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The inverse of the engine's reverse-Z projection, row-major, row-vector:
//   P = [m00 0 0 0; 0 m11 0 0; m20 m21 A -1; 0 0 B 0], A = n/(f-n), B = nf/(f-n)
//   P^-1 = [1/m00 0 0 0; 0 1/m11 0 0; 0 0 0 1/B; m20/m00 m21/m11 -1 1/f]
// With m20 = m21 = 0 this is exactly what PerspectiveFovInverse produces.
void OffAxisInverse(float m00, float m11, float m20, float m21, float zFar, float zNear, float out[16]) {
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = 1.0f / m00;
    out[5] = 1.0f / m11;
    out[11] = (zFar - zNear) / (zFar * zNear);
    out[12] = m20 / m00;
    out[13] = m21 / m11;
    out[14] = -1.0f;
    out[15] = 1.0f / zFar;
}

// Whether near/far are the active (main) camera's.
bool IsMainView(float zFar, float zNear) {
    std::uint8_t* manager = nullptr;
    std::uint8_t* active = nullptr;
    float activeNear = 0.0f, activeFar = 0.0f;
    if (!SafeRead(&manager, reinterpret_cast<const void*>(g_imageBase + kCameraManagerRva), sizeof(manager)) || !manager ||
        !SafeRead(&active, manager + kActiveCamera, sizeof(active)) || !active ||
        !SafeRead(&activeNear, active + kNear, 4) || !SafeRead(&activeFar, active + kFar, 4) || !(activeNear > 0.0f) || !(activeFar > activeNear)) return false;
    return std::abs(zNear - activeNear) <= std::max(activeNear, 0.01f) * 0.1f && std::abs(zFar - activeFar) <= activeFar * 0.01f;
}

// PerspectiveFovInverse: let the engine build its symmetric inverse, then,
// for main-view passes while an eye is rendering, overwrite it with the exact
// inverse of the projection that eye was rendered with.
float* __fastcall PerspectiveFovInverseDetour(float* out, float fov, float aspect, float zFar, float zNear) {
    float* result = g_originalInverse(out, fov, aspect, zFar, zNear);
    if (!out || !g_active.load() || !g_options.offAxisReconstruction) return result;

    // One-time layout check against the engine's own output: rebuild the
    // symmetric case with our formula and compare entry by entry.
    if (g_layoutCheck.load() == 0 && fov > 0.1f && fov < 3.0f && aspect > 0.1f && zFar > zNear && zNear > 0.0f) {
        const float tanHalf = std::tan(0.5f * fov);
        float mine[16];
        OffAxisInverse(1.0f / (aspect * tanHalf), 1.0f / tanHalf, 0.0f, 0.0f, zFar, zNear, mine);
        float worst = 0.0f;
        for (int i = 0; i != 16; ++i) worst = std::max(worst, std::abs(mine[i] - out[i]) / std::max(1.0f, std::abs(out[i])));
        g_layoutCheck.store(worst < 1e-3f ? 1 : -1);
        TraceRuntime("engine lighting: PerspectiveFovInverse layout check " + std::string(worst < 1e-3f ? "OK" : "FAILED (substitution disabled)") +
            " max_rel_diff=" + std::to_string(worst) + " engine=[" + std::to_string(out[0]) + "," + std::to_string(out[5]) + "," +
            std::to_string(out[11]) + "," + std::to_string(out[14]) + "," + std::to_string(out[15]) + "]");
    }
    if (g_layoutCheck.load() != 1 || !IsNativeSceneRendering() || !IsMainView(zFar, zNear)) {
        ++g_passedThrough;
        return result;
    }
    // The projection of the scene this frame is being rendered with (the
    // same value JC3VR uses to tell which eye a finished image belongs to).
    const auto projection = GetLatestCameraProjection();
    if (!projection.valid || !(projection.m00 > 0.0f) || !(projection.m11 > 0.0f)) {
        ++g_passedThrough;
        return result;
    }
    OffAxisInverse(projection.m00, projection.m11, projection.m20, projection.m21, zFar, zNear, out);
    ++g_substituted;
    return result;
}

// The active camera (CameraManager::m_ActiveCamera), or null.
std::uint8_t* ActiveCamera() {
    std::uint8_t* manager = nullptr;
    std::uint8_t* active = nullptr;
    if (!SafeRead(&manager, reinterpret_cast<const void*>(g_imageBase + kCameraManagerRva), sizeof(manager)) || !manager ||
        !SafeRead(&active, manager + kActiveCamera, sizeof(active))) return nullptr;
    return active;
}

// ShadowManager::UpdateRender, around the cascade fit:
//  - every cascade's update level set to 0, so the whole atlas is re-rendered
//    for the eye this frame draws instead of mixing frames (and eyes);
//  - the fit camera temporarily gets the both-eye union field of view, so the
//    fitted cascades cover what both eyes see;
//  - its forward axis temporarily becomes the head's heading (yaw only), so
//    the cascade centre doesn't slide with head pitch/roll or with the lag of
//    the steered engine camera.
// Everything on the camera is put back before returning.
std::uint64_t __fastcall ShadowUpdateRenderDetour(void* self, float dt, float dtf) {
    // Observe the existing render callback's fraction; do not change cascades
    // or advance simulation. The seated camera uses this same interpolation.
    if (self && IsNativeSceneRendering()) SetVehicleRenderInterpolation(dtf);
    if (!g_active.load() || !self || !IsNativeSceneRendering()) return g_originalShadowUpdate(self, dt, dtf);
    if (g_options.shadowsEveryFrame) {
        int levels[6]{};
        auto* at = static_cast<std::uint8_t*>(self) + kCascadeUpdateLevels;
        // Only overwrite something that looks like the update pattern.
        if (SafeRead(levels, at, sizeof(levels)) && std::all_of(std::begin(levels), std::end(levels), [](int l) { return l >= 0 && l <= 8; }) &&
            std::any_of(std::begin(levels), std::end(levels), [](int l) { return l != 0; })) {
            const int zero[6]{};
            if (SafeWrite(at, zero, sizeof(zero))) ++g_levelsZeroed;
        }
    }
    std::uint8_t* camera = ActiveCamera();
    float savedProjection[2]{}, savedForward[3]{};
    bool widened = false, steered = false;
    if (camera && g_options.shadowFitUnion) {
        float zNear = 0.0f, m00 = 0.0f, m11 = 0.0f;
        if (SafeRead(&zNear, camera + kNear, 4) && zNear > 0.0f && GetEngineCullUnionScale(zNear, m00, m11) &&
            SafeRead(&savedProjection[0], camera + kProjectionF, 4) && SafeRead(&savedProjection[1], camera + kProjectionF + 5 * 4, 4)) {
            widened = SafeWrite(camera + kProjectionF, &m00, 4) && SafeWrite(camera + kProjectionF + 5 * 4, &m11, 4);
        }
    }
    if (camera && g_options.shadowFitHeading && SafeRead(savedForward, camera + kTransformT1 + 8 * 4, sizeof(savedForward))) {
        // Row 2 of the transform (the back axis), flattened onto the ground;
        // from the head view when we have it.
        float head[16];
        const float* row = GetEngineCullHeadWorld(head) ? head + 8 : savedForward;
        const float length = std::sqrt(row[0] * row[0] + row[2] * row[2]);
        if (length > 1e-4f) {
            const float flat[3]{row[0] / length, 0.0f, row[2] / length};
            steered = SafeWrite(camera + kTransformT1 + 8 * 4, flat, sizeof(flat));
        }
    }
    const std::uint64_t result = g_originalShadowUpdate(self, dt, dtf);
    if (widened) {
        SafeWrite(camera + kProjectionF, &savedProjection[0], 4);
        SafeWrite(camera + kProjectionF + 5 * 4, &savedProjection[1], 4);
    }
    if (steered) SafeWrite(camera + kTransformT1 + 8 * 4, savedForward, sizeof(savedForward));
    ++g_shadowUpdates;
    return result;
}
}

void ConfigureEngineLighting(const EngineLightingOptions& options) {
    g_options = options;
    TraceRuntime(std::string("engine lighting: off-axis reconstruction ") + (options.offAxisReconstruction ? "on" : "off") + " shadows_every_frame=" + (options.shadowsEveryFrame ? "1" : "0") + " shadow_fit_union=" + (options.shadowFitUnion ? "1" : "0") + " shadow_fit_heading=" + (options.shadowFitHeading ? "1" : "0"));
}

bool InstallEngineLightingHooks() {
#if JC3VR_HAVE_MINHOOK
    if (g_installed) return true;
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    // Each hook has its own byte guard; one failing doesn't block the other.
    auto* target = reinterpret_cast<void*>(g_imageBase + kPerspectiveFovInverseRva);
    std::uint8_t actual[sizeof(kPerspectiveFovInverseBytes)]{};
    const bool inverseBytesOk = SafeRead(actual, target, sizeof(actual)) && std::memcmp(actual, kPerspectiveFovInverseBytes, sizeof(actual)) == 0;
    if (!inverseBytesOk) TraceRuntime("engine lighting: PerspectiveFovInverse byte guard failed; reconstruction fix not installed");
    const MH_STATUS created = g_options.offAxisReconstruction && inverseBytesOk ? MH_CreateHook(target, reinterpret_cast<void*>(&PerspectiveFovInverseDetour), reinterpret_cast<void**>(&g_originalInverse)) : MH_UNKNOWN;
    const MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
    // The shadow-fit hook is independent: its own byte guard, its own result.
    MH_STATUS shadow = MH_UNKNOWN;
    auto* shadowTarget = reinterpret_cast<void*>(g_imageBase + kShadowUpdateRenderRva);
    std::uint8_t shadowBytes[sizeof(kShadowUpdateRenderBytes)]{};
    if ((g_options.shadowsEveryFrame || g_options.shadowFitUnion || g_options.shadowFitHeading) && SafeRead(shadowBytes, shadowTarget, sizeof(shadowBytes)) &&
        std::memcmp(shadowBytes, kShadowUpdateRenderBytes, sizeof(shadowBytes)) == 0) {
        shadow = MH_CreateHook(shadowTarget, reinterpret_cast<void*>(&ShadowUpdateRenderDetour), reinterpret_cast<void**>(&g_originalShadowUpdate));
        if (shadow == MH_OK) shadow = MH_EnableHook(shadowTarget);
    }
    g_installed = enabled == MH_OK || shadow == MH_OK;
    TraceRuntime("engine lighting hooks: PerspectiveFovInverse=" + std::to_string(static_cast<int>(enabled)) +
        " ShadowManager::UpdateRender=" + std::to_string(static_cast<int>(shadow)) + " (0 = MH_OK)");
    return g_installed;
#else
    return false;
#endif
}

void EngineLightingPresentTick() {
    // Shift+F6: all lighting fixes on/off (plain F6 pauses camera writes).
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0 && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (down && !wasDown) {
        g_active.store(!g_active.load());
        TraceRuntime(std::string("Shift+F6: lighting fixes (reconstruction + shadows) ") + (g_active.load() ? "ON" : "OFF"));
    }
    wasDown = down;
    static std::uint64_t presents = 0;
    if (++presents % 1800 == 0)
        TraceRuntime("engine lighting: reconstruction substituted=" + std::to_string(g_substituted.load()) + " passed_through=" +
            std::to_string(g_passedThrough.load()) + " layout_check=" + std::to_string(g_layoutCheck.load()) + " shadow_updates=" + std::to_string(g_shadowUpdates.load()) + " cascade_levels_zeroed=" + std::to_string(g_levelsZeroed.load()) + " active=" + (g_active.load() ? "1" : "0"));
}
}
