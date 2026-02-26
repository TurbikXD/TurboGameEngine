#pragma once

#include <string>

#include "engine/rhi/Pipeline.h"

namespace engine::rhi::dx12 {

class DxShaderModuleResource final : public IShaderModule {
public:
    explicit DxShaderModuleResource(const ShaderModuleDesc& desc);
    ShaderStage stage() const override;

private:
    ShaderStage m_stage{ShaderStage::Vertex};
    std::string m_path;
};

class DxPipelineLayoutResource final : public IPipelineLayout {};

class DxBindGroupLayoutResource final : public IBindGroupLayout {};

class DxBindGroupResource final : public IBindGroup {};

class DxGraphicsPipelineResource final : public IGraphicsPipeline {
public:
    explicit DxGraphicsPipelineResource(PrimitiveTopology topology);

private:
    PrimitiveTopology m_topology{PrimitiveTopology::TriangleList};
};

} // namespace engine::rhi::dx12
