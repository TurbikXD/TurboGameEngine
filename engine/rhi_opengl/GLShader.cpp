#include "engine/rhi_opengl/GLShader.h"

#include <utility>

#include "engine/rhi_opengl/GLUtils.h"

namespace engine::rhi::gl {

GLShaderModule::GLShaderModule(ShaderStage stage, std::uint32_t shaderId, std::string sourcePath)
    : m_stage(stage), m_shaderId(shaderId), m_sourcePath(std::move(sourcePath)) {}

GLShaderModule::~GLShaderModule() {
    if (m_shaderId != 0) {
        glDeleteShader(m_shaderId);
        m_shaderId = 0;
    }
}

ShaderStage GLShaderModule::stage() const {
    return m_stage;
}

std::uint32_t GLShaderModule::id() const {
    return m_shaderId;
}

const std::string& GLShaderModule::sourcePath() const {
    return m_sourcePath;
}

} // namespace engine::rhi::gl
