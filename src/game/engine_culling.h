#pragma once

// Widens JC3's own visibility culls to the union of both eyes.
//
// JC3 decides visibility once per frame, in the simulation phase, against
// copies of its active camera. JC3VR writes a single eye's camera there, and
// it is consumed a frame or more later, so each eye lost terrain and objects on
// its outer side, and flying made it worse.
//
// Mechanism and addresses: ferrobrew's MIT-licensed jc3vr and pyxis-defs
// (https://github.com/ferrobrew/jc3vr, docs/mod/stereo/culling.md; build
// "JustCause3 Steam 20206564", verified identical to ours on 2026-10-01:
// CameraManager singleton 0x142ED0E20, and GetRenderCamera = mov rax,[rcx+5C8h]).
// See THIRD_PARTY_NOTICES.md and docs/ENGINE_REFERENCE.md §4.
//
//   terrain camera:   TerrainPatchSystemUpdate copies the active camera into
//                     STerrainPatchSystem::m_TerrainCamera (+0x58), and terrain
//                     patches cull against its planes. After the copy, stamp
//                     a symmetric union projection and rebuild the planes with
//                     the engine's Camera::UpdateFrustum.
//   terrain hull:     CRenderBlockTypeTerrainPatch back-patch (+0x44D) and
//                     frustum-patch (+0x44E) culls, baked from the render
//                     camera's axis/frustum, are disabled.
//   scene cull:       OccluderCollectionManager::GetBFBCFrustumParamsForCameraAndTime
//                     for the main cull camera (this+0x8): union projection,
//                     the size-cull FOV reset (JC3VR widens the active camera's
//                     FOV fields), and the software-occlusion frusta dropped.
namespace jc3vr {
struct EngineCullingOptions {
    bool terrainCamera = true;
    bool terrainHullCulls = true;  // true = relax (disable) the hull culls
    bool sceneCull = true;
    bool disableOcclusion = true;
    float padding = 0.4f;          // tangent scale 1 + padding per side
    float sizeCullFovDegrees = 65.0f;  // engine's own vertical FOV; 0 = leave
    // Per-render-block model cull (active camera planes, Camera::UpdateFrustum).
    bool modelCull = true;
    // Cull from the head view JC3VR renders, not the copied engine camera
    // (which lags the head by up to ~20 deg in fast turns and follows the
    // flight path in the wingsuit).
    bool fromHead = true;
};
void ConfigureEngineCulling(const EngineCullingOptions& options);
// Hooks the engine functions (MinHook must be initialized). Safe to call more
// than once; returns false and changes nothing if a byte guard fails.
bool InstallEngineCullingHooks();
// Per XR frame: both eyes' field-of-view tangents (left, right, up, down) and
// the eye separation in metres. Until this is called no widen happens (2D).
void SetEngineCullEyes(const float leftEye[4], const float rightEye[4], float ipdMeters);
// The eye camera world transform JC3VR just wrote (row-major, rows right/up/
// back + translation): the view the culls are built from.
void SetEngineCullViewWorld(const float world[16]);
// For other engine fixes (the shadow fit): the union projection's x/y scale
// (m00, m11) for a given near plane, and the head (eye) world transform.
// False when no fresh XR eyes / head view are available.
bool GetEngineCullUnionScale(float zNear, float& m00, float& m11);
bool GetEngineCullHeadWorld(float world[16]);
// Present tick: hull-cull flags, the live toggle (Shift+F5), and stats.
void EngineCullingPresentTick();
}
