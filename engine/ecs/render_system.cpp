#include "engine/ecs/render_system.h"

#include <glm/mat4x4.hpp>

#include "engine/ecs/components.h"
#include "engine/ecs/world.h"
#include "engine/renderer/RenderAdapter.h"

namespace engine::ecs {

namespace {

glm::mat4 computeWorldMatrix(World& world, const EntityId entity) {
    auto* transform = world.getComponent<Transform>(entity);
    if (transform == nullptr) {
        return glm::mat4(1.0F);
    }

    glm::mat4 worldMatrix = transform->toMatrix();
    EntityId parent = kInvalidEntity;
    if (const auto* hierarchy = world.getComponent<Hierarchy>(entity)) {
        parent = hierarchy->parent;
    }

    int safetyCounter = 0;
    while (parent != kInvalidEntity && safetyCounter < 64) {
        if (const auto* parentTransform = world.getComponent<Transform>(parent)) {
            worldMatrix = parentTransform->toMatrix() * worldMatrix;
        }

        const auto* parentHierarchy = world.getComponent<Hierarchy>(parent);
        if (parentHierarchy == nullptr || parentHierarchy->parent == parent) {
            break;
        }
        parent = parentHierarchy->parent;
        ++safetyCounter;
    }

    return worldMatrix;
}

} // namespace

void RenderSystem::render(
    World& world,
    renderer::RenderAdapter& renderer,
    const glm::mat4& viewProjectionMatrix) {
    world.forEach<Transform, MeshRenderer>([&](EntityId entity, Transform& transform, MeshRenderer& meshRenderer) {
        (void)transform;
        if (!meshRenderer.visible) {
            return;
        }

        if (meshRenderer.mesh == nullptr && !meshRenderer.meshId.empty()) {
            meshRenderer.mesh = renderer.loadMesh(meshRenderer.meshId);
        }
        if (meshRenderer.texture == nullptr && !meshRenderer.textureId.empty()) {
            meshRenderer.texture = renderer.loadTexture(meshRenderer.textureId);
        }
        if (meshRenderer.shader == nullptr && !meshRenderer.shaderId.empty()) {
            meshRenderer.shader = renderer.loadShaderProgram(meshRenderer.shaderId);
        }

        const glm::mat4 modelMatrix = computeWorldMatrix(world, entity);
        if (meshRenderer.hasGpuResources()) {
            renderer.drawMesh(
                *meshRenderer.mesh,
                *meshRenderer.shader,
                *meshRenderer.texture,
                viewProjectionMatrix * modelMatrix,
                meshRenderer.tint);
            return;
        }

        renderer.drawPrimitive(meshRenderer.primitiveType, modelMatrix, meshRenderer.tint);
    });
}

} // namespace engine::ecs
