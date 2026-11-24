#include "game/engine_culling.h"
#include "core/logging.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif

namespace jc3vr {
namespace {
// Addresses for build "JustCause3 Steam 20206564", which is our fingerprinted
// exe, taken from ferrobrew/pyxis-defs (MIT). Before anything is hooked or
// called, the first bytes of each function are compared with what we read out
// of our own JustCause3.exe, so a different build simply disables this.
constexpr std::uintptr_t kTerrainPatchSystemUpdateRva = 0x32F780;
constexpr std::uintptr_t kCameraUpdateFrustumRva = 0xB2FC0;
constexpr std::uintptr_t kGetBfbcFrustumParamsRva = 0xD68B0;
constexpr std::uintptr_t kCameraManagerRva = 0x2ED0E20;              // -> CameraManager*
constexpr std::uintptr_t kTerrainPatchTypeSingletonRva = 0x2EED238;  // -> CRenderBlockTypeTerrainPatch*
constexpr std::uintptr_t kTerrainPatchTypeVtableRva = 0x235E528;
constexpr std::uint8_t kTerrainUpdateBytes[]{0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x80, 0x79, 0x50, 0x00};
constexpr std::uint8_t kUpdateFrustumBytes[]{0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
constexpr std::uint8_t kGetBfbcBytes[]{0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

// Field offsets. Camera is 0x5B0 bytes; see docs/ENGINE_REFERENCE.md �3.
constexpr std::size_t kTransformF = 0x54, kProjectionF = 0x154, kView = 0x2D4, kViewProjection = 0x314;
constexpr std::size_t kFovT1 = 0x584, kNear = 0x594, kFar = 0x598;
constexpr std::size_t kActiveCamera = 0x5C0;             // CameraManager::m_ActiveCamera
constexpr std::size_t kTerrainCameraOffset = 0x58;       // STerrainPatchSystem::m_TerrainCamera
constexpr std::size_t kCullingCameraOffset = 0x8;        // OccluderCollectionManager's own cull camera
constexpr std::size_t kBfbcFrustumCount = 0x1280;        // BFBCFrustumCullParameters::m_FrustumCount
constexpr std::size_t kBackPatchCulling = 0x44D, kFrustumPatchCulling = 0x44E;  // terrain-patch type flags

using TerrainUpdateFn = void(__fastcall*)(void* handle, void* context);
using UpdateFrustumFn = void(__fastcall*)(void* camera, const float* transform);
using GetBfbcFn = void(__fastcall*)(void* self, const void* camera, int time, void** outParams, std::uint64_t* outState);

// Hook state. g_updateFrustum is the engine function itself (we call it for
// the terrain camera); g_originalUpdateFrustum is MinHook's trampoline once
// that function is hooked too, and is what we must call from then on.
std::uintptr_t g_imageBase = 0;
bool g_installed = false;
TerrainUpdateFn g_originalTerrainUpdate = nullptr;
GetBfbcFn g_originalGetBfbc = nullptr;
UpdateFrustumFn g_originalUpdateFrustum = nullptr;
UpdateFrustumFn g_updateFrustum = nullptr;

EngineCullingOptions g_options;
std::atomic<bool> g_active = true;  // Shift+F5 flips this for A/B tests

// What the XR loop tells us each frame: both eyes' extents, the IPD, and
// the eye camera JC3VR just rendered from. Written on the Present thread,
// read from whichever game thread runs the culls.
std::mutex g_eyeMutex;
bool g_haveEyes = false;
float g_tangents[4]{};  // union of both eyes: left, right, up, down (before padding)
float g_ipd = 0.064f;
ULONGLONG g_eyesAt = 0;
std::mutex g_viewMutex;
float g_viewWorld[16]{};
ULONGLONG g_viewWorldAt = 0;

// Terrain hull-cull flags: the engine's values, kept so we can put them back.
bool g_hullSaved = false;
std::uint8_t g_hullOriginal[2]{};
bool g_hullRelaxed = false;

// Counters for the periodic log line.
std::atomic<std::uint64_t> g_terrainWidens = 0, g_sceneWidens = 0, g_modelWidens = 0, g_occlusionDrops = 0, g_rejects = 0;

// Memory access that survives a bad pointer (the engine's objects come and go
// across loads).
bool SafeRead(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWrite(void* destination, const void* source, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True if the code at image+rva starts with the bytes we expect.
bool BytesMatch(std::uintptr_t rva, const std::uint8_t* expected, std::size_t size) {
    std::uint8_t actual[16]{};
    return size <= sizeof(actual) && SafeRead(actual, reinterpret_cast<const void*>(g_imageBase + rva), size) && std::memcmp(actual, expected, size) == 0;
}

// Union tangents padded like ferrobrew's cull projection: each side scaled by
// (1 + padding) plus the eye-shift margin (IPD/2)/zNear, clamped to 85 deg.
// False while no XR frame has supplied eyes recently (2D, menus without XR).
bool UnionTangents(float zNear, float out[4]) {
    std::lock_guard lock(g_eyeMutex);
    if (!g_haveEyes || GetTickCount64() - g_eyesAt > 500) return false;
    const float pad = 1.0f + std::max(0.0f, g_options.padding);
    const float margin = 0.5f * g_ipd / std::max(zNear, 1e-3f);
    const float limit = std::tan(85.0f * 3.14159265f / 180.0f);
    for (int i = 0; i != 4; ++i) {
        const float t = g_tangents[i];
        out[i] = std::clamp(t * pad + std::copysign(margin, t), -limit, limit);
    }
    return true;
}

// Standard-depth, right-handed, off-centre projection in the engine's
// row-major row-vector layout (m[row*4+col]), NDC z in [0,1].
void UnionProjection(const float t[4], float zNear, float zFar, float m[16]) {
    const float l = t[0], r = t[1], u = t[2], d = t[3];
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (r - l);
    m[5] = 2.0f / (u - d);
    m[8] = (l + r) / (r - l);
    m[9] = (d + u) / (u - d);
    m[10] = zFar / (zNear - zFar);
    m[11] = -1.0f;
    m[14] = zNear * zFar / (zNear - zFar);
}

// The head (eye) view: world W (rows right, up, back, translation) and its
// rigid inverse V (row-vector convention: p_view = p_world * V). False if not
// fresh or disabled, so callers fall back to the camera's own view.
bool HeadView(float world[16], float view[16]) {
    if (!g_options.fromHead) return false;
    {
        std::lock_guard lock(g_viewMutex);
        if (!g_viewWorldAt || GetTickCount64() - g_viewWorldAt > 250) return false;
        std::memcpy(world, g_viewWorld, sizeof(g_viewWorld));
    }
    std::memset(view, 0, 16 * sizeof(float));
    for (int i = 0; i != 3; ++i)
        for (int j = 0; j != 3; ++j) view[i * 4 + j] = world[j * 4 + i];
    for (int j = 0; j != 3; ++j) view[12 + j] = -(world[12] * world[j * 4] + world[13] * world[j * 4 + 1] + world[14] * world[j * 4 + 2]);
    view[15] = 1.0f;
    return true;
}

// Row-vector matrix product out = a * b (the engine's convention, so a view
// times a projection gives the view-projection).
void Multiply(const float a[16], const float b[16], float out[16]) {
    for (int i = 0; i != 4; ++i)
        for (int j = 0; j != 4; ++j) out[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

// The engine's plane rebuild, via the trampoline when we've hooked it.
UpdateFrustumFn FrustumRebuild() { return g_originalUpdateFrustum ? g_originalUpdateFrustum : g_updateFrustum; }

// The camera's own near/far clip distances, sanity-checked.
bool CameraPlanes(const std::uint8_t* camera, float& zNear, float& zFar) {
    return SafeRead(&zNear, camera + kNear, 4) && SafeRead(&zFar, camera + kFar, 4) && std::isfinite(zNear) && std::isfinite(zFar) && zNear > 0.0f && zFar > zNear;
}

// TerrainPatchSystemUpdate: the engine has just copied the active camera into
// the landscape system's terrain camera, and terrain patches are about to be
// culled against that copy. Turn it into the head view with the both-eye
// union projection and rebuild its frustum planes.
void __fastcall TerrainUpdateDetour(void* handle, void* context) {
    g_originalTerrainUpdate(handle, context);
    if (!g_active.load() || !g_options.terrainCamera || !handle || !g_updateFrustum) return;
    auto* camera = static_cast<std::uint8_t*>(handle) + kTerrainCameraOffset;
    float zNear = 0.0f, zFar = 0.0f, tangents[4]{};
    if (!CameraPlanes(camera, zNear, zFar) || !UnionTangents(zNear, tangents)) return;
    float projection[16], view[16], transform[16], viewProjection[16]{};
    if (HeadView(transform, view)) {
        // Cull from the rendered head view, not the copied engine camera.
        SafeWrite(camera + kTransformF, transform, sizeof(transform));
        SafeWrite(camera + kView, view, sizeof(view));
    } else if (!SafeRead(view, camera + kView, sizeof(view)) || !SafeRead(transform, camera + kTransformF, sizeof(transform)) ||
               std::abs(view[15] - 1.0f) > 1e-3f || std::abs(transform[15] - 1.0f) > 1e-3f) {
        ++g_rejects;
        return;
    }
    UnionProjection(tangents, zNear, zFar, projection);
    Multiply(view, projection, viewProjection);
    if (!SafeWrite(camera + kProjectionF, projection, sizeof(projection)) || !SafeWrite(camera + kViewProjection, viewProjection, sizeof(viewProjection))) {
        ++g_rejects;
        return;
    }
    // The engine's own plane rebuild (planes, AAB normals, distances,
    // closest corners) from the standard-depth m_ViewProjection.
    FrustumRebuild()(camera, transform);
    ++g_terrainWidens;
}

// GetBFBCFrustumParamsForCameraAndTime: the scene cull (models, streaming,
// AO volumes) builds its frustum here. For the occluder manager's own cull
// camera only: head view + union projection before the engine builds the
// frustum, the size-cull FOV put back to the engine's 65 deg, and afterwards
// the occluder frusta dropped (they're computed from one centre viewpoint,
// which hides things an offset eye can see).
void __fastcall GetBfbcDetour(void* self, const void* camera, int time, void** outParams, std::uint64_t* outState) {
    const bool main = self && camera == static_cast<std::uint8_t*>(self) + kCullingCameraOffset;
    if (main && g_active.load() && g_options.sceneCull) {
        auto* cam = const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(camera));
        float zNear = 0.0f, zFar = 0.0f, tangents[4]{}, projection[16];
        if (CameraPlanes(cam, zNear, zFar) && UnionTangents(zNear, tangents)) {
            UnionProjection(tangents, zNear, zFar, projection);
            // Permanent: the cull camera is the manager's own per-frame copy
            // of the active camera, not a render camera.
            SafeWrite(cam + kProjectionF, projection, sizeof(projection));
            float world[16], view[16];
            if (HeadView(world, view)) {
                SafeWrite(cam + kTransformF, world, sizeof(world));
                SafeWrite(cam + kView, view, sizeof(view));
            }
            if (g_options.sizeCullFovDegrees > 0.0f) {
                const float fov = g_options.sizeCullFovDegrees * 3.14159265f / 180.0f;
                SafeWrite(cam + kFovT1, &fov, 4);
            }
            ++g_sceneWidens;
        }
    }
    g_originalGetBfbc(self, camera, time, outParams, outState);
    if (main && g_active.load() && g_options.disableOcclusion && outParams) {
        void* params = nullptr;
        std::uint32_t count = 0;
        if (SafeRead(&params, outParams, sizeof(params)) && params &&
            SafeRead(&count, static_cast<std::uint8_t*>(params) + kBfbcFrustumCount, 4) && count >= 2 && count <= 33) {
            const std::uint32_t one = 1;
            if (SafeWrite(static_cast<std::uint8_t*>(params) + kBfbcFrustumCount, &one, 4)) ++g_occlusionDrops;
        }
    }
}

// Camera::UpdateFrustum for the active camera: its planes gate the
// per-render-block model cull (CModelInstance::AddToRender ->
// Camera::IsBoxVisible), the instant hide instead of the fade, road meshes
// and far lights. Rebuild them once more from the head view x union, then
// restore m_ViewProjection so only the planes widen (ferrobrew widen_model_cull).
void __fastcall UpdateFrustumDetour(void* camera, const float* transform) {
    g_originalUpdateFrustum(camera, transform);
    if (!g_active.load() || !g_options.modelCull || !camera) return;
    std::uint8_t* manager = nullptr;
    std::uint8_t* active = nullptr;
    if (!SafeRead(&manager, reinterpret_cast<const void*>(g_imageBase + kCameraManagerRva), sizeof(manager)) || !manager ||
        !SafeRead(&active, manager + kActiveCamera, sizeof(active)) || active != camera) return;
    auto* cam = static_cast<std::uint8_t*>(camera);
    float zNear = 0.0f, zFar = 0.0f, tangents[4]{};
    if (!CameraPlanes(cam, zNear, zFar) || !UnionTangents(zNear, tangents)) return;
    float saved[16], view[16], world[16], projection[16], viewProjection[16];
    if (!SafeRead(saved, cam + kViewProjection, sizeof(saved))) return;
    const bool head = HeadView(world, view);
    if (!head && !SafeRead(view, cam + kView, sizeof(view))) return;
    UnionProjection(tangents, zNear, zFar, projection);
    Multiply(view, projection, viewProjection);
    if (!SafeWrite(cam + kViewProjection, viewProjection, sizeof(viewProjection))) return;
    g_originalUpdateFrustum(camera, head ? world : transform);
    SafeWrite(cam + kViewProjection, saved, sizeof(saved));
    ++g_modelWidens;
}

// The terrain-patch type singleton, verified by its exact vtable.
std::uint8_t* TerrainPatchType() {
    std::uint8_t* object = nullptr;
    std::uintptr_t vtable = 0;
    if (!SafeRead(&object, reinterpret_cast<const void*>(g_imageBase + kTerrainPatchTypeSingletonRva), sizeof(object)) || !object ||
        !SafeRead(&vtable, object, sizeof(vtable)) || vtable != g_imageBase + kTerrainPatchTypeVtableRva) return nullptr;
    return object;
}
}

void ConfigureEngineCulling(const EngineCullingOptions& options) {
    g_options = options;
    TraceRuntime(std::string("engine culling widen (both-eye union): terrain_camera=") + (options.terrainCamera ? "1" : "0") +
        " terrain_hull_culls_relaxed=" + (options.terrainHullCulls ? "1" : "0") + " scene_cull=" + (options.sceneCull ? "1" : "0") +
        " occlusion_off=" + (options.disableOcclusion ? "1" : "0") + " padding=" + std::to_string(options.padding) +
        " size_cull_fov_deg=" + std::to_string(options.sizeCullFovDegrees));
}

bool InstallEngineCullingHooks() {
#if JC3VR_HAVE_MINHOOK
    if (g_installed) return true;
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_options.terrainCamera && !g_options.sceneCull && !g_options.terrainHullCulls) return false;
    const bool terrainOk = BytesMatch(kTerrainPatchSystemUpdateRva, kTerrainUpdateBytes, sizeof(kTerrainUpdateBytes));
    const bool frustumOk = BytesMatch(kCameraUpdateFrustumRva, kUpdateFrustumBytes, sizeof(kUpdateFrustumBytes));
    const bool bfbcOk = BytesMatch(kGetBfbcFrustumParamsRva, kGetBfbcBytes, sizeof(kGetBfbcBytes));
    if (!terrainOk || !frustumOk || !bfbcOk) {
        TraceRuntime(std::string("engine culling: byte guard failed (terrain=") + (terrainOk ? "1" : "0") + " frustum=" + (frustumOk ? "1" : "0") +
            " bfbc=" + (bfbcOk ? "1" : "0") + "); no hook installed");
        return false;
    }
    g_updateFrustum = reinterpret_cast<UpdateFrustumFn>(g_imageBase + kCameraUpdateFrustumRva);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    auto* terrainTarget = reinterpret_cast<void*>(g_imageBase + kTerrainPatchSystemUpdateRva);
    auto* bfbcTarget = reinterpret_cast<void*>(g_imageBase + kGetBfbcFrustumParamsRva);
    const MH_STATUS terrain = g_options.terrainCamera ? MH_CreateHook(terrainTarget, reinterpret_cast<void*>(&TerrainUpdateDetour), reinterpret_cast<void**>(&g_originalTerrainUpdate)) : MH_OK;
    const MH_STATUS bfbc = g_options.sceneCull ? MH_CreateHook(bfbcTarget, reinterpret_cast<void*>(&GetBfbcDetour), reinterpret_cast<void**>(&g_originalGetBfbc)) : MH_OK;
    auto* frustumTarget = reinterpret_cast<void*>(g_imageBase + kCameraUpdateFrustumRva);
    const MH_STATUS frustum = g_options.modelCull ? MH_CreateHook(frustumTarget, reinterpret_cast<void*>(&UpdateFrustumDetour), reinterpret_cast<void**>(&g_originalUpdateFrustum)) : MH_OK;
    if (frustum != MH_OK) g_originalUpdateFrustum = nullptr;
    if (frustum == MH_OK && g_options.modelCull) MH_EnableHook(frustumTarget);
    if (terrain == MH_OK && g_options.terrainCamera) MH_EnableHook(terrainTarget);
    if (bfbc == MH_OK && g_options.sceneCull) MH_EnableHook(bfbcTarget);
    g_installed = true;
    TraceRuntime("engine culling hooks: TerrainPatchSystemUpdate=" + std::to_string(static_cast<int>(terrain)) +
        " GetBFBCFrustumParams=" + std::to_string(static_cast<int>(bfbc)) + " Camera::UpdateFrustum=" + std::to_string(static_cast<int>(frustum)) + " (0 = MH_OK)");
    return terrain == MH_OK && bfbc == MH_OK;
#else
    return false;
#endif
}

void SetEngineCullEyes(const float leftEye[4], const float rightEye[4], float ipdMeters) {
    std::lock_guard lock(g_eyeMutex);
    g_tangents[0] = std::min(leftEye[0], rightEye[0]);
    g_tangents[1] = std::max(leftEye[1], rightEye[1]);
    g_tangents[2] = std::max(leftEye[2], rightEye[2]);
    g_tangents[3] = std::min(leftEye[3], rightEye[3]);
    if (std::isfinite(ipdMeters) && ipdMeters > 0.04f && ipdMeters < 0.09f) g_ipd = ipdMeters;
    g_haveEyes = true;
    g_eyesAt = GetTickCount64();
}

bool GetEngineCullUnionScale(float zNear, float& m00, float& m11) {
    float t[4]{};
    if (!UnionTangents(zNear, t)) return false;
    m00 = 2.0f / (t[1] - t[0]);
    m11 = 2.0f / (t[2] - t[3]);
    return true;
}

bool GetEngineCullHeadWorld(float world[16]) {
    float view[16];
    return HeadView(world, view);
}
void SetEngineCullViewWorld(const float world[16]) {
    std::lock_guard lock(g_viewMutex);
    std::memcpy(g_viewWorld, world, sizeof(g_viewWorld));
    g_viewWorldAt = GetTickCount64();
}

void EngineCullingPresentTick() {
    if (!g_imageBase) g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // Shift+F5: all engine-cull widens on/off (plain F5 cycles JC3VR's culling modes).
    static bool wasDown = false;
    const bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0 && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (down && !wasDown) {
        g_active.store(!g_active.load());
        TraceRuntime(std::string("Shift+F5: engine culling widen ") + (g_active.load() ? "ON" : "OFF"));
    }
    wasDown = down;
    // Terrain hull culls: relaxed while widening in VR, restored otherwise.
    bool haveEyes = false;
    {
        std::lock_guard lock(g_eyeMutex);
        haveEyes = g_haveEyes && GetTickCount64() - g_eyesAt < 500;
    }
    const bool relax = g_options.terrainHullCulls && g_active.load() && haveEyes;
    if (relax != g_hullRelaxed) {
        if (std::uint8_t* type = TerrainPatchType()) {
            if (!g_hullSaved) g_hullSaved = SafeRead(&g_hullOriginal[0], type + kBackPatchCulling, 1) && SafeRead(&g_hullOriginal[1], type + kFrustumPatchCulling, 1);
            if (g_hullSaved) {
                const std::uint8_t zero = 0;
                SafeWrite(type + kBackPatchCulling, relax ? &zero : &g_hullOriginal[0], 1);
                SafeWrite(type + kFrustumPatchCulling, relax ? &zero : &g_hullOriginal[1], 1);
                g_hullRelaxed = relax;
                TraceRuntime(std::string("engine culling: terrain hull back/frustum patch culls ") + (relax ? "relaxed" : "restored") +
                    " (engine values " + std::to_string(g_hullOriginal[0]) + "/" + std::to_string(g_hullOriginal[1]) + ")");
            }
        }
    }
    static std::uint64_t presents = 0;
    if (++presents % 1800 == 0)
        TraceRuntime("engine culling: terrain_widens=" + std::to_string(g_terrainWidens.load()) + " scene_widens=" + std::to_string(g_sceneWidens.load()) + " model_widens=" + std::to_string(g_modelWidens.load()) +
            " occlusion_drops=" + std::to_string(g_occlusionDrops.load()) + " rejects=" + std::to_string(g_rejects.load()) +
            " active=" + (g_active.load() ? "1" : "0") + " hull_relaxed=" + (g_hullRelaxed ? "1" : "0"));
}
}
