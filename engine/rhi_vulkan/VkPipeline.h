#pragma once

#include <string>

#include "engine/rhi/Pipeline.h"

namespace engine::rhi::vk {

class VkShaderModuleResource final : public IShaderModule {
public:
    explicit VkShaderModuleResource(const ShaderModuleDesc& desc);
    ShaderStage stage() const override;

private:
    ShaderStage m_stage{ShaderStage::Vertex};
    std::string m_path;
};

class VkPipelineLayoutResource final : public IPipelineLayout {};

class VkBindGroupLayoutResource final : public IBindGroupLayout {};

class VkBindGroupResource final : public IBindGroup {};

class VkGraphicsPipelineResource final : public IGraphicsPipeline {
public:
    explicit VkGraphicsPipelineResource(PrimitiveTopology topology);

private:
    PrimitiveTopology m_topology{PrimitiveTopology::TriangleList};
};

} // namespace engine::rhi::vk
