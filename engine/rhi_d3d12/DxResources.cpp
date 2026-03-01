#include "engine/rhi_d3d12/DxResources.h"

namespace engine::rhi::dx12 {

DxBufferResource::DxBufferResource(const BufferDesc& desc)
    : m_size(desc.size), m_usage(desc.usage), m_handle(reinterpret_cast<ResourceHandle>(this)) {}

std::size_t DxBufferResource::size() const {
    return m_size;
}

BufferUsage DxBufferResource::usage() const {
    return m_usage;
}

ResourceHandle DxBufferResource::handle() const {
    return m_handle;
}

DxImageResource::DxImageResource(const ImageDesc& desc)
    : m_width(desc.width), m_height(desc.height), m_handle(reinterpret_cast<ResourceHandle>(this)) {}

std::uint32_t DxImageResource::width() const {
    return m_width;
}

std::uint32_t DxImageResource::height() const {
    return m_height;
}

ResourceHandle DxImageResource::handle() const {
    return m_handle;
}

} // namespace engine::rhi::dx12
