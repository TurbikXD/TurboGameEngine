#include "engine/rhi_vulkan/VkPipeline.h"

namespace engine::rhi::vk {

VkShaderModuleResource::VkShaderModuleResource(const ShaderModuleDesc& desc)
    : m_stage(desc.stage), m_path(desc.path) {}

ShaderStage VkShaderModuleResource::stage() const {
    return m_stage;
}

VkGraphicsPipelineResource::VkGraphicsPipelineResource(PrimitiveTopology topology) : m_topology(topology) {}

} // namespace engine::rhi::vk
