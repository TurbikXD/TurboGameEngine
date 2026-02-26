#include "engine/rhi_vulkan/VkSync.h"

namespace engine::rhi::vk {

VkSemaphoreSync::VkSemaphoreSync(::VkSemaphore handle) : m_handle(handle) {}

::VkSemaphore VkSemaphoreSync::handle() const {
    return m_handle;
}

VkFenceSync::VkFenceSync(bool signaled) : m_signaled(signaled) {}

void VkFenceSync::wait() {
    m_signaled = true;
}

void VkFenceSync::reset() {
    m_signaled = false;
}

void VkFenceSync::signal() {
    m_signaled = true;
}

} // namespace engine::rhi::vk
