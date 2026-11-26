#include "engine/ecs/collision_utils.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "engine/ecs/transform_utils.h"
#include "engine/ecs/world.h"

namespace engine::ecs {

namespace {

bool finiteVector(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validAabb(const WorldAabb& aabb) {
    return finiteVector(aabb.min) && finiteVector(aabb.max) &&
        aabb.min.x <= aabb.max.x && aabb.min.y <= aabb.max.y && aabb.min.z <= aabb.max.z;
}

bool validShape(const WorldColliderShape& shape) {
    switch (shape.type) {
        case ColliderType::Aabb: return validAabb(shape.aabb);
        case ColliderType::Sphere:
            return finiteVector(shape.sphere.center) && std::isfinite(shape.sphere.radius) && shape.sphere.radius > 0;
    }
    return false;
}

float contactDepth(const double depth) {
    return static_cast<float>(std::min(depth, static_cast<double>(std::numeric_limits<float>::max())));
}

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
    const glm::dvec3 overlap = glm::min(glm::dvec3(aabbA.max) - glm::dvec3(aabbB.min),
        glm::dvec3(aabbB.max) - glm::dvec3(aabbA.min));
    if (overlap.x <= 0.0F || overlap.y <= 0.0F || overlap.z <= 0.0F) {
        return false;
    }

    const glm::vec3 centerDelta = aabbB.center() - aabbA.center();

    outContact = CollisionContact{};
    outContact.entityA = entityA;
    outContact.entityB = entityB;
    outContact.contactPoint = glm::max(aabbA.min, aabbB.min) * 0.5F + glm::min(aabbA.max, aabbB.max) * 0.5F;

    if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
        outContact.penetrationDepth = contactDepth(overlap.x);
        outContact.normal = glm::vec3(centerDelta.x < 0.0F ? -1.0F : 1.0F, 0.0F, 0.0F);
        return true;
    }

    if (overlap.y <= overlap.z) {
        outContact.penetrationDepth = contactDepth(overlap.y);
        outContact.normal = glm::vec3(0.0F, centerDelta.y < 0.0F ? -1.0F : 1.0F, 0.0F);
        return true;
    }

