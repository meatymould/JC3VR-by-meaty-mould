// OpenXR session and the per-frame VR pipeline (Present thread).
//
// Map (see docs/ARCHITECTURE.md section 3 for the frame order):
//   runtime/session:  CreateRuntime, EnsureOpenXrSession (bounded retry, loader
//                     kept resident), InitializeHandActions, UpdateHandPoses
//   native stereo:    EnsureNativeAlternateResources, SubmitNativeAlternateFrame
//                     (retain the eye JC3 just rendered, identified by its
//                     projection; choose and write the next eye; submit),
//                     RenderNativeEyeToOpenXr (ray resample + frame-pose
//                     reprojection), ProjectionForView
//   aim/steering:     UpdateHandAim (engine camera follows head or hand aim via
//                     relative mouse, online-measured gain), snap turn
//   HUD:              ComposeHudForEye (HUD = pixels changed since the pre-HUD
//                     copy; fixed depth per eye; gun/grapple crosshairs)
//   controllers:      EnsureControllerModels, RenderHandMarkers
//   diagnostics:      CaptureRetainedEye/Pair (F3), LogStereoCalibration
//   legacy:           SubmitGeo11SbsFrame (Geo-11 SBS), mono quad path
#include "openxr/xr_session.h"
#include "openxr/xr_loader.h"
#include "core/config.h"
#include "core/logging.h"
#include "game/camera_capture.h"
#include "game/engine_culling.h"
#include "game/vr_weapons.h"
#include "game/vr_c4.h"
#include "game/vr_gamepad.h"
#include "game/pause_chord.h"
#include "game/helicopter_pull.h"
#include "game/physical_hijack.h"
#include "render/vr_hands.h"
#include "render/tracked_hand_rig.h"
#include "render/controller_model.h"
#include "render/weapon_selector.h"
#include "render/rifle_scope.h"
#include "render/wrist_hud.h"
#include "render/front_end_panel.h"
#include "render/hud_composite.h"
#include "game/hud_snapshot.h"
#include "game/support_grip.h"
#include "game/vehicle_camera.h"
#include "game/tracking_limits.h"
#include "game/dual_wield.h"
#include "game/grapple_probe.h"
#include "game/camera_source_scan.h"
#include "game/vr_body_rig.h"
#include "game/native_grab_probe.h"
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "render/creator_mirror.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <atomic>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include <map>
#include <filesystem>

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace jc3vr {
bool OpenXrResultRequiresReconnect(std::int32_t result) {
    return result == XR_SESSION_LOSS_PENDING || result == XR_ERROR_SESSION_LOST ||
        result == XR_ERROR_INSTANCE_LOST || result == XR_ERROR_SESSION_NOT_RUNNING || result == XR_ERROR_RUNTIME_FAILURE;
}
namespace {
std::mutex g_runtimeMutex;
bool g_runtimeReconnectPending = false;
XrResult ObserveRuntimeResult(XrResult result) {
    if (OpenXrResultRequiresReconnect(result)) {
        if (!g_runtimeReconnectPending)
            TraceRuntime("OpenXR disconnected; recreation scheduled result=" + std::to_string(result));
        g_runtimeReconnectPending = true;
    }
    // LOSS_PENDING is a positive success code, but further rendering must
    // stop until this session is rebuilt. Make existing failure gates stop.
    return result == XR_SESSION_LOSS_PENDING ? XR_ERROR_SESSION_LOST : result;
}
struct RuntimeState {
    struct GeoResampleResources {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputTarget;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencil;
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
    } geoResample;
    HMODULE loader = nullptr;
    PFN_xrGetInstanceProcAddr getProc = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrActionSet handActionSet = XR_NULL_HANDLE;
    XrAction handGripAction = XR_NULL_HANDLE;
    bool handActionsAttached = false;
    bool handDebugCaptureAttempted = false;
    std::array<XrPath, 2> handPaths{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2> handSpaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrSpaceLocation, 2> handLocations{{{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}}};
    std::array<bool, 2> handPoseActive{false, false};
    Microsoft::WRL::ComPtr<ID3D11VertexShader> handVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> handPixelShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> scopeVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> scopePixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> scopeConstants;
    bool scopeResourcesFailed=false;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> wristVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> wristPixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> wristConstants;
    bool wristResourcesFailed=false;
    HudSnapshot wristSnapshot;
    int wristWeaponIcon=-1;
    XrVector3f wristHeadCentre{};
    struct ScopeScene {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        uint64_t frame=0;
        std::uintptr_t weapon=0;
    };
    std::array<ScopeScene,2> scopeScenes,committedScopeScenes;
    std::array<uint64_t,2> committedEyeRetainedFrames{};
    Microsoft::WRL::ComPtr<ID3D11InputLayout> handInputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> handVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11BlendState> handBlend;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> iconVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> iconPixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> iconInputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> iconVertices;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> iconSampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState> iconBlend;
    std::map<int, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> iconTextures;
    std::map<int, float> iconAspects;
    // 3D hands: own depth buffer (sized to the eye target), no-cull raster
    // state, and per-hand finger curl from the controller inputs.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> handDepth;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> handDepthView;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> handDepthState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> handRaster;
    // Quest controller render models (GPU): per-hand vertex buffer + texture.
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 2> controllerVertices;
    std::array<UINT, 2> controllerVertexCount{};
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2> controllerTextures;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> controllerVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> controllerPixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> controllerInputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> controllerConstants;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> controllerSampler;
    bool controllerGpuReady = false;
    bool controllerGpuFailed = false;
    Microsoft::WRL::ComPtr<ID3D11Buffer> chargeVertices;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chargeTexture;
    UINT chargeVertexCount=0;
    bool chargeGpuFailed=false;
    float controllerGripSign = 0.0f;  // +/-1 once the rotate_xyz convention is measured
    UINT handDepthWidth = 0, handDepthHeight = 0;
    std::array<HandCurl, 2> handCurl{};
    PocketHolster pocketHolster{};
    // Per-eye request->retain latency histogram (0,1,2,3+ frames).
    std::array<uint64_t, 2> eyeRequestFrame{};
    std::array<std::array<uint64_t, 4>, 2> eyeLatency{};
    // Mean Present interval preceding each retained eye image (pacing).
    std::array<double, 2> eyeIntervalSum{};
    std::array<double, 2> eyeIntervalMax{};
    std::array<uint64_t, 2> eyeIntervalCount{};
    LARGE_INTEGER lastPresentCounter{};
    PFN_xrSyncActions syncActions = nullptr;
    PFN_xrGetActionStatePose getActionStatePose = nullptr;
    // Controller inputs for the virtual XInput pad (vr_gamepad=1).
    XrAction stickAction = XR_NULL_HANDLE, triggerAction = XR_NULL_HANDLE, squeezeAction = XR_NULL_HANDLE;
    XrAction stickClickAction = XR_NULL_HANDLE, aAction = XR_NULL_HANDLE, bAction = XR_NULL_HANDLE;
    XrAction xAction = XR_NULL_HANDLE, yAction = XR_NULL_HANDLE, menuAction = XR_NULL_HANDLE;
    // Pointing ray per hand (/input/aim/pose) for hand aim.
    XrAction handAimAction = XR_NULL_HANDLE;
    XrAction hapticAction = XR_NULL_HANDLE;
    PFN_xrApplyHapticFeedback applyHapticFeedback=nullptr;
    PFN_xrStopHapticFeedback stopHapticFeedback=nullptr;
    std::array<bool,2> hapticPlaying{};
    ULONGLONG hapticSentAt=0;
    unsigned hapticErrors=0;
    std::array<XrSpace, 2> handAimSpaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrPosef, 2> handAimPoses{};
    SupportGripState supportGrip;
    HelicopterPullState helicopterPull;
    bool supportFrameValid=false;
    float supportForward[3]{}, supportSocket[3]{};
    std::array<bool, 2> handAimActive{false, false};
    PFN_xrGetActionStateFloat getActionStateFloat = nullptr;
    PFN_xrGetActionStateBoolean getActionStateBoolean = nullptr;
    PFN_xrGetActionStateVector2f getActionStateVector2f = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
    bool sessionRunning = false;
    PauseChord pauseChord;
    bool focused = false;
    bool swapchainsReady = false;
    uint32_t width = 0, height = 0;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    bool frontEndPanelActive=false;
    DXGI_FORMAT frontEndFormat=DXGI_FORMAT_UNKNOWN;
    bool sbsSwapchainsReady = false;
    uint32_t recommendedEyeWidth = 0, recommendedEyeHeight = 0;
    uint32_t maximumEyeWidth = 0, maximumEyeHeight = 0;
    uint32_t geoSourceEyeWidth = 0, geoSourceHeight = 0;
    uint32_t sbsEyeWidth = 0, sbsEyeHeight = 0;
    std::array<XrSwapchain, 2> sbsSwapchains{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<std::vector<XrSwapchainImageD3D11KHR>, 2> sbsImages;
    bool nativeSwapchainsReady = false;
    DXGI_FORMAT nativeSwapchainFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t nativeSourceWidth = 0, nativeSourceHeight = 0;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> nativeEyeSources;
    // HUD re-placement: after-HUD copy, pre-HUD view, composite output.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hudAfter, hudOut;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hudAfterView, hudBeforeView;
    Microsoft::WRL::ComPtr<ID3D11BlendState> wristBlend;
    ID3D11Texture2D* hudBeforeTexture = nullptr;
    uint64_t wristNativeHudFrame=0; // completed matched before/after pair, shared by both eyes
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> hudOutTarget;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> hudVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> hudPixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> hudConstants;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> hudSampler;
    bool hudFailed = false;
    // Full-screen menu detection (map, menus): an occlusion query counts the
    // pixels JC3 drew after the world (the HUD layer). In play that is a few
    // percent; the map covers nearly the whole frame. Read a frame later.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> hudCoveragePixelShader, hudMenuPixelShader;
    Microsoft::WRL::ComPtr<ID3D11Query> hudCoverageQuery;
    Microsoft::WRL::ComPtr<ID3D11BlendState> hudNoColorWrite;
    bool hudCoveragePending = false;
    float hudCoverage = 0.0f;
    int hudMenuVotes = 0;
    bool hudMenuMode = false;
    std::array<bool, 2> nativeEyeReady{false, false};
    std::array<uint64_t, 2> nativeEyeRetainedAtFrame{};
    bool nativeEyeCaptureAttempted = false;
    std::string finalHandCaptureStem;
    uint64_t finalHandCaptureFrame = 0;
    std::array<bool,2> finalHandCapturePending{};
    std::array<NativeEyeProjection, 2> nativeEyeProjections{};
    std::array<std::uint64_t, 2> nativeEyeArmDrawCounts{};
    std::array<CameraProjectionSample, 2> nativeEyeSourceProjections{};
    struct NativeEyeCamera {
        uint32_t sourceView = 0;
        XrPosef pose{};
        XrFovf fov{};
        float cameraLocalX = 0.0f;
        NativeCameraOffset nativeOffset{};
    };
    std::array<NativeEyeCamera, 2> nativeEyeCameras{};
    std::array<NativeEyeCamera, 2> retainedEyeCameras{};
    std::array<NativeEyeProjection, 2> retainedEyeProjections{};
    // Paired submission: the headset only receives a new eye pair once both
    // eyes were rendered since the last commit, so both images change on the
    // same submitted frame (runtime motion estimation from one eye then
    // stays valid for the other). Committed copies are what gets submitted.
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> committedSources;
    std::array<NativeEyeCamera, 2> committedCameras{};
    std::array<NativeEyeProjection, 2> committedProjections{};
    std::array<CameraProjectionSample, 2> committedSourceProjections{};
    bool pairCommitted = false;
    uint64_t pairCommitFrame = 0;
    uint64_t pairCommits = 0;
    std::array<XrSwapchain, 2> nativeSwapchains{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<std::vector<XrSwapchainImageD3D11KHR>, 2> nativeImages;
    bool stereoCalibrationLogged = false;
    bool sourceCoverageLogged = false;
    uint64_t frameCount = 0;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerateFormats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireImage = nullptr;
    PFN_xrWaitSwapchainImage waitImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseImage = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
};

struct GeoResampleConstants {
    float sourceM00;
    float sourceM11;
    float sourceM20;
    float sourceM21;
    float tangentLeft;
    float tangentRight;
    float tangentUp;
    float tangentDown;
    float sourceScaleX;
    float sourceOffsetX;
    float padding0;
    float padding1;
    // Rows of source_view_from_target_view (float4-aligned); identity keeps
    // the plain tangent remap.
    float rotation[12]{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
};
RuntimeState g_runtime;
OpenXrHeadPose g_latestPose;
std::mutex g_poseMutex;
XrVector3f RotateBy(const XrQuaternionf& q, const XrVector3f& v);
StereoMode g_stereoMode = StereoMode::MonoDebug;
CreatorMirrorEye g_creatorMirrorEye = CreatorMirrorEye::Off;
bool g_geo11SwapEyes = false;
bool g_helicopterHeadSteering = true;
Geo11StereoSource g_geo11StereoSource = Geo11StereoSource::ProperSbs;
bool g_geo11LogExtractionEveryFrame = true;
NativeStereoRigMode g_nativeStereoRigMode = NativeStereoRigMode::Center;
float g_jc3WorldUnitsPerMeter = 0.0f;
bool g_sixDof = false;
float g_sixDofScale = 1.0f;
float g_sixDofLimitMeters = 0.65f;
float g_sixDofVerticalLimitMeters = 1.8f;
int g_recenterKey = VK_F2;
bool g_forceNativeProjectionBuffer = false;
bool g_nativeEyeCapture = false;
bool g_motionControllers = false;
bool g_handDebugPreview = false;
bool g_recenterValid = false;
bool g_headTranslationArmed = false;
bool g_directHeadRotation = false;
bool g_vrGamepad = false;
// Hand aim: JC3's engine camera (the aim) is steered toward the right-hand
// pointing ray with ordinary relative mouse input; the render is composed on
// JC3VR's own body yaw, turned with the right stick.
bool g_handAim = false;
bool g_pairedSubmission = false;
// HUD re-placement: JC3's screen-space HUD is moved per eye to a fixed
// distance in front of the head (F11 toggles).
bool g_hudAlign = true;
bool g_wristHud = true;
float g_hudDistance = 3.0f;
float g_hudScale = 0.65f;
// Full-screen menus (map) are shown as a smaller screen further away (user
// 2026-10-02: full size was too close to read). Home/End and Page Up/Down
// adjust these instead of the HUD while a menu is open.
float g_menuScale = 0.55f;
float g_menuDistance = 4.0f;
bool g_menuOpen = false;
// Point-to-aim: -1 = head follow, 0/1 = the hand whose aim pose steers the
// engine camera (right trigger = gun, left grip = grapple).
int g_activeAimHand = -1;
// Engine-camera steering state, for F3 capture metadata (hole diagnosis):
// the engine camera drives some of JC3's terrain visibility.
float g_steerYawErrorDeg = 0.0f, g_steerPitchErrorDeg = 0.0f;
bool g_steerSuppressed = false;
std::uint32_t g_steerBackoffs = 0;
float g_steerEngineForward[3]{};
// F12 diagnostic: render each eye on two consecutive frames and show only the
// second, so every frame's "previous frame" is the same eye. Discriminates
// holes caused by previous-frame (other-eye) state: culling camera, occlusion
// or streaming. Halves the per-eye rate.
bool g_doubleRender = false;
// F12: render both eyes with the same symmetric union frustum (covering both
// eyes' extents), then resample each eye's real view out of it. A JC3
// terrain consumer that uses a stale (other-eye or older) render frustum then
// sees the same shape, so it stops culling each eye's outer edge (live
// 2026-09-30: terrain tiles missing on each eye's outer side while the engine
// camera was aligned). Costs ~20% horizontal pixel density.
bool g_symmetricEyes = false;
int g_pairStage = 0;  // 0: the pending request is an eye's first render
// Right hand is the default aim source while it points roughly where the
// head looks (engage < 60 deg, release > 75 deg); otherwise the head.
bool g_aimFollowsRightHand = false;
// Draw Quest controllers instead of the procedural hands.
bool g_controllerModel = true;
bool g_trackedHands = false;
bool g_cullOverrideTestOwnsF5 = false;
// Automatic culling-mode check: after 15 s of eye rendering, modes 0/1/2 for
// 5 s each with an eye-pair capture at the end of each (headset left still).
bool g_cullingModeTest = false;
int g_cullingModeDefault = 2;
// LOD scale test: phases of 900 Presents with P2 scale 1.0/1.5/2.0/1.0 and
// an eye-pair capture near each phase end (headset left still).
bool g_lodScaleTest = false;
float g_lodScale = 1.0f;
bool g_forcedCapture = false;
std::string g_forcedCaptureTag;
// Drawn aim line on the aiming hand (off: the game's own crosshair, moved to
// the aiming hand in the HUD pass, is the only aim indicator).
bool g_aimLines = false;
// Left grip past the XInput LB threshold (grapple button), for the probe.
bool g_grappleHeld = false;
// Controller render models load on a background thread (OBJ + PNG decode).
std::array<ControllerModelAsset, 2> g_controllerAssets;
ControllerModelAsset g_chargeAsset;
std::atomic<int> g_chargeAssetState{0};
std::atomic<int> g_controllerAssetState = 0;  // 0 idle, 1 loading, 2 ready, 3 failed
// Alyx-style gesture D-pad: hold the right stick click and move the right
// hand up/down/left/right, release to press that D-pad direction.
bool g_dpadSelecting = false;
std::array<int,4> g_selectorIconIds{-1,-1,-1,-1};
XrVector3f g_dpadCentre{};
XrVector3f g_dpadRight{1.0f, 0.0f, 0.0f};
XrQuaternionf g_dpadOrientation{0.0f, 0.0f, 0.0f, 1.0f};
int g_dpadDirection = -1;
bool g_aimAligned = false;
ULONGLONG g_aimEngagedAt = 0;
bool g_reprojectToFramePose = false;
// Steer JC3's engine camera toward the headset direction so engine-side
// culling, LOD and streaming follow the head.
bool g_followHead = false;
float g_handAimGain = 0.5f;
float g_snapTurnRadians = 0.5235988f;
bool g_debugHandAim = false;
float g_debugHandAimYaw = 0.0f;
float g_debugHandAimPitch = 0.0f;
bool g_bodyFrameValid = false;
float g_bodyRight[3]{1.0f, 0.0f, 0.0f};
bool g_snapLatched = false;
// Controller recenter (both thumbsticks clicked), consumed by the rig.
bool g_controllerRecenterRequest = false;
bool g_stickComboWasDown = false;
std::uint64_t g_recenterCount = 0;
float g_aimRemainderX = 0.0f;
// Presents since JC3VR last injected steering input (cut detection must not
// mistake its own steering for a camera cut).
int g_presentsSinceSteering = 1000;
// Set when the body yaw re-initializes so feed-forward does not lurch.
bool g_steeringTargetReset = true;
float g_aimRemainderY = 0.0f;
float g_debugHeadYawRadians = 0.0f;
float g_debugHeadYawWobbleRadians = 0.0f;
bool g_cameraWritesPaused = false;
bool g_pauseKeyWasDown = false;
bool g_recenterKeyWasDown = false;
XrVector3f g_recenterHeadCentre{};
XrVector3f g_recenterRight{1.0f, 0.0f, 0.0f};
XrVector3f g_recenterForward{0.0f, 0.0f, -1.0f};

uint32_t SourceEyeForRuntimeEye(uint32_t runtimeEye) {
    switch (g_geo11StereoSource) {
    case Geo11StereoSource::LeftToBoth: return 0;
    case Geo11StereoSource::RightToBoth: return 1;
    default: return g_geo11SwapEyes ? 1u - runtimeEye : runtimeEye;
    }
}

const char* EyeName(uint32_t eye) { return eye == 0 ? "left" : "right"; }

std::string DescribeTexture(ID3D11Resource* resource) {
    if (!resource) return "none";
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture) return "non-Texture2D";
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
           " fmt=" + std::to_string(static_cast<uint32_t>(desc.Format)) +
           " samples=" + std::to_string(desc.SampleDesc.Count) +
           " bind=0x" + [&] { std::ostringstream stream; stream << std::hex << desc.BindFlags; return stream.str(); }();
}

std::string DescribeRect(const D3D11_RECT& rect) {
    return "[" + std::to_string(rect.left) + "," + std::to_string(rect.top) +
           ".." + std::to_string(rect.right) + "," + std::to_string(rect.bottom) + ")";
}

// This is deliberately a read-only snapshot taken only at the JC3 final-image
// boundary and our own resample pass.  It distinguishes a bad source image from
// a bad OpenXR copy without observing unrelated engine draw calls.
void TraceRenderState(ID3D11DeviceContext* context, const char* stage, ID3D11Resource* subject) {
    if (!context) return;
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
    Microsoft::WRL::ComPtr<ID3D11Resource> rtvResource;
    Microsoft::WRL::ComPtr<ID3D11Resource> dsvResource;
    if (rtv) rtv->GetResource(&rtvResource);
    if (dsv) dsv->GetResource(&dsvResource);
    std::ostringstream stream;
    stream << "render-path " << stage << " subject=" << DescribeTexture(subject)
           << " rtv=" << DescribeTexture(rtvResource.Get())
           << " dsv=" << DescribeTexture(dsvResource.Get())
           << " viewport_count=" << viewportCount;
    for (UINT index = 0; index < viewportCount; ++index) {
        const auto& viewport = viewports[index];
        stream << " vp" << index << "=[" << viewport.TopLeftX << "," << viewport.TopLeftY
               << " " << viewport.Width << "x" << viewport.Height << " z=" << viewport.MinDepth << ".." << viewport.MaxDepth << "]";
    }
    stream << " scissor_count=" << scissorCount;
    for (UINT index = 0; index < scissorCount; ++index) stream << " sc" << index << "=" << DescribeRect(scissors[index]);
    TraceRuntime(stream.str());
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
}

template <typename T>
bool LoadProc(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char* name, T& destination) {
    return XR_SUCCEEDED(getProc(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&destination))) && destination != nullptr;
}

// Grip-pose input is optional. No failure in this path may prevent normal
// OpenXR image submission or affect the existing keyboard/gamepad controls.
void InitializeHandActions(RuntimeState& state) {
    if (!g_motionControllers) return;
    PFN_xrStringToPath stringToPath{};
    PFN_xrCreateActionSet createActionSet{};
    PFN_xrCreateAction createAction{};
    PFN_xrSuggestInteractionProfileBindings suggestBindings{};
    PFN_xrAttachSessionActionSets attachActionSets{};
    PFN_xrCreateActionSpace createActionSpace{};
    if (!LoadProc(state.getProc, state.instance, "xrStringToPath", stringToPath) ||
        !LoadProc(state.getProc, state.instance, "xrCreateActionSet", createActionSet) ||
        !LoadProc(state.getProc, state.instance, "xrCreateAction", createAction) ||
        !LoadProc(state.getProc, state.instance, "xrSuggestInteractionProfileBindings", suggestBindings) ||
        !LoadProc(state.getProc, state.instance, "xrAttachSessionActionSets", attachActionSets) ||
        !LoadProc(state.getProc, state.instance, "xrCreateActionSpace", createActionSpace) ||
        !LoadProc(state.getProc, state.instance, "xrSyncActions", state.syncActions) ||
        !LoadProc(state.getProc, state.instance, "xrGetActionStatePose", state.getActionStatePose) ||
        !LoadProc(state.getProc, state.instance, "xrLocateSpace", state.locateSpace)) {
        TraceRuntime("hand actions unavailable: required OpenXR action entry point missing; desktop controls unchanged");
        return;
    }
    const char* handPathNames[2] = {"/user/hand/left", "/user/hand/right"};
    for (uint32_t hand = 0; hand < 2; ++hand) {
        if (XR_FAILED(stringToPath(state.instance, handPathNames[hand], &state.handPaths[hand]))) {
            TraceRuntime("hand actions unavailable: hand path rejected");
            return;
        }
    }
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "jc3vr_hands");
    strcpy_s(setInfo.localizedActionSetName, "JC3VR Hands");
    if (XR_FAILED(createActionSet(state.instance, &setInfo, &state.handActionSet))) {
        TraceRuntime("hand actions unavailable: xrCreateActionSet failed");
        return;
    }
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(actionInfo.actionName, "hand_grip_pose");
    strcpy_s(actionInfo.localizedActionName, "Hand Grip Pose");
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    actionInfo.countSubactionPaths = 2;
    actionInfo.subactionPaths = state.handPaths.data();
    if (XR_FAILED(createAction(state.handActionSet, &actionInfo, &state.handGripAction))) {
        TraceRuntime("hand actions unavailable: xrCreateAction failed");
        return;
    }
    if (g_handAim) {
        XrActionCreateInfo aimInfo{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(aimInfo.actionName, "hand_aim_pose");
        strcpy_s(aimInfo.localizedActionName, "Hand Aim Pose");
        aimInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        aimInfo.countSubactionPaths = 2;
        aimInfo.subactionPaths = state.handPaths.data();
        if (XR_FAILED(createAction(state.handActionSet, &aimInfo, &state.handAimAction))) {
            state.handAimAction = XR_NULL_HANDLE;
            TraceRuntime("hand aim pose action unavailable; hand aim disabled");
        }
    }
    if(VrHapticsEnabled() &&
       LoadProc(state.getProc,state.instance,"xrApplyHapticFeedback",state.applyHapticFeedback) &&
       LoadProc(state.getProc,state.instance,"xrStopHapticFeedback",state.stopHapticFeedback)) {
        XrActionCreateInfo haptic{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(haptic.actionName,"controller_vibration");
        strcpy_s(haptic.localizedActionName,"Controller Vibration");
        haptic.actionType=XR_ACTION_TYPE_VIBRATION_OUTPUT;
        haptic.countSubactionPaths=2;haptic.subactionPaths=state.handPaths.data();
        if(XR_FAILED(createAction(state.handActionSet,&haptic,&state.hapticAction))) state.hapticAction=XR_NULL_HANDLE;
        TraceRuntime(state.hapticAction!=XR_NULL_HANDLE?"VR haptics: per-hand OpenXR output action created":
            "VR haptics: output action unavailable; tracking unchanged");
    }
    // Controller input actions for the optional virtual XInput pad.
    const bool gamepadActions = g_vrGamepad &&
        LoadProc(state.getProc, state.instance, "xrGetActionStateFloat", state.getActionStateFloat) &&
        LoadProc(state.getProc, state.instance, "xrGetActionStateBoolean", state.getActionStateBoolean) &&
        LoadProc(state.getProc, state.instance, "xrGetActionStateVector2f", state.getActionStateVector2f);
    if (gamepadActions) {
        const auto makeAction = [&](const char* name, XrActionType type, bool perHand, XrAction& action) {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            strcpy_s(info.actionName, name);
            strcpy_s(info.localizedActionName, name);
            info.actionType = type;
            info.countSubactionPaths = perHand ? 2 : 0;
            info.subactionPaths = perHand ? state.handPaths.data() : nullptr;
            return XR_SUCCEEDED(createAction(state.handActionSet, &info, &action));
        };
        const bool created =
            makeAction("thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true, state.stickAction) &&
            makeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true, state.triggerAction) &&
            makeAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true, state.squeezeAction) &&
            makeAction("thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true, state.stickClickAction) &&
            makeAction("button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false, state.aAction) &&
            makeAction("button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false, state.bAction) &&
            makeAction("button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false, state.xAction) &&
            makeAction("button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false, state.yAction) &&
            makeAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false, state.menuAction);
        if (!created) {
            state.stickAction = XR_NULL_HANDLE;
            TraceRuntime("VR gamepad actions unavailable: xrCreateAction failed; grip poses only");
        }
    }
    // A pose action does not require game input injection. The runtime chooses
    // its active profile; suggest the same physical grip source on common
    // controllers and leave unsupported profiles inactive.
    const char* profiles[] = {
        "/interaction_profiles/khr/simple_controller",
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/htc/vive_controller",
        "/interaction_profiles/microsoft/motion_controller",
    };
    uint32_t acceptedProfiles = 0;
    for (const char* profileName : profiles) {
        XrPath profile = XR_NULL_PATH;
        if (XR_FAILED(stringToPath(state.instance, profileName, &profile))) continue;
        std::vector<XrActionSuggestedBinding> bindings(2);
        for (uint32_t hand = 0; hand < 2; ++hand) {
            std::string bindingName = std::string(handPathNames[hand]) + "/input/grip/pose";
            bindings[hand].action = state.handGripAction;
            if (XR_FAILED(stringToPath(state.instance, bindingName.c_str(), &bindings[hand].binding))) break;
        }
        if (bindings[0].binding == XR_NULL_PATH || bindings[1].binding == XR_NULL_PATH) continue;
        if (state.handAimAction != XR_NULL_HANDLE) {
            for (uint32_t hand = 0; hand < 2; ++hand) {
                XrActionSuggestedBinding binding{state.handAimAction, XR_NULL_PATH};
                const std::string aimPath = std::string(handPathNames[hand]) + "/input/aim/pose";
                if (XR_SUCCEEDED(stringToPath(state.instance, aimPath.c_str(), &binding.binding))) bindings.push_back(binding);
            }
        }
        if (state.stickAction != XR_NULL_HANDLE && std::string(profileName) == "/interaction_profiles/oculus/touch_controller") {
            const std::pair<XrAction, const char*> inputs[] = {
                {state.stickAction, "/user/hand/left/input/thumbstick"}, {state.stickAction, "/user/hand/right/input/thumbstick"},
                {state.triggerAction, "/user/hand/left/input/trigger/value"}, {state.triggerAction, "/user/hand/right/input/trigger/value"},
                {state.squeezeAction, "/user/hand/left/input/squeeze/value"}, {state.squeezeAction, "/user/hand/right/input/squeeze/value"},
                {state.stickClickAction, "/user/hand/left/input/thumbstick/click"}, {state.stickClickAction, "/user/hand/right/input/thumbstick/click"},
                {state.aAction, "/user/hand/right/input/a/click"}, {state.bAction, "/user/hand/right/input/b/click"},
                {state.xAction, "/user/hand/left/input/x/click"}, {state.yAction, "/user/hand/left/input/y/click"},
                {state.menuAction, "/user/hand/left/input/menu/click"},
            };
            for (const auto& [action, path] : inputs) {
                XrActionSuggestedBinding binding{action, XR_NULL_PATH};
                if (XR_SUCCEEDED(stringToPath(state.instance, path, &binding.binding))) bindings.push_back(binding);
            }
        }
        XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        if(state.hapticAction!=XR_NULL_HANDLE) {
            for(uint32_t hand=0;hand<2;++hand) {
                XrActionSuggestedBinding binding{state.hapticAction,XR_NULL_PATH};
                const std::string output=std::string(handPathNames[hand])+"/output/haptic";
                if(XR_SUCCEEDED(stringToPath(state.instance,output.c_str(),&binding.binding))) bindings.push_back(binding);
            }
        }
        suggestion.interactionProfile = profile;
        suggestion.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggestion.suggestedBindings = bindings.data();
        if (XR_SUCCEEDED(suggestBindings(state.instance, &suggestion))) ++acceptedProfiles;
    }
    for (uint32_t hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = state.handGripAction;
        spaceInfo.subactionPath = state.handPaths[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(createActionSpace(state.session, &spaceInfo, &state.handSpaces[hand]))) {
            TraceRuntime("hand actions unavailable: xrCreateActionSpace failed; desktop controls unchanged");
            return;
        }
    }
    if (state.handAimAction != XR_NULL_HANDLE) {
        for (uint32_t hand = 0; hand < 2; ++hand) {
            XrActionSpaceCreateInfo aimSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            aimSpaceInfo.action = state.handAimAction;
            aimSpaceInfo.subactionPath = state.handPaths[hand];
            aimSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
            if (XR_FAILED(createActionSpace(state.session, &aimSpaceInfo, &state.handAimSpaces[hand]))) state.handAimSpaces[hand] = XR_NULL_HANDLE;
        }
    }
    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &state.handActionSet;
    if (XR_FAILED(attachActionSets(state.session, &attachInfo))) {
        TraceRuntime("hand actions unavailable: xrAttachSessionActionSets failed; desktop controls unchanged");
        return;
    }
    state.handActionsAttached = true;
    TraceRuntime("hand grip actions attached: profiles_accepted=" + std::to_string(acceptedProfiles) + " spaces=2; game inputs unchanged");
}

void UpdateHandPoses(RuntimeState& state, XrTime predictedDisplayTime,const XrQuaternionf* currentHead,const XrVector3f* currentHeadPosition) {
    state.supportFrameValid=false;
    if (!state.handActionsAttached || !state.syncActions || !state.getActionStatePose || !state.locateSpace) {
        CancelPauseChord(state.pauseChord);
        state.helicopterPull={};
        RequestDualFire(0,false,0,0);return;
    }
    const XrActiveActionSet activeSet{state.handActionSet, XR_NULL_PATH};
    const XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &activeSet};
    if (XR_FAILED(state.syncActions(state.session, &syncInfo))) {
        CancelPauseChord(state.pauseChord);
        state.helicopterPull={};
        RequestDualFire(0,false,0,0);return;
    }
    for (uint32_t hand = 0; hand < 2; ++hand) {
        state.handPoseActive[hand] = false;
        if (state.handSpaces[hand] == XR_NULL_HANDLE) continue;
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = state.handGripAction;
        getInfo.subactionPath = state.handPaths[hand];
        XrActionStatePose actionState{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(state.getActionStatePose(state.session, &getInfo, &actionState)) || !actionState.isActive) continue;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(state.locateSpace(state.handSpaces[hand], state.localSpace, predictedDisplayTime, &location))) continue;
        constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((location.locationFlags & required) != required) continue;
        state.handLocations[hand] = location;
        state.handPoseActive[hand] = true;
    }
    for (uint32_t hand = 0; hand < 2; ++hand) {
        state.handAimActive[hand] = false;
        if (state.handAimSpaces[hand] == XR_NULL_HANDLE) continue;
        XrActionStateGetInfo aimGet{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, state.handAimAction, state.handPaths[hand]};
        XrActionStatePose aimState{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(state.getActionStatePose(state.session, &aimGet, &aimState)) || !aimState.isActive) continue;
        XrSpaceLocation aimLocation{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(state.locateSpace(state.handAimSpaces[hand], state.localSpace, predictedDisplayTime, &aimLocation))) continue;
        if (!(aimLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) continue;
        state.handAimPoses[hand] = aimLocation.pose;
        state.handAimActive[hand] = true;
    }
    if (state.stickAction != XR_NULL_HANDLE && state.getActionStateFloat && state.getActionStateBoolean && state.getActionStateVector2f) {
        const auto readFloat = [&](XrAction action, XrPath subaction) {
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, subaction};
            XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
            return XR_SUCCEEDED(state.getActionStateFloat(state.session, &info, &value)) && value.isActive ? value.currentState : 0.0f;
        };
        const auto readBool = [&](XrAction action, XrPath subaction) {
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, subaction};
            XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
            return XR_SUCCEEDED(state.getActionStateBoolean(state.session, &info, &value)) && value.isActive && value.currentState;
        };
        const auto readStick = [&](XrPath subaction, float& x, float& y) {
            XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, state.stickAction, subaction};
            XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(state.getActionStateVector2f(state.session, &info, &value)) && value.isActive) {
                x = value.currentState.x;
                y = value.currentState.y;
            }
        };
        VrControllerInputs inputs{};
        inputs.active = state.handPoseActive[0] || state.handPoseActive[1];
        readStick(state.handPaths[0], inputs.leftStickX, inputs.leftStickY);
        readStick(state.handPaths[1], inputs.rightStickX, inputs.rightStickY);
        inputs.leftTrigger = readFloat(state.triggerAction, state.handPaths[0]);
        inputs.rightTrigger = readFloat(state.triggerAction, state.handPaths[1]);
        inputs.leftGrip = readFloat(state.squeezeAction, state.handPaths[0]);
        inputs.rightGrip = readFloat(state.squeezeAction, state.handPaths[1]);
        inputs.leftStickClick = readBool(state.stickClickAction, state.handPaths[0]);
        inputs.rightStickClick = readBool(state.stickClickAction, state.handPaths[1]);
        inputs.a = readBool(state.aAction, XR_NULL_PATH);
        inputs.b = readBool(state.bAction, XR_NULL_PATH);
        inputs.x = readBool(state.xAction, XR_NULL_PATH);
        inputs.y = readBool(state.yAction, XR_NULL_PATH);
        inputs.menu = readBool(state.menuAction, XR_NULL_PATH);
        const auto pause = UpdatePauseChord(state.pauseChord,
            state.focused && state.sessionRunning && state.handPoseActive[0],
            inputs.x, inputs.y, GetTickCount64());
        if (pause.consume) inputs.x = inputs.y = false;
        if (pause.pause) inputs.extraButtons |= 0x0010; // XInput START: native pause menu
        if (pause.fired) TraceRuntime("controller pause: left X+Y held 1s; one START pulse");
        // Both thumbsticks clicked together = recenter (same as F2); the
        // clicks are withheld from the game while the combo is held.
        const bool stickCombo = inputs.leftStickClick && inputs.rightStickClick;
        static RecenterTap recenterTap;
        if(TapRecenter(recenterTap,stickCombo))
            g_controllerRecenterRequest=true;
        g_stickComboWasDown = stickCombo;
        if (stickCombo) inputs.leftStickClick = inputs.rightStickClick = false;
        {
            // Map on the left stick (user 2026-10-02): holding it down for
            // 0.6 s presses the game's map button (XInput BACK / View). A
            // shorter click is delivered as a normal stick click on release.
            // A recenter combo cancels it.
            constexpr ULONGLONG kMapHoldMs = 600;
            static ULONGLONG downSince = 0, pulseUntil = 0;
            static bool spent = false;
            static std::uint16_t pulse = 0;
            const ULONGLONG now = GetTickCount64();
            if (stickCombo) spent = true;
            if (inputs.leftStickClick) {
                if (!downSince) { downSince = now; spent = stickCombo; }
                if (!spent && now - downSince >= kMapHoldMs) {
                    spent = true;
                    pulse = 0x0020;  // XInput BACK: JC3 opens the map
                    pulseUntil = now + 150;
                    TraceRuntime("left stick hold: map button pressed");
                }
            } else if (downSince) {
                if (!spent) { pulse = 0x0040; pulseUntil = now + 120; }  // the click itself (XInput LEFT_THUMB)
                downSince = 0;
                spent = false;
            }
            inputs.leftStickClick = false;  // delivered only as the pulses above
            if (now < pulseUntil) inputs.extraButtons |= pulse;
        }
        {
            // Gesture D-pad (JC3 switches weapons on the D-pad): while the
            // right stick is held down, a >= 7 cm hand move up/down/left/
            // right (head-relative) picks that direction; releasing presses
            // it for 120 ms. A click without a move stays a stick click.
            static bool cancelled = false;
            static ULONGLONG pulseUntil = 0;
            static std::uint16_t pulseButton = 0;
            const ULONGLONG now = GetTickCount64();
            if (stickCombo) cancelled = true;
            const bool held = inputs.rightStickClick && state.handPoseActive[1];
            const XrVector3f hand = state.handLocations[1].pose.position;
            if (held && !g_dpadSelecting) {
                g_dpadSelecting = true;
                g_selectorIconIds = GetWeaponSelectorIconIds();
                TraceRuntime("weapon selector icons up/down/left/right=" + std::to_string(g_selectorIconIds[0]) + "/" +
                    std::to_string(g_selectorIconIds[1]) + "/" + std::to_string(g_selectorIconIds[2]) + "/" + std::to_string(g_selectorIconIds[3]));
                cancelled = false;
                g_dpadDirection = -1;
                g_dpadCentre = hand;
                OpenXrHeadPose head{};
                {
                    std::lock_guard poseLock(g_poseMutex);
                    head = g_latestPose;
                }
                const XrQuaternionf headOrientation{head.orientationX, head.orientationY, head.orientationZ, head.orientationW};
                XrVector3f right = head.valid ? RotateBy(headOrientation, {1.0f, 0.0f, 0.0f}) : XrVector3f{1.0f, 0.0f, 0.0f};
                right.y = 0.0f;
                const float length = std::sqrt(right.x * right.x + right.z * right.z);
                g_dpadRight = length > 0.1f ? XrVector3f{right.x / length, 0.0f, right.z / length} : XrVector3f{1.0f, 0.0f, 0.0f};
                // Yaw-only orientation for the selector markers.
                const float yaw = std::atan2(-g_dpadRight.z, g_dpadRight.x);
                g_dpadOrientation = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
            }
            if (g_dpadSelecting && held) {
                const XrVector3f delta{hand.x - g_dpadCentre.x, hand.y - g_dpadCentre.y, hand.z - g_dpadCentre.z};
                const float dx = delta.x * g_dpadRight.x + delta.z * g_dpadRight.z;
                const float dy = delta.y;
                const float reach = std::max(std::abs(dx), std::abs(dy));
                if (reach >= 0.07f) g_dpadDirection = std::abs(dx) > std::abs(dy) ? (dx > 0.0f ? 3 : 2) : (dy > 0.0f ? 0 : 1);
                else if (reach < 0.04f) g_dpadDirection = -1;
            }
            if (g_dpadSelecting && !held) {
                g_dpadSelecting = false;
                if (!cancelled) {
                    // XInput D-pad bits: up 0x1, down 0x2, left 0x4, right 0x8.
                    static constexpr std::uint16_t kDpad[4]{0x0001, 0x0002, 0x0004, 0x0008};
                    static constexpr const char* kNames[4]{"up", "down", "left", "right"};
                    pulseButton = g_dpadDirection >= 0 ? kDpad[g_dpadDirection] : static_cast<std::uint16_t>(0x0080);
                    pulseUntil = now + 120;
                    TraceRuntime(std::string("gesture D-pad: ") + (g_dpadDirection >= 0 ? kNames[g_dpadDirection] : "stick click (no move)"));
                }
                g_dpadDirection = -1;
            }
            // The physical click is withheld while selecting; the choice is
            // delivered as a pulse after release.
            inputs.rightStickClick = false;
            if (now < pulseUntil) inputs.extraButtons |= pulseButton;
        }
        // Finger curl for the 3D hands: trigger -> index, grip -> other
        // fingers, a pressed face button/stick -> thumb.
        state.handCurl[0] = {inputs.leftTrigger, inputs.leftGrip, (inputs.x || inputs.y || inputs.menu || inputs.leftStickClick) ? 1.0f : 0.0f};
        state.handCurl[1] = {inputs.rightTrigger, inputs.rightGrip, (inputs.a || inputs.b || inputs.rightStickClick) ? 1.0f : 0.0f};
        bool handsMode = false;
        if (RicoPocketHolsterAvailable()) {
            const bool eligible = currentHead && currentHeadPosition && state.focused && inputs.active &&
                state.handPoseActive[1] && VrBodyCharacter() && !GetVehicleCameraAnchor().valid &&
                HasNativeGameplayCamera() && !g_menuOpen && !state.frontEndPanelActive;
            bool pocket = false;
            if (eligible) {
                const auto right = RotateBy(*currentHead, {1, 0, 0});
                const float horizontal = std::hypot(right.x, right.z);
                const auto& hand = state.handLocations[1].pose.position;
                const float x = hand.x - currentHeadPosition->x, z = hand.z - currentHeadPosition->z;
                if (horizontal > 0.2f)
                    pocket = InRightPocket((x * right.x + z * right.z) / horizontal,
                        currentHeadPosition->y - hand.y, (x * right.z - z * right.x) / horizontal);
            }
            const auto action = UpdatePocketHolster(state.pocketHolster, eligible, pocket, inputs.rightGrip);
            handsMode = state.pocketHolster.hands;
            UpdateRicoPocketRequest(handsMode, action);
            PublishNativeGrabProbeGrips(handsMode && action == PocketAction::None,
                inputs.leftGrip, inputs.rightGrip);
            if (handsMode || action != PocketAction::None) {
                inputs.rightGrip = 0;
                inputs.rightTrigger = 0;
            }
            if (handsMode) {
                inputs.leftGrip = 0;
                inputs.leftTrigger = 0;
                inputs.extraButtons &= ~0x000fu;
            }
        }
        // Optic activation comes from eye alignment, not R3/native zoom. Do
        // not consume the button until the lens GPU path has proved ready.
        if(VrScopeReady(GetVrSniperWeapon()) && state.handPoseActive[1] && state.handAimActive[1]) inputs.rightStickClick=false;
        {
            PhysicalHijackInput physical{};physical.now=GetTickCount64();
            physical.active=currentHeadPosition && currentHead && state.focused && state.sessionRunning &&
                state.handPoseActive[0] && HasNativeGameplayCamera() && !g_menuOpen && !g_dpadSelecting && !state.frontEndPanelActive;
            physical.grip=inputs.leftGrip;
            const auto p=state.handLocations[0].pose.position;
            physical.hand[0]=p.x;physical.hand[1]=p.y;physical.hand[2]=p.z;
            if(currentHeadPosition){physical.head[0]=currentHeadPosition->x;physical.head[1]=currentHeadPosition->y;physical.head[2]=currentHeadPosition->z;}
            const auto response=SubmitPhysicalHijackInput(physical);
            if(response.consume)inputs.leftGrip=0;
            if(response.ready)RequestVrHapticPulse(0,0.18f);
            if(response.grabbed)RequestVrHapticPulse(0,0.3f);
            if(response.opened)RequestVrHapticPulse(0,0.65f);
        }
        {
            HelicopterPullInput pull{};
            // No new engine call here: a fresh exterior-helicopter identity is
            // published by the existing local-character callback.
            pull.vehicle=GetExteriorPullHelicopter();pull.now=GetTickCount64();
            pull.active=pull.vehicle && currentHeadPosition && currentHead && state.focused &&
                state.sessionRunning && state.handPoseActive[0] && HasNativeGameplayCamera() &&
                !g_menuOpen && !g_dpadSelecting && !state.frontEndPanelActive;
            pull.modifier=inputs.x && !inputs.y;pull.grip=inputs.leftGrip;
            const auto hand=state.handLocations[0].pose.position;
            pull.hand[0]=hand.x;pull.hand[1]=hand.y;pull.hand[2]=hand.z;
            if(currentHeadPosition) {pull.head[0]=currentHeadPosition->x;pull.head[1]=currentHeadPosition->y;pull.head[2]=currentHeadPosition->z;}
            const auto gesture=UpdateHelicopterPull(state.helicopterPull,pull);
            if(gesture.consume) {inputs.x=false;inputs.leftGrip=0;}
            // Installed native keymap ENTER_VEHICLE = RPADUP (Xbox Y).
            // Engine retains all distance/state/occupant eligibility checks.
            if(gesture.enter)inputs.extraButtons|=0x8000;
            if(gesture.grabbed)RequestVrHapticPulse(0,0.25f);
            if(gesture.fired) {
                RequestVrHapticPulse(0,0.6f);
                TraceRuntime("helicopter pull: 20cm gesture completed; native ENTER_VEHICLE requested (not confirmation of canopy opening)");
            }
        }
        {
            SupportGripInput grip{};
            grip.weapon=GetSupportGripWeapon(); grip.eligible=grip.weapon!=0;
            const auto supportProfile=GetSupportGripProfile(grip.weapon);
            grip.minimum=supportProfile.minimum;grip.maximum=supportProfile.maximum;grip.radius=supportProfile.radius;
            grip.gameplay=HasNativeGameplayCamera() && !g_dpadSelecting;
            grip.tracking=state.handPoseActive[0] && state.handPoseActive[1] && state.handAimActive[1];
            grip.squeeze=inputs.leftGrip;
            const auto left=state.handLocations[0].pose.position;
            const auto right=state.handLocations[1].pose.position;
            const auto forward=RotateBy(state.handAimPoses[1].orientation,{0,0,-1});
            grip.left[0]=left.x; grip.left[1]=left.y; grip.left[2]=left.z;
            grip.right[0]=right.x; grip.right[1]=right.y; grip.right[2]=right.z;
            grip.forward[0]=forward.x; grip.forward[1]=forward.y; grip.forward[2]=forward.z;
            const auto grab=UpdateSupportGrip(state.supportGrip,grip);
            state.supportFrameValid=SupportGripFrame(state.supportGrip,grip.left,grip.right,state.supportForward,state.supportSocket);
            RequestSupportGripPrecision(grab.precision,grip.weapon);
            // A captured grab cannot also press LB/fire a grapple. Away from
            // the fore-end, the existing left squeeze binding stays unchanged.
            if (grab.consumeGrapple) inputs.leftGrip=0;
        }
        const auto seatedControls=GetVehicleCameraAnchor();
        if ((g_handAim || g_followHead) && !seatedControls.valid) {
            // Right stick X turns the body (snap turn); with hand aim the
            // engine camera is driven by the hand, so the right stick is
            // withheld from the game.
            if (std::abs(inputs.rightStickX) > 0.6f) {
                if (!g_snapLatched && g_bodyFrameValid) {
                    const float angle = inputs.rightStickX > 0.0f ? g_snapTurnRadians : -g_snapTurnRadians;
                    const float right[3]{g_bodyRight[0], 0.0f, g_bodyRight[2]};
                    const float forward[3]{right[2], 0.0f, -right[0]};
                    g_bodyRight[0] = right[0] * std::cos(angle) - forward[0] * std::sin(angle);
                    g_bodyRight[2] = right[2] * std::cos(angle) - forward[2] * std::sin(angle);
                    const float length = std::sqrt(g_bodyRight[0] * g_bodyRight[0] + g_bodyRight[2] * g_bodyRight[2]);
                    g_bodyRight[0] /= length;
                    g_bodyRight[2] /= length;
                    TraceRuntime(std::string("hand aim: snap turn ") + (angle > 0.0f ? "right" : "left"));
                }
                g_snapLatched = true;
            } else if (std::abs(inputs.rightStickX) < 0.3f) {
                g_snapLatched = false;
            }
            inputs.rightStickX = 0.0f;
            inputs.rightStickY = 0.0f;
        }
        const bool nativeGunAim=WeaponAimFromController();
        if (nativeGunAim) {
            const int grappleHand=NativeGrappleAimHand(g_handAim,seatedControls.valid,inputs.active,state.handAimActive[0],inputs.leftGrip);
            if (g_activeAimHand!=grappleHand) {
                g_activeAimHand=grappleHand;
                g_aimEngagedAt=GetTickCount64();
                g_aimAligned=false;
                TraceRuntime(std::string("grapple aim ownership: ")+(grappleHand==0?"left-controller":"head/reset")+"; native gun ray independent");
            }
            // Gate ONLY grapple until its engine targeting camera has caught
            // up. Native right/dual gun rays must remain free to fire.
            if (grappleHand==0 && !g_aimAligned && GetTickCount64()-g_aimEngagedAt<250) inputs.leftGrip=0.0f;
        } else if (LegacyAimGate(g_handAim,nativeGunAim,seatedControls.valid)) {
            // Starting to pull the right trigger (gun) or squeeze the left
            // grip (grapple) hands the engine camera to that hand's aim.
            static bool rightWanted = false, leftWanted = false;
            const bool rightWants = inputs.rightTrigger > 0.05f && state.handAimActive[1];
            const bool leftWants = inputs.leftGrip > 0.3f && state.handAimActive[0];
            const ULONGLONG now = GetTickCount64();
            static bool rightDefault = false;
            if (g_aimFollowsRightHand && state.handAimActive[1]) {
                OpenXrHeadPose head{};
                {
                    std::lock_guard poseLock(g_poseMutex);
                    head = g_latestPose;
                }
                if (head.valid) {
                    const XrVector3f headForward = RotateBy({head.orientationX, head.orientationY, head.orientationZ, head.orientationW}, {0.0f, 0.0f, -1.0f});
                    const XrVector3f handForward = RotateBy(state.handAimPoses[1].orientation, {0.0f, 0.0f, -1.0f});
                    const float cosine = headForward.x * handForward.x + headForward.y * handForward.y + headForward.z * handForward.z;
                    if (cosine > 0.5f) rightDefault = true;          // within 60 deg
                    else if (cosine < 0.2588f) rightDefault = false; // beyond 75 deg
                }
            } else rightDefault = false;
            if (rightWants && !rightWanted && g_activeAimHand != 1) { g_activeAimHand = 1; g_aimEngagedAt = now; g_aimAligned = false; }
            if (leftWants && !leftWanted) { g_activeAimHand = 0; g_aimEngagedAt = now; g_aimAligned = false; }
            if (!rightWants && !leftWants) {
                if (rightDefault && g_activeAimHand != 1) { g_aimEngagedAt = now; g_aimAligned = false; }
                g_activeAimHand = rightDefault ? 1 : -1;
            }
            else if (g_activeAimHand == 1 && !rightWants) g_activeAimHand = 0;
            else if (g_activeAimHand == 0 && !leftWants) g_activeAimHand = 1;
            rightWanted = rightWants;
            leftWanted = leftWants;
            // JC3 fires/grapples along the engine camera the instant the
            // button goes down: hold the button back until the camera has
            // swung onto the hand's aim (or 250 ms, so it never feels dead).
            const bool settling = g_activeAimHand >= 0 && !g_aimAligned && now - g_aimEngagedAt < 250;
            if (settling && g_activeAimHand == 1) inputs.rightTrigger = 0.0f;
            if (settling && g_activeAimHand == 0) inputs.leftGrip = 0.0f;
        } else {
            g_activeAimHand=-1;
            g_aimAligned=false;
        }
        const bool grappleHeld=inputs.leftGrip>0.55f;
        if(grappleHeld && !g_grappleHeld && inputs.active && HasNativeGameplayCamera()) RequestVrHapticPulse(0,0.45f);
        g_grappleHeld = grappleHeld;
        const auto dual=handsMode ? 0 : GetDualWieldWeapon();
        const bool dualActive=dual && inputs.active && HasNativeGameplayCamera() && !g_dpadSelecting &&
            state.handPoseActive[0] && state.handPoseActive[1] && state.handAimActive[0] && state.handAimActive[1];
        RequestDualFire(dual,dualActive,state.handCurl[0].index,state.handCurl[1].index);
        if(dualActive) {
            // Keep the game's permission/reload path active for either trigger,
            // then split it at the native per-gun update. LT remains unchanged
            // for all non-dual equipment (C4, rifles and vehicle controls).
            inputs.rightTrigger=std::max(state.handCurl[0].index,state.handCurl[1].index);
            inputs.leftTrigger=0;
        }
        const auto c4=handsMode ? 0 : GetVrC4Weapon();
        RequestVrC4Trigger(c4,inputs.active && state.handPoseActive[1] && state.handCurl[1].index>0.55f,g_jc3WorldUnitsPerMeter);
        if(c4) inputs.rightTrigger=VrC4NativePress(c4)?1.0f:0.0f;
        if(g_helicopterHeadSteering && currentHead && state.focused && inputs.active &&
           HasNativeGameplayCamera() && !g_menuOpen && !g_dpadSelecting &&
           seatedControls.driver && seatedControls.aircraft==AircraftKind::Helicopter) {
            // This frame's locateViews orientation, not a stale latest-pose
            // snapshot. Recenter defines neutral; manual rudder wins.
            const auto forward=RotateBy(*currentHead,{0,0,-1});
            inputs.rightStickX=HelicopterGazeYaw(
                forward.x*g_recenterRight.x+forward.z*g_recenterRight.z,
                forward.x*g_recenterForward.x+forward.z*g_recenterForward.z,inputs.rightStickX);
            static bool traced=false;
            if(!traced) {traced=true;TraceRuntime("helicopter: head yaw routed to verified HELI_TURN native stick actions; manual override, bounded rate");}
        }
        PublishVrControllerInputs(inputs);
        if (state.frameCount == 0 || state.frameCount % 600 == 0) {
            const auto pad = MapVrControllersToGamepad(inputs);
            TraceRuntime("VR gamepad inputs: active=" + std::to_string(inputs.active) + " buttons=0x" +
                [&] { char b[8]; std::snprintf(b, sizeof(b), "%04x", pad.buttons); return std::string(b); }() +
                " LT=" + std::to_string(pad.leftTrigger) + " RT=" + std::to_string(pad.rightTrigger) +
                " LS=[" + std::to_string(pad.thumbLX) + "," + std::to_string(pad.thumbLY) + "] RS=[" +
                std::to_string(pad.thumbRX) + "," + std::to_string(pad.thumbRY) + "]");
        }
    }
    if (state.frameCount == 0 || state.frameCount % 600 == 0) {
        TraceRuntime("hand grip pose status: left=" + std::to_string(state.handPoseActive[0]) +
            " right=" + std::to_string(state.handPoseActive[1]) +
            " predicted_time=" + std::to_string(static_cast<int64_t>(predictedDisplayTime)));
    }
}

OpenXrBootstrapResult CreateRuntime(RuntimeState& state) {
    std::string loaderLocation;
    state.loader = LoadActiveOpenXrLoader(loaderLocation);
    if (!state.loader) return {false, "OpenXR loader unavailable: " + loaderLocation};
    state.getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(state.loader, "xrGetInstanceProcAddr"));
    PFN_xrCreateInstance createInstance{};
    if (!state.getProc || !LoadProc(state.getProc, XR_NULL_HANDLE, "xrCreateInstance", createInstance)) return {false, "OpenXR loader has no xrCreateInstance entry point"};
    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "JC3VR");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "Apex");
    createInfo.applicationInfo.engineVersion = 1;
    // SteamVR's registered loader on this machine rejects the newer SDK's
    // XR_CURRENT_API_VERSION. JC3VR currently needs only baseline 1.0 calls.
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = extensions;
    const XrResult createResult = createInstance(&createInfo, &state.instance);
    if (XR_FAILED(createResult)) return {false, "xrCreateInstance failed: " + std::to_string(createResult)};
    PFN_xrGetSystem getSystem{};
    if (!LoadProc(state.getProc, state.instance, "xrGetSystem", getSystem)) return {false, "xrGetSystem is unavailable"};
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult systemResult = getSystem(state.instance, &systemInfo, &state.system);
    if (XR_FAILED(systemResult)) return {false, "xrGetSystem(HMD) failed: " + std::to_string(systemResult)};
    PFN_xrEnumerateViewConfigurationViews enumerateViews{};
    if (!LoadProc(state.getProc, state.instance, "xrEnumerateViewConfigurationViews", enumerateViews)) return {true, "OpenXR instance and HMD system ready; runtime did not expose view-configuration enumeration"};
    uint32_t viewCount = 0;
    if (XR_FAILED(enumerateViews(state.instance, state.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr)) || viewCount != 2) return {true, "OpenXR instance and HMD system ready; primary stereo view count unavailable"};
    std::array<XrViewConfigurationView, 2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
    if (XR_FAILED(enumerateViews(state.instance, state.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data()))) return {true, "OpenXR instance and HMD system ready; recommended eye image size unavailable"};
    state.recommendedEyeWidth = views[0].recommendedImageRectWidth;
    state.recommendedEyeHeight = views[0].recommendedImageRectHeight;
    state.maximumEyeWidth = views[0].maxImageRectWidth;
    state.maximumEyeHeight = views[0].maxImageRectHeight;
    std::ostringstream detail;
    detail << "OpenXR instance and HMD system ready; recommended eye images="
           << views[0].recommendedImageRectWidth << "x" << views[0].recommendedImageRectHeight << " (left), "
           << views[1].recommendedImageRectWidth << "x" << views[1].recommendedImageRectHeight << " (right); maximum="
           << views[0].maxImageRectWidth << "x" << views[0].maxImageRectHeight << " (left), "
           << views[1].maxImageRectWidth << "x" << views[1].maxImageRectHeight << " (right); recommended samples="
           << views[0].recommendedSwapchainSampleCount << "/" << views[1].recommendedSwapchainSampleCount
           << " maximum samples=" << views[0].maxSwapchainSampleCount << "/" << views[1].maxSwapchainSampleCount;
    return {true, detail.str()};
}

