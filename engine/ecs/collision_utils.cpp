#include "engine/ecs/collision_utils.h"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "engine/ecs/transform_utils.h"
#include "engine/ecs/world.h"

namespace engine::ecs {

namespace {

glm::mat3 absoluteRotationScale(const glm::mat4& matrix) {
    glm::mat3 absMatrix = glm::mat3(matrix);
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            absMatrix[column][row] = std::abs(absMatrix[column][row]);
        }
    }
    return absMatrix;
}

bool intersectAabbAabb(
    EntityId entityA,
    const WorldAabb& aabbA,
    EntityId entityB,
    const WorldAabb& aabbB,
    CollisionContact& outContact) {
    const glm::vec3 overlap = glm::min(aabbA.max, aabbB.max) - glm::max(aabbA.min, aabbB.min);
    if (overlap.x <= 0.0F || overlap.y <= 0.0F || overlap.z <= 0.0F) {
        return false;
    }

    const glm::vec3 centerDelta = aabbB.center() - aabbA.center();

    outContact = CollisionContact{};
    outContact.entityA = entityA;
    outContact.entityB = entityB;
    outContact.contactPoint = (glm::max(aabbA.min, aabbB.min) + glm::min(aabbA.max, aabbB.max)) * 0.5F;

    if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
        outContact.penetrationDepth = overlap.x;
        outContact.normal = glm::vec3(centerDelta.x < 0.0F ? -1.0F : 1.0F, 0.0F, 0.0F);
        return true;
    }

    if (overlap.y <= overlap.z) {
        outContact.penetrationDepth = overlap.y;
        outContact.normal = glm::vec3(0.0F, centerDelta.y < 0.0F ? -1.0F : 1.0F, 0.0F);
        return true;
    }

    outContact.penetrationDepth = overlap.z;
    outContact.normal = glm::vec3(0.0F, 0.0F, centerDelta.z < 0.0F ? -1.0F : 1.0F);
    return true;
}

bool intersectSphereSphere(
    const EntityId entityA,
    const WorldSphere& sphereA,
    const EntityId entityB,
    const WorldSphere& sphereB,
    CollisionContact& outContact) {
    const glm::vec3 delta = sphereB.center - sphereA.center;
    const float distanceSq = glm::dot(delta, delta);
    const float radiusSum = sphereA.radius + sphereB.radius;
    if (distanceSq >= radiusSum * radiusSum) {
        return false;
    }

    const float distance = std::sqrt(std::max(distanceSq, 0.0F));
    glm::vec3 normal(1.0F, 0.0F, 0.0F);
    if (distance > 1e-5F) {
        normal = delta / distance;
    }

    const glm::vec3 pointOnA = sphereA.center + normal * sphereA.radius;
    const glm::vec3 pointOnB = sphereB.center - normal * sphereB.radius;

    outContact = CollisionContact{};
    outContact.entityA = entityA;
    outContact.entityB = entityB;
    outContact.normal = normal;
    outContact.penetrationDepth = radiusSum - distance;
    outContact.contactPoint = (pointOnA + pointOnB) * 0.5F;
    return true;
}

