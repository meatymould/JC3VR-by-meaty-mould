// JC3 render-thread integration: D3D11 context hooks and native camera writes.
//
// Map (engine facts in docs/ENGINE_REFERENCE.md sections 2-4):
//   hooks:            MapDetour / UnmapDetour / UpdateSubresourceDetour (Globals
//                     block tracking, VR body palette re-assert + upload
//                     rewrite/carry), draw detours (scene-draw counting only),
//                     ExecuteCommandListDetour (pre-HUD copy), shader creation
//                     observers (terrain landmark)
//   eye scheduling:   SetNativeEyeProjection, ShouldAdvanceNativeEye,
//                     ForceNativeEyeProjection (off by default)
//   camera records:   SetNativeCameraOffset(ForCameras) writes owner+0x5C0/0x5C8
//                     (head rotation, eye offset), SyncNativeCameraViewProjection,
//                     SyncNativeCameraProjectionToEye, RebuildNativeCullingState
//                     (inverse views, V*P2, planes + companions; culling modes)
//   engine camera:    GetEngineCameraBasis/Position (captured before our write)
//   probes (default off, bounded): native renderer/camera getter/projection
//                     producer, stereo coherence, camera timeline, shadow, HUD
#include "game/camera_capture.h"
#include "game/camera_probe.h"
#include "game/camera_source_scan.h"
#include "game/vr_body_rig.h"
#include "game/engine_culling.h"
#include "game/vehicle_camera.h"
#include "core/logging.h"
#include "render/mapped_write_registry.h"
#include <Windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <utility>

namespace jc3vr {
namespace {
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using SetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateDomainShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**);
using SetPixelShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using SetDomainShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DomainShader*, ID3D11ClassInstance* const*, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUavsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using OMSetBlendStateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using NativeCameraGetterFn = void*(__fastcall*)(void*);
using NativeProjectionProducerFn = void(__fastcall*)(void*, void*, std::uint8_t);
using NativeFrameRendererFn = void(__fastcall*)(void*, void*);
UpdateSubresourceFn g_originalUpdateSubresource = nullptr;
MapFn g_originalMap = nullptr;
UnmapFn g_originalUnmap = nullptr;
SetConstantBuffersFn g_originalVSSetConstantBuffers = nullptr;
CreatePixelShaderFn g_originalCreatePixelShader = nullptr;
CreateDomainShaderFn g_originalCreateDomainShader = nullptr;
SetPixelShaderFn g_originalPSSetShader = nullptr;
SetDomainShaderFn g_originalDSSetShader = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
DrawIndexedInstancedFn g_originalDrawIndexedInstanced = nullptr;
DrawFn g_originalDraw = nullptr;
DrawInstancedFn g_originalDrawInstanced = nullptr;
OMSetRenderTargetsFn g_originalOMSetRenderTargets = nullptr;
OMSetRenderTargetsAndUavsFn g_originalOMSetRenderTargetsAndUavs = nullptr;
OMSetBlendStateFn g_originalOMSetBlendState = nullptr;
// HUD render-target probe. Its hooks are created at install but only
// ENABLED for single sampled frames (always-on versions of these hooks
// coincided with eye mislabelling/flashing, 2026-09-29). Render thread only.
ID3D11DeviceContext* g_immediateContext = nullptr;
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
ExecuteCommandListFn g_originalExecuteCommandList = nullptr;
std::array<void*, 6> g_hudProbeTargets{};
std::atomic<bool> g_hudProbeActive = false;
// Pre-HUD capture (probe v2, 2026-09-29: JC3 draws its HUD by executing one
// deferred command list on the immediate context with the backbuffer bound,
// right after the final full-screen pass). Render thread only.
bool g_preHudEnabled = false;
void* g_preHudBackbuffer = nullptr;
Microsoft::WRL::ComPtr<ID3D11Texture2D> g_preHudCopy;
bool g_preHudValid = false;
std::uint64_t g_preHudCaptures = 0;
int g_hudProbeFramesLeft = 0;
void* g_hudProbeBackbuffer = nullptr;
std::mutex g_hudProbeMutex;
struct HudProbeSegment {
    ID3D11DeviceContext* context = nullptr;
    DWORD thread = 0;
    bool execute = false;         // ExecuteCommandList marker on this context
    void* resource = nullptr;     // RTV0 resource (for markers: target bound then)
    UINT width = 0, height = 0;
    int format = 0;
    bool backbuffer = false;
    std::uint32_t draws = 0, blended = 0;
    void* psSource = nullptr;
    UINT psSourceWidth = 0, psSourceHeight = 0;
};
std::unordered_map<ID3D11DeviceContext*, std::size_t> g_hudProbeCurrent;
std::unordered_map<ID3D11DeviceContext*, bool> g_hudProbeBlendByContext;
std::vector<HudProbeSegment> g_hudProbeSegments;
NativeCameraGetterFn g_originalNativeCameraGetter = nullptr;
NativeProjectionProducerFn g_originalNativeProjectionProducer = nullptr;
NativeFrameRendererFn g_originalNativeFrameRenderer = nullptr;
std::atomic<bool> g_installed = false;
std::atomic<std::uint64_t> g_bufferUpdates = 0;
std::atomic<std::uint64_t> g_dynamicMapWrites = 0;
std::atomic<std::uint64_t> g_matrixSamples = 0;
std::atomic<std::uint64_t> g_projectionCandidates = 0;
std::atomic<std::uint64_t> g_rigidTransformCandidates = 0;
std::atomic<std::uint32_t> g_projectionReports = 0;
std::atomic<std::uint32_t> g_projectionBlockReports = 0;
std::atomic<std::uint32_t> g_sceneProjectionBlockReports = 0;
std::atomic<float> g_latestProjectionM00 = 0.0f;
std::atomic<float> g_latestProjectionM11 = 0.0f;
std::atomic<float> g_latestProjectionM20 = 0.0f;
std::atomic<float> g_latestProjectionM21 = 0.0f;
std::atomic<bool> g_nativeEyeProjectionEnabled = false;
std::atomic<bool> g_nativeEyeProjectionValid = false;
std::atomic<std::uint32_t> g_nativeEye = 0;
std::atomic<float> g_nativeProjectionM00 = 0.0f;
std::atomic<float> g_nativeProjectionM11 = 0.0f;
std::atomic<float> g_nativeProjectionM20 = 0.0f;
std::atomic<float> g_nativeProjectionM21 = 0.0f;
std::atomic<float> g_nativeCameraLocalX = 0.0f;
std::atomic<std::uint64_t> g_nativeProjectionWrites = 0;
std::atomic<bool> g_nativeCameraProducerBlockTraced = false;
std::atomic<bool> g_nativeFinalProjectionBlockTraced = false;
std::atomic<bool> g_nativeProjectionCallerStackTraced = false;
std::atomic<bool> g_nativeCameraGetterInstalled = false;
std::atomic<bool> g_nativeCameraObjectTraced = false;
std::atomic<bool> g_nativeProjectionProducerInstalled = false;
std::atomic<bool> g_nativeProjectionProducerInputTraced = false;
std::atomic<bool> g_nativeFrameRendererInstalled = false;
std::atomic<bool> g_nativeFrameRendererInlineInstalled = false;
std::atomic<std::uint32_t> g_nativeFrameRendererTraceCount = 0;
std::atomic<std::uint64_t> g_nativeFrameRendererCalls = 0;
std::atomic<bool> g_nativeCameraProbePermitted = false;
std::atomic<bool> g_nativeCameraProbeArmRequested = false;
std::atomic<std::uint64_t> g_verifiedFinalProjectionUploads = 0;
std::atomic<std::uint32_t> g_nativeFrameRendererEntryReports = 0;
std::atomic<ID3D11Buffer*> g_projectionBuffer = nullptr;
std::mutex g_projectionSnapshotMutex;
ID3D11Buffer* g_projectionBufferOwned = nullptr;
std::array<std::byte, 784> g_projectionSnapshot{};
bool g_projectionSnapshotValid = false;
std::atomic<std::uint64_t> g_forcedProjectionWrites = 0;
std::atomic<std::uint64_t> g_observedDraws = 0;
thread_local bool g_internalRenderPass = false;
std::atomic<bool> g_nativeViewProjectionSyncEnabled = false;
std::atomic<bool> g_nativeCameraProjectionSyncEnabled = false;
std::atomic<std::uint64_t> g_nativeCameraProjectionSyncWrites = 0;
std::atomic<std::uint64_t> g_nativeViewProjectionSyncWrites = 0;
std::array<std::atomic<std::uint64_t>, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> g_projectionVsBindings{};
std::atomic<ID3D11PixelShader*> g_activePixelShader = nullptr;
std::array<std::atomic<ID3D11PixelShader*>, 3> g_terrainShaders{};
std::array<std::atomic<std::uint64_t>, 4> g_terrainDraws{};
std::array<std::atomic<std::uint32_t>, 4> g_terrainReports{};
std::atomic<ID3D11DomainShader*> g_activeDomainShader = nullptr;
std::atomic<ID3D11DomainShader*> g_tessellatedTerrainDomainShader = nullptr;
std::atomic<bool> g_terrainCaptureInstalled = false;
std::mutex g_terrainShaderNamesMutex;
std::unordered_map<ID3D11PixelShader*, int> g_knownTerrainShaderNames;
std::unordered_map<ID3D11DomainShader*, bool> g_knownTerrainDomainShaderNames;

struct MappedWrite {
    void* bytes = nullptr;
    D3D11_MAP type = D3D11_MAP_READ;
};
MappedWriteRegistry<MappedWrite> g_mappedWrites;
struct LastNativeTranslation {
    bool valid = false;
    std::array<float, 3> written{};
    std::array<float, 3> appliedOffset{};
    // Direct head rotation keeps the engine basis it replaced, so a record the
    // engine did not refresh is not rotated a second time.
    bool rotationValid = false;
    std::array<float, 9> engineBasis{};
    std::array<float, 9> writtenBasis{};
};
std::atomic<std::uint64_t> g_nativeHeadRotationWrites = 0;
// The engine copies owner+0x5C0 into 0x5C8 once per frame (live timeline),
// so our values move between records. Bookkeeping is therefore keyed by the
// written value, not by address: any record holding a value we wrote maps
// back to the engine value it replaced. Guarded by g_nativeCameraOffsetMutex.
struct WrittenCameraValue {
    bool valid = false;
    std::array<float, 3> writtenTranslation{};
    std::array<float, 9> writtenBasis{};
    std::array<float, 3> engineTranslation{};
    std::array<float, 9> engineBasis{};
};
std::array<WrittenCameraValue, 256> g_writtenRing{};
// Additional render-camera records found by the engine FOV scan (mode 3);
// guarded by g_nativeCameraOffsetMutex.
std::vector<void*> g_extraCameras;
std::size_t g_writtenRingNext = 0;
std::atomic<std::uint64_t> g_ringRecognitions = 0;
// Engine camera basis of the projection camera (+0x94), captured under
// g_nativeCameraOffsetMutex during each camera write.
std::array<float, 9> g_engineCameraBasis{};
bool g_engineCameraBasisValid = false;
std::array<float, 3> g_engineCameraPosition{};
std::atomic<bool> g_pairedUploadSyncEnabled = false;
std::mutex g_pairedUploadMutex;
NativeCameraOffset g_pairedOffset{};
NativeEyeProjection g_pairedProjection{};
std::byte* g_pairedCamera = nullptr;
std::array<float, 3> g_pairedExpected{};
std::uint32_t g_pairedAppliesThisFrame = 0;
std::atomic<std::uint64_t> g_pairedUploadApplies = 0;
bool SetNativeCameraOffsetForCameras(const NativeCameraOffset& offset, unsigned cameraMask, bool trace);
void ReapplyPairedCameraAtUpload();
std::atomic<std::uint32_t> g_nativeHeadRotationRejects = 0;
std::mutex g_nativeCameraOffsetMutex;
std::unordered_map<std::uintptr_t, LastNativeTranslation> g_lastNativeTranslations;
std::atomic<std::uint64_t> g_nativeCameraOffsetWrites = 0;
std::atomic<bool> g_shadowProbeEnabled = false;
std::atomic<std::uint64_t> g_shadowProbeDraws = 0;
std::atomic<std::uint32_t> g_shadowProbeReports = 0;
// Read-only stereo coherence probe. Every 784-byte upload is counted (not only
// the canonical-looking ones), and armed draws read back the block the GPU
// actually has bound, so eye ownership is not inferred from our own writes.
std::atomic<bool> g_coherenceProbeEnabled = false;
std::atomic<bool> g_coherenceDrawArmed = false;
std::atomic<std::uint32_t> g_coherenceDrawReports = 0;
std::atomic<std::uint32_t> g_coherenceUploadReports = 0;
std::atomic<std::uint64_t> g_all784Uploads = 0;
std::atomic<std::uint64_t> g_globalsProjectionUploads = 0;
// Set when this frame's scene projection was recovered from Globals c29-c32.
// Later identity-view uploads (HUD/viewmodel-style passes carrying JC3's
// default projection) must not overwrite it as "the rendered projection".
std::atomic<bool> g_sceneProjectionThisFrame = false;
// Exact per-eye expectation for Globals c29-c32 rows 0-2 (rotation x P),
// recorded when the eye's camera is written. Matching uploads against it
// identifies the rendered eye without reconstructing P from a camera
// rotation that may already have moved (head motion made the W*VP
// recovery report m20 of -0.216/-0.264 instead of +-0.2425).
struct ExpectedEyeUpload {
    bool valid = false;
    std::array<float, 12> rows{};
    float m00 = 0.0f, m11 = 0.0f, m20 = 0.0f, m21 = 0.0f;
};
std::mutex g_expectedEyeMutex;
std::array<ExpectedEyeUpload, 2> g_expectedEyeUploads{};
// Presents since a scene projection was last recovered (in-world signal).
std::atomic<std::uint32_t> g_presentsSinceSceneProjection = 1000;
// owner+0x5C8 camera +0x94 world transform, validated at the Present eye
// handoff; used to recover P from the rotated Globals c29-c32.
std::atomic<const float*> g_projectionCameraWorld = nullptr;
bool IsNativeWorldTransform(const float* matrix);
bool RebuildNativeViewProjection(std::byte* camera);
std::atomic<std::uint32_t> g_lastCameraOffsetEye = 0;
ID3D11Buffer* g_coherenceStaging = nullptr;
std::array<std::atomic<std::uint32_t>, 8> g_projectionSyncRejects{};

bool IsRigidTransform(const MatrixDiagnostics& matrix);
bool HasNativeCameraSignature(const std::byte* camera);

struct ImageTextRange {
    std::uintptr_t imageBase = 0;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

// The projection matrix itself lives in a GPU upload buffer, not in the game
// camera object.  The call stack at this exact upload is the bounded bridge
// back to the owning JC3 producer.  Reject addresses outside JustCause3.exe's
// executable section so a hook/trampoline frame can never be mistaken for a
// game caller.
ImageTextRange GetMainImageTextRange() {
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!imageBase) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        const auto begin = imageBase + section->VirtualAddress;
        const auto size = std::max<std::uint32_t>(section->Misc.VirtualSize, section->SizeOfRawData);
        if (size) return {imageBase, begin, begin + size};
    }
    return {};
}

std::string Hex(std::uintptr_t value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << value;
    return stream.str();
}

void TraceProjectionProducerCallers() {
    if (g_nativeProjectionCallerStackTraced.exchange(true)) return;
    const auto text = GetMainImageTextRange();
    if (!text.imageBase || text.end <= text.begin) {
        TraceRuntime("native projection caller trace rejected: JustCause3.exe executable range unavailable");
        return;
    }

    void* frames[24]{};
    const USHORT count = RtlCaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    std::string result = "native projection caller stack: exe_base=" + Hex(text.imageBase) +
                         " text=[" + Hex(text.begin - text.imageBase) + "," + Hex(text.end - text.imageBase) + ")";
    std::uint32_t gameFrames = 0;
    for (USHORT index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
        if (address < text.begin || address >= text.end) continue;
        result += " frame" + std::to_string(gameFrames++) + "=rva" + Hex(address - text.imageBase);
    }
    if (!gameFrames) result += " no validated game frames";
    TraceRuntime(result);
}

bool IsReadableRange(const void* address, std::size_t byteCount) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !byteCount || VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + byteCount;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return end >= start && end <= regionEnd;
}

bool IsWritableRange(const void* address, std::size_t byteCount) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !byteCount || VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const DWORD protection = info.Protect & 0xff;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + byteCount;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return end >= start && end <= regionEnd;
}

std::string FormatFloat4(const float* values) {
    return "[" + std::to_string(values[0]) + "," + std::to_string(values[1]) + "," +
           std::to_string(values[2]) + "," + std::to_string(values[3]) + "]";
}

std::string FormatMatrixAt(const std::byte* bytes, std::size_t offset) {
    const auto* matrix = reinterpret_cast<const float*>(bytes + offset);
    return "@" + Hex(offset) + "=" + FormatFloat4(matrix) + "/" +
           FormatFloat4(matrix + 4) + "/" + FormatFloat4(matrix + 8) + "/" +
           FormatFloat4(matrix + 12);
}

