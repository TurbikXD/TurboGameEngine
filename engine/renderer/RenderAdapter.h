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
    explicit RenderAdapter(Renderer& renderer, const glm::vec3& cameraPosition = glm::vec3(0.0F))
        : m_renderer(renderer), m_cameraPosition(cameraPosition) {}

    void setSceneLighting(const SceneLighting& lighting) { m_renderer.setSceneLighting(lighting); }

    std::shared_ptr<resources::Mesh> loadMesh(const std::string& id) {
        return m_renderer.loadMesh(id);
    }

    std::shared_ptr<resources::Texture> loadTexture(const std::string& id) {
        return m_renderer.loadTexture(id);
    }

    std::shared_ptr<resources::ShaderProgram> loadShaderProgram(const std::string& id) {
        return m_renderer.loadShaderProgram(id);
    }
    std::shared_ptr<resources::Material> loadMaterial(const std::string& id) {
        return m_renderer.materialLibrary().load(id);
    }
    void drawMaterialMesh(const resources::Mesh& mesh, resources::Material& material,
        const glm::mat4& model, const glm::mat4& viewProjection, const glm::vec4& tint, const glm::vec2& uvScale) {
        m_renderer.drawMaterialMesh(mesh, material, model, viewProjection, tint, uvScale, m_cameraPosition);
    }
    [[nodiscard]] std::optional<resources::MaterialBlend> prepareMaterialPreview(
        const std::shared_ptr<resources::Material>& draft) {
        return m_renderer.prepareMaterialPreview(draft);
    }
    void drawMaterialPreviewMesh(const resources::Mesh& mesh, const glm::mat4& model,
        const glm::mat4& viewProjection, const glm::vec4& tint, const glm::vec2& uvScale) {
        m_renderer.drawMaterialPreviewMesh(mesh, model, viewProjection, tint, uvScale, m_cameraPosition);
    }

    std::unique_ptr<rhi::IBuffer> createBuffer(const rhi::BufferDesc& desc, const void* initialData) {
        return m_renderer.createGpuBuffer(desc, initialData);
    }

    std::unique_ptr<rhi::IImage> createImage(const rhi::ImageDesc& desc, const void* initialData) {
        return m_renderer.createGpuImage(desc, initialData);
    }

    std::shared_ptr<resources::ShaderProgram> createShaderProgram(
        const std::string& key,
        const std::string& vertexPath,
        const std::string& fragmentPath) {
        return m_renderer.createShaderProgramFromFiles(key, vertexPath, fragmentPath);
    }

    void drawPrimitive(
        ecs::PrimitiveType primitiveType,
        const glm::mat4& transformMatrix,
        const glm::vec4& tint) {
        m_renderer.drawPrimitive(primitiveType, transformMatrix, tint);
    }

    void drawMesh(
        const resources::Mesh& mesh,
        const resources::ShaderProgram& shader,
        const resources::Texture* texture,
        const glm::mat4& modelMatrix,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& tint,
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F)) {
        m_renderer.drawMesh(mesh, shader, texture, modelMatrix, viewProjectionMatrix, tint, uvScale);
    }

    void drawMesh(
        const resources::Mesh& mesh,
        const resources::ShaderProgram& shader,
        const resources::Texture& texture,
        const glm::mat4& modelMatrix,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& tint,
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F)) {
        m_renderer.drawMesh(mesh, shader, texture, modelMatrix, viewProjectionMatrix, tint, uvScale);
    }

    void drawDebugAabb(
        const glm::vec3& min,
        const glm::vec3& max,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& color,
        const float lineThickness = 0.04F) {
        m_renderer.drawDebugAabb(min, max, viewProjectionMatrix, color, lineThickness);
    }

private:
    Renderer& m_renderer;
    glm::vec3 m_cameraPosition;
};

}
