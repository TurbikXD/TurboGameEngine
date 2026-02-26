#pragma once

#include <cstddef>

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
