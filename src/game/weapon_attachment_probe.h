#pragma once
#include <cstdint>
namespace jc3vr {
// Observation only. No hook until explicitly armed with F3 in the world.
bool WeaponAttachmentProbeActive();
void ObserveWeaponAttachmentPlacement(std::uintptr_t data,std::uintptr_t owner,unsigned slot,const float native[32]);
void WeaponAttachmentProbePresent();
}
