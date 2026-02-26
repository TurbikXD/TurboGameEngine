#pragma once

#include "engine/rhi/Swapchain.h"

namespace engine::platform {
class Window;
}

namespace engine::rhi::gl {

class GLSwapchain final : public ISwapchain {
public:
    GLSwapchain(platform::Window& window, const SwapchainDesc& desc);
    ~GLSwapchain() override = default;

    std::uint32_t acquireNextImage(ISemaphore* imageAvailable) override;
    void present(IQueue& queue, ISemaphore* renderFinished) override;
    void resize(std::uint32_t width, std::uint32_t height) override;
    Extent2D extent() const override;

private:
    platform::Window& m_window;
    Extent2D m_extent{};
    bool m_vsync{true};
};

} // namespace engine::rhi::gl
