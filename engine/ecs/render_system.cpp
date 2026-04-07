#include "engine/ecs/render_system.h"

#include "engine/ecs/components.h"
#include "engine/ecs/transform_utils.h"
#include "engine/ecs/world.h"
#include "engine/renderer/RenderAdapter.h"

namespace engine::ecs {

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
        if (meshRenderer.mesh != nullptr && meshRenderer.shader != nullptr) {
            renderer.drawMesh(
                *meshRenderer.mesh,
                *meshRenderer.shader,
                meshRenderer.texture.get(),
                modelMatrix,
                viewProjectionMatrix,
                meshRenderer.tint,
                meshRenderer.uvScale);
            return;
        }

        renderer.drawPrimitive(meshRenderer.primitiveType, modelMatrix, meshRenderer.tint);
    });
}

} // namespace engine::ecs
