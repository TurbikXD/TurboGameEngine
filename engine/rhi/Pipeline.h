#pragma once

#include "engine/rhi/Types.h"

namespace engine::rhi {

class IShaderModule {
public:
    virtual ~IShaderModule() = default;
    virtual ShaderStage stage() const = 0;
};

class IPipelineLayout {
public:
    virtual ~IPipelineLayout() = default;
};

class IGraphicsPipeline {
public:
    virtual ~IGraphicsPipeline() = default;
};

class IBindGroupLayout {
public:
    virtual ~IBindGroupLayout() = default;
};

class IBindGroup {
public:
    virtual ~IBindGroup() = default;
};

} // namespace engine::rhi
