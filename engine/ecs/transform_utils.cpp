#include "engine/ecs/transform_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include "engine/ecs/components.h"
#include "engine/ecs/render_interpolation.h"
#include "engine/ecs/world.h"

namespace engine::ecs {
namespace {

template <class Visitor>
bool visitHierarchy(const World& world, EntityId current, Visitor&& visitor) {
    std::array<EntityId, 64> visited{};
    std::size_t count = 0U;
    std::unordered_set<EntityId> extended;
    while (current != kInvalidEntity) {
        if (count < visited.size()) {
            const auto end = visited.begin() + static_cast<std::ptrdiff_t>(count);
            if (std::find(visited.begin(), end, current) != end) return false;
            visited[count++] = current;
        } else {
            if (extended.empty()) extended.insert(visited.begin(), visited.end());
            if (!extended.insert(current).second) return false;
        }
        if (!visitor(current)) return false;
        const auto* hierarchy = world.getComponent<Hierarchy>(current);
        current = hierarchy ? hierarchy->parent : kInvalidEntity;
    }
    return true;
}

glm::mat4 ancestorMatrix(const World& world, const EntityId entity, glm::mat4 result, float renderFraction = 1.0F) {
    visitHierarchy(world, entity, [&](const EntityId current) {
        if (current != entity) {
            if (const auto* transform = world.getComponent<Transform>(current)) {
                result = sampleRenderTransform(world, current, *transform, renderFraction).toMatrix() * result;
            }
        }
        return true;
    });
    return result;
}
}

glm::mat4 computeWorldMatrix(const World& world, const EntityId entity, float renderFraction) {
    const auto* transform = world.getComponent<Transform>(entity);
    if (!transform) return glm::mat4(1.0F);
    const auto local = sampleRenderTransform(world, entity, *transform, renderFraction).toMatrix();
    return ancestorMatrix(world, entity, local, renderFraction);
}

glm::mat4 computeParentWorldMatrix(const World& world, const EntityId entity) {
    return ancestorMatrix(world, entity, glm::mat4(1.0F));
}

glm::mat3 computeParentWorldRotation(const World& world, const EntityId entity) {
    glm::mat3 result(1.0F);
    visitHierarchy(world, entity, [&](const EntityId current) {
        if (current != entity) {
            if (const auto* transform = world.getComponent<Transform>(current)) {
                Transform rotation;
                rotation.rotationEulerRadians = transform->rotationEulerRadians;
                result = glm::mat3(rotation.toMatrix()) * result;
            }
        }
        return true;
    });
    return result;
}

bool isHierarchyParentValid(const World& world, const EntityId entity, const EntityId candidateParent) {
    if (!world.isAlive(entity)) return false;
    if (candidateParent == kInvalidEntity) return true;
    if (!world.isAlive(candidateParent)) return false;
    return visitHierarchy(world, candidateParent, [entity](const EntityId current) { return current != entity; });
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

}
