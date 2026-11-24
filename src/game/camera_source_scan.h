#pragma once

namespace jc3vr {
// One-shot, read-only search for every copy of the engine (aim) camera's
// rotation in process memory, attributed to its owning object via RTTI.
// Purpose: find the camera that drives JC3's visibility/LOD/streaming
// (pop-in follows the aim camera even though every render camera is synced
// to the eyes). Off unless camera_source_scan=1. Runs on its own thread once
// the engine rotation has been stable for 90 Presents in gameplay.
void ConfigureCameraSourceScan(bool enabled);
// One-shot read-only search for the engine camera's FOV value (radians, as
// seen in the render camera before JC3VR widens it), attributed via RTTI.
void ConfigureFovSourceScan(bool enabled);
void NoteEngineCameraFov(float radians);
// One-shot read-only probe for Rico's skeleton / pose data (VR body work).
void ConfigureSkeletonProbe(bool enabled);
void SkeletonProbeTick();
void CameraSourceScanTick();

// Visibility-camera A/B test (cull_override_test=1): while a hand steers the
// engine camera, F5 cycles which candidate copy of the camera rotation is
// kept along the head (rewritten at Present and at each Globals upload).
void ConfigureCullOverrideTest(bool enabled, bool automatic);
// Yaw (radians) the automatic A/B wants the engine aim swung by, else 0.
float CullAutoTestAimYaw();
void SetCullHeadForward(const float forward[3], bool handSteering);
void CullOverridePresent();
void CullOverrideAtUpload();
}
