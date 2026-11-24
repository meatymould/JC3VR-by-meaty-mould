#pragma once
#include <cstdint>
#include <string>

namespace jc3vr {
struct OpenXrHeadPose {
    bool valid = false;
    float orientationX = 0.0f;
    float orientationY = 0.0f;
    float orientationZ = 0.0f;
    float orientationW = 1.0f;
    std::uint64_t sequence = 0;
};

// The native OpenXR bootstrap. Session creation is intentionally deferred until
// a validated game-owned ID3D11Device is available from the Present observer.
struct OpenXrBootstrapResult { bool success = false; std::string detail; };
enum class StereoMode;
enum class Geo11StereoSource;
enum class NativeStereoRigMode;
OpenXrBootstrapResult TryCreateOpenXrInstance();

// Called exactly from the first observed real game Present. The device is never
// created or owned by JC3VR: OpenXR uses the game's existing D3D11 device.
OpenXrBootstrapResult EnsureOpenXrSession(void* d3d11Device);
OpenXrBootstrapResult SubmitMonoOpenXrFrame(void* dxgiSwapchain, void* d3d11Context);
OpenXrHeadPose GetLatestOpenXrHeadPose();
bool OpenXrResultRequiresReconnect(std::int32_t result);
void ConfigureVrGamepadActions(bool enabled);
void ConfigureEngineCameraFollowHead(bool enabled);
void ConfigurePairedEyeSubmission(bool enabled);
void ConfigureLodScale(float scale, bool test);
void ConfigureCullingMode(int mode, bool cullOverrideTest, bool modeTest);
void ConfigureAimAndControllers(bool aimFollowsRightHand, bool controllerModel, bool aimLines);
void ConfigureTrackedHands(bool enabled);
void ConfigureHudPlacement(bool enabled, float distanceMeters, float scale);
void ConfigureWristHud(bool enabled);
void ConfigureCreatorMirror(const std::wstring& eye);
// Full-screen menus (map): shown as a screen this far away at this fraction of the view.
void ConfigureMenuScreen(float distanceMeters, float scale);
void ConfigureReprojectToFramePose(bool enabled);
void ConfigureHandAim(bool enabled, float gain, float snapTurnDegrees, bool debugAim, float debugYawDegrees, float debugPitchDegrees);
void ConfigureDirectHeadRotation(bool enabled, float debugYawDegrees = 0.0f, float debugYawWobbleDegrees = 0.0f);
void ConfigureOpenXrStereoMode(StereoMode mode, bool geo11SwapEyes = false, Geo11StereoSource geo11StereoSource = static_cast<Geo11StereoSource>(0), bool geo11LogExtractionEveryFrame = true, NativeStereoRigMode nativeStereoRigMode = static_cast<NativeStereoRigMode>(0), float jc3WorldUnitsPerMeter = 0.0f, bool sixDof = false, float sixDofScale = 1.0f, float sixDofLimitMeters = 0.65f, int recenterKey = 0x71, bool forceNativeProjectionBuffer = false, bool nativeEyeCapture = false, bool motionControllers = false, bool handDebugPreview = false);
void ConfigureHelicopterHeadSteering(bool enabled);
void ConfigureVerticalTrackingLimit(float metres);
}