void UpdateControllerHaptics(RuntimeState& state,bool renderActive) {
    const bool active=renderActive && state.focused && state.sessionRunning && state.handActionsAttached;
    const auto amplitudes=GetVrHapticAmplitudes(active);
    if(state.hapticAction==XR_NULL_HANDLE || !state.applyHapticFeedback || !state.stopHapticFeedback) return;
    const auto now=GetTickCount64();
    const bool send=now-state.hapticSentAt>=25;
    for(unsigned hand=0;hand<2;++hand) {
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action=state.hapticAction;info.subactionPath=state.handPaths[hand];
        const float amplitude=active && state.handPoseActive[hand]?amplitudes[hand]:0;
        if(amplitude<=0) {
            if(state.hapticPlaying[hand]) state.stopHapticFeedback(state.session,&info);
            state.hapticPlaying[hand]=false;
        } else if(send) {
            // Never an indefinite buzz: expires even if the game/XR stalls.
            XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
            vibration.duration=50'000'000;vibration.frequency=XR_FREQUENCY_UNSPECIFIED;
            vibration.amplitude=amplitude;
            const auto result=state.applyHapticFeedback(state.session,&info,reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
            state.hapticPlaying[hand]=XR_SUCCEEDED(result);
            if(XR_FAILED(result) && state.hapticErrors++<4) TraceRuntime("VR haptics: output rejected result="+std::to_string(result));
        }
    }
    if(send) state.hapticSentAt=now;
}

void DestroyIncompleteRuntime(RuntimeState& state) {
    UpdateControllerHaptics(state,false);
    if (state.getProc && state.instance != XR_NULL_HANDLE) {
        PFN_xrDestroySpace destroySpace{};
        PFN_xrDestroySession destroySession{};
        PFN_xrDestroyInstance destroyInstance{};
        LoadProc(state.getProc, state.instance, "xrDestroySpace", destroySpace);
        LoadProc(state.getProc, state.instance, "xrDestroySession", destroySession);
        LoadProc(state.getProc, state.instance, "xrDestroyInstance", destroyInstance);
        if (destroySpace && state.viewSpace != XR_NULL_HANDLE) destroySpace(state.viewSpace);
        if (destroySpace && state.localSpace != XR_NULL_HANDLE) destroySpace(state.localSpace);
        for (XrSpace handSpace : state.handSpaces) if (destroySpace && handSpace != XR_NULL_HANDLE) destroySpace(handSpace);
        for (XrSpace handSpace : state.handAimSpaces) if (destroySpace && handSpace != XR_NULL_HANDLE) destroySpace(handSpace);
        PFN_xrDestroySwapchain destroySwapchain{};
        LoadProc(state.getProc, state.instance, "xrDestroySwapchain", destroySwapchain);
        if (destroySwapchain) {
            if (state.swapchain != XR_NULL_HANDLE) destroySwapchain(state.swapchain);
            for (auto chain : state.sbsSwapchains) if (chain != XR_NULL_HANDLE) destroySwapchain(chain);
            for (auto chain : state.nativeSwapchains) if (chain != XR_NULL_HANDLE) destroySwapchain(chain);
        }
        PFN_xrDestroyActionSet destroyActionSet{};
        LoadProc(state.getProc, state.instance, "xrDestroyActionSet", destroyActionSet);
        if (destroyActionSet && state.handActionSet != XR_NULL_HANDLE) destroyActionSet(state.handActionSet);
        if (destroySession && state.session != XR_NULL_HANDLE) destroySession(state.session);
        if (destroyInstance) destroyInstance(state.instance);
    }
    // Keep the OpenXR loader resident for the lifetime of the game process.
    // SteamVR can still have loader-owned work in flight after a failed
    // xrCreateInstance attempt.  Unloading here made that work return through
    // openxr_loader.dll_unloaded and crashed JC3 before the scheduled retry.
    // LoadActiveOpenXrLoader() reuses the resident module on later attempts.
    state = {};
}

bool EnsureSbsSwapchains(RuntimeState& state, const DXGI_SWAP_CHAIN_DESC& desktopDesc, std::string& error) {
    if (state.sbsSwapchainsReady) return true;
    if (desktopDesc.BufferDesc.Width < 2 || desktopDesc.BufferDesc.Height == 0) { error = "Geo-11 SBS requires a non-empty even-width backbuffer"; return false; }
    uint32_t formatCount = 0;
    if (XR_FAILED(state.enumerateFormats(state.session, 0, &formatCount, nullptr)) || formatCount == 0) { error = "no OpenXR swapchain formats"; return false; }
    std::vector<int64_t> formats(formatCount);
    state.enumerateFormats(state.session, formatCount, &formatCount, formats.data());
    int64_t format = static_cast<int64_t>(desktopDesc.BufferDesc.Format);
    if (std::find(formats.begin(), formats.end(), format) == formats.end()) format = formats.front();
    state.geoSourceEyeWidth = desktopDesc.BufferDesc.Width / 2;
    state.geoSourceHeight = desktopDesc.BufferDesc.Height;
    // The SBS source is rendered with JC3's symmetric camera projection.  The
    // OpenXR target is deliberately rendered at its recommended resolution so
    // that each source ray can be remapped to the runtime's asymmetric frustum.
    state.sbsEyeWidth = state.recommendedEyeWidth ? state.recommendedEyeWidth : state.geoSourceEyeWidth;
    state.sbsEyeHeight = state.recommendedEyeHeight ? state.recommendedEyeHeight : state.geoSourceHeight;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = state.sbsEyeWidth;
    info.height = state.sbsEyeHeight;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    for (uint32_t eye = 0; eye != 2; ++eye) {
        if (XR_FAILED(state.createSwapchain(state.session, &info, &state.sbsSwapchains[eye]))) { error = "xrCreateSwapchain for Geo-11 SBS eye failed"; return false; }
        uint32_t imageCount = 0;
        if (XR_FAILED(state.enumerateImages(state.sbsSwapchains[eye], 0, &imageCount, nullptr))) { error = "xrEnumerateSwapchainImages count failed"; return false; }
        state.sbsImages[eye].resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(state.enumerateImages(state.sbsSwapchains[eye], imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(state.sbsImages[eye].data())))) { error = "xrEnumerateSwapchainImages for Geo-11 SBS eye failed"; return false; }
    }
    state.sbsSwapchainsReady = true;
    return true;
}

