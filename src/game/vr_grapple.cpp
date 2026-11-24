// Local Rico's gauntlet and native wire endpoint, using one tracked wrist.
// Addresses and ABIs verified against the fingerprinted executable by offline
// disassembly (2026-10-01). No shared animation-hand pose is written.
#include "game/vr_grapple.h"
#include "game/vr_body_rig.h"
#include "core/logging.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <string>
#include <intrin.h>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif

namespace jc3vr {
namespace {
std::uintptr_t g_image = 0;
using WorldFn = void(__fastcall*)(void*, std::uint32_t, float*);
using PositionFn = float*(__fastcall*)(void*, float*);
using ProxyPositionFn = bool(__fastcall*)(void*, float*, float*);
using ProxyMatrixFn = bool(__fastcall*)(void*, float*);
using ProxyGraphicsFn = void(__fastcall*)(void*, float*);
using SafeIndexFn = std::uint32_t(__fastcall*)(void*, std::uint32_t);
WorldFn g_world = nullptr, g_model = nullptr;
PositionFn g_spawn = nullptr;
ProxyPositionFn g_proxyPosition = nullptr;
ProxyMatrixFn g_proxyMatrix = nullptr;
ProxyGraphicsFn g_proxyGraphics = nullptr;
SafeIndexFn g_safeIndex = nullptr;
std::atomic<std::uint64_t> g_worldMoves{0}, g_modelMoves{0}, g_spawns{0}, g_endpointMoves{0};

bool Read(void* out, const void* in, std::size_t size) {
    __try { std::memcpy(out, in, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Write(void* out, const void* in, std::size_t size) {
    __try { std::memcpy(out, in, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Local(void* character) {
    const auto rico = VrBodyCharacter();
    return rico && reinterpret_cast<std::uintptr_t>(character) == rico - 8;
}
bool Resolve(void* self, std::uint32_t hash, std::uint32_t& bone) {
    if (!g_safeIndex) return false;
    __try { bone = g_safeIndex(self, hash); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Multiply(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r != 4; ++r)
        for (int c = 0; c != 4; ++c) {
            out[r*4+c] = 0;
            for (int k = 0; k != 4; ++k) out[r*4+c] += a[r*4+k] * b[k*4+c];
        }
}
bool WorldToModel(void* self, const float world[16], float model[16]) {
    float character[16], inverse[16]{};
    if (!Read(character, reinterpret_cast<const std::byte*>(self) + 0x2830, sizeof(character))) return false;
    for (float v : character) if (!std::isfinite(v)) return false;
    if (std::abs(character[15]-1) > 1e-3f) return false;
    for (int r = 0; r != 3; ++r)
        for (int c = 0; c != 3; ++c) inverse[r*4+c] = character[c*4+r];
    for (int c = 0; c != 3; ++c)
        inverse[12+c] = -(character[12]*inverse[c] + character[13]*inverse[4+c] + character[14]*inverse[8+c]);
    inverse[15] = 1;
    Multiply(world, inverse, model);
    return true;
}
void __fastcall WorldDetour(void* self, std::uint32_t hash, float* out) {
    g_world(self, hash, out);
    std::uint32_t bone = 0;
    float mount[16];
    if (out && Local(self) && Resolve(self, hash, bone) && bone == 45 && GetVrBodyLeftWristMount(45, mount) && Write(out, mount, sizeof(mount))) {
        RegisterVrBodyPlacedGrapple(mount);
        ++g_worldMoves;
    }
}
void __fastcall ModelDetour(void* self, std::uint32_t bone, float* out) {
    g_model(self, bone, out);
    float mount[16], model[16];
    if (out && bone == 45 && Local(self) && GetVrBodyLeftWristMount(45, mount) && WorldToModel(self, mount, model) && Write(out, model, sizeof(model))) {
        RegisterVrBodyPlacedGrapple(mount);
        ++g_modelMoves;
    }
}
float* __fastcall SpawnDetour(void* self, float* out) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    float* result = g_spawn(self, out);
    float wrist[16];
    if (caller == g_image + 0x94EF47 && out && Local(self) && GetVrBodyLeftWristMount(25, wrist) && Write(out, wrist+12, 12)) ++g_spawns;
    return result;
}
bool ProxyWrist(void* proxy, float world[16]) {
    std::uintptr_t vtable = 0, character = 0;
    std::uint32_t bone = ~0u;
    const auto p = reinterpret_cast<std::uintptr_t>(proxy);
    if (!p || !Read(&vtable, proxy, sizeof(vtable)) || vtable != g_image + 0x23E6418 ||
        !Read(&character, reinterpret_cast<const void*>(p+0x60), sizeof(character)) || !Local(reinterpret_cast<void*>(character)) ||
        !Read(&bone, reinterpret_cast<const void*>(p+0x58), sizeof(bone)) || (bone != 25 && bone != 176)) return false;
    float local[16], wrist[16];
    if (!Read(local, reinterpret_cast<const void*>(p+0x18), sizeof(local)) || !GetVrBodyLeftWristMount(25, wrist)) return false;
    for (float v : local) if (!std::isfinite(v) || std::abs(v) > 5.0f) return false;
    // The game's near-end setup (0x94C3D0) uses an identity local matrix on
    // safe hash 0x9C12B794 (rf_GrappleWirePosition, 176). Its rest pose is a
    // root-level animation marker, NOT a forearm socket; use the tracked wrist.
    Multiply(local, wrist, world);
    return true;
}
bool __fastcall ProxyPositionDetour(void* self, float* position, float* direction) {
    const bool result = g_proxyPosition(self, position, direction);
    float world[16];
    if (result && position && direction && ProxyWrist(self, world)) {
        if (Write(position, world+12, 12) && Write(direction, world+8, 12)) ++g_endpointMoves;
    }
    return result;
}
bool __fastcall ProxyMatrixDetour(void* self, float* matrix) {
    const bool result = g_proxyMatrix(self, matrix);
    float world[16];
    if (result && matrix && ProxyWrist(self, world) && Write(matrix, world, sizeof(world))) ++g_endpointMoves;
    return result;
}
void __fastcall ProxyGraphicsDetour(void* self, float* position) {
    g_proxyGraphics(self, position);
    float world[16];
    if (position && ProxyWrist(self, world) && Write(position, world+12, 12)) ++g_endpointMoves;
}
#if JC3VR_HAVE_MINHOOK
bool Hook(std::uintptr_t rva, const unsigned char* signature, std::size_t length, void* detour, void** original) {
    if (*original) return true;
    unsigned char actual[16]{};
    if (length > sizeof(actual) || !Read(actual, reinterpret_cast<void*>(g_image+rva), length) || std::memcmp(actual, signature, length)) {
        TraceRuntime("VR grapple: byte guard rejected rva=" + std::to_string(rva)); return false;
    }
    auto target = reinterpret_cast<void*>(g_image+rva);
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) status = MH_EnableHook(target);
    char text[128]; std::snprintf(text, sizeof(text), "VR grapple hook: rva=0x%llX status=%d", static_cast<unsigned long long>(rva), static_cast<int>(status));
    TraceRuntime(text);
    return status == MH_OK;
}
#endif
}
bool InstallVrGrappleHooks() {
#if JC3VR_HAVE_MINHOOK
    g_image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    static const unsigned char safe[]{0x44,0x0F,0xB7,0x81,0xC8,0x21,0,0};
    unsigned char actual[sizeof(safe)]{};
    if (!Read(actual, reinterpret_cast<void*>(g_image+0x79AB30), sizeof(actual)) || std::memcmp(actual,safe,sizeof(safe))) return false;
    g_safeIndex = reinterpret_cast<SafeIndexFn>(g_image+0x79AB30);
    static const unsigned char world[]{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20};
    static const unsigned char model[]{0x48,0x8B,0x89,0xF8,0x18,0,0,0xE9};
    static const unsigned char spawn[]{0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x81,0xEC,0x80,0,0,0};
    static const unsigned char position[]{0x48,0x8B,0xC4,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18};
    static const unsigned char matrix[]{0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xEC,0xE0,0,0,0};
    static const unsigned char graphics[]{0x48,0x83,0xEC,0x38,0x80,0x79,0x75,0,0x74};
    const bool a = Hook(0x79AEB0,world,sizeof(world),reinterpret_cast<void*>(&WorldDetour),reinterpret_cast<void**>(&g_world));
    const bool b = Hook(0x760210,model,sizeof(model),reinterpret_cast<void*>(&ModelDetour),reinterpret_cast<void**>(&g_model));
    const bool c = Hook(0x79BA30,spawn,sizeof(spawn),reinterpret_cast<void*>(&SpawnDetour),reinterpret_cast<void**>(&g_spawn));
    const bool d = Hook(0x8D8180,position,sizeof(position),reinterpret_cast<void*>(&ProxyPositionDetour),reinterpret_cast<void**>(&g_proxyPosition));
    const bool e = Hook(0x8D8280,matrix,sizeof(matrix),reinterpret_cast<void*>(&ProxyMatrixDetour),reinterpret_cast<void**>(&g_proxyMatrix));
    const bool f = Hook(0x8CA310,graphics,sizeof(graphics),reinterpret_cast<void*>(&ProxyGraphicsDetour),reinterpret_cast<void**>(&g_proxyGraphics));
    return a && b && c && d && e && f;
#else
    return false;
#endif
}
void VrGrapplePresentTick() {
    static std::uint64_t presents = 0;
    if (++presents % 900 == 0)
        TraceRuntime("VR grapple: gauntlet_world=" + std::to_string(g_worldMoves.load()) + " gauntlet_model=" + std::to_string(g_modelMoves.load()) +
            " wrist_spawns=" + std::to_string(g_spawns.load()) + " wrist_endpoint_updates=" + std::to_string(g_endpointMoves.load()));
}
}