// 0x0F1D10 is the normal per-frame render entry immediately enclosing the
// validated final-projection upload.  Its first three-byte instruction saves
// RSP in RAX, so the hook is placed at 0x0F1D13 immediately after that setup.
// RCX/RDX still hold the live arguments and the trampoline retains the normal
// prologue.  Its RCX is the live render-frame object whose +0x720 subobject
// flows straight into the projection producer.  The traces below intentionally
// read only fields used by that proven caller chain; this is not a scan.
void TraceNativeFrameRendererState(void* frame, void* frameArgs) {
    constexpr std::uintptr_t kRendererGlobalRva = 0x02ED0E18;
    constexpr std::uintptr_t kCameraStateGlobalRva = 0x02ED0E70;
    constexpr std::size_t kFrameCameraBlockOffset = 0x720;
    constexpr std::size_t kFrameCameraBlockBytes = 0x460;
    constexpr std::size_t kRendererStateBytes = 0x1D40;
    constexpr std::size_t kCameraStateBytes = 0xA0;

    const auto frameCall = g_nativeFrameRendererCalls.fetch_add(1) + 1;
    if (g_nativeFrameRendererEntryReports.fetch_add(1) == 0) {
        TraceRuntime("native camera probe: first guarded renderer entry after explicit arm");
    }
    // Capture the boot/menu state and then spaced live snapshots.  This lets
    // an on-foot transition show precisely which existing camera-path fields
    // change without flooding the bootstrap trace.
    if (frameCall != 1 && frameCall % 900 != 0) return;
    if (g_nativeFrameRendererTraceCount.fetch_add(1) >= 24) return;

    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* renderer = nullptr;
    void* cameraState = nullptr;
    if (imageBase) {
        if (IsReadableRange(reinterpret_cast<const void*>(imageBase + kRendererGlobalRva), sizeof(renderer))) {
            std::memcpy(&renderer, reinterpret_cast<const void*>(imageBase + kRendererGlobalRva), sizeof(renderer));
        }
        if (IsReadableRange(reinterpret_cast<const void*>(imageBase + kCameraStateGlobalRva), sizeof(cameraState))) {
            std::memcpy(&cameraState, reinterpret_cast<const void*>(imageBase + kCameraStateGlobalRva), sizeof(cameraState));
        }
    }
    if (!frame || !IsReadableRange(static_cast<const std::byte*>(frame) + kFrameCameraBlockOffset, kFrameCameraBlockBytes) ||
        !IsReadableRange(renderer, kRendererStateBytes) ||
        !IsReadableRange(cameraState, kCameraStateBytes)) {
        TraceRuntime("native frame renderer trace rejected: frame=" + Hex(reinterpret_cast<std::uintptr_t>(frame)) +
                     " renderer=" + Hex(reinterpret_cast<std::uintptr_t>(renderer)) +
                     " camera_state=" + Hex(reinterpret_cast<std::uintptr_t>(cameraState)));
        return;
    }

    const auto* frameBytes = static_cast<const std::byte*>(frame);
    const auto* rendererBytes = static_cast<const std::byte*>(renderer);
    const auto* cameraBytes = static_cast<const std::byte*>(cameraState);
    const auto* cameraFloats = reinterpret_cast<const float*>(cameraBytes);
    const auto* fovAndBounds = reinterpret_cast<const float*>(rendererBytes + 0x1538);
    const auto* frameBlock = reinterpret_cast<const float*>(frameBytes + kFrameCameraBlockOffset);
    // The candidate camera must be reachable from the known owner, readable,
    // finite, and match the already observed live projection. This probe is
    // observational: a failure only logs and returns before any candidate use.
    void* cameraOwner = nullptr;
    void* camera = nullptr;
    constexpr std::uintptr_t kCameraOwnerGlobalRva = 0x02ED0E20;
    constexpr std::size_t kCameraProjectionOffset = 0x194;
    bool cameraSignatureValid = false;
    if (imageBase && IsReadableRange(reinterpret_cast<const void*>(imageBase + kCameraOwnerGlobalRva), sizeof(cameraOwner))) {
        std::memcpy(&cameraOwner, reinterpret_cast<const void*>(imageBase + kCameraOwnerGlobalRva), sizeof(cameraOwner));
        if (cameraOwner && IsReadableRange(static_cast<const std::byte*>(cameraOwner) + 0x5C8, sizeof(camera))) {
            std::memcpy(&camera, static_cast<const std::byte*>(cameraOwner) + 0x5C8, sizeof(camera));
        }
    }
    if (camera && IsReadableRange(static_cast<const std::byte*>(camera) + kCameraProjectionOffset, sizeof(float) * 16)) {
        std::array<float, 16> projection{};
        std::memcpy(projection.data(), static_cast<const std::byte*>(camera) + kCameraProjectionOffset, sizeof(projection));
        bool finite = true;
        for (float value : projection) finite = finite && std::isfinite(value);
        const auto liveM00 = g_latestProjectionM00.load();
        const auto liveM11 = g_latestProjectionM11.load();
        cameraSignatureValid = finite && liveM00 > 0.1f && liveM11 > 0.1f &&
            std::abs(projection[0] - liveM00) < 0.01f && std::abs(projection[5] - liveM11) < 0.01f &&
            std::abs(projection[11] + 1.0f) < 0.001f && std::abs(projection[14] - 0.1f) < 0.001f;
    }
    if (!cameraSignatureValid) {
        TraceRuntime("native camera probe guard rejected candidate: owner=" + Hex(reinterpret_cast<std::uintptr_t>(cameraOwner)) +
                     " camera=" + Hex(reinterpret_cast<std::uintptr_t>(camera)) + " reason=unreadable_or_projection_signature_mismatch");
        return;
    }
    std::string result = "native frame renderer state: call=" + std::to_string(frameCall) +
        " frame=" + Hex(reinterpret_cast<std::uintptr_t>(frame)) +
        " args=" + Hex(reinterpret_cast<std::uintptr_t>(frameArgs)) +
        " block+0x720@0=" + FormatFloat4(frameBlock) +
        " @0x440=" + FormatFloat4(reinterpret_cast<const float*>(frameBytes + kFrameCameraBlockOffset + 0x440)) +
        " renderer=" + Hex(reinterpret_cast<std::uintptr_t>(renderer)) +
        " fov_bounds=" + FormatFloat4(fovAndBounds) + "/" + FormatFloat4(fovAndBounds + 4) +
        " camera_state=" + Hex(reinterpret_cast<std::uintptr_t>(cameraState)) +
        " camera_owner=" + Hex(reinterpret_cast<std::uintptr_t>(cameraOwner)) +
        " camera=" + Hex(reinterpret_cast<std::uintptr_t>(camera)) +
        " state_vec0c=" + FormatFloat4(cameraFloats + 0x0C / sizeof(float)) +
        " state_vec24=" + FormatFloat4(cameraFloats + 0x24 / sizeof(float)) +
        " state_vec90=" + FormatFloat4(cameraFloats + 0x90 / sizeof(float));
    TraceRuntime(result);
}

void __fastcall NativeFrameRendererDetour(void* frame, void* frameArgs) {
    TraceNativeFrameRendererState(frame, frameArgs);
    g_originalNativeFrameRenderer(frame, frameArgs);
}

bool InstallInlineNativeFrameRendererProbe(void* target) {
    // The checked bytes are the post-`mov rax,rsp` prologue of the exact
    // installed JC3 build.  Refuse to write on even a one-byte mismatch.
    constexpr std::array<std::uint8_t, 20> kExpectedPrologue = {
        0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x8D, 0x68, 0xC8, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00};
    constexpr std::size_t kPatchBytes = kExpectedPrologue.size();
    std::array<std::uint8_t, kPatchBytes> original{};
    std::memcpy(original.data(), target, original.size());
    if (original != kExpectedPrologue) {
        TraceRuntime("native frame renderer inline probe rejected: build signature mismatch at rva=0xF1D13");
        return false;
    }

    // MinHook cannot relocate this compiler-specific stack-establishing
    // prologue.  The bridge below has no game-specific control flow: it saves
    // RAX/RCX/RDX, calls the bounded observer, restores the exact entry state,
    // and executes the copied original prologue before returning to JC3.
    auto* bridge = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!bridge) {
        TraceRuntime("native frame renderer inline probe rejected: VirtualAlloc failed=" + std::to_string(GetLastError()));
        return false;
    }
    auto* gateway = bridge + 48;
    std::memcpy(gateway, original.data(), original.size());
    std::size_t gatewayOffset = kPatchBytes;
    gateway[gatewayOffset++] = 0x49; gateway[gatewayOffset++] = 0xBB; // mov r11, continuation
    const auto continuation = reinterpret_cast<std::uintptr_t>(target) + kPatchBytes;
    std::memcpy(gateway + gatewayOffset, &continuation, sizeof(continuation)); gatewayOffset += sizeof(continuation);
    gateway[gatewayOffset++] = 0x41; gateway[gatewayOffset++] = 0xFF; gateway[gatewayOffset++] = 0xE3; // jmp r11

    std::size_t bridgeOffset = 0;
    bridge[bridgeOffset++] = 0x50; // push rax: preserves the original rsp captured before this site
    bridge[bridgeOffset++] = 0x51; // push rcx
    bridge[bridgeOffset++] = 0x52; // push rdx
    bridge[bridgeOffset++] = 0x48; bridge[bridgeOffset++] = 0x83; bridge[bridgeOffset++] = 0xEC; bridge[bridgeOffset++] = 0x20; // shadow space
    bridge[bridgeOffset++] = 0x48; bridge[bridgeOffset++] = 0xB8; // mov rax, trace function
    const auto traceFunction = reinterpret_cast<std::uintptr_t>(&TraceNativeFrameRendererState);
    std::memcpy(bridge + bridgeOffset, &traceFunction, sizeof(traceFunction)); bridgeOffset += sizeof(traceFunction);
    bridge[bridgeOffset++] = 0xFF; bridge[bridgeOffset++] = 0xD0; // call rax
    bridge[bridgeOffset++] = 0x48; bridge[bridgeOffset++] = 0x83; bridge[bridgeOffset++] = 0xC4; bridge[bridgeOffset++] = 0x20;
    bridge[bridgeOffset++] = 0x5A; bridge[bridgeOffset++] = 0x59; bridge[bridgeOffset++] = 0x58; // restore rdx, rcx, rax
    bridge[bridgeOffset++] = 0x49; bridge[bridgeOffset++] = 0xBB; // mov r11, gateway
    const auto gatewayAddress = reinterpret_cast<std::uintptr_t>(gateway);
    std::memcpy(bridge + bridgeOffset, &gatewayAddress, sizeof(gatewayAddress)); bridgeOffset += sizeof(gatewayAddress);
    bridge[bridgeOffset++] = 0x41; bridge[bridgeOffset++] = 0xFF; bridge[bridgeOffset++] = 0xE3; // jmp r11

    std::array<std::uint8_t, kPatchBytes> patch{};
    patch.fill(0x90);
    patch[0] = 0xFF; patch[1] = 0x25; // jmp qword ptr [rip+0]
    const auto bridgeAddress = reinterpret_cast<std::uintptr_t>(bridge);
    std::memcpy(patch.data() + 6, &bridgeAddress, sizeof(bridgeAddress));
    DWORD oldProtection = 0;
    if (!VirtualProtect(target, kPatchBytes, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        TraceRuntime("native frame renderer inline probe rejected: VirtualProtect failed=" + std::to_string(GetLastError()));
        VirtualFree(bridge, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(target, patch.data(), patch.size());
    FlushInstructionCache(GetCurrentProcess(), target, kPatchBytes);
    DWORD ignored = 0;
    VirtualProtect(target, kPatchBytes, oldProtection, &ignored);
    g_nativeFrameRendererInlineInstalled.store(true);
    TraceRuntime("native frame renderer inline probe installed at rva=0xF1D13 after exact signature check; original prologue preserved");
    return true;
}

void InstallNativeFrameRendererProbe() {
    if (g_nativeFrameRendererInstalled.exchange(true)) return;
    constexpr std::uintptr_t kFrameRendererRva = 0x000F1D13;
    const auto text = GetMainImageTextRange();
    const auto target = text.imageBase + kFrameRendererRva;
    if (!text.imageBase || target < text.begin || target >= text.end) {
        TraceRuntime("native frame renderer probe rejected: target outside executable text");
        return;
    }
    const auto create = MH_CreateHook(reinterpret_cast<void*>(target), &NativeFrameRendererDetour,
                                      reinterpret_cast<void**>(&g_originalNativeFrameRenderer));
    const auto enable = create == MH_OK ? MH_EnableHook(reinterpret_cast<void*>(target)) : create;
    if (enable == MH_OK) {
        TraceRuntime("native frame renderer probe installed at rva=0xF1D13 after stack setup; tracing only the validated +0x720 camera render block");
    } else {
        TraceRuntime("native frame renderer probe unavailable: MinHook=" + std::to_string(static_cast<int>(enable)));
    }
}

void TryArmNativeFrameRendererProbe() {
    if (!g_nativeCameraProbePermitted.load() || g_nativeCameraProbeArmRequested.load()) return;
    if ((GetAsyncKeyState(VK_F4) & 1) == 0) return;
    const auto uploads = g_verifiedFinalProjectionUploads.load();
    // F4 is deliberately a player-confirmed in-world action. Requiring a
    // sustained sequence of the exact live final projection also prevents an
    // accidental startup/menu key press from arming this probe.
    if (uploads < 120) {
        TraceRuntime("native camera probe arm ignored: F4 received before 120 verified live projection uploads; count=" + std::to_string(uploads));
        return;
    }
    if (g_nativeCameraProbeArmRequested.exchange(true)) return;
    TraceRuntime("native camera probe arm accepted: player-confirmed F4 after verified projection uploads=" + std::to_string(uploads));
    InstallNativeFrameRendererProbe();
}

// Static inspection of the validated caller chain shows that 0x185E7B calls
// this getter, then copies [returned_camera + 0x194, 64 bytes] into the final
// 784-byte projection producer.  Watching that one return gives us the owning
// game camera record without scanning unrelated render constant buffers.
void TraceNativeCameraObject(void* camera, void* owner) {
    constexpr std::size_t kProjectionOffset = 0x194;
    constexpr std::size_t kInspectBytes = 0x600;
    if (!IsReadableRange(camera, kInspectBytes)) {
        TraceRuntime("native camera getter returned unreadable candidate=" + Hex(reinterpret_cast<std::uintptr_t>(camera)));
        return;
    }

    // The getter is also used during renderer start-up, where it returns an
    // identity/default record.  Only retain the invocation whose embedded
    // projection is the same canonical live projection uploaded at the
    // proven producer; this is the camera we can safely act on.
    const auto* liveProjection = static_cast<const float*>(static_cast<const void*>(
        static_cast<const std::byte*>(camera) + kProjectionOffset));
    if (!(std::abs(liveProjection[0]) > 0.1f && std::abs(liveProjection[5]) > 0.1f &&
          std::abs(liveProjection[11] + 1.0f) < 0.001f &&
          std::abs(liveProjection[14] - 0.1f) < 0.001f)) return;
    if (g_nativeCameraObjectTraced.exchange(true)) return;

    std::array<std::byte, kInspectBytes> snapshot{};
    std::memcpy(snapshot.data(), camera, snapshot.size());
    const auto* floats = reinterpret_cast<const float*>(snapshot.data());
    const auto* projection = reinterpret_cast<const float*>(snapshot.data() + kProjectionOffset);
    std::string result = "native camera object validated: ptr=" + Hex(reinterpret_cast<std::uintptr_t>(camera)) +
                         " projection@0x194 rows=" + FormatFloat4(projection) + "/" +
                         FormatFloat4(projection + 4) + "/" + FormatFloat4(projection + 8) + "/" +
                         FormatFloat4(projection + 12) +
                         " vectors@0x74=" + FormatFloat4(floats + 0x74 / sizeof(float)) +
                         " @0x84=" + FormatFloat4(floats + 0x84 / sizeof(float));

    // The surrounding producer copies these two exact, non-16-byte-aligned
    // records from the camera object.  They are the only direct view-matrix
    // candidates on the path to the final projection, so inspect them
    // explicitly rather than performing an address-wide matrix search.
    result += " camera_matrices=" + FormatMatrixAt(snapshot.data(), 0x94) +
              ";" + FormatMatrixAt(snapshot.data(), 0xD4) +
              ";" + FormatMatrixAt(snapshot.data(), 0x154) +
              ";" + FormatMatrixAt(snapshot.data(), 0x194) +
              ";" + FormatMatrixAt(snapshot.data(), 0x1D4);

    std::uint32_t transforms = 0;
    for (std::size_t offset = 0; offset + sizeof(float) * 16 <= snapshot.size(); offset += sizeof(float) * 4) {
        std::array<float, 16> matrix{};
        std::memcpy(matrix.data(), snapshot.data() + offset, sizeof(matrix));
        bool finite = true;
        for (const float value : matrix) finite = finite && std::isfinite(value);
        if (!finite || !IsRigidTransform(DiagnoseMatrix(matrix))) continue;
        result += " rigid@" + Hex(offset) + ".translation=" +
                  FormatFloat4(matrix.data() + 12);
        ++transforms;
    }
    result += " rigid_count=" + std::to_string(transforms);

    // The camera-state owner contains a paired record at +0x5C0.  The
    // neighbouring engine accessor copies that record's matrices at +0x94
    // and +0xD4, while this projection path reads +0x5C8.  This is the
    // specific sibling relationship that can contain the world/view pose.
    constexpr std::size_t kOwnerCameraOffset = 0x5C0;
    constexpr std::size_t kOwnerProjectionCameraOffset = 0x5C8;
    if (IsReadableRange(owner, kOwnerProjectionCameraOffset + sizeof(void*))) {
        const auto* ownerBytes = static_cast<const std::byte*>(owner);
        void* pairedCamera = nullptr;
        void* projectionCamera = nullptr;
        std::memcpy(&pairedCamera, ownerBytes + kOwnerCameraOffset, sizeof(pairedCamera));
        std::memcpy(&projectionCamera, ownerBytes + kOwnerProjectionCameraOffset, sizeof(projectionCamera));
        result += " owner=" + Hex(reinterpret_cast<std::uintptr_t>(owner)) +
                  " paired@0x5c0=" + Hex(reinterpret_cast<std::uintptr_t>(pairedCamera)) +
                  " projection@0x5c8=" + Hex(reinterpret_cast<std::uintptr_t>(projectionCamera));
        if (IsReadableRange(pairedCamera, 0x214)) {
            std::array<std::byte, 0x214> pairedSnapshot{};
            std::memcpy(pairedSnapshot.data(), pairedCamera, pairedSnapshot.size());
            result += " paired_matrices=" + FormatMatrixAt(pairedSnapshot.data(), 0x94) +
                      ";" + FormatMatrixAt(pairedSnapshot.data(), 0xD4) +
                      ";" + FormatMatrixAt(pairedSnapshot.data(), 0x154) +
                      ";" + FormatMatrixAt(pairedSnapshot.data(), 0x194);
        }
    }
    TraceRuntime(result);
}

void* __fastcall NativeCameraGetterDetour(void* owner) {
    auto* camera = g_originalNativeCameraGetter(owner);
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    // The call after the getter is at 0x185E80 in the verified executable.
    // Filtering here avoids treating other consumers of the same getter as
    // the projection-producing camera path.
    if (imageBase && returnAddress - imageBase == 0x185E80) TraceNativeCameraObject(camera, owner);
    return camera;
}

void InstallNativeCameraGetterProbe() {
    if (g_nativeCameraGetterInstalled.exchange(true)) return;
    const auto text = GetMainImageTextRange();
    constexpr std::uintptr_t kGetterRva = 0x009D380;
    const auto target = text.imageBase + kGetterRva;
    if (!text.imageBase || target < text.begin || target >= text.end) {
        TraceRuntime("native camera getter probe rejected: target outside executable text");
        return;
    }
    const auto create = MH_CreateHook(reinterpret_cast<void*>(target), &NativeCameraGetterDetour,
                                      reinterpret_cast<void**>(&g_originalNativeCameraGetter));
    const auto enable = create == MH_OK ? MH_EnableHook(reinterpret_cast<void*>(target)) : create;
    if (enable == MH_OK) {
        TraceRuntime("native camera getter probe installed at rva=0x9D380 for validated projection producer");
    } else {
        TraceRuntime("native camera getter probe unavailable: MinHook=" + std::to_string(static_cast<int>(enable)));
    }
}

void TraceNativeProjectionProducerInput(void* renderer, void* input) {
    constexpr std::uintptr_t kRendererGlobalRva = 0x02ED0E18;
    constexpr std::size_t kInputFirstOffset = 0x454;
    constexpr std::size_t kInputFirstBytes = 0x200;
    constexpr std::size_t kInputSecondOffset = 0x654;
    constexpr std::size_t kInputSecondBytes = 0x120;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* expectedRenderer = nullptr;
    if (imageBase && IsReadableRange(reinterpret_cast<const void*>(imageBase + kRendererGlobalRva), sizeof(expectedRenderer))) {
        std::memcpy(&expectedRenderer, reinterpret_cast<const void*>(imageBase + kRendererGlobalRva), sizeof(expectedRenderer));
    }
    if (renderer != expectedRenderer || !IsReadableRange(static_cast<const std::byte*>(input) + kInputSecondOffset, kInputSecondBytes)) {
        return;
    }
    if (g_nativeProjectionProducerInputTraced.exchange(true)) return;

    std::array<std::byte, kInputFirstBytes + kInputSecondBytes> snapshot{};
    std::memcpy(snapshot.data(), static_cast<const std::byte*>(input) + kInputFirstOffset, kInputFirstBytes);
    std::memcpy(snapshot.data() + kInputFirstBytes, static_cast<const std::byte*>(input) + kInputSecondOffset, kInputSecondBytes);
    std::string result = "native projection producer input: renderer=" + Hex(reinterpret_cast<std::uintptr_t>(renderer)) +
                         " first=[input+0x454,0x654) second=[input+0x654,0x774)";
    std::uint32_t transforms = 0;
    for (std::size_t offset = 0; offset + sizeof(float) * 16 <= snapshot.size(); offset += sizeof(float)) {
        std::array<float, 16> matrix{};
        std::memcpy(matrix.data(), snapshot.data() + offset, sizeof(matrix));
        bool finite = true;
        for (const float value : matrix) finite = finite && std::isfinite(value);
        if (!finite || !IsRigidTransform(DiagnoseMatrix(matrix))) continue;
        result += " rigid_input@" + Hex((offset < kInputFirstBytes ? kInputFirstOffset : kInputSecondOffset) +
                                         (offset < kInputFirstBytes ? offset : offset - kInputFirstBytes)) +
                  ".translation=" + FormatFloat4(matrix.data() + 12);
        if (++transforms == 12) break;
    }
    if (!transforms) result += " no rigid 4x4 candidate";
    TraceRuntime(result);
}

void __fastcall NativeProjectionProducerDetour(void* renderer, void* input, std::uint8_t mode) {
    TraceNativeProjectionProducerInput(renderer, input);
    g_originalNativeProjectionProducer(renderer, input, mode);
}

void InstallNativeProjectionProducerProbe() {
    if (g_nativeProjectionProducerInstalled.exchange(true)) return;
    const auto text = GetMainImageTextRange();
    constexpr std::uintptr_t kProducerRva = 0x0185743;
    const auto target = text.imageBase + kProducerRva;
    if (!text.imageBase || target < text.begin || target >= text.end) {
        TraceRuntime("native projection producer probe rejected: target outside executable text");
        return;
    }
    const auto create = MH_CreateHook(reinterpret_cast<void*>(target), &NativeProjectionProducerDetour,
                                      reinterpret_cast<void**>(&g_originalNativeProjectionProducer));
    const auto enable = create == MH_OK ? MH_EnableHook(reinterpret_cast<void*>(target)) : create;
    if (enable == MH_OK) {
        TraceRuntime("native projection producer input probe installed at rva=0x185743");
    } else {
        TraceRuntime("native projection producer probe unavailable: MinHook=" + std::to_string(static_cast<int>(enable)));
    }
}

void TraceProjectionRendererState() {
    constexpr std::uintptr_t kRendererGlobalRva = 0x02ED0E18;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* renderer = nullptr;
    if (!imageBase || !IsReadableRange(reinterpret_cast<const void*>(imageBase + kRendererGlobalRva), sizeof(renderer))) return;
    std::memcpy(&renderer, reinterpret_cast<const void*>(imageBase + kRendererGlobalRva), sizeof(renderer));
    constexpr std::size_t kLastRequiredOffset = 0x1D2C + sizeof(float) * 16;
    if (!IsReadableRange(renderer, kLastRequiredOffset)) return;

    const auto* bytes = static_cast<const std::byte*>(renderer);
    const auto* fovAndBounds = reinterpret_cast<const float*>(bytes + 0x1538);
    const auto* firstInput = reinterpret_cast<const float*>(bytes + 0x14F0);
    void* producerDependency = nullptr;
    std::memcpy(&producerDependency, bytes + 0x18D0, sizeof(producerDependency));
    std::array<std::byte, sizeof(float) * 16> producerMatrix{};
    std::array<std::byte, sizeof(float) * 16> copiedMatrix{};
    std::memcpy(producerMatrix.data(), bytes + 0x1880, producerMatrix.size());
    std::memcpy(copiedMatrix.data(), bytes + 0x1D2C, copiedMatrix.size());
    std::string result = "native projection renderer state: ptr=" + Hex(reinterpret_cast<std::uintptr_t>(renderer)) +
                 " fields@0x14f0=" + FormatFloat4(firstInput) +
                 " fields@0x1538=" + FormatFloat4(fovAndBounds) + "/" + FormatFloat4(fovAndBounds + 4) +
                 " producer_dependency@0x18d0=" + Hex(reinterpret_cast<std::uintptr_t>(producerDependency)) +
                 " matrix@0x1880=" + FormatMatrixAt(producerMatrix.data(), 0) +
                 " copied@0x1d2c=" + FormatMatrixAt(copiedMatrix.data(), 0);
    // The enclosing projection producer copies exactly 0x38 bytes from this
    // dependency at +0x190 immediately before it copies the live camera
    // projection.  Capture that declared record only; it is the next direct
    // candidate for the camera basis/world-position data.
    if (IsReadableRange(static_cast<const std::byte*>(producerDependency) + 0x190, 0x38)) {
        std::array<float, 14> dependencyRecord{};
        std::memcpy(dependencyRecord.data(), static_cast<const std::byte*>(producerDependency) + 0x190, 0x38);
        result += " dependency_record@0x190=" + FormatFloat4(dependencyRecord.data()) + "/" +
                  FormatFloat4(dependencyRecord.data() + 4) + "/" + FormatFloat4(dependencyRecord.data() + 8) +
                  "/tail=[" + std::to_string(dependencyRecord[12]) + "," + std::to_string(dependencyRecord[13]) + "]";
    }
    TraceRuntime(result);
}

bool IsRigidTransform(const MatrixDiagnostics& matrix) {
    return std::abs(std::abs(matrix.determinant) - 1.0f) < 0.15f &&
           std::abs(matrix.basisLengths[0] - 1.0f) < 0.15f &&
           std::abs(matrix.basisLengths[1] - 1.0f) < 0.15f &&
           std::abs(matrix.basisLengths[2] - 1.0f) < 0.15f &&
           matrix.orthogonalityError < 0.12f;
}

bool IsCanonicalProjection(const std::array<float, 16>& matrix, const MatrixDiagnostics& diagnostics) {
    return diagnostics.resemblesProjection &&
           std::abs(matrix[0]) > 0.1f && std::abs(matrix[0]) < 20.0f &&
           std::abs(matrix[5]) > 0.1f && std::abs(matrix[5]) < 20.0f &&
           std::abs(matrix[11]) > 0.5f && std::abs(matrix[11]) < 1.5f &&
           std::abs(matrix[15]) < 0.001f;
}

std::string SnapshotFloats(const void* source, UINT byteCount) {
    const auto* values = static_cast<const float*>(source);
    std::string result;
    for (UINT index = 0; index < byteCount / sizeof(float); ++index) {
        if (!result.empty()) result += ',';
        result += std::to_string(index);
        result += '=';
        result += std::to_string(values[index]);
    }
    return result;
}

// Read-only summary of both known camera records for the coherence probe:
// world (+0x94), P (+0x154) and VP (+0x194), plus whether the +0x94
// translation still holds the last per-eye 6DOF write.
std::string DescribeProjectionCameraRecords() {
    constexpr std::uintptr_t kOwnerGlobalRva = 0x02ED0E20;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* owner = nullptr;
    if (!imageBase || !IsReadableRange(reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner))) return "cameras=unavailable";
    std::memcpy(&owner, reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner));
    if (!owner || !IsReadableRange(static_cast<const std::byte*>(owner) + 0x5C0, sizeof(void*) * 2)) return "cameras=no-owner";
    std::string result;
    for (const std::size_t slot : {std::size_t{0x5C0}, std::size_t{0x5C8}}) {
        const std::byte* camera = nullptr;
        std::memcpy(&camera, static_cast<const std::byte*>(owner) + slot, sizeof(camera));
        result += (slot == 0x5C0 ? "cam5c0" : " cam5c8");
        if (!HasNativeCameraSignature(camera)) { result += "=invalid"; continue; }
        const auto* world = reinterpret_cast<const float*>(camera + 0x94);
        bool holdsEyeWrite = false;
        {
            std::lock_guard lock(g_nativeCameraOffsetMutex);
            const auto it = g_lastNativeTranslations.find(reinterpret_cast<std::uintptr_t>(world));
            holdsEyeWrite = it != g_lastNativeTranslations.end() && it->second.valid &&
                std::abs(world[12] - it->second.written[0]) < 0.0005f &&
                std::abs(world[13] - it->second.written[1]) < 0.0005f &&
                std::abs(world[14] - it->second.written[2]) < 0.0005f;
        }
        result += " world" + FormatMatrixAt(camera, 0x94) + " P" + FormatMatrixAt(camera, 0x154) +
                  " VP" + FormatMatrixAt(camera, 0x194) + " holds_last_eye_write=" + (holdsEyeWrite ? "1" : "0");
    }
    return result + " last_offset_eye=" + std::to_string(g_lastCameraOffsetEye.load());
}