bool EnsureNativeAlternateResources(RuntimeState& state, ID3D11Device* device, const DXGI_SWAP_CHAIN_DESC& desktopDesc, std::string& error) {
    if (!device || desktopDesc.BufferDesc.Width == 0 || desktopDesc.BufferDesc.Height == 0) { error = "native alternate rendering requires a non-empty JC3 backbuffer"; return false; }
    const bool sourceMatches = state.nativeEyeSources[0] && state.nativeEyeSources[1] &&
        state.nativeSourceWidth == desktopDesc.BufferDesc.Width &&
        state.nativeSourceHeight == desktopDesc.BufferDesc.Height;
    if (state.nativeSwapchainsReady && sourceMatches) return true;

    // ResizeBuffers can change JC3's desktop image while the OpenXR session and
    // its recommended-size eye swapchains remain valid. CopyResource requires
    // identical texture dimensions, so rebuild only the two retained desktop
    // frames and invalidate their contents before accepting another copy.
    if (state.nativeSwapchainsReady) {
        TraceRuntime("native alternate source resize: old=" + std::to_string(state.nativeSourceWidth) + "x" +
                     std::to_string(state.nativeSourceHeight) + " new=" +
                     std::to_string(desktopDesc.BufferDesc.Width) + "x" +
                     std::to_string(desktopDesc.BufferDesc.Height));
        for (auto& texture : state.nativeEyeSources) texture.Reset();
        state.nativeEyeReady = {false, false};
        state.nativeEyeSourceProjections = {};
    }

    if (!state.nativeSwapchainsReady) {
    uint32_t formatCount = 0;
    if (XR_FAILED(state.enumerateFormats(state.session, 0, &formatCount, nullptr)) || formatCount == 0) { error = "no OpenXR swapchain formats"; return false; }
    std::vector<int64_t> formats(formatCount);
    state.enumerateFormats(state.session, formatCount, &formatCount, formats.data());
    int64_t format = static_cast<int64_t>(desktopDesc.BufferDesc.Format);
    // JC3's completed 8-bit backbuffer contains display-referred sRGB values.
    // Prefer an sRGB OpenXR swapchain so sampling and target writes perform the
    // matching decode/encode instead of making the compositor gamma-encode the
    // already encoded game image a second time.
    DXGI_FORMAT preferredSrgb = DXGI_FORMAT_UNKNOWN;
    switch (desktopDesc.BufferDesc.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: preferredSrgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: preferredSrgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: preferredSrgb = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; break;
    default: break;
    }
    if (preferredSrgb != DXGI_FORMAT_UNKNOWN &&
        std::find(formats.begin(), formats.end(), static_cast<int64_t>(preferredSrgb)) != formats.end()) {
        format = static_cast<int64_t>(preferredSrgb);
    }
    if (std::find(formats.begin(), formats.end(), format) == formats.end()) format = formats.front();
    state.nativeSwapchainFormat = static_cast<DXGI_FORMAT>(format);
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = state.recommendedEyeWidth;
    info.height = state.recommendedEyeHeight;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    for (uint32_t eye = 0; eye != 2; ++eye) {
        if (XR_FAILED(state.createSwapchain(state.session, &info, &state.nativeSwapchains[eye]))) { error = "native alternate OpenXR eye swapchain creation failed"; return false; }
        uint32_t imageCount = 0;
        if (XR_FAILED(state.enumerateImages(state.nativeSwapchains[eye], 0, &imageCount, nullptr))) { error = "native alternate eye image count failed"; return false; }
        state.nativeImages[eye].resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(state.enumerateImages(state.nativeSwapchains[eye], imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(state.nativeImages[eye].data())))) { error = "native alternate eye image enumeration failed"; return false; }
    }
    state.nativeSwapchainsReady = true;
    TraceRuntime("native alternate OpenXR swapchain format=" + std::to_string(static_cast<uint32_t>(state.nativeSwapchainFormat)) +
                 " desktop_format=" + std::to_string(static_cast<uint32_t>(desktopDesc.BufferDesc.Format)));
    }

    state.nativeSourceWidth = desktopDesc.BufferDesc.Width;
    state.nativeSourceHeight = desktopDesc.BufferDesc.Height;
    D3D11_TEXTURE2D_DESC source{};
    source.Width = state.nativeSourceWidth;
    source.Height = state.nativeSourceHeight;
    source.MipLevels = 1;
    source.ArraySize = 1;
    source.Format = desktopDesc.BufferDesc.Format;
    source.SampleDesc.Count = 1;
    source.Usage = D3D11_USAGE_DEFAULT;
    for (auto& texture : state.nativeEyeSources) {
        if (FAILED(device->CreateTexture2D(&source, nullptr, &texture))) { error = "native alternate eye-retention texture creation failed"; return false; }
    }
    Log("native alternate resources ready: retaining full JC3 frames " + std::to_string(state.nativeSourceWidth) + "x" + std::to_string(state.nativeSourceHeight) + " for two eyes; OpenXR targets=" + std::to_string(state.recommendedEyeWidth) + "x" + std::to_string(state.recommendedEyeHeight));
    return true;
}

bool CompileGeoResampleShaders(RuntimeState::GeoResampleResources& resources, ID3D11Device* device, std::string& error) {
    static constexpr const char* shaderSource = R"(
Texture2D SourceTexture : register(t0);
SamplerState SourceSampler : register(s0);
cbuffer RayMapping : register(b0) {
    float SourceM00;
    float SourceM11;
    float SourceM20;
    float SourceM21;
    float TangentLeft;
    float TangentRight;
    float TangentUp;
    float TangentDown;
    float SourceScaleX;
    float SourceOffsetX;
    float Padding0;
    float Padding1;
    row_major float3x3 SourceFromTarget;
};
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VertexOutput VS(uint vertexId : SV_VertexID) {
    const float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    const float2 uvs[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
float4 PS(VertexOutput input) : SV_Target {
    const float3 targetRay = float3(lerp(TangentLeft, TangentRight, input.uv.x), lerp(TangentUp, TangentDown, input.uv.y), -1.0);
    const float3 sourceRay = mul(SourceFromTarget, targetRay);
    if (sourceRay.z > -0.01) return float4(0.0, 0.0, 0.0, 1.0);
    const float rayX = sourceRay.x / -sourceRay.z;
    const float rayY = sourceRay.y / -sourceRay.z;
    const float sourceU = saturate(0.5 * (SourceM00 * rayX - SourceM20 + 1.0));
    const float sourceV = saturate(0.5 * (1.0 - SourceM11 * rayY + SourceM21));
    return SourceTexture.Sample(SourceSampler, float2(SourceOffsetX + SourceScaleX * sourceU, sourceV));
}
)";
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> compilerErrors;
    HRESULT result = D3DCompile(shaderSource, std::strlen(shaderSource), "jc3vr_geo_ray_resample.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vertexBlob, &compilerErrors);
    if (FAILED(result)) {
        error = "Geo SBS vertex-shader compilation failed";
        if (compilerErrors) error += ": " + std::string(static_cast<const char*>(compilerErrors->GetBufferPointer()), compilerErrors->GetBufferSize());
        return false;
    }
    result = D3DCompile(shaderSource, std::strlen(shaderSource), "jc3vr_geo_ray_resample.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &pixelBlob, &compilerErrors);
    if (FAILED(result)) {
        error = "Geo SBS pixel-shader compilation failed";
        if (compilerErrors) error += ": " + std::string(static_cast<const char*>(compilerErrors->GetBufferPointer()), compilerErrors->GetBufferSize());
        return false;
    }
    if (FAILED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &resources.vertexShader)) ||
        FAILED(device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &resources.pixelShader))) {
        error = "Geo SBS ray-resample shader creation failed";
        return false;
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler, &resources.sampler))) { error = "Geo SBS sampler creation failed"; return false; }
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(GeoResampleConstants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&constants, nullptr, &resources.constants))) { error = "Geo SBS ray-mapping constant buffer creation failed"; return false; }
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer, &resources.rasterizer))) { error = "Geo SBS rasterizer creation failed"; return false; }
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend, &resources.blend))) { error = "Geo SBS blend-state creation failed"; return false; }
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depth, &resources.depthStencil))) { error = "Geo SBS depth-state creation failed"; return false; }
    return true;
}

bool EnsureGeoResampleResources(RuntimeState& state, ID3D11Device* device, const DXGI_SWAP_CHAIN_DESC& desktopDesc, std::string& error) {
    auto& resources = state.geoResample;
    if (desktopDesc.SampleDesc.Count != 1) { error = "Geo SBS ray-resample does not yet support multisampled desktop backbuffers"; return false; }
    if (resources.sourceTexture && resources.sourceWidth == desktopDesc.BufferDesc.Width && resources.sourceHeight == desktopDesc.BufferDesc.Height && resources.sourceFormat == desktopDesc.BufferDesc.Format) return true;
    resources = {};
    D3D11_TEXTURE2D_DESC source{};
    source.Width = desktopDesc.BufferDesc.Width;
    source.Height = desktopDesc.BufferDesc.Height;
    source.MipLevels = 1;
    source.ArraySize = 1;
    source.Format = desktopDesc.BufferDesc.Format;
    DXGI_FORMAT sourceViewFormat = source.Format;
    switch (source.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        source.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        sourceViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        source.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        sourceViewFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        break;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        source.Format = DXGI_FORMAT_B8G8R8X8_TYPELESS;
        sourceViewFormat = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        break;
    default: break;
    }
    source.SampleDesc.Count = 1;
    source.Usage = D3D11_USAGE_DEFAULT;
    source.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{};
    sourceView.Format = sourceViewFormat;
    sourceView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sourceView.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&source, nullptr, &resources.sourceTexture)) ||
        FAILED(device->CreateShaderResourceView(resources.sourceTexture.Get(), &sourceView, &resources.sourceView))) {
        error = "Geo SBS source texture/SRV creation failed";
        resources = {};
        return false;
    }
    if (!CompileGeoResampleShaders(resources, device, error)) { resources = {}; return false; }
    resources.sourceWidth = source.Width;
    resources.sourceHeight = source.Height;
    resources.sourceFormat = desktopDesc.BufferDesc.Format;
    return true;
}

bool EnsureGeoResampleOutput(RuntimeState& state, ID3D11Device* device, ID3D11Texture2D* openXrImage, std::string& error) {
    auto& resources = state.geoResample;
    D3D11_TEXTURE2D_DESC targetDesc{};
    openXrImage->GetDesc(&targetDesc);
    if (targetDesc.SampleDesc.Count != 1 || targetDesc.ArraySize != 1) {
        error = "OpenXR eye image has an unsupported sample count or array size";
        return false;
    }
    // SteamVR's OpenXR D3D11 images can be typeless resources.  They are valid
    // copy destinations, but a typeless texture cannot be used as a render
    // target.  Render to the matching typed member of the same format family,
    // then CopyResource it into the acquired OpenXR image.
    DXGI_FORMAT renderFormat = targetDesc.Format;
    switch (renderFormat) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        renderFormat = state.nativeSwapchainFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        renderFormat = state.nativeSwapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
            ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        renderFormat = state.nativeSwapchainFormat == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
            ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
        break;
    default: break;
    }
    if (resources.outputTexture && resources.outputWidth == targetDesc.Width && resources.outputHeight == targetDesc.Height && resources.outputFormat == renderFormat) return true;
    resources.outputTexture.Reset();
    resources.outputTarget.Reset();
    D3D11_TEXTURE2D_DESC output{};
    output.Width = targetDesc.Width;
    output.Height = targetDesc.Height;
    output.MipLevels = 1;
    output.ArraySize = 1;
    output.Format = renderFormat;
    output.SampleDesc.Count = 1;
    output.Usage = D3D11_USAGE_DEFAULT;
    output.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT result = device->CreateTexture2D(&output, nullptr, &resources.outputTexture);
    if (FAILED(result)) {
        error = "Geo SBS intermediate output texture creation failed (HRESULT " + std::to_string(static_cast<long>(result)) + ")";
        return false;
    }
    result = device->CreateRenderTargetView(resources.outputTexture.Get(), nullptr, &resources.outputTarget);
    if (FAILED(result)) {
        error = "Geo SBS intermediate RTV creation failed (HRESULT " + std::to_string(static_cast<long>(result)) + ", format " + std::to_string(static_cast<uint32_t>(targetDesc.Format)) + ")";
        resources.outputTexture.Reset();
        return false;
    }
    resources.outputWidth = output.Width;
    resources.outputHeight = output.Height;
    resources.outputFormat = output.Format;
    TraceRuntime("OpenXR intermediate output: acquired format=" + std::to_string(static_cast<uint32_t>(targetDesc.Format)) + " render format=" + std::to_string(static_cast<uint32_t>(renderFormat)));
    return true;
}

void EndEmptyOpenXrFrame(RuntimeState& state, const XrFrameState& frameState) {
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ObserveRuntimeResult(state.endFrame(state.session, &endInfo));
}

void RenderHandMarkers(RuntimeState& state, ID3D11Device* device, ID3D11DeviceContext* context, const XrPosef& eyePose, const XrFovf& fov);
bool CaptureRetainedEye(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, const std::string& path);

XrVector3f RotateBy(const XrQuaternionf& q, const XrVector3f& v);

// JC3 composites its HUD in screen space onto each eye image, and the eye
// images use off-axis projections (m20 = -/+0.24): the same HUD pixel was a
// different direction in each eye (~16 deg apart). The HUD is isolated as the
// pixels that changed between the pre-HUD copy and the final frame, and
// re-drawn over the pre-HUD scene with a per-eye shift that puts it at
// g_hudDistance straight ahead (du = -0.5*(m00*e/D + m20), dv = 0.5*m21 in
// UV) and scaled toward the centre.
bool ComposeHudForEye(RuntimeState& state, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer,
                      ID3D11Texture2D* before, ID3D11Texture2D* eyeTarget, uint32_t eye, const NativeEyeProjection& projection, float ipd,
                      const XrQuaternionf& eyeOrientation, const XrVector3f* aimDirection, const XrVector3f* secondAim) {
    if (state.hudFailed) return false;
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    if (!state.hudPixelShader) {
        const auto& shader=kHudCompositeShader;
        Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, coverage, menu;
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 64;
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "jc3vr_hud.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, nullptr)) ||
            FAILED(D3DCompile(shader, sizeof(shader) - 1, "jc3vr_hud.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, nullptr)) ||
            FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &state.hudVertexShader)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &state.hudPixelShader)) ||
            FAILED(device->CreateBuffer(&constants, nullptr, &state.hudConstants)) ||
            FAILED(device->CreateSamplerState(&sampler, &state.hudSampler))) {
            state.hudFailed = true;
            TraceRuntime("HUD re-placement disabled: shader/resource creation failed");
            return false;
        }
        // Menu detection is optional: without it the HUD path is unchanged.
        D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION, 0};
        D3D11_BLEND_DESC noWrite{};
        noWrite.RenderTarget[0].RenderTargetWriteMask = 0;
        if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "jc3vr_hud.hlsl", nullptr, nullptr, "PSCoverage", "ps_5_0", 0, 0, &coverage, nullptr)) ||
            FAILED(device->CreatePixelShader(coverage->GetBufferPointer(), coverage->GetBufferSize(), nullptr, &state.hudCoveragePixelShader)) ||
            FAILED(device->CreateQuery(&query, &state.hudCoverageQuery)) || FAILED(device->CreateBlendState(&noWrite, &state.hudNoColorWrite)) ||
            FAILED(D3DCompile(shader, sizeof(shader) - 1, "jc3vr_hud.hlsl", nullptr, nullptr, "PSMenu", "ps_5_0", 0, 0, &menu, nullptr)) ||
            FAILED(device->CreatePixelShader(menu->GetBufferPointer(), menu->GetBufferSize(), nullptr, &state.hudMenuPixelShader))) {
            state.hudCoveragePixelShader.Reset();
            TraceRuntime("HUD menu detection unavailable (coverage query creation failed)");
        }
    }
    D3D11_TEXTURE2D_DESC existing{};
    if (state.hudAfter) state.hudAfter->GetDesc(&existing);
    if (!state.hudAfter || existing.Width != desc.Width || existing.Height != desc.Height || existing.Format != desc.Format) {
        state.hudAfter.Reset();
        state.hudAfterView.Reset();
        state.hudOut.Reset();
        state.hudOutTarget.Reset();
        D3D11_TEXTURE2D_DESC textureDesc = desc;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.CPUAccessFlags = 0;
        textureDesc.MiscFlags = 0;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC outDesc = textureDesc;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&textureDesc, nullptr, &state.hudAfter)) ||
            FAILED(device->CreateShaderResourceView(state.hudAfter.Get(), nullptr, &state.hudAfterView)) ||
            FAILED(device->CreateTexture2D(&outDesc, nullptr, &state.hudOut)) ||
            FAILED(device->CreateRenderTargetView(state.hudOut.Get(), nullptr, &state.hudOutTarget))) {
            state.hudFailed = true;
            TraceRuntime("HUD re-placement disabled: texture creation failed format=" + std::to_string(desc.Format));
            return false;
        }
    }
    if (state.hudBeforeTexture != before) {
        state.hudBeforeView.Reset();
        state.hudBeforeTexture = nullptr;
        if (FAILED(device->CreateShaderResourceView(before, nullptr, &state.hudBeforeView))) return false;
        state.hudBeforeTexture = before;
    }
    const float width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height);
    const float eyeX = (eye == 0 ? -0.5f : 0.5f) * ipd;
    const float distance = std::max(0.3f, g_hudDistance);
    const float scale = std::clamp(g_hudScale, 0.3f, 1.0f);
    float constants[16]{-0.5f * width * (projection.m00 * eyeX / distance + projection.m20), 0.5f * height * projection.m21, scale, 3.0f / 255.0f,
        width, height, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const bool replacedStatus=g_wristHud && g_motionControllers && state.wristConstants && !state.wristResourcesFailed;
    constants[6]=replacedStatus && state.handPoseActive[0]?1.f:0.f;
    constants[7]=replacedStatus && state.handPoseActive[1]?1.f:0.f;
    // Aim direction in this eye image: rotate into the eye's view frame (the
    // image is displayed with this pose), then project with the eye's own
    // projection (u = 0.5*(m00*x - m20 + 1)).
    const auto place = [&](const XrVector3f* direction, float* out) {
        if (!direction) return;
        const XrQuaternionf inverse{-eyeOrientation.x, -eyeOrientation.y, -eyeOrientation.z, eyeOrientation.w};
        const XrVector3f view = RotateBy(inverse, *direction);
        if (view.z >= -0.1f) return;
        const float tx = view.x / -view.z, ty = view.y / -view.z;
        out[0] = width * 0.5f * (projection.m00 * tx - projection.m20 + 1.0f);
        out[1] = height * 0.5f * (1.0f - projection.m11 * ty + projection.m21);
        out[2] = 0.045f * width;
        out[3] = 1.0f;
    };
    place(aimDirection, constants + 8);
    place(secondAim, constants + 12);
    context->CopyResource(state.hudAfter.Get(), backbuffer);
    state.wristNativeHudFrame=state.frameCount;
    D3D11_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
    ID3D11RenderTargetView* target = state.hudOutTarget.Get();
    ID3D11ShaderResourceView* views[2] = {state.hudBeforeView.Get(), state.hudAfterView.Get()};
    ID3D11Buffer* constantBuffer = state.hudConstants.Get();
    ID3D11SamplerState* sampler = state.hudSampler.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(state.hudVertexShader.Get(), nullptr, 0);
    context->PSSetShader(state.hudPixelShader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 2, views);
    context->PSSetSamplers(0, 1, &sampler);
    context->UpdateSubresource(constantBuffer, 0, nullptr, constants, 0, 0);
    context->PSSetConstantBuffers(0, 1, &constantBuffer);
    SetInternalRenderPass(true);
    if (state.hudCoveragePixelShader) {
        // Previous measurement first (non-blocking), then a new one.
        if (state.hudCoveragePending) {
            UINT64 samples = 0;
            if (context->GetData(state.hudCoverageQuery.Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                state.hudCoveragePending = false;
                state.hudCoverage = static_cast<float>(static_cast<double>(samples) / (static_cast<double>(width) * height));
                static std::uint32_t coverageReads = 0;
                if (++coverageReads % 600 == 1)
                    TraceRuntime("HUD coverage " + std::to_string(state.hudCoverage) + " paused=" + (GameSimulationPaused() ? "1" : "0") + " menu=" + (state.hudMenuMode ? "1" : "0"));
                // Hysteresis: three reads over 35 % enter menu mode, three
                // under 20 % leave it, so a big explosion or splash cannot flip it.
                if (state.hudCoverage > 0.35f) state.hudMenuVotes = std::min(state.hudMenuVotes + 1, 3);
                else if (state.hudCoverage < 0.20f) state.hudMenuVotes = std::max(state.hudMenuVotes - 1, -3);
                // A full-screen layer alone is not a menu: the near-death
                // filter recolours the whole screen while the game runs
                // (user 2026-10-02). Menus also pause the simulation.
                const bool paused = GameSimulationPaused();
                const bool menu = paused && (state.hudMenuMode ? state.hudMenuVotes > -3 : state.hudMenuVotes >= 3);
                if (menu != state.hudMenuMode) {
                    state.hudMenuMode = menu;
                    TraceRuntime(std::string("HUD: full-screen menu ") + (menu ? "detected, frame shown unshrunk" : "closed, HUD re-placement resumed") +
                        " (coverage " + std::to_string(state.hudCoverage) + ")");
                }
            }
        }
        if (!state.hudCoveragePending) {
            context->PSSetShader(state.hudCoveragePixelShader.Get(), nullptr, 0);
            context->OMSetBlendState(state.hudNoColorWrite.Get(), nullptr, 0xffffffffu);
            context->Begin(state.hudCoverageQuery.Get());
            context->Draw(3, 0);
            context->End(state.hudCoverageQuery.Get());
            state.hudCoveragePending = true;
            context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
            context->PSSetShader(state.hudPixelShader.Get(), nullptr, 0);
        }
    }
    if (state.hudMenuMode && !GameSimulationPaused()) {
        state.hudMenuMode = false;  // the game resumed: back to the HUD at once
        state.hudMenuVotes = 0;
        TraceRuntime("HUD: simulation resumed, HUD re-placement resumed");
    }
    g_menuOpen = state.hudMenuMode;
    if (state.hudMenuMode) {
        // Menus: the whole frame as a screen g_menuDistance away at
        // g_menuScale of the view, no relocated crosshairs.
        const float menuDistance = std::max(0.5f, g_menuDistance);
        float menu[16]{-0.5f * width * (projection.m00 * eyeX / menuDistance + projection.m20), 0.5f * height * projection.m21,
            std::clamp(g_menuScale, 0.2f, 1.0f), 0.0f, width, height};
        context->UpdateSubresource(constantBuffer, 0, nullptr, menu, 0, 0);
        context->PSSetShader(state.hudMenuPixelShader.Get(), nullptr, 0);
        context->Draw(3, 0);
        SetInternalRenderPass(false);
        ID3D11ShaderResourceView* none[2]{};
        context->PSSetShaderResources(0, 2, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(eyeTarget, state.hudOut.Get());
        return true;
    }
    context->Draw(3, 0);
    SetInternalRenderPass(false);
    ID3D11ShaderResourceView* noViews[2]{};
    context->PSSetShaderResources(0, 2, noViews);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(eyeTarget, state.hudOut.Get());
    static std::uint64_t composed = 0;
    if (++composed == 1 || composed % 1800 == 0)
        TraceRuntime("HUD re-placed for eye " + std::to_string(eye) + " shift_px=[" + std::to_string(constants[0]) + "," + std::to_string(constants[1]) +
            "] scale=" + std::to_string(scale) + " distance_m=" + std::to_string(distance) + " frames=" + std::to_string(composed));
    return true;
}

bool RetainScopeScene(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* source,RuntimeState::ScopeScene& retained,uint64_t frame,std::uintptr_t weapon) {
    retained.frame=0;
    if(!source) return false;
    D3D11_TEXTURE2D_DESC desc{},old{};source->GetDesc(&desc);
    if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.MipLevels!=1) return false;
    DXGI_FORMAT viewFormat=desc.Format;
    switch(desc.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;viewFormat=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        desc.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS;viewFormat=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;break;
    default: return false;
    }
    if(retained.texture) retained.texture->GetDesc(&old);
    if(!retained.texture || !retained.view || old.Width!=desc.Width || old.Height!=desc.Height || old.Format!=desc.Format) {
        retained.view.Reset();retained.texture.Reset();desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags=0;desc.MiscFlags=0;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=viewFormat;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&retained.texture)) ||
           FAILED(device->CreateShaderResourceView(retained.texture.Get(),&view,&retained.view))) return false;
    }
    context->CopyResource(retained.texture.Get(),source);retained.frame=frame;retained.weapon=weapon;return true;
}

void RenderRifleScope(RuntimeState& state,ID3D11Device* device,ID3D11DeviceContext* context,
                      const XrPosef& eye,const XrFovf& fov,const CameraProjectionSample& sourceProjection,const XrQuaternionf& sourceOrientation,ID3D11ShaderResourceView* scene) {
    const auto weapon=GetVrSniperWeapon();
    const auto optic=GetVrSniperOpticMount(weapon);
    static bool sourceProbeArmed=false;
    if(weapon && optic.valid && !sourceProbeArmed) {sourceProbeArmed=true;BeginScopeSourceProbe(weapon);}
    if(!weapon || !optic.valid || g_menuOpen || !HasNativeGameplayCamera() || !state.handPoseActive[1] || !state.handAimActive[1] || state.scopeResourcesFailed) {
        PublishVrScopeReady(0);return;
    }
    // A missing/offscreen opposite-eye capture must not revoke a valid
    // monocular optic and intermittently hand the zoom click back to JC3.
    if(!scene) return;
    if(!state.scopeConstants) {
        Microsoft::WRL::ComPtr<ID3DBlob> vs,ps,errors;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=9*16;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(D3DCompile(kRifleScopeShader,sizeof(kRifleScopeShader)-1,"jc3vr_scope.hlsl",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&errors)) ||
           FAILED(D3DCompile(kRifleScopeShader,sizeof(kRifleScopeShader)-1,"jc3vr_scope.hlsl",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&errors)) ||
           FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&state.scopeVertexShader)) ||
           FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&state.scopePixelShader)) ||
           FAILED(device->CreateBuffer(&desc,nullptr,&state.scopeConstants))) {
            state.scopeResourcesFailed=true;PublishVrScopeReady(0);
            TraceRuntime("VR scope: GPU resource failure; native sniper mode preserved");return;
        }
        TraceRuntime("VR scope: mesh-fitted sniper eyepiece + native placed-body grip offset; retained-image angular magnifier, NOT separate scene camera");
    }
    const auto orientation=state.handAimPoses[1].orientation;
    const auto grip=state.handLocations[1].pose.position;
    auto barrel=RotateBy(orientation,{0,0,-1});const auto handUp=RotateBy(orientation,{0,1,0});
    if(state.supportFrameValid) barrel={state.supportForward[0],state.supportForward[1],state.supportForward[2]};
    const float p[3]{grip.x,grip.y,grip.z},b[3]{barrel.x,barrel.y,barrel.z},h[3]{handUp.x,handUp.y,handUp.z};
    float l[3]{},r[3]{},u[3]{},f[3]{};
    if(!BuildScopeLensFrame(p,b,h,l,r,u,f,&optic,g_jc3WorldUnitsPerMeter)) {PublishVrScopeReady(0);return;}
    const XrVector3f lens{l[0],l[1],l[2]},right{r[0],r[1],r[2]},up{u[0],u[1],u[2]},forward{f[0],f[1],f[2]};
    const XrQuaternionf inverse{-eye.orientation.x,-eye.orientation.y,-eye.orientation.z,eye.orientation.w};
    const XrQuaternionf sourceInverse{-sourceOrientation.x,-sourceOrientation.y,-sourceOrientation.z,sourceOrientation.w};
    const XrVector3f centre=RotateBy(inverse,{lens.x-eye.position.x,lens.y-eye.position.y,lens.z-eye.position.z});
    const auto rightEye=RotateBy(inverse,right),upEye=RotateBy(inverse,up);
    const float radius=optic.radius/g_jc3WorldUnitsPerMeter;
    if(!std::isfinite(radius) || radius<0.003f || radius>0.05f ||
       centre.z+radius*(std::abs(rightEye.z)+std::abs(upEye.z))>=-0.01f) {return;}
    const float ep[3]{eye.position.x,eye.position.y,eye.position.z},lp[3]{lens.x,lens.y,lens.z},fp[3]{forward.x,forward.y,forward.z};
    const float visibility=ScopeEyeVisibility(ep,lp,fp,radius);
    const float relief=(lens.x-eye.position.x)*forward.x+(lens.y-eye.position.y)*forward.y+(lens.z-eye.position.z)*forward.z;
    const auto sr=RotateBy(sourceInverse,right),su=RotateBy(sourceInverse,up),sf=RotateBy(sourceInverse,forward);
    const float constants[36]{centre.x,centre.y,centre.z,0,rightEye.x,rightEye.y,rightEye.z,0,upEye.x,upEye.y,upEye.z,0,
        std::tan(fov.angleLeft),std::tan(fov.angleRight),std::tan(fov.angleUp),std::tan(fov.angleDown),
        sourceProjection.m00,sourceProjection.m11,sourceProjection.m20,sourceProjection.m21,
        sr.x,sr.y,sr.z,0,su.x,su.y,su.z,0,sf.x,sf.y,sf.z,0,radius,4.0f,relief,visibility};
    auto* cb=state.scopeConstants.Get();auto* sampler=state.geoResample.sampler.Get();
    context->UpdateSubresource(cb,0,nullptr,constants,0,0);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(state.scopeVertexShader.Get(),nullptr,0);context->PSSetShader(state.scopePixelShader.Get(),nullptr,0);
    context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
    context->PSSetShaderResources(0,1,&scene);context->PSSetSamplers(0,1,&sampler);
    SetInternalRenderPass(true);context->Draw(6,0);SetInternalRenderPass(false);
    PublishVrScopeReady(weapon); // only after a valid optical frame and lens draw
    ID3D11ShaderResourceView* none=nullptr;context->PSSetShaderResources(0,1,&none);
    if(state.frameCount%600==0) TraceRuntime("VR scope: eye-box visibility="+std::to_string(visibility)+" relief_m="+std::to_string(relief));
}

