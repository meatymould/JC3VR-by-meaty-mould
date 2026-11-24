#pragma once
#include <cstdint>

// Diagnostic: who touches the game's gun matrix?
//
// Arms x64 hardware data breakpoints (debug registers DR0-DR2) on every game
// thread for a short window, watching the held gun's CWeaponData world
// matrix translation (+0x1C8 in T1, +0x188 in T0). A vectored exception
// handler counts each instruction that writes or reads it, with three caller
// frames, and the window's report goes to the runtime log as exe RVAs. Read
// only with respect to game state: nothing in the game is changed, and the
// debug registers are cleared when the window ends.
//
// Trigger: %LOCALAPPDATA%\JC3VR\jc3vr_weapon_watch (needs a gun drawn, i.e.
// firing). Default off; costs nothing until triggered.
namespace jc3vr {
// From the present tick. `weaponData` is the CWeaponData* to watch (0 when
// none is held); the window lasts `milliseconds`.
void WeaponWatchTick(std::uintptr_t weaponData);
}
