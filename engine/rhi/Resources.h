#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/rhi/Types.h"

namespace engine::rhi {

class IBuffer {
public:
    virtual ~IBuffer() = default;
    virtual std::size_t size() const = 0;
    virtual BufferUsage usage() const = 0;
    virtual ResourceHandle handle() const = 0;
};

class IImage {
public:
    virtual ~IImage() = default;
    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;
    virtual ResourceHandle handle() const = 0;
    // Borrowed backend SRV, valid while this image lives. ImGui's Diligent
    // renderer expects the view, not the underlying texture's native handle.
    virtual void* uiShaderResourceView() const { return nullptr; }
};

class IImageView {
public:
    virtual ~IImageView() = default;
};

class ISampler {
public:
    virtual ~ISampler() = default;
};

} // namespace engine::rhi
