#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include "game/rico_hand_pose.h"

struct ID3D11Resource;

// Rico's arms follow the VR controllers (render-side, v1).
//
// Live map (docs/DEVELOPMENT_NOTES.md, 2026-09-30): Rico's CCharacter O
// (vtable RVA 0x23d16d8 at O, skeleton path at O+0x4E0 ".../jc_skeletons/
// characters/rico"), world transform (row-major, yaw only) at O+0x27E8,
// CAnimatedModel at O+0x1778 -> +0x178 CAnimationControl -> +0xA8
// CSkinningPaletteData {begin, end, ..., +0x48 count=177, +0x50 177 x 3x4}.
// Palette entry i maps rest-pose model points to animated model points
// (column-vector 3x4). The rest pose comes from the 177-bone hkaSkeleton
// "Reference" (LeftHand=25, RightHand=65).
//
// Each frame the arm chains' palette entries are re-solved with two-bone IK
// towards the hand targets (at the Globals map and again before each bone
// upload, because the animation thread rewrites the palette mid-frame).
// Because the game fills bone uploads from its own copy of a fresh pose every
// second frame, the uploads themselves are also rewritten at Unmap. With
// vr_body_hide_all the body is collapsed and the IK only drives carried
// objects (the gun). See docs/ENGINE_REFERENCE.md section 5.
namespace jc3vr {
void ConfigureVrBody(bool enabled, bool debugPose, bool hideHead);
void ConfigureRicoNativeHands(bool enabled, const std::filesystem::path& gameDirectory,
                              const std::array<RicoHandTuning, 2>& tuning);
bool RicoNativeHandsEnabled();
bool RicoNativeHandPrepared(int side);
// At every Present (any mode): discovery scheduling and validation.
void VrBodyPresentTick();
// Controller grip in world space (metres, JC3 world units) from the XR loop:
// position, finger-pointing direction (grip -Z) and handle-down direction
// (grip -Y). valid=false releases that arm back to the animation.
void SetVrBodyHandTarget(int hand, bool valid, const float grip[3], const float fingers[3], const float down[3],
                         const float* cameraOrigin = nullptr, bool firstPersonAnchor = false,
                         RicoFingerInput fingersInput = {});
bool GetVrBodyRenderHandTarget(int hand,float grip[3],float fingers[3],float down[3]);
void BeginScopeSourceProbe(std::uintptr_t sniper);
bool ScopeSourceProbeActive();
bool IsScopeSourceBuffer(ID3D11Resource* resource);
void EndScopeSourceProbe();
// The last held object (gun) the carry moved on a hand: the game's own world
// matrix, the matrix JC3VR drew instead (both row-major 4x4), the bone it
// rode, and when (GetTickCount64). False until one has been carried.
bool GetVrBodyLastCarried(int hand, float original[16], float carried[16], int& bone, unsigned long long& tick);
// The gun mounts (hand-attach bones 26, 44, 66, 84) in Rico's model space,
// row-major 4x4: as the game posed them and as JC3VR draws them. valid[k] is
// false while that hand has no controller target.
// While true, the carry leaves objects on the hand-attach bones alone
// (vr_weapons has already moved the game's guns there).
void SetVrBodyGunsPlacedInGame(bool placed);
// Publish only successful CPU weapon placements; prevents a second render
// carry even when a tracked gun happens to be near a different animated bone.
void RegisterVrBodyPlacedWeapon(std::uintptr_t weapon, const float matrix[16], int hand=-1);
void RegisterVrBodyPlacedGrapple(const float matrix[16]);
void SetVrBodyTrackedGauntletReady(bool ready);
bool GetVrBodyAttachMounts(float game[4][16], float carried[4][16], bool valid[4], unsigned long long& tick);
// The latest of those (any thread); false while that hand has no target.
bool GetVrBodyHandTarget(int hand, float grip[3], float fingers[3], float down[3]);
// Controller-stable rest-pose attachment: left hand (25) or gauntlet (45).
// Wire marker 176 is animation-driven, not a usable rest-pose wrist socket.
bool GetVrBodyLeftWristMount(int bone, float world[16]);
// Render thread, at the Globals constant-buffer upload.
void VrBodyApplyAtUpload();
// True once Rico's rig is bound (cheap gate for per-draw checks).
bool VrBodyActive();
// Native pose-finalization hide uses the discovered head subtree, not NPCs
// or an assumed skeleton. Returns zero before validated Rico discovery.
bool VrBodyHeadHideEnabled();
std::size_t GetVrBodyHeadBones(std::uint32_t* bones, std::size_t capacity);
// Rico's CCharacter O (the address holding vtable 0x23d16d8), or 0 before
// discovery. ferrobrew/pyxis's Character base is O - 8.
std::uintptr_t VrBodyCharacter();
// Diagnostic: every buffer upload (Unmap / UpdateSubresource) is checked for
// Rico's palette for ~10 s after discovery.
void VrBodyObserveUpload(ID3D11Resource* resource, const void* data, std::size_t size, const char* how);
// Held objects follow the IK hands: at Unmap, before the upload reaches the
// GPU, a rigid object world matrix at a hand's animated position is moved by
// the same world-space correction the IK applied to that hand.
void ConfigureVrBodyCarry(bool enabled);
// Hide Rico entirely (palette collapsed) while the IK keeps carrying the gun.
void ConfigureVrBodyHideAll(bool hideAll);
// At Unmap of Rico's 4 KB / 528 B bone uploads: replace the game's entries
// (matched by value) with JC3VR's, so frames the game fills from its own
// copy of a fresh pose also get the IK / hidden body.
void VrBodyRewriteUpload(ID3D11Resource* resource, void* data);
void VrBodyCarryUpload(ID3D11Resource* resource, void* data);
// Hand orientation source for SetVrBodyHandTarget: the controller aim pose
// (true) or the grip pose. SteamVR's Quest grip runs along the handle: live,
// aim forward in the grip frame was (0, -0.866, -0.5), i.e. 60 deg off.
void ConfigureVrBodyHandFrame(bool aimFrame);
bool VrBodyHandsUseAimFrame();
}
