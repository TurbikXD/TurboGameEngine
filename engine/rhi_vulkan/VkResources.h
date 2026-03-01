#pragma once

#include "engine/rhi/Resources.h"

namespace engine::rhi::vk {

class VkBufferResource final : public IBuffer {
public:
    explicit VkBufferResource(const BufferDesc& desc);

    std::size_t size() const override;
    BufferUsage usage() const override;
    ResourceHandle handle() const override;

private:
    std::size_t m_size{0};
    BufferUsage m_usage{BufferUsage::Vertex};
    ResourceHandle m_handle{0};
};

class VkImageResource final : public IImage {
public:
    explicit VkImageResource(const ImageDesc& desc);

    std::uint32_t width() const override;
    std::uint32_t height() const override;
    ResourceHandle handle() const override;

private:
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    ResourceHandle m_handle{0};
};

} // namespace engine::rhi::vk