void CaptureConstantBuffer(ID3D11Resource* resource, const D3D11_BOX* box, const void* source, const char* uploadPath, bool mayModifyNativeProjection = false) {
    if (!resource || box || !source) return;
    ID3D11Buffer* buffer = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || !buffer) return;
    D3D11_BUFFER_DESC description{};
    buffer->GetDesc(&description);
    buffer->Release();
    if (description.ByteWidth < sizeof(float) * 16 || description.ByteWidth > 4096) return;
    ++g_bufferUpdates;
    const auto* bytes = static_cast<const std::byte*>(source);
    // This is a single, bounded inspection of the same 784-byte dynamic
    // producer that owns the proven final projection.  It replaces a broad
    // matrix search and gives the next native translation experiment its
    // concrete candidate fields.
    if (mayModifyNativeProjection && description.ByteWidth == 784 &&
        !g_nativeCameraProducerBlockTraced.exchange(true)) {
        TraceRuntime("native camera producer block (784 bytes): " + SnapshotFloats(source, description.ByteWidth));
    }
    if (mayModifyNativeProjection && description.ByteWidth == 784) {
        const auto uploads = ++g_all784Uploads;
        if (g_coherenceProbeEnabled.load() && uploads % 1500 == 0 && g_coherenceUploadReports.fetch_add(1) < 10)
            TraceRuntime("coherence upload sample: all_784_uploads=" + std::to_string(uploads) +
                " canonical_uploads=" + std::to_string(g_verifiedFinalProjectionUploads.load()) +
                " eye_label=" + std::to_string(g_nativeEye.load()) + " " + DescribeProjectionCameraRecords() +
                " block=" + SnapshotFloats(source, description.ByteWidth));
        // Globals c29-c32 rows 0-2 are the camera-relative view-projection
        // (rotation x P) JC3 uploaded for this pass. Match them against the
        // exact expectation recorded for each eye's camera write; shadow and
        // HUD passes match neither and are ignored.
        const auto* vp = reinterpret_cast<const float*>(source) + 116;
        int matchedEye = -1;
        ExpectedEyeUpload matched{};
        {
            std::lock_guard expectedLock(g_expectedEyeMutex);
            for (int candidateEye = 0; candidateEye != 2 && matchedEye < 0; ++candidateEye) {
                const auto& expected = g_expectedEyeUploads[candidateEye];
                if (!expected.valid) continue;
                bool same = true;
                for (int index = 0; index != 12 && same; ++index)
                    same = std::abs(vp[index] - expected.rows[index]) <= 1e-3f * std::max(1.0f, std::abs(expected.rows[index]));
                if (same) { matchedEye = candidateEye; matched = expected; }
            }
        }
        if (matchedEye >= 0) {
            g_latestProjectionM00.store(matched.m00);
            g_latestProjectionM11.store(matched.m11);
            g_latestProjectionM20.store(matched.m20);
            g_latestProjectionM21.store(matched.m21);
            ++g_globalsProjectionUploads;
            g_sceneProjectionThisFrame.store(true);
        }
    }
    for (UINT offset = 0; offset + sizeof(float) * 16 <= description.ByteWidth; offset += sizeof(float) * 4) {
        std::array<float, 16> candidate{};
        std::memcpy(candidate.data(), bytes + offset, sizeof(candidate));
        bool finite = true;
        for (const float value : candidate) finite = finite && std::isfinite(value);
        if (!finite) continue;
        const auto diagnostics = DiagnoseMatrix(candidate);
        ++g_matrixSamples;
        if (diagnostics.resemblesProjection) ++g_projectionCandidates;
        if (IsRigidTransform(diagnostics)) ++g_rigidTransformCandidates;
        if (description.ByteWidth == 784 && offset == 0 &&
            std::abs(candidate[11] + 1.0f) < 0.001f && std::abs(candidate[14] - 0.1f) < 0.001f) {
            if (mayModifyNativeProjection && !g_nativeFinalProjectionBlockTraced.exchange(true)) {
                TraceRuntime("native final projection producer block (784 bytes): " + SnapshotFloats(source, description.ByteWidth));
                TraceProjectionRendererState();
            }
            if (mayModifyNativeProjection) TraceProjectionProducerCallers();
            const bool sceneRecorded = g_sceneProjectionThisFrame.load();
            if (!sceneRecorded) {
                g_latestProjectionM00.store(candidate[0]);
                g_latestProjectionM11.store(candidate[5]);
                g_latestProjectionM20.store(candidate[8]);
                g_latestProjectionM21.store(candidate[9]);
            }
            ++g_verifiedFinalProjectionUploads;
            TryArmNativeFrameRendererProbe();
            g_projectionBuffer.store(buffer);
            if (mayModifyNativeProjection && g_nativeEyeProjectionEnabled.load() && g_nativeEyeProjectionValid.load()) {
                auto* writable = static_cast<float*>(const_cast<void*>(source));
                writable[0] = g_nativeProjectionM00.load();
                writable[5] = g_nativeProjectionM11.load();
                writable[8] = g_nativeProjectionM20.load();
                writable[9] = g_nativeProjectionM21.load();
                // For row-vector D3D projection, composing a camera-local X
                // translation before this projection changes m30 by -m00*x.
                // Its post-divide effect is depth dependent: real parallel
                // stereo parallax, not a screen-space optical-centre shift.
                writable[12] -= writable[0] * g_nativeCameraLocalX.load();
                if (!sceneRecorded) {
                    g_latestProjectionM00.store(writable[0]);
                    g_latestProjectionM11.store(writable[5]);
                    g_latestProjectionM20.store(writable[8]);
                    g_latestProjectionM21.store(writable[9]);
                }
                const auto writes = ++g_nativeProjectionWrites;
                if (writes == 1 || writes % 1200 == 0) {
                    Log("native eye projection applied: eye=" + std::to_string(g_nativeEye.load()) +
                        " m00=" + std::to_string(writable[0]) + " m11=" + std::to_string(writable[5]) +
                        " m20=" + std::to_string(writable[8]) + " m21=" + std::to_string(writable[9]) +
                        " local_x=" + std::to_string(g_nativeCameraLocalX.load()) + " m30=" + std::to_string(writable[12]));
                    TraceRuntime("native eye projection write eye=" + std::to_string(g_nativeEye.load()) +
                        " m00=" + std::to_string(writable[0]) + " m11=" + std::to_string(writable[5]) +
                        " m20=" + std::to_string(writable[8]) + " m21=" + std::to_string(writable[9]) +
                        " local_x=" + std::to_string(g_nativeCameraLocalX.load()) + " m30=" + std::to_string(writable[12]));
                }
            }
            if (mayModifyNativeProjection) {
                std::lock_guard lock(g_projectionSnapshotMutex);
                if (g_projectionBufferOwned != buffer) {
                    buffer->AddRef();
                    if (g_projectionBufferOwned) g_projectionBufferOwned->Release();
                    g_projectionBufferOwned = buffer;
                }
                std::memcpy(g_projectionSnapshot.data(), source, g_projectionSnapshot.size());
                g_projectionSnapshotValid = true;
            }
            if (g_projectionBlockReports.fetch_add(1) == 0) {
                Log("camera projection block startup snapshot (784 bytes): " + SnapshotFloats(source, description.ByteWidth));
            }
            if (g_projectionVsBindings[0].load() > 0 && g_sceneProjectionBlockReports.fetch_add(1) == 0) {
                Log("camera projection block scene snapshot (784 bytes): " + SnapshotFloats(source, description.ByteWidth));
            }
        }
        if (IsCanonicalProjection(candidate, diagnostics) && g_projectionReports.fetch_add(1) < 24) {
            Log(std::string("camera candidate: upload=") + uploadPath +
                " bufferBytes=" + std::to_string(description.ByteWidth) +
                " offset=" + std::to_string(offset) +
                " m00=" + std::to_string(candidate[0]) +
                " m11=" + std::to_string(candidate[5]) +
                " m20=" + std::to_string(candidate[8]) +
                " m21=" + std::to_string(candidate[9]) +
                " m22=" + std::to_string(candidate[10]) +
                " m23=" + std::to_string(candidate[11]) +
                " m32=" + std::to_string(candidate[14]));
        }
    }
}

void STDMETHODCALLTYPE UpdateSubresourceDetour(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT destinationSubresource, const D3D11_BOX* destinationBox, const void* sourceData, UINT sourceRowPitch, UINT sourceDepthPitch) {
    CaptureConstantBuffer(resource, destinationBox, sourceData, "UpdateSubresource");
    if (!destinationBox) VrBodyObserveUpload(resource, sourceData, 0, "UpdateSubresource");
    // UpdateSubresource sources are const engine memory, unlike write maps.
    // Apply the same owned-body fix to a private copy, never to the caller's
    // buffer. Previously this upload route bypassed both body/carry fix-ups.
    std::array<float, 1024> bodyCopy;
    if (!destinationBox && sourceData && resource && VrBodyActive()) {
        D3D11_RESOURCE_DIMENSION dimension{};
        resource->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC desc{};
            static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
            if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && (desc.ByteWidth == 4096 || desc.ByteWidth == 528 || desc.ByteWidth == 208 || desc.ByteWidth == 128)) {
                std::memcpy(bodyCopy.data(), sourceData, desc.ByteWidth);
                VrBodyCarryUpload(resource, bodyCopy.data());
                VrBodyRewriteUpload(resource, bodyCopy.data());
                sourceData = bodyCopy.data();
            }
        }
    }
    g_originalUpdateSubresource(context, resource, destinationSubresource, destinationBox, sourceData, sourceRowPitch, sourceDepthPitch);
}

bool IsWriteMap(D3D11_MAP type) {
    return type == D3D11_MAP_WRITE || type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE;
}

// Camera refresh timeline probe (read-only, bounded). Uses JC3VR's own
// per-eye camera writes as markers: "J" = the record still holds our last
// write, "E" = the engine rewrote it. Starts once the Globals block is
// mapped many times per frame (in-world; the menu maps it about once).
std::atomic<bool> g_timelineEnabled = false;
std::atomic<int> g_timelineFramesLeft = 0;
std::atomic<int> g_timelineRuns = 0;
std::atomic<std::uint32_t> g_globalsMapsThisFrame = 0;
std::atomic<std::uint32_t> g_busyFrames = 0;

