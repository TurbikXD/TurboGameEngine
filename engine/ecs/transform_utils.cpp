#include "engine/ecs/transform_utils.h"

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include "engine/ecs/components.h"
#include "engine/ecs/world.h"

namespace engine::ecs {

glm::mat4 computeWorldMatrix(const World& world, const EntityId entity) {
    const auto* transform = world.getComponent<Transform>(entity);
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

glm::vec3 transformPoint(const glm::mat4& matrix, const glm::vec3& point) {
    return glm::vec3(matrix * glm::vec4(point, 1.0F));
}

glm::vec3 extractWorldScale(const glm::mat4& matrix) {
    const glm::vec3 axisX(matrix[0]);
    const glm::vec3 axisY(matrix[1]);
    const glm::vec3 axisZ(matrix[2]);
    return glm::vec3(glm::length(axisX), glm::length(axisY), glm::length(axisZ));
}

glm::vec3 forwardFromEuler(const glm::vec3& rotationEulerRadians) {
    const float pitch = rotationEulerRadians.x;
    const float yaw = rotationEulerRadians.y;
    const glm::vec3 forward(
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::cos(yaw));
    if (glm::dot(forward, forward) <= 1e-6F) {
        return glm::vec3(0.0F, 0.0F, 1.0F);
    }
    return glm::normalize(forward);
}

glm::vec3 rightFromEuler(const glm::vec3& rotationEulerRadians) {
    const glm::vec3 forward = forwardFromEuler(rotationEulerRadians);
    const glm::vec3 right = glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), forward);
    if (glm::dot(right, right) <= 1e-6F) {
        return glm::vec3(1.0F, 0.0F, 0.0F);
    }
    return glm::normalize(right);
}

glm::vec3 upFromEuler(const glm::vec3& rotationEulerRadians) {
    const glm::vec3 forward = forwardFromEuler(rotationEulerRadians);
    const glm::vec3 right = rightFromEuler(rotationEulerRadians);
    const glm::vec3 up = glm::cross(forward, right);
    if (glm::dot(up, up) <= 1e-6F) {
        return glm::vec3(0.0F, 1.0F, 0.0F);
    }
    return glm::normalize(up);
}

} // namespace engine::ecs