bool intersectAabbSphere(
    const EntityId entityA,
    const WorldAabb& aabbA,
    const EntityId entityB,
    const WorldSphere& sphereB,
    CollisionContact& outContact) {
    const glm::vec3 closestPoint = glm::clamp(sphereB.center, aabbA.min, aabbA.max);
    const glm::vec3 delta = sphereB.center - closestPoint;
    const float distanceSq = glm::dot(delta, delta);
    const float radiusSq = sphereB.radius * sphereB.radius;
    if (distanceSq > radiusSq) {
        return false;
    }

    glm::vec3 normal(0.0F, 1.0F, 0.0F);
    glm::vec3 contactPoint = closestPoint;
    float penetrationDepth = sphereB.radius;

    if (distanceSq > 1e-6F) {
        const float distance = std::sqrt(distanceSq);
        normal = delta / distance;
        penetrationDepth = sphereB.radius - distance;
    } else {
        const float distanceToMinX = sphereB.center.x - aabbA.min.x;
        const float distanceToMaxX = aabbA.max.x - sphereB.center.x;
        const float distanceToMinY = sphereB.center.y - aabbA.min.y;
        const float distanceToMaxY = aabbA.max.y - sphereB.center.y;
        const float distanceToMinZ = sphereB.center.z - aabbA.min.z;
        const float distanceToMaxZ = aabbA.max.z - sphereB.center.z;

        float nearestFaceDistance = distanceToMaxX;
        normal = glm::vec3(1.0F, 0.0F, 0.0F);
        contactPoint = glm::vec3(aabbA.max.x, sphereB.center.y, sphereB.center.z);

        if (distanceToMinX < nearestFaceDistance) {
            nearestFaceDistance = distanceToMinX;
            normal = glm::vec3(-1.0F, 0.0F, 0.0F);
            contactPoint = glm::vec3(aabbA.min.x, sphereB.center.y, sphereB.center.z);
        }
        if (distanceToMaxY < nearestFaceDistance) {
            nearestFaceDistance = distanceToMaxY;
            normal = glm::vec3(0.0F, 1.0F, 0.0F);
            contactPoint = glm::vec3(sphereB.center.x, aabbA.max.y, sphereB.center.z);
        }
        if (distanceToMinY < nearestFaceDistance) {
            nearestFaceDistance = distanceToMinY;
            normal = glm::vec3(0.0F, -1.0F, 0.0F);
            contactPoint = glm::vec3(sphereB.center.x, aabbA.min.y, sphereB.center.z);
        }
        if (distanceToMaxZ < nearestFaceDistance) {
            nearestFaceDistance = distanceToMaxZ;
            normal = glm::vec3(0.0F, 0.0F, 1.0F);
            contactPoint = glm::vec3(sphereB.center.x, sphereB.center.y, aabbA.max.z);
        }
        if (distanceToMinZ < nearestFaceDistance) {
            nearestFaceDistance = distanceToMinZ;
            normal = glm::vec3(0.0F, 0.0F, -1.0F);
            contactPoint = glm::vec3(sphereB.center.x, sphereB.center.y, aabbA.min.z);
        }

        penetrationDepth = sphereB.radius + nearestFaceDistance;
    }

    outContact = CollisionContact{};
    outContact.entityA = entityA;
    outContact.entityB = entityB;
    outContact.normal = normal;
    outContact.penetrationDepth = penetrationDepth;
    outContact.contactPoint = contactPoint;
    return true;
}

} // namespace

bool computeWorldColliderShape(const World& world, const EntityId entity, const Collider& collider, WorldColliderShape& outShape) {
    if (!collider.enabled) {
        return false;
    }

    const glm::mat4 worldMatrix = computeWorldMatrix(world, entity);
    const glm::vec3 worldCenter = transformPoint(worldMatrix, collider.offset);

    outShape = WorldColliderShape{};
    outShape.type = collider.type;

    switch (collider.type) {
        case ColliderType::Aabb: {
            const glm::mat3 absBasis = absoluteRotationScale(worldMatrix);
            const glm::vec3 worldHalfExtents = absBasis * collider.aabb.halfExtents;
            outShape.aabb.min = worldCenter - worldHalfExtents;
            outShape.aabb.max = worldCenter + worldHalfExtents;
            return true;
        }

        case ColliderType::Sphere: {
            const glm::vec3 worldScale = extractWorldScale(worldMatrix);
            const float scale = std::max(worldScale.x, std::max(worldScale.y, worldScale.z));
            outShape.sphere.center = worldCenter;
            outShape.sphere.radius = std::max(0.0F, collider.sphere.radius * scale);
            return outShape.sphere.radius > 0.0F;
        }
    }

    return false;
}

WorldAabb computeWorldBounds(const WorldColliderShape& shape) {
    switch (shape.type) {
        case ColliderType::Aabb:
            return shape.aabb;

        case ColliderType::Sphere: {
            const glm::vec3 radius(shape.sphere.radius);
            return WorldAabb{shape.sphere.center - radius, shape.sphere.center + radius};
        }
    }

    return WorldAabb{};
}

bool intersectColliders(
    const EntityId entityA,
    const WorldColliderShape& shapeA,
    const EntityId entityB,
    const WorldColliderShape& shapeB,
    CollisionContact& outContact) {
    if (shapeA.type == ColliderType::Aabb && shapeB.type == ColliderType::Aabb) {
        return intersectAabbAabb(entityA, shapeA.aabb, entityB, shapeB.aabb, outContact);
    }

    if (shapeA.type == ColliderType::Sphere && shapeB.type == ColliderType::Sphere) {
        return intersectSphereSphere(entityA, shapeA.sphere, entityB, shapeB.sphere, outContact);
    }

    if (shapeA.type == ColliderType::Aabb && shapeB.type == ColliderType::Sphere) {
        return intersectAabbSphere(entityA, shapeA.aabb, entityB, shapeB.sphere, outContact);
    }

    if (shapeA.type == ColliderType::Sphere && shapeB.type == ColliderType::Aabb) {
        if (!intersectAabbSphere(entityB, shapeB.aabb, entityA, shapeA.sphere, outContact)) {
            return false;
        }
        std::swap(outContact.entityA, outContact.entityB);
        outContact.normal *= -1.0F;
        return true;
    }

    return false;
}

} // namespace engine::ecs
