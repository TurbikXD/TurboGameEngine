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

} // namespace engine::rhi::vk
