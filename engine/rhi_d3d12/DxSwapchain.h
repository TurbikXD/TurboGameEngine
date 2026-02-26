#pragma once

#include "engine/rhi/Swapchain.h"

namespace engine::rhi::dx12 {

class DxSwapchain final : public ISwapchain {
public:
    explicit DxSwapchain(const SwapchainDesc& desc);

    std::uint32_t acquireNextImage(ISemaphore* imageAvailable) override;
    void present(IQueue& queue, ISemaphore* renderFinished) override;
    void resize(std::uint32_t width, std::uint32_t height) override;
    Extent2D extent() const override;

private:
    Extent2D m_extent{};
};

} // namespace engine::rhi::dx12
