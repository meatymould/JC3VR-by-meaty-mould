#pragma once
#include <cstdint>
#include <array>

namespace jc3vr {
// Raw OpenXR controller inputs for one frame (sticks -1..1, analog 0..1).
struct VrControllerInputs {
    bool active = false;
    float leftStickX = 0.0f, leftStickY = 0.0f;
    float rightStickX = 0.0f, rightStickY = 0.0f;
    float leftTrigger = 0.0f, rightTrigger = 0.0f;
    float leftGrip = 0.0f, rightGrip = 0.0f;
    bool a = false, b = false, x = false, y = false;
    bool leftStickClick = false, rightStickClick = false, menu = false;
    // XInput button bits JC3VR adds itself (gesture D-pad, deferred clicks).
    std::uint16_t extraButtons = 0;
};

// XInput-shaped gamepad state (XINPUT_GAMEPAD field layout and button bits).
struct VrGamepadState {
    std::uint16_t buttons = 0;
    std::uint8_t leftTrigger = 0, rightTrigger = 0;
    std::int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
};

// Maps Touch-style controllers onto JC3's Xbox layout: sticks -> sticks,
// triggers -> LT/RT, grips -> LB (grapple) / RB, A/B/X/Y -> A/B/X/Y, stick
// clicks -> LS/RS, left menu -> Start. Radial stick deadzone, grip threshold.
VrGamepadState MapVrControllersToGamepad(const VrControllerInputs& inputs);

// The virtual pad replaces XInput player 1 only while fresh controller input
// is being published; otherwise JC3 sees its real XInput devices unchanged.
void ConfigureVrGamepad(bool enabled);
void ConfigureVrHaptics(bool enabled,float intensity);
bool VrHapticsEnabled();
void RequestVrHapticPulse(unsigned hand,float amplitude);
std::array<float,2> GetVrHapticAmplitudes(bool active);
bool InstallVrGamepadHook();
void PublishVrControllerInputs(const VrControllerInputs& inputs);
// Test scripts cannot inject controls in normal play. Explicit process opt-in
// is required before launch: JC3VR_ENABLE_TEST_INPUT=1.
bool DeveloperTestInputEnabled();
// Automation: presses XInput buttons on player 1 for `milliseconds`
// (menus accept the pad), independent of controller tracking.
void InjectVrGamepadButtons(std::uint16_t buttons, unsigned milliseconds);
}