void RenderWristHud(RuntimeState& state,ID3D11Device* device,ID3D11DeviceContext* context,const XrPosef& eye,const XrFovf& fov) {
    if(!g_wristHud || !g_motionControllers || g_menuOpen ||
       UseFrontEndPanel(HasNativeGameplayCamera(),IsNativeSceneRendering()) || state.wristResourcesFailed) return;
    if(!state.wristConstants) {
        Microsoft::WRL::ComPtr<ID3DBlob> vs,ps,errors;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=7*16;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_BLEND_DESC blend{};blend.RenderTarget[0].BlendEnable=TRUE;
        blend.RenderTarget[0].SrcBlend=D3D11_BLEND_ONE;blend.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp=blend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;blend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(FAILED(D3DCompile(kWristHudShader,sizeof(kWristHudShader)-1,"jc3vr_wrist.hlsl",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&errors)) ||
           FAILED(D3DCompile(kWristHudShader,sizeof(kWristHudShader)-1,"jc3vr_wrist.hlsl",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&errors)) ||
           FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&state.wristVertexShader)) ||
           FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&state.wristPixelShader)) ||
           FAILED(device->CreateBuffer(&desc,nullptr,&state.wristConstants)) || FAILED(device->CreateBlendState(&blend,&state.wristBlend))) {
            state.wristResourcesFailed=true;TraceRuntime("Wrist HUD resource failure; native HUD unchanged");return;
        }
        TraceRuntime("Wrist HUD GPU ready: transparent native status, right beside gun / left above controller; head-facing shared frame; duplicated status regions removed");
    }
    const XrQuaternionf inverse{-eye.orientation.x,-eye.orientation.y,-eye.orientation.z,eye.orientation.w};
    const bool nativeStatus=state.wristNativeHudFrame==state.frameCount && state.hudAfterView && state.hudBeforeView && !state.hudMenuMode;
    D3D11_TEXTURE2D_DESC nativeDesc{};
    if(nativeStatus) state.hudAfter->GetDesc(&nativeDesc);
    for(unsigned hand=0;hand<2;++hand) {
        // No placeholder health panel: only expose independently verified data.
        if(hand==0 && !nativeStatus) continue;
        if(!state.handPoseActive[hand]) continue;
        const auto pose=state.handLocations[hand].pose;
        const auto anchorOrientation=state.handAimActive[hand]?state.handAimPoses[hand].orientation:pose.orientation;
        const auto offset=RotateBy(anchorOrientation,hand==1?XrVector3f{-.16f,.10f,-.12f}:XrVector3f{-.03f,.12f,.03f});
        const XrVector3f centre{pose.position.x+offset.x,pose.position.y+offset.y,pose.position.z+offset.z};
        const XrVector3f toHead{state.wristHeadCentre.x-centre.x,state.wristHeadCentre.y-centre.y,state.wristHeadCentre.z-centre.z};
        // Same head-centre visibility gate in both eyes. No per-eye flicker.
        const float toward[3]{toHead.x,toHead.y,toHead.z};float rr[3]{},uu[3]{};
        if(!ReadableHudFrame(toward,rr,uu)) continue;
        const XrVector3f right{rr[0],rr[1],rr[2]},up{uu[0],uu[1],uu[2]};
        const auto c=RotateBy(inverse,{centre.x-eye.position.x,centre.y-eye.position.y,centre.z-eye.position.z});
        const auto r=RotateBy(inverse,right),u=RotateBy(inverse,up);
        const float width=.09f,height=nativeStatus?.09f:.055f;
        if(c.z+width*std::abs(r.z)+height*std::abs(u.z)>-.01f) continue;
        ID3D11ShaderResourceView* icon=nullptr;
        const auto found=state.iconTextures.find(state.wristWeaponIcon);
        if(hand==1 && found!=state.iconTextures.end()) icon=found->second.Get();
        const auto aspect=state.iconAspects.find(state.wristWeaponIcon);
        const float iconAspect=aspect==state.iconAspects.end()?1.0f:aspect->second;
        const auto& hud=state.wristSnapshot;
        // Isolated fixed safe-area regions; directional POIs/reticles stay
        // on the existing world/head HUD. Menus never enter these panels.
        const float rect[4]{hand==0?.015f:.76f,hand==0?.015f:.73f,hand==0?.34f:.995f,hand==0?.34f:.995f};
        const float sourceAspect=nativeStatus?((rect[2]-rect[0])*nativeDesc.Width)/((rect[3]-rect[1])*nativeDesc.Height):1.f;
        const float constants[28]{c.x,c.y,c.z,nativeStatus?1.f:0.f,r.x,r.y,r.z,3.f/255.f,u.x,u.y,u.z,sourceAspect,
            std::tan(fov.angleLeft),std::tan(fov.angleRight),std::tan(fov.angleUp),std::tan(fov.angleDown),
            static_cast<float>(hud.magazine),static_cast<float>(hud.reserve),hand==1?static_cast<float>(hud.capacity):hud.health,static_cast<float>(hand),width,height,icon?1.0f:0.0f,iconAspect,rect[0],rect[1],rect[2],rect[3]};
        auto* cb=state.wristConstants.Get();auto* sampler=state.geoResample.sampler.Get();auto* target=state.geoResample.outputTarget.Get();
        context->OMSetRenderTargets(1,&target,nullptr);
        context->OMSetBlendState(state.wristBlend.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(state.geoResample.depthStencil.Get(),0);
        context->RSSetState(state.geoResample.rasterizer.Get());
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(state.wristVertexShader.Get(),nullptr,0);context->PSSetShader(state.wristPixelShader.Get(),nullptr,0);
        context->UpdateSubresource(cb,0,nullptr,constants,0,0);context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
        ID3D11ShaderResourceView* views[3]{icon,nativeStatus?state.hudAfterView.Get():nullptr,nativeStatus?state.hudBeforeView.Get():nullptr};
        context->PSSetShaderResources(0,3,views);context->PSSetSamplers(0,1,&sampler);
        SetInternalRenderPass(true);context->Draw(6,0);SetInternalRenderPass(false);
        ID3D11ShaderResourceView* none[3]{};context->PSSetShaderResources(0,3,none);
        if(nativeStatus && state.frameCount%600==0)
            TraceRuntime("Wrist native status: hand="+std::to_string(hand)+" source="+std::to_string(nativeDesc.Width)+"x"+std::to_string(nativeDesc.Height)+
                " rect="+std::to_string(rect[0])+","+std::to_string(rect[1])+","+std::to_string(rect[2])+","+std::to_string(rect[3])+" aspect="+std::to_string(sourceAspect));
    }
}

bool RenderNativeEyeToOpenXr(RuntimeState& state, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* nativeSource, ID3D11Texture2D* openXrImage, const CameraProjectionSample& sourceProjection, const NativeEyeProjection& projection, const XrPosef& eyePose, const XrFovf& fov, std::string& error, const XrQuaternionf* sourceOrientation = nullptr,ID3D11ShaderResourceView* scopeScene=nullptr) {
    if (!EnsureGeoResampleOutput(state, device, openXrImage, error)) return false;
    // The retained source is a completed JC3 render.  Its ray directions must
    // be decoded with JC3's projection as it was actually rendered, never the
    // OpenXR projection we may use for a future native eye render.
    if (!sourceProjection.valid) {
        error = "final JC3 source projection is unavailable for the OpenXR image remap";
        return false;
    }
    if (!state.sourceCoverageLogged) {
        state.sourceCoverageLogged = true;
        const float sourceLeft = (-1.0f + sourceProjection.m20) / sourceProjection.m00;
        const float sourceRight = (1.0f + sourceProjection.m20) / sourceProjection.m00;
        const float sourceUp = (1.0f + sourceProjection.m21) / sourceProjection.m11;
        const float sourceDown = (-1.0f + sourceProjection.m21) / sourceProjection.m11;
        const float targetLeft = std::tan(fov.angleLeft);
        const float targetRight = std::tan(fov.angleRight);
        const float targetUp = std::tan(fov.angleUp);
        const float targetDown = std::tan(fov.angleDown);
        const float horizontalSpan = targetRight - targetLeft;
        const float verticalSpan = targetUp - targetDown;
        const float coveredHorizontal = std::max(0.0f, std::min(sourceRight, targetRight) - std::max(sourceLeft, targetLeft));
        const float coveredVertical = std::max(0.0f, std::min(sourceUp, targetUp) - std::max(sourceDown, targetDown));
        const float firstValidV = verticalSpan > 0.0f ? (targetUp - std::min(sourceUp, targetUp)) / verticalSpan : 0.0f;
        const float lastValidV = verticalSpan > 0.0f ? (targetUp - std::max(sourceDown, targetDown)) / verticalSpan : 1.0f;
        std::ostringstream coverage;
        coverage << std::fixed << std::setprecision(4)
                 << "render-path source-ray coverage: JC3=" << state.geoResample.sourceWidth << "x" << state.geoResample.sourceHeight
                 << " XR=" << state.recommendedEyeWidth << "x" << state.recommendedEyeHeight
                 << " XR_max=" << state.maximumEyeWidth << "x" << state.maximumEyeHeight
                 << " source_tangents=[L " << sourceLeft << ",R " << sourceRight << ",U " << sourceUp << ",D " << sourceDown << "]"
                 << " target_tangents=[L " << targetLeft << ",R " << targetRight << ",U " << targetUp << ",D " << targetDown << "]"
                 << " ray_coverage=[H " << (horizontalSpan > 0.0f ? coveredHorizontal / horizontalSpan : 0.0f)
                 << ",V " << (verticalSpan > 0.0f ? coveredVertical / verticalSpan : 0.0f) << "]"
                 << " valid_target_v=[" << firstValidV << "," << lastValidV << "]"
                 << " outside rays are currently clamped by saturate(), producing repeated edge texels";
        TraceRuntime(coverage.str());
    }
    const bool traceThisFrame = state.frameCount < 3 || state.frameCount % 600 == 0;
    if (traceThisFrame) {
        const D3D11_RECT sourceRect{0, 0, static_cast<LONG>(state.geoResample.sourceWidth), static_cast<LONG>(state.geoResample.sourceHeight)};
        const D3D11_RECT destinationRect{0, 0, static_cast<LONG>(state.recommendedEyeWidth), static_cast<LONG>(state.recommendedEyeHeight)};
        TraceRuntime("render-path native eye=" + std::to_string(projection.eye) +
                     " copy-source=" + DescribeTexture(nativeSource) +
                     " intermediate-source=" + DescribeTexture(state.geoResample.sourceTexture.Get()) +
                     " intermediate-output=" + DescribeTexture(state.geoResample.outputTexture.Get()) +
                     " xr-image=" + DescribeTexture(openXrImage) +
                     " src_rect=" + DescribeRect(sourceRect) +
                     " dst_rect=" + DescribeRect(destinationRect) +
                     " uv_scale_offset=[1,0;1,0] source_projection=[" + std::to_string(sourceProjection.m00) + "," + std::to_string(sourceProjection.m11) + "," + std::to_string(sourceProjection.m20) + "," + std::to_string(sourceProjection.m21) + "]" +
                     " target_projection=[" + std::to_string(projection.m00) + "," + std::to_string(projection.m11) + "," + std::to_string(projection.m20) + "," + std::to_string(projection.m21) + "]" +
                     " xr_tangents=[" + std::to_string(std::tan(fov.angleLeft)) + "," + std::to_string(std::tan(fov.angleRight)) + "," + std::to_string(std::tan(fov.angleUp)) + "," + std::to_string(std::tan(fov.angleDown)) + "]");
        TraceRenderState(context, ("native-eye-pre-copy-" + std::to_string(projection.eye)).c_str(), nativeSource);
    }
    context->CopyResource(state.geoResample.sourceTexture.Get(), nativeSource);
    GeoResampleConstants constants{
        sourceProjection.m00,
        sourceProjection.m11,
        sourceProjection.m20,
        sourceProjection.m21,
        std::tan(fov.angleLeft),
        std::tan(fov.angleRight),
        std::tan(fov.angleUp),
        std::tan(fov.angleDown),
        1.0f,
        0.0f,
        0.0f,
        0.0f,
    };
    if (sourceOrientation) {
        // source_from_target = R_source^T * R_target (view -> tracking rotations).
        const XrVector3f targetAxes[3]{RotateBy(eyePose.orientation, {1.0f, 0.0f, 0.0f}), RotateBy(eyePose.orientation, {0.0f, 1.0f, 0.0f}), RotateBy(eyePose.orientation, {0.0f, 0.0f, 1.0f})};
        const XrVector3f sourceAxes[3]{RotateBy(*sourceOrientation, {1.0f, 0.0f, 0.0f}), RotateBy(*sourceOrientation, {0.0f, 1.0f, 0.0f}), RotateBy(*sourceOrientation, {0.0f, 0.0f, 1.0f})};
        for (int row = 0; row != 3; ++row)
            for (int column = 0; column != 3; ++column)
                constants.rotation[row * 4 + column] = sourceAxes[row].x * targetAxes[column].x + sourceAxes[row].y * targetAxes[column].y + sourceAxes[row].z * targetAxes[column].z;
    }
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(state.recommendedEyeWidth);
    viewport.Height = static_cast<float>(state.recommendedEyeHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ID3D11RenderTargetView* target = state.geoResample.outputTarget.Get();
    ID3D11ShaderResourceView* source = state.geoResample.sourceView.Get();
    ID3D11SamplerState* sampler = state.geoResample.sampler.Get();
    ID3D11Buffer* constantBuffer = state.geoResample.constants.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(state.geoResample.blend.Get(), nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(state.geoResample.depthStencil.Get(), 0);
    context->RSSetState(state.geoResample.rasterizer.Get());
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(state.geoResample.vertexShader.Get(), nullptr, 0);
    context->PSSetShader(state.geoResample.pixelShader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &source);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetConstantBuffers(0, 1, &constantBuffer);
    context->UpdateSubresource(constantBuffer, 0, nullptr, &constants, 0, 0);
    SetInternalRenderPass(true);
    context->Draw(3, 0);
    SetInternalRenderPass(false);
    RenderRifleScope(state,device,context,eyePose,fov,sourceProjection,sourceOrientation?*sourceOrientation:eyePose.orientation,scopeScene);
    RenderHandMarkers(state, device, context, eyePose, fov);
    RenderWristHud(state,device,context,eyePose,fov);
    if (state.finalHandCaptureFrame != state.frameCount) state.finalHandCapturePending = {};
    if (projection.eye < 2 && state.finalHandCapturePending[projection.eye]) {
        const auto path = state.finalHandCaptureStem + (projection.eye ? "_right.rgba" : "_left.rgba");
        const bool captured = CaptureRetainedEye(device, context, state.geoResample.outputTexture.Get(), path);
        state.finalHandCapturePending[projection.eye] = false;
        TraceRuntime("final tracked-hand eye capture eye=" + std::to_string(projection.eye) + " success=" + std::to_string(captured) + " path=" + path);
        if (!state.finalHandCapturePending[0] && !state.finalHandCapturePending[1]) {
            const std::string metadata = "width="+std::to_string(state.recommendedEyeWidth)+"\nheight="+std::to_string(state.recommendedEyeHeight)+
                "\nformat=DXGI_FORMAT_R8G8B8A8_UNORM\nrow_order=top_to_bottom\nphase=after_reprojection_and_tracked_hands\nsubmission_frame="+
                std::to_string(state.frameCount)+"\n";
            HANDLE file = CreateFileA((state.finalHandCaptureStem+".txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written{}; WriteFile(file,metadata.data(),static_cast<DWORD>(metadata.size()),&written,nullptr); CloseHandle(file);
            }
        }
    }
    if (g_handDebugPreview && projection.eye == 0 && !state.handDebugCaptureAttempted) {
        state.handDebugCaptureAttempted = true;
        char directory[MAX_PATH]{};
        const DWORD size = GetEnvironmentVariableA("LOCALAPPDATA", directory, MAX_PATH);
        if (size && size < MAX_PATH) {
            const std::string path = std::string(directory) + "\\JC3VR\\hand_debug_" +
                std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()) + "_eye0.rgba";
            TraceRuntime("synthetic hand eye-output capture=" + std::to_string(CaptureRetainedEye(device, context, state.geoResample.outputTexture.Get(), path)) +
                " path=" + path + " size=" + std::to_string(state.recommendedEyeWidth) + "x" + std::to_string(state.recommendedEyeHeight));
        }
    }
    if (traceThisFrame) TraceRenderState(context, ("native-eye-after-resample-" + std::to_string(projection.eye)).c_str(), state.geoResample.outputTexture.Get());
    ID3D11ShaderResourceView* noSource[] = {nullptr};
    context->PSSetShaderResources(0, 1, noSource);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(openXrImage, state.geoResample.outputTexture.Get());
    return true;
}

NativeEyeProjection ProjectionForView(uint32_t eye, const XrFovf& fov, float cameraLocalX) {
    const float left = std::tan(fov.angleLeft);
    const float right = std::tan(fov.angleRight);
    const float up = std::tan(fov.angleUp);
    const float down = std::tan(fov.angleDown);
    NativeEyeProjection result{};
    result.valid = std::abs(right - left) > 0.0001f && std::abs(up - down) > 0.0001f;
    result.eye = eye;
    result.m00 = 2.0f / (right - left);
    result.m11 = 2.0f / (up - down);
    // Row-major D3D projection with m23=-1; the signs preserve parallel
    // cameras while shifting the optical centre to this runtime eye's frustum.
    result.m20 = (left + right) / (right - left);
    result.m21 = (up + down) / (up - down);
    result.cameraLocalX = cameraLocalX;
    return result;
}

XrVector3f RotateBy(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f qv{q.x, q.y, q.z};
    const XrVector3f cross{qv.y * v.z - qv.z * v.y, qv.z * v.x - qv.x * v.z, qv.x * v.y - qv.y * v.x};
    const XrVector3f twiceCross{2.0f * cross.x, 2.0f * cross.y, 2.0f * cross.z};
    const XrVector3f qCross{qv.y * twiceCross.z - qv.z * twiceCross.y, qv.z * twiceCross.x - qv.x * twiceCross.z, qv.x * twiceCross.y - qv.y * twiceCross.x};
    return {v.x + q.w * twiceCross.x + qCross.x, v.y + q.w * twiceCross.y + qCross.y, v.z + q.w * twiceCross.z + qCross.z};
}

void SetDebugHandPoses(RuntimeState& state, const std::array<XrView, 2>& views) {
    if (!g_handDebugPreview) return;
    for (uint32_t hand = 0; hand < 2; ++hand) {
        if (state.handPoseActive[hand]) continue;
        const XrVector3f offset = RotateBy(views[0].pose.orientation,
            {hand == 0 ? -0.23f : 0.23f, -0.23f, -0.55f});
        auto& location = state.handLocations[hand];
        location = {XR_TYPE_SPACE_LOCATION};
        location.locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        // Held-controller orientation: grip frame = head rotated 80 deg about
        // X, so extended fingers (-Y) point forward and the thumb (-Z) up.
        const XrQuaternionf& h = views[0].pose.orientation;
        const float halfAngle = 0.5f * 1.3963f;
        const XrQuaternionf tilt{std::sin(halfAngle), 0.0f, 0.0f, std::cos(halfAngle)};
        location.pose.orientation = {
            h.w * tilt.x + h.x * tilt.w + h.y * tilt.z - h.z * tilt.y,
            h.w * tilt.y - h.x * tilt.z + h.y * tilt.w + h.z * tilt.x,
            h.w * tilt.z + h.x * tilt.y - h.y * tilt.x + h.z * tilt.w,
            h.w * tilt.w - h.x * tilt.x - h.y * tilt.y - h.z * tilt.z};
        // Demonstrate articulation: left open, right fist with index out.
        state.handCurl[hand] = hand == 0 ? HandCurl{0.1f, 0.2f, 0.0f} : HandCurl{0.0f, 1.0f, 0.6f};
        location.pose.position = {views[0].pose.position.x + offset.x,
            views[0].pose.position.y + offset.y,
            views[0].pose.position.z + offset.z};
        state.handPoseActive[hand] = true;
    }
    if (state.frameCount == 0) TraceRuntime("synthetic hand poses active for one-shot renderer validation; not controller tracking");
}

struct HandVertex {
    float x, y, z, w;
    float r, g, b, a;
};

bool EnsureHandVisualResources(RuntimeState& state, ID3D11Device* device) {
    if (state.handVertexBuffer) return true;
    static constexpr char shader[] = R"(
struct Input { float4 position : POSITION; float4 color : COLOR; };
struct Output { float4 position : SV_Position; float4 color : COLOR; };
Output VS(Input input) {
    Output output;
    output.position = input.position;
    output.color = input.color;
    return output;
}
float4 PS(Output input) : SV_Target { return input.color; }
)";
    Microsoft::WRL::ComPtr<ID3DBlob> vs;
    Microsoft::WRL::ComPtr<ID3DBlob> ps;
    if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "jc3vr_hands.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, nullptr)) ||
        FAILED(D3DCompile(shader, sizeof(shader) - 1, "jc3vr_hands.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, nullptr))) return false;
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(HandVertex) * 4096;
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &state.handVertexShader)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &state.handPixelShader)) ||
        FAILED(device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)), vs->GetBufferPointer(), vs->GetBufferSize(), &state.handInputLayout)) ||
        FAILED(device->CreateBlendState(&blend, &state.handBlend)) ||
        FAILED(device->CreateDepthStencilState(&depth, &state.handDepthState)) ||
        FAILED(device->CreateRasterizerState(&raster, &state.handRaster)) ||
        FAILED(device->CreateBuffer(&buffer, nullptr, &state.handVertexBuffer))) return false;
    TraceRuntime("3D VR hand GPU resources ready (depth-tested, two-sided lighting)");
    return true;
}

bool EnsureHandDepth(RuntimeState& state, ID3D11Device* device, UINT width, UINT height) {
    if (state.handDepthView && state.handDepthWidth == width && state.handDepthHeight == height) return true;
    state.handDepthView.Reset();
    state.handDepth.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &state.handDepth)) ||
        FAILED(device->CreateDepthStencilView(state.handDepth.Get(), nullptr, &state.handDepthView))) return false;
    state.handDepthWidth = width;
    state.handDepthHeight = height;
    return true;
}

bool EnsureControllerModels(RuntimeState& state, ID3D11Device* device, ID3D11DeviceContext* context) {
    if (state.controllerGpuReady) return true;
    if (state.controllerGpuFailed) return false;
    int expected = 0;
    if (g_controllerAssetState.compare_exchange_strong(expected, 1)) {
        std::thread([] {
            std::string errors;
            bool ok = true;
            for (int hand = 0; hand != 2; ++hand) {
                std::string error;
                if (!LoadQuestPlusControllerModel(hand == 0, g_controllerAssets[hand], error)) {
                    ok = false;
                    errors += (hand == 0 ? "left: " : "right: ") + error + "; ";
                }
            }
            TraceRuntime(ok ? "Quest Touch Plus render models loaded from the SteamVR install: vertices=" +
                                  std::to_string(g_controllerAssets[0].vertices.size()) + "/" + std::to_string(g_controllerAssets[1].vertices.size()) +
                                  " texture=" + std::to_string(g_controllerAssets[1].width) + "x" + std::to_string(g_controllerAssets[1].height)
                            : "Quest render models unavailable (" + errors + "); using the procedural controllers");
            g_controllerAssetState.store(ok ? 2 : 3);
        }).detach();
    }
    const int assets = g_controllerAssetState.load();
    if (assets == 3) { state.controllerGpuFailed = true; return false; }
    if (assets != 2) return false;
    static constexpr char shader[] = R"(
cbuffer Model : register(b0) { row_major float4x4 ModelToView; float4 Tangents; };
Texture2D Albedo : register(t0);
SamplerState Linear : register(s0);
struct Input { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
Output VS(Input input) {
    Output output;
    const float3 v = mul(float4(input.position, 1.0), ModelToView).xyz;
    const float w = -v.z;
    // Tangent-space frustum (left, right, up, down); depth n=0.03 m, f=100 m.
    output.position = float4((2.0 * v.x - (Tangents.y + Tangents.x) * w) / (Tangents.y - Tangents.x),
                             (2.0 * v.y - (Tangents.z + Tangents.w) * w) / (Tangents.z - Tangents.w),
                             1.0003 * w - 0.030009, w);
    output.normal = mul(float4(input.normal, 0.0), ModelToView).xyz;
    output.uv = input.uv;
    return output;
}
float4 PS(Output input) : SV_Target {
    const float3 n = normalize(input.normal);
    const float lambert = abs(dot(n, normalize(float3(0.30, 0.75, 0.59))));
    const float3 albedo = Albedo.Sample(Linear, input.uv).rgb;
    return float4(albedo * (0.35 + 0.75 * lambert), 1.0);
}
)";
    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps;
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 80;
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 8;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "jc3vr_controller.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, nullptr)) ||
        FAILED(D3DCompile(shader, sizeof(shader) - 1, "jc3vr_controller.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, nullptr)) ||
        FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &state.controllerVertexShader)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &state.controllerPixelShader)) ||
        FAILED(device->CreateInputLayout(layout, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &state.controllerInputLayout)) ||
        FAILED(device->CreateBuffer(&constants, nullptr, &state.controllerConstants)) ||
        FAILED(device->CreateSamplerState(&sampler, &state.controllerSampler))) {
        state.controllerGpuFailed = true;
        TraceRuntime("Quest render models disabled: shader creation failed");
        return false;
    }
    for (int hand = 0; hand != 2; ++hand) {
        const auto& asset = g_controllerAssets[hand];
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = static_cast<UINT>(asset.vertices.size() * sizeof(ControllerModelVertex));
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{asset.vertices.data(), 0, 0};
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = asset.width;
        texture.Height = asset.height;
        texture.MipLevels = 0;
        texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        texture.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> textureObject;
        if (FAILED(device->CreateBuffer(&buffer, &data, &state.controllerVertices[hand])) ||
            FAILED(device->CreateTexture2D(&texture, nullptr, &textureObject)) ||
            FAILED(device->CreateShaderResourceView(textureObject.Get(), nullptr, &state.controllerTextures[hand]))) {
            state.controllerGpuFailed = true;
            TraceRuntime("Quest render models disabled: GPU resource creation failed");
            return false;
        }
        context->UpdateSubresource(textureObject.Get(), 0, nullptr, asset.rgba.data(), asset.width * 4, 0);
        context->GenerateMips(state.controllerTextures[hand].Get());
        state.controllerVertexCount[hand] = static_cast<UINT>(asset.vertices.size());
    }
    state.controllerGpuReady = true;
    return true;
}

// Model space -> OpenXR grip frame: g = Rx(-s*theta) * (p - origin), with the
// "openxr_grip" component (theta = rotate_xyz.x). The sign convention s is
// measured once from the live grip->aim relation against the JSON's aim.
XrVector3f ControllerModelToGrip(const RenderModelComponent& grip, float sign, const XrVector3f& p) {
    const float angle = -sign * grip.rotateXyzDegrees[0] * 3.14159265f / 180.0f;
    const float c = std::cos(angle), s = std::sin(angle);
    const XrVector3f d{p.x - grip.origin[0], p.y - grip.origin[1], p.z - grip.origin[2]};
    return {d.x, c * d.y - s * d.z, s * d.y + c * d.z};
}

bool EnsureChargeModel(RuntimeState& state,ID3D11Device* device,ID3D11DeviceContext* context) {
    if(state.chargeVertexCount) return true;
    if(state.chargeGpuFailed || !state.controllerGpuReady) return false;
    int expected=0;
    if(g_chargeAssetState.compare_exchange_strong(expected,1)) {
        std::thread([] {
            bool okay=false;std::string error;
            try {
                wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
                const auto folder=std::filesystem::path(exe).parent_path()/L"mods"/L"jc3vr_assets";
                okay=LoadInstalledChargeAsset(folder.wstring(),g_chargeAsset,error);
            } catch(...) {error="charge cache loader exception";}
            TraceRuntime(okay?"C4: original w401 inactive charge cache loaded; authored geometry/diffuse, reconstructed normals":
                "C4: held original charge unavailable ("+error+"); native equipment retained, no box fallback");
            g_chargeAssetState.store(okay?2:3);
        }).detach();
    }
    const int loaded=g_chargeAssetState.load();
    if(loaded==3) {state.chargeGpuFailed=true;return false;}
    if(loaded!=2) return false;
    const auto& asset=g_chargeAsset;
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=static_cast<UINT>(asset.vertices.size()*sizeof(ControllerModelVertex));
    buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{asset.vertices.data(),0,0};
    D3D11_TEXTURE2D_DESC texture{};texture.Width=asset.width;texture.Height=asset.height;
    texture.MipLevels=1;texture.ArraySize=1;texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_IMMUTABLE;texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA pixels{asset.rgba.data(),asset.width*4,0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> object;
    if(FAILED(device->CreateBuffer(&buffer,&data,&state.chargeVertices)) ||
       FAILED(device->CreateTexture2D(&texture,&pixels,&object)) ||
       FAILED(device->CreateShaderResourceView(object.Get(),nullptr,&state.chargeTexture))) {
        state.chargeGpuFailed=true;TraceRuntime("C4: held-charge GPU allocation failed; native equipment retained");return false;
    }
    state.chargeVertexCount=static_cast<UINT>(asset.vertices.size());
    TraceRuntime("C4: original held-charge GPU mesh ready for dual-eye draw; provisional hand socket, native planting unchanged");
    return true;
}

struct WeaponIconAsset { std::vector<std::uint8_t> rgba; unsigned width=0,height=0; };
std::map<int,WeaponIconAsset> g_weaponIconAssets;
std::atomic<int> g_weaponIconLoadState{0};
struct IconVertex { float x,y,z,w,u,v,r,g,b,a; };

bool EnsureWeaponIcons(RuntimeState& state, ID3D11Device* device) {
    if (state.iconVertices) return true;
    int expected=0;
    if (g_weaponIconLoadState.compare_exchange_strong(expected,1)) {
        std::thread([] {
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr,exe,MAX_PATH);
            const auto folder=std::filesystem::path(exe).parent_path()/L"mods"/L"jc3vr_weapon_icons";
            std::error_code error;
            for (const auto& entry: std::filesystem::directory_iterator(folder,error)) {
                if (entry.path().extension()!=L".png") continue;
                const auto stem=entry.path().stem().string();
                if (stem.empty() || stem.find_first_not_of("0123456789")!=std::string::npos || stem.size()>4) continue;
                WeaponIconAsset asset;
                if (LoadRgbaPng(entry.path().wstring(),asset.rgba,asset.width,asset.height) &&
                    asset.width<=4096 && asset.height<=4096)
                    g_weaponIconAssets.emplace(std::stoi(stem),std::move(asset));
            }
            TraceRuntime("original JC3 weapon icons loaded="+std::to_string(g_weaponIconAssets.size()));
            g_weaponIconLoadState.store(g_weaponIconAssets.empty()?3:2);
        }).detach();
    }
    if (g_weaponIconLoadState.load()!=2) return false;
    const auto& shader=kWeaponIconShader;
    Microsoft::WRL::ComPtr<ID3DBlob> vs,ps;
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,"weapon_icons",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,nullptr)) ||
        FAILED(D3DCompile(shader,sizeof(shader)-1,"weapon_icons",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,nullptr))) return false;
    const D3D11_INPUT_ELEMENT_DESC layout[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
    D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=6*sizeof(IconVertex); buffer.Usage=D3D11_USAGE_DYNAMIC;
    buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER; buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sampler{}; sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD=D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC blend{}; auto& b=blend.RenderTarget[0]; b.BlendEnable=TRUE;
    b.SrcBlend=D3D11_BLEND_SRC_ALPHA; b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA; b.BlendOp=D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha=D3D11_BLEND_ONE; b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    // Create VB last: it is the all-resources-ready marker.
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&state.iconVertexShader)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&state.iconPixelShader)) ||
        FAILED(device->CreateInputLayout(layout,3,vs->GetBufferPointer(),vs->GetBufferSize(),&state.iconInputLayout)) ||
        FAILED(device->CreateSamplerState(&sampler,&state.iconSampler)) || FAILED(device->CreateBlendState(&blend,&state.iconBlend))) return false;
    for (const auto& [id,asset]:g_weaponIconAssets) {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=asset.width; desc.Height=asset.height;
        desc.MipLevels=desc.ArraySize=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{asset.rgba.data(),asset.width*4,0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (SUCCEEDED(device->CreateTexture2D(&desc,&data,&texture)) &&
            SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&view))) {
            state.iconTextures[id]=view;
            state.iconAspects[id]=static_cast<float>(asset.width)/asset.height;
        }
    }
    return SUCCEEDED(device->CreateBuffer(&buffer,nullptr,&state.iconVertices));
}