std::string CompactCameraState() {
    constexpr std::uintptr_t kOwnerGlobalRva = 0x02ED0E20;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* owner = nullptr;
    if (!imageBase || !IsReadableRange(reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner))) return "?";
    std::memcpy(&owner, reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner));
    if (!owner || !IsReadableRange(static_cast<const std::byte*>(owner) + 0x5C0, sizeof(void*) * 2)) return "?";
    std::string result;
    for (const std::size_t slot : {std::size_t{0x5C0}, std::size_t{0x5C8}}) {
        const std::byte* camera = nullptr;
        std::memcpy(&camera, static_cast<const std::byte*>(owner) + slot, sizeof(camera));
        result += slot == 0x5C0 ? "c0=" : " c8=";
        if (!camera || !IsReadableRange(camera + 0x94, 64)) { result += "-"; continue; }
        const auto* world = reinterpret_cast<const float*>(camera + 0x94);
        char state = 'E';
        {
            std::lock_guard lock(g_nativeCameraOffsetMutex);
            const auto it = g_lastNativeTranslations.find(reinterpret_cast<std::uintptr_t>(world));
            if (it != g_lastNativeTranslations.end() && it->second.valid &&
                std::abs(world[12] - it->second.written[0]) < 0.0005f &&
                std::abs(world[13] - it->second.written[1]) < 0.0005f &&
                std::abs(world[14] - it->second.written[2]) < 0.0005f) state = 'J';
        }
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%c[%.3f,%.3f,%.3f|b%.3f,%.3f]", state, world[12], world[13], world[14], world[8], world[10]);
        result += buffer;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE MapDetour(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    if (resource && IsWriteMap(mapType) && resource == static_cast<ID3D11Resource*>(g_projectionBuffer.load())) {
        const auto index = ++g_globalsMapsThisFrame;
        if (g_timelineFramesLeft.load() > 0 && (index <= 6 || index % 16 == 0))
            TraceRuntime("timeline map#" + std::to_string(index) + " tid=" + std::to_string(GetCurrentThreadId()) + " " + CompactCameraState());
    }
    if (g_pairedUploadSyncEnabled.load() && resource && IsWriteMap(mapType) &&
        resource == static_cast<ID3D11Resource*>(g_projectionBuffer.load()))
        ReapplyPairedCameraAtUpload();
    // Rico's arms/head (VR body): before any buffer is filled for a draw, make
    // sure the skinning palette still holds JC3VR's result (the game's
    // animation thread can rewrite it mid-frame; live: one eye showed the
    // original pose). Cheap when intact.
    // Only 4 KB buffers carry Rico's per-draw bone subsets (live upload diag);
    // the Globals upload keeps the once-per-frame solve.
    if (resource && IsWriteMap(mapType)) {
        bool check = resource == static_cast<ID3D11Resource*>(g_projectionBuffer.load());
        if (!check && VrBodyActive()) {
            D3D11_RESOURCE_DIMENSION dimension{};
            resource->GetType(&dimension);
            if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC desc{};
                static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
                // 528 B: Rico's hair/eye draws upload their head-bone subset
                // there (live: floating hair and eyes in one eye while the
                // body was hidden, 2026-09-30).
                check = (desc.ByteWidth == 4096 || desc.ByteWidth == 528) && (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER);
            }
        }
        if (check) VrBodyApplyAtUpload();
    }
    const HRESULT result = g_originalMap(context, resource, subresource, mapType, mapFlags, mapped);
    if (SUCCEEDED(result) && resource && mapped && mapped->pData && IsWriteMap(mapType)) {
        g_mappedWrites.Publish({context,resource,subresource},{mapped->pData,mapType});
    }
    return result;
}

void STDMETHODCALLTYPE UnmapDetour(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource) {
    MappedWrite mapped{};
    const bool found = g_mappedWrites.Consume({context,resource,subresource},mapped);
    if (found) {
        ++g_dynamicMapWrites;
        CaptureConstantBuffer(resource, nullptr, mapped.bytes, "Map/Unmap", true);
        VrBodyObserveUpload(resource, mapped.bytes, 0, "Map/Unmap");
        // Held objects (the gun) follow Rico's IK hands.
        VrBodyCarryUpload(resource, mapped.bytes);
        VrBodyRewriteUpload(resource, mapped.bytes);
    }
    g_originalUnmap(context, resource, subresource);
}

uint32_t Crc32(const void* data, std::size_t bytes) {
    uint32_t crc = 0xffffffffu;
    const auto* input = static_cast<const uint8_t*>(data);
    for (std::size_t index = 0; index < bytes; ++index) {
        crc ^= input[index];
        for (uint32_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

std::uint64_t MigotoShaderHash(const void* data, std::size_t bytes) {
    // 3DMigoto's traditional shader filenames use unseeded 64-bit FNV-1.
    constexpr std::uint64_t kPrime = 0x100000001b3ull;
    std::uint64_t hash = 0;
    const auto* input = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < bytes; ++index) {
        hash *= kPrime;
        hash ^= input[index];
    }
    return hash;
}

int TerrainShaderIndex(uint32_t hash) {
    switch (hash) {
    case 0x87E0C350u: return 0; // close terrain
    case 0xE2313E10u: return 1; // mid terrain
    case 0x95A485D4u: return 2; // far terrain
    default: return -1;
    }
}

const char* TerrainShaderName(int index) {
    static constexpr const char* kNames[] = {"close", "mid", "far", "tessellated-domain"};
    return index >= 0 && index < static_cast<int>(std::size(kNames)) ? kNames[index] : "unknown";
}

bool IsTessellatedTerrainDomainShader(ID3D11DomainShader* shader) {
    if (!shader) return false;
    std::lock_guard lock(g_terrainShaderNamesMutex);
    const auto known = g_knownTerrainDomainShaderNames.find(shader);
    if (known != g_knownTerrainDomainShaderNames.end()) return known->second;
    char name[256]{};
    UINT bytes = sizeof(name) - 1;
    const HRESULT result = shader->GetPrivateData(WKPDID_D3DDebugObjectName, &bytes, name);
    bool isTerrain = false;
    if (SUCCEEDED(result) && bytes) {
        name[std::min<UINT>(bytes, sizeof(name) - 1)] = '\0';
        isTerrain = std::strstr(name, "7a1ee521893b9809") != nullptr ||
                    std::strstr(name, "7A1EE521893B9809") != nullptr;
    }
    g_knownTerrainDomainShaderNames.emplace(shader, isTerrain);
    if (isTerrain) {
        g_tessellatedTerrainDomainShader.store(shader);
        TraceRuntime("terrain shader landmark resolved from runtime domain-shader name: tessellated-domain name=" + std::string(name));
    }
    return isTerrain;
}

int TerrainShaderIndexFromDebugName(ID3D11PixelShader* shader) {
    if (!shader) return -1;
    std::lock_guard lock(g_terrainShaderNamesMutex);
    const auto known = g_knownTerrainShaderNames.find(shader);
    if (known != g_knownTerrainShaderNames.end()) return known->second;
    char name[256]{};
    UINT bytes = sizeof(name) - 1;
    const HRESULT result = shader->GetPrivateData(WKPDID_D3DDebugObjectName, &bytes, name);
    int terrainIndex = -1;
    if (SUCCEEDED(result) && bytes) {
        name[std::min<UINT>(bytes, sizeof(name) - 1)] = '\0';
        if (std::strstr(name, "87E0C350")) terrainIndex = 0;
        else if (std::strstr(name, "E2313E10")) terrainIndex = 1;
        else if (std::strstr(name, "95A485D4")) terrainIndex = 2;
    }
    g_knownTerrainShaderNames.emplace(shader, terrainIndex);
    if (terrainIndex >= 0) {
        g_terrainShaders[terrainIndex].store(shader);
        TraceRuntime("terrain shader landmark resolved from runtime name: " + std::string(TerrainShaderName(terrainIndex)) +
                     " name=" + std::string(name));
    }
    return terrainIndex;
}

std::string DescribeBoundTexture(ID3D11Resource* resource) {
    if (!resource) return "none";
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) || !texture) return "non-Texture2D";
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    texture->Release();
    return std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
           " fmt=" + std::to_string(static_cast<uint32_t>(desc.Format)) +
           " samples=" + std::to_string(desc.SampleDesc.Count);
}

void TraceTerrainDrawState(ID3D11DeviceContext* context, int terrainIndex, const char* drawKind, UINT verticesOrIndices) {
    if (!context) return;
    const auto drawNumber = g_terrainDraws[terrainIndex].fetch_add(1) + 1;
    const auto reportNumber = g_terrainReports[terrainIndex].fetch_add(1);
    if (reportNumber >= 6 && drawNumber % 1200 != 0) return;
    constexpr UINT kCapacity = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT, kCapacity> viewports{};
    std::array<D3D11_RECT, kCapacity> scissors{};
    UINT viewportCount = kCapacity;
    UINT scissorCount = kCapacity;
    context->RSGetViewports(&viewportCount, viewports.data());
    context->RSGetScissorRects(&scissorCount, scissors.data());
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Resource* rtvResource = nullptr;
    ID3D11Resource* dsvResource = nullptr;
    if (rtv) rtv->GetResource(&rtvResource);
    if (dsv) dsv->GetResource(&dsvResource);
    std::ostringstream stream;
    stream << "terrain draw " << TerrainShaderName(terrainIndex) << " #" << drawNumber
           << " kind=" << drawKind << " count=" << verticesOrIndices
           << " rtv=" << DescribeBoundTexture(rtvResource)
           << " dsv=" << DescribeBoundTexture(dsvResource)
           << " viewport_count=" << viewportCount;
    for (UINT index = 0; index < viewportCount; ++index) {
        const auto& viewport = viewports[index];
        stream << " vp" << index << "=[" << viewport.TopLeftX << "," << viewport.TopLeftY << " "
               << viewport.Width << "x" << viewport.Height << " z=" << viewport.MinDepth << ".." << viewport.MaxDepth << "]";
    }
    stream << " scissor_count=" << scissorCount;
    for (UINT index = 0; index < scissorCount; ++index) {
        const auto& scissor = scissors[index];
        stream << " sc" << index << "=[" << scissor.left << "," << scissor.top << ".." << scissor.right << "," << scissor.bottom << ")";
    }
    TraceRuntime(stream.str());
    if (rtvResource) rtvResource->Release();
    if (dsvResource) dsvResource->Release();
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
}

