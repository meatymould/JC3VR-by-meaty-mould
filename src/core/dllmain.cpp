#include "core/build_fingerprint.h"
#include "core/resource_crash_trace.h"
#include <cctype>
#include "core/config.h"
#include "core/logging.h"
#include "game/camera_capture.h"
#include "game/head_look.h"
#include "game/vr_gamepad.h"
#include "openxr/xr_instance.h"
#include "openxr/xr_session.h"
#include "game/grapple_probe.h"
#include "game/camera_source_scan.h"
#include "game/vr_body_rig.h"
#include "game/engine_culling.h"
#include "game/engine_lighting.h"
#include "game/vr_weapons.h"
#include "game/native_grab_probe.h"
#include "game/hijack_state_probe.h"
#include "game/physical_hijack.h"
#include "game/render_character_root.h"
#include "game/hud_snapshot.h"
#include "render/dx11_hook.h"
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>

extern "C" __declspec(dllexport) void WINAPI JC3VR_Initialize();
extern "C" __declspec(dllexport) void WINAPI JC3VR_ArmEarlyGraphicsHooks();

namespace {
INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_earlyInitOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_startOnce = INIT_ONCE_STATIC_INIT;

void BootstrapTrace(const char* event, DWORD error = ERROR_SUCCESS) {
    char message[512]{};
    const int count = snprintf(message, sizeof(message), "JC3VR bootstrap: %s (last_error=%lu)\r\n", event, static_cast<unsigned long>(error));
    OutputDebugStringA(message);
    char base[MAX_PATH]{};
    const DWORD baseLength = GetEnvironmentVariableA("LOCALAPPDATA", base, static_cast<DWORD>(std::size(base)));
    if (!baseLength || baseLength >= std::size(base)) return;
    const std::string directory = std::string(base) + "\\JC3VR";
    CreateDirectoryA(directory.c_str(), nullptr);
    const std::string path = directory + "\\bootstrap_worker.log";
    HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(count), &written, nullptr);
    CloseHandle(file);
}

std::filesystem::path ExecutablePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}

// The one JC3 build every engine address in JC3VR was found and verified on
// (docs/BUILD_FINGERPRINTS.md). Anything else fails closed: no hooks, no
// memory writes, the game runs as if JC3VR were not installed.
constexpr char kSupportedJc3Sha256[] = "a53c2d6b2837f45d97244fc096e68f27ba9036e0582a0d9aada2e2f222dac293";

