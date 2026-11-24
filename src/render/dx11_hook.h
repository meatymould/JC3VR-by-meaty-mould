#pragma once
#include <cstdint>

namespace jc3vr {
enum class StereoMode;
struct Dx11HookStatus {
    bool installed = false;
    std::uint64_t presentCount = 0;
    std::uint64_t resizeCount = 0;
};

// Mono mode builds an isolated dummy swapchain to discover the system DXGI
// vtable. Geo-11 mode instead waits for the game's real creation call so it
// never initializes a competing Geo-11 device.
// Replaces JC3's swapchain size at creation/resize (0 = off).
void ConfigureRenderResolution(unsigned width, unsigned height);
void ConfigureDx11StereoMode(StereoMode mode);
bool InstallDx11Hooks();
// Native/system DXGI only: discover Present's implementation with a disposable
// D3D11 device if the real creation call preceded bootstrap. Never presents,
// never owns an XR session, never runs for Geo-11/wrapper modes.
bool ArmNativePresentFallback();
Dx11HookStatus GetDx11HookStatus();
}