HRESULT STDMETHODCALLTYPE CreatePixelShaderDetour(ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader) {
    const HRESULT result = g_originalCreatePixelShader(device, bytecode, bytecodeLength, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && bytecode && bytecodeLength) {
        const uint32_t hash = Crc32(bytecode, bytecodeLength);
        const int terrainIndex = TerrainShaderIndex(hash);
        if (terrainIndex >= 0) {
            g_terrainShaders[terrainIndex].store(*shader);
            TraceRuntime("terrain shader landmark created: " + std::string(TerrainShaderName(terrainIndex)) +
                         " crc32=0x" + Hex(hash) + " bytes=" + std::to_string(bytecodeLength));
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CreateDomainShaderDetour(ID3D11Device* device, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11DomainShader** shader) {
    constexpr std::uint64_t kTessellatedTerrainHash = 0x7a1ee521893b9809ull;
    const std::uint64_t hash = bytecode && bytecodeLength ? MigotoShaderHash(bytecode, bytecodeLength) : 0;
    const HRESULT result = g_originalCreateDomainShader(device, bytecode, bytecodeLength, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && hash == kTessellatedTerrainHash) {
        g_tessellatedTerrainDomainShader.store(*shader);
        TraceRuntime("terrain shader landmark created: tessellated-domain 3dmigoto_hash=0x" +
                     Hex(static_cast<std::uintptr_t>(hash)) + " bytes=" + std::to_string(bytecodeLength));
    }
    return result;
}

void STDMETHODCALLTYPE PSSetShaderDetour(ID3D11DeviceContext* context, ID3D11PixelShader* shader, ID3D11ClassInstance* const* classes, UINT classCount) {
    TerrainShaderIndexFromDebugName(shader);
    g_activePixelShader.store(shader);
    g_originalPSSetShader(context, shader, classes, classCount);
}

void STDMETHODCALLTYPE DSSetShaderDetour(ID3D11DeviceContext* context, ID3D11DomainShader* shader, ID3D11ClassInstance* const* classes, UINT classCount) {
    IsTessellatedTerrainDomainShader(shader);
    g_activeDomainShader.store(shader);
    g_originalDSSetShader(context, shader, classes, classCount);
}

int ActiveTerrainShaderIndex() {
    const auto active = g_activePixelShader.load();
    for (int index = 0; index < 3; ++index) {
        if (active && active == g_terrainShaders[index].load()) return index;
    }
    const auto activeDomain = g_activeDomainShader.load();
    if (activeDomain && activeDomain == g_tessellatedTerrainDomainShader.load()) return 3;
    return -1;
}

// Armed from Present at a bounded cadence. Samples the first immediate-context
// scene depth draw (non-square viewport, so not the 2048x2048 shadow pass)
// whose VS slot 0 is a 784-byte block, and reads back what the GPU will use.
void SampleStereoCoherenceDraw(ID3D11DeviceContext* context, const char* drawKind) {
    if (!g_coherenceDrawArmed.load() || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    ID3D11Buffer* slot0 = nullptr;
    context->VSGetConstantBuffers(0, 1, &slot0);
    if (!slot0) return;
    D3D11_BUFFER_DESC slotDesc{};
    slot0->GetDesc(&slotDesc);
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    UINT viewportCount = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&viewportCount, &viewport);
    const bool qualifies = slotDesc.ByteWidth == 784 && dsv && viewportCount &&
        viewport.Width >= 640.0f && viewport.Width != viewport.Height;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    if (dsv) dsv->GetDesc(&dsvDesc);
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    if (!qualifies || !g_coherenceDrawArmed.exchange(false)) { slot0->Release(); return; }
    std::array<float, 196> gpu{};
    bool readBack = false;
    if (!g_coherenceStaging) {
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth = 784;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (device) {
            device->CreateBuffer(&stagingDesc, nullptr, &g_coherenceStaging);
            device->Release();
        }
    }
    if (g_coherenceStaging && g_originalMap && g_originalUnmap) {
        context->CopyResource(g_coherenceStaging, slot0);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_originalMap(context, g_coherenceStaging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
            std::memcpy(gpu.data(), mapped.pData, sizeof(gpu));
            g_originalUnmap(context, g_coherenceStaging, 0);
            readBack = true;
        }
    }
    bool ownedBuffer = false;
    {
        std::lock_guard lock(g_projectionSnapshotMutex);
        ownedBuffer = slot0 == g_projectionBufferOwned;
    }
    slot0->Release();
    const auto report = g_coherenceDrawReports.fetch_add(1) + 1;
    if (report >= 24) g_coherenceProbeEnabled.store(false);
    TraceRuntime("coherence draw sample " + std::to_string(report) + ": kind=" + drawKind +
        " eye_label=" + std::to_string(g_nativeEye.load()) +
        " requested=[" + std::to_string(g_nativeProjectionM00.load()) + "," + std::to_string(g_nativeProjectionM11.load()) + "," +
        std::to_string(g_nativeProjectionM20.load()) + "," + std::to_string(g_nativeProjectionM21.load()) + "]" +
        " latest_recorded=[" + std::to_string(g_latestProjectionM00.load()) + "," + std::to_string(g_latestProjectionM11.load()) + "," +
        std::to_string(g_latestProjectionM20.load()) + "," + std::to_string(g_latestProjectionM21.load()) + "]" +
        " slot0_is_forced_buffer=" + (ownedBuffer ? "1" : "0") +
        " all_784_uploads=" + std::to_string(g_all784Uploads.load()) +
        " canonical_uploads=" + std::to_string(g_verifiedFinalProjectionUploads.load()) +
        " globals_projection_uploads=" + std::to_string(g_globalsProjectionUploads.load()) +
        " forced_writes=" + std::to_string(g_forcedProjectionWrites.load()) +
        " camera_p_syncs=" + std::to_string(g_nativeCameraProjectionSyncWrites.load()) +
        " viewport=" + std::to_string(viewport.Width) + "x" + std::to_string(viewport.Height) +
        " dsv_format=" + std::to_string(static_cast<int>(dsvDesc.Format)) + " " + DescribeProjectionCameraRecords() +
        " gpu_block=" + (readBack ? SnapshotFloats(gpu.data(), 784) : std::string("readback-failed")));
}

// ---- HUD render-target probe (one frame at a time, read-only) -------------
void HudProbeDescribe(void* resource, UINT& width, UINT& height, int* format) {
    ID3D11Texture2D* texture = nullptr;
    if (resource && SUCCEEDED(static_cast<ID3D11Resource*>(resource)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        width = desc.Width;
        height = desc.Height;
        if (format) *format = static_cast<int>(desc.Format);
        texture->Release();
    }
}

// Caller holds g_hudProbeMutex.
std::size_t HudProbeNewSegment(ID3D11DeviceContext* context, void* resource) {
    HudProbeSegment segment{};
    segment.context = context;
    segment.thread = GetCurrentThreadId();
    segment.resource = resource;
    segment.backbuffer = resource && resource == g_hudProbeBackbuffer;
    HudProbeDescribe(resource, segment.width, segment.height, &segment.format);
    g_hudProbeSegments.push_back(segment);
    return g_hudProbeCurrent[context] = g_hudProbeSegments.size() - 1;
}

void HudProbeTargets(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views) {
    if (!g_hudProbeActive.load() || count == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
    ID3D11Resource* resource = nullptr;
    if (count > 0 && views && views[0]) views[0]->GetResource(&resource);
    if (resource) resource->Release();  // pointer identity only
    std::lock_guard lock(g_hudProbeMutex);
    if (g_hudProbeSegments.size() >= 6000) return;
    HudProbeNewSegment(context, resource);
}

void HudProbeDraw(ID3D11DeviceContext* context) {
    if (!g_hudProbeActive.load() || g_internalRenderPass) return;
    std::lock_guard lock(g_hudProbeMutex);
    auto current = g_hudProbeCurrent.find(context);
    std::size_t index = 0;
    if (current == g_hudProbeCurrent.end()) {
        if (g_hudProbeSegments.size() >= 6000) return;
        index = HudProbeNewSegment(context, nullptr);
    } else index = current->second;
    auto& segment = g_hudProbeSegments[index];
    ++segment.draws;
    if (g_hudProbeBlendByContext[context]) ++segment.blended;
    if (segment.backbuffer && !segment.psSource) {
        ID3D11ShaderResourceView* view = nullptr;
        context->PSGetShaderResources(0, 1, &view);
        if (view) {
            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            if (resource) {
                segment.psSource = resource;
                HudProbeDescribe(resource, segment.psSourceWidth, segment.psSourceHeight, nullptr);
                resource->Release();
            }
            view->Release();
        }
    }
}

void STDMETHODCALLTYPE ExecuteCommandListDetour(ID3D11DeviceContext* context, ID3D11CommandList* list, BOOL restore) {
    if (g_preHudEnabled && context == g_immediateContext && g_preHudBackbuffer && !g_preHudValid) {
        ID3D11RenderTargetView* view = nullptr;
        context->OMGetRenderTargets(1, &view, nullptr);
        if (view) {
            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            if (resource && resource == g_preHudBackbuffer) {
                auto* backbuffer = static_cast<ID3D11Texture2D*>(g_preHudBackbuffer);
                D3D11_TEXTURE2D_DESC desc{};
                backbuffer->GetDesc(&desc);
                D3D11_TEXTURE2D_DESC existing{};
                if (g_preHudCopy) g_preHudCopy->GetDesc(&existing);
                if (!g_preHudCopy || existing.Width != desc.Width || existing.Height != desc.Height || existing.Format != desc.Format) {
                    g_preHudCopy.Reset();
                    D3D11_TEXTURE2D_DESC copyDesc = desc;
                    copyDesc.Usage = D3D11_USAGE_DEFAULT;
                    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    copyDesc.CPUAccessFlags = 0;
                    copyDesc.MiscFlags = 0;
                    ID3D11Device* device = nullptr;
                    context->GetDevice(&device);
                    if (device) {
                        device->CreateTexture2D(&copyDesc, nullptr, &g_preHudCopy);
                        device->Release();
                    }
                }
                if (g_preHudCopy) {
                    context->CopyResource(g_preHudCopy.Get(), backbuffer);
                    g_preHudValid = true;
                    if (++g_preHudCaptures == 1) TraceRuntime("HUD: captured the scene just before JC3's HUD command list");
                }
            }
            if (resource) resource->Release();
            view->Release();
        }
    }
    if (g_hudProbeActive.load()) {
        std::lock_guard lock(g_hudProbeMutex);
        if (g_hudProbeSegments.size() < 6000) {
            HudProbeSegment marker{};
            marker.context = context;
            marker.thread = GetCurrentThreadId();
            marker.execute = true;
            auto current = g_hudProbeCurrent.find(context);
            if (current != g_hudProbeCurrent.end()) marker.resource = g_hudProbeSegments[current->second].resource;
            g_hudProbeSegments.push_back(marker);
            // Without state restore the context's bindings are cleared.
            if (!restore) g_hudProbeCurrent.erase(context);
        }
    }
    g_originalExecuteCommandList(context, list, restore);
}

void STDMETHODCALLTYPE OMSetRenderTargetsDetour(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) {
    HudProbeTargets(context, count, views);
    g_originalOMSetRenderTargets(context, count, views, depth);
}

void STDMETHODCALLTYPE OMSetRenderTargetsAndUavsDetour(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth,
                                                      UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts) {
    HudProbeTargets(context, count, views);
    g_originalOMSetRenderTargetsAndUavs(context, count, views, depth, uavStart, uavCount, uavs, initialCounts);
}

void STDMETHODCALLTYPE OMSetBlendStateDetour(ID3D11DeviceContext* context, ID3D11BlendState* state, const FLOAT factor[4], UINT mask) {
    if (g_hudProbeActive.load()) {
        D3D11_BLEND_DESC desc{};
        if (state) state->GetDesc(&desc);
        std::lock_guard lock(g_hudProbeMutex);
        g_hudProbeBlendByContext[context] = state && desc.RenderTarget[0].BlendEnable != FALSE;
    }
    g_originalOMSetBlendState(context, state, factor, mask);
}

void TraceScopeSourceDraw(ID3D11DeviceContext* context) {
    if(!ScopeSourceProbeActive() || g_internalRenderPass) return;
    static std::atomic<unsigned> attempts=0,reports=0;
    if(++attempts>12000) {EndScopeSourceProbe();TraceRuntime("scope source probe ended: bounded draw budget reached; no renderer reentry/draw suppression");return;}
    ID3D11Buffer* buffers[6]{};context->VSGetConstantBuffers(0,6,buffers);
    bool sniper=false;
    for(auto* buffer:buffers) if(buffer) {sniper=IsScopeSourceBuffer(buffer)||sniper;buffer->Release();}
    if(!sniper) return;
    ID3D11RenderTargetView* targets[4]{};ID3D11DepthStencilView* depth=nullptr;
    context->OMGetRenderTargets(4,targets,&depth);
    std::string detail;
    for(int slot=0;slot<4;++slot) if(targets[slot]) {
        ID3D11Resource* resource=nullptr;targets[slot]->GetResource(&resource);
        if(resource) {
            D3D11_RESOURCE_DIMENSION kind;resource->GetType(&kind);
            if(kind==D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC desc{};static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
                detail+=" RT"+std::to_string(slot)+"="+std::to_string(desc.Width)+"x"+std::to_string(desc.Height)+
                    " format="+std::to_string(desc.Format)+" final_backbuffer="+(resource==g_preHudBackbuffer?"yes":"no");
            }
            resource->Release();
        }
        targets[slot]->Release();
    }
    if(depth) depth->Release();
    if(detail.empty()) return; // shadow-only passes do not consume scene-source reports
    UINT count=1;D3D11_VIEWPORT viewport{};context->RSGetViewports(&count,&viewport);
    TraceRuntime("scope source: exact local sniper matrix CB draw context="+std::string(context==g_immediateContext?"immediate":"deferred")+
        detail+" viewport="+std::to_string(viewport.Width)+"x"+std::to_string(viewport.Height));
    if(++reports>=8) EndScopeSourceProbe();
}
void STDMETHODCALLTYPE DrawInstancedDetour(ID3D11DeviceContext* context, UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance) {
    TraceScopeSourceDraw(context);
    HudProbeDraw(context);
    g_originalDrawInstanced(context, vertexCount, instanceCount, startVertex, startInstance);
}

void STDMETHODCALLTYPE DrawIndexedDetour(ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex) {
    TraceScopeSourceDraw(context);
    HudProbeDraw(context);
    if (g_internalRenderPass) { g_originalDrawIndexed(context, indexCount, startIndex, baseVertex); return; }
    ++g_observedDraws;
    if (g_coherenceProbeEnabled.load()) SampleStereoCoherenceDraw(context, "indexed");
    const int terrainIndex = ActiveTerrainShaderIndex();
    if (terrainIndex >= 0) TraceTerrainDrawState(context, terrainIndex, "indexed", indexCount);
    // Sample a tiny fraction of depth-only draws after the normal gameplay
    // camera appears. This is a read-only discriminator for independent shadow
    // cameras and has a hard cap so it cannot flood the render thread.
    if (g_shadowProbeEnabled.load() && g_shadowProbeReports.load() < 12 &&
        (g_shadowProbeDraws.fetch_add(1) % 128) == 0 && HasNativeGameplayCamera()) {
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        context->OMGetRenderTargets(1, &rtv, &dsv);
        if (!rtv && dsv) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsv->GetDesc(&dsvDesc);
            UINT viewportCount = 1;
            D3D11_VIEWPORT viewport{};
            context->RSGetViewports(&viewportCount, &viewport);
            const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            void* owner = nullptr;
            void* camera = nullptr;
            if (imageBase && IsReadableRange(reinterpret_cast<const void*>(imageBase + 0x02ED0E20), sizeof(owner)))
                std::memcpy(&owner, reinterpret_cast<const void*>(imageBase + 0x02ED0E20), sizeof(owner));
            if (owner && IsReadableRange(static_cast<const std::byte*>(owner) + 0x5C8, sizeof(camera)))
                std::memcpy(&camera, static_cast<const std::byte*>(owner) + 0x5C8, sizeof(camera));
            if (HasNativeCameraSignature(static_cast<const std::byte*>(camera)) && g_shadowProbeReports.fetch_add(1) < 12) {
                const auto* world = reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + 0x94);
                const auto* viewProjection = reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + 0x194);
                std::array<ID3D11Buffer*, 4> vertexBuffers{};
                context->VSGetConstantBuffers(0, static_cast<UINT>(vertexBuffers.size()), vertexBuffers.data());
                std::ostringstream bufferDescription;
                for (std::size_t slot = 0; slot != vertexBuffers.size(); ++slot) {
                    if (slot) bufferDescription << ',';
                    bufferDescription << slot << ':';
                    if (vertexBuffers[slot]) {
                        D3D11_BUFFER_DESC desc{};
                        vertexBuffers[slot]->GetDesc(&desc);
                        bufferDescription << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(vertexBuffers[slot])
                                          << std::dec << '/' << desc.ByteWidth;
                        vertexBuffers[slot]->Release();
                    } else bufferDescription << "none";
                }
                TraceRuntime("shadow depth-only draw sample: eye=" + std::to_string(g_nativeEye.load()) +
                    " viewport=" + std::to_string(viewport.Width) + "x" + std::to_string(viewport.Height) +
                    " dsv_format=" + std::to_string(static_cast<int>(dsvDesc.Format)) +
                    " vs_cb=[" + bufferDescription.str() + "]" +
                    " camera_world=[" + std::to_string(world[12]) + "," + std::to_string(world[13]) + "," + std::to_string(world[14]) +
                    "] camera_vp_row3=[" + std::to_string(viewProjection[12]) + "," + std::to_string(viewProjection[13]) + "," + std::to_string(viewProjection[14]) +
                    "," + std::to_string(viewProjection[15]) + "] camera_offset_writes=" + std::to_string(g_nativeCameraOffsetWrites.load()));
            }
        }
        if (rtv) rtv->Release();
        if (dsv) dsv->Release();
    }
    g_originalDrawIndexed(context, indexCount, startIndex, baseVertex);
}

void STDMETHODCALLTYPE DrawDetour(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) {
    TraceScopeSourceDraw(context);
    if (g_internalRenderPass) { g_originalDraw(context, vertexCount, startVertex); return; }
    ++g_observedDraws;
    const int terrainIndex = ActiveTerrainShaderIndex();
    if (terrainIndex >= 0) TraceTerrainDrawState(context, terrainIndex, "non-indexed", vertexCount);
    HudProbeDraw(context);
    g_originalDraw(context, vertexCount, startVertex);
}

void STDMETHODCALLTYPE DrawIndexedInstancedDetour(ID3D11DeviceContext* context, UINT indexCount, UINT instanceCount,
                                                   UINT startIndex, INT baseVertex, UINT startInstance) {
    TraceScopeSourceDraw(context);
    HudProbeDraw(context);
    g_originalDrawIndexedInstanced(context, indexCount, instanceCount, startIndex, baseVertex, startInstance);
}

void STDMETHODCALLTYPE VSSetConstantBuffersDetour(ID3D11DeviceContext* context, UINT startSlot, UINT bufferCount, ID3D11Buffer* const* buffers) {
    const auto* projectionBuffer = g_projectionBuffer.load();
    if (projectionBuffer && buffers) {
        for (UINT index = 0; index < bufferCount && startSlot + index < g_projectionVsBindings.size(); ++index) {
            if (buffers[index] == projectionBuffer) ++g_projectionVsBindings[startSlot + index];
        }
    }
    g_originalVSSetConstantBuffers(context, startSlot, bufferCount, buffers);
}

bool IsNativeWorldTransform(const float* matrix) {
    if (!matrix) return false;
    for (int index = 0; index != 16; ++index) if (!std::isfinite(matrix[index])) return false;
    const auto length = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    return std::abs(length(matrix) - 1.0f) < 0.08f &&
           std::abs(length(matrix + 4) - 1.0f) < 0.08f &&
           std::abs(length(matrix + 8) - 1.0f) < 0.08f &&
           std::abs(dot(matrix, matrix + 4)) < 0.08f &&
           std::abs(dot(matrix, matrix + 8)) < 0.08f &&
           std::abs(dot(matrix + 4, matrix + 8)) < 0.08f &&
           std::abs(matrix[3]) < 0.001f && std::abs(matrix[7]) < 0.001f &&
           std::abs(matrix[11]) < 0.001f && std::abs(matrix[15] - 1.0f) < 0.001f;
}

bool HasNativeCameraSignature(const std::byte* camera) {
    if (!camera || !IsReadableRange(camera, 0x214)) return false;
    const auto* projection = reinterpret_cast<const float*>(camera + 0x154);
    bool finite = true;
    for (int index = 0; index != 16; ++index) finite = finite && std::isfinite(projection[index]);
    return finite && std::abs(projection[0]) > 0.1f && std::abs(projection[0]) < 10.0f &&
           std::abs(projection[5]) > 0.1f && std::abs(projection[5]) < 10.0f &&
           std::abs(std::abs(projection[11]) - 1.0f) < 0.01f &&
           std::abs(std::abs(projection[14]) - 0.1f) < 0.01f &&
           IsNativeWorldTransform(reinterpret_cast<const float*>(camera + 0x94));
}

std::array<float, 3> NativeWorldOffset(const float* transform, const NativeCameraOffset& requested) {
    if(requested.baseAnchor) {
        float frame[9]{};
        for(int axis=0;axis<3;++axis) for(int c=0;c<3;++c) frame[axis*3+c]=transform[axis*4+c];
        std::array<float,3> result{};
        VehicleLocalOffset(frame,requested.right,requested.up,requested.forward,result.data());
        return result;
    }
    // Row 2 is the camera's back (+Z) axis: the live VP readback has clip
    // w = dot(p - eye, -row2), so view-forward is -row2.
    std::array<float, 3> forward{-transform[8], 0.0f, -transform[10]};
    std::array<float, 3> right{transform[0], 0.0f, transform[2]};
    const auto normalize = [](std::array<float, 3>& v) {
        const float length = std::sqrt(v[0] * v[0] + v[2] * v[2]);
        if (length > 0.0001f) { v[0] /= length; v[2] /= length; }
    };
    normalize(forward);
    normalize(right);
    // JC3's live camera basis already contains rotational head-look. Remove
    // the OpenXR yaw relative to recenter so a physical sideways lean remains
    // body-relative when the player turns their head.
    const float sine = std::sin(requested.headYawRadians);
    const float cosine = std::cos(requested.headYawRadians);
    const std::array<float, 3> bodyForward{
        cosine * forward[0] - sine * right[0], 0.0f,
        cosine * forward[2] - sine * right[2]};
    const std::array<float, 3> bodyRight{
        sine * forward[0] + cosine * right[0], 0.0f,
        sine * forward[2] + cosine * right[2]};
    return {
        bodyRight[0] * requested.right + bodyForward[0] * requested.forward,
        requested.up,
        bodyRight[2] * requested.right + bodyForward[2] * requested.forward,
    };
}
}

bool InstallCameraCapture(ID3D11DeviceContext* context) {
    if (g_installed) return true;
    if (!context) return false;
    auto** vtable = *reinterpret_cast<void***>(context);
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) return false;
    auto** deviceVtable = *reinterpret_cast<void***>(device);
    // ID3D11DeviceContext inherits four ID3D11DeviceChild entries after IUnknown;
    // slots 7/14/15/48 are VSSetConstantBuffers/Map/Unmap/UpdateSubresource in the local Windows SDK.
    void* updateTarget = vtable[48];
    void* mapTarget = vtable[14];
    void* unmapTarget = vtable[15];
    void* vsSetConstantBuffersTarget = vtable[7];
    void* psSetShaderTarget = vtable[9];
    // ID3D11DeviceContext::DSSetShader is slot 60 in the Windows SDK used to
    // build JC3VR. The local ShaderFixes landmark 7a1ee521893b9809 is a domain
    // shader, so pixel-stage-only observation cannot see tessellated terrain.
    void* dsSetShaderTarget = vtable[60];
    void* drawIndexedTarget = vtable[12];
    void* drawIndexedInstancedTarget = vtable[20];
    void* drawTarget = vtable[13];
    // Probe-only hooks (created, enabled one frame at a time): DrawInstanced 21,
    // OMSetRenderTargets 33, OMSetRenderTargetsAndUnorderedAccessViews 34,
    // OMSetBlendState 35, plus the existing DrawIndexedInstanced 20.
    g_hudProbeTargets = {vtable[21], vtable[33], vtable[34], vtable[35], vtable[20], vtable[58]};  // 58 = ExecuteCommandList
    g_immediateContext = context;
    void* createPixelShaderTarget = deviceVtable[15];
    void* createDomainShaderTarget = deviceVtable[17];
    device->Release();
    if (MH_CreateHook(updateTarget, &UpdateSubresourceDetour, reinterpret_cast<void**>(&g_originalUpdateSubresource)) != MH_OK ||
        MH_CreateHook(mapTarget, &MapDetour, reinterpret_cast<void**>(&g_originalMap)) != MH_OK ||
        MH_CreateHook(unmapTarget, &UnmapDetour, reinterpret_cast<void**>(&g_originalUnmap)) != MH_OK ||
        MH_CreateHook(vsSetConstantBuffersTarget, &VSSetConstantBuffersDetour, reinterpret_cast<void**>(&g_originalVSSetConstantBuffers)) != MH_OK ||
        MH_CreateHook(psSetShaderTarget, &PSSetShaderDetour, reinterpret_cast<void**>(&g_originalPSSetShader)) != MH_OK ||
        MH_CreateHook(dsSetShaderTarget, &DSSetShaderDetour, reinterpret_cast<void**>(&g_originalDSSetShader)) != MH_OK ||
        MH_CreateHook(drawIndexedTarget, &DrawIndexedDetour, reinterpret_cast<void**>(&g_originalDrawIndexed)) != MH_OK ||
        MH_CreateHook(drawIndexedInstancedTarget, &DrawIndexedInstancedDetour, reinterpret_cast<void**>(&g_originalDrawIndexedInstanced)) != MH_OK ||
        MH_CreateHook(drawTarget, &DrawDetour, reinterpret_cast<void**>(&g_originalDraw)) != MH_OK ||
        MH_CreateHook(createPixelShaderTarget, &CreatePixelShaderDetour, reinterpret_cast<void**>(&g_originalCreatePixelShader)) != MH_OK ||
        MH_CreateHook(createDomainShaderTarget, &CreateDomainShaderDetour, reinterpret_cast<void**>(&g_originalCreateDomainShader)) != MH_OK ||
        MH_EnableHook(updateTarget) != MH_OK ||
        MH_EnableHook(mapTarget) != MH_OK ||
        MH_EnableHook(unmapTarget) != MH_OK ||
        MH_EnableHook(vsSetConstantBuffersTarget) != MH_OK ||
        MH_EnableHook(psSetShaderTarget) != MH_OK ||
        MH_EnableHook(dsSetShaderTarget) != MH_OK ||
        MH_EnableHook(drawIndexedTarget) != MH_OK ||
        MH_EnableHook(drawTarget) != MH_OK ||
        MH_EnableHook(createPixelShaderTarget) != MH_OK) return false;
    if (MH_EnableHook(createDomainShaderTarget) != MH_OK) return false;
    if (MH_CreateHook(g_hudProbeTargets[0], &DrawInstancedDetour, reinterpret_cast<void**>(&g_originalDrawInstanced)) != MH_OK ||
        MH_CreateHook(g_hudProbeTargets[1], &OMSetRenderTargetsDetour, reinterpret_cast<void**>(&g_originalOMSetRenderTargets)) != MH_OK ||
        MH_CreateHook(g_hudProbeTargets[2], &OMSetRenderTargetsAndUavsDetour, reinterpret_cast<void**>(&g_originalOMSetRenderTargetsAndUavs)) != MH_OK ||
        MH_CreateHook(g_hudProbeTargets[3], &OMSetBlendStateDetour, reinterpret_cast<void**>(&g_originalOMSetBlendState)) != MH_OK ||
        MH_CreateHook(g_hudProbeTargets[5], &ExecuteCommandListDetour, reinterpret_cast<void**>(&g_originalExecuteCommandList)) != MH_OK) {
        g_hudProbeTargets = {};
        TraceRuntime("HUD render-target probe hooks unavailable");
    } else {
        void* executeTarget = g_hudProbeTargets[5];
        g_hudProbeTargets[5] = nullptr;
        if (MH_EnableHook(executeTarget) != MH_OK) TraceRuntime("HUD: ExecuteCommandList hook unavailable; HUD left unchanged");
    }
    g_terrainCaptureInstalled.store(true);
    g_installed = true;
    return true;
}

std::string DescribeCameraCapture() {
    if (!g_installed) return "camera capture is not installed";
    std::string projectionVsSlots;
    for (UINT slot = 0; slot < g_projectionVsBindings.size(); ++slot) {
        const auto bindings = g_projectionVsBindings[slot].load();
        if (bindings) projectionVsSlots += (projectionVsSlots.empty() ? "" : ",") + std::to_string(slot) + ":" + std::to_string(bindings);
    }
    return "camera capture: buffer updates=" + std::to_string(g_bufferUpdates.load()) +
           " dynamic map writes=" + std::to_string(g_dynamicMapWrites.load()) +
           " matrix samples=" + std::to_string(g_matrixSamples.load()) +
           " projection-shaped=" + std::to_string(g_projectionCandidates.load()) +
           " rigid-transform-shaped=" + std::to_string(g_rigidTransformCandidates.load()) +
           " projection-buffer VS slots=" + (projectionVsSlots.empty() ? "none" : projectionVsSlots) +
           " terrain capture=" + (g_terrainCaptureInstalled.load() ? "installed" : "unavailable") +
           " terrain draws=[close " + std::to_string(g_terrainDraws[0].load()) +
           ",mid " + std::to_string(g_terrainDraws[1].load()) +
           ",far " + std::to_string(g_terrainDraws[2].load()) +
           ",tessellated-domain " + std::to_string(g_terrainDraws[3].load()) + "]";
}

