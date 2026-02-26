#include "engine/rhi_vulkan/VkResources.h"

namespace engine::rhi::vk {

VkBufferResource::VkBufferResource(const BufferDesc& desc)
    : m_size(desc.size), m_usage(desc.usage), m_handle(reinterpret_cast<ResourceHandle>(this)) {}

std::size_t VkBufferResource::size() const {
    return m_size;
}

BufferUsage VkBufferResource::usage() const {
    return m_usage;
}

ResourceHandle VkBufferResource::handle() const {
    return m_handle;
}

} // namespace engine::rhi::vk