// Decided once per process: enabled=0 in jc3vr.ini switches JC3VR off (the
// emergency off switch), and an unknown executable switches it off unless
// allow_unknown_build=1 (for development only).
bool ModAllowedToRun(const jc3vr::Config& config, const std::filesystem::path& executable) {
    static const bool allowed = [&] {
        if (!config.enabled) {
            jc3vr::Log("JC3VR disabled: enabled=0 in jc3vr.ini; no hooks installed");
            return false;
        }
        std::string error;
        const auto fingerprint = jc3vr::FingerprintExecutable(executable, error);
        std::string sha = fingerprint ? fingerprint->sha256 : std::string();
        for (auto& c : sha) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (sha == kSupportedJc3Sha256) return true;
        if (config.allowUnknownBuild) {
            jc3vr::Log("WARNING: unsupported JC3 build, running anyway because allow_unknown_build=1 (sha256=" + sha + ")");
            return true;
        }
        jc3vr::Log("JC3VR disabled: unsupported JC3 build (sha256=" + (sha.empty() ? "unavailable: " + error : sha) +
            ", supported " + kSupportedJc3Sha256 + "); no hooks installed, the game runs unmodified");
        return false;
    }();
    return allowed;
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
    jc3vr::InitializeLogging();
    jc3vr::Log("JC3VR 0.0.1-prealpha.1 initialization requested by dinput8 proxy");
    const auto executable = ExecutablePath();
    jc3vr::Log("host executable: " + executable.string());
    std::string error;
    if (const auto fingerprint = jc3vr::FingerprintExecutable(executable, error)) jc3vr::Log("build fingerprint: " + fingerprint->ToString());
    else jc3vr::Log("build fingerprint unavailable: " + error);
    const auto config = jc3vr::LoadConfig(executable.parent_path() / L"jc3vr.ini");
    if (!ModAllowedToRun(config, executable)) return TRUE;
    jc3vr::Log(std::string("configuration: enabled=") + (config.enabled ? "true" : "false") + " head_tracking=" + (config.headTracking ? "true" : "false") + " camera_mode=" + jc3vr::ToString(config.cameraMode) + " stereo_mode=" + jc3vr::ToString(config.stereoMode) + " stereo_source=" + jc3vr::ToString(config.geo11StereoSource) + " geo11_swap_eyes=" + (config.geo11SwapEyes ? "true" : "false") + " native_camera_probe_enabled=" + (config.nativeCameraProbeEnabled ? "true" : "false"));
    jc3vr::ConfigureHeadLook(config);
    jc3vr::ConfigureOpenXrStereoMode(config.stereoMode, config.geo11SwapEyes, config.geo11StereoSource, config.geo11LogExtractionEveryFrame, config.nativeStereoRigMode, config.jc3WorldUnitsPerMeter, config.sixDof, config.sixDofScale, config.sixDofLimitMeters, config.recenterKey, config.forceNativeProjectionBuffer, config.nativeEyeCapture, config.motionControllers, config.handDebugPreview);
    jc3vr::ConfigureDx11StereoMode(config.stereoMode);
    jc3vr::ConfigureCreatorMirror(config.creatorMirrorEye);
    jc3vr::ConfigureRenderResolution(static_cast<unsigned>(config.renderWidth), static_cast<unsigned>(config.renderHeight));
    jc3vr::ConfigureVrGamepad(config.vrGamepad && config.motionControllers);
    jc3vr::ConfigureVrHaptics(config.controllerHaptics,config.hapticIntensity);
    jc3vr::ConfigureVrGamepadActions(config.vrGamepad && config.motionControllers);
    jc3vr::ConfigurePairedEyeSubmission(config.pairedEyeSubmission);
    jc3vr::ConfigureLodScale(config.lodProjectionScale, config.lodScaleTest);
    jc3vr::ConfigureCullingMode(config.cullingMode, config.cullOverrideTest, config.cullingModeTest);
    jc3vr::ConfigureAimAndControllers(config.aimFollowsRightHand, config.controllerModel, config.aimLines);
    jc3vr::ConfigureTrackedHands(config.trackedHands);
    jc3vr::ConfigureGrappleProbe(config.grappleProbe);
    jc3vr::ConfigureCameraSourceScan(config.cameraSourceScan);
    jc3vr::ConfigureFovSourceScan(config.fovSourceScan);
    jc3vr::ConfigureSkeletonProbe(config.skeletonProbe);
    jc3vr::ConfigureVrBody(config.vrBody, config.vrBodyDebugPose, config.vrBodyHideHead);
    jc3vr::ConfigureVrBodyCarry(config.vrBodyCarry);
    jc3vr::ConfigureVrBodyHideAll(config.vrBodyHideAll);
    jc3vr::ConfigureRicoNativeHands(config.ricoNativeHands && config.vrBody && config.vrBodyHideAll &&
        config.motionControllers && !config.vrBodyDebugPose, executable.parent_path(), config.ricoHandTuning);
    jc3vr::ConfigureEngineCulling({config.widenTerrainCull, config.relaxTerrainHullCulls, config.widenSceneCull, config.disableSceneOcclusion, config.cullFovPadding, config.cullSizeFovDegrees, config.widenModelCull, config.cullFromHead});
    jc3vr::ConfigureEngineLighting({config.offAxisReconstruction, config.shadowsEveryFrame, config.shadowFitUnion, config.shadowFitHeading});
    jc3vr::ConfigureVrBodyHandFrame(config.vrBodyHandAimFrame);
    jc3vr::ConfigureCullOverrideTest(config.cullOverrideTest, config.cullOverrideAuto);
    jc3vr::ConfigureHudPlacement(config.hudAlign, config.hudDistanceMeters, config.hudScale);
    jc3vr::ConfigureWristHud(config.wristHud);
    jc3vr::ConfigureMenuScreen(config.menuDistanceMeters, config.menuScale);
    jc3vr::ConfigureReprojectToFramePose(config.reprojectToFramePose);
    jc3vr::ConfigureEngineCameraFollowHead(config.engineFollowHead && config.directHeadRotation);
    jc3vr::ConfigureHandAim(config.handAim && config.motionControllers && config.directHeadRotation, config.handAimGain, config.snapTurnDegrees, config.debugHandAim, config.debugHandAimYawDegrees, config.debugHandAimPitchDegrees);
    jc3vr::InstallVrGamepadHook();
    jc3vr::InstallEngineCullingHooks();
    jc3vr::InstallEngineLightingHooks();
    jc3vr::InstallRenderCharacterRoot(config.nativeAttachmentRenderRoot);
    jc3vr::ConfigureVrWeapons({config.aimFromController, config.handsInGame, config.gunsToController, config.weaponControllerRoll});
    jc3vr::ConfigureRicoPocketHolster(config.ricoPocketHolster && jc3vr::RicoNativeHandsEnabled());
    jc3vr::ConfigureNativeGrabProbe(config.nativeGrabProbe, config.nativeGrabReachMeters, config.jc3WorldUnitsPerMeter);
    jc3vr::InstallVrWeaponHooks();
    if(config.wristHud) jc3vr::InstallHudSnapshotHook();
    jc3vr::ConfigureVrDamageSuppression(config.vrSuppressDamageEffects);
    jc3vr::InstallVrDamageHooks();
    jc3vr::ConfigureNativeEyeProjection(config.nativeProjectionOverride && (config.stereoMode == jc3vr::StereoMode::NativeAlternate || config.stereoMode == jc3vr::StereoMode::NativeFull));
    jc3vr::ConfigureNativeCameraProbe(config.nativeCameraProbeEnabled);
    jc3vr::ConfigureShadowProbe(config.shadowProbe);
    jc3vr::ConfigureNativeViewProjectionSync(config.syncNativeViewProjection);
    jc3vr::ConfigureNativeCameraProjectionSync(config.syncNativeProjectionToXr);
    jc3vr::ConfigureStereoCoherenceProbe(config.stereoCoherenceProbe);
    jc3vr::ConfigurePairedCameraUploadSync(config.syncPairedCameraAtUpload);
    jc3vr::ConfigureCameraTimelineProbe(config.cameraTimelineProbe);
    jc3vr::ConfigureCullingCameraSync(config.syncNativeCullingCamera, static_cast<unsigned>(config.cullingSyncParts));
    jc3vr::ConfigureDirectHeadRotation(config.headTracking && config.directHeadRotation, config.debugHeadYawDegrees, config.debugHeadYawWobbleDegrees);
    jc3vr::ConfigureHelicopterHeadSteering(config.helicopterHeadSteering);
    jc3vr::ConfigureHelicopterPullHijack(config.helicopterPullHijack);
    jc3vr::ConfigureHijackStateProbe(config.helicopterHijackTrace);
    jc3vr::ConfigurePhysicalHijack(config.physicalHelicopterHijack);
    jc3vr::ConfigureVerticalTrackingLimit(config.sixDofVerticalLimitMeters);
    const auto xr = jc3vr::ProbeOpenXrLoader();
    jc3vr::Log("OpenXR loader: " + xr.detail);
    // Early graphics hooks may already be creating the live-device session.
    // A disposable instance here competes with that session (some runtimes
    // permit only one instance per process) and bypasses its lifecycle mutex.
    // Loader availability is sufficient: Present owns runtime creation/retry.
    BootstrapTrace("OpenXR instance creation delegated to live graphics session", ERROR_SUCCESS);
#if JC3VR_HAVE_MINHOOK
    jc3vr::InstallDx11Hooks();
#endif
    return TRUE;
}

