#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace jc3vr {
struct CameraProjectionSample {
    bool valid = false;
    float m00 = 0.0f;
    float m11 = 0.0f;
    float m20 = 0.0f;
    float m21 = 0.0f;
};

// The native alternate-eye renderer updates this immediately before the next
// JC3 render.  It is intentionally limited to the proven final projection
// buffer; no engine address or speculative camera write is used.
struct NativeEyeProjection {
    bool valid = false;
    std::uint32_t eye = 0;
    float m00 = 0.0f;
    float m11 = 0.0f;
    float m20 = 0.0f;
    float m21 = 0.0f;
    // Camera-local X translation composed into the verified final JC3
    // view/projection upload.  Negative is the left eye; no rotation/toe-in.
    float cameraLocalX = 0.0f;
};

struct NativeCameraOffset {
    bool valid = false;
    std::uint32_t eye = 0;
    float right = 0.0f;
    float up = 0.0f;
    float forward = 0.0f;
    float headYawRadians = 0.0f;
    // Direct head rotation: this eye's axes (right, up, back) expressed in the
    // recentred body frame (x=right, y=up, z=back). When set they replace the
    // engine camera's pitch/roll and are composed with its yaw only.
    bool rotate = false;
    float headAxes[9]{};
    // Hand aim: compose on JC3VR's own body yaw (horizontal right vector in
    // JC3 world) instead of the engine camera's yaw, which then belongs to
    // the aim. The engine camera position is still used.
    bool bodyFrame = false;
    float bodyRight[3]{};
    // Per-XR-frame seated eye anchor, copied to both cameras and paired upload.
    bool baseAnchor=false;
    float anchorPosition[3]{},anchorBasis[9]{};
};

bool InstallCameraCapture(ID3D11DeviceContext* context);
std::string DescribeCameraCapture();
CameraProjectionSample GetLatestCameraProjection();
void ConfigureNativeEyeProjection(bool enabled);
// This only permits a manual arm. It never installs the native renderer probe
// by itself; the player must press F4 after entering a stable world scene.
void ConfigureNativeCameraProbe(bool enabled);
void ConfigureShadowProbe(bool enabled);
void ConfigureNativeViewProjectionSync(bool enabled);
void ConfigureNativeCameraProjectionSync(bool enabled);
// True when the Present eye handoff should switch to the other eye.
bool ShouldAdvanceNativeEye(bool drewSinceArm);
void SetNativeEyeProjection(const NativeEyeProjection& projection);
// Refresh the proven dynamic final-projection buffer even on frames where JC3
// reuses its previous constant-buffer contents instead of uploading them.
bool ForceNativeEyeProjection(ID3D11DeviceContext* context, const NativeEyeProjection& projection);
// Applies a bounded, non-accumulating displacement to the validated native
// camera world-transform records for the next alternate-eye render.
bool SetNativeCameraOffset(const NativeCameraOffset& offset);
bool SyncNativeCameraProjectionToEye(const NativeEyeProjection& projection);
// Read-only diagnostic: the next qualifying scene depth draw copies its bound
// VS slot-0 block to a staging buffer and traces it with the camera records.
void ConfigureStereoCoherenceProbe(bool enabled);
void ArmStereoCoherenceDrawSample();
// Read-only: logs which camera records hold JC3VR writes around Present and
// each Globals-block map for a few in-world frames.
void ConfigureCameraTimelineProbe(bool enabled);
// Keeps each render camera's inverse view, culling projection and frustum
// planes consistent with the head/eye world transform JC3VR writes.
void ConfigureCullingCameraSync(bool enabled, unsigned parts = 7u);
void CameraTimelineAtPresent(const char* phase);
void ConfigurePairedCameraUploadSync(bool enabled);
// Records the eye just handed to JC3 so the paired record can be re-synced
// when the engine maps its Globals block for that render.
// Writes the requested eye's frustum into the paired owner+0x5C0 record.
void SyncPairedCameraProjectionToEye(const NativeEyeProjection& projection);
void ArmPairedCameraUploadSync(const NativeCameraOffset& offset, const NativeEyeProjection& projection);
bool HasNativeGameplayCamera();
// Engine (aim) camera basis rows (right, up, back) as last seen before
// JC3VR's write at the Present handoff; false until one was observed.
bool GetEngineCameraBasis(float rows[9]);
// Engine (aim) camera world position, captured with the basis.
bool GetEngineCameraPosition(float position[3]);
// True while JC3 is rendering the world with JC3VR eye cameras (not menus).
bool IsNativeSceneRendering();
// Scale on JC3's culling/LOD projection P2 (1 = engine's own).
void SetLodProjectionScale(float scale);
// Culling frustum mode: 0 eye, 1 both-eye union, 2 union + widened P2.
int CycleCullingMode();
void SetCullingMode(int mode);
int GetCullingMode();
// Mode 3: camera records found from engine FOV scan hits.
void RegisterCameraFovHits(const std::vector<std::uintptr_t>& hits);
// One-frame, read-only render-target probe (finds where JC3 draws its HUD).
void HudProbePresent(ID3D11Texture2D* backbuffer);
// Pre-HUD scene copy, taken when JC3 executes its HUD command list on the
// backbuffer. Consume returns it once per capture (nullptr otherwise).
void SetPreHudCapture(bool enabled, ID3D11Texture2D* backbuffer);
// Borrowed pointer owned by camera_capture; consuming clears frame-validity,
// NOT COM ownership. Callers must not Release() it.
ID3D11Texture2D* ConsumePreHudCopy();
std::uint32_t GetActiveNativeEye();
std::uint64_t GetNativeProjectionWriteCount();
std::uint64_t GetObservedDrawCount();
// Exclude JC3VR's own image-resample/hand overlay draws from the game-scene
// freshness counter used by native alternate-eye retention.
void SetInternalRenderPass(bool active);
}
