#include "game/vr_gamepad.h"
#include "game/haptic_feedback.h"
#include "core/logging.h"
#include <Windows.h>
#include <Xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#if JC3VR_HAVE_MINHOOK
#include <MinHook.h>
#endif

namespace jc3vr {
namespace {
constexpr float kStickDeadzone = 0.12f;
constexpr float kGripThreshold = 0.55f;

void MapStick(float x, float y, std::int16_t& outX, std::int16_t& outY) {
    const float magnitude = std::sqrt(x * x + y * y);
    if (!(magnitude > kStickDeadzone)) { outX = outY = 0; return; }
    // Rescale past the deadzone so small deflections still start at zero.
    const float scaled = std::min(1.0f, (magnitude - kStickDeadzone) / (1.0f - kStickDeadzone)) / magnitude;
    outX = static_cast<std::int16_t>(std::clamp(x * scaled, -1.0f, 1.0f) * 32767.0f);
    outY = static_cast<std::int16_t>(std::clamp(y * scaled, -1.0f, 1.0f) * 32767.0f);
}

std::uint8_t MapTrigger(float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

std::atomic<bool> g_enabled = false;
std::atomic<bool> g_hapticsEnabled = true;
float g_hapticIntensity=0.7f;
HapticFeedback g_haptics;
std::atomic<bool> g_testInputEnabled = false;
std::mutex g_mutex;
VrControllerInputs g_inputs{};
ULONGLONG g_publishedAt = 0;
DWORD g_packet = 0;
VrGamepadState g_lastState{};
std::uint16_t g_injectedButtons = 0;
std::uint8_t g_injectedLeftTrigger = 0, g_injectedRightTrigger = 0;
ULONGLONG g_injectedUntil = 0;

bool FreshState(VrGamepadState& state) {
    std::lock_guard lock(g_mutex);
    const bool injecting = GetTickCount64() < g_injectedUntil;
    // Stale input (XR loop stalled, headset off) must never look held.
    if (!injecting && (!g_inputs.active || GetTickCount64() - g_publishedAt > 250)) return false;
    state = injecting && !g_inputs.active ? VrGamepadState{} : MapVrControllersToGamepad(g_inputs);
    if (injecting) {
        state.buttons |= g_injectedButtons;
        state.leftTrigger = std::max(state.leftTrigger, g_injectedLeftTrigger);
        state.rightTrigger = std::max(state.rightTrigger, g_injectedRightTrigger);
    }
    if (std::memcmp(&state, &g_lastState, sizeof(state)) != 0) {
        g_lastState = state;
        ++g_packet;
    }
    return true;
}

#if JC3VR_HAVE_MINHOOK
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
XInputGetStateFn g_originalGetState = nullptr;
XInputGetCapabilitiesFn g_originalGetCapabilities = nullptr;
using XInputSetStateFn=DWORD(WINAPI*)(DWORD,XINPUT_VIBRATION*);
XInputSetStateFn g_originalSetState=nullptr;
std::atomic<bool> g_rumbleBridgeReady=false;
DWORD WINAPI XInputSetStateDetour(DWORD userIndex,XINPUT_VIBRATION* vibration) {
    if(userIndex==0 && vibration && g_enabled.load() && g_hapticsEnabled.load()) {
        std::lock_guard lock(g_mutex);
        if(g_inputs.active && GetTickCount64()-g_publishedAt<=250) {
            g_haptics.Rumble(vibration->wLeftMotorSpeed,vibration->wRightMotorSpeed,GetTickCount64());
            return ERROR_SUCCESS;
        }
    }
    return g_originalSetState(userIndex,vibration);
}
std::atomic<std::uint64_t> g_injectedPolls = 0;

// Automation: %LOCALAPPDATA%\JC3VR\jc3vr_press_a presses pad A once. Polled
// here (twice a second) so it also works without an OpenXR session.
void PollAutomationTrigger() {
    if (!g_testInputEnabled.load()) return;
    static ULONGLONG nextCheck = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < nextCheck) return;
    nextCheck = now + 500;
    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    const std::string trigger = std::string(directory) + "\\JC3VR\\jc3vr_press_a";
    if (GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileA(trigger.c_str())) {
        {
            std::lock_guard lock(g_mutex);
            g_injectedLeftTrigger = g_injectedRightTrigger = 0;
        }
        InjectVrGamepadButtons(XINPUT_GAMEPAD_A, 250);
    }
    // %LOCALAPPDATA%\JC3VR\jc3vr_pad: "<control> <ms>" holds one pad control
    // (A B X Y LB RB LS RS UP DOWN LEFT RIGHT START BACK LT RT) for unattended
    // 2D tests (e.g. "LT 4000" aims).
    const std::string pad = std::string(directory) + "\\JC3VR\\jc3vr_pad";
    if (GetFileAttributesA(pad.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    char text[64]{};
    if (FILE* file = nullptr; fopen_s(&file, pad.c_str(), "rb") == 0 && file) {
        fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
    }
    DeleteFileA(pad.c_str());
    char control[16]{};
    unsigned milliseconds = 250;
    if (sscanf_s(text, "%15s %u", control, static_cast<unsigned>(sizeof(control)), &milliseconds) < 1) return;
    milliseconds = std::min(milliseconds, 20000u);
    const std::string name = control;
    static const std::pair<const char*, std::uint16_t> kButtons[]{
        {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
        {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"LS", XINPUT_GAMEPAD_LEFT_THUMB},
        {"RS", XINPUT_GAMEPAD_RIGHT_THUMB}, {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
        {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT}, {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK}};
    std::uint16_t buttons = 0;
    for (const auto& [label, mask] : kButtons) if (name == label) buttons = mask;
    {
        std::lock_guard lock(g_mutex);
        g_injectedLeftTrigger = name == "LT" ? 255 : 0;
        g_injectedRightTrigger = name == "RT" ? 255 : 0;
    }
    InjectVrGamepadButtons(buttons, milliseconds);
    TraceRuntime("VR gamepad: automation control " + name + " for " + std::to_string(milliseconds) + " ms");
}

DWORD WINAPI XInputGetStateDetour(DWORD userIndex, XINPUT_STATE* state) {
    if (userIndex == 0 && g_enabled.load()) PollAutomationTrigger();
    VrGamepadState pad{};
    if (userIndex == 0 && state && g_enabled.load() && FreshState(pad)) {
        std::memset(state, 0, sizeof(*state));
        {
            std::lock_guard lock(g_mutex);
            state->dwPacketNumber = g_packet;
        }
        state->Gamepad.wButtons = pad.buttons;
        state->Gamepad.bLeftTrigger = pad.leftTrigger;
        state->Gamepad.bRightTrigger = pad.rightTrigger;
        state->Gamepad.sThumbLX = pad.thumbLX;
        state->Gamepad.sThumbLY = pad.thumbLY;
        state->Gamepad.sThumbRX = pad.thumbRX;
        state->Gamepad.sThumbRY = pad.thumbRY;
        const auto polls = ++g_injectedPolls;
        if (polls == 1) TraceRuntime("VR gamepad: first injected XInput state for player 1");
        return ERROR_SUCCESS;
    }
    return g_originalGetState(userIndex, state);
}

DWORD WINAPI XInputGetCapabilitiesDetour(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* capabilities) {
    VrGamepadState pad{};
    if (userIndex == 0 && capabilities && g_enabled.load() && FreshState(pad)) {
        std::memset(capabilities, 0, sizeof(*capabilities));
        capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
        capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        capabilities->Gamepad.wButtons = 0xF3FF;
        capabilities->Gamepad.bLeftTrigger = capabilities->Gamepad.bRightTrigger = 0xFF;
        capabilities->Gamepad.sThumbLX = capabilities->Gamepad.sThumbLY = static_cast<SHORT>(0xFFC0);
        capabilities->Gamepad.sThumbRX = capabilities->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
        if(g_hapticsEnabled.load() && g_rumbleBridgeReady.load()) {
            capabilities->Vibration.wLeftMotorSpeed=capabilities->Vibration.wRightMotorSpeed=0xFFFF;
        }
        return ERROR_SUCCESS;
    }
    return g_originalGetCapabilities(userIndex, flags, capabilities);
}
#endif
}

VrGamepadState MapVrControllersToGamepad(const VrControllerInputs& inputs) {
    VrGamepadState state{};
    if (!inputs.active) return state;
    MapStick(inputs.leftStickX, inputs.leftStickY, state.thumbLX, state.thumbLY);
    MapStick(inputs.rightStickX, inputs.rightStickY, state.thumbRX, state.thumbRY);
    state.leftTrigger = MapTrigger(inputs.leftTrigger);
    state.rightTrigger = MapTrigger(inputs.rightTrigger);
    if (inputs.leftGrip > kGripThreshold) state.buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (inputs.rightGrip > kGripThreshold) state.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (inputs.a) state.buttons |= XINPUT_GAMEPAD_A;
    if (inputs.b) state.buttons |= XINPUT_GAMEPAD_B;
    if (inputs.x) state.buttons |= XINPUT_GAMEPAD_X;
    if (inputs.y) state.buttons |= XINPUT_GAMEPAD_Y;
    if (inputs.leftStickClick) state.buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (inputs.rightStickClick) state.buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    // The left menu button is not bound: on Quest it opens the Steam menu
    // (user 2026-10-02). The map is a left-stick hold (xr_session); the
    // pause menu uses a left X+Y hold chord (xr_session), via extraButtons.
    state.buttons |= inputs.extraButtons;
    return state;
}

void ConfigureVrGamepad(bool enabled) {
    g_enabled.store(enabled);
    char optIn[2]{};
    g_testInputEnabled.store(GetEnvironmentVariableA("JC3VR_ENABLE_TEST_INPUT",optIn,sizeof(optIn))==1 && optIn[0]=='1');
    TraceRuntime(std::string("Developer scripted input ")+(g_testInputEnabled.load()?"EXPLICITLY ENABLED":"disabled; test trigger files ignored"));
    TraceRuntime(std::string("VR gamepad (OpenXR controllers -> XInput player 1) ") + (enabled ? "enabled" : "disabled"));
}

void InjectVrGamepadButtons(std::uint16_t buttons, unsigned milliseconds) {
    if (!g_testInputEnabled.load()) return;
    std::lock_guard lock(g_mutex);
    g_injectedButtons = buttons;
    g_injectedUntil = GetTickCount64() + milliseconds;
    TraceRuntime("VR gamepad: automation button injection 0x" + std::to_string(buttons) + " for " + std::to_string(milliseconds) + " ms");
}

void PublishVrControllerInputs(const VrControllerInputs& inputs) {
    std::lock_guard lock(g_mutex);
    g_inputs = inputs;
    g_publishedAt = GetTickCount64();
}
void ConfigureVrHaptics(bool enabled,float intensity) {
    std::lock_guard lock(g_mutex);
    g_hapticsEnabled=enabled;g_hapticIntensity=std::isfinite(intensity)?std::clamp(intensity,0.f,1.f):0.7f;
    g_haptics={};
}
bool VrHapticsEnabled() { return g_hapticsEnabled.load(); }
void RequestVrHapticPulse(unsigned hand,float amplitude) {
    if(!g_hapticsEnabled.load()) return;
    std::lock_guard lock(g_mutex);g_haptics.Pulse(hand,amplitude,GetTickCount64());
}
std::array<float,2> GetVrHapticAmplitudes(bool active) {
    std::lock_guard lock(g_mutex);
    if(!active || !g_enabled.load() || !g_hapticsEnabled.load() || !g_inputs.active || GetTickCount64()-g_publishedAt>250) {
        g_haptics={};return {};
    }
    return g_haptics.Sample(GetTickCount64(),g_hapticIntensity);
}
bool DeveloperTestInputEnabled() { return g_testInputEnabled.load(); }

bool InstallVrGamepadHook() {
#if JC3VR_HAVE_MINHOOK
    if (!g_enabled.load()) return false;
    static bool installed = false;
    if (installed) return true;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    // JC3 statically imports XINPUT9_1_0.dll (see BUILD_FINGERPRINTS.md).
    const HMODULE xinput = GetModuleHandleW(L"XINPUT9_1_0.dll");
    void* getState = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    void* getCapabilities = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetCapabilities")) : nullptr;
    if (!getState || !getCapabilities ||
        MH_CreateHook(getState, &XInputGetStateDetour, reinterpret_cast<void**>(&g_originalGetState)) != MH_OK ||
        MH_CreateHook(getCapabilities, &XInputGetCapabilitiesDetour, reinterpret_cast<void**>(&g_originalGetCapabilities)) != MH_OK ||
        MH_EnableHook(getState) != MH_OK || MH_EnableHook(getCapabilities) != MH_OK) {
        TraceRuntime("VR gamepad: XInput hook unavailable; JC3 input unchanged");
        return false;
    }
    installed = true;
    // Optional independent hook: failure must never disable working input.
    void* setState=xinput?reinterpret_cast<void*>(GetProcAddress(xinput,"XInputSetState")):nullptr;
    if(g_hapticsEnabled.load() && setState) {
        const bool created=MH_CreateHook(setState,&XInputSetStateDetour,reinterpret_cast<void**>(&g_originalSetState))==MH_OK;
        const bool ready=created && MH_EnableHook(setState)==MH_OK;
        g_rumbleBridgeReady=ready;
        if(created && !ready) MH_RemoveHook(setState);
        TraceRuntime(ready?"VR haptics: native XInput rumble bridge installed":"VR haptics: native rumble bridge unavailable; controls unchanged");
    }
    TraceRuntime("VR gamepad: XINPUT9_1_0 XInputGetState/XInputGetCapabilities hooked");
    return true;
#else
    return false;
#endif
}
}
