#include "engine/rhi_vulkan/VkSwapchain.h"

namespace engine::rhi::vk {

VkSwapchain::VkSwapchain(const SwapchainDesc& desc) : m_extent{desc.width, desc.height} {}

std::uint32_t VkSwapchain::acquireNextImage(ISemaphore* imageAvailable) {
    (void)imageAvailable;
    return 0;
}

void VkSwapchain::present(IQueue& queue, ISemaphore* renderFinished) {
    (void)queue;
    (void)renderFinished;
}

void VkSwapchain::resize(std::uint32_t width, std::uint32_t height) {
    m_extent = Extent2D{width, height};
}

Extent2D VkSwapchain::extent() const {
    return m_extent;
}

} // namespace engine::rhi::vk
