#include "engine/rhi_opengl/GLCommandBuffer.h"

#include <cstdint>
#include <cstring>

#include <glm/glm.hpp>

#include "engine/rhi_opengl/GLPipeline.h"
#include "engine/rhi_opengl/GLResources.h"
#include "engine/rhi_opengl/GLUtils.h"

namespace engine::rhi::gl {

GLCommandBuffer::GLCommandBuffer() {
    glGenVertexArrays(1, &m_vaoId);
}

GLCommandBuffer::~GLCommandBuffer() {
    if (m_vaoId != 0) {
        glDeleteVertexArrays(1, &m_vaoId);
        m_vaoId = 0;
    }
}

void GLCommandBuffer::begin() {
    m_recording = true;
    m_renderPassActive = false;
    m_boundPipeline = nullptr;
    m_indexBufferBound = false;
}

void GLCommandBuffer::end() {
    m_recording = false;
    m_renderPassActive = false;
    m_boundPipeline = nullptr;
}

void GLCommandBuffer::beginRenderPass(const RenderPassDesc& desc) {
    if (!m_recording) {
        return;
    }
    m_renderPassActive = true;
    glDisable(GL_SCISSOR_TEST);

    if (desc.clearColorTarget) {
        glClearColor(desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void GLCommandBuffer::endRenderPass() {
    m_renderPassActive = false;
}

void GLCommandBuffer::setViewport(const Viewport& viewport) {
    glViewport(
        static_cast<GLint>(viewport.x),
        static_cast<GLint>(viewport.y),
        static_cast<GLsizei>(viewport.width),
        static_cast<GLsizei>(viewport.height));
}

void GLCommandBuffer::setScissor(const Rect2D& scissor) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(
        scissor.x,
        scissor.y,
        static_cast<GLsizei>(scissor.width),
        static_cast<GLsizei>(scissor.height));
}

void GLCommandBuffer::bindPipeline(IGraphicsPipeline& pipeline) {
    m_boundPipeline = dynamic_cast<GLGraphicsPipeline*>(&pipeline);
    if (m_boundPipeline != nullptr) {
        glUseProgram(m_boundPipeline->programId());
    }
}

void GLCommandBuffer::bindVertexBuffer(IBuffer& buffer) {
    auto* glBuffer = dynamic_cast<GLBuffer*>(&buffer);
    if (glBuffer == nullptr) {
        return;
    }

    glBindVertexArray(m_vaoId);
    glBindBuffer(GL_ARRAY_BUFFER, glBuffer->id());
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);

    switch (glBuffer->vertexLayout()) {
        case VertexLayout::Position2Color3: {
            constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 5);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 2));
            break;
        }
        case VertexLayout::Position3Normal3Uv2: {
            constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 8);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 3));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(sizeof(float) * 6));
            break;
        }
        default:
            break;
    }
}

void GLCommandBuffer::bindIndexBuffer(IBuffer& buffer) {
    auto* glBuffer = dynamic_cast<GLBuffer*>(&buffer);
    if (glBuffer == nullptr) {
        return;
    }
    glBindVertexArray(m_vaoId);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, glBuffer->id());
    m_indexBufferBound = true;
}

void GLCommandBuffer::bindImage(std::uint32_t slot, IImage& image) {
    auto* glImage = dynamic_cast<GLImage*>(&image);
    if (glImage == nullptr) {
        return;
    }

    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, glImage->id());

    if (m_boundPipeline == nullptr) {
        return;
    }
    const GLint location = m_boundPipeline->textureUniformLocation();
    if (location >= 0) {
        glUniform1i(location, static_cast<GLint>(slot));
    }
}

void GLCommandBuffer::bindBindGroup(std::uint32_t slot, IBindGroup& bindGroup) {
    (void)slot;
    (void)bindGroup;
}

void GLCommandBuffer::pushConstants(const void* data, std::size_t size) {
    if (m_boundPipeline == nullptr || data == nullptr || size < sizeof(glm::mat4)) {
        return;
    }

    glm::mat4 matrix(1.0F);
    std::memcpy(&matrix, data, sizeof(glm::mat4));
    const GLint location = m_boundPipeline->transformUniformLocation();
    if (location >= 0) {
        glUniformMatrix4fv(location, 1, GL_FALSE, &matrix[0][0]);
    }

    const GLint tintLocation = m_boundPipeline->tintUniformLocation();
    if (tintLocation >= 0 && size >= sizeof(glm::mat4) + sizeof(glm::vec4)) {
        glm::vec4 tint(1.0F);
        std::memcpy(&tint, reinterpret_cast<const std::uint8_t*>(data) + sizeof(glm::mat4), sizeof(glm::vec4));
        glUniform4fv(tintLocation, 1, &tint[0]);
    }
}

void GLCommandBuffer::draw(std::uint32_t vertexCount, std::uint32_t firstVertex) {
    if (!m_renderPassActive) {
        return;
    }
    glBindVertexArray(m_vaoId);
    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount));
}

void GLCommandBuffer::drawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex) {
    if (!m_renderPassActive || !m_indexBufferBound) {
        return;
    }
    glBindVertexArray(m_vaoId);
    const auto offset = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(firstIndex * sizeof(std::uint32_t)));
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indexCount), GL_UNSIGNED_INT, offset);
}

void GLCommandBuffer::barrier(const BarrierDesc& barrierDesc) {
    (void)barrierDesc;
}

} // namespace engine::rhi::gl
