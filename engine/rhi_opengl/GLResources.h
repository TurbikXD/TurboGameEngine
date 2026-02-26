#pragma once

#include <cstdint>

#include "engine/rhi/Resources.h"

namespace engine::rhi::gl {

class GLBuffer final : public IBuffer {
public:
    GLBuffer(const BufferDesc& desc, const void* initialData);
    ~GLBuffer() override;

    std::size_t size() const override;
    BufferUsage usage() const override;
    ResourceHandle handle() const override;
    std::uint32_t id() const;

private:
    std::size_t m_size{0};
    BufferUsage m_usage{BufferUsage::Vertex};
    ResourceHandle m_handle{0};
    std::uint32_t m_bufferId{0};
};

} // namespace engine::rhi::gl
