#pragma once
#include <cstdint>
namespace jc3vr {
void ConfigureHijackStateProbe(bool enabled);
bool HijackStateProbeActive();
// Only existing validated local-character callback thread may call this.
void ObserveHijackState(std::uintptr_t characterBase,std::uintptr_t helicopter);
void HijackStateProbePresent();
}
