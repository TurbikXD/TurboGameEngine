#pragma once

#include <memory>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "engine/ecs/components.h"
#include "engine/renderer/Renderer.h"

namespace engine::renderer {

class RenderAdapter final {
public:
    explicit RenderAdapter(Renderer& renderer) : m_renderer(&renderer) {}

    std::shared_ptr<resources::Mesh> loadMesh(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadMesh(id) : nullptr;
    }

    std::shared_ptr<resources::Texture> loadTexture(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadTexture(id) : nullptr;
    }

    std::shared_ptr<resources::ShaderProgram> loadShaderProgram(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadShaderProgram(id) : nullptr;
    }

    std::unique_ptr<rhi::IBuffer> createBuffer(const rhi::BufferDesc& desc, const void* initialData) {
        return m_renderer != nullptr ? m_renderer->createGpuBuffer(desc, initialData) : nullptr;
    }

    std::unique_ptr<rhi::IImage> createImage(const rhi::ImageDesc& desc, const void* initialData) {
        return m_renderer != nullptr ? m_renderer->createGpuImage(desc, initialData) : nullptr;
    }

    std::shared_ptr<resources::ShaderProgram> createShaderProgram(
        const std::string& key,
        const std::string& vertexPath,
        const std::string& fragmentPath) {
        return m_renderer != nullptr ? m_renderer->createShaderProgramFromFiles(key, vertexPath, fragmentPath) : nullptr;
    }

    void drawPrimitive(
        ecs::PrimitiveType primitiveType,
        const glm::mat4& transformMatrix,
        const glm::vec4& tint) {
        if (m_renderer != nullptr) {
            m_renderer->drawPrimitive(primitiveType, transformMatrix, tint);
        }
    }

    void drawMesh(
        const resources::Mesh& mesh,
        const resources::ShaderProgram& shader,
        const resources::Texture* texture,
        const glm::mat4& modelMatrix,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& tint,
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F)) {
        if (m_renderer != nullptr) {
            m_renderer->drawMesh(mesh, shader, texture, modelMatrix, viewProjectionMatrix, tint, uvScale);
        }
    }

    void drawMesh(
        const resources::Mesh& mesh,
        const resources::ShaderProgram& shader,
        const resources::Texture& texture,
        const glm::mat4& modelMatrix,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& tint,
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F)) {
        if (m_renderer != nullptr) {
            m_renderer->drawMesh(mesh, shader, texture, modelMatrix, viewProjectionMatrix, tint, uvScale);
        }
    }

    void drawDebugAabb(
        const glm::vec3& min,
        const glm::vec3& max,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& color,
        const float lineThickness = 0.04F) {
        if (m_renderer != nullptr) {
            m_renderer->drawDebugAabb(min, max, viewProjectionMatrix, color, lineThickness);
        }
    }

private:
    Renderer* m_renderer{nullptr};
};

} // namespace engine::renderer
