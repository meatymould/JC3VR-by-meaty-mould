#pragma once
#include <array>
#include <cstdint>
#include "support_grip.h"
#include "pocket_holster.h"
#include "render/rifle_scope.h"

// Weapons aimed by the controller itself.
//
// Everything the player fires at goes through one point: each frame the game
// raycasts along its camera and hands the hit to Character::UpdateWeaponAiming,
// which stores it in Character::m_AimTargetPositionWeapons; every held weapon
// then fires from its own muzzle toward that point. We swap that point for one
// along the right controller's aim ray, so bullets go where the gun points and
// the game camera no longer has to be swung to the hand first (no lag, no fire
// hold-back). Mechanism from ferrobrew/jc3vr's docs (aim-pipeline.md, MIT);
// the setter's bytes are verified in our exe before hooking.
namespace jc3vr {
struct VrWeaponOptions {
    bool aimFromController = true;
    // Put Rico's hand bones on the controllers inside the game's own pose
    // (see the note at InstallVrWeaponHooks).
    bool handsInGame = false;
    // Move the game's own gun (CWeaponData world matrices) to where JC3VR
    // draws it, so bullets and muzzle flash leave the drawn barrel.
    bool gunsToController = true;
    bool controllerRoll = true;  // full tracked aim frame, rather than animated roll
};
void ConfigureVrWeapons(const VrWeaponOptions& options);
// Hooks UpdateWeaponAiming (the aim point) and, with handsInGame,
// Character::UpdatePropEffects: the last step of the game's pose
// finalisation. Writing Rico's hand bones there (AnimationController::SetJoint,
// which re-derives every child bone) moves the hand-attach bones the guns
// ride, so the game itself draws the gun in the hand, fires from that
// muzzle, and puts the muzzle flash there; one gun per hand for dual wield.
// The render-side carry (vr_body_rig) then only corrects the last frame of
// controller motion. Mechanism: ferrobrew/jc3vr's docs
// (hands-locomotion-and-state.md section 1, skeleton.md), MIT.
bool InstallVrWeaponHooks();
// Per XR frame from the controller loop: the right controller's aim ray in
// JC3 world space (origin at the grip, direction along the aim pose's -Z),
// and whether the player is currently aiming with that hand.
void SetWeaponAimRay(bool valid, const float origin[3], const float direction[3]);
void PublishHelicopterGaze(std::uintptr_t vehicle, const float target[3]);
// True when the aim point is being supplied by the controller (the XR loop then
// leaves the engine camera on the head instead of steering it to the hand).
bool WeaponAimFromController();
// True when the game's simulation is not running at a normal rate (map, pause
// menus): fewer than 8 gun placements in the last 400 ms. False when unknown.
bool GameSimulationPaused();
// Present tick: the live toggles (Shift+F7 aim, Shift+F8 hands, Shift+F9
// guns) and stats.
void VrWeaponsPresentTick();
// Opt-in gesture prototype. Eligibility published by the existing character
// update, never by calling engine state functions from the XR/input thread.
void ConfigureHelicopterPullHijack(bool enabled);
std::uintptr_t GetExteriorPullHelicopter();
// Up/down/left/right; read-only inventory snapshot. -1 means unavailable.
std::array<int, 4> GetWeaponSelectorIconIds();
int GetEquippedWeaponIconId(); // same validated inventory, currently active only
bool RicoInventoryIsHolstered(); // visual pose only, unknown states preserve animation
void ConfigureRicoPocketHolster(bool enabled);
bool RicoPocketHolsterAvailable();
void UpdateRicoPocketRequest(bool hands, PocketAction action);
// Only a currently wielded two-handed/special weapon, never a holstered gun.
std::uintptr_t GetSupportGripWeapon();
SupportGripProfile GetSupportGripProfile(std::uintptr_t weapon);
// XR thread requests; the existing local-character simulation hook applies
// precision mode on the game thread. Requests expire on tracking/session loss.
void RequestSupportGripPrecision(bool enabled, std::uintptr_t weapon);
// Exact local equipped sniper only. Optics readiness expires on XR loss.
std::uintptr_t GetVrSniperWeapon();
void PublishVrScopeReady(std::uintptr_t weapon);
bool VrScopeReady(std::uintptr_t weapon);
SniperOpticMount GetVrSniperOpticMount(std::uintptr_t weapon);
}
