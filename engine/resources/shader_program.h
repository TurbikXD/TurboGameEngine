#pragma once

#include <memory>
#include <string>

#include "engine/rhi/Pipeline.h"

namespace engine::resources {

class ShaderProgram final {
public:
    std::string key;
    std::string vertexPath;
    std::string fragmentPath;
    std::unique_ptr<rhi::IShaderModule> vertexShader;
    std::unique_ptr<rhi::IShaderModule> fragmentShader;
    std::unique_ptr<rhi::IPipelineLayout> pipelineLayout;
    std::unique_ptr<rhi::IGraphicsPipeline> pipeline;

    [[nodiscard]] bool isReady() const {
        return vertexShader != nullptr && fragmentShader != nullptr && pipelineLayout != nullptr && pipeline != nullptr;
    }
};

} // namespace engine::resources
