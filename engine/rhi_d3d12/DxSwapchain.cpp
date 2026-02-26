#include "engine/rhi_d3d12/DxSwapchain.h"

namespace engine::rhi::dx12 {

DxSwapchain::DxSwapchain(const SwapchainDesc& desc) : m_extent{desc.width, desc.height} {}

std::uint32_t DxSwapchain::acquireNextImage(ISemaphore* imageAvailable) {
    (void)imageAvailable;
    return 0;
}

void DxSwapchain::present(IQueue& queue, ISemaphore* renderFinished) {
    (void)queue;
    (void)renderFinished;
}

void DxSwapchain::resize(std::uint32_t width, std::uint32_t height) {
    m_extent = Extent2D{width, height};
}

Extent2D DxSwapchain::extent() const {
    return m_extent;
}

} // namespace engine::rhi::dx12
