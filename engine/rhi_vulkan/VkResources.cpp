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

VkImageResource::VkImageResource(const ImageDesc& desc)
    : m_width(desc.width), m_height(desc.height), m_handle(reinterpret_cast<ResourceHandle>(this)) {}

std::uint32_t VkImageResource::width() const {
    return m_width;
}

std::uint32_t VkImageResource::height() const {
    return m_height;
}

ResourceHandle VkImageResource::handle() const {
    return m_handle;
}

} // namespace engine::rhi::vk
