#pragma once

// Fixes to JC3's lighting for off-axis VR eyes.
//
// So far: the screen-space reconstruction basis. JC3's deferred lighting,
// screen-space reflections, SSAO, subsurface, atmospheric scattering and depth
// of field all rebuild a clip-to-view matrix with Matrix4::PerspectiveFovInverse
// (vertical FOV + aspect), which can only describe a symmetric frustum. Our
// eye projections are off-axis (m20 = -/+0.24), so every pixel's reconstructed
// position came out slightly wrong, mirror-opposite between the eyes: specular
// and reflections disagree between the eyes, and the sun shadow sampled over
// those positions slides. We hand those passes the exact inverse of the eye
// projection actually being rendered instead.
//
// Mechanism and address from ferrobrew/jc3vr (MIT), reconstruction.rs; build
// verified identical to ours. See docs/ENGINE_REFERENCE.md §2.
namespace jc3vr {
struct EngineLightingOptions {
    bool offAxisReconstruction = true;
    // Sun shadows. JC3 re-renders its far cascades only every 2/4/8 frames
    // and keeps two parity copies; with one eye per frame each eye sampled
    // shadows from a different frame and camera, so shadow regions differed
    // between the eyes (live 2026-10-01).
    bool shadowsEveryFrame = true;  // all cascades refresh every frame
    bool shadowFitUnion = true;     // fit cascades to the both-eye field of view
    bool shadowFitHeading = true;   // fit centre follows head heading, not pitch/roll
};
void ConfigureEngineLighting(const EngineLightingOptions& options);
// Hooks PerspectiveFovInverse and ShadowManager::UpdateRender (MinHook must
// be initialized). Each hook is skipped if its function's bytes are not the
// ones we expect.
bool InstallEngineLightingHooks();
// Present tick: the live toggle (Shift+F6, all lighting fixes) and periodic stats.
void EngineLightingPresentTick();
}
