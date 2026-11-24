#pragma once
#include <cstdint>

struct IDXGISwapChain;

namespace jc3vr {
struct SwapchainObservation { std::uint32_t width = 0; std::uint32_t height = 0; std::uint32_t bufferCount = 0; };
SwapchainObservation DescribeSwapchain(IDXGISwapChain* swapchain);
}