    outContact.penetrationDepth = contactDepth(overlap.z);
    outContact.normal = glm::vec3(0.0F, 0.0F, centerDelta.z < 0.0F ? -1.0F : 1.0F);
    return true;
}

bool intersectSphereSphere(
    const EntityId entityA,
    const WorldSphere& sphereA,
    const EntityId entityB,
    const WorldSphere& sphereB,
    CollisionContact& outContact) {
    const glm::dvec3 delta = glm::dvec3(sphereB.center) - glm::dvec3(sphereA.center);
    const double distanceSq = glm::dot(delta, delta);
    const double radiusSum = static_cast<double>(sphereA.radius) + sphereB.radius;
    if (distanceSq >= radiusSum * radiusSum) {
        return false;
    }

    const double distance = std::sqrt(std::max(distanceSq, 0.0));
    glm::vec3 normal(1.0F, 0.0F, 0.0F);
    if (distance > 0.0) {
        normal = glm::vec3(delta / distance);
    }

    const glm::dvec3 pointOnA = glm::dvec3(sphereA.center) + glm::dvec3(normal) * static_cast<double>(sphereA.radius);
    const glm::dvec3 pointOnB = glm::dvec3(sphereB.center) - glm::dvec3(normal) * static_cast<double>(sphereB.radius);

    outContact = CollisionContact{};
    outContact.entityA = entityA;
    outContact.entityB = entityB;
    outContact.normal = normal;
    outContact.penetrationDepth = contactDepth(radiusSum - distance);
    outContact.contactPoint = glm::vec3((pointOnA + pointOnB) * 0.5);
    return true;
}

bool intersectAabbSphere(
    const EntityId entityA,
    const WorldAabb& aabbA,
    const EntityId entityB,
    const WorldSphere& sphereB,
    CollisionContact& outContact) {
    const glm::vec3 closestPoint = glm::clamp(sphereB.center, aabbA.min, aabbA.max);
    const glm::dvec3 delta = glm::dvec3(sphereB.center) - glm::dvec3(closestPoint);
    const double distanceSq = glm::dot(delta, delta);
    const double radiusSq = static_cast<double>(sphereB.radius) * sphereB.radius;
    if (distanceSq > radiusSq) {
        return false;
    }

    glm::vec3 normal(0.0F, 1.0F, 0.0F);
    glm::vec3 contactPoint = closestPoint;
    double penetrationDepth = sphereB.radius;

    if (distanceSq > 0.0) {
        const double distance = std::sqrt(distanceSq);
        normal = glm::vec3(delta / distance);
        penetrationDepth = sphereB.radius - distance;
    } else {
        const double distanceToMinX = static_cast<double>(sphereB.center.x) - aabbA.min.x;
        const double distanceToMaxX = static_cast<double>(aabbA.max.x) - sphereB.center.x;
        const double distanceToMinY = static_cast<double>(sphereB.center.y) - aabbA.min.y;
        const double distanceToMaxY = static_cast<double>(aabbA.max.y) - sphereB.center.y;
        const double distanceToMinZ = static_cast<double>(sphereB.center.z) - aabbA.min.z;
        const double distanceToMaxZ = static_cast<double>(aabbA.max.z) - sphereB.center.z;

        double nearestFaceDistance = distanceToMaxX;
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
    outContact.penetrationDepth = contactDepth(penetrationDepth);
    outContact.contactPoint = contactPoint;
    return true;
}

}

bool computeWorldColliderShape(const World& world, const EntityId entity, const Collider& collider, WorldColliderShape& outShape) {
    return computeWorldColliderShape(computeWorldMatrix(world, entity), collider, outShape);
}

bool computeWorldColliderShape(const glm::mat4& worldMatrix, const Collider& collider, WorldColliderShape& outShape) {
    if (!collider.enabled) {
        return false;
    }

    const glm::vec3 worldCenter = transformPoint(worldMatrix, collider.offset);
    if (!finiteVector(worldCenter)) return false;

    outShape = WorldColliderShape{};
    outShape.type = collider.type;

    switch (collider.type) {
        case ColliderType::Aabb: {
            if (!finiteVector(collider.aabb.halfExtents) || collider.aabb.halfExtents.x < 0 ||
                collider.aabb.halfExtents.y < 0 || collider.aabb.halfExtents.z < 0) return false;
            const glm::mat3 absBasis = absoluteRotationScale(worldMatrix);
            const glm::vec3 worldHalfExtents = absBasis * collider.aabb.halfExtents;
            outShape.aabb.min = worldCenter - worldHalfExtents;
            outShape.aabb.max = worldCenter + worldHalfExtents;
            return validAabb(outShape.aabb);
        }

        case ColliderType::Sphere: {
            if (!std::isfinite(collider.sphere.radius) || collider.sphere.radius <= 0) return false;
            const double scale = std::max({glm::length(glm::dvec3(worldMatrix[0])),
                glm::length(glm::dvec3(worldMatrix[1])), glm::length(glm::dvec3(worldMatrix[2]))});
            const double radius = collider.sphere.radius * scale;
            if (!std::isfinite(radius) || radius > std::numeric_limits<float>::max()) return false;
            outShape.sphere.center = worldCenter;
            outShape.sphere.radius = static_cast<float>(radius);
            return validShape(outShape) && validAabb(computeWorldBounds(outShape));
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
    if (!validShape(shapeA) || !validShape(shapeB)) return false;
    CollisionContact contact;
    bool intersects = false;
    if (shapeA.type == ColliderType::Aabb && shapeB.type == ColliderType::Aabb) {
        intersects = intersectAabbAabb(entityA, shapeA.aabb, entityB, shapeB.aabb, contact);
    } else if (shapeA.type == ColliderType::Sphere && shapeB.type == ColliderType::Sphere) {
        intersects = intersectSphereSphere(entityA, shapeA.sphere, entityB, shapeB.sphere, contact);
    } else if (shapeA.type == ColliderType::Aabb && shapeB.type == ColliderType::Sphere) {
        intersects = intersectAabbSphere(entityA, shapeA.aabb, entityB, shapeB.sphere, contact);
    } else if (shapeA.type == ColliderType::Sphere && shapeB.type == ColliderType::Aabb) {
        intersects = intersectAabbSphere(entityB, shapeB.aabb, entityA, shapeA.sphere, contact);
        if (intersects) {
            std::swap(contact.entityA, contact.entityB);
            contact.normal *= -1.0F;
        }
    }
    if (!intersects || !finiteVector(contact.contactPoint) || !finiteVector(contact.normal) ||
        !std::isfinite(contact.penetrationDepth)) return false;
    outContact = contact;
    return true;
}

}
