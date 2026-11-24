#pragma once
#include <cstdint>

namespace jc3vr {
struct NativeGrabRay {
    float origin[3], direction[3];
};
using NativeGrabCast = bool(__fastcall*)(void*, const NativeGrabRay*, float, float, void*, void*, unsigned, int);

void ConfigureNativeGrabProbe(bool enabled, float reachMeters, float unitsPerMeter);
void PublishNativeGrabProbeGrips(bool handsMode, float left, float right);
void SampleNativeGrabProbe(void* physics, std::uintptr_t caller, NativeGrabCast cast);
}
