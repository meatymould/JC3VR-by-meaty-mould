#pragma once
#include "openxr/xr_session.h"
#include <string>

namespace jc3vr {
struct Config;

struct HeadRotationDelta {
    float yawRadians = 0.0f;
    float pitchRadians = 0.0f;
};

HeadRotationDelta CalculateHeadRotationDelta(const OpenXrHeadPose& previous, const OpenXrHeadPose& current);
void ConfigureHeadLook(const Config& config);
std::string ApplyHeadLook(const OpenXrHeadPose& pose);
}
