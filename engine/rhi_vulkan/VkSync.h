#pragma once

#include <vulkan/vulkan.h>

#include "engine/rhi/Sync.h"

namespace engine::rhi::vk {

class VkSemaphoreSync final : public ISemaphore {
public:
    explicit VkSemaphoreSync(::VkSemaphore handle = VK_NULL_HANDLE);
    ::VkSemaphore handle() const;

private:
    ::VkSemaphore m_handle{VK_NULL_HANDLE};
};

class VkFenceSync final : public IFence {
public:
    explicit VkFenceSync(bool signaled);

    void wait() override;
    void reset() override;
    void signal() override;

private:
    bool m_signaled{false};
};

} // namespace engine::rhi::vk
