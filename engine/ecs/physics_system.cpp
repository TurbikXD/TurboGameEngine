#include "engine/ecs/physics_system.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include "engine/ecs/world.h"

namespace engine::ecs {

namespace {

struct BodyProxy final {
    EntityId entity{kInvalidEntity};
    Transform* transform{nullptr};
    Rigidbody* rigidbody{nullptr};
    Collider* collider{nullptr};
    WorldColliderShape worldShape{};
    WorldAabb worldBounds{};
};

struct CellKey final {
    int x{0};
    int y{0};
    int z{0};

    [[nodiscard]] bool operator==(const CellKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct CellKeyHash final {
    std::size_t operator()(const CellKey& key) const {
        const std::size_t hx = std::hash<int>{}(key.x);
        const std::size_t hy = std::hash<int>{}(key.y);
        const std::size_t hz = std::hash<int>{}(key.z);
        return hx ^ (hy + 0x9E3779B9U + (hx << 6U) + (hx >> 2U)) ^
               (hz + 0x85EBCA6BU + (hy << 5U) + (hy >> 3U));
    }
};

struct PotentialPair final {
    std::size_t bodyIndexA{0};
    std::size_t bodyIndexB{0};
};

PhysicsSystem::CollisionPair makeCollisionPair(const EntityId entityA, const EntityId entityB) {
    if (entityA < entityB) {
        return PhysicsSystem::CollisionPair{entityA, entityB};
    }
    return PhysicsSystem::CollisionPair{entityB, entityA};
}

CollisionContact canonicalizeContact(CollisionContact contact) {
    if (contact.entityA <= contact.entityB) {
        return contact;
    }

    std::swap(contact.entityA, contact.entityB);
    contact.normal *= -1.0F;
    return contact;
}

float effectiveInverseMass(const Rigidbody* rigidbody) {
    if (rigidbody == nullptr || rigidbody->isStatic || rigidbody->isKinematic || !std::isfinite(rigidbody->inverseMass) ||
        rigidbody->inverseMass <= 0.0F) {
        return 0.0F;
    }
    return rigidbody->inverseMass;
}

float sanitizeScalar(const float value, const float fallback) {
    return std::isfinite(value) ? value : fallback;
}

glm::vec3 sanitizeVector(const glm::vec3& value) {
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
        return glm::vec3(0.0F);
    }
    return value;
}

void wakeRigidBody(Rigidbody* rigidbody) {
    if (rigidbody != nullptr) {
        rigidbody->wakeUp();
    }
}

void sanitizeRigidBody(Rigidbody& rigidbody) {
    if (!std::isfinite(rigidbody.mass) || rigidbody.mass <= 0.0F) {
        rigidbody.mass = 1.0F;
    }

    rigidbody.linearDamping = std::max(0.0F, sanitizeScalar(rigidbody.linearDamping, 0.08F));
    rigidbody.angularDamping = std::max(0.0F, sanitizeScalar(rigidbody.angularDamping, 0.6F));
    rigidbody.restitution = std::clamp(sanitizeScalar(rigidbody.restitution, 0.06F), 0.0F, 1.0F);
    rigidbody.friction = std::max(0.0F, sanitizeScalar(rigidbody.friction, 0.7F));
    rigidbody.sleepTimer = std::max(0.0F, sanitizeScalar(rigidbody.sleepTimer, 0.0F));
    rigidbody.velocity = sanitizeVector(rigidbody.velocity);
    rigidbody.acceleration = sanitizeVector(rigidbody.acceleration);
    rigidbody.accumulatedForce = sanitizeVector(rigidbody.accumulatedForce);
    rigidbody.angularVelocity = sanitizeVector(rigidbody.angularVelocity);
    rigidbody.accumulatedTorque = sanitizeVector(rigidbody.accumulatedTorque);
    rigidbody.recalculateMassProperties();
}

glm::mat3 rotationMatrixFromEuler(const glm::vec3& rotationEulerRadians) {
    glm::mat4 rotation(1.0F);
    rotation = glm::rotate(rotation, rotationEulerRadians.x, glm::vec3(1.0F, 0.0F, 0.0F));
    rotation = glm::rotate(rotation, rotationEulerRadians.y, glm::vec3(0.0F, 1.0F, 0.0F));
    rotation = glm::rotate(rotation, rotationEulerRadians.z, glm::vec3(0.0F, 0.0F, 1.0F));
    return glm::mat3(rotation);
}

glm::vec3 localHalfExtentsForBody(const Transform& transform, const Collider* collider) {
    const glm::vec3 scale = glm::max(glm::abs(transform.scale), glm::vec3(0.05F));
    if (collider == nullptr) {
        return scale * 0.5F;
    }

    switch (collider->type) {
        case ColliderType::Aabb:
            return glm::max(scale * collider->aabb.halfExtents, glm::vec3(0.05F));

        case ColliderType::Sphere: {
            const float radiusScale = std::max(scale.x, std::max(scale.y, scale.z));
            const float radius = std::max(0.05F, collider->sphere.radius * radiusScale);
            return glm::vec3(radius);
        }
    }

    return scale * 0.5F;
}

glm::mat3 worldInverseInertiaTensor(const Transform& transform, const Collider* collider, const Rigidbody& rigidbody) {
    if (!rigidbody.isDynamic()) {
        return glm::mat3(0.0F);
    }

    glm::vec3 inverseInertiaLocal(0.0F);
    switch (collider != nullptr ? collider->type : ColliderType::Aabb) {
        case ColliderType::Aabb: {
            const glm::vec3 halfExtents = localHalfExtentsForBody(transform, collider);
            const glm::vec3 size = halfExtents * 2.0F;
            const float x2 = size.x * size.x;
            const float y2 = size.y * size.y;
            const float z2 = size.z * size.z;
            const float mass = rigidbody.mass;
            const float inertiaX = mass * (y2 + z2) / 12.0F;
            const float inertiaY = mass * (x2 + z2) / 12.0F;
            const float inertiaZ = mass * (x2 + y2) / 12.0F;
            inverseInertiaLocal.x = inertiaX > 1e-6F ? 1.0F / inertiaX : 0.0F;
            inverseInertiaLocal.y = inertiaY > 1e-6F ? 1.0F / inertiaY : 0.0F;
            inverseInertiaLocal.z = inertiaZ > 1e-6F ? 1.0F / inertiaZ : 0.0F;
            break;
        }

        case ColliderType::Sphere: {
            const float radius = localHalfExtentsForBody(transform, collider).x;
            const float inertia = 0.4F * rigidbody.mass * radius * radius;
            const float inverseInertia = inertia > 1e-6F ? 1.0F / inertia : 0.0F;
            inverseInertiaLocal = glm::vec3(inverseInertia);
            break;
        }
    }

    const glm::mat3 rotation = rotationMatrixFromEuler(transform.rotationEulerRadians);
    const glm::mat3 localInverseInertia(
        inverseInertiaLocal.x,
        0.0F,
        0.0F,
        0.0F,
        inverseInertiaLocal.y,
        0.0F,
        0.0F,
        0.0F,
        inverseInertiaLocal.z);
    return rotation * localInverseInertia * glm::transpose(rotation);
}

void updateSleepState(Rigidbody& rigidbody, const PhysicsSystem::Settings& settings, const float dtSeconds) {
    if (!rigidbody.isDynamic() || !rigidbody.canSleep) {
        rigidbody.isSleeping = false;
        rigidbody.sleepTimer = 0.0F;
        return;
    }

    const float linearSpeedSq = glm::dot(rigidbody.velocity, rigidbody.velocity);
    const float angularSpeedSq = glm::dot(rigidbody.angularVelocity, rigidbody.angularVelocity);
    const float linearThresholdSq = settings.sleepLinearSpeedThreshold * settings.sleepLinearSpeedThreshold;
    const float angularThresholdSq = settings.sleepAngularSpeedThreshold * settings.sleepAngularSpeedThreshold;

    if (linearSpeedSq <= linearThresholdSq && angularSpeedSq <= angularThresholdSq) {
        rigidbody.sleepTimer += dtSeconds;
        if (rigidbody.sleepTimer >= settings.sleepTimeThreshold) {
            rigidbody.isSleeping = true;
            rigidbody.velocity = glm::vec3(0.0F);
            rigidbody.angularVelocity = glm::vec3(0.0F);
        }
        return;
    }

    rigidbody.isSleeping = false;
    rigidbody.sleepTimer = 0.0F;
}

void integrateBody(
    Transform& transform,
    Rigidbody& rigidbody,
    const Collider* collider,
    const PhysicsSystem::Settings& settings,
    const float dtSeconds) {
    sanitizeRigidBody(rigidbody);

    if (rigidbody.isStatic || rigidbody.isKinematic || rigidbody.inverseMass <= 0.0F) {
        rigidbody.accumulatedForce = glm::vec3(0.0F);
        rigidbody.accumulatedTorque = glm::vec3(0.0F);
        rigidbody.velocity = glm::vec3(0.0F);
        rigidbody.angularVelocity = glm::vec3(0.0F);
        rigidbody.isSleeping = false;
        rigidbody.sleepTimer = 0.0F;
        return;
    }

    const bool hasPendingForce = glm::dot(rigidbody.accumulatedForce, rigidbody.accumulatedForce) > 1e-8F;
    const bool hasPendingTorque = glm::dot(rigidbody.accumulatedTorque, rigidbody.accumulatedTorque) > 1e-8F;
    if (rigidbody.isSleeping && !hasPendingForce && !hasPendingTorque) {
        rigidbody.accumulatedForce = glm::vec3(0.0F);
        rigidbody.accumulatedTorque = glm::vec3(0.0F);
        rigidbody.velocity = glm::vec3(0.0F);
        rigidbody.angularVelocity = glm::vec3(0.0F);
        return;
    }

    if (hasPendingForce || hasPendingTorque) {
        rigidbody.wakeUp();
    }

    glm::vec3 totalAcceleration = rigidbody.acceleration;
    if (rigidbody.useGravity) {
        totalAcceleration += settings.gravity;
    }
    totalAcceleration += rigidbody.accumulatedForce * rigidbody.inverseMass;

    rigidbody.velocity += totalAcceleration * dtSeconds;
    rigidbody.velocity *= 1.0F / (1.0F + rigidbody.linearDamping * dtSeconds);

    const float speedSq = glm::dot(rigidbody.velocity, rigidbody.velocity);
    const float maxSpeedSq = settings.maxLinearSpeed * settings.maxLinearSpeed;
    if (speedSq > maxSpeedSq && speedSq > 0.0F) {
        rigidbody.velocity = glm::normalize(rigidbody.velocity) * settings.maxLinearSpeed;
    }

    const glm::mat3 inverseInertia = worldInverseInertiaTensor(transform, collider, rigidbody);
    rigidbody.angularVelocity += inverseInertia * rigidbody.accumulatedTorque * dtSeconds;
    rigidbody.angularVelocity *= 1.0F / (1.0F + rigidbody.angularDamping * dtSeconds);

    const float angularSpeedSq = glm::dot(rigidbody.angularVelocity, rigidbody.angularVelocity);
    const float maxAngularSpeedSq = settings.maxAngularSpeed * settings.maxAngularSpeed;
    if (angularSpeedSq > maxAngularSpeedSq && angularSpeedSq > 0.0F) {
        rigidbody.angularVelocity = glm::normalize(rigidbody.angularVelocity) * settings.maxAngularSpeed;
    }

    transform.position += rigidbody.velocity * dtSeconds;
    transform.rotationEulerRadians += rigidbody.angularVelocity * dtSeconds;
    transform.rotationEulerRadians.x = std::remainder(transform.rotationEulerRadians.x, glm::two_pi<float>());
    transform.rotationEulerRadians.y = std::remainder(transform.rotationEulerRadians.y, glm::two_pi<float>());
    transform.rotationEulerRadians.z = std::remainder(transform.rotationEulerRadians.z, glm::two_pi<float>());

    rigidbody.accumulatedForce = glm::vec3(0.0F);
    rigidbody.accumulatedTorque = glm::vec3(0.0F);
}

glm::vec3 velocityAtPoint(const BodyProxy& body, const glm::vec3& worldPoint) {
    if (body.transform == nullptr || body.rigidbody == nullptr) {
        return glm::vec3(0.0F);
    }
    const glm::vec3 offset = worldPoint - body.transform->position;
    return body.rigidbody->velocity + glm::cross(body.rigidbody->angularVelocity, offset);
}

void applyImpulse(
    BodyProxy& body,
    const glm::vec3& impulse,
    const glm::vec3& contactOffset,
    const float inverseMass,
    const glm::mat3& inverseInertiaWorld) {
    if (body.rigidbody == nullptr || body.transform == nullptr || inverseMass <= 0.0F) {
        return;
    }

    body.rigidbody->velocity += impulse * inverseMass;
    body.rigidbody->angularVelocity += inverseInertiaWorld * glm::cross(contactOffset, impulse);
    if (glm::dot(impulse, impulse) > 1e-8F) {
        wakeRigidBody(body.rigidbody);
    }
}

void resolveCollision(BodyProxy& bodyA, BodyProxy& bodyB, const CollisionContact& contact) {
    constexpr float kPenetrationSlop = 0.01F;
    constexpr float kCorrectionPercent = 0.34F;
    constexpr float kMaxCorrectionDepth = 0.35F;
    constexpr float kBounceSpeedThreshold = 1.15F;

    const float inverseMassA = effectiveInverseMass(bodyA.rigidbody);
    const float inverseMassB = effectiveInverseMass(bodyB.rigidbody);
    const float inverseMassSum = inverseMassA + inverseMassB;

    if (inverseMassSum > 0.0F) {
        const float correctionDepth =
            std::min(std::max(contact.penetrationDepth - kPenetrationSlop, 0.0F), kMaxCorrectionDepth);
        if (correctionDepth > 0.0F) {
            const glm::vec3 correction = contact.normal * (correctionDepth * kCorrectionPercent);
            if (bodyA.transform != nullptr && inverseMassA > 0.0F) {
                bodyA.transform->position -= correction * (inverseMassA / inverseMassSum);
            }
            if (bodyB.transform != nullptr && inverseMassB > 0.0F) {
                bodyB.transform->position += correction * (inverseMassB / inverseMassSum);
            }
        }
    }

    if (inverseMassSum <= 0.0F || bodyA.transform == nullptr || bodyB.transform == nullptr) {
        return;
    }

    const glm::vec3 offsetA = contact.contactPoint - bodyA.transform->position;
    const glm::vec3 offsetB = contact.contactPoint - bodyB.transform->position;
    const glm::mat3 inverseInertiaA =
        bodyA.rigidbody != nullptr ? worldInverseInertiaTensor(*bodyA.transform, bodyA.collider, *bodyA.rigidbody) : glm::mat3(0.0F);
    const glm::mat3 inverseInertiaB =
        bodyB.rigidbody != nullptr ? worldInverseInertiaTensor(*bodyB.transform, bodyB.collider, *bodyB.rigidbody) : glm::mat3(0.0F);

    const glm::vec3 relativeVelocity = velocityAtPoint(bodyB, contact.contactPoint) - velocityAtPoint(bodyA, contact.contactPoint);
    const float closingSpeed = glm::dot(relativeVelocity, contact.normal);
    if (closingSpeed >= 0.0F) {
        return;
    }

    const glm::vec3 angularFactorA = glm::cross(inverseInertiaA * glm::cross(offsetA, contact.normal), offsetA);
    const glm::vec3 angularFactorB = glm::cross(inverseInertiaB * glm::cross(offsetB, contact.normal), offsetB);
    const float normalDenominator = inverseMassSum + glm::dot(angularFactorA + angularFactorB, contact.normal);
    if (normalDenominator <= 1e-6F) {
        return;
    }

    const float restitutionA = bodyA.rigidbody != nullptr ? bodyA.rigidbody->restitution : 0.0F;
    const float restitutionB = bodyB.rigidbody != nullptr ? bodyB.rigidbody->restitution : 0.0F;
    float restitution = 0.5F * (restitutionA + restitutionB);
    if (-closingSpeed < kBounceSpeedThreshold) {
        restitution = 0.0F;
    }
    const float normalImpulseMagnitude = -(1.0F + restitution) * closingSpeed / normalDenominator;
    const glm::vec3 normalImpulse = contact.normal * normalImpulseMagnitude;

    applyImpulse(bodyA, -normalImpulse, offsetA, inverseMassA, inverseInertiaA);
    applyImpulse(bodyB, normalImpulse, offsetB, inverseMassB, inverseInertiaB);

    const glm::vec3 postNormalRelativeVelocity =
        velocityAtPoint(bodyB, contact.contactPoint) - velocityAtPoint(bodyA, contact.contactPoint);
    const glm::vec3 tangentVelocity =
        postNormalRelativeVelocity - glm::dot(postNormalRelativeVelocity, contact.normal) * contact.normal;
    if (glm::dot(tangentVelocity, tangentVelocity) <= 1e-8F) {
        return;
    }

    const glm::vec3 tangent = glm::normalize(tangentVelocity);
    const glm::vec3 tangentAngularFactorA = glm::cross(inverseInertiaA * glm::cross(offsetA, tangent), offsetA);
    const glm::vec3 tangentAngularFactorB = glm::cross(inverseInertiaB * glm::cross(offsetB, tangent), offsetB);
    const float tangentDenominator = inverseMassSum + glm::dot(tangentAngularFactorA + tangentAngularFactorB, tangent);
    if (tangentDenominator <= 1e-6F) {
        return;
    }

    const float frictionA = bodyA.rigidbody != nullptr ? bodyA.rigidbody->friction : 0.0F;
    const float frictionB = bodyB.rigidbody != nullptr ? bodyB.rigidbody->friction : 0.0F;
    const float friction = std::sqrt(std::max(0.0F, frictionA) * std::max(0.0F, frictionB));
    float tangentImpulseMagnitude = -glm::dot(postNormalRelativeVelocity, tangent) / tangentDenominator;
    const float maxFrictionImpulse = normalImpulseMagnitude * friction;
    tangentImpulseMagnitude = std::clamp(tangentImpulseMagnitude, -maxFrictionImpulse, maxFrictionImpulse);

    const glm::vec3 tangentImpulse = tangent * tangentImpulseMagnitude;
    applyImpulse(bodyA, -tangentImpulse, offsetA, inverseMassA, inverseInertiaA);
    applyImpulse(bodyB, tangentImpulse, offsetB, inverseMassB, inverseInertiaB);
}

std::vector<PotentialPair> buildBroadphasePairs(const std::vector<BodyProxy>& bodies, const float cellSize) {
    std::vector<PotentialPair> pairs;
    if (bodies.size() < 2U) {
        return pairs;
    }

    if (!std::isfinite(cellSize) || cellSize <= 0.0F) {
        pairs.reserve((bodies.size() * (bodies.size() - 1U)) / 2U);
        for (std::size_t bodyIndexA = 0; bodyIndexA < bodies.size(); ++bodyIndexA) {
            for (std::size_t bodyIndexB = bodyIndexA + 1U; bodyIndexB < bodies.size(); ++bodyIndexB) {
                if (effectiveInverseMass(bodies[bodyIndexA].rigidbody) <= 0.0F &&
                    effectiveInverseMass(bodies[bodyIndexB].rigidbody) <= 0.0F) {
                    continue;
                }
                pairs.push_back(PotentialPair{bodyIndexA, bodyIndexB});
            }
        }
        return pairs;
    }

    std::unordered_map<CellKey, std::vector<std::size_t>, CellKeyHash> cells;
    cells.reserve(bodies.size() * 2U);

    for (std::size_t bodyIndex = 0; bodyIndex < bodies.size(); ++bodyIndex) {
        const WorldAabb& bounds = bodies[bodyIndex].worldBounds;
        const glm::vec3 minCellFloat = glm::floor(bounds.min / cellSize);
        const glm::vec3 maxCellFloat = glm::floor(bounds.max / cellSize);
        const glm::ivec3 minCell(
            static_cast<int>(minCellFloat.x),
            static_cast<int>(minCellFloat.y),
            static_cast<int>(minCellFloat.z));
        const glm::ivec3 maxCell(
            static_cast<int>(maxCellFloat.x),
            static_cast<int>(maxCellFloat.y),
            static_cast<int>(maxCellFloat.z));

        for (int x = minCell.x; x <= maxCell.x; ++x) {
            for (int y = minCell.y; y <= maxCell.y; ++y) {
                for (int z = minCell.z; z <= maxCell.z; ++z) {
                    cells[CellKey{x, y, z}].push_back(bodyIndex);
                }
            }
        }
    }

    std::unordered_set<std::uint64_t> visitedPairs;
    visitedPairs.reserve(bodies.size() * 4U);
    pairs.reserve(bodies.size() * 3U);

    for (const auto& [cell, bodyIndices] : cells) {
        (void)cell;
        for (std::size_t i = 0; i < bodyIndices.size(); ++i) {
            for (std::size_t j = i + 1U; j < bodyIndices.size(); ++j) {
                const std::size_t bodyIndexA = bodyIndices[i];
                const std::size_t bodyIndexB = bodyIndices[j];
                if (effectiveInverseMass(bodies[bodyIndexA].rigidbody) <= 0.0F &&
                    effectiveInverseMass(bodies[bodyIndexB].rigidbody) <= 0.0F) {
                    continue;
                }

                const std::uint32_t minIndex = static_cast<std::uint32_t>(std::min(bodyIndexA, bodyIndexB));
                const std::uint32_t maxIndex = static_cast<std::uint32_t>(std::max(bodyIndexA, bodyIndexB));
                const std::uint64_t pairKey =
                    (static_cast<std::uint64_t>(minIndex) << 32U) | static_cast<std::uint64_t>(maxIndex);
                if (!visitedPairs.insert(pairKey).second) {
                    continue;
                }

                pairs.push_back(PotentialPair{bodyIndexA, bodyIndexB});
            }
        }
    }

    return pairs;
}

} // namespace

PhysicsSystem::PhysicsSystem(Settings settings) : m_settings(std::move(settings)) {}

void PhysicsSystem::setSettings(const Settings& settings) {
    m_settings = settings;
}

const PhysicsSystem::Settings& PhysicsSystem::settings() const {
    return m_settings;
}

std::size_t PhysicsSystem::activeCollisionCount() const {
    return m_activeContacts.size();
}

std::size_t PhysicsSystem::bodyCount() const {
    return m_lastBodyCount;
}

std::size_t PhysicsSystem::broadphasePairCount() const {
    return m_lastBroadphasePairCount;
}

std::size_t PhysicsSystem::sleepingBodyCount() const {
    return m_lastSleepingBodyCount;
}

void PhysicsSystem::clear() {
    m_activeContacts.clear();
    m_lastBodyCount = 0;
    m_lastBroadphasePairCount = 0;
    m_lastSleepingBodyCount = 0;
}

std::size_t PhysicsSystem::CollisionPairHash::operator()(const CollisionPair& pair) const {
    const std::size_t first = std::hash<EntityId>{}(pair.first);
    const std::size_t second = std::hash<EntityId>{}(pair.second);
    return first ^ (second + 0x9E3779B9U + (first << 6U) + (first >> 2U));
}

void PhysicsSystem::update(World& world, const double dt, core::EventBus& eventBus) {
    if (dt <= 0.0) {
        return;
    }

    const float dtSeconds = static_cast<float>(dt);

    world.forEach<Transform, Rigidbody>([&](EntityId entity, Transform& transform, Rigidbody& rigidbody) {
        integrateBody(transform, rigidbody, world.getComponent<Collider>(entity), m_settings, dtSeconds);
    });

    std::vector<BodyProxy> bodies;
    world.forEach<Transform, Collider>([&](EntityId entity, Transform& transform, Collider& collider) {
        if (!collider.enabled) {
            return;
        }

        BodyProxy proxy{};
        proxy.entity = entity;
        proxy.transform = &transform;
        proxy.rigidbody = world.getComponent<Rigidbody>(entity);
        proxy.collider = &collider;
        if (!computeWorldColliderShape(world, entity, collider, proxy.worldShape)) {
            return;
        }

        if (proxy.rigidbody != nullptr) {
            sanitizeRigidBody(*proxy.rigidbody);
        }
        proxy.worldBounds = computeWorldBounds(proxy.worldShape);
        bodies.push_back(proxy);
    });

    m_lastBodyCount = bodies.size();
    const std::vector<PotentialPair> potentialPairs = buildBroadphasePairs(bodies, m_settings.broadphaseCellSize);
    m_lastBroadphasePairCount = potentialPairs.size();

    std::unordered_map<CollisionPair, CollisionContact, CollisionPairHash> frameContacts;
    const std::size_t iterations = std::max<std::size_t>(1, m_settings.solverIterations);

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        for (const PotentialPair& potentialPair : potentialPairs) {
            BodyProxy& bodyA = bodies[potentialPair.bodyIndexA];
            BodyProxy& bodyB = bodies[potentialPair.bodyIndexB];

            const bool bodyASleeping = bodyA.rigidbody != nullptr && bodyA.rigidbody->isSleeping;
            const bool bodyBSleeping = bodyB.rigidbody != nullptr && bodyB.rigidbody->isSleeping;
            if (iteration > 0U && bodyASleeping && bodyBSleeping) {
                continue;
            }

            CollisionContact contact{};
            if (!intersectColliders(bodyA.entity, bodyA.worldShape, bodyB.entity, bodyB.worldShape, contact)) {
                continue;
            }

            if (iteration == 0U) {
                const CollisionContact canonicalContact = canonicalizeContact(contact);
                frameContacts[makeCollisionPair(canonicalContact.entityA, canonicalContact.entityB)] = canonicalContact;
            }

            resolveCollision(bodyA, bodyB, contact);
            computeWorldColliderShape(world, bodyA.entity, *bodyA.collider, bodyA.worldShape);
            computeWorldColliderShape(world, bodyB.entity, *bodyB.collider, bodyB.worldShape);
            bodyA.worldBounds = computeWorldBounds(bodyA.worldShape);
            bodyB.worldBounds = computeWorldBounds(bodyB.worldShape);
        }
    }

    m_lastSleepingBodyCount = 0;
    world.forEach<Rigidbody>([&](EntityId entity, Rigidbody& rigidbody) {
        (void)entity;
        updateSleepState(rigidbody, m_settings, dtSeconds);
        if (rigidbody.isSleeping) {
            ++m_lastSleepingBodyCount;
        }
    });

    for (const auto& [pair, contact] : frameContacts) {
        const auto previousIt = m_activeContacts.find(pair);
        if (previousIt == m_activeContacts.end()) {
            eventBus.publish(CollisionEnterEvent{contact});
        } else {
            eventBus.publish(CollisionStayEvent{contact});
        }
    }

    for (const auto& [pair, contact] : m_activeContacts) {
        if (frameContacts.find(pair) == frameContacts.end()) {
            eventBus.publish(CollisionExitEvent{contact});
        }
    }

    m_activeContacts = std::move(frameContacts);
}

} // namespace engine::ecs
