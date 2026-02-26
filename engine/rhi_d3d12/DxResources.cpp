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

} // namespace engine::rhi::dx12