BOOL CALLBACK ArmEarlyGraphicsHooks(PINIT_ONCE, PVOID, PVOID*) {
    jc3vr::InitializeLogging();
    const auto executable = ExecutablePath();
    const auto config = jc3vr::LoadConfig(executable.parent_path() / L"jc3vr.ini");
    if (!ModAllowedToRun(config, executable)) return TRUE;
    jc3vr::InstallResourceCrashTrace();
    jc3vr::ConfigureHeadLook(config);
    jc3vr::ConfigureOpenXrStereoMode(config.stereoMode, config.geo11SwapEyes, config.geo11StereoSource, config.geo11LogExtractionEveryFrame, config.nativeStereoRigMode, config.jc3WorldUnitsPerMeter, config.sixDof, config.sixDofScale, config.sixDofLimitMeters, config.recenterKey, config.forceNativeProjectionBuffer, config.nativeEyeCapture, config.motionControllers, config.handDebugPreview);
    jc3vr::ConfigureDx11StereoMode(config.stereoMode);
    jc3vr::ConfigureCreatorMirror(config.creatorMirrorEye);
    jc3vr::ConfigureRenderResolution(static_cast<unsigned>(config.renderWidth), static_cast<unsigned>(config.renderHeight));
    jc3vr::ConfigureVrGamepad(config.vrGamepad && config.motionControllers);
    jc3vr::ConfigureVrHaptics(config.controllerHaptics,config.hapticIntensity);
    jc3vr::ConfigureVrGamepadActions(config.vrGamepad && config.motionControllers);
    jc3vr::ConfigurePairedEyeSubmission(config.pairedEyeSubmission);
    jc3vr::ConfigureLodScale(config.lodProjectionScale, config.lodScaleTest);
    jc3vr::ConfigureCullingMode(config.cullingMode, config.cullOverrideTest, config.cullingModeTest);
    jc3vr::ConfigureAimAndControllers(config.aimFollowsRightHand, config.controllerModel, config.aimLines);
    jc3vr::ConfigureTrackedHands(config.trackedHands);
    jc3vr::ConfigureGrappleProbe(config.grappleProbe);
    jc3vr::ConfigureCameraSourceScan(config.cameraSourceScan);
    jc3vr::ConfigureFovSourceScan(config.fovSourceScan);
    jc3vr::ConfigureSkeletonProbe(config.skeletonProbe);
    jc3vr::ConfigureVrBody(config.vrBody, config.vrBodyDebugPose, config.vrBodyHideHead);
    jc3vr::ConfigureVrBodyCarry(config.vrBodyCarry);
    jc3vr::ConfigureVrBodyHideAll(config.vrBodyHideAll);
    jc3vr::ConfigureRicoNativeHands(config.ricoNativeHands && config.vrBody && config.vrBodyHideAll &&
        config.motionControllers && !config.vrBodyDebugPose, executable.parent_path(), config.ricoHandTuning);
    jc3vr::ConfigureEngineCulling({config.widenTerrainCull, config.relaxTerrainHullCulls, config.widenSceneCull, config.disableSceneOcclusion, config.cullFovPadding, config.cullSizeFovDegrees, config.widenModelCull, config.cullFromHead});
    jc3vr::ConfigureEngineLighting({config.offAxisReconstruction, config.shadowsEveryFrame, config.shadowFitUnion, config.shadowFitHeading});
    jc3vr::ConfigureVrBodyHandFrame(config.vrBodyHandAimFrame);
    jc3vr::ConfigureCullOverrideTest(config.cullOverrideTest, config.cullOverrideAuto);
    jc3vr::ConfigureHudPlacement(config.hudAlign, config.hudDistanceMeters, config.hudScale);
    jc3vr::ConfigureWristHud(config.wristHud);
    jc3vr::ConfigureMenuScreen(config.menuDistanceMeters, config.menuScale);
    jc3vr::ConfigureReprojectToFramePose(config.reprojectToFramePose);
    jc3vr::ConfigureEngineCameraFollowHead(config.engineFollowHead && config.directHeadRotation);
    jc3vr::ConfigureHandAim(config.handAim && config.motionControllers && config.directHeadRotation, config.handAimGain, config.snapTurnDegrees, config.debugHandAim, config.debugHandAimYawDegrees, config.debugHandAimPitchDegrees);
    jc3vr::ConfigureVrWeapons({config.aimFromController, config.handsInGame, config.gunsToController, config.weaponControllerRoll});
    jc3vr::ConfigureRicoPocketHolster(config.ricoPocketHolster && jc3vr::RicoNativeHandsEnabled());
    jc3vr::ConfigureNativeGrabProbe(config.nativeGrabProbe, config.nativeGrabReachMeters, config.jc3WorldUnitsPerMeter);
    jc3vr::ConfigureNativeEyeProjection(config.nativeProjectionOverride && (config.stereoMode == jc3vr::StereoMode::NativeAlternate || config.stereoMode == jc3vr::StereoMode::NativeFull));
    jc3vr::ConfigureNativeCameraProbe(config.nativeCameraProbeEnabled);
    jc3vr::ConfigureShadowProbe(config.shadowProbe);
    jc3vr::ConfigureNativeViewProjectionSync(config.syncNativeViewProjection);
    jc3vr::ConfigureNativeCameraProjectionSync(config.syncNativeProjectionToXr);
    jc3vr::ConfigureStereoCoherenceProbe(config.stereoCoherenceProbe);
    jc3vr::ConfigurePairedCameraUploadSync(config.syncPairedCameraAtUpload);
    jc3vr::ConfigureCameraTimelineProbe(config.cameraTimelineProbe);
    jc3vr::ConfigureCullingCameraSync(config.syncNativeCullingCamera, static_cast<unsigned>(config.cullingSyncParts));
    jc3vr::ConfigureDirectHeadRotation(config.headTracking && config.directHeadRotation, config.debugHeadYawDegrees, config.debugHeadYawWobbleDegrees);
    jc3vr::ConfigureHelicopterHeadSteering(config.helicopterHeadSteering);
    jc3vr::ConfigureHelicopterPullHijack(config.helicopterPullHijack);
    jc3vr::ConfigureHijackStateProbe(config.helicopterHijackTrace);
    jc3vr::ConfigurePhysicalHijack(config.physicalHelicopterHijack);
    jc3vr::ConfigureVerticalTrackingLimit(config.sixDofVerticalLimitMeters);
#if JC3VR_HAVE_MINHOOK
    jc3vr::TraceRuntime("early graphics arm: installing DX11 before gameplay hooks");
    jc3vr::InstallDx11Hooks();
#endif
    jc3vr::InstallVrGamepadHook();
    jc3vr::InstallEngineCullingHooks();
    jc3vr::InstallEngineLightingHooks();
    jc3vr::InstallRenderCharacterRoot(config.nativeAttachmentRenderRoot);
    jc3vr::InstallVrWeaponHooks();
    if(config.wristHud) jc3vr::InstallHudSnapshotHook();
    jc3vr::ConfigureVrDamageSuppression(config.vrSuppressDamageEffects);
    jc3vr::InstallVrDamageHooks();
    return TRUE;
}

