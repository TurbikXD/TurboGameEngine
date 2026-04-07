#pragma once

#include <memory>
#include <string>
#include <atomic>

#include "engine/rhi/Pipeline.h"
#include "engine/resources/resource_state.h"

namespace engine::resources {

class ShaderProgram final {
public:
    std::string key;
    std::string vertexPath;
    std::string fragmentPath;
    std::string descriptorPath;
    std::unique_ptr<rhi::IShaderModule> vertexShader;
    std::unique_ptr<rhi::IShaderModule> fragmentShader;
    std::unique_ptr<rhi::IPipelineLayout> pipelineLayout;
    std::unique_ptr<rhi::IGraphicsPipeline> pipeline;
    std::string lastError;

    [[nodiscard]] ResourceLoadState loadState() const {
        return m_loadState.load(std::memory_order_relaxed);
    }

    void setLoadState(const ResourceLoadState state) {
        m_loadState.store(state, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isReady() const {
        return loadState() == ResourceLoadState::Loaded && vertexShader != nullptr && fragmentShader != nullptr &&
               pipelineLayout != nullptr && pipeline != nullptr;
    }

private:
    std::atomic<ResourceLoadState> m_loadState{ResourceLoadState::Unloaded};
};

} // namespace engine::resources