CameraProjectionSample GetLatestCameraProjection() {
    const float m00 = g_latestProjectionM00.load();
    const float m11 = g_latestProjectionM11.load();
    return {m00 > 0.1f && m11 > 0.1f, m00, m11, g_latestProjectionM20.load(), g_latestProjectionM21.load()};
}

void ConfigureNativeEyeProjection(bool enabled) {
    g_nativeEyeProjectionEnabled.store(enabled);
    if (!enabled) g_nativeEyeProjectionValid.store(false);
}

void ConfigureNativeCameraProbe(bool enabled) {
    g_nativeCameraProbePermitted.store(enabled);
    if (!enabled) {
        TraceRuntime("native camera probe disabled at boot; no renderer hook will install");
    } else {
        TraceRuntime("native camera probe permitted but disarmed; press F4 only after a stable in-world scene and 120 verified projection uploads");
    }
}

void ConfigureShadowProbe(bool enabled) {
    g_shadowProbeEnabled.store(enabled);
    if (enabled) TraceRuntime("bounded depth-only shadow-pass sampler enabled: one in 128 indexed draws, maximum 12 reports");
}

void ConfigureNativeViewProjectionSync(bool enabled) {
    g_nativeViewProjectionSyncEnabled.store(enabled);
    TraceRuntime(std::string("native camera view-projection translation synchronization ") + (enabled ? "enabled" : "disabled"));
}

void ConfigureNativeCameraProjectionSync(bool enabled) {
    g_nativeCameraProjectionSyncEnabled.store(enabled);
    TraceRuntime(std::string("native projection-camera XR frustum synchronization ") + (enabled ? "enabled (experimental)" : "disabled"));
}

void SetPreHudCapture(bool enabled, ID3D11Texture2D* backbuffer) {
    g_preHudEnabled = enabled;
    g_preHudBackbuffer = backbuffer;
}

ID3D11Texture2D* ConsumePreHudCopy() {
    const bool valid = std::exchange(g_preHudValid, false);
    return valid ? g_preHudCopy.Get() : nullptr;
}

void HudProbePresent(ID3D11Texture2D* backbuffer) {
    if (!g_hudProbeTargets[0]) return;
    if (g_hudProbeActive.load()) {
        // Two Presents: deferred contexts may record a frame before execution.
        if (--g_hudProbeFramesLeft > 0) return;
        g_hudProbeActive.store(false);
        for (void* target : g_hudProbeTargets) MH_DisableHook(target);
        std::lock_guard lock(g_hudProbeMutex);
        // Merge consecutive segments of the same context and target.
        std::vector<HudProbeSegment> merged;
        for (const auto& segment : g_hudProbeSegments) {
            if (!merged.empty() && !segment.execute && !merged.back().execute &&
                merged.back().context == segment.context && merged.back().resource == segment.resource) {
                merged.back().draws += segment.draws;
                merged.back().blended += segment.blended;
                if (!merged.back().psSource && segment.psSource) {
                    merged.back().psSource = segment.psSource;
                    merged.back().psSourceWidth = segment.psSourceWidth;
                    merged.back().psSourceHeight = segment.psSourceHeight;
                }
            } else merged.push_back(segment);
        }
        std::unordered_map<ID3D11DeviceContext*, std::array<std::uint32_t, 3>> totals;  // draws, blended, executes
        for (const auto& segment : merged) {
            auto& total = totals[segment.context];
            total[0] += segment.draws;
            total[1] += segment.blended;
            total[2] += segment.execute ? 1u : 0u;
        }
        std::ostringstream summary;
        summary << "hud probe v2 (2 frames): segments=" << merged.size() << " contexts=" << totals.size() << " [";
        for (const auto& [context, total] : totals)
            summary << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(context) << std::dec << (context == g_immediateContext ? "(immediate)" : "")
                    << " draws=" << total[0] << " blended=" << total[1] << " executes=" << total[2] << "; ";
        summary << "]";
        TraceRuntime(summary.str());
        // Report blended-heavy targets (HUD candidates), executes and the final pass.
        int lines = 0;
        for (std::size_t index = 0; index != merged.size() && lines < 60; ++index) {
            const auto& segment = merged[index];
            if (!(segment.execute || segment.backbuffer || segment.blended >= 4 || segment.draws >= 200)) continue;
            ++lines;
            std::ostringstream line;
            line << "hud probe v2 #" << index << " ctx=0x" << std::hex << reinterpret_cast<std::uintptr_t>(segment.context)
                 << " tid=" << std::dec << segment.thread << (segment.execute ? " EXECUTE_COMMAND_LIST" : "")
                 << " target=0x" << std::hex << reinterpret_cast<std::uintptr_t>(segment.resource) << std::dec
                 << ' ' << segment.width << 'x' << segment.height << " fmt=" << segment.format
                 << (segment.backbuffer ? " BACKBUFFER" : "") << " draws=" << segment.draws << " blended=" << segment.blended;
            if (segment.psSource)
                line << " ps_t0=0x" << std::hex << reinterpret_cast<std::uintptr_t>(segment.psSource) << std::dec
                     << ' ' << segment.psSourceWidth << 'x' << segment.psSourceHeight;
            TraceRuntime(line.str());
        }
        g_hudProbeSegments.clear();
        g_hudProbeCurrent.clear();
        g_hudProbeBlendByContext.clear();
        return;
    }
    // Arm: after 10 s of continuous eye rendering, then every ~40 s, 3 max.
    static std::uint32_t captures = 0;
    static std::uint64_t sceneFrames = 0;
    if (IsNativeSceneRendering()) ++sceneFrames; else sceneFrames = 0;
    if (captures >= 3 || sceneFrames < 900 || (sceneFrames - 900) % 3600 != 0) return;
    ++captures;
    {
        std::lock_guard lock(g_hudProbeMutex);
        g_hudProbeBackbuffer = backbuffer;
        g_hudProbeSegments.clear();
        g_hudProbeSegments.reserve(6000);
        g_hudProbeCurrent.clear();
        g_hudProbeBlendByContext.clear();
    }
    g_hudProbeFramesLeft = 2;
    g_hudProbeActive.store(true);
    for (void* target : g_hudProbeTargets) MH_EnableHook(target);
}

void ConfigureStereoCoherenceProbe(bool enabled) {
    g_coherenceProbeEnabled.store(enabled);
    TraceRuntime(std::string("stereo coherence draw/upload probe ") + (enabled ? "enabled (read-only, bounded)" : "disabled"));
}

void ConfigurePairedCameraUploadSync(bool enabled) {
    g_pairedUploadSyncEnabled.store(enabled);
    TraceRuntime(std::string("paired camera re-sync at Globals upload ") + (enabled ? "enabled (experimental)" : "disabled"));
}

void SyncPairedCameraProjectionToEye(const NativeEyeProjection& eye) {
    // JC3 copies owner+0x5C0 into 0x5C8 whenever its camera moves. With the
    // engine camera steered every frame, a copy landing after the Present
    // write carried the previous eye's projection into 0x5C8 (live: left eye
    // starved for thousands of frames). Keep both records on the requested
    // eye from the Present handoff onward.
    if (!g_nativeCameraProjectionSyncEnabled.load() || !eye.valid || !std::isfinite(eye.m00) || !std::isfinite(eye.m11) ||
        !std::isfinite(eye.m20) || !std::isfinite(eye.m21)) return;
    constexpr std::uintptr_t kOwnerGlobalRva = 0x02ED0E20;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* owner = nullptr;
    if (!imageBase || !IsReadableRange(reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner))) return;
    std::memcpy(&owner, reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner));
    std::byte* camera = nullptr;
    if (!owner || !IsReadableRange(static_cast<const std::byte*>(owner) + 0x5C0, sizeof(camera))) return;
    std::memcpy(&camera, static_cast<const std::byte*>(owner) + 0x5C0, sizeof(camera));
    if (!HasNativeCameraSignature(camera) || !IsWritableRange(camera + 0x154, sizeof(float) * 16)) return;
    std::lock_guard lock(g_nativeCameraOffsetMutex);
    auto* projection = reinterpret_cast<float*>(camera + 0x154);
    if (!(std::abs(projection[11] + 1.0f) < 0.001f)) return;
    projection[0] = eye.m00;
    projection[5] = eye.m11;
    projection[8] = eye.m20;
    projection[9] = eye.m21;
    RebuildNativeViewProjection(camera);
}

void ArmPairedCameraUploadSync(const NativeCameraOffset& offset, const NativeEyeProjection& projection) {
    std::lock_guard lock(g_pairedUploadMutex);
    g_pairedOffset = offset;
    g_pairedProjection = projection;
    g_pairedAppliesThisFrame = 0;
    g_pairedCamera = nullptr;
}

void ConfigureCameraTimelineProbe(bool enabled) {
    g_timelineEnabled.store(enabled);
    TraceRuntime(std::string("camera refresh timeline probe ") + (enabled ? "enabled (read-only, 3 bursts of 6 frames)" : "disabled"));
}

void CameraTimelineAtPresent(const char* phase) {
    if (!g_timelineEnabled.load()) return;
    const bool before = std::strcmp(phase, "present-before-writes") == 0;
    if (before) {
        const auto maps = g_globalsMapsThisFrame.exchange(0);
        const auto busy = maps > 20 ? g_busyFrames.fetch_add(1) + 1 : (g_busyFrames.store(0), 0u);
        auto left = g_timelineFramesLeft.load();
        if (left > 0) g_timelineFramesLeft.store(--left);
        else if (busy == 300 || busy == 3000 || busy == 9000) {
            if (g_timelineRuns.fetch_add(1) < 3) g_timelineFramesLeft.store(6);
        }
        if (left > 0 || g_timelineFramesLeft.load() > 0)
            TraceRuntime("timeline frame-end globals_maps=" + std::to_string(maps) + " tid=" + std::to_string(GetCurrentThreadId()));
    }
    if (g_timelineFramesLeft.load() > 0)
        TraceRuntime(std::string("timeline ") + phase + " tid=" + std::to_string(GetCurrentThreadId()) + " " + CompactCameraState());
}

void ArmStereoCoherenceDrawSample() {
    if (g_coherenceProbeEnabled.load()) g_coherenceDrawArmed.store(true);
}

bool ShouldAdvanceNativeEye(bool drewSinceArm) {
    // In-world, advance to the other eye only if a scene was actually
    // rendered with the current request since the last Present. JC3 can
    // Present without a new scene render; flipping on those Presents
    // overwrote the pending request and locked rendering onto one eye (live:
    // right eye 2400 frames stale). Menus have no recovered scene
    // projection, so they keep the draw-count signal.
    const bool inWorld = g_presentsSinceSceneProjection.load() < 30;
    static bool heldLastPresent = false;
    const bool advance = !inWorld ? drewSinceArm : (g_sceneProjectionThisFrame.load() || heldLastPresent);
    heldLastPresent = !advance;
    return advance;
}

void SetNativeEyeProjection(const NativeEyeProjection& projection) {
    if (g_sceneProjectionThisFrame.exchange(false)) g_presentsSinceSceneProjection.store(0);
    else g_presentsSinceSceneProjection.store(std::min<std::uint32_t>(g_presentsSinceSceneProjection.load() + 1, 1000));
    g_nativeEye.store(projection.eye);
    g_nativeProjectionM00.store(projection.m00);
    g_nativeProjectionM11.store(projection.m11);
    g_nativeProjectionM20.store(projection.m20);
    g_nativeProjectionM21.store(projection.m21);
    g_nativeCameraLocalX.store(projection.cameraLocalX);
    g_nativeEyeProjectionValid.store(projection.valid);
}

