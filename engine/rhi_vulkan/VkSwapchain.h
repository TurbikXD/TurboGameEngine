#pragma once

#include <vulkan/vulkan.h>

#include "engine/rhi/Swapchain.h"

namespace engine::rhi::vk {

class VkSwapchain final : public ISwapchain {
public:
    explicit VkSwapchain(const SwapchainDesc& desc);

    std::uint32_t acquireNextImage(ISemaphore* imageAvailable) override;
    void present(IQueue& queue, ISemaphore* renderFinished) override;
    void resize(std::uint32_t width, std::uint32_t height) override;
    Extent2D extent() const override;

private:
    Extent2D m_extent{};
    ::VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
};

} // namespace engine::rhi::vk
