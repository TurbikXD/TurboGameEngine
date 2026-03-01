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
    VertexLayout vertexLayout() const;
    std::uint32_t id() const;

private:
    std::size_t m_size{0};
    BufferUsage m_usage{BufferUsage::Vertex};
    VertexLayout m_vertexLayout{VertexLayout::Position2Color3};
    ResourceHandle m_handle{0};
    std::uint32_t m_bufferId{0};
};

class GLImage final : public IImage {
public:
    GLImage(const ImageDesc& desc, const void* initialData);
    ~GLImage() override;

    std::uint32_t width() const override;
    std::uint32_t height() const override;
    ResourceHandle handle() const override;
    std::uint32_t id() const;

private:
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    ResourceHandle m_handle{0};
    std::uint32_t m_textureId{0};
};

} // namespace engine::rhi::gl