bool ForceNativeEyeProjection(ID3D11DeviceContext* context, const NativeEyeProjection& projection) {
    if (!context || !projection.valid || !g_nativeEyeProjectionEnabled.load()) return false;
    ID3D11Buffer* buffer = nullptr;
    std::array<std::byte, 784> snapshot{};
    {
        std::lock_guard lock(g_projectionSnapshotMutex);
        if (!g_projectionSnapshotValid || !g_projectionBufferOwned) return false;
        buffer = g_projectionBufferOwned;
        buffer->AddRef();
        snapshot = g_projectionSnapshot;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (desc.ByteWidth != snapshot.size() || desc.Usage != D3D11_USAGE_DYNAMIC ||
        !(desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) || !g_originalMap || !g_originalUnmap) {
        buffer->Release();
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mappedResult = g_originalMap(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(mappedResult) || !mapped.pData) {
        buffer->Release();
        return false;
    }
    std::memcpy(mapped.pData, snapshot.data(), snapshot.size());
    auto* values = static_cast<float*>(mapped.pData);
    values[0] = projection.m00;
    values[5] = projection.m11;
    values[8] = projection.m20;
    values[9] = projection.m21;
    values[12] -= projection.m00 * projection.cameraLocalX;
    g_originalUnmap(context, buffer, 0);
    g_latestProjectionM00.store(projection.m00);
    g_latestProjectionM11.store(projection.m11);
    g_latestProjectionM20.store(projection.m20);
    g_latestProjectionM21.store(projection.m21);
    const auto count = ++g_forcedProjectionWrites;
    if (count == 1 || count % 1200 == 0)
        TraceRuntime("forced native projection eye=" + std::to_string(projection.eye) +
            " m20=" + std::to_string(projection.m20) +
            " normal_uploads=" + std::to_string(g_nativeProjectionWrites.load()) +
            " forced_uploads=" + std::to_string(count));
    buffer->Release();
    return true;
}

std::uint64_t GetObservedDrawCount() { return g_observedDraws.load(); }
void SetInternalRenderPass(bool active) { g_internalRenderPass = active; }

bool SyncNativeCameraViewProjection(std::byte* camera) {
    if (!g_nativeViewProjectionSyncEnabled.load() || !camera ||
        !IsReadableRange(camera + 0x154, sizeof(float) * 16) ||
        !IsWritableRange(camera + 0x194, sizeof(float) * 16)) return false;
    const auto* world = reinterpret_cast<const float*>(camera + 0x94);
    const auto* projection = reinterpret_cast<const float*>(camera + 0x154);
    auto* viewProjection = reinterpret_cast<float*>(camera + 0x194);
    if (!IsNativeWorldTransform(world)) return false;
    // V = inverse of the rigid row-vector camera-world matrix. The first
    // three VP rows must already describe this rotation and projection;
    // otherwise this is not the camera record we can safely reconcile.
    for (int row = 0; row != 3; ++row) {
        for (int column = 0; column != 4; ++column) {
            double expected = 0.0;
            for (int axis = 0; axis != 3; ++axis)
                expected += static_cast<double>(world[axis * 4 + row]) * projection[axis * 4 + column];
            if (!std::isfinite(expected) || !std::isfinite(viewProjection[row * 4 + column]) ||
                std::abs(expected - viewProjection[row * 4 + column]) > 0.025) return false;
        }
    }
    double viewTranslation[3]{};
    for (int axis = 0; axis != 3; ++axis)
        for (int component = 0; component != 3; ++component)
            viewTranslation[axis] -= static_cast<double>(world[axis * 4 + component]) * world[12 + component];
    float expectedRow[4]{};
    for (int column = 0; column != 4; ++column) {
        double expected = projection[12 + column];
        for (int axis = 0; axis != 3; ++axis)
            expected += viewTranslation[axis] * projection[axis * 4 + column];
        if (!std::isfinite(expected)) return false;
        expectedRow[column] = static_cast<float>(expected);
    }
    const float previousX = viewProjection[12];
    const float previousY = viewProjection[13];
    const float previousW = viewProjection[15];
    std::memcpy(viewProjection + 12, expectedRow, sizeof(expectedRow));
    const auto writes = ++g_nativeViewProjectionSyncWrites;
    if (writes == 1 || writes % 1200 == 0)
        TraceRuntime("native VP translation reconciled: residual_before=[" +
            std::to_string(previousX - expectedRow[0]) + "," +
            std::to_string(previousY - expectedRow[1]) + "," +
            std::to_string(previousW - expectedRow[3]) + "] writes=" + std::to_string(writes));
    return true;
}

namespace {
// True when VP rows 0-2 equal inverse(rigid world) * P for this record, i.e.
// the engine (or our last rebuild) left world, P and VP describing one camera.
bool NativeViewProjectionRowsMatch(const std::byte* camera) {
    const auto* world = reinterpret_cast<const float*>(camera + 0x94);
    const auto* projection = reinterpret_cast<const float*>(camera + 0x154);
    const auto* viewProjection = reinterpret_cast<const float*>(camera + 0x194);
    for (int row = 0; row != 3; ++row)
        for (int column = 0; column != 4; ++column) {
            double expected = 0.0;
            for (int axis = 0; axis != 3; ++axis)
                expected += static_cast<double>(world[axis * 4 + row]) * projection[axis * 4 + column];
            if (!std::isfinite(expected) || !std::isfinite(viewProjection[row * 4 + column]) ||
                std::abs(expected - viewProjection[row * 4 + column]) > 0.025) return false;
        }
    return true;
}

// Full VP = inverse(rigid world) * P, required after a rotation write.
bool RebuildNativeViewProjection(std::byte* camera) {
    if (!IsWritableRange(camera + 0x194, sizeof(float) * 16)) return false;
    const auto* world = reinterpret_cast<const float*>(camera + 0x94);
    const auto* projection = reinterpret_cast<const float*>(camera + 0x154);
    double viewTranslation[3]{};
    for (int axis = 0; axis != 3; ++axis)
        for (int component = 0; component != 3; ++component)
            viewTranslation[axis] -= static_cast<double>(world[axis * 4 + component]) * world[12 + component];
    float result[16]{};
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column) {
            double value = row == 3 ? projection[12 + column] : 0.0;
            for (int axis = 0; axis != 3; ++axis)
                value += (row == 3 ? viewTranslation[axis] : static_cast<double>(world[axis * 4 + row])) * projection[axis * 4 + column];
            if (!std::isfinite(value)) return false;
            result[row * 4 + column] = static_cast<float>(value);
        }
    std::memcpy(camera + 0x194, result, sizeof(result));
    return true;
}

// Engine yaw (horizontal right axis) composed with the head axes given in the
// recentred body frame. World up is +Y; back = right x up.
bool ComposeHeadBasis(const std::array<float, 9>& engine, const float* headAxes, float* rows) {
    float right[3]{engine[0], 0.0f, engine[2]};
    const float length = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (length < 0.2f) return false;
    right[0] /= length;
    right[2] /= length;
    const float up[3]{0.0f, 1.0f, 0.0f};
    const float back[3]{-right[2], 0.0f, right[0]};
    for (int axis = 0; axis != 3; ++axis)
        for (int component = 0; component != 3; ++component) {
            rows[axis * 3 + component] = headAxes[axis * 3] * right[component] +
                headAxes[axis * 3 + 1] * up[component] + headAxes[axis * 3 + 2] * back[component];
            if (!std::isfinite(rows[axis * 3 + component])) return false;
        }
    return true;
}

bool BasisMatches(const float* matrix, const std::array<float, 9>& basis) {
    for (int axis = 0; axis != 3; ++axis)
        for (int component = 0; component != 3; ++component)
            if (std::abs(matrix[axis * 4 + component] - basis[axis * 3 + component]) > 0.0005f) return false;
    return true;
}

// Render-camera culling set (live layout, CONFIRMED on this build): three
// copies of {inverse view V, V*P2, P2} at +0x214/+0x254/+0x294 with stride
// 0xC0 (the third copy has no P2), six outward frustum planes (n, d) at
// +0x414 (near, far, left, right, top, bottom) and each plane's normal split
// into positive/negative parts at +0x474 (32 bytes per plane). The engine
// only refreshes these when its own camera changes, so after writing the
// head/eye world transform they are rebuilt from it and the eye frustum.
std::atomic<bool> g_cullingSyncEnabled = false;
// Diagnostic bisection mask: 1 = inverse views, 2 = V*P2, 4 = frustum planes.
std::atomic<unsigned> g_cullingSyncParts = 7u;
// Scale on JC3's culling projection P2 (m00/m11). HYPOTHESIS: P2 feeds
// screen-size LOD selection; with m00 0.83 vs the eye's 1.69 JC3 thinks
// objects are ~2x smaller than the headset shows them and picks far proxies
// (black/blue slabs). 1 = unchanged (engine P2).
std::atomic<float> g_lodProjectionScale = 1.0f;
// Culling frustum mode (0 eye, 1 both-eye union, 2 union + widened P2); F5 cycles.
std::atomic<int> g_cullingMode = 2;
std::atomic<std::uint64_t> g_cullingSyncWrites = 0;
std::atomic<std::uint32_t> g_cullingSyncRejects = 0;

bool IsRigidRowMatrix(const float* m) {
    for (int i = 0; i != 16; ++i) if (!std::isfinite(m[i])) return false;
    auto length = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    return std::abs(length(m) - 1.0f) < 0.01f && std::abs(length(m + 4) - 1.0f) < 0.01f &&
           std::abs(length(m + 8) - 1.0f) < 0.01f && std::abs(m[3]) < 1e-4f && std::abs(m[7]) < 1e-4f &&
           std::abs(m[11]) < 1e-4f && std::abs(m[15] - 1.0f) < 1e-4f;
}

bool RebuildNativeCullingState(std::byte* camera) {
    constexpr std::size_t kViews[3]{0x214, 0x2D4, 0x394};
    constexpr std::size_t kViewProjections[3]{0x254, 0x314, 0x3D4};
    constexpr std::size_t kProjections[2]{0x294, 0x354};
    constexpr std::size_t kPlanes = 0x414;
    constexpr std::size_t kPlaneParts = 0x474;
    const auto reject = [](const char* reason) {
        const auto count = ++g_cullingSyncRejects;
        if (count == 1 || count % 600 == 0)
            TraceRuntime(std::string("culling camera sync skipped: ") + reason + " count=" + std::to_string(count));
        return false;
    };
    if (!IsWritableRange(camera + 0x214, 0x578 - 0x214)) return reject("range not writable");
    auto* planes = reinterpret_cast<float*>(camera + kPlanes);
    auto* parts = reinterpret_cast<float*>(camera + kPlaneParts);
    for (const auto offset : kViews)
        if (!IsRigidRowMatrix(reinterpret_cast<const float*>(camera + offset))) return reject("inverse-view layout mismatch");
    for (int plane = 0; plane != 6; ++plane) {
        const float* p = planes + plane * 4;
        const float* positive = parts + plane * 8;
        const float* negative = positive + 4;
        if (!(std::abs(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - 1.0f) < 0.01f)) return reject("plane layout mismatch");
        for (int c = 0; c != 3; ++c)
            if (std::abs(positive[c] + negative[c] - p[c]) > 1e-4f || positive[c] < 0.0f || negative[c] > 0.0f) return reject("split-normal layout mismatch");
    }
    const auto* storedNegatedDistances = reinterpret_cast<const float*>(camera + 0x534);
    for (int plane = 0; plane != 6; ++plane) {
        const float d = planes[plane * 4 + 3];
        if (std::abs(storedNegatedDistances[plane] + d) > 0.01f * std::max(1.0f, std::abs(d))) return reject("negated-distance layout mismatch");
    }
    for (const auto offset : kProjections) {
        const float* p2 = reinterpret_cast<const float*>(camera + offset);
        if (!(std::abs(p2[11] + 1.0f) < 0.001f) || !std::isfinite(p2[0]) || !std::isfinite(p2[5])) return reject("culling projection layout mismatch");
    }
    const auto* world = reinterpret_cast<const float*>(camera + 0x94);
    const float* right = world;
    const float* up = world + 4;
    const float* back = world + 8;
    const float* position = world + 12;
    // Near/far distances as the engine stored them, measured against the
    // camera position implied by the current inverse view (both were written
    // together, by the engine or by us).
    const float* oldView = reinterpret_cast<const float*>(camera + kViews[0]);
    float oldPosition[3]{};
    for (int axis = 0; axis != 3; ++axis)
        oldPosition[axis] = -(oldView[12] * oldView[axis * 4] + oldView[13] * oldView[axis * 4 + 1] + oldView[14] * oldView[axis * 4 + 2]);
    const auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    // The game thread rewrites the paired record concurrently, so a read can
    // mix its planes with a different frame's view (measured near -0.8..-1.4
    // instead of the consistent 0.1). Accept only plausible measurements and
    // otherwise reuse the last consistent near/far pair.
    static std::atomic<float> lastNear = -1.0f;
    static std::atomic<float> lastFar = -1.0f;
    float nearDistance = dot3(planes, oldPosition) + planes[3];
    float farDistance = -(dot3(planes + 4, oldPosition) + planes[7]);
    // The culling projection's row-3 depth term is the exact near distance
    // (0.1 on this build, matching consistent plane reads).
    if (const float projectedNear = reinterpret_cast<const float*>(camera + kProjections[0])[14]; projectedNear > 0.01f && projectedNear < 1.0f)
        nearDistance = projectedNear;
    if (std::isfinite(nearDistance) && std::isfinite(farDistance) && nearDistance > 0.01f && nearDistance < 1.0f && farDistance > 100.0f) {
        lastNear.store(nearDistance);
        lastFar.store(farDistance);
    } else if (lastNear.load() > 0.0f) {
        nearDistance = lastNear.load();
        farDistance = lastFar.load();
    } else {
        return reject(("near/far distances implausible near=" + std::to_string(nearDistance) + " far=" + std::to_string(farDistance)).c_str());
    }
    // Inverse view of the rigid world transform.
    float view[16]{};
    for (int row = 0; row != 3; ++row)
        for (int column = 0; column != 3; ++column) view[row * 4 + column] = world[column * 4 + row];
    for (int column = 0; column != 3; ++column) view[12 + column] = -dot3(position, world + column * 4);
    view[15] = 1.0f;
    // P2 stays JC3's own projection: writing the wide headset frustum into it
    // turned distant buildings into black LOD proxies (it appears to feed
    // screen-size LOD selection). The frustum planes below use the eye
    // frustum instead, so culling still covers everything the eye can see.
    const bool eyeFrustum = g_nativeCameraProjectionSyncEnabled.load() && g_nativeEyeProjectionValid.load();
    float p2[16]{};
    std::memcpy(p2, camera + kProjections[0], sizeof(p2));
    for (const float value : p2) if (!std::isfinite(value)) return reject("culling projection not finite");
    // Culling frustum. Mode 0: the eye being requested. Mode 1: union of both
    // eye frusta (JC3 appears to consume the culling camera a frame after it
    // is written, i.e. for the other eye: live, the right eye lost terrain at
    // its right edge; the union made it "way better"), with a 20% tangent
    // margin for head motion between frames. Mode 2: mode 1 plus JC3's own
    // culling projection P2 widened to the same union (P2 is 16:9, tangents
    // ~1.2 x 0.67, far narrower than the eye's 1.38 x 1.43 down).
    float frustum[4]{p2[0], p2[5], p2[8], p2[9]};
    const int mode = g_cullingMode.load();
    if (eyeFrustum) {
        frustum[0] = g_nativeProjectionM00.load();
        frustum[1] = g_nativeProjectionM11.load();
        frustum[2] = g_nativeProjectionM20.load();
        frustum[3] = g_nativeProjectionM21.load();
        if (mode >= 1) {
            constexpr float kMargin = 1.2f;
            frustum[0] = frustum[0] / ((1.0f + std::abs(frustum[2])) * kMargin);
            frustum[1] = frustum[1] / ((1.0f + std::abs(frustum[3])) * kMargin);
            frustum[2] = 0.0f;
            frustum[3] = 0.0f;
        }
    }
    {
        // Track the engine's own P2 m00/m11 (a value we wrote is recognised
        // and not mistaken for the engine's), then write the LOD-scaled engine
        // values, or the eye union in mode 2.
        static float writtenM00 = 0.0f, writtenM11 = 0.0f, engineM00 = 0.0f, engineM11 = 0.0f;
        if (p2[0] != writtenM00 || p2[5] != writtenM11) {
            engineM00 = p2[0];
            engineM11 = p2[5];
        }
        const float scale = std::clamp(g_lodProjectionScale.load(), 0.5f, 4.0f);
        if (engineM00 > 0.0f && engineM11 > 0.0f) {
            const bool widen = mode >= 2 && eyeFrustum;
            p2[0] = widen ? frustum[0] : engineM00 * scale;
            p2[5] = widen ? frustum[1] : engineM11 * scale;
            if (widen) { p2[8] = 0.0f; p2[9] = 0.0f; }
            const bool ours = widen || scale != 1.0f;
            writtenM00 = ours ? p2[0] : 0.0f;
            writtenM11 = ours ? p2[5] : 0.0f;
            for (const auto offset : kProjections) {
                auto* stored = reinterpret_cast<float*>(camera + offset);
                stored[0] = p2[0];
                stored[5] = p2[5];
                if (widen) { stored[8] = 0.0f; stored[9] = 0.0f; }
            }
        }
    }
    float viewProjection[16]{};
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column) {
            double value = 0.0;
            for (int k = 0; k != 4; ++k) value += static_cast<double>(view[row * 4 + k]) * p2[k * 4 + column];
            viewProjection[row * 4 + column] = static_cast<float>(value);
        }
    // Outward view-space side-plane normals for a row-vector P looking down -Z.
    const float m00 = frustum[0], m11 = frustum[1], m20 = frustum[2], m21 = frustum[3];
    const float sides[4][3]{{-m00, 0.0f, 1.0f - m20}, {m00, 0.0f, m20 + 1.0f}, {0.0f, m11, m21 + 1.0f}, {0.0f, -m11, 1.0f - m21}};
    float newPlanes[24]{};
    for (int c = 0; c != 3; ++c) {
        newPlanes[c] = back[c];
        newPlanes[4 + c] = -back[c];
    }
    newPlanes[3] = nearDistance - dot3(back, position);
    newPlanes[7] = -farDistance + dot3(back, position);
    for (int side = 0; side != 4; ++side) {
        float* n = newPlanes + (side + 2) * 4;
        const float length = std::sqrt(sides[side][0] * sides[side][0] + sides[side][1] * sides[side][1] + sides[side][2] * sides[side][2]);
        if (!(length > 1e-4f)) return reject("degenerate eye frustum");
        for (int c = 0; c != 3; ++c)
            n[c] = (sides[side][0] * right[c] + sides[side][1] * up[c] + sides[side][2] * back[c]) / length;
        n[3] = -dot3(n, position);
    }
    for (const float value : newPlanes) if (!std::isfinite(value)) return reject("plane not finite");
    const unsigned parts_mask = g_cullingSyncParts.load();
    if (parts_mask & 1u) for (const auto offset : kViews) std::memcpy(camera + offset, view, sizeof(view));
    if (parts_mask & 2u) for (const auto offset : kViewProjections) std::memcpy(camera + offset, viewProjection, sizeof(viewProjection));
    if (parts_mask & 4u) {
        std::memcpy(planes, newPlanes, sizeof(newPlanes));
        // Companion culling data (live layout): the six plane distances again,
        // negated, at +0x534, then the culling position at +0x560 and
        // view-forward at +0x56C. Leaving these stale while the normals move
        // made JC3 cull in-view objects and drop the town to LOD proxies.
        auto* negatedDistances = reinterpret_cast<float*>(camera + 0x534);
        for (int plane = 0; plane != 6; ++plane) negatedDistances[plane] = -newPlanes[plane * 4 + 3];
        auto* cullingPosition = reinterpret_cast<float*>(camera + 0x560);
        auto* cullingForward = reinterpret_cast<float*>(camera + 0x56C);
        for (int c = 0; c != 3; ++c) {
            cullingPosition[c] = position[c];
            cullingForward[c] = -back[c];
        }
    }
    for (int plane = 0; plane != 6 && (parts_mask & 4u); ++plane) {
        float* positive = parts + plane * 8;
        float* negative = positive + 4;
        for (int c = 0; c != 3; ++c) {
            positive[c] = std::max(newPlanes[plane * 4 + c], 0.0f);
            negative[c] = std::min(newPlanes[plane * 4 + c], 0.0f);
        }
        positive[3] = negative[3] = 0.0f;
    }
    // Mode 2: JC3's own FOV fields widened to the eye union. Live dump
    // (2026-09-29 20:21): +0x580/+0x584/+0x588 vertical FOV 1.13446 rad (65
    // deg, three copies), +0x58C 1/tan(v/2) 1.57048, +0x594 near 0.1, +0x598
    // far 40000, +0x5A0 tan(v/2) 0.636747, +0x5A4 aspect 0.927536 (the
    // 2048x2208 window). A visibility test on these keeps only +/-32.5 deg
    // vertically while the eyes reach 44 deg up / 55 deg down. Written only
    // when the layout still matches (tan = 1/m, near 0.1, far > 1000).
    if (g_cullingMode.load() >= 2 && eyeFrustum && IsWritableRange(camera + 0x578, 0x30)) {
        auto* fov = reinterpret_cast<float*>(camera + 0x578);  // fov[k] = +0x578 + 4k
        const float tanHalfV = fov[10], inverse = fov[5], nearValue = fov[7], farValue = fov[8], aspect = fov[11];
        const bool layout = tanHalfV > 0.1f && tanHalfV < 10.0f && std::abs(tanHalfV * inverse - 1.0f) < 0.02f &&
                            std::abs(nearValue - 0.1f) < 0.02f && farValue > 1000.0f && aspect > 0.3f && aspect < 3.0f &&
                            std::abs(fov[2] - 2.0f * std::atan(tanHalfV)) < 0.05f;
        if (layout) {
            // Union half-extents (tangent) from the frustum already computed
            // (m00/m11 include the both-eye union and the 20% margin).
            const float newTanV = 1.0f / m11;
            const float newTanH = 1.0f / m00;
            const float newFov = 2.0f * std::atan(newTanV);
            if (std::abs(fov[2] - newFov) > 1e-4f) NoteEngineCameraFov(fov[2]);
            fov[2] = fov[3] = fov[4] = newFov;
            fov[5] = 1.0f / newTanV;
            fov[10] = newTanV;
            fov[11] = newTanH / newTanV;
            static std::uint32_t fovReports = 0;
            if (fovReports++ % 3600 == 0)
                TraceRuntime("culling FOV fields widened: v_fov_deg=" + std::to_string(newFov * 57.2958f) + " tan_v=" + std::to_string(newTanV) +
                    " aspect=" + std::to_string(newTanH / newTanV) + " (engine was tan_v=" + std::to_string(tanHalfV) + " aspect=" + std::to_string(aspect) + ")");
        } else {
            static std::uint32_t mismatches = 0;
            if (mismatches++ % 3600 == 0) TraceRuntime("culling FOV fields not written: layout mismatch");
        }
    }
    // One-time read-only dump of the render-camera tail (+0x560..+0x800): FOV
    // scalars at +0x578.. and the unsynced engine-orientation copies noted
    // at +0x5CC/+0x60C/+0x64C/+0x6CC/+0x70C/+0x74C, to map them before any
    // write (edge holes persist with the plane union and widened P2).
    if (g_cullingSyncWrites.load() == 2400 && IsReadableRange(camera + 0x560, 0x800 - 0x560)) {
        const auto* tail = reinterpret_cast<const float*>(camera + 0x560);
        for (int block = 0; block != (0x800 - 0x560) / 0x40; ++block) {
            std::ostringstream line;
            line << "render-camera tail +0x" << std::hex << (0x560 + block * 0x40) << std::dec << ":";
            for (int i = 0; i != 16; ++i) line << ' ' << tail[block * 16 + i];
            TraceRuntime(line.str());
        }
        std::ostringstream eye;
        eye << "render-camera tail context: eye m00/m11/m20/m21=" << g_nativeProjectionM00.load() << '/' << g_nativeProjectionM11.load() << '/'
            << g_nativeProjectionM20.load() << '/' << g_nativeProjectionM21.load() << " world_row2(back)=" << back[0] << ',' << back[1] << ',' << back[2]
            << " position=" << position[0] << ',' << position[1] << ',' << position[2];
        TraceRuntime(eye.str());
    }
    const auto writes = ++g_cullingSyncWrites;
    if (writes == 1 || writes % 1200 == 0)
        TraceRuntime("culling camera synced: near=" + std::to_string(nearDistance) + " far=" + std::to_string(farDistance) +
            " eye_frustum=" + (eyeFrustum ? "1" : "0") + " left_plane=[" + std::to_string(newPlanes[8]) + "," +
            std::to_string(newPlanes[9]) + "," + std::to_string(newPlanes[10]) + "] writes=" + std::to_string(writes));
    return true;
}
}

bool SetNativeCameraOffset(const NativeCameraOffset& offset) {
    return SetNativeCameraOffsetForCameras(offset, 3u, true);
}

bool GetEngineCameraPosition(float position[3]) {
    std::lock_guard lock(g_nativeCameraOffsetMutex);
    if (!g_engineCameraBasisValid) return false;
    std::memcpy(position, g_engineCameraPosition.data(), sizeof(float) * 3);
    return true;
}

bool GetEngineCameraBasis(float rows[9]) {
    std::lock_guard lock(g_nativeCameraOffsetMutex);
    if (!g_engineCameraBasisValid) return false;
    std::memcpy(rows, g_engineCameraBasis.data(), sizeof(float) * 9);
    return true;
}

bool IsNativeSceneRendering() { return g_presentsSinceSceneProjection.load() < 30; }

int CycleCullingMode() {
    const int mode = (g_cullingMode.load() + 1) % 4;
    g_cullingMode.store(mode);
    return mode;
}

void SetCullingMode(int mode) { g_cullingMode.store(std::clamp(mode, 0, 3)); }

int GetCullingMode() { return g_cullingMode.load(); }

// Engine FOV scan hits -> camera records. A record's FOV triple sits at
// +0x580/+0x584/+0x588 (live dump), so candidates are hit - 0x580 (small
// alignment tweaks tried); each must pass the render-camera signature and the
// FOV layout (tan at +0x5A0 = 1 / value at +0x58C).
void RegisterCameraFovHits(const std::vector<std::uintptr_t>& hits) {
    std::vector<void*> found;
    for (const auto hit : hits) {
        for (int tweak = -4; tweak <= 4; ++tweak) {
            auto* base = reinterpret_cast<std::byte*>(hit - 0x580 + tweak * 4);
            if (!IsReadableRange(base, 0x5B0) || !HasNativeCameraSignature(base)) continue;
            const auto* fov = reinterpret_cast<const float*>(base + 0x578);
            if (!(std::abs(fov[10] * fov[5] - 1.0f) < 0.02f)) continue;
            if (std::find(found.begin(), found.end(), base) == found.end()) found.push_back(base);
        }
    }
    std::lock_guard lock(g_nativeCameraOffsetMutex);
    g_extraCameras = found;
    std::ostringstream line;
    line << "extra camera records registered: " << found.size();
    for (void* camera : found) line << " 0x" << std::hex << reinterpret_cast<std::uintptr_t>(camera);
    TraceRuntime(line.str());
}

