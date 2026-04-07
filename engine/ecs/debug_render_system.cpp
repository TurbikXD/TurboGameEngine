#include "engine/ecs/debug_render_system.h"

#include <algorithm>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include "engine/ecs/collision_utils.h"
#include "engine/ecs/world.h"
#include "engine/renderer/RenderAdapter.h"

namespace engine::ecs {

namespace {

constexpr const char* kDebugCubeMeshId = "__diligent_cube__";
constexpr const char* kDebugShaderId = "assets/shaders_hlsl/textured.shader.json";
constexpr int kSphereRingSegments = 24;

glm::mat4 buildSegmentModel(const glm::vec3& start, const glm::vec3& end, const float thickness) {
    const glm::vec3 delta = end - start;
    const float length = glm::length(delta);
    if (length <= 1e-5F) {
        return glm::mat4(1.0F);
    }

    const glm::vec3 axisX = delta / length;
    const glm::vec3 fallbackUp = std::abs(axisX.y) > 0.98F ? glm::vec3(0.0F, 0.0F, 1.0F) : glm::vec3(0.0F, 1.0F, 0.0F);
    const glm::vec3 axisZ = glm::normalize(glm::cross(axisX, fallbackUp));
    const glm::vec3 axisY = glm::normalize(glm::cross(axisZ, axisX));
    const glm::vec3 center = (start + end) * 0.5F;

    return glm::mat4(
        glm::vec4(axisX * length, 0.0F),
        glm::vec4(axisY * thickness, 0.0F),
        glm::vec4(axisZ * thickness, 0.0F),
        glm::vec4(center, 1.0F));
}

void drawDebugSphereRings(
    renderer::RenderAdapter& renderer,
    const WorldSphere& sphere,
    const glm::mat4& viewProjectionMatrix,
    const glm::vec4& color) {
    auto cubeMesh = renderer.loadMesh(kDebugCubeMeshId);
    auto shader = renderer.loadShaderProgram(kDebugShaderId);
    if (cubeMesh == nullptr || shader == nullptr) {
        return;
    }

    const float thickness = std::clamp(sphere.radius * 0.045F, 0.008F, 0.026F);
    const float radius = sphere.radius + thickness * 1.1F;

    const auto drawRing = [&](const glm::vec3& axisA, const glm::vec3& axisB) {
        glm::vec3 previousPoint = sphere.center + axisA * radius;
        for (int segmentIndex = 1; segmentIndex <= kSphereRingSegments; ++segmentIndex) {
            const float angle = (glm::two_pi<float>() * static_cast<float>(segmentIndex)) /
                                static_cast<float>(kSphereRingSegments);
            const glm::vec3 point =
                sphere.center + (axisA * std::cos(angle) + axisB * std::sin(angle)) * radius;
            const glm::mat4 model = buildSegmentModel(previousPoint, point, thickness);
            renderer.drawMesh(*cubeMesh, *shader, nullptr, model, viewProjectionMatrix, color);
            previousPoint = point;
        }
    };

    drawRing(glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    drawRing(glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F));
    drawRing(glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F));
}

}

void DebugRenderSystem::render(
    World& world,
    renderer::RenderAdapter& renderer,
    const glm::mat4& viewProjectionMatrix) {
    world.forEach<Transform, Collider>([&](EntityId entity, Transform& transform, Collider& collider) {
        (void)transform;

        WorldColliderShape worldShape{};
        if (!computeWorldColliderShape(world, entity, collider, worldShape)) {
            return;
        }

        const WorldAabb bounds = computeWorldBounds(worldShape);
        const auto* rigidbody = world.getComponent<Rigidbody>(entity);
        const glm::vec4 color =
            (rigidbody != nullptr && rigidbody->isDynamic()) ? glm::vec4(0.25F, 0.95F, 0.45F, 1.0F)
                                                             : glm::vec4(0.95F, 0.7F, 0.2F, 1.0F);

        if (worldShape.type == ColliderType::Sphere) {
            drawDebugSphereRings(renderer, worldShape.sphere, viewProjectionMatrix, color);
            return;
        }

        renderer.drawDebugAabb(bounds.min, bounds.max, viewProjectionMatrix, color);
    });
}

} // namespace engine::ecs
