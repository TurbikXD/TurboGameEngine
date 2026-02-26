#pragma once

#include <cstdint>

#include "engine/rhi/Types.h"

namespace engine::rhi {

class IQueue;
class ISemaphore;

class ISwapchain {
public:
    virtual ~ISwapchain() = default;

    virtual std::uint32_t acquireNextImage(ISemaphore* imageAvailable) = 0;
    virtual void present(IQueue& queue, ISemaphore* renderFinished) = 0;
    virtual void resize(std::uint32_t width, std::uint32_t height) = 0;
    virtual Extent2D extent() const = 0;
};

} // namespace engine::rhi
