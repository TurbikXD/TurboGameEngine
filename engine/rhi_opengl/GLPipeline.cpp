#include "engine/rhi_opengl/GLPipeline.h"

#include "engine/rhi_opengl/GLUtils.h"

namespace engine::rhi::gl {

GLGraphicsPipeline::GLGraphicsPipeline(std::uint32_t programId) : m_programId(programId) {
    m_transformUniformLocation = glGetUniformLocation(m_programId, "uTransform");
    m_tintUniformLocation = glGetUniformLocation(m_programId, "uTintColor");
    m_textureUniformLocation = glGetUniformLocation(m_programId, "uTexture");
}

GLGraphicsPipeline::~GLGraphicsPipeline() {
    if (m_programId != 0) {
        glDeleteProgram(m_programId);
        m_programId = 0;
    }
}

std::uint32_t GLGraphicsPipeline::programId() const {
    return m_programId;
}

int GLGraphicsPipeline::transformUniformLocation() const {
    return m_transformUniformLocation;
}

int GLGraphicsPipeline::tintUniformLocation() const {
    return m_tintUniformLocation;
}

int GLGraphicsPipeline::textureUniformLocation() const {
    return m_textureUniformLocation;
}

} // namespace engine::rhi::gl
