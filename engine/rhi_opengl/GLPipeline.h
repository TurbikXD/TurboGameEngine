#pragma once

#include <cstdint>

#include "engine/rhi/Pipeline.h"

namespace engine::rhi::gl {

class GLPipelineLayout final : public IPipelineLayout {};

class GLBindGroupLayout final : public IBindGroupLayout {};

class GLBindGroup final : public IBindGroup {};

class GLGraphicsPipeline final : public IGraphicsPipeline {
public:
    explicit GLGraphicsPipeline(std::uint32_t programId);
    ~GLGraphicsPipeline() override;

    std::uint32_t programId() const;
    int transformUniformLocation() const;

private:
    std::uint32_t m_programId{0};
    int m_transformUniformLocation{-1};
};

} // namespace engine::rhi::gl
