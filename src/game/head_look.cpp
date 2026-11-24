#include "game/head_look.h"
#include "core/config.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace jc3vr {
namespace {
struct HeadLookState {
    Config config{};
    bool configured = false;
    bool hasPreviousPose = false;
    OpenXrHeadPose previousPose{};
    float horizontalRemainder = 0.0f;
    float verticalRemainder = 0.0f;
    bool foregroundNoticeLogged = false;
    bool dispatchLogged = false;
};

HeadLookState g_state;
std::mutex g_mutex;

bool IsGameForeground() {
    DWORD foregroundPid = 0;
    if (const HWND window = GetForegroundWindow()) GetWindowThreadProcessId(window, &foregroundPid);
    return foregroundPid == GetCurrentProcessId();
}
}

HeadRotationDelta CalculateHeadRotationDelta(const OpenXrHeadPose& previous, const OpenXrHeadPose& current) {
    // OpenXR's orientation is a unit quaternion in the usual X(pitch), Y(yaw),
    // Z(roll) convention. Compose inverse(previous) * current so this is a
    // true head delta, not an absolute orientation or a forced recenter.
    const float x = previous.orientationW * current.orientationX - previous.orientationX * current.orientationW - previous.orientationY * current.orientationZ + previous.orientationZ * current.orientationY;
    const float y = previous.orientationW * current.orientationY + previous.orientationX * current.orientationZ - previous.orientationY * current.orientationW - previous.orientationZ * current.orientationX;
    const float z = previous.orientationW * current.orientationZ - previous.orientationX * current.orientationY + previous.orientationY * current.orientationX - previous.orientationZ * current.orientationW;
    const float w = previous.orientationW * current.orientationW + previous.orientationX * current.orientationX + previous.orientationY * current.orientationY + previous.orientationZ * current.orientationZ;
    const float yaw = std::atan2(2.0f * (w * y + x * z), 1.0f - 2.0f * (x * x + y * y));
    const float pitch = std::asin(std::clamp(2.0f * (w * x - y * z), -1.0f, 1.0f));
    return {yaw, pitch};
}

void ConfigureHeadLook(const Config& config) {
    std::lock_guard lock(g_mutex);
    g_state.config = config;
    g_state.configured = true;
}

std::string ApplyHeadLook(const OpenXrHeadPose& pose) {
    std::lock_guard lock(g_mutex);
    if (!g_state.configured || !g_state.config.headTracking) return "HMD head-look disabled by configuration";
    if (g_state.config.directHeadRotation) return "HMD head-look uses direct native camera rotation; mouse bridge disabled";
    if (!pose.valid) return "HMD head-look waiting for a valid OpenXR pose";
    if (!g_state.hasPreviousPose) {
        g_state.previousPose = pose;
        g_state.hasPreviousPose = true;
        return "HMD head-look pose baseline established";
    }
    if (pose.sequence == g_state.previousPose.sequence) return "HMD head-look waiting for a newer pose";

    const HeadRotationDelta delta = CalculateHeadRotationDelta(g_state.previousPose, pose);
    g_state.previousPose = pose;
    // Mouse deltas are intentionally sent only through the normal Windows input
    // path and only while JC3 owns the foreground. No game memory is modified.
    constexpr float mouseCountsPerRadian = 800.0f;
    g_state.horizontalRemainder += -delta.yawRadians * mouseCountsPerRadian * g_state.config.yawScale;
    g_state.verticalRemainder += -delta.pitchRadians * mouseCountsPerRadian * g_state.config.pitchScale;
    const LONG dx = static_cast<LONG>(std::trunc(g_state.horizontalRemainder));
    const LONG dy = static_cast<LONG>(std::trunc(g_state.verticalRemainder));
    g_state.horizontalRemainder -= static_cast<float>(dx);
    g_state.verticalRemainder -= static_cast<float>(dy);
    if (dx == 0 && dy == 0) return "HMD head-look awaiting a measurable rotation";
    if (!IsGameForeground()) {
        if (!g_state.foregroundNoticeLogged) {
            g_state.foregroundNoticeLogged = true;
            return "HMD head-look is armed but paused because JC3 is not the foreground window";
        }
        return "HMD head-look waiting for JC3 to be foreground";
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    if (SendInput(1, &input, sizeof(input)) != 1) return "HMD head-look SendInput failed";
    if (!g_state.dispatchLogged) {
        g_state.dispatchLogged = true;
        return "HMD head-look is dispatching foreground-only relative mouse input";
    }
    return "HMD head-look dispatched";
}
}