void RenderWeaponSelector(RuntimeState& state,ID3D11Device* device,ID3D11DeviceContext* context,
                          const XrPosef& eye,const XrFovf& fov) {
    if (!g_dpadSelecting || !EnsureWeaponIcons(state,device)) return;
    const float l=std::tan(fov.angleLeft),r=std::tan(fov.angleRight),u=std::tan(fov.angleUp),d=std::tan(fov.angleDown);
    const XrQuaternionf inverse{-eye.orientation.x,-eye.orientation.y,-eye.orientation.z,eye.orientation.w};
    const XrVector3f offsets[4]{{0,1,0},{0,-1,0},{-g_dpadRight.x,0,-g_dpadRight.z},g_dpadRight};
    context->IASetInputLayout(state.iconInputLayout.Get());
    context->VSSetShader(state.iconVertexShader.Get(),nullptr,0); context->PSSetShader(state.iconPixelShader.Get(),nullptr,0);
    context->OMSetBlendState(state.iconBlend.Get(),nullptr,0xffffffff);
    ID3D11SamplerState* sampler=state.iconSampler.Get(); context->PSSetSamplers(0,1,&sampler);
    ID3D11Buffer* buffer=state.iconVertices.Get(); const UINT stride=sizeof(IconVertex),offset=0;
    context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);
    for (int direction=0;direction<4;++direction) {
        const int id=g_selectorIconIds[direction];
        const auto texture=state.iconTextures.find(id); if (texture==state.iconTextures.end()) continue;
        const auto& asset=g_weaponIconAssets.at(id);
        const bool picked=direction==g_dpadDirection;
        for (int pass=0;pass<2;++pass) {
        const bool backing=pass==0;
        const auto corners=backing?WeaponIconBacking(asset.width,asset.height,picked):WeaponIconQuad(asset.width,asset.height,picked);
        IconVertex vertices[6]{}; bool valid=true;
        for (int i=0;i<6;++i) {
            const auto& c=corners[i]; const auto local=RotateBy(g_dpadOrientation,{c.x,c.y,0});
            const auto v=RotateBy(inverse,{g_dpadCentre.x+0.12f*offsets[direction].x+local.x-eye.position.x,
                g_dpadCentre.y+0.12f*offsets[direction].y+local.y-eye.position.y,
                g_dpadCentre.z+0.12f*offsets[direction].z+local.z-eye.position.z});
            const float w=-v.z; if (w<0.03f) { valid=false; break; }
            vertices[i]={(2*v.x-(r+l)*w)/(r-l),(2*v.y-(u+d)*w)/(u-d),(1.0003f+(backing?0.00001f:0.0f))*w-0.030009f,w,
                c.u,c.v,backing?0.0f:1.0f,backing?0.0f:(picked?0.75f:1.0f),backing?0.0f:(picked?0.1f:1.0f),backing?0.82f:1.0f};
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!valid || FAILED(context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) continue;
        std::memcpy(mapped.pData,vertices,sizeof(vertices)); context->Unmap(buffer,0);
        ID3D11ShaderResourceView* view=texture->second.Get(); context->PSSetShaderResources(0,1,&view); context->Draw(6,0);
        }
    }
    ID3D11ShaderResourceView* none=nullptr; context->PSSetShaderResources(0,1,&none);
}

