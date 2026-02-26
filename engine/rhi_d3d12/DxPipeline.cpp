#include "engine/rhi_d3d12/DxPipeline.h"

namespace engine::rhi::dx12 {

DxShaderModuleResource::DxShaderModuleResource(const ShaderModuleDesc& desc)
    : m_stage(desc.stage), m_path(desc.path) {}

ShaderStage DxShaderModuleResource::stage() const {
    return m_stage;
}

DxGraphicsPipelineResource::DxGraphicsPipelineResource(PrimitiveTopology topology) : m_topology(topology) {}

} // namespace engine::rhi::dx12
