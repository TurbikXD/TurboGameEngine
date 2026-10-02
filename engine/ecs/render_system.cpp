#include "engine/ecs/render_system.h"

#include <vector>

#include "engine/core/Profiling.h"
#include "engine/ecs/components.h"
#include "engine/ecs/transform_utils.h"
#include "engine/ecs/transform_batch.h"
#include "engine/ecs/world.h"
#include "engine/renderer/RenderAdapter.h"

namespace engine::ecs {

void RenderSystem::render(
    World& world,
    renderer::RenderAdapter& renderer,
    const glm::mat4& viewProjectionMatrix) {
    ENGINE_PROFILE_ZONE("RenderSystem");
    {
        ENGINE_PROFILE_ZONE("Render Gather");
        m_entities.clear();
        m_renderers.clear();
        m_entities.reserve(world.aliveCount());
        m_renderers.reserve(world.aliveCount());
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
            m_entities.push_back(entity);
            m_renderers.push_back(&meshRenderer);
        });
    }

    m_matrices.resize(m_entities.size());
    prepareWorldMatrices(world, m_entities, m_matrices, m_jobs);

    {
        ENGINE_PROFILE_ZONE("Render Submit");
        for (std::size_t index = 0; index < m_entities.size(); ++index) {
            MeshRenderer& meshRenderer = *m_renderers[index];
            const glm::mat4& modelMatrix = m_matrices[index];
            if (meshRenderer.mesh != nullptr && meshRenderer.shader != nullptr) {
                renderer.drawMesh(
                    *meshRenderer.mesh,
                    *meshRenderer.shader,
                    meshRenderer.texture.get(),
                    modelMatrix,
                    viewProjectionMatrix,
                    meshRenderer.tint,
                    meshRenderer.uvScale);
                continue;
            }

            renderer.drawPrimitive(meshRenderer.primitiveType, modelMatrix, meshRenderer.tint);
        }
    }
}

} // namespace engine::ecs