void RenderHandMarkers(RuntimeState& state, ID3D11Device* device, ID3D11DeviceContext* context, const XrPosef& eyePose, const XrFovf& fov) {
    SetVrBodyTrackedGauntletReady(false);
    if (!g_motionControllers || (!state.handPoseActive[0] && !state.handPoseActive[1])) return;
    if (!EnsureHandVisualResources(state, device)) return;
    EnsureWeaponIcons(state,device); // prewarm before the user opens the selector
    const float left = std::tan(fov.angleLeft);
    const float right = std::tan(fov.angleRight);
    const float up = std::tan(fov.angleUp);
    const float down = std::tan(fov.angleDown);
    if (right - left < 0.01f || up - down < 0.01f) return;
    // Current eye target: the hands get their own depth buffer of its size.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    context->OMGetRenderTargets(1, &target, nullptr);
    if (!target) return;
    Microsoft::WRL::ComPtr<ID3D11Resource> targetResource;
    target->GetResource(&targetResource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture;
    if (FAILED(targetResource.As(&targetTexture))) return;
    D3D11_TEXTURE2D_DESC targetDesc{};
    targetTexture->GetDesc(&targetDesc);
    if (!EnsureHandDepth(state, device, targetDesc.Width, targetDesc.Height)) return;

    const XrQuaternionf inverseEye{-eyePose.orientation.x, -eyePose.orientation.y, -eyePose.orientation.z, eyePose.orientation.w};
    static std::vector<HandMeshVertex> mesh;
    static std::vector<HandVertex> vertices;
    vertices.clear();
    // Light from above and slightly toward the viewer, in view space.
    const XrVector3f light{0.30f, 0.75f, 0.59f};
    const auto emit = [&](const XrPosef& pose) {
        for (std::size_t first = 0; first + 3 <= mesh.size(); first += 3) {
            HandVertex triangle[3]{};
            bool visible = true;
            for (int corner = 0; corner != 3 && visible; ++corner) {
                const auto& source = mesh[first + corner];
                const XrVector3f rotated = RotateBy(pose.orientation, {source.position[0], source.position[1], source.position[2]});
                const XrVector3f relative{pose.position.x + rotated.x - eyePose.position.x,
                    pose.position.y + rotated.y - eyePose.position.y,
                    pose.position.z + rotated.z - eyePose.position.z};
                const XrVector3f view = RotateBy(inverseEye, relative);
                if (view.z >= -0.03f) { visible = false; break; }
                const XrVector3f normal = RotateBy(inverseEye, RotateBy(pose.orientation, {source.normal[0], source.normal[1], source.normal[2]}));
                const float lambert = std::abs(normal.x * light.x + normal.y * light.y + normal.z * light.z);
                const float shade = 0.38f + 0.62f * lambert;
                const float rayX = view.x / -view.z;
                const float rayY = view.y / -view.z;
                triangle[corner] = {(2.0f * rayX - right - left) / (right - left), (2.0f * rayY - up - down) / (up - down),
                    std::clamp(1.0003f - 0.030009f / -view.z, 0.0f, 0.9999f), 1.0f,
                    source.color[0] * shade, source.color[1] * shade, source.color[2] * shade, 1.0f};
                if (!std::isfinite(triangle[corner].x) || !std::isfinite(triangle[corner].y)) visible = false;
            }
            if (visible && vertices.size() + 3 <= 4096) vertices.insert(vertices.end(), triangle, triangle + 3);
        }
    };
    const bool models = !g_trackedHands && g_controllerModel && EnsureControllerModels(state, device, context);
    const bool heldCharge=models && GetVrC4Weapon() && std::isfinite(g_jc3WorldUnitsPerMeter) && g_jc3WorldUnitsPerMeter>0 &&
        !g_menuOpen && HasNativeGameplayCamera() &&
        state.handPoseActive[1] && state.handAimActive[1] && EnsureChargeModel(state,device,context);
    // Measure the rotate_xyz sign once: the runtime's aim forward in the grip
    // frame has y = sin(s*(theta_aim - theta_grip)) for the JSON's frames.
    if (models && state.controllerGripSign == 0.0f && state.handPoseActive[1] && state.handAimActive[1] && g_controllerAssets[1].aim.found) {
        const XrQuaternionf grip = state.handLocations[1].pose.orientation;
        const XrVector3f aimForward = RotateBy(state.handAimPoses[1].orientation, {0.0f, 0.0f, -1.0f});
        const XrVector3f inGrip = RotateBy({-grip.x, -grip.y, -grip.z, grip.w}, aimForward);
        const float expected = std::sin((g_controllerAssets[1].aim.rotateXyzDegrees[0] - g_controllerAssets[1].grip.rotateXyzDegrees[0]) * 3.14159265f / 180.0f);
        state.controllerGripSign = (inGrip.y * expected >= 0.0f) ? 1.0f : -1.0f;
        TraceRuntime("Quest render model grip convention: runtime aim-in-grip=[" + std::to_string(inGrip.x) + "," + std::to_string(inGrip.y) + "," +
            std::to_string(inGrip.z) + "] json sin(aim-grip)=" + std::to_string(expected) + " -> sign=" + std::to_string(state.controllerGripSign));
    }
    // Only the locally prepared original charge can replace the right-hand
    // controller visual. Native equipment/inventory remain engine-owned.
    for (uint32_t hand = 0; hand < 2 && !models; ++hand) {
        if (!state.handPoseActive[hand]) continue;
        if (!g_trackedHands && RicoNativeHandPrepared(hand)) continue;
        mesh.clear();
        if (g_trackedHands) {
            if (!state.handAimActive[hand]) continue;
            const auto& grip = state.handLocations[hand].pose.position;
            const auto& aim = state.handAimPoses[hand].orientation;
            const float p[3]{grip.x,grip.y,grip.z}, q[4]{aim.x,aim.y,aim.z,aim.w};
            float position[3], orientation[4];
            if (!BuildTrackedHandFrame(p,q,position,orientation)) continue;
            BuildHandMesh(hand == 0, state.handCurl[hand], mesh);
            for (auto& vertex : mesh) vertex.position[1] += kTrackedWristToPalm-0.05f;
            emit({{orientation[0],orientation[1],orientation[2],orientation[3]}, {position[0],position[1],position[2]}});
            continue;
        }
        if (g_controllerModel) BuildControllerMesh(hand == 0, state.handCurl[hand], mesh);
        else BuildHandMesh(hand == 0, state.handCurl[hand], mesh);
        emit(state.handLocations[hand].pose);
    }
    // Aim line from the hand that currently steers the engine camera:
    // orange while the camera is still swinging onto it, green when aligned.
    if (g_aimLines && g_handAim && g_activeAimHand >= 0 && state.handAimActive[g_activeAimHand]) {
        static const float kSwinging[3]{1.0f, 0.45f, 0.08f};
        static const float kAligned[3]{0.25f, 1.0f, 0.35f};
        mesh.clear();
        BuildAimBeam(1.5f, 0.006f, g_aimAligned ? kAligned : kSwinging, mesh);
        emit(state.handAimPoses[g_activeAimHand]);
    }
    if (vertices.empty() && !models && !g_dpadSelecting) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!vertices.empty()) {
        if (FAILED(context->Map(state.handVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(HandVertex));
        context->Unmap(state.handVertexBuffer.Get(), 0);
    }
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> previousRaster;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> previousDepth;
    UINT previousStencilRef = 0;
    context->RSGetState(&previousRaster);
    context->OMGetDepthStencilState(&previousDepth, &previousStencilRef);
    context->ClearDepthStencilView(state.handDepthView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11RenderTargetView* targets[1] = {target.Get()};
    context->OMSetRenderTargets(1, targets, state.handDepthView.Get());
    const UINT stride = sizeof(HandVertex);
    const UINT offset = 0;
    ID3D11Buffer* buffer = state.handVertexBuffer.Get();
    context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    context->IASetInputLayout(state.handInputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(state.handVertexShader.Get(), nullptr, 0);
    context->PSSetShader(state.handPixelShader.Get(), nullptr, 0);
    context->OMSetBlendState(state.handBlend.Get(), nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(state.handDepthState.Get(), 0);
    context->RSSetState(state.handRaster.Get());
    SetInternalRenderPass(true);
    if (!vertices.empty()) context->Draw(static_cast<UINT>(vertices.size()), 0);
    if (g_trackedHands && state.handPoseActive[0] && state.handAimActive[0] && !vertices.empty())
        SetVrBodyTrackedGauntletReady(true);
    if (models) {
        const float sign = state.controllerGripSign == 0.0f ? 1.0f : state.controllerGripSign;
        context->IASetInputLayout(state.controllerInputLayout.Get());
        context->VSSetShader(state.controllerVertexShader.Get(), nullptr, 0);
        context->PSSetShader(state.controllerPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* constants = state.controllerConstants.Get();
        ID3D11SamplerState* sampler = state.controllerSampler.Get();
        context->VSSetConstantBuffers(0, 1, &constants);
        context->PSSetSamplers(0, 1, &sampler);
        for (uint32_t hand = 0; hand < 2; ++hand) {
            if (!state.handPoseActive[hand] || !state.controllerVertexCount[hand]) continue;
            if (RicoNativeHandPrepared(hand)) continue;
            if(hand==1 && heldCharge) continue;
            auto pose = state.handLocations[hand].pose;
            if (hand==0 && state.supportFrameValid)
                pose.position={state.supportSocket[0],state.supportSocket[1],state.supportSocket[2]};
            const auto& grip = g_controllerAssets[hand].grip;
            // Row-vector model->view matrix: rows are the images of the model
            // axes, row 3 the image of the model origin.
            const auto toView = [&](const XrVector3f& p) {
                const XrVector3f g = ControllerModelToGrip(grip, sign, p);
                const XrVector3f rotated = RotateBy(pose.orientation, g);
                return RotateBy(inverseEye, {pose.position.x + rotated.x - eyePose.position.x,
                    pose.position.y + rotated.y - eyePose.position.y, pose.position.z + rotated.z - eyePose.position.z});
            };
            const XrVector3f origin = toView({0.0f, 0.0f, 0.0f});
            const XrVector3f ax = toView({1.0f, 0.0f, 0.0f}), ay = toView({0.0f, 1.0f, 0.0f}), az = toView({0.0f, 0.0f, 1.0f});
            const float modelConstants[20]{
                ax.x - origin.x, ax.y - origin.y, ax.z - origin.z, 0.0f,
                ay.x - origin.x, ay.y - origin.y, ay.z - origin.z, 0.0f,
                az.x - origin.x, az.y - origin.y, az.z - origin.z, 0.0f,
                origin.x, origin.y, origin.z, 1.0f,
                left, right, up, down};
            context->UpdateSubresource(constants, 0, nullptr, modelConstants, 0, 0);
            const UINT modelStride = sizeof(ControllerModelVertex);
            const UINT modelOffset = 0;
            ID3D11Buffer* modelBuffer = state.controllerVertices[hand].Get();
            ID3D11ShaderResourceView* texture = state.controllerTextures[hand].Get();
            context->IASetVertexBuffers(0, 1, &modelBuffer, &modelStride, &modelOffset);
            context->PSSetShaderResources(0, 1, &texture);
            context->Draw(state.controllerVertexCount[hand], 0);
        }
        if(heldCharge) {
            // Authored plate is X/Z with thickness along +Y. Map its face
            // normal to hand aim; positions stay LOCAL metres, not root-speed
            // compensated game space. Same pose and geometry in both eyes.
            const auto aim=state.handAimPoses[1].orientation;
            const auto grip=state.handLocations[1].pose.position;
            const auto toView=[&](XrVector3f p) {
                const float metres=1.0f/g_jc3WorldUnitsPerMeter;
                const auto local=RotateBy(aim,{p.x*metres,p.z*metres,-p.y*metres-0.04f});
                return RotateBy(inverseEye,{grip.x+local.x-eyePose.position.x,
                    grip.y+local.y-eyePose.position.y,grip.z+local.z-eyePose.position.z});
            };
            const auto origin=toView({0,0,0}),ax=toView({1,0,0}),ay=toView({0,1,0}),az=toView({0,0,1});
            const float modelConstants[20]{ax.x-origin.x,ax.y-origin.y,ax.z-origin.z,0,
                ay.x-origin.x,ay.y-origin.y,ay.z-origin.z,0,az.x-origin.x,az.y-origin.y,az.z-origin.z,0,
                origin.x,origin.y,origin.z,1,left,right,up,down};
            context->UpdateSubresource(constants,0,nullptr,modelConstants,0,0);
            const UINT stride=sizeof(ControllerModelVertex),offset=0;
            auto* vertices=state.chargeVertices.Get();auto* texture=state.chargeTexture.Get();
            context->IASetVertexBuffers(0,1,&vertices,&stride,&offset);context->PSSetShaderResources(0,1,&texture);
            context->Draw(state.chargeVertexCount,0);
        }
        ID3D11ShaderResourceView* noTexture = nullptr;
        context->PSSetShaderResources(0, 1, &noTexture);
    }
    RenderWeaponSelector(state,device,context,eyePose,fov);
    SetInternalRenderPass(false);
    context->OMSetRenderTargets(1, targets, nullptr);
    context->OMSetDepthStencilState(previousDepth.Get(), previousStencilRef);
    context->RSSetState(previousRaster.Get());
    if (state.frameCount == 0 || state.frameCount % 600 == 0)
        TraceRuntime("3D VR hands drawn vertices=" + std::to_string(vertices.size()) +
            " curl_left=[" + std::to_string(state.handCurl[0].index) + "," + std::to_string(state.handCurl[0].grip) + "]" +
            " curl_right=[" + std::to_string(state.handCurl[1].index) + "," + std::to_string(state.handCurl[1].grip) + "]");
}

XrFovf CentreFov(const std::array<XrView, 2>& views) {
    const float left = 0.5f * (std::tan(views[0].fov.angleLeft) + std::tan(views[1].fov.angleLeft));
    const float right = 0.5f * (std::tan(views[0].fov.angleRight) + std::tan(views[1].fov.angleRight));
    const float up = 0.5f * (std::tan(views[0].fov.angleUp) + std::tan(views[1].fov.angleUp));
    const float down = 0.5f * (std::tan(views[0].fov.angleDown) + std::tan(views[1].fov.angleDown));
    return {std::atan(left), std::atan(right), std::atan(up), std::atan(down)};
}

std::array<RuntimeState::NativeEyeCamera, 2> BuildNativeEyeRig(const std::array<XrView, 2>& views, XrTime predictedDisplayTime) {
    const XrVector3f eyeDelta{
        views[1].pose.position.x - views[0].pose.position.x,
        views[1].pose.position.y - views[0].pose.position.y,
        views[1].pose.position.z - views[0].pose.position.z,
    };
    const XrVector3f headRight = RotateBy(views[0].pose.orientation, {1.0f, 0.0f, 0.0f});
    const float lateralMeters = eyeDelta.x * headRight.x + eyeDelta.y * headRight.y + eyeDelta.z * headRight.z;
    const XrPosef centrePose{
        views[0].pose.orientation,
        {0.5f * (views[0].pose.position.x + views[1].pose.position.x), 0.5f * (views[0].pose.position.y + views[1].pose.position.y), 0.5f * (views[0].pose.position.z + views[1].pose.position.z)},
    };
    auto horizontalUnit = [](XrVector3f value, XrVector3f fallback) {
        value.y = 0.0f;
        const float length = std::sqrt(value.x * value.x + value.z * value.z);
        return length > 0.0001f ? XrVector3f{value.x / length, 0.0f, value.z / length} : fallback;
    };
    const XrVector3f currentForward = horizontalUnit(RotateBy(centrePose.orientation, {0.0f, 0.0f, -1.0f}), g_recenterForward);
    const XrVector3f currentRight = horizontalUnit(RotateBy(centrePose.orientation, {1.0f, 0.0f, 0.0f}), g_recenterRight);
    const bool recenterKeyDown = g_recenterKey > 0 && (GetAsyncKeyState(g_recenterKey) & 0x8000) != 0;
    const bool gameplayCameraReady = HasNativeGameplayCamera();
    // Automatic F2: the startup recenter happens in the menu (arbitrary head
    // pose, translation unarmed), and the user had to press F2 in every
    // session. Recenter once gameplay has been live for ~1 s after at least
    // ~1 s without it (startup, loads); short flickers do not re-trigger.
    static int gameplayFrames = 0;
    static int absentFrames = 1000;
    bool autoRecenter = false;
    if (gameplayCameraReady) {
        if (++gameplayFrames == 90 && absentFrames >= 90 && !g_headTranslationArmed) autoRecenter = true;
        if (gameplayFrames >= 90) absentFrames = 0;
    } else {
        gameplayFrames = 0;
        ++absentFrames;
    }
    const bool recenterRequested = (recenterKeyDown && !g_recenterKeyWasDown) || std::exchange(g_controllerRecenterRequest, false) || autoRecenter;
    g_recenterKeyWasDown = recenterKeyDown;
    if (autoRecenter) TraceRuntime("6DOF auto-recenter: gameplay camera live for 90 frames (automatic F2)");
    if (!g_recenterValid || recenterRequested) {
        g_recenterValid = true;
        if (recenterRequested) g_headTranslationArmed = true;
        if (recenterRequested) ++g_recenterCount;
        g_recenterHeadCentre = centrePose.position;
        g_recenterForward = currentForward;
        g_recenterRight = currentRight;
        TraceRuntime(std::string("6DOF recentered") + (recenterRequested ? " by key; positional translation armed" : " at first valid XR view; positional translation waits for recenter key") +
            ": head_m=[" + std::to_string(centrePose.position.x) + "," + std::to_string(centrePose.position.y) + "," + std::to_string(centrePose.position.z) +
            "] tracking_to_body right=[" + std::to_string(g_recenterRight.x) + ",0," + std::to_string(g_recenterRight.z) +
            "] forward=[" + std::to_string(g_recenterForward.x) + ",0," + std::to_string(g_recenterForward.z) + "]");
    }
    XrVector3f headDelta{
        centrePose.position.x - g_recenterHeadCentre.x,
        centrePose.position.y - g_recenterHeadCentre.y,
        centrePose.position.z - g_recenterHeadCentre.z,
    };
    if (!g_sixDof || !gameplayCameraReady || !g_headTranslationArmed) headDelta = {};
    float clamped[3]{headDelta.x,headDelta.y,headDelta.z};
    if(ClampRoomOffset(clamped,g_sixDofLimitMeters,g_sixDofVerticalLimitMeters))
        headDelta={clamped[0],clamped[1],clamped[2]};
    else headDelta={};
    const float headYawRadians = std::atan2(
        currentForward.x * g_recenterRight.x + currentForward.z * g_recenterRight.z,
        currentForward.x * g_recenterForward.x + currentForward.z * g_recenterForward.z);
    const XrFovf centreFov = CentreFov(views);
    std::array<RuntimeState::NativeEyeCamera, 2> rig{};
    // One seat snapshot for both eyes of this XR frame.
    auto vehicle=GetFirstPersonCameraAnchor(true);
    if(vehicle.traversal && g_bodyFrameValid) SetTraversalYawFrame(vehicle,g_bodyRight);
    for (uint32_t outputEye = 0; outputEye != 2; ++outputEye) {
        uint32_t sourceView = outputEye;
        if (g_nativeStereoRigMode == NativeStereoRigMode::Center) sourceView = 0;
        else if (g_nativeStereoRigMode == NativeStereoRigMode::LeftToBoth) sourceView = 0;
        else if (g_nativeStereoRigMode == NativeStereoRigMode::RightToBoth) sourceView = 1;
        else if (g_nativeStereoRigMode == NativeStereoRigMode::SwapEyes) sourceView = 1u - outputEye;
        rig[outputEye].sourceView = sourceView;
        rig[outputEye].pose = views[sourceView].pose;
        rig[outputEye].fov = views[sourceView].fov;
        rig[outputEye].cameraLocalX = 0.0f;
        XrVector3f offsetTracking{
            headDelta.x + views[sourceView].pose.position.x - centrePose.position.x,
            headDelta.y + views[sourceView].pose.position.y - centrePose.position.y,
            headDelta.z + views[sourceView].pose.position.z - centrePose.position.z,
        };
        if (g_nativeStereoRigMode == NativeStereoRigMode::Center) {
            rig[outputEye].pose = centrePose;
            rig[outputEye].fov = centreFov;
            rig[outputEye].cameraLocalX = 0.0f;
            offsetTracking = headDelta;
        }
        const float unitsPerMeter = g_jc3WorldUnitsPerMeter * g_sixDofScale;
        rig[outputEye].nativeOffset = {
            true,
            outputEye,
            unitsPerMeter * (offsetTracking.x * g_recenterRight.x + offsetTracking.z * g_recenterRight.z),
            unitsPerMeter * offsetTracking.y,
            unitsPerMeter * (offsetTracking.x * g_recenterForward.x + offsetTracking.z * g_recenterForward.z),
            // With the mouse bridge the engine basis already contains head
            // yaw, which must be removed; direct rotation leaves it out.
            g_directHeadRotation ? 0.0f : headYawRadians,
        };
        if (g_directHeadRotation && (g_handAim || g_followHead) && g_bodyFrameValid) {
            rig[outputEye].nativeOffset.bodyFrame = true;
            rig[outputEye].nativeOffset.bodyRight[0] = g_bodyRight[0];
            rig[outputEye].nativeOffset.bodyRight[2] = g_bodyRight[2];
        }
        if(vehicle.valid) {
            rig[outputEye].nativeOffset.baseAnchor=true;
            std::memcpy(rig[outputEye].nativeOffset.anchorPosition,vehicle.position,sizeof(vehicle.position));
            std::memcpy(rig[outputEye].nativeOffset.anchorBasis,vehicle.basis,sizeof(vehicle.basis));
        }
        if (g_directHeadRotation) {
            // This eye's orientation in the recentred, gravity-aligned body
            // frame (x=right, y=up, z=back), the same frame as its offset.
            rig[outputEye].nativeOffset.rotate = true;
            const XrQuaternionf orientation = rig[outputEye].pose.orientation;
            const XrVector3f axes[3]{RotateBy(orientation, {1.0f, 0.0f, 0.0f}), RotateBy(orientation, {0.0f, 1.0f, 0.0f}), RotateBy(orientation, {0.0f, 0.0f, 1.0f})};
            for (int axis = 0; axis != 3; ++axis) {
                rig[outputEye].nativeOffset.headAxes[axis * 3] = axes[axis].x * g_recenterRight.x + axes[axis].z * g_recenterRight.z;
                rig[outputEye].nativeOffset.headAxes[axis * 3 + 1] = axes[axis].y;
                rig[outputEye].nativeOffset.headAxes[axis * 3 + 2] = -(axes[axis].x * g_recenterForward.x + axes[axis].z * g_recenterForward.z);
            }
            // Diagnostic only: optional 0.5 Hz yaw sweep so a motionless
            // headset still produces a camera rotation that changes every frame.
            const float wobble = g_debugHeadYawWobbleRadians != 0.0f
                ? g_debugHeadYawWobbleRadians * std::sin(static_cast<float>(GetTickCount64() % 2000ull) / 2000.0f * 6.2831853f) : 0.0f;
            if (g_debugHeadYawRadians + wobble != 0.0f) {
                // Diagnostic only: a fixed extra head yaw (positive = right)
                // about body up, so rotation coverage can be verified from
                // draw readbacks without anyone turning their head.
                const float c = std::cos(g_debugHeadYawRadians + wobble);
                const float s = std::sin(g_debugHeadYawRadians + wobble);
                for (int axis = 0; axis != 3; ++axis) {
                    float* v = rig[outputEye].nativeOffset.headAxes + axis * 3;
                    const float x = v[0] * c - v[2] * s;
                    const float z = v[0] * s + v[2] * c;
                    v[0] = x;
                    v[2] = z;
                }
            }
        }
    }
    static uint64_t rigBuildCount = 0;
    ++rigBuildCount;
    if (rigBuildCount == 1 || rigBuildCount % 600 == 0) {
        TraceRuntime("native XR rig invariant: predicted_time=" + std::to_string(static_cast<int64_t>(predictedDisplayTime)) +
            " mode=" + ToString(g_nativeStereoRigMode) + " views=[0:left,1:right] lateral_m=" + std::to_string(lateralMeters) +
            " jc3_units_per_m=" + std::to_string(g_jc3WorldUnitsPerMeter) + " six_dof=" + (g_sixDof ? "true" : "false") +
            " head_translation_armed=" + (g_headTranslationArmed ? "true" : "false") +
            " head_delta_m=[" + std::to_string(headDelta.x) + "," + std::to_string(headDelta.y) + "," + std::to_string(headDelta.z) +
            "] body_ruf_units_left=[" + std::to_string(rig[0].nativeOffset.right) + "," + std::to_string(rig[0].nativeOffset.up) + "," + std::to_string(rig[0].nativeOffset.forward) +
            "] right=[" + std::to_string(rig[1].nativeOffset.right) + "," + std::to_string(rig[1].nativeOffset.up) + "," + std::to_string(rig[1].nativeOffset.forward) +
            "] image_fov_layer_pose_bound_together=true orientation_bridge=" +
            (g_directHeadRotation ? "direct_native_camera_same_frame" : "mouse_delta_next_game_frame"));
    }
    return rig;
}

OpenXrBootstrapResult SubmitGeo11SbsFrame(RuntimeState& state, ID3D11Texture2D* backbuffer, ID3D11DeviceContext* context, const XrFrameState& frameState, const std::array<XrView, 2>& views) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    backbuffer->GetDevice(&device);
    DXGI_SWAP_CHAIN_DESC unusedDesktopDesc{};
    D3D11_TEXTURE2D_DESC sourceDesc{};
    backbuffer->GetDesc(&sourceDesc);
    unusedDesktopDesc.BufferDesc.Width = sourceDesc.Width;
    unusedDesktopDesc.BufferDesc.Height = sourceDesc.Height;
    unusedDesktopDesc.BufferDesc.Format = sourceDesc.Format;
    unusedDesktopDesc.SampleDesc = sourceDesc.SampleDesc;
    std::string resampleError;
    const auto cameraProjection = GetLatestCameraProjection();
    const bool canRayResample = cameraProjection.valid && device && EnsureGeoResampleResources(state, device.Get(), unusedDesktopDesc, resampleError);
    if (canRayResample) context->CopyResource(state.geoResample.sourceTexture.Get(), backbuffer);

    uint32_t imageIndices[2]{};
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    for (uint32_t eye = 0; eye != 2; ++eye) {
        const XrResult acquireResult = ObserveRuntimeResult(state.acquireImage(state.sbsSwapchains[eye], &acquireInfo, &imageIndices[eye]));
        if (XR_FAILED(acquireResult)) {
            backbuffer->Release();
            EndEmptyOpenXrFrame(state, frameState);
            return {false, "Geo-11 SBS OpenXR eye acquire failed: " + std::to_string(acquireResult)};
        }
        const XrResult waitResult = ObserveRuntimeResult(state.waitImage(state.sbsSwapchains[eye], &waitInfo));
        if (XR_FAILED(waitResult)) {
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            ObserveRuntimeResult(state.releaseImage(state.sbsSwapchains[eye], &releaseInfo));
            backbuffer->Release();
            EndEmptyOpenXrFrame(state, frameState);
            return {false, "Geo-11 SBS OpenXR eye wait failed: " + std::to_string(waitResult)};
        }
        const uint32_t sourceEye = SourceEyeForRuntimeEye(eye);
        if (canRayResample) {
            if (!EnsureGeoResampleOutput(state, device.Get(), state.sbsImages[eye][imageIndices[eye]].texture, resampleError)) {
                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                ObserveRuntimeResult(state.releaseImage(state.sbsSwapchains[eye], &releaseInfo));
                backbuffer->Release();
                EndEmptyOpenXrFrame(state, frameState);
                return {false, resampleError};
            }
            const GeoResampleConstants constants{
                cameraProjection.m00,
                cameraProjection.m11,
                0.0f,
                0.0f,
                std::tan(views[eye].fov.angleLeft),
                std::tan(views[eye].fov.angleRight),
                std::tan(views[eye].fov.angleUp),
                std::tan(views[eye].fov.angleDown),
                0.5f,
                0.5f * static_cast<float>(sourceEye),
                0.0f,
                0.0f,
            };
            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(state.sbsEyeWidth);
            viewport.Height = static_cast<float>(state.sbsEyeHeight);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            ID3D11RenderTargetView* target = state.geoResample.outputTarget.Get();
            ID3D11ShaderResourceView* source = state.geoResample.sourceView.Get();
            ID3D11SamplerState* sampler = state.geoResample.sampler.Get();
            ID3D11Buffer* constantBuffer = state.geoResample.constants.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            context->OMSetBlendState(state.geoResample.blend.Get(), nullptr, 0xffffffffu);
            context->OMSetDepthStencilState(state.geoResample.depthStencil.Get(), 0);
            context->RSSetState(state.geoResample.rasterizer.Get());
            context->RSSetViewports(1, &viewport);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(state.geoResample.vertexShader.Get(), nullptr, 0);
            context->PSSetShader(state.geoResample.pixelShader.Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, &source);
            context->PSSetSamplers(0, 1, &sampler);
            context->PSSetConstantBuffers(0, 1, &constantBuffer);
            context->UpdateSubresource(constantBuffer, 0, nullptr, &constants, 0, 0);
            SetInternalRenderPass(true);
            context->Draw(3, 0);
            SetInternalRenderPass(false);
            ID3D11ShaderResourceView* noSource[] = {nullptr};
            context->PSSetShaderResources(0, 1, noSource);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            context->CopyResource(state.sbsImages[eye][imageIndices[eye]].texture, state.geoResample.outputTexture.Get());
        } else {
            // Retain a visible frame if the live camera projection is not yet
            // available (for example, during a loading screen), but make the
            // fallback explicit in the log instead of silently hiding it.
            D3D11_BOX sourceBox{};
            sourceBox.left = sourceEye * state.geoSourceEyeWidth;
            sourceBox.right = sourceBox.left + state.geoSourceEyeWidth;
            sourceBox.bottom = state.geoSourceHeight;
            sourceBox.back = 1;
            context->CopySubresourceRegion(state.sbsImages[eye][imageIndices[eye]].texture, 0, 0, 0, 0, backbuffer, 0, &sourceBox);
        }
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ObserveRuntimeResult(state.releaseImage(state.sbsSwapchains[eye], &releaseInfo));
    }
    if (g_geo11LogExtractionEveryFrame) {
        const uint64_t frame = state.frameCount + 1;
        const uint32_t leftSource = SourceEyeForRuntimeEye(0);
        const uint32_t rightSource = SourceEyeForRuntimeEye(1);
        Log("Geo SBS extraction frame=" + std::to_string(frame)
            + ": OpenXR left<-Geo " + EyeName(leftSource) + " sourceRect=[x=" + std::to_string(leftSource * state.geoSourceEyeWidth) + ".." + std::to_string((leftSource + 1) * state.geoSourceEyeWidth - 1) + ", y=0.." + std::to_string(state.geoSourceHeight - 1) + "]"
            + "; OpenXR right<-Geo " + EyeName(rightSource) + " sourceRect=[x=" + std::to_string(rightSource * state.geoSourceEyeWidth) + ".." + std::to_string((rightSource + 1) * state.geoSourceEyeWidth - 1) + ", y=0.." + std::to_string(state.geoSourceHeight - 1) + "]"
            + "; destinationRect=[x=0.." + std::to_string(state.sbsEyeWidth - 1) + ", y=0.." + std::to_string(state.sbsEyeHeight - 1) + "]"
            + (canRayResample ? "; ray-resample uses source symmetric projection -> runtime asymmetric eye FOV; no source crop/offset" : "; RAW COPY FALLBACK: " + (cameraProjection.valid ? resampleError : std::string("JC3 camera projection unavailable"))));
    }
    backbuffer->Release();
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    for (uint32_t eye = 0; eye != 2; ++eye) {
        projectionViews[eye].pose = views[eye].pose;
        projectionViews[eye].fov = views[eye].fov;
        projectionViews[eye].subImage.swapchain = state.sbsSwapchains[eye];
        projectionViews[eye].subImage.imageRect.extent = {static_cast<int32_t>(state.sbsEyeWidth), static_cast<int32_t>(state.sbsEyeHeight)};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = state.localSpace;
    layer.viewCount = static_cast<uint32_t>(projectionViews.size());
    layer.views = projectionViews.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = frameState.shouldRender ? 1u : 0u;
    endInfo.layers = frameState.shouldRender ? layers : nullptr;
    if (XR_FAILED(ObserveRuntimeResult(state.endFrame(state.session, &endInfo)))) return {false, "Geo-11 SBS xrEndFrame failed"};
    ++state.frameCount;
    if (state.frameCount == 1 || state.frameCount % 600 == 0) return {true, "submitted Geo-11 SBS images as distinct OpenXR left/right projection views"};
    return {true, "Geo-11 SBS OpenXR frame submitted"};
}

bool CaptureRetainedEye(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, const std::string& path) {
    if (!device || !context || !source) return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0 || desc.Width > 8192 || desc.Height > 8192 ||
        desc.SampleDesc.Count != 1 || (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { context->Unmap(staging.Get(), 0); return false; }
    const DWORD rowBytes = desc.Width * 4;
    bool success = true;
    for (UINT row = 0; row < desc.Height; ++row) {
        DWORD written = 0;
        const auto* pixels = static_cast<const BYTE*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch;
        if (!WriteFile(file, pixels, rowBytes, &written, nullptr) || written != rowBytes) { success = false; break; }
    }
    CloseHandle(file);
    context->Unmap(staging.Get(), 0);
    return success;
}

void CaptureRetainedPair(RuntimeState& state, ID3D11Device* device, ID3D11DeviceContext* context) {
    // F3 requests an extra pair (at most six) once the player is in-world;
    // the original automatic one-shot at frame 3000 usually lands in a menu.
    static bool captureKeyWasDown = false;
    static int keyCaptures = 0;
    const SHORT captureKeyState=GetAsyncKeyState(VK_F3);
    const bool captureKeyDown = (captureKeyState & 0x8000) != 0;
    const bool forced = std::exchange(g_forcedCapture, false);
    // Keep the held-key edge guard; also accept Windows' best-effort press bit
    // for taps between XR frames. It is not the sole input/readiness signal.
    const bool keyRequested = forced || (g_nativeEyeCapture && (captureKeyState & 0x8001) && !captureKeyWasDown && keyCaptures < 6);
    captureKeyWasDown = captureKeyDown;
    if (!g_nativeEyeCapture || !state.nativeEyeReady[0] || !state.nativeEyeReady[1]) return;
    if (!keyRequested && (state.nativeEyeCaptureAttempted || state.frameCount < 3000)) return;
    if (keyRequested && !forced) ++keyCaptures;
    else state.nativeEyeCaptureAttempted = true;
    char localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    const std::string directory = std::string(localAppData) + "\\JC3VR";
    CreateDirectoryA(directory.c_str(), nullptr);
    const std::string stem = directory + "\\native_eye_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
    if (keyRequested) {
        state.finalHandCaptureStem = stem + "_final";
        state.finalHandCaptureFrame = state.frameCount;
        state.finalHandCapturePending = {true,true};
    }
    const bool left = CaptureRetainedEye(device, context, state.nativeEyeSources[0].Get(), stem + "_left.rgba");
    const bool right = CaptureRetainedEye(device, context, state.nativeEyeSources[1].Get(), stem + "_right.rgba");
    std::ostringstream metadata;
    metadata << "width=" << state.nativeSourceWidth << "\nheight=" << state.nativeSourceHeight
             << "\nformat=DXGI_FORMAT_R8G8B8A8_UNORM\nrow_order=top_to_bottom\n"
             << "left_retained_frame=" << state.nativeEyeRetainedAtFrame[0]
             << "\nright_retained_frame=" << state.nativeEyeRetainedAtFrame[1]
             << "\nsubmission_frame=" << state.frameCount
             << "\nprojection_uploads=" << GetNativeProjectionWriteCount()
             << "\nleft_m20=" << state.nativeEyeSourceProjections[0].m20
             << "\nright_m20=" << state.nativeEyeSourceProjections[1].m20
             << "\ntrigger=" << (forced ? g_forcedCaptureTag : keyRequested ? std::string("F3") : std::string("auto"))
             // Engine (aim) camera vs the steering target (head or aiming hand)
             // at capture time: JC3 decides some terrain visibility from it.
             << "\nengine_vs_target_yaw_deg=" << g_steerYawErrorDeg << "\nengine_vs_target_pitch_deg=" << g_steerPitchErrorDeg
             << "\nengine_forward=" << g_steerEngineForward[0] << "," << g_steerEngineForward[1] << "," << g_steerEngineForward[2]
             << "\nsteering_suppressed=" << (g_steerSuppressed ? 1 : 0) << "\nsteering_backoffs=" << g_steerBackoffs
             << "\naim_hand=" << g_activeAimHand << "\nculling_mode=" << GetCullingMode();
    if (RicoNativeHandsEnabled())
        metadata << "\nrico_native_hand_palette_prepared=" << RicoNativeHandPrepared(0) << "," << RicoNativeHandPrepared(1);
    if (forced) TraceRuntime("forced capture " + g_forcedCaptureTag + " -> " + stem);
    for (int eye = 0; eye != 2; ++eye) {
        const auto& p = state.retainedEyeProjections[eye];
        const auto& c = state.retainedEyeCameras[eye];
        metadata << "\n" << (eye ? "right" : "left") << "_requested_projection=" << p.m00 << "," << p.m11 << "," << p.m20 << "," << p.m21
                 << "\n" << (eye ? "right" : "left") << "_fov_rad=" << c.fov.angleLeft << "," << c.fov.angleRight << "," << c.fov.angleUp << "," << c.fov.angleDown
                 << "\n" << (eye ? "right" : "left") << "_pose_pos=" << c.pose.position.x << "," << c.pose.position.y << "," << c.pose.position.z
                 << "\n" << (eye ? "right" : "left") << "_native_offset_ruf=" << c.nativeOffset.right << "," << c.nativeOffset.up << "," << c.nativeOffset.forward;
    }
    metadata << "\n";
    HANDLE file = CreateFileA((stem + ".txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const auto value = metadata.str();
        DWORD written = 0;
        WriteFile(file, value.data(), static_cast<DWORD>(value.size()), &written, nullptr);
        CloseHandle(file);
    }
    TraceRuntime("native retained-eye capture left=" + std::to_string(left) + " right=" + std::to_string(right) + " stem=" + stem);
}

// Hand aim: initialize the body yaw from the engine camera, then steer the
// engine (aim) camera toward the right-hand pointing ray with relative mouse
// input. Only in-world, with the cursor hidden and JC3 in the foreground, so
// menus and other windows never receive it.
void UpdateHandAim(RuntimeState& state, bool recenterRequested) {
    // F7 toggles head-follow steering live (safety while it is unproven).
    static bool followConfigured = g_followHead;
    static bool f7WasDown = false;
    // (Shift+F7 belongs to vr_weapons.)
    const bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0 && (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0;
    if (followConfigured && f7Down && !f7WasDown) {
        g_followHead = !g_followHead;
        TraceRuntime(std::string("F7: engine camera head-follow ") + (g_followHead ? "resumed" : "paused"));
    }
    f7WasDown = f7Down;
    if ((!g_handAim && !g_followHead) || !g_directHeadRotation) return;
    float engine[9]{};
    if (!GetEngineCameraBasis(engine)) return;
    const auto vehicle=GetVehicleCameraAnchor(true);
    const bool seated=vehicle.valid;
    if(seated) std::memcpy(engine,vehicle.basis,sizeof(vehicle.basis));
    static bool wasSeated=false;
    if (seated!=wasSeated) { g_bodyFrameValid=false; g_steeringTargetReset=true; }
    wasSeated=seated;
    // Menus render a backdrop camera unrelated to gameplay: re-initialize the
    // body yaw whenever world rendering (re)starts, as well as on F2.
    static bool wasInWorld = false;
    const bool inWorld = IsNativeSceneRendering();
    if (inWorld && !wasInWorld) g_bodyFrameValid = false;
    wasInWorld = inWorld;
    // A camera cut (load, respawn, cutscene end) moves the engine camera far
    // more in one frame than steering can (<= ~11 deg): re-initialize then.
    static float lastForward[3]{};
    static bool haveLastForward = false;
    const float currentForward[3]{-engine[6], -engine[7], -engine[8]};
    const float frameTurn = haveLastForward ? std::acos(std::clamp(currentForward[0] * lastForward[0] +
        currentForward[1] * lastForward[1] + currentForward[2] * lastForward[2], -1.0f, 1.0f)) : 0.0f;
    // Live: 657 false cuts in one session because JC3's mouse sensitivity
    // let JC3VR's own steering exceed 45 deg/frame; each re-init moved the
    // body yaw and made Rico turn. Only count a cut when JC3VR has not
    // steered for two Presents.
    ++g_presentsSinceSteering;
    if (frameTurn > 0.785f && g_presentsSinceSteering > 2) {
        g_bodyFrameValid = false;
        TraceRuntime("hand aim: engine camera cut detected (" + std::to_string(frameTurn * 57.2958f) + " deg); body yaw re-initialized");
    }
    std::memcpy(lastForward, currentForward, sizeof(lastForward));
    haveLastForward = true;
    if (!g_bodyFrameValid || recenterRequested || seated) {
        const float length = std::sqrt(engine[0] * engine[0] + engine[2] * engine[2]);
        if (length < 0.2f) return;
        g_bodyRight[0] = engine[0] / length;
        g_bodyRight[2] = engine[2] / length;
        g_bodyFrameValid = true;
        g_steeringTargetReset = true;
        if (!seated) TraceRuntime("hand aim: body yaw initialized from the engine camera");
    }
    // Head rotation is composed directly on the seated body frame. Relative
    // mouse input here would rotate the chase camera/vehicle aim a second time.
    if (seated) { g_aimRemainderX=g_aimRemainderY=0.0f; return; }
    // Target in the recentred body frame (x=right, y=up, z=forward).
    float bodyTarget[3]{};
    bool haveTarget = false;
    // Hand aim takes the engine camera while the right trigger is held (or
    // always when head follow is off); otherwise it follows the headset.
    int aimHand = g_handAim ? g_activeAimHand : -1;
    if (g_handAim && !g_followHead && aimHand < 0) aimHand = 1;
    const bool handAiming = aimHand >= 0 && state.handAimActive[aimHand];
    if (g_followHead && !handAiming) {
        OpenXrHeadPose head{};
        {
            std::lock_guard poseLock(g_poseMutex);
            head = g_latestPose;
        }
        if (head.valid) {
            const XrVector3f forward = RotateBy({head.orientationX, head.orientationY, head.orientationZ, head.orientationW}, {0.0f, 0.0f, -1.0f});
            bodyTarget[0] = forward.x * g_recenterRight.x + forward.z * g_recenterRight.z;
            bodyTarget[1] = forward.y;
            bodyTarget[2] = forward.x * g_recenterForward.x + forward.z * g_recenterForward.z;
            haveTarget = true;
        }
    } else if (handAiming) {
        const XrVector3f aim = aimHand==1 && state.supportFrameValid?
            XrVector3f{state.supportForward[0],state.supportForward[1],state.supportForward[2]}:
            RotateBy(state.handAimPoses[aimHand].orientation, {0.0f, 0.0f, -1.0f});
        bodyTarget[0] = aim.x * g_recenterRight.x + aim.z * g_recenterRight.z;
        bodyTarget[1] = aim.y;
        bodyTarget[2] = aim.x * g_recenterForward.x + aim.z * g_recenterForward.z;
        haveTarget = true;
    } else if (g_debugHandAim) {
        bodyTarget[0] = std::sin(g_debugHandAimYaw) * std::cos(g_debugHandAimPitch);
        bodyTarget[1] = std::sin(g_debugHandAimPitch);
        bodyTarget[2] = std::cos(g_debugHandAimYaw) * std::cos(g_debugHandAimPitch);
        haveTarget = true;
    }
    // Automatic visibility-camera A/B: swing the head-follow target by a
    // fixed yaw (no controller needed) so the engine camera points away from
    // the view while candidate copies are tested.
    const float autoYaw = CullAutoTestAimYaw();
    const bool autoAiming = autoYaw != 0.0f && haveTarget && !handAiming;
    if (autoAiming) {
        const float x = bodyTarget[0], z = bodyTarget[2];
        bodyTarget[0] = x * std::cos(autoYaw) + z * std::sin(autoYaw);
        bodyTarget[2] = -x * std::sin(autoYaw) + z * std::cos(autoYaw);
    }
    if (!haveTarget) return;
    const float bodyForward[3]{g_bodyRight[2], 0.0f, -g_bodyRight[0]};
    {
        // Head forward in the world, for the visibility-camera A/B.
        OpenXrHeadPose head{};
        {
            std::lock_guard poseLock(g_poseMutex);
            head = g_latestPose;
        }
        if (head.valid) {
            const XrVector3f forward = RotateBy({head.orientationX, head.orientationY, head.orientationZ, head.orientationW}, {0.0f, 0.0f, -1.0f});
            const float bx = forward.x * g_recenterRight.x + forward.z * g_recenterRight.z;
            const float bz = forward.x * g_recenterForward.x + forward.z * g_recenterForward.z;
            const float world[3]{bx * g_bodyRight[0] + bz * bodyForward[0], forward.y, bx * g_bodyRight[2] + bz * bodyForward[2]};
            SetCullHeadForward(world, handAiming || autoAiming);
        }
    }
    float target[3]{};
    for (int c = 0; c != 3; ++c)
        target[c] = bodyTarget[0] * g_bodyRight[c] + bodyTarget[2] * bodyForward[c] + (c == 1 ? bodyTarget[1] : 0.0f);
    const float engineForward[3]{-engine[6], -engine[7], -engine[8]};
    const float engineRight[3]{engine[0], 0.0f, engine[2]};
    const float yawError = std::atan2(target[0] * engineRight[0] + target[2] * engineRight[2],
        target[0] * engineForward[0] + target[2] * engineForward[2]);
    const float pitchError = std::asin(std::clamp(target[1], -1.0f, 1.0f)) - std::asin(std::clamp(engineForward[1], -1.0f, 1.0f));
    // ~3 deg: close enough to release a held-back shot or grapple.
    if (handAiming) g_aimAligned = std::max(std::abs(yawError), std::abs(pitchError)) < 0.05f;
    static std::uint32_t reports = 0;
    const bool report = reports++ % (g_debugHandAim ? 10u : 120u) == 0;
    // JC3 leaves the system cursor visible in gameplay, so menus are detected
    // by response instead: if injected movement leaves the engine camera
    // unmoved for 10 frames, steering backs off for a second.
    static bool sentLastFrame = false;
    static int unresponsiveFrames = 0;
    static ULONGLONG suppressedUntil = 0;
    if (sentLastFrame) unresponsiveFrames = frameTurn < 0.0009f ? unresponsiveFrames + 1 : 0;
    if (unresponsiveFrames >= 10) {
        suppressedUntil = GetTickCount64() + 1000;
        unresponsiveFrames = 0;
        ++g_steerBackoffs;
        TraceRuntime("hand aim: engine camera not responding to steering (menu/pause?); backing off 1 s: yaw_err_deg=" +
            std::to_string(yawError * 57.2958f) + " pitch_err_deg=" + std::to_string(pitchError * 57.2958f) +
            " engine_pitch_deg=" + std::to_string(std::asin(std::clamp(engineForward[1], -1.0f, 1.0f)) * 57.2958f) +
            " target_pitch_deg=" + std::to_string(std::asin(std::clamp(target[1], -1.0f, 1.0f)) * 57.2958f) +
            " backoffs=" + std::to_string(g_steerBackoffs));
    }
    sentLastFrame = false;
    DWORD foregroundPid = 0;
    if (const HWND window = GetForegroundWindow()) GetWindowThreadProcessId(window, &foregroundPid);
    const bool foreground = foregroundPid == GetCurrentProcessId();
    const bool suppressed = GetTickCount64() < suppressedUntil;
    g_steerYawErrorDeg = yawError * 57.2958f;
    g_steerPitchErrorDeg = pitchError * 57.2958f;
    g_steerSuppressed = suppressed;
    std::copy(engineForward, engineForward + 3, g_steerEngineForward);
    const bool allowed = inWorld && foreground && !suppressed;
    if (report)
        TraceRuntime("hand aim: source=" + std::string(handAiming ? (aimHand == 1 ? "right_hand" : "left_hand") : g_followHead ? "head" : "debug") +
            " yaw_err_deg=" + std::to_string(yawError * 57.2958f) + " pitch_err_deg=" + std::to_string(pitchError * 57.2958f) +
            " steering=" + (allowed ? "1" : "0") + " gates[world=" + (inWorld ? "1" : "0") + " suppressed=" + (suppressed ? "1" : "0") +
            " foreground=" + (foreground ? "1" : "0") + "]");
    // Feed-forward: how far the target itself moved this frame (head/hand
    // motion), so the engine camera keeps up instead of lagging and lurching.
    static float lastTarget[3]{};
    static bool haveLastTarget = false;
    float feedYaw = 0.0f, feedPitch = 0.0f;
    if (g_steeringTargetReset) { haveLastTarget = false; g_steeringTargetReset = false; }
    if (haveLastTarget) {
        const float lastRight[3]{-lastTarget[2], 0.0f, lastTarget[0]};
        feedYaw = std::atan2(target[0] * lastRight[0] + target[2] * lastRight[2], target[0] * lastTarget[0] + target[2] * lastTarget[2]);
        feedPitch = std::asin(std::clamp(target[1], -1.0f, 1.0f)) - std::asin(std::clamp(lastTarget[1], -1.0f, 1.0f));
    }
    std::memcpy(lastTarget, target, sizeof(lastTarget));
    haveLastTarget = true;
    // Measured JC3 mouse response (radians of engine turn per injected count).
    // Live 13:14-13:17: the fixed 800 counts/rad guess made the loop swing
    // +/-20..79 deg yaw with sign flips every ~1.3 s (a limit cycle), and an
    // earlier session saw >45 deg/frame from <=200 counts, i.e. roughly
    // 1/250 rad/count: effective loop gain ~3x too high. The response is
    // therefore estimated online from windowed sums of injected counts vs.
    // observed engine turn (window >> input latency, so latency only adds a
    // small bias); the signed pitch estimate also absorbs an inverted Y.
    static float yawRadPerCount = 1.0f / 250.0f;
    static float pitchRadPerCount = 1.0f / 250.0f;
    static float windowCountsX = 0.0f, windowCountsY = 0.0f, windowYaw = 0.0f, windowPitch = 0.0f;
    static int windowFrames = 0;
    static LONG lastDx = 0, lastDy = 0;
    static float lastEngineForward[3]{};
    static bool haveLastEngineForward = false;
    if (haveLastEngineForward && frameTurn < 0.785f) {
        const float lastRight[3]{-lastEngineForward[2], 0.0f, lastEngineForward[0]};
        windowYaw += std::atan2(engineForward[0] * lastRight[0] + engineForward[2] * lastRight[2],
                                engineForward[0] * lastEngineForward[0] + engineForward[2] * lastEngineForward[2]);
        windowPitch += std::asin(std::clamp(engineForward[1], -1.0f, 1.0f)) - std::asin(std::clamp(lastEngineForward[1], -1.0f, 1.0f));
        windowCountsX += static_cast<float>(lastDx);
        windowCountsY -= static_cast<float>(lastDy);
        if (++windowFrames >= 45) {
            auto update = [](float& estimate, float counts, float turn, bool allowNegative) {
                if (std::abs(counts) < 150.0f || std::abs(turn) < 0.02f) return;
                float measured = turn / counts;
                if (!allowNegative && measured <= 0.0f) return;
                const float sign = measured < 0.0f ? -1.0f : 1.0f;
                measured = sign * std::clamp(std::abs(measured), 1.0f / 5000.0f, 1.0f / 40.0f);
                // A sign change replaces the estimate; otherwise smooth it.
                estimate = (measured < 0.0f) != (estimate < 0.0f) ? measured : 0.7f * estimate + 0.3f * measured;
            };
            update(yawRadPerCount, windowCountsX, windowYaw, false);
            update(pitchRadPerCount, windowCountsY, windowPitch, true);
            static int estimateReports = 0;
            if (estimateReports++ % 20 == 0)
                TraceRuntime("hand aim: measured engine response yaw=" + std::to_string(1.0f / yawRadPerCount) +
                    " counts/rad pitch=" + std::to_string(1.0f / pitchRadPerCount) + " counts/rad (window counts x=" +
                    std::to_string(windowCountsX) + " y=" + std::to_string(windowCountsY) + " turn yaw_deg=" +
                    std::to_string(windowYaw * 57.2958f) + " pitch_deg=" + std::to_string(windowPitch * 57.2958f) + ")");
            windowCountsX = windowCountsY = windowYaw = windowPitch = 0.0f;
            windowFrames = 0;
        }
    }
    std::memcpy(lastEngineForward, engineForward, sizeof(lastEngineForward));
    haveLastEngineForward = true;
    lastDx = lastDy = 0;
    if (!allowed) { g_aimRemainderX = g_aimRemainderY = 0.0f; return; }
    // Hysteresis: once within ~0.4 deg stop nudging (constant micro
    // corrections made Rico twitch); re-engage past ~1.5 deg.
    static bool correcting = true;
    const float errorMagnitude = std::max(std::abs(yawError), std::abs(pitchError));
    if (errorMagnitude > 0.026f) correcting = true;
    else if (errorMagnitude < 0.007f) correcting = false;
    // JC3's response lags injection by a frame or two; proportional gain
    // above ~0.45/frame then oscillates, so the configured gain is capped.
    const float gain = std::min(g_handAimGain, 0.35f);
    const float yawCommand = feedYaw + (correcting ? yawError * gain : 0.0f);
    const float pitchCommand = feedPitch + (correcting ? pitchError * gain : 0.0f);
    // At most ~6 deg of engine turn per frame.
    constexpr float kMaxTurnPerFrame = 0.1f;
    g_aimRemainderX += std::clamp(yawCommand, -kMaxTurnPerFrame, kMaxTurnPerFrame) / yawRadPerCount;
    g_aimRemainderY -= std::clamp(pitchCommand, -kMaxTurnPerFrame, kMaxTurnPerFrame) / pitchRadPerCount;
    const LONG dx = static_cast<LONG>(std::trunc(g_aimRemainderX));
    const LONG dy = static_cast<LONG>(std::trunc(g_aimRemainderY));
    g_aimRemainderX -= static_cast<float>(dx);
    g_aimRemainderY -= static_cast<float>(dy);
    if (!dx && !dy) return;
    lastDx = dx;
    lastDy = dy;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
    sentLastFrame = true;
    g_presentsSinceSteering = 0;
}

// Already inside a begun XR frame. This consumes/releases the backbuffer on
// every path. No wrist/weapon rendering, stereo retention, crop or remapping.
OpenXrBootstrapResult SubmitFrontEndPanel(RuntimeState& state,ID3D11Texture2D* backbuffer,
                                        ID3D11DeviceContext* context,const XrFrameState& frameState) {
    D3D11_TEXTURE2D_DESC source{};backbuffer->GetDesc(&source);
    const auto fail=[&](const std::string& reason) {
        backbuffer->Release();EndEmptyOpenXrFrame(state,frameState);
        ++state.frameCount;return OpenXrBootstrapResult{false,reason};
    };
    if(!source.Width || !source.Height || source.SampleDesc.Count!=1)
        return fail("front-end panel: unsupported source dimensions/sample count");
    if(state.swapchain!=XR_NULL_HANDLE &&
       (state.width!=source.Width || state.height!=source.Height || state.frontEndFormat!=source.Format)) {
        PFN_xrDestroySwapchain destroy{};
        LoadProc(state.getProc,state.instance,"xrDestroySwapchain",destroy);
        if(!destroy || XR_FAILED(ObserveRuntimeResult(destroy(state.swapchain))))
            return fail("front-end panel: resize swapchain destruction failed");
        state.swapchain=XR_NULL_HANDLE;state.images.clear();state.swapchainsReady=false;
    }
    if(state.swapchain==XR_NULL_HANDLE) {
        uint32_t count=0;
        if(XR_FAILED(ObserveRuntimeResult(state.enumerateFormats(state.session,0,&count,nullptr))) || !count)
            return fail("front-end panel: no runtime formats");
        std::vector<int64_t> formats(count);
        if(XR_FAILED(ObserveRuntimeResult(state.enumerateFormats(state.session,count,&count,formats.data()))))
            return fail("front-end panel: format enumeration failed");
        // Backbuffer pixels are display-encoded. Copy their bits into an sRGB
        // layer when supported, as in native stereo, not double gamma encode.
        auto format=FrontEndSrgbFormat(source.Format);
        if(std::find(formats.begin(),formats.end(),static_cast<int64_t>(format))==formats.end()) format=source.Format;
        if(std::find(formats.begin(),formats.end(),static_cast<int64_t>(format))==formats.end())
            return fail("front-end panel: exact backbuffer format unavailable (no unsafe copy)");
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format=format;info.sampleCount=1;info.width=source.Width;info.height=source.Height;
        info.faceCount=info.arraySize=info.mipCount=1;
        if(XR_FAILED(ObserveRuntimeResult(state.createSwapchain(state.session,&info,&state.swapchain))))
            return fail("front-end panel: swapchain creation failed");
        state.width=source.Width;state.height=source.Height;state.frontEndFormat=source.Format;
    }
    if(state.images.empty()) {
        uint32_t count=0;
        if(XR_FAILED(ObserveRuntimeResult(state.enumerateImages(state.swapchain,0,&count,nullptr))) || !count)
            return fail("front-end panel: image count failed");
        std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if(XR_FAILED(ObserveRuntimeResult(state.enumerateImages(state.swapchain,count,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))))
            return fail("front-end panel: image enumeration failed");
        state.images=std::move(images);state.swapchainsReady=true;
        TraceRuntime("front-end panel: full native loading/menu image "+std::to_string(source.Width)+"x"+
            std::to_string(source.Height)+" at 2m, no crop; wrist HUD suppressed");
    }
    if(!frameState.shouldRender) {
        backbuffer->Release();EndEmptyOpenXrFrame(state,frameState);++state.frameCount;
        return {true,"front-end panel: runtime skipped rendering"};
    }
    uint32_t index=0;XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if(XR_FAILED(ObserveRuntimeResult(state.acquireImage(state.swapchain,&acquire,&index))))
        return fail("front-end panel: acquire failed");
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    if(XR_FAILED(ObserveRuntimeResult(state.waitImage(state.swapchain,&wait)))) {
        // Do not acquire again on a chain whose image has not become writable.
        // Tear down through the existing session recovery path next Present.
        g_runtimeReconnectPending=true;
        return fail("front-end panel: wait failed; reconnect may be required");
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if(index>=state.images.size() || !state.images[index].texture) {
        ObserveRuntimeResult(state.releaseImage(state.swapchain,&release));
        return fail("front-end panel: invalid acquired image");
    }
    D3D11_TEXTURE2D_DESC destination{};state.images[index].texture->GetDesc(&destination);
    if(destination.Width!=source.Width || destination.Height!=source.Height ||
       !FrontEndCopyCompatible(source.Format,destination.Format) || destination.SampleDesc.Count!=source.SampleDesc.Count ||
       destination.ArraySize!=source.ArraySize || destination.MipLevels!=source.MipLevels) {
        ObserveRuntimeResult(state.releaseImage(state.swapchain,&release));
        return fail("front-end panel: copy compatibility rejected");
    }
    context->CopyResource(state.images[index].texture,backbuffer);
    if(XR_FAILED(ObserveRuntimeResult(state.releaseImage(state.swapchain,&release)))) {
        g_runtimeReconnectPending=true;
        return fail("front-end panel: release failed");
    }
    backbuffer->Release();
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space=state.viewSpace;layer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    layer.pose.orientation.w=1;layer.pose.position.z=-2;
    layer.size={2.4f,FrontEndPanelHeight(source.Width,source.Height)};
    layer.subImage.swapchain=state.swapchain;
    layer.subImage.imageRect.extent={static_cast<int32_t>(source.Width),static_cast<int32_t>(source.Height)};
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=frameState.predictedDisplayTime;
    end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=layers;
    const auto result=ObserveRuntimeResult(state.endFrame(state.session,&end));++state.frameCount;
    return {XR_SUCCEEDED(result),XR_SUCCEEDED(result)?"native loading/menu panel submitted":"front-end panel: end frame failed"};
}

OpenXrBootstrapResult SubmitNativeAlternateFrame(RuntimeState& state, ID3D11Texture2D* backbuffer, ID3D11DeviceContext* context, const DXGI_SWAP_CHAIN_DESC& desktopDesc, const XrFrameState& frameState, const std::array<XrView, 2>& views) {
    // Keep a desktop-only reference across the existing backbuffer Release.
    // Copy ONLY after both eye images and xrEndFrame have completed successfully.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> creatorDesktop;
    if(g_creatorMirrorEye!=CreatorMirrorEye::Off)creatorDesktop=backbuffer;
    const bool frontEnd=UseFrontEndPanel(HasNativeGameplayCamera(),IsNativeSceneRendering());
    if(frontEnd!=state.frontEndPanelActive) {
        state.frontEndPanelActive=frontEnd;
        state.wristNativeHudFrame=0;state.nativeEyeReady={false,false};state.pairCommitted=false;
        state.finalHandCapturePending={false,false};
        TraceRuntime(frontEnd?"presentation: native loading/menu panel active":"presentation: gameplay stereo resumed; awaiting fresh eye pair");
    }
    // Presentation selection must NOT bypass native camera/paired-upload
    // preparation below: the scene-ready observer depends on those writes.
    // Decide which layer to submit only after the normal preparation completes.
    const auto rig = BuildNativeEyeRig(views, frameState.predictedDisplayTime);
    {
        const auto aircraft=GetVehicleCameraAnchor(true);
        const auto& camera=rig[0].nativeOffset;
        float target[3]{};
        bool valid=aircraft.valid && aircraft.driver && aircraft.aircraft==AircraftKind::Helicopter &&
            HasNativeGameplayCamera() && camera.baseAnchor && camera.rotate && state.focused;
        if(valid) {
            // Use the exact banked frame used for this eye rig. The gaze is
            // a direction request, not a change to physical head translation.
            for(int c=0;c<3;++c) {
                float forward=0;
                for(int axis=0;axis<3;++axis) forward-=camera.headAxes[6+axis]*camera.anchorBasis[axis*3+c];
                target[c]=camera.anchorPosition[c]+forward*(1000.0f*g_jc3WorldUnitsPerMeter);
            }
        }
        PublishHelicopterGaze(valid?aircraft.vehicle:0,target);
    }
    {
        // Both eyes' extents for JC3's own visibility culls (engine_culling).
        const float leftEye[4]{std::tan(views[0].fov.angleLeft), std::tan(views[0].fov.angleRight), std::tan(views[0].fov.angleUp), std::tan(views[0].fov.angleDown)};
        const float rightEye[4]{std::tan(views[1].fov.angleLeft), std::tan(views[1].fov.angleRight), std::tan(views[1].fov.angleUp), std::tan(views[1].fov.angleDown)};
        const float dx = views[1].pose.position.x - views[0].pose.position.x, dy = views[1].pose.position.y - views[0].pose.position.y, dz = views[1].pose.position.z - views[0].pose.position.z;
        SetEngineCullEyes(leftEye, rightEye, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    {
        // VR body: controller grip positions in the world, placed exactly like
        // the eyes (engine camera + body-frame offset from the recentred head),
        // so Rico's hands meet the rendered controllers.
        float engine[3]{};
        bool valid[2]{};
        float world[2][3]{}, fingersWorld[2][3]{}, downWorld[2][3]{};
        float aimDirection[3]{};  // right hand, always from the aim pose (vr_weapons)
        bool aimValid = false;
        float sampledHandCamera[3]{};
        bool sampledHandCameraValid = false;
        if (GetEngineCameraPosition(engine)) {
            const bool armed = g_sixDof && g_headTranslationArmed;
            const XrVector3f reference = armed ? g_recenterHeadCentre : XrVector3f{
                0.5f * (views[0].pose.position.x + views[1].pose.position.x), 0.5f * (views[0].pose.position.y + views[1].pose.position.y),
                0.5f * (views[0].pose.position.z + views[1].pose.position.z)};
            float bodyRight[3]{1.0f, 0.0f, 0.0f};
            float engineRows[9]{};
            if (g_bodyFrameValid) { bodyRight[0] = g_bodyRight[0]; bodyRight[2] = g_bodyRight[2]; }
            else if (GetEngineCameraBasis(engineRows)) {
                const float length = std::sqrt(engineRows[0] * engineRows[0] + engineRows[2] * engineRows[2]);
                if (length > 0.2f) { bodyRight[0] = engineRows[0] / length; bodyRight[2] = engineRows[2] / length; }
            }
            float bodyForward[3]{bodyRight[2], 0.0f, -bodyRight[0]};
            float bodyUp[3]{0,1,0};
            if (rig[0].nativeOffset.baseAnchor) {
                // Use the very same seated frame and position as the eye rig,
                // including vehicle roll/pitch; never mix it with a yaw-only
                // hand frame or a later simulation snapshot.
                const auto& anchor=rig[0].nativeOffset;
                std::memcpy(engine,anchor.anchorPosition,sizeof(anchor.anchorPosition));
                for(int c=0;c<3;++c) {
                    bodyRight[c]=anchor.anchorBasis[c];
                    bodyUp[c]=anchor.anchorBasis[3+c];
                    bodyForward[c]=-anchor.anchorBasis[6+c];
                }
            }
            const float scale = g_jc3WorldUnitsPerMeter * g_sixDofScale;
            std::copy(engine, engine + 3, sampledHandCamera);
            sampledHandCameraValid = true;
            for (int hand = 0; hand != 2; ++hand) {
                if (!state.handPoseActive[hand]) continue;
                const XrVector3f grip = hand==0 && state.supportFrameValid?
                    XrVector3f{state.supportSocket[0],state.supportSocket[1],state.supportSocket[2]}:state.handLocations[hand].pose.position;
                const XrVector3f offset{grip.x - reference.x, grip.y - reference.y, grip.z - reference.z};
                const float r = scale * (offset.x * g_recenterRight.x + offset.z * g_recenterRight.z);
                const float u = scale * offset.y;
                const float f = scale * (offset.x * g_recenterForward.x + offset.z * g_recenterForward.z);
                for (int c = 0; c != 3; ++c) world[hand][c] = engine[c] + bodyRight[c] * r + bodyForward[c] * f + bodyUp[c]*u;
                // Hand axes: -Z = where the fingers/gun point, -Y = index ->
                // pinky. From the aim pose by default: SteamVR's Quest grip
                // frame runs along the handle (live: aim forward in the grip
                // frame = (0, -0.866, -0.5), 60 deg away), which pointed the
                // hand, and a gun carried by it, 60 deg off the bullet line.
                const bool fromAim = VrBodyHandsUseAimFrame() && state.handAimActive[hand];
                const XrQuaternionf q = fromAim ? state.handAimPoses[hand].orientation : state.handLocations[hand].pose.orientation;
                const XrVector3f axes[2]{RotateBy(q, {0.0f, 0.0f, -1.0f}), RotateBy(q, {0.0f, -1.0f, 0.0f})};
                float* out[2]{fingersWorld[hand], downWorld[hand]};
                for (int a = 0; a != 2; ++a) {
                    const float ar = axes[a].x * g_recenterRight.x + axes[a].z * g_recenterRight.z;
                    const float af = axes[a].x * g_recenterForward.x + axes[a].z * g_recenterForward.z;
                    for (int c = 0; c != 3; ++c) out[a][c] = bodyRight[c] * ar + bodyForward[c] * af + bodyUp[c]*axes[a].y;
                }
                valid[hand] = true;
                // The weapon aim ray: from the right grip along the aim pose's
                // -Z, the line the gun crosshair is drawn on, whichever frame
                // the hands use.
                if (hand == 1 && state.handAimActive[1]) {
                    const XrVector3f axis = state.supportFrameValid?
                        XrVector3f{state.supportForward[0],state.supportForward[1],state.supportForward[2]}:
                        RotateBy(state.handAimPoses[1].orientation, {0.0f, 0.0f, -1.0f});
                    const float ar = axis.x * g_recenterRight.x + axis.z * g_recenterRight.z;
                    const float af = axis.x * g_recenterForward.x + axis.z * g_recenterForward.z;
                    for (int c = 0; c != 3; ++c) aimDirection[c] = bodyRight[c] * ar + bodyForward[c] * af + bodyUp[c]*axis.y;
                    aimValid = true;
                }
            }
        }
        if (state.supportFrameValid && valid[0] && valid[1] && aimValid)
            std::memcpy(fingersWorld[1],aimDirection,sizeof(aimDirection));
        const bool unarmed = RicoNativeHandsEnabled() && RicoInventoryIsHolstered();
        for (int hand = 0; hand != 2; ++hand) SetVrBodyHandTarget(hand, valid[hand], world[hand], fingersWorld[hand], downWorld[hand],
            sampledHandCameraValid ? sampledHandCamera : nullptr, rig[0].nativeOffset.baseAnchor,
            {unarmed, state.handCurl[hand].grip, state.handCurl[hand].index});
        SetWeaponAimRay(aimValid, world[1], aimDirection);
    }
    // The pre-HUD copy of the frame being presented (consumed every Present,
    // so a skipped frame never leaves a stale copy), then arm the next one.
    ID3D11Texture2D* const preHudCopy = ConsumePreHudCopy();
    SetPreHudCapture(g_hudAlign && !frontEnd, backbuffer);
    GrappleProbeTick(g_grappleHeld);
    CameraSourceScanTick();
    SkeletonProbeTick();
    if (g_cullingModeTest) {
        static int phase = -1;
        static std::uint64_t phaseFrame = 0, sceneFrames = 0;
        if (IsNativeSceneRendering()) ++sceneFrames; else sceneFrames = 0;
        if (phase < 0 && sceneFrames >= 1350) { phase = 0; phaseFrame = 0; TraceRuntime("culling mode test: starting (modes 0/1/2, 5 s each)"); }
        if (phase >= 0 && phase < 3) {
            SetCullingMode(phase);
            if (++phaseFrame == 420) {
                g_forcedCapture = true;
                g_forcedCaptureTag = "cullmode" + std::to_string(phase);
            }
            if (phaseFrame == 450) {
                ++phase;
                phaseFrame = 0;
                if (phase == 3) { SetCullingMode(g_cullingModeDefault); TraceRuntime("culling mode test: finished"); }
            }
        }
    }
    if (g_lodScaleTest) {
        static int phase = -1;
        static std::uint64_t phaseFrame = 0, sceneFrames = 0;
        static LARGE_INTEGER phaseStart{};
        static const float kScales[4]{1.0f, 1.5f, 2.0f, 1.0f};
        if (IsNativeSceneRendering()) ++sceneFrames; else sceneFrames = 0;
        if (phase < 0 && sceneFrames >= 1350) { phase = 0; phaseFrame = 0; TraceRuntime("LOD scale test: starting (4 phases x 10 s; keep the headset still)"); }
        if (phase >= 0 && phase < 4) {
            SetLodProjectionScale(kScales[phase]);
            if (++phaseFrame == 300) QueryPerformanceCounter(&phaseStart);
            if (phaseFrame == 850) {
                g_forcedCapture = true;
                g_forcedCaptureTag = "phase" + std::to_string(phase) + "_scale" + std::to_string(kScales[phase]);
            }
            if (phaseFrame == 900) {
                LARGE_INTEGER now{}, frequency{};
                QueryPerformanceCounter(&now);
                QueryPerformanceFrequency(&frequency);
                const double ms = 1000.0 * static_cast<double>(now.QuadPart - phaseStart.QuadPart) / static_cast<double>(frequency.QuadPart) / 600.0;
                TraceRuntime("LOD scale test phase " + std::to_string(phase) + " scale=" + std::to_string(kScales[phase]) +
                    " mean_present_ms=" + std::to_string(ms) + " draws_total=" + std::to_string(GetObservedDrawCount()));
                ++phase;
                phaseFrame = 0;
                if (phase == 4) { SetLodProjectionScale(g_lodScale); TraceRuntime("LOD scale test: finished"); }
            }
        }
    }
    CullOverridePresent();
    static bool f11WasDown = false;
    const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (f11Down && !f11WasDown) {
        g_hudAlign = !g_hudAlign;
        TraceRuntime(std::string("F11: HUD re-placement ") + (g_hudAlign ? "on" : "off"));
    }
    f11WasDown = f11Down;
    // F5: culling frustum = both-eye union (on) vs current eye (off), for
    // comparing terrain holes at the view edges. Ignored while the
    // visibility-camera A/B test owns F5.
    static bool f5WasDown = false;
    const bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    // Shift+F5 belongs to the engine-culling widen A/B (engine_culling.cpp).
    if (f5Down && !f5WasDown && !g_cullOverrideTestOwnsF5 && !(GetAsyncKeyState(VK_SHIFT) & 0x8000))
    {
        static const char* kModes[4]{"current eye only", "both-eye union", "both-eye union + widened P2/FOV", "union + FOV + extra camera records"};
        TraceRuntime(std::string("F5: culling mode ") + kModes[CycleCullingMode()]);
    }
    f5WasDown = f5Down;
    // Page Up/Down: HUD distance x/÷1.25; Home/End: HUD size +/-0.05.
    static std::array<bool, 4> hudKeysWasDown{};
    const int hudKeys[4]{VK_PRIOR, VK_NEXT, VK_HOME, VK_END};
    for (int key = 0; key != 4; ++key) {
        const bool down = (GetAsyncKeyState(hudKeys[key]) & 0x8000) != 0;
        if (down && !hudKeysWasDown[key]) {
            // While a full-screen menu is open these size the menu screen.
            float& distance = g_menuOpen ? g_menuDistance : g_hudDistance;
            float& scale = g_menuOpen ? g_menuScale : g_hudScale;
            if (key == 0) distance = std::min(20.0f, distance * 1.25f);
            if (key == 1) distance = std::max(0.5f, distance / 1.25f);
            if (key == 2) scale = std::min(1.0f, scale + 0.05f);
            if (key == 3) scale = std::max(g_menuOpen ? 0.2f : 0.3f, scale - 0.05f);
            TraceRuntime(std::string(g_menuOpen ? "menu screen" : "HUD") + " adjusted: distance_m=" + std::to_string(distance) + " scale=" + std::to_string(scale));
        }
        hudKeysWasDown[key] = down;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    backbuffer->GetDevice(&device);
    std::string error;
    if (!EnsureNativeAlternateResources(state, device.Get(), desktopDesc, error) || !EnsureGeoResampleResources(state, device.Get(), desktopDesc, error)) {
        TraceRuntime("native alternate resource setup failed: " + error);
        backbuffer->Release();
        EndEmptyOpenXrFrame(state, frameState);
        return {false, error};
    }
    const uint32_t completedEye = GetActiveNativeEye() % 2;
    const bool drewSinceArm = GetObservedDrawCount() > state.nativeEyeArmDrawCounts[completedEye];
    if (state.nativeEyeProjections[completedEye].valid && (drewSinceArm || !state.nativeEyeReady[completedEye])) {
        const auto renderedProjection = GetLatestCameraProjection();
        const auto matchesEye = [&](uint32_t eye) {
            const auto& requested = state.nativeEyeProjections[eye];
            return requested.valid && renderedProjection.valid &&
                std::abs(renderedProjection.m00 - requested.m00) < 0.01f &&
                std::abs(renderedProjection.m11 - requested.m11) < 0.01f &&
                std::abs(renderedProjection.m20 - requested.m20) < 0.01f &&
                std::abs(renderedProjection.m21 - requested.m21) < 0.01f;
        };
        // Identify the finished image by the projection it was rendered with,
        // not by the alternation label: at headset frame rates JC3's render
        // thread can lag one frame (live: a frame labelled left carried the
        // right eye's m20), and filing it under the label swaps the eyes.
        // Each eye's request (projection + camera pose) is kept until that
        // eye is requested again, so the other eye's record is still valid.
        uint32_t retainEye = completedEye;
        bool projectionMatches = matchesEye(completedEye);
        if (!projectionMatches && matchesEye(1u - completedEye)) {
            retainEye = 1u - completedEye;
            projectionMatches = true;
            static std::uint32_t slipReports = 0;
            ++slipReports;
            if (slipReports <= 4 || slipReports % 300 == 0)
                TraceRuntime("native eye phase slip: frame labelled eye=" + std::to_string(completedEye) +
                    " was rendered for eye=" + std::to_string(retainEye) + " count=" + std::to_string(slipReports));
        }
        // Menus, loading screens and cutscenes use JC3's own camera, so no
        // eye frustum arrives. After 45 frames without a match, retain the
        // real render and let the resampler decode it with the projection it
        // was actually rendered with (correct, if not stereo). Never used for
        // an image that matches either eye (that case is handled above).
        const bool staleEye = !projectionMatches && renderedProjection.valid &&
            (!state.nativeEyeReady[completedEye] || state.frameCount - state.nativeEyeRetainedAtFrame[completedEye] > 45);
        if (staleEye) {
            static std::uint32_t fallbackReports = 0;
            if (fallbackReports++ < 4 || fallbackReports % 600 == 0)
                TraceRuntime("native eye retained with its rendered (non-eye) projection after 45 stale frames: eye=" +
                    std::to_string(completedEye) + " rendered_m00=" + std::to_string(renderedProjection.m00) +
                    " rendered_m20=" + std::to_string(renderedProjection.m20));
        }
        if (!projectionMatches && !staleEye) {
            static std::uint32_t mismatchReports = 0;
            ++mismatchReports;
            if (mismatchReports <= 4 || mismatchReports % 300 == 0) TraceRuntime("native eye source rejected (count=" + std::to_string(mismatchReports) + "): completed_eye=" + std::to_string(completedEye) +
                " expected_m20=" + std::to_string(state.nativeEyeProjections[completedEye].m20) +
                " observed_m20=" + std::to_string(renderedProjection.m20));
            // A paused/menu frame can Present the previous eye's backbuffer
            // without issuing a fresh scene projection upload. Keep the last
            // correctly owned eye image and camera instead of replacing it.
        }
        // Double-render test: the first render of each eye pair is not shown.
        const bool skipFirstOfPair = g_doubleRender && g_pairStage == 0 && projectionMatches && state.nativeEyeReady[retainEye];
        if ((projectionMatches || staleEye) && !skipFirstOfPair) {
        state.nativeEyeSourceProjections[retainEye] = renderedProjection;
        const bool traceThisFrame = state.frameCount < 3 || state.frameCount % 600 == 0;
        if (traceThisFrame) TraceRenderState(context, "final-jc3-backbuffer-before-retain", backbuffer);
        context->CopyResource(state.nativeEyeSources[retainEye].Get(), backbuffer);
        state.scopeScenes[retainEye].frame=0;
        if(!frontEnd && GetVrSniperWeapon() && projectionMatches && IsNativeSceneRendering())
            RetainScopeScene(device.Get(),context,preHudCopy,state.scopeScenes[retainEye],state.frameCount,GetVrSniperWeapon());
        if (!frontEnd && preHudCopy && g_hudAlign && projectionMatches && IsNativeSceneRendering()) {
            const float ipd = std::sqrt((views[1].pose.position.x - views[0].pose.position.x) * (views[1].pose.position.x - views[0].pose.position.x) +
                (views[1].pose.position.y - views[0].pose.position.y) * (views[1].pose.position.y - views[0].pose.position.y) +
                (views[1].pose.position.z - views[0].pose.position.z) * (views[1].pose.position.z - views[0].pose.position.z));
            // While a hand steers the aim, the crosshair follows that hand.
            XrVector3f aim{};
            const bool handAiming = g_handAim && g_activeAimHand >= 0 && state.handAimActive[g_activeAimHand];
            if (handAiming) aim = RotateBy(state.handAimPoses[g_activeAimHand].orientation, {0.0f, 0.0f, -1.0f});
            // Otherwise the crosshair still sits on the gun hand's ray (user
            // 2026-09-30: "the crosshair only lines up after I start
            // shooting"). The first shot is already held back until the
            // engine camera has swung onto that ray, so it lands there.
            const bool gunRay = !handAiming && g_handAim && state.handAimActive[1];
            if (gunRay) aim = RotateBy(state.handAimPoses[1].orientation, {0.0f, 0.0f, -1.0f});
            if (state.supportFrameValid && (gunRay || (handAiming && g_activeAimHand==1)))
                aim={state.supportForward[0],state.supportForward[1],state.supportForward[2]};
            // A second copy on the left (grapple) hand's ray, always, unless
            // the left hand is the one steering (then it is the primary).
            XrVector3f grappleAim{};
            const bool grappleRay = g_handAim && state.handAimActive[0] && !(handAiming && g_activeAimHand == 0);
            if (grappleRay) grappleAim = RotateBy(state.handAimPoses[0].orientation, {0.0f, 0.0f, -1.0f});
            ComposeHudForEye(state, device.Get(), context, backbuffer, preHudCopy, state.nativeEyeSources[retainEye].Get(), retainEye,
                state.nativeEyeProjections[retainEye], ipd > 0.04f && ipd < 0.09f ? ipd : 0.064f,
                state.nativeEyeCameras[retainEye].pose.orientation, handAiming || gunRay ? &aim : nullptr, grappleRay ? &grappleAim : nullptr);
        }
        state.nativeEyeReady[retainEye] = true;
        state.nativeEyeRetainedAtFrame[retainEye] = state.frameCount;
        ++state.eyeLatency[retainEye][std::min<uint64_t>(3, state.frameCount - state.eyeRequestFrame[retainEye])];
        {
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            if (state.lastPresentCounter.QuadPart) {
                const double ms = 1000.0 * static_cast<double>(now.QuadPart - state.lastPresentCounter.QuadPart) / static_cast<double>(frequency.QuadPart);
                state.eyeIntervalSum[retainEye] += ms;
                state.eyeIntervalMax[retainEye] = std::max(state.eyeIntervalMax[retainEye], ms);
                ++state.eyeIntervalCount[retainEye];
            }
            state.lastPresentCounter = now;
        }
        state.retainedEyeCameras[retainEye] = state.nativeEyeCameras[retainEye];
        state.retainedEyeProjections[retainEye] = state.nativeEyeProjections[retainEye];
        if (traceThisFrame) {
            TraceRuntime("render-path retained native eye=" + std::to_string(retainEye) +
                         " source=" + DescribeTexture(backbuffer) +
                         " retained=" + DescribeTexture(state.nativeEyeSources[retainEye].Get()) +
                         " full-copy src_rect=" + DescribeRect({0, 0, static_cast<LONG>(desktopDesc.BufferDesc.Width), static_cast<LONG>(desktopDesc.BufferDesc.Height)}) +
                         " dst_rect=" + DescribeRect({0, 0, static_cast<LONG>(state.nativeSourceWidth), static_cast<LONG>(state.nativeSourceHeight)}) );
        }
        if (state.frameCount < 4) TraceRuntime("native alternate retained completed JC3 eye=" + std::to_string(retainEye));
        }
    }
    // Re-request the same eye (with a fresh pose) until JC3 has rendered it.
    // F8 (diagnostic) re-requests once to swap which eye renders on even and
    // odd frames: a parity-bound artefact then moves to the other eye.
    static bool f8WasDown = false;
    const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0 && !(GetAsyncKeyState(VK_SHIFT) & 0x8000);  // Shift+F8: vr_weapons
    const bool phaseShift = f8Down && !f8WasDown;
    f8WasDown = f8Down;
    static bool f12WasDown = false;
    const bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12Down && !f12WasDown) {
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            g_doubleRender = !g_doubleRender;
            g_pairStage = 0;
            TraceRuntime(std::string("Shift+F12: double-render test (each eye rendered twice, second shown) ") + (g_doubleRender ? "ON" : "OFF"));
        } else {
            g_symmetricEyes = !g_symmetricEyes;
            TraceRuntime(std::string("F12: symmetric union eye frustum ") + (g_symmetricEyes ? "ON" : "OFF"));
        }
    }
    f12WasDown = f12Down;
    bool advanceEye = ShouldAdvanceNativeEye(drewSinceArm) && !phaseShift;
    if (g_doubleRender && advanceEye) {
        // First render of this eye done: render it once more before moving on.
        if (g_pairStage == 0) { advanceEye = false; g_pairStage = 1; }
        else g_pairStage = 0;
    }
    if (phaseShift) TraceRuntime("F8: eye phase shifted by one frame (left/right now render on swapped frame parity)");
    const uint32_t nextEye = advanceEye ? 1u - completedEye : completedEye;
    if (!advanceEye) {
        static std::uint32_t holdReports = 0;
        if (holdReports++ < 4 || holdReports % 600 == 0)
            TraceRuntime("native eye request held (no scene rendered since last Present): eye=" + std::to_string(nextEye) +
                " count=" + std::to_string(holdReports));
    }
    state.nativeEyeCameras[nextEye] = rig[nextEye];
    state.eyeRequestFrame[nextEye] = state.frameCount;
    XrFovf renderFov = rig[nextEye].fov;
    if (g_symmetricEyes) {
        // Union of both eyes' angular extents. A +-0.03 tangent tag (window
        // shifted, widened to keep full coverage) keeps the two projections
        // distinguishable for eye identification (m20 about -+0.021).
        const float tag = nextEye == 0 ? -0.03f : 0.03f;
        const float left = std::min(std::tan(rig[0].fov.angleLeft), std::tan(rig[1].fov.angleLeft)) - 0.03f + tag;
        const float right = std::max(std::tan(rig[0].fov.angleRight), std::tan(rig[1].fov.angleRight)) + 0.03f + tag;
        const float up = std::max(std::tan(rig[0].fov.angleUp), std::tan(rig[1].fov.angleUp));
        const float down = std::min(std::tan(rig[0].fov.angleDown), std::tan(rig[1].fov.angleDown));
        renderFov = {std::atan(left), std::atan(right), std::atan(up), std::atan(down)};
    }
    state.nativeEyeProjections[nextEye] = ProjectionForView(nextEye, renderFov, rig[nextEye].cameraLocalX);
    state.nativeEyeArmDrawCounts[nextEye] = GetObservedDrawCount();
    SetNativeEyeProjection(state.nativeEyeProjections[nextEye]);
    if (g_forceNativeProjectionBuffer && !ForceNativeEyeProjection(context, state.nativeEyeProjections[nextEye])) {
        static std::uint32_t forceFailures = 0;
        if (forceFailures++ < 4) TraceRuntime("native eye projection buffer refresh unavailable; retaining last verified eye images if JC3 skips its own upload");
    }
    // F6 diagnostic: neutralize JC3VR's camera position/rotation writes (a
    // zero offset restores the engine values) to test whether they cause the
    // first-person regression. Projection sync is unaffected.
    // Shift+F6 belongs to the off-axis reconstruction A/B (engine_lighting.cpp).
    const bool pauseKeyDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0 && !(GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (pauseKeyDown && !g_pauseKeyWasDown) {
        g_cameraWritesPaused = !g_cameraWritesPaused;
        TraceRuntime(std::string("F6: JC3VR native camera position/rotation writes ") + (g_cameraWritesPaused ? "PAUSED (engine camera only)" : "resumed"));
    }
    g_pauseKeyWasDown = pauseKeyDown;
    CameraTimelineAtPresent("present-before-writes");
    NativeCameraOffset cameraOffset = rig[nextEye].nativeOffset;
    if (g_cameraWritesPaused) cameraOffset = {true, nextEye};
    static bool nativeCameraOffsetFailureTraced = false;
    if (!SetNativeCameraOffset(cameraOffset) && !nativeCameraOffsetFailureTraced) {
        nativeCameraOffsetFailureTraced = true;
        TraceRuntime("native camera world offset unavailable: validated owner+0x5c0/0x5c8 transform chain not ready");
    }
    SyncNativeCameraProjectionToEye(state.nativeEyeProjections[nextEye]);
    SyncPairedCameraProjectionToEye(state.nativeEyeProjections[nextEye]);
    ArmPairedCameraUploadSync(cameraOffset, state.nativeEyeProjections[nextEye]);
    static std::uint64_t lastRecenterCount = 0;
    UpdateHandAim(state, g_recenterCount != lastRecenterCount);
    lastRecenterCount = g_recenterCount;
    CameraTimelineAtPresent("present-after-writes");
    if(frontEnd) {
        // The complete normal rig/synchronization path has run, so gameplay
        // uploads can make next Present's scene observer ready. Quad rendering
        // itself remains unchanged and never draws wrist/weapon overlays.
        if(state.frameCount%600==0)
            TraceRuntime("presentation loading gate after native preparation: camera="+
                std::to_string(HasNativeGameplayCamera())+" scene="+std::to_string(IsNativeSceneRendering()));
        PublishHelicopterGaze(0,nullptr);
        return SubmitFrontEndPanel(state,backbuffer,context,frameState);
    }
    // 151 is odd, so successive samples alternate between the two eyes.
    if (state.frameCount % 151 == 0) ArmStereoCoherenceDrawSample();
    backbuffer->Release();

    if (!state.nativeEyeReady[0] || !state.nativeEyeReady[1]) {
        static std::uint64_t warmupReports = 0;
        if (warmupReports++ < 4 || warmupReports % 600 == 0)
            TraceRuntime("native alternate warm-up: left_ready=" + std::to_string(state.nativeEyeReady[0]) + " right_ready=" + std::to_string(state.nativeEyeReady[1]) + " next_eye=" + std::to_string(nextEye));
        EndEmptyOpenXrFrame(state, frameState);
        return {true, "native alternate eye warm-up: retaining the first left/right JC3 renders"};
    }
    CaptureRetainedPair(state, device.Get(), context);
    // Automation hook: %LOCALAPPDATA%\JC3VR\jc3vr_press_a presses pad A once.
    if (DeveloperTestInputEnabled() && state.frameCount % 15 == 0) {
        char directory[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", directory, MAX_PATH);
        if (length && length < MAX_PATH) {
            const std::string trigger = std::string(directory) + "\\JC3VR\\jc3vr_press_a";
            if (GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileA(trigger.c_str()))
                InjectVrGamepadButtons(0x1000, 250);
        }
    }
    // F10 toggles frame-pose re-projection live (A/B for the right-eye ghost).
    static bool f10WasDown = false;
    const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10Down && !f10WasDown) {
        g_reprojectToFramePose = !g_reprojectToFramePose;
        TraceRuntime(std::string("F10: eye re-projection to the frame pose ") + (g_reprojectToFramePose ? "on" : "off"));
    }
    f10WasDown = f10Down;
    // F9 toggles paired submission live (A/B for the right-eye ghost).
    static bool f9WasDown = false;
    const bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 && !(GetAsyncKeyState(VK_SHIFT) & 0x8000);  // Shift+F9: vr_weapons
    if (f9Down && !f9WasDown) {
        g_pairedSubmission = !g_pairedSubmission;
        state.pairCommitted = false;
        TraceRuntime(std::string("F9: paired eye submission ") + (g_pairedSubmission ? "on" : "off"));
    }
    f9WasDown = f9Down;
    if (g_pairedSubmission) {
        const bool bothFresh = state.nativeEyeRetainedAtFrame[0] >= state.pairCommitFrame &&
                               state.nativeEyeRetainedAtFrame[1] >= state.pairCommitFrame &&
                               state.nativeEyeRetainedAtFrame[0] != state.nativeEyeRetainedAtFrame[1];
        if (!state.pairCommitted || bothFresh) {
            for (uint32_t eye = 0; eye != 2; ++eye) {
                if (!state.committedSources[eye]) {
                    D3D11_TEXTURE2D_DESC desc{};
                    state.nativeEyeSources[eye]->GetDesc(&desc);
                    if (FAILED(device->CreateTexture2D(&desc, nullptr, &state.committedSources[eye]))) {
                        g_pairedSubmission = false;
                        TraceRuntime("paired eye submission unavailable: committed texture creation failed");
                        break;
                    }
                }
                context->CopyResource(state.committedSources[eye].Get(), state.nativeEyeSources[eye].Get());
                state.committedScopeScenes[eye].frame=0;
                if(state.scopeScenes[eye].frame==state.nativeEyeRetainedAtFrame[eye] && state.scopeScenes[eye].frame)
                    RetainScopeScene(device.Get(),context,state.scopeScenes[eye].texture.Get(),state.committedScopeScenes[eye],state.scopeScenes[eye].frame,state.scopeScenes[eye].weapon);
                state.committedEyeRetainedFrames[eye]=state.nativeEyeRetainedAtFrame[eye];
                state.committedCameras[eye] = state.retainedEyeCameras[eye];
                state.committedProjections[eye] = state.retainedEyeProjections[eye];
                state.committedSourceProjections[eye] = state.nativeEyeSourceProjections[eye];
            }
            state.pairCommitted = true;
            state.pairCommitFrame = state.frameCount + 1;
            if (++state.pairCommits == 1 || state.pairCommits % 600 == 0)
                TraceRuntime("paired eye submission: committed pair " + std::to_string(state.pairCommits) + " at frame " + std::to_string(state.frameCount));
        }
    }
    const bool usePair = g_pairedSubmission && state.pairCommitted;
    if (state.frameCount % 600 == 0) {
        const auto histogram = [&](int eye) {
            const auto& h = state.eyeLatency[eye];
            return std::to_string(h[0]) + "/" + std::to_string(h[1]) + "/" + std::to_string(h[2]) + "/" + std::to_string(h[3]);
        };
        TraceRuntime("native eye request->retain latency (frames 0/1/2/3+): left=" + histogram(0) + " right=" + histogram(1));
        state.eyeLatency = {};
        const auto pacing = [&](int eye) {
            const double mean = state.eyeIntervalCount[eye] ? state.eyeIntervalSum[eye] / static_cast<double>(state.eyeIntervalCount[eye]) : 0.0;
            char text[64];
            std::snprintf(text, sizeof(text), "mean=%.2fms max=%.2fms n=%llu", mean, state.eyeIntervalMax[eye],
                static_cast<unsigned long long>(state.eyeIntervalCount[eye]));
            return std::string(text);
        };
        TraceRuntime("native eye frame pacing (Present interval before each retained image): left " + pacing(0) + " | right " + pacing(1) +
            " | headset display period=" + std::to_string(static_cast<double>(frameState.predictedDisplayPeriod) / 1.0e6) + "ms");
        state.eyeIntervalSum = {};
        state.eyeIntervalMax = {};
        state.eyeIntervalCount = {};
        TraceRuntime("native eye retention age: left=" + std::to_string(state.frameCount - state.nativeEyeRetainedAtFrame[0]) +
            " right=" + std::to_string(state.frameCount - state.nativeEyeRetainedAtFrame[1]) +
            " projection_uploads=" + std::to_string(GetNativeProjectionWriteCount()));
    }
    uint32_t imageIndices[2]{};
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    for (uint32_t eye = 0; eye != 2; ++eye) {
        const auto& camera = usePair ? state.committedCameras[eye] : state.retainedEyeCameras[eye];
        const XrResult acquireResult = ObserveRuntimeResult(state.acquireImage(state.nativeSwapchains[eye], &acquireInfo, &imageIndices[eye]));
        if (XR_FAILED(acquireResult)) {
            const auto detail = "native alternate eye acquire failed eye=" + std::to_string(eye) + " result=" + std::to_string(acquireResult);
            TraceRuntime(detail);
            EndEmptyOpenXrFrame(state, frameState);
            return {false, detail};
        }
        const XrResult waitResult = ObserveRuntimeResult(state.waitImage(state.nativeSwapchains[eye], &waitInfo));
        if (XR_FAILED(waitResult)) {
            TraceRuntime("native alternate eye wait failed eye=" + std::to_string(eye) + " result=" + std::to_string(waitResult));
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            ObserveRuntimeResult(state.releaseImage(state.nativeSwapchains[eye], &releaseInfo));
            EndEmptyOpenXrFrame(state, frameState);
            return {false, "native alternate eye wait failed: " + std::to_string(waitResult)};
        }
        ID3D11Texture2D* eyeSource = usePair ? state.committedSources[eye].Get() : state.nativeEyeSources[eye].Get();
        const auto& eyeSourceProjection = usePair ? state.committedSourceProjections[eye] : state.nativeEyeSourceProjections[eye];
        const auto& eyeProjection = usePair ? state.committedProjections[eye] : state.retainedEyeProjections[eye];
        const XrPosef& submitPose = g_reprojectToFramePose ? views[eye].pose : camera.pose;
        const XrFovf& submitFov = g_reprojectToFramePose ? views[eye].fov : camera.fov;
        const auto& scopeScene=usePair?state.committedScopeScenes[eye]:state.scopeScenes[eye];
        const auto scopeFrame=usePair?state.committedEyeRetainedFrames[eye]:state.nativeEyeRetainedAtFrame[eye];
        auto* scopeView=scopeScene.frame && scopeScene.frame==scopeFrame && scopeScene.weapon==GetVrSniperWeapon()?scopeScene.view.Get():nullptr;
        if (!RenderNativeEyeToOpenXr(state, device.Get(), context, eyeSource, state.nativeImages[eye][imageIndices[eye]].texture, eyeSourceProjection, eyeProjection, submitPose, submitFov, error,
                                     g_reprojectToFramePose ? &camera.pose.orientation : nullptr,scopeView)) {
            TraceRuntime("native alternate eye render failed eye=" + std::to_string(eye) + ": " + error);
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            ObserveRuntimeResult(state.releaseImage(state.nativeSwapchains[eye], &releaseInfo));
            EndEmptyOpenXrFrame(state, frameState);
            return {false, error};
        }
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ObserveRuntimeResult(state.releaseImage(state.nativeSwapchains[eye], &releaseInfo));
    }
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    for (uint32_t eye = 0; eye != 2; ++eye) {
        const auto& camera = usePair ? state.committedCameras[eye] : state.retainedEyeCameras[eye];
        // Both views carry this frame's located poses: the two eye images
        // were rendered 1 frame apart, and a runtime that corrects the whole
        // layer from one view's pose (the suspected SteamVR behaviour behind
        // the right-eye ghost during head rotation) then sees consistent data.
        projectionViews[eye].pose = g_reprojectToFramePose ? views[eye].pose : camera.pose;
        projectionViews[eye].fov = g_reprojectToFramePose ? views[eye].fov : camera.fov;
        projectionViews[eye].subImage.swapchain = state.nativeSwapchains[eye];
        projectionViews[eye].subImage.imageRect.extent = {static_cast<int32_t>(state.recommendedEyeWidth), static_cast<int32_t>(state.recommendedEyeHeight)};
        if (state.frameCount < 3 || state.frameCount % 600 == 0) {
            TraceRuntime("render-path OpenXR eye=" + std::to_string(eye) +
                         " image_rect=[0,0.." + std::to_string(state.recommendedEyeWidth) + "," + std::to_string(state.recommendedEyeHeight) + ")" +
                         " swapchain_image=" + DescribeTexture(state.nativeImages[eye][imageIndices[eye]].texture));
        }
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = state.localSpace;
    layer.viewCount = 2;
    layer.views = projectionViews.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = frameState.shouldRender ? 1u : 0u;
    endInfo.layers = frameState.shouldRender ? layers : nullptr;
    const XrResult endResult = ObserveRuntimeResult(state.endFrame(state.session, &endInfo));
    if (XR_FAILED(endResult)) {
        TraceRuntime("native alternate xrEndFrame failed result=" + std::to_string(endResult));
        return {false, "native alternate xrEndFrame failed: " + std::to_string(endResult)};
    }
    if(creatorDesktop) {
        const auto eye=static_cast<unsigned>(g_creatorMirrorEye);
        if(CreatorMirrorReady(g_creatorMirrorEye,state.nativeEyeReady[eye],frontEnd,
                             state.frameCount,state.nativeEyeRetainedAtFrame[eye]) &&
           CopyCreatorMirror(context,creatorDesktop.Get(),state.nativeEyeSources[eye].Get())) {
            static bool reported=false;
            if(!reported){TraceRuntime("creator mirror: fixed eye full-frame desktop copy active; no extra scene render");reported=true;}
        }
    }
    ++state.frameCount;
    if (state.frameCount == 1 || state.frameCount % 600 == 0) {
        TraceRuntime("native alternate submitted matched XR eye cameras; frame=" + std::to_string(state.frameCount) + " projection_writes=" + std::to_string(GetNativeProjectionWriteCount()) + " mode=" + ToString(g_nativeStereoRigMode));
        return {true, "native alternate eye images submitted; projection writes=" + std::to_string(GetNativeProjectionWriteCount())};
    }
    return {true, "native alternate eye frame submitted"};
}

float Degrees(float radians) { return radians * 57.295779513f; }

void LogStereoCalibration(RuntimeState& state, const std::array<XrView, 2>& views) {
    if (state.stereoCalibrationLogged) return;
    state.stereoCalibrationLogged = true;
    const float dx = views[1].pose.position.x - views[0].pose.position.x;
    const float dy = views[1].pose.position.y - views[0].pose.position.y;
    const float dz = views[1].pose.position.z - views[0].pose.position.z;
    const float ipdMeters = std::sqrt(dx * dx + dy * dy + dz * dz);
    const auto sourceProjection = GetLatestCameraProjection();
    const auto logView = [](std::ostringstream& stream, const char* label, const XrView& view) {
        const float horizontalTangent = std::tan(view.fov.angleRight) - std::tan(view.fov.angleLeft);
        const float verticalTangent = std::tan(view.fov.angleUp) - std::tan(view.fov.angleDown);
        stream << label << " pose=(" << view.pose.position.x << "," << view.pose.position.y << "," << view.pose.position.z << ")"
               << " fov_deg=(L " << Degrees(view.fov.angleLeft) << ", R " << Degrees(view.fov.angleRight)
               << ", U " << Degrees(view.fov.angleUp) << ", D " << Degrees(view.fov.angleDown) << ")"
               << " frustum_aspect=" << (horizontalTangent / verticalTangent);
    };
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(4)
           << "Stereo calibration: OpenXR eye delta=(" << dx << "," << dy << "," << dz << ") m, runtime IPD=" << ipdMeters << " m; "
           << "Geo SBS source halves=" << state.geoSourceEyeWidth << "x" << state.geoSourceHeight
           << " (source aspect=" << (static_cast<float>(state.geoSourceEyeWidth) / state.geoSourceHeight) << "); "
           << "OpenXR eye targets=" << state.sbsEyeWidth << "x" << state.sbsEyeHeight << "; "
           << (sourceProjection.valid
                ? "JC3 symmetric source FOV_deg=(H " + std::to_string(Degrees(2.0f * std::atan(1.0f / sourceProjection.m00))) + ", V " + std::to_string(Degrees(2.0f * std::atan(1.0f / sourceProjection.m11))) + ") from m00=" + std::to_string(sourceProjection.m00) + ", m11=" + std::to_string(sourceProjection.m11) + "; "
                : "JC3 source projection unavailable at calibration time; ")
           << "mapping=ray-resampled source symmetric projection -> runtime asymmetric OpenXR FOV; "
           << "mapping=OpenXR left<-Geo " << EyeName(SourceEyeForRuntimeEye(0))
           << ", right<-Geo " << EyeName(SourceEyeForRuntimeEye(1)) << ". ";
    logView(stream, "left", views[0]);
    stream << "; ";
    logView(stream, "right", views[1]);
    Log(stream.str());
}
}

OpenXrBootstrapResult TryCreateOpenXrInstance() {
    RuntimeState state{};
    const auto result = CreateRuntime(state);
    if (!result.success) return result;
    PFN_xrDestroyInstance destroyInstance{};
    LoadProc(state.getProc, state.instance, "xrDestroyInstance", destroyInstance);
    if (destroyInstance) destroyInstance(state.instance);
    // The loader is deliberately process-lifetime resident.  See
    // DestroyIncompleteRuntime(): unloading immediately after probing can race
    // runtime-owned work just as the live-session failure path can.
    return result;
}

OpenXrBootstrapResult EnsureOpenXrSession(void* d3d11Device) {
    std::lock_guard attemptLock(g_runtimeMutex);
    static OpenXrBootstrapResult result{false, "OpenXR session has not been attempted"};
    static ULONGLONG nextAttempt = 0;
    if (g_runtimeReconnectPending) {
        DestroyIncompleteRuntime(g_runtime);
        g_runtimeReconnectPending = false;
        { std::lock_guard poseLock(g_poseMutex); g_latestPose.valid = false; }
        const float zero[3]{};
        for (int hand = 0; hand != 2; ++hand) SetVrBodyHandTarget(hand, false, zero, zero, zero);
        PublishVrControllerInputs({});
        PublishHelicopterGaze(0,nullptr);
        SetVrBodyTrackedGauntletReady(false);
        SetWeaponAimRay(false, zero, zero);
        SetNativeEyeProjection({});
        SetNativeCameraOffset({});
        g_recenterValid = false;
        g_bodyFrameValid = false;
        result = {false, "OpenXR disconnected; retrying runtime/session in 5 seconds"};
        nextAttempt = GetTickCount64() + 5000;
        TraceRuntime(result.detail);
    }
    if (g_runtime.session != XR_NULL_HANDLE) return result;
    const ULONGLONG now = GetTickCount64();
    if (now < nextAttempt) return result;

    RuntimeState state{};
    const auto fail = [&](std::string detail) {
        result = {false, std::move(detail)};
        DestroyIncompleteRuntime(state);
        nextAttempt = GetTickCount64() + 5000;
        TraceRuntime("OpenXR live-device retry scheduled in 5 seconds: " + result.detail);
        return result;
    };
        result = CreateRuntime(state);
        if (!result.success) return fail(result.detail);
        PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements{};
        PFN_xrCreateSession createSession{};
        PFN_xrCreateReferenceSpace createSpace{};
        if (!LoadProc(state.getProc, state.instance, "xrGetD3D11GraphicsRequirementsKHR", getRequirements) ||
            !LoadProc(state.getProc, state.instance, "xrCreateSession", createSession) ||
            !LoadProc(state.getProc, state.instance, "xrCreateReferenceSpace", createSpace)) {
            return fail("OpenXR D3D11/session entry points unavailable");
        }
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        const XrResult requirementsResult = getRequirements(state.instance, state.system, &requirements);
        if (XR_FAILED(requirementsResult)) return fail("xrGetD3D11GraphicsRequirementsKHR failed: " + std::to_string(requirementsResult));
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = static_cast<ID3D11Device*>(d3d11Device);
        XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
        createInfo.next = &binding;
        createInfo.systemId = state.system;
        const XrResult sessionResult = createSession(state.instance, &createInfo, &state.session);
        if (XR_FAILED(sessionResult)) return fail("xrCreateSession(game D3D11 device) failed: " + std::to_string(sessionResult));
        XrReferenceSpaceCreateInfo localInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        localInfo.poseInReferenceSpace.orientation.w = 1.0f;
        XrReferenceSpaceCreateInfo viewInfo = localInfo;
        viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        const XrResult localResult = createSpace(state.session, &localInfo, &state.localSpace);
        const XrResult viewResult = createSpace(state.session, &viewInfo, &state.viewSpace);
        if (XR_FAILED(localResult) || XR_FAILED(viewResult)) return fail("OpenXR LOCAL/VIEW reference-space creation failed");
        if (!LoadProc(state.getProc, state.instance, "xrPollEvent", state.pollEvent) ||
            !LoadProc(state.getProc, state.instance, "xrBeginSession", state.beginSession) ||
            !LoadProc(state.getProc, state.instance, "xrEndSession", state.endSession) ||
            !LoadProc(state.getProc, state.instance, "xrEnumerateSwapchainFormats", state.enumerateFormats) ||
            !LoadProc(state.getProc, state.instance, "xrCreateSwapchain", state.createSwapchain) ||
            !LoadProc(state.getProc, state.instance, "xrEnumerateSwapchainImages", state.enumerateImages) ||
            !LoadProc(state.getProc, state.instance, "xrAcquireSwapchainImage", state.acquireImage) ||
            !LoadProc(state.getProc, state.instance, "xrWaitSwapchainImage", state.waitImage) ||
            !LoadProc(state.getProc, state.instance, "xrReleaseSwapchainImage", state.releaseImage) ||
            !LoadProc(state.getProc, state.instance, "xrWaitFrame", state.waitFrame) ||
            !LoadProc(state.getProc, state.instance, "xrBeginFrame", state.beginFrame) ||
            !LoadProc(state.getProc, state.instance, "xrLocateViews", state.locateViews) ||
            !LoadProc(state.getProc, state.instance, "xrEndFrame", state.endFrame)) {
            return fail("OpenXR frame-loop entry points unavailable");
        }
        InitializeHandActions(state);
        g_runtime = std::move(state);
        result = {true, "OpenXR session and LOCAL/VIEW spaces created with live JC3 D3D11 device"};
        nextAttempt = 0;
        TraceRuntime("OpenXR live-device retry path succeeded");
    return result;
}

OpenXrBootstrapResult SubmitMonoOpenXrFrame(void* dxgiSwapchain, void* d3d11Context) {
    std::lock_guard lock(g_runtimeMutex);
    RuntimeState& state = g_runtime;
    if (state.session == XR_NULL_HANDLE) return {false, "OpenXR session is unavailable"};
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult pollResult;
    while ((pollResult = ObserveRuntimeResult(state.pollEvent(state.instance, &event))) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_runtimeReconnectPending = true;
            return {false, "OpenXR instance loss; reconnect scheduled"};
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed->session != state.session) { event = {XR_TYPE_EVENT_DATA_BUFFER}; continue; }
            state.focused=changed->state==XR_SESSION_STATE_FOCUSED;
            if (!state.focused) CancelPauseChord(state.pauseChord);
            if(!state.focused) PublishHelicopterGaze(0,nullptr);
            if (changed->state == XR_SESSION_STATE_LOSS_PENDING || changed->state == XR_SESSION_STATE_EXITING) {
                g_runtimeReconnectPending = true;
                return {false, "OpenXR session ended/lost; reconnect scheduled"};
            }
            if (changed->state == XR_SESSION_STATE_READY && !state.sessionRunning) {
                XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult beginResult = ObserveRuntimeResult(state.beginSession(state.session, &beginInfo));
                if (XR_FAILED(beginResult)) return {false, "xrBeginSession failed: " + std::to_string(beginResult)};
                state.sessionRunning = true;
            } else if (changed->state == XR_SESSION_STATE_STOPPING && state.sessionRunning) {
                ObserveRuntimeResult(state.endSession(state.session));
                state.sessionRunning = false;
                PublishVrControllerInputs({});
                PublishHelicopterGaze(0,nullptr);
                SetVrBodyTrackedGauntletReady(false);
                { std::lock_guard poseLock(g_poseMutex); g_latestPose.valid = false; }
                const float zero[3]{};
                for (int hand = 0; hand != 2; ++hand) SetVrBodyHandTarget(hand, false, zero, zero, zero);
                SetWeaponAimRay(false, zero, zero);
                SetNativeEyeProjection({});
                SetNativeCameraOffset({});
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (g_runtimeReconnectPending) return {false, "OpenXR runtime lost; reconnect scheduled"};
    if (XR_FAILED(pollResult)) return {false, "xrPollEvent failed: " + std::to_string(pollResult)};
    if (!state.sessionRunning) return {true, "OpenXR session waiting for READY state"};

    auto* swapchain = static_cast<IDXGISwapChain*>(dxgiSwapchain);
    auto* context = static_cast<ID3D11DeviceContext*>(d3d11Context);
    DXGI_SWAP_CHAIN_DESC desktopDesc{};
    if (FAILED(swapchain->GetDesc(&desktopDesc))) return {false, "unable to describe JC3 swapchain"};
    if (g_stereoMode != StereoMode::Geo11 && g_stereoMode != StereoMode::NativeAlternate && g_stereoMode != StereoMode::NativeFull && !state.swapchainsReady) {
        uint32_t formatCount = 0;
        if (XR_FAILED(state.enumerateFormats(state.session, 0, &formatCount, nullptr)) || formatCount == 0) return {false, "no OpenXR swapchain formats"};
        std::vector<int64_t> formats(formatCount);
        state.enumerateFormats(state.session, formatCount, &formatCount, formats.data());
        int64_t format = static_cast<int64_t>(desktopDesc.BufferDesc.Format);
        if (std::find(formats.begin(), formats.end(), format) == formats.end()) format = formats.front();
        state.width = desktopDesc.BufferDesc.Width;
        state.height = desktopDesc.BufferDesc.Height;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = state.width;
        info.height = state.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (XR_FAILED(state.createSwapchain(state.session, &info, &state.swapchain))) return {false, "xrCreateSwapchain failed"};
        uint32_t imageCount = 0;
        state.enumerateImages(state.swapchain, 0, &imageCount, nullptr);
        state.images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(state.enumerateImages(state.swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(state.images.data())))) return {false, "xrEnumerateSwapchainImages failed"};
        state.swapchainsReady = true;
    }

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(ObserveRuntimeResult(state.waitFrame(state.session, &waitInfo, &frameState)))) return {false, "xrWaitFrame failed"};
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(ObserveRuntimeResult(state.beginFrame(state.session, &beginInfo)))) return {false, "xrBeginFrame failed"};
    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = state.localSpace;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    uint32_t viewCount = 0;
    if (XR_FAILED(ObserveRuntimeResult(state.locateViews(state.session, &locateInfo, &viewState, 2, &viewCount, views.data()))) || viewCount != 2) return {false, "xrLocateViews failed"};
    const XrVector3f inputHeadCentre{
        (views[0].pose.position.x+views[1].pose.position.x)*0.5f,
        (views[0].pose.position.y+views[1].pose.position.y)*0.5f,
        (views[0].pose.position.z+views[1].pose.position.z)*0.5f};
    UpdateHandPoses(state, frameState.predictedDisplayTime,
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)?&views[0].pose.orientation:nullptr,
        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)?&inputHeadCentre:nullptr);
    UpdateControllerHaptics(state,frameState.shouldRender && !UseFrontEndPanel(HasNativeGameplayCamera(),IsNativeSceneRendering()));
    SetDebugHandPoses(state, views);
    state.wristSnapshot=GetHudSnapshot();
    PublishHudVrFrame(state.focused && frameState.shouldRender && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)!=0);
    state.wristWeaponIcon=GetEquippedWeaponIconId();
    state.wristHeadCentre={.5f*(views[0].pose.position.x+views[1].pose.position.x),
        .5f*(views[0].pose.position.y+views[1].pose.position.y),.5f*(views[0].pose.position.z+views[1].pose.position.z)};
    {
        std::lock_guard poseLock(g_poseMutex);
        g_latestPose.valid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        g_latestPose.orientationX = views[0].pose.orientation.x;
        g_latestPose.orientationY = views[0].pose.orientation.y;
        g_latestPose.orientationZ = views[0].pose.orientation.z;
        g_latestPose.orientationW = views[0].pose.orientation.w;
        ++g_latestPose.sequence;
    }
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)))) return {false, "unable to acquire JC3 backbuffer"};
    if (g_stereoMode == StereoMode::Geo11) {
        std::string error;
        if (!EnsureSbsSwapchains(state, desktopDesc, error)) { backbuffer->Release(); return {false, error}; }
        LogStereoCalibration(state, views);
        return SubmitGeo11SbsFrame(state, backbuffer, context, frameState, views);
    }
    if (g_stereoMode == StereoMode::NativeAlternate) {
        return SubmitNativeAlternateFrame(state, backbuffer, context, desktopDesc, frameState, views);
    }
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo imageWaitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    imageWaitInfo.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(ObserveRuntimeResult(state.acquireImage(state.swapchain, &acquireInfo, &imageIndex))) ||
        XR_FAILED(ObserveRuntimeResult(state.waitImage(state.swapchain, &imageWaitInfo))) ) { backbuffer->Release(); return {false, "OpenXR quad-image acquire/wait failed"}; }
    context->CopyResource(state.images[imageIndex].texture, backbuffer);
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    ObserveRuntimeResult(state.releaseImage(state.swapchain, &releaseInfo));
    backbuffer->Release();
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.space = state.viewSpace;
    layer.pose.orientation.w = 1.0f;
    layer.pose.position.z = -1.5f;
    layer.size = {2.0f, 1.125f};
    layer.subImage.swapchain = state.swapchain;
    layer.subImage.imageRect.extent = {static_cast<int32_t>(state.width), static_cast<int32_t>(state.height)};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = frameState.shouldRender ? 1u : 0u;
    endInfo.layers = frameState.shouldRender ? layers : nullptr;
    if (XR_FAILED(ObserveRuntimeResult(state.endFrame(state.session, &endInfo)))) return {false, "xrEndFrame failed"};
    ++state.frameCount;
    if (state.frameCount == 1 || state.frameCount % 600 == 0) return {true, "submitted mono OpenXR quad at 1.5m; head position x=" + std::to_string(views[0].pose.position.x)};
    return {true, "OpenXR frame submitted"};
}

OpenXrHeadPose GetLatestOpenXrHeadPose() {
    std::lock_guard lock(g_poseMutex);
    return g_latestPose;
}

void ConfigureVrGamepadActions(bool enabled) { g_vrGamepad = enabled; }

void ConfigureReprojectToFramePose(bool enabled) {
    g_reprojectToFramePose = enabled;
    TraceRuntime(std::string("eye re-projection to the frame pose (both views submitted with this frame's poses; F10 toggles): ") + (enabled ? "on" : "off"));
}

void ConfigureMenuScreen(float distanceMeters, float scale) {
    g_menuDistance = distanceMeters;
    g_menuScale = scale;
    TraceRuntime("menu screen (map and other full-screen menus): distance_m=" + std::to_string(distanceMeters) + " scale=" + std::to_string(scale));
}

void ConfigureHudPlacement(bool enabled, float distanceMeters, float scale) {
    g_hudAlign = enabled;
    g_hudDistance = distanceMeters;
    g_hudScale = scale;
    TraceRuntime(std::string("HUD re-placement (per-eye shift to a fixed distance; F11 toggles): ") + (enabled ? "on" : "off") +
        " distance_m=" + std::to_string(distanceMeters) + " scale=" + std::to_string(scale));
}

void ConfigureAimAndControllers(bool aimFollowsRightHand, bool controllerModel, bool aimLines) {
    g_aimFollowsRightHand = aimFollowsRightHand;
    g_aimLines = aimLines;
    g_controllerModel = controllerModel;
    TraceRuntime(std::string("aim follows right hand by default: ") + (aimFollowsRightHand ? "on" : "off") +
        "; hand model: " + (controllerModel ? "Quest controllers" : "procedural hands"));
}
void ConfigureTrackedHands(bool enabled) {
    g_trackedHands = enabled;
    SetVrBodyTrackedGauntletReady(false);
    TraceRuntime(std::string("late tracked hand rig: ") + (enabled ? "on (procedural articulated hands, aim-aligned)" : "off"));
}

void ConfigureCullingMode(int mode, bool cullOverrideTest, bool modeTest) {
    SetCullingMode(mode);
    g_cullingModeDefault = mode;
    g_cullingModeTest = modeTest;
    g_cullOverrideTestOwnsF5 = cullOverrideTest;
    TraceRuntime("culling mode=" + std::to_string(mode) + " (0 eye, 1 both-eye union, 2 union + widened P2; F5 cycles)");
}

void ConfigureLodScale(float scale, bool test) {
    g_lodScale = scale;
    g_lodScaleTest = test;
    SetLodProjectionScale(scale);
    TraceRuntime("LOD projection scale=" + std::to_string(scale) + (test ? " (automatic 1.0/1.5/2.0/1.0 test enabled)" : ""));
}

void ConfigurePairedEyeSubmission(bool enabled) {
    g_pairedSubmission = enabled;
    TraceRuntime(std::string("paired eye submission (both eyes change on the same submitted frame; F9 toggles): ") + (enabled ? "on" : "off"));
}

void ConfigureEngineCameraFollowHead(bool enabled) {
    g_followHead = enabled;
    TraceRuntime(std::string("engine camera follows headset (culling/LOD/streaming): ") + (enabled ? "enabled" : "disabled"));
}

void ConfigureHandAim(bool enabled, float gain, float snapTurnDegrees, bool debugAim, float debugYawDegrees, float debugPitchDegrees) {
    g_handAim = enabled;
    g_handAimGain = std::clamp(gain, 0.05f, 2.0f);
    g_snapTurnRadians = snapTurnDegrees * 3.14159265f / 180.0f;
    g_debugHandAim = debugAim;
    g_debugHandAimYaw = debugYawDegrees * 3.14159265f / 180.0f;
    g_debugHandAimPitch = debugPitchDegrees * 3.14159265f / 180.0f;
    TraceRuntime(std::string("hand aim ") + (enabled ? "enabled" : "disabled") + " gain=" + std::to_string(g_handAimGain) +
        " snap_turn_deg=" + std::to_string(snapTurnDegrees) + (debugAim ? " debug_target=[" + std::to_string(debugYawDegrees) + "," + std::to_string(debugPitchDegrees) + "]" : std::string()));
}

void ConfigureHelicopterHeadSteering(bool enabled) {
    g_helicopterHeadSteering=enabled;
    TraceRuntime(std::string("helicopter: head yaw steering ")+(enabled?"enabled":"disabled"));
}

void ConfigureWristHud(bool enabled) {g_wristHud=enabled;}
void ConfigureCreatorMirror(const std::wstring& eye) {
    g_creatorMirrorEye=ParseCreatorMirrorEye(eye);
    TraceRuntime(g_creatorMirrorEye==CreatorMirrorEye::Off ? "creator mirror: off" :
        g_creatorMirrorEye==CreatorMirrorEye::Left ? "creator mirror: left eye" : "creator mirror: right eye");
}
void ConfigureVerticalTrackingLimit(float metres) {
    g_sixDofVerticalLimitMeters=std::isfinite(metres)&&metres>=0&&metres<=3?metres:1.8f;
    TraceRuntime("6DOF: independent vertical limit_m="+std::to_string(g_sixDofVerticalLimitMeters)+"; horizontal limit unchanged, controller recenter needs deliberate 1s hold");
}
void ConfigureDirectHeadRotation(bool enabled, float debugYawDegrees, float debugYawWobbleDegrees) {
    g_directHeadRotation = enabled;
    g_debugHeadYawRadians = debugYawDegrees * 3.14159265f / 180.0f;
    g_debugHeadYawWobbleRadians = debugYawWobbleDegrees * 3.14159265f / 180.0f;
    TraceRuntime(std::string("head orientation bridge: ") + (enabled ? "direct native camera rotation (mouse bridge off)" : "relative mouse input") +
        " debug_yaw_deg=" + std::to_string(debugYawDegrees));
}

void ConfigureOpenXrStereoMode(StereoMode mode, bool geo11SwapEyes, Geo11StereoSource geo11StereoSource, bool geo11LogExtractionEveryFrame, NativeStereoRigMode nativeStereoRigMode, float jc3WorldUnitsPerMeter, bool sixDof, float sixDofScale, float sixDofLimitMeters, int recenterKey, bool forceNativeProjectionBuffer, bool nativeEyeCapture, bool motionControllers, bool handDebugPreview) {
    g_stereoMode = mode;
    g_geo11SwapEyes = geo11SwapEyes;
    g_geo11StereoSource = geo11StereoSource;
    g_geo11LogExtractionEveryFrame = geo11LogExtractionEveryFrame;
    g_nativeStereoRigMode = nativeStereoRigMode;
    g_jc3WorldUnitsPerMeter = jc3WorldUnitsPerMeter;
    g_sixDof = sixDof;
    g_sixDofScale = std::max(0.0f, sixDofScale);
    g_sixDofLimitMeters = std::max(0.0f, sixDofLimitMeters);
    g_recenterKey = recenterKey;
    g_recenterValid = false;
    g_headTranslationArmed = false;
    g_recenterKeyWasDown = false;
    g_forceNativeProjectionBuffer = forceNativeProjectionBuffer;
    g_nativeEyeCapture = nativeEyeCapture;
    g_motionControllers = motionControllers;
    g_handDebugPreview = handDebugPreview;
    TraceRuntime("native XR rig configured: mode=" + ToString(g_nativeStereoRigMode) + " jc3_world_units_per_meter=" + std::to_string(g_jc3WorldUnitsPerMeter) +
        " six_dof=" + (g_sixDof ? "true" : "false") + " six_dof_scale=" + std::to_string(g_sixDofScale) +
        " limit_m=" + std::to_string(g_sixDofLimitMeters) + " recenter_vk=" + std::to_string(g_recenterKey) +
        " force_native_projection_buffer=" + (g_forceNativeProjectionBuffer ? "true" : "false") +
        "; same-frame stereo pending a native render-loop re-entry hook");
}
}
