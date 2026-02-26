#pragma once

#include <cstdint>
#include <string>

#include "engine/rhi/Pipeline.h"

namespace engine::rhi::gl {

class GLShaderModule final : public IShaderModule {
public:
    GLShaderModule(ShaderStage stage, std::uint32_t shaderId, std::string sourcePath);
    ~GLShaderModule() override;

    ShaderStage stage() const override;
    std::uint32_t id() const;
    const std::string& sourcePath() const;

private:
    ShaderStage m_stage{ShaderStage::Vertex};
    std::uint32_t m_shaderId{0};
    std::string m_sourcePath;
};

} // namespace engine::rhi::gl