void SetLodProjectionScale(float scale) {
    g_lodProjectionScale.store(scale);
}

void ConfigureCullingCameraSync(bool enabled, unsigned parts) {
    g_cullingSyncEnabled.store(enabled);
    g_cullingSyncParts.store(parts);
    TraceRuntime(std::string("render-camera culling set (inverse view, V*P2, frustum planes) sync ") + (enabled ? "enabled (experimental)" : "disabled") + " parts_mask=" + std::to_string(parts));
}

namespace {
bool SetNativeCameraOffsetForCameras(const NativeCameraOffset& offset, unsigned cameraMask, bool trace) {
    if (!offset.valid) return false;
    constexpr std::uintptr_t kCameraOwnerGlobalRva = 0x02ED0E20;
    constexpr std::array<std::size_t, 3> kWorldTransformOffsets{0x54, 0x94, 0xD4};
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* owner = nullptr;
    if (!imageBase || !IsReadableRange(reinterpret_cast<const void*>(imageBase + kCameraOwnerGlobalRva), sizeof(owner))) return false;
    std::memcpy(&owner, reinterpret_cast<const void*>(imageBase + kCameraOwnerGlobalRva), sizeof(owner));
    if (!owner || !IsReadableRange(static_cast<const std::byte*>(owner) + 0x5C8, sizeof(void*))) return false;
    std::vector<void*> cameras(2, nullptr);
    std::memcpy(&cameras[0], static_cast<const std::byte*>(owner) + 0x5C0, sizeof(void*));
    std::memcpy(&cameras[1], static_cast<const std::byte*>(owner) + 0x5C8, sizeof(void*));
    bool wrote = false;
    std::array<float, 3> firstWorldOffset{};
    // Refresh once for this camera transaction, shared by all paired records.
    // Never freeze the seat's absolute position across moving game frames.
    auto liveSeat = offset.baseAnchor ? GetFirstPersonCameraAnchor(true) : VehicleCameraAnchor{};
    if(liveSeat.traversal && offset.bodyFrame) SetTraversalYawFrame(liveSeat,offset.bodyRight);
    if (offset.baseAnchor && !liveSeat.valid) return false;
    std::lock_guard lock(g_nativeCameraOffsetMutex);
    // Mode 3: the other camera records carrying the engine view (per-frame
    // snapshots) get the same eye transform, union culling and FOV, but only
    // on the full Present-time write.
    if ((cameraMask & 2u) && g_cullingMode.load() >= 3)
        for (void* extra : g_extraCameras) cameras.push_back(extra);
    for (std::size_t cameraIndex = 0; cameraIndex != cameras.size(); ++cameraIndex) {
        if (cameraIndex < 2 && !(cameraMask & (1u << cameraIndex))) continue;
        auto* camera = static_cast<std::byte*>(cameras[cameraIndex]);
        if (!camera || std::find(cameras.begin(), cameras.begin() + cameraIndex, cameras[cameraIndex]) != cameras.begin() + cameraIndex ||
            !HasNativeCameraSignature(camera)) continue;
        const bool rotate = offset.rotate;
        bool cameraWrote = false;
        bool cameraRotated = false;
        for (const auto transformOffset : kWorldTransformOffsets) {
            auto* matrix = reinterpret_cast<float*>(camera + transformOffset);
            if (!IsWritableRange(matrix, sizeof(float) * 16) || !IsNativeWorldTransform(matrix)) continue;
            std::array<float, 3> engineTranslation{matrix[12], matrix[13], matrix[14]};
            std::array<float, 9> engineBasis{matrix[0], matrix[1], matrix[2], matrix[4], matrix[5], matrix[6], matrix[8], matrix[9], matrix[10]};
            // A value we wrote (to this or the other record) maps back to the
            // engine value it replaced; anything else is a fresh engine value.
            for (const auto& entry : g_writtenRing) {
                if (!entry.valid || !BasisMatches(matrix, entry.writtenBasis) ||
                    std::abs(matrix[12] - entry.writtenTranslation[0]) >= 0.0005f ||
                    std::abs(matrix[13] - entry.writtenTranslation[1]) >= 0.0005f ||
                    std::abs(matrix[14] - entry.writtenTranslation[2]) >= 0.0005f) continue;
                engineTranslation = entry.engineTranslation;
                engineBasis = entry.engineBasis;
                ++g_ringRecognitions;
                break;
            }
            if (cameraIndex == 1 && transformOffset == 0x94) {
                g_engineCameraBasis = engineBasis;
                g_engineCameraPosition = engineTranslation;
                g_engineCameraBasisValid = true;
            }
            // Frame the head rotation and eye offsets are composed on: the
            // engine camera's yaw, or with hand aim JC3VR's own body yaw.
            std::array<float, 9> frameBasis = engineBasis;
            if (offset.bodyFrame) {
                const float* r = offset.bodyRight;
                frameBasis = {r[0], 0.0f, r[2], 0.0f, 1.0f, 0.0f, -r[2], 0.0f, r[0]};
            }
            float engineMatrix[16]{};
            std::memcpy(engineMatrix, matrix, sizeof(engineMatrix));
            for (int axis = 0; axis != 3; ++axis)
                for (int component = 0; component != 3; ++component)
                    engineMatrix[axis * 4 + component] = frameBasis[axis * 3 + component];
            if(offset.baseAnchor) {
                std::copy(liveSeat.basis,liveSeat.basis+9,frameBasis.begin());
                for(int axis=0;axis<3;++axis) for(int c=0;c<3;++c) engineMatrix[axis*4+c]=frameBasis[axis*3+c];
            }
            const auto worldOffset = NativeWorldOffset(engineMatrix, offset);
            if (!wrote) firstWorldOffset = worldOffset;
            std::array<float, 9> rows = engineBasis;
            if(rotate) {
                const bool composed=offset.baseAnchor?
                    ComposeVehicleHeadBasis(frameBasis.data(),offset.headAxes,rows.data()):
                    ComposeHeadBasis(frameBasis,offset.headAxes,rows.data());
                if(!composed) rows=engineBasis;
            }
            if (!BasisMatches(matrix, rows)) cameraRotated = true;
            for (int axis = 0; axis != 3; ++axis) {
                for (int component = 0; component != 3; ++component)
                    matrix[axis * 4 + component] = rows[axis * 3 + component];
                matrix[12 + axis] = (offset.baseAnchor?liveSeat.position[axis]:engineTranslation[axis]) + worldOffset[axis];
            }
            // The rendered eye view, for JC3's own visibility culls (engine_culling).
            if (cameraIndex == 1 && transformOffset == 0x94) SetEngineCullViewWorld(matrix);
            auto& entry = g_writtenRing[g_writtenRingNext++ % g_writtenRing.size()];
            entry = {true, {matrix[12], matrix[13], matrix[14]}, rows, engineTranslation, engineBasis};
            // Kept for the J/E probes: the last value written at this address.
            auto& previous = g_lastNativeTranslations[reinterpret_cast<std::uintptr_t>(matrix)];
            previous.valid = true;
            for (int axis = 0; axis != 3; ++axis) {
                previous.written[axis] = matrix[12 + axis];
                previous.appliedOffset[axis] = worldOffset[axis];
            }
            wrote = true;
            cameraWrote = true;
        }
        if (cameraRotated || rotate) {
            if (RebuildNativeViewProjection(camera)) ++g_nativeHeadRotationWrites;
        } else if (cameraWrote) SyncNativeCameraViewProjection(camera);
        if (cameraWrote && g_cullingSyncEnabled.load()) RebuildNativeCullingState(camera);
    }
    if (wrote && trace) {
        g_lastCameraOffsetEye.store(offset.eye);
        const auto writes = ++g_nativeCameraOffsetWrites;
        if (writes == 1 || writes % 600 == 0) {
            TraceRuntime("native camera world offset applied eye=" + std::to_string(offset.eye) +
                " body_ruf=[" + std::to_string(offset.right) + "," + std::to_string(offset.up) + "," + std::to_string(offset.forward) +
                "] head_yaw=" + std::to_string(offset.headYawRadians) + " direct_rotation=" + (offset.rotate ? "1" : "0") +
                " rotation_writes=" + std::to_string(g_nativeHeadRotationWrites.load()) +
                " ring_recognitions=" + std::to_string(g_ringRecognitions.load()) + " world_xyz=[" +
                std::to_string(firstWorldOffset[0]) + "," + std::to_string(firstWorldOffset[1]) + "," + std::to_string(firstWorldOffset[2]) +
                "] records=owner+0x5c0/0x5c8 transforms=+0x54/+0x94/+0xd4");
        }
    }
    return wrote;
}

// Called from MapDetour for the Globals buffer. The engine refreshes the
// paired record after Present; restore this eye's position/rotation and XR
// frustum there (P x/y terms are depth-convention independent) and rebuild
// VP, at most a few times per frame and only when the engine overwrote it.
void ReapplyPairedCameraAtUpload() {
    CullOverrideAtUpload();
    std::lock_guard lock(g_pairedUploadMutex);
    if (!g_pairedOffset.valid || g_pairedAppliesThisFrame >= 4) return;
    if (g_pairedCamera && IsReadableRange(g_pairedCamera + 0x94, sizeof(float) * 16)) {
        const auto* world = reinterpret_cast<const float*>(g_pairedCamera + 0x94);
        if (std::abs(world[12] - g_pairedExpected[0]) < 0.0005f &&
            std::abs(world[13] - g_pairedExpected[1]) < 0.0005f &&
            std::abs(world[14] - g_pairedExpected[2]) < 0.0005f) return;
    }
    ++g_pairedAppliesThisFrame;
    if (!SetNativeCameraOffsetForCameras(g_pairedOffset, 1u, false)) return;
    constexpr std::uintptr_t kOwnerGlobalRva = 0x02ED0E20;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* owner = nullptr;
    std::memcpy(&owner, reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner));
    std::byte* camera = nullptr;
    if (!owner || !IsReadableRange(static_cast<const std::byte*>(owner) + 0x5C0, sizeof(camera))) return;
    std::memcpy(&camera, static_cast<const std::byte*>(owner) + 0x5C0, sizeof(camera));
    if (!HasNativeCameraSignature(camera) || !IsWritableRange(camera + 0x154, sizeof(float) * 16)) return;
    auto* projection = reinterpret_cast<float*>(camera + 0x154);
    const auto& eye = g_pairedProjection;
    if (g_nativeCameraProjectionSyncEnabled.load() && eye.valid && std::abs(projection[11] + 1.0f) < 0.001f &&
        std::isfinite(eye.m00) && std::isfinite(eye.m11) && std::isfinite(eye.m20) && std::isfinite(eye.m21)) {
        projection[0] = eye.m00;
        projection[5] = eye.m11;
        projection[8] = eye.m20;
        projection[9] = eye.m21;
    }
    if (!RebuildNativeViewProjection(camera)) return;
    const auto* world = reinterpret_cast<const float*>(camera + 0x94);
    g_pairedCamera = camera;
    g_pairedExpected = {world[12], world[13], world[14]};
    const auto applies = ++g_pairedUploadApplies;
    if (applies == 1 || applies % 600 == 0)
        TraceRuntime("paired camera owner+0x5c0 re-synced at Globals upload: eye=" + std::to_string(eye.eye) +
            " m20=" + std::to_string(projection[8]) + " applies=" + std::to_string(applies));
}
}

bool SyncNativeCameraProjectionToEye(const NativeEyeProjection& eye) {
    if (!g_nativeCameraProjectionSyncEnabled.load() || !eye.valid ||
        !std::isfinite(eye.m00) || !std::isfinite(eye.m11) ||
        !std::isfinite(eye.m20) || !std::isfinite(eye.m21) ||
        eye.m00 < 0.2f || eye.m00 > 5.0f || eye.m11 < 0.2f || eye.m11 > 5.0f ||
        std::abs(eye.m20) > 1.0f || std::abs(eye.m21) > 1.0f) return false;
    // Each guard names its rejection so a silent no-op is distinguishable
    // from a successful write in the live trace (first hit, then every 600).
    const auto reject = [](std::size_t code, const char* reason) {
        const auto count = ++g_projectionSyncRejects[code];
        if (count == 1 || count % 600 == 0)
            TraceRuntime(std::string("projection-camera eye frustum sync rejected: ") + reason + " count=" + std::to_string(count));
        return false;
    };
    constexpr std::uintptr_t kOwnerGlobalRva = 0x02ED0E20;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* owner = nullptr;
    if (!imageBase || !IsReadableRange(reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner))) return reject(0, "owner-global");
    std::memcpy(&owner, reinterpret_cast<const void*>(imageBase + kOwnerGlobalRva), sizeof(owner));
    void* cameraPointer = nullptr;
    if (!owner || !IsReadableRange(static_cast<const std::byte*>(owner) + 0x5C8, sizeof(cameraPointer))) return reject(1, "owner-null");
    std::memcpy(&cameraPointer, static_cast<const std::byte*>(owner) + 0x5C8, sizeof(cameraPointer));
    auto* camera = static_cast<std::byte*>(cameraPointer);
    if (!HasNativeCameraSignature(camera) ||
        !IsWritableRange(camera + 0x154, sizeof(float) * 16) ||
        !IsWritableRange(camera + 0x194, sizeof(float) * 16) ||
        !IsWritableRange(camera + 0x1D4, sizeof(float) * 16)) return reject(2, "camera-signature-or-writable");
    std::lock_guard lock(g_nativeCameraOffsetMutex);
    const auto* world = reinterpret_cast<const float*>(camera + 0x94);
    g_projectionCameraWorld.store(world);
    auto* projection = reinterpret_cast<float*>(camera + 0x154);
    auto* viewProjection = reinterpret_cast<float*>(camera + 0x194);
    auto* projectionCopy = reinterpret_cast<float*>(camera + 0x1D4);
    if (!IsNativeWorldTransform(world) ||
        std::abs(projection[11] + 1.0f) > 0.001f ||
        std::abs(projection[14] - 0.1f) > 0.001f) return reject(3, "world-or-depth-convention");
    // The engine sometimes resets P (+0x154) but not its duplicate (+0x1D4),
    // which still holds our previous eye frustum. Outside the x/y frustum
    // terms (m00, m11, m20, m21) the two must still agree; the frustum terms
    // are exactly what this function writes into both.
    for (int index = 0; index != 16; ++index) {
        if (!std::isfinite(projection[index]) || !std::isfinite(projectionCopy[index])) return reject(4, "p-duplicate-not-finite");
        if (index == 0 || index == 5 || index == 8 || index == 9) continue;
        if (std::abs(projection[index] - projectionCopy[index]) > 0.005f) return reject(4, "p-duplicate-mismatch");
    }
    // Prove that this is still the already validated projection-camera pair
    // before changing either record. A menu/default or transient engine write
    // that breaks P/VP coherence is rejected without touching game memory.
    for (int row = 0; row != 3; ++row)
        for (int column = 0; column != 4; ++column) {
            double expected = 0.0;
            for (int axis = 0; axis != 3; ++axis)
                expected += static_cast<double>(world[axis * 4 + row]) * projection[axis * 4 + column];
            if (!std::isfinite(expected) || !std::isfinite(viewProjection[row * 4 + column]) ||
                std::abs(expected - viewProjection[row * 4 + column]) > 0.025) reject(5, "old-vp-rotation-mismatch (VP rebuilt from world and eye P)");
        }
    const float oldM00 = projection[0];
    const float oldM11 = projection[5];
    const float oldM20 = projection[8];
    const float terms[4]{eye.m00, eye.m11, eye.m20, eye.m21};
    constexpr int indices[4]{0, 5, 8, 9};
    std::array<float, 16> candidateProjection{};
    std::memcpy(candidateProjection.data(), projection, sizeof(candidateProjection));
    for (int index = 0; index != 4; ++index) candidateProjection[indices[index]] = terms[index];
    double viewTranslation[3]{};
    for (int axis = 0; axis != 3; ++axis)
        for (int component = 0; component != 3; ++component)
            viewTranslation[axis] -= static_cast<double>(world[axis * 4 + component]) * world[12 + component];
    float newVp[16]{};
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column) {
            double value = row == 3 ? candidateProjection[12 + column] : 0.0;
            for (int axis = 0; axis != 3; ++axis)
                value += (row == 3 ? viewTranslation[axis] : static_cast<double>(world[axis * 4 + row])) * candidateProjection[axis * 4 + column];
            if (!std::isfinite(value)) return reject(6, "non-finite-candidate");
            newVp[row * 4 + column] = static_cast<float>(value);
        }
    std::memcpy(projection, candidateProjection.data(), sizeof(candidateProjection));
    std::memcpy(projectionCopy, candidateProjection.data(), sizeof(candidateProjection));
    std::memcpy(viewProjection, newVp, sizeof(newVp));
    if (eye.eye < 2) {
        std::lock_guard expectedLock(g_expectedEyeMutex);
        auto& expected = g_expectedEyeUploads[eye.eye];
        expected.valid = true;
        std::memcpy(expected.rows.data(), newVp, sizeof(float) * 12);
        expected.m00 = eye.m00;
        expected.m11 = eye.m11;
        expected.m20 = eye.m20;
        expected.m21 = eye.m21;
    }
    const auto writes = ++g_nativeCameraProjectionSyncWrites;
    if (writes == 1 || writes % 600 == 0)
        TraceRuntime("projection-camera eye frustum aligned: eye=" + std::to_string(eye.eye) +
            " old=[" + std::to_string(oldM00) + "," + std::to_string(oldM11) + "," + std::to_string(oldM20) +
            "] new=[" + std::to_string(eye.m00) + "," + std::to_string(eye.m11) + "," + std::to_string(eye.m20) +
            "] writes=" + std::to_string(writes));
    return true;
}

bool HasNativeGameplayCamera() {
    constexpr std::uintptr_t kCameraOwnerGlobalRva = 0x02ED0E20;
    const auto imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* owner = nullptr;
    if (!imageBase || !IsReadableRange(reinterpret_cast<const void*>(imageBase + kCameraOwnerGlobalRva), sizeof(owner))) return false;
    std::memcpy(&owner, reinterpret_cast<const void*>(imageBase + kCameraOwnerGlobalRva), sizeof(owner));
    if (!owner || !IsReadableRange(static_cast<const std::byte*>(owner) + 0x5C8, sizeof(void*))) return false;
    void* camera = nullptr;
    std::memcpy(&camera, static_cast<const std::byte*>(owner) + 0x5C8, sizeof(camera));
    const auto* bytes = static_cast<const std::byte*>(camera);
    if (!HasNativeCameraSignature(bytes)) return false;
    const auto* transform = reinterpret_cast<const float*>(bytes + 0x94);
    // The observed front-end record is identity near (0,0,0); the live player
    // record had a rotated basis and kilometre-scale world coordinates. This
    // threshold only decides when to capture a neutral XR pose, never where
    // the JC3 camera is placed.
    return std::abs(transform[12]) + std::abs(transform[13]) + std::abs(transform[14]) > 10.0f;
}

std::uint32_t GetActiveNativeEye() { return g_nativeEye.load(); }
std::uint64_t GetNativeProjectionWriteCount() { return g_nativeProjectionWrites.load(); }
}