DWORD WINAPI BootstrapWorker(void*) {
    BootstrapTrace("worker entry");
    jc3vr::InitializeLogging();
    jc3vr::Log("JC3VR bootstrap worker entry");
    JC3VR_ArmEarlyGraphicsHooks();
    jc3vr::Log("JC3VR bootstrap worker: first graphics initialization call returned");
    JC3VR_Initialize();
#if JC3VR_HAVE_MINHOOK
    // Creation can race an asynchronous bootstrap. Native mode may recover
    // the system Present implementation even after the real device exists.
    for(unsigned attempt=0;attempt<6;++attempt) {
        if(jc3vr::ArmNativePresentFallback()) break;
        Sleep(250);
    }
#endif
    jc3vr::Log("JC3VR bootstrap worker complete");
    BootstrapTrace("worker complete");
    return 0;
}

BOOL CALLBACK StartBootstrapWorker(PINIT_ONCE, PVOID, PVOID*) {
    BootstrapTrace("JC3VR_Start callback entry");
    jc3vr::InitializeLogging();
    SetLastError(ERROR_SUCCESS);
    HANDLE thread = CreateThread(nullptr, 0, BootstrapWorker, nullptr, 0, nullptr);
    if (!thread) {
        BootstrapTrace("CreateThread(worker) failed", GetLastError());
        jc3vr::Log("JC3VR_Start failed to create bootstrap worker; GetLastError=" + std::to_string(GetLastError()));
        return FALSE;
    }
    BootstrapTrace("CreateThread(worker) succeeded");
    jc3vr::Log("JC3VR_Start created bootstrap worker handle=" + std::to_string(reinterpret_cast<std::uintptr_t>(thread)));
    CloseHandle(thread);
    return TRUE;
}
}

extern "C" __declspec(dllexport) BOOL WINAPI JC3VR_Start() {
    BootstrapTrace("JC3VR_Start invoked");
    const BOOL result = InitOnceExecuteOnce(&g_startOnce, StartBootstrapWorker, nullptr, nullptr);
    BootstrapTrace(result ? "JC3VR_Start returned success" : "JC3VR_Start returned failure", result ? ERROR_SUCCESS : GetLastError());
    return result;
}

extern "C" __declspec(dllexport) void WINAPI JC3VR_Initialize() {
    InitOnceExecuteOnce(&g_initOnce, Initialize, nullptr, nullptr);
}

extern "C" __declspec(dllexport) void WINAPI JC3VR_ArmEarlyGraphicsHooks() {
    InitOnceExecuteOnce(&g_earlyInitOnce, ArmEarlyGraphicsHooks, nullptr, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // No file I/O, graphics, or worker creation under loader lock.  This
        // debug trace is intentionally the only attach-side instrumentation.
        OutputDebugStringA("JC3VR: DLL_PROCESS_ATTACH\n");
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
