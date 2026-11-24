#pragma once
#include <cstdint>
namespace jc3vr {
struct PhysicalHijackInput {
    bool active=false;
    std::uint64_t now=0;
    float grip=0,hand[3]{},head[3]{}; // unmodified OpenXR LOCAL metres
};
struct PhysicalHijackFeedback {bool consume=false,ready=false,grabbed=false,opened=false;};
void ConfigurePhysicalHijack(bool enabled);
bool InstallPhysicalHijackHooks();
PhysicalHijackFeedback SubmitPhysicalHijackInput(const PhysicalHijackInput& input);
void PhysicalHijackCharacterTick(std::uintptr_t characterBase);
}
