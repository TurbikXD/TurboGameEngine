#include "engine/ecs/render_system.h"
#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/core/Profiling.h"
#include "engine/ecs/components.h"
#include "engine/ecs/frustum.h"
#include "engine/ecs/transform_batch.h"
#include "engine/ecs/transform_utils.h"
#include "engine/ecs/world.h"
#include "engine/renderer/RenderAdapter.h"

namespace engine::ecs {

namespace {

renderer::SceneLighting sceneLighting(const World& world) {
    renderer::SceneLighting lighting;
    EntityId sun = kInvalidEntity;
    std::size_t pointCount = 0;
    bool authored = false;
    world.forEach<Transform, Light>([&](const EntityId entity, const Transform& transform, const Light& light) {
        authored = true;
        if (!light.enabled || light.intensity <= 0.0F) return;
        if (light.kind == LightKind::Directional) {
            if (sun != kInvalidEntity && sun <= entity) return;
            Transform rotation;
            rotation.rotationEulerRadians = transform.rotationEulerRadians;
            const glm::vec3 direction = computeParentWorldRotation(world, entity) * glm::vec3(rotation.toMatrix()[2]);
            const float lengthSquared = glm::dot(direction, direction);
            if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-8F) return;
            lighting.direction = direction / std::sqrt(lengthSquared);
            auto& color = lighting.constants.directionalColorIntensity;
            color[0] = light.color.r;
            color[1] = light.color.g;
            color[2] = light.color.b;
            color[3] = light.intensity;
            sun = entity;
            return;
        }
        if (pointCount >= rhi::MaxScenePointLights || light.range <= 0.0F) return;
        const glm::vec3 position(computeWorldMatrix(world, entity)[3]);
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return;
        lighting.constants.pointLights[pointCount++] = {
            {position.x, position.y, position.z, light.range},
            {light.color.r, light.color.g, light.color.b, light.intensity}};
    });
    if (authored && sun == kInvalidEntity) lighting.constants.directionalColorIntensity[3] = 0.0F;
    lighting.constants.pointLightInfo[0] = static_cast<float>(pointCount);
    return lighting;
}

}

void RenderSystem::render(
    World& world,
    renderer::RenderAdapter& renderer,
    const glm::mat4& viewProjectionMatrix,
    const EntityId previewEntity,
    const std::shared_ptr<resources::Material>& preview,
    float renderFraction) {
    ENGINE_PROFILE_ZONE("RenderSystem");
    m_submitted = m_culled = 0;
    const auto phase = world.stablePhase();
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
            if (meshRenderer.materialId.empty() && meshRenderer.texture == nullptr && !meshRenderer.textureId.empty()) {
                meshRenderer.texture = renderer.loadTexture(meshRenderer.textureId);
            }
            if (meshRenderer.materialId.empty() && meshRenderer.shader == nullptr && !meshRenderer.shaderId.empty()) {
                meshRenderer.shader = renderer.loadShaderProgram(meshRenderer.shaderId);
            }
            if (!meshRenderer.materialId.empty() && (!meshRenderer.material || meshRenderer.materialCacheKey != meshRenderer.materialId)) {
                meshRenderer.material = renderer.loadMaterial(meshRenderer.materialId);
                meshRenderer.materialCacheKey = meshRenderer.materialId;
            }
            m_entities.push_back(entity);
            m_renderers.push_back(&meshRenderer);
        });
    }

    const auto target = preview ? std::find(m_entities.begin(), m_entities.end(), previewEntity) : m_entities.end();
    const auto previewIndex = static_cast<std::size_t>(target - m_entities.begin());
    const bool hasPreviewTarget = target != m_entities.end() && m_renderers[previewIndex]->mesh &&
        !m_renderers[previewIndex]->materialId.empty() && m_renderers[previewIndex]->material &&
        m_renderers[previewIndex]->material->path == preview->path;
    const auto previewBlend = renderer.prepareMaterialPreview(hasPreviewTarget ? preview : nullptr);

    m_matrices.resize(m_entities.size());
    prepareWorldMatrices(world, m_entities, m_matrices, m_jobs, renderFraction);
    m_order.resize(m_entities.size());
    std::iota(m_order.begin(), m_order.end(), std::size_t{0});
    const auto transparent = [&](const std::size_t index) {
        if (previewBlend && index == previewIndex) return *previewBlend != resources::MaterialBlend::Opaque;
        const auto& item = *m_renderers[index];
        return !item.materialId.empty() && item.material && item.material->ready &&
            item.material->definition.blend != resources::MaterialBlend::Opaque;
    };
    const auto depth = [&](const std::size_t index) {
        const auto& mesh = m_renderers[index]->mesh;
        const glm::vec3 center = mesh && mesh->isReady() && mesh->hasBounds
            ? mesh->boundsMinimum * 0.5F + mesh->boundsMaximum * 0.5F : glm::vec3(0);
        const auto clip = viewProjectionMatrix * m_matrices[index] * glm::vec4(center, 1);
        const float value = clip.w > 0.00001F ? clip.z / clip.w : -1.0F;
        return std::isfinite(value) ? value : -1.0F;
    };
    if (std::any_of(m_order.begin(), m_order.end(), transparent)) std::stable_sort(m_order.begin(), m_order.end(), [&](const auto a, const auto b) {
        const bool ta = transparent(a), tb = transparent(b);
        if (ta != tb) return !ta;
        if (!ta) return false;
        return depth(a) > depth(b);
    });

    {
        ENGINE_PROFILE_ZONE("Render Submit");
        renderer.setSceneLighting(sceneLighting(world));
        for (const std::size_t index : m_order) {
            MeshRenderer& meshRenderer = *m_renderers[index];
            const glm::mat4& modelMatrix = m_matrices[index];
            if (meshRenderer.mesh && meshRenderer.mesh->isReady() && meshRenderer.mesh->hasBounds &&
                !intersectsFrustum(viewProjectionMatrix * modelMatrix, meshRenderer.mesh->boundsMinimum, meshRenderer.mesh->boundsMaximum)) { ++m_culled; continue; }
            ++m_submitted;
            if (previewBlend && index == previewIndex) {
                renderer.drawMaterialPreviewMesh(*meshRenderer.mesh, modelMatrix, viewProjectionMatrix,
                    meshRenderer.tint, meshRenderer.uvScale);
                continue;
            }
            if (meshRenderer.mesh && !meshRenderer.materialId.empty() && meshRenderer.material) {
                renderer.drawMaterialMesh(*meshRenderer.mesh, *meshRenderer.material, modelMatrix,
                    viewProjectionMatrix, meshRenderer.tint, meshRenderer.uvScale);
                continue;
            }
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
        renderer.setSceneLighting({});
    }
}

}
