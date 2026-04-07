#pragma once

#include <glm/glm.hpp>

#include "engine/ecs/components.h"

namespace engine::ecs {

class World;

struct WorldAabb final {
    glm::vec3 min{0.0F, 0.0F, 0.0F};
    glm::vec3 max{0.0F, 0.0F, 0.0F};

    [[nodiscard]] glm::vec3 center() const {
        return (min + max) * 0.5F;
    }

    [[nodiscard]] glm::vec3 extents() const {
        return (max - min) * 0.5F;
    }
};

struct WorldSphere final {
    glm::vec3 center{0.0F, 0.0F, 0.0F};
    float radius{0.0F};
};

struct WorldColliderShape final {
    ColliderType type{ColliderType::Aabb};
    WorldAabb aabb{};
    WorldSphere sphere{};
};

struct CollisionContact final {
    EntityId entityA{kInvalidEntity};
    EntityId entityB{kInvalidEntity};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    float penetrationDepth{0.0F};
    glm::vec3 contactPoint{0.0F, 0.0F, 0.0F};
};

bool computeWorldColliderShape(const World& world, EntityId entity, const Collider& collider, WorldColliderShape& outShape);
WorldAabb computeWorldBounds(const WorldColliderShape& shape);
bool intersectColliders(
    EntityId entityA,
    const WorldColliderShape& shapeA,
    EntityId entityB,
    const WorldColliderShape& shapeB,
    CollisionContact& outContact);

} // namespace engine::ecs
