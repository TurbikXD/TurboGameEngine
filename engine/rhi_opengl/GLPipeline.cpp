#include "engine/rhi_opengl/GLPipeline.h"

#include "engine/rhi_opengl/GLUtils.h"

namespace engine::rhi::gl {

GLGraphicsPipeline::GLGraphicsPipeline(std::uint32_t programId) : m_programId(programId) {
    m_transformUniformLocation = glGetUniformLocation(m_programId, "uTransform");
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

} // namespace engine::rhi::gl
