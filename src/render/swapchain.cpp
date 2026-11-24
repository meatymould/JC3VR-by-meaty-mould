#include "render/swapchain.h"
#include <dxgi.h>

namespace jc3vr {
SwapchainObservation DescribeSwapchain(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC description{};
    if (!swapchain || FAILED(swapchain->GetDesc(&description))) return {};
    return {description.BufferDesc.Width, description.BufferDesc.Height, description.BufferCount};
}
}
