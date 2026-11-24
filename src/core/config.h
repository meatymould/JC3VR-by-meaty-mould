#pragma once
#include "game/rico_hand_pose.h"
#include <filesystem>
#include <string>

namespace jc3vr {
enum class CameraMode { CurrentWorking, FirstPerson };
enum class StereoMode { MonoDebug, Geo11, ExternalSbs, NativeFull, NativeAlternate };
enum class Geo11StereoSource { ProperSbs, LeftToBoth, RightToBoth };
enum class NativeStereoRigMode { Center, LeftToBoth, RightToBoth, NativeStereo, SwapEyes };

struct Config {
    bool enabled = true;
    bool allowUnknownBuild = false;  // development only: run on an unverified JC3 executable
    bool debugLogging = true;
    bool headTracking = true;
    // head_look_mode=direct_camera writes HMD orientation into the native
    // camera records instead of sending relative mouse input.
    bool directHeadRotation = false;
    bool positionalTracking = false;
    float worldScale = 1.0f;
    float yawScale = 1.0f;
    float pitchScale = 1.0f;
    float cameraShakeScale = 0.25f;
    CameraMode cameraMode = CameraMode::CurrentWorking;
    StereoMode stereoMode = StereoMode::MonoDebug;
    // Optional desktop-only fixed-eye mirror. Invalid/off keeps original output.
    std::wstring creatorMirrorEye = L"off";
    bool showBody = true;
    bool showHeadMesh = true;
    float nearClip = 0.05f;
    // Diagnostic only: maps the Geo-11 SBS halves to the opposite OpenXR eye.
    // It is off by default because Geo-11's documented normal SBS order is used.
    bool geo11SwapEyes = false;
    // Temporary calibration controls. They only choose/cross-map the already
    // rendered Geo SBS source; they never alter JC3's camera or add an IPD.
    Geo11StereoSource geo11StereoSource = Geo11StereoSource::ProperSbs;
    bool geo11LogExtractionEveryFrame = true;
    // The sole OpenXR-metres to JC3-camera-units conversion.  It remains zero
    // until a measured JC3-world reference establishes the value.
    float jc3WorldUnitsPerMeter = 0.0f;
    NativeStereoRigMode nativeStereoRigMode = NativeStereoRigMode::Center;
    // Visual-integrity A/B switch.  Disabled keeps JC3's original projection
    // intact while the final image path is being validated.
    bool nativeProjectionOverride = false;
    bool forceNativeProjectionBuffer = false;
    bool syncNativeViewProjection = false;
    // Experimental: keep the projection camera's own P/VP records in the
    // same asymmetric eye frustum as the final D3D11 projection buffer.
    bool syncNativeProjectionToXr = false;
    // Never installs automatically. When explicitly enabled, this only permits
    // the one-shot F4 arm after the player is already in a stable world scene.
    bool nativeCameraProbeEnabled = false;
    bool sixDof = false;
    float sixDofScale = 1.0f;
    float sixDofLimitMeters = 0.65f;
    int recenterKey = 0x71; // VK_F2
    bool motionControllers = false;
    bool handDebugPreview = false;
    bool debugOverlay = false;
    bool shadowProbe = false;
    // One bounded GPU readback of the two retained native eye sources for
    // offline pixel/ownership analysis. Disabled in normal builds.
    bool nativeEyeCapture = false;
    // Bounded, read-only draw-time readback of the bound 784-byte VS slot-0
    // block plus native camera records. Diagnostic only; off by default.
    bool stereoCoherenceProbe = false;
    // Re-apply head/eye camera state to the paired owner+0x5C0 record when
    // JC3 maps the 784-byte Globals block (the engine refreshes that record
    // after Present, so a Present-time write rarely reaches its consumers).
    bool syncPairedCameraAtUpload = false;
    bool cameraTimelineProbe = false;
    bool syncNativeCullingCamera = false;
    int cullingSyncParts = 7;
    // Diagnostic: fixed extra head yaw in direct mode (0 = off).
    float debugHeadYawDegrees = 0.0f;
    float debugHeadYawWobbleDegrees = 0.0f;
    // JC3 swapchain/render size override for the headset (0 = off).
    int renderWidth = 0;
    // OpenXR controllers drive a virtual XInput pad (needs motion_controllers).
    bool vrGamepad = false;
    bool controllerHaptics = true;
    float hapticIntensity = 0.7f;
    // Right-hand pointing drives JC3's aim camera; right stick snap-turns.
    bool handAim = false;
    // Steer JC3's own camera toward the headset so culling follows the head.
    bool engineFollowHead = false;
    // Submit left/right only as freshly rendered pairs (F9 toggles live).
    bool pairedEyeSubmission = false;
    bool aimFollowsRightHand = false;
    bool controllerModel = true;
    bool trackedHands = false; // late-rendered articulated hand rig, opt-in
    bool aimLines = false;
    // Read-only grapple vtable call probe (RE; off by default).
    bool grappleProbe = false;
    // One-shot read-only search for copies of the engine camera rotation.
    bool cameraSourceScan = false;
    // Visibility-camera A/B (F5 cycles candidate groups).
    bool cullOverrideTest = false;
    bool cullOverrideAuto = false;
    float lodProjectionScale = 1.0f;
    bool lodScaleTest = false;
    int cullingMode = 2;
    bool cullingModeTest = false;
    bool fovSourceScan = false;
    bool skeletonProbe = false;
    // Rico's arms follow the controllers (render-side IK on the skin palette).
    bool vrBody = false;
    bool vrBodyDebugPose = false;
    // Collapse Rico's head at the neck (for a full-body model in first person).
    bool vrBodyHideHead = false;
    // Held objects (the gun) follow the IK hands.
    bool vrBodyCarry = true;
    // Hide Rico's whole body; only the controllers and the carried gun show.
    bool vrBodyHideAll = false;
    bool ricoNativeHands = false;
    bool ricoPocketHolster = false;
    bool nativeGrabProbe = false;
    float nativeGrabReachMeters = 0.15f;
    std::array<RicoHandTuning, 2> ricoHandTuning{};
    // Widen JC3's own visibility culls to the both-eye union (engine_culling).
    bool widenTerrainCull = true;
    bool relaxTerrainHullCulls = true;
    bool widenSceneCull = true;
    bool disableSceneOcclusion = true;
    float cullFovPadding = 0.4f;
    float cullSizeFovDegrees = 65.0f;
    bool widenModelCull = true;
    bool cullFromHead = true;
    // Exact off-axis clip-to-view inverse for JC3's screen-space lighting passes.
    bool offAxisReconstruction = true;
    // Bullets go along the right controller's ray (vr_weapons).
    bool aimFromController = true;
    // Rico's hand bones placed on the controllers in the game's pose (vr_weapons).
    bool gunsToController = true;  // the game's gun moved to the drawn one (vr_weapons)
    bool weaponControllerRoll = true;
    bool nativeAttachmentRenderRoot = true;
    bool handsInGame = false;  // user 2026-10-01: "the new one is horrible"; render-side carry is better
    bool shadowsEveryFrame = true;
    bool shadowFitUnion = true;
    bool shadowFitHeading = true;
    // Orient Rico's hands from the controller aim pose (true) or grip pose.
    bool vrBodyHandAimFrame = true;
    // Re-place the screen-space HUD per eye at a fixed distance.
    bool hudAlign = true;
    bool wristHud = true; // additive alpha prototype; native HUD remains available
    bool vrSuppressDamageEffects = true; // presentation only, never changes health
    float hudDistanceMeters = 3.0f;
    float hudScale = 0.65f;
    float menuDistanceMeters = 4.0f;  // map / full-screen menu screen
    float menuScale = 0.55f;
    // Rotate each retained eye image to this frame's pose before submission.
    bool reprojectToFramePose = false;
    float handAimGain = 0.5f;
    float snapTurnDegrees = 30.0f;
    bool helicopterHeadSteering = true;
    bool helicopterPullHijack = false; // experimental gesture -> native entry request
    bool helicopterHijackTrace = false; // bounded read-only F3 phase capture
    bool physicalHelicopterHijack = false; // experimental native door-stage grip/pull
    float sixDofVerticalLimitMeters = 1.8f;
    bool debugHandAim = false;
    float debugHandAimYawDegrees = 0.0f;
    float debugHandAimPitchDegrees = 0.0f;
    int renderHeight = 0;
};
Config LoadConfig(const std::filesystem::path& path);
std::string ToString(CameraMode mode);
std::string ToString(StereoMode mode);
std::string ToString(Geo11StereoSource source);
std::string ToString(NativeStereoRigMode mode);
}
