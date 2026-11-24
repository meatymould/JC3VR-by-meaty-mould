#pragma once
#include <cstdint>
namespace jc3vr {
bool InstallVrC4Hooks();
std::uintptr_t GetVrC4Weapon();
void RequestVrC4Trigger(std::uintptr_t weapon, bool pressed,float worldUnitsPerMeter=1);
bool VrC4NativePress(std::uintptr_t weapon);
bool VrC4ContactReady(std::uintptr_t weapon);
}
