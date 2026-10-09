#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/EventBus.h"
#include "engine/core/JobSystem.h"
#include "engine/ecs/components.h"
#include "engine/ecs/physics_system.h"
#include "engine/ecs/world.h"

namespace {

bool expect(const bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "Test failure: " << message << '\n';
    return false;
}

engine::ecs::EntityId spawnBox(
    engine::ecs::World& world,
    const glm::vec3& position,
    const glm::vec3& scale,
    const bool isStatic,
    const float mass) {
    const engine::ecs::EntityId entity = world.createEntity();

    engine::ecs::Transform transform{};
    transform.position = position;
    transform.scale = scale;

    engine::ecs::Collider collider{};
    collider.type = engine::ecs::ColliderType::Aabb;
    collider.aabb.halfExtents = glm::vec3(0.5F);

    engine::ecs::Rigidbody rigidbody{};
    rigidbody.isStatic = isStatic;
    rigidbody.mass = mass;
    rigidbody.recalculateMassProperties();

    world.addComponent<engine::ecs::Transform>(entity, transform);
    world.addComponent<engine::ecs::Collider>(entity, collider);
    world.addComponent<engine::ecs::Rigidbody>(entity, rigidbody);
    return entity;
}

engine::ecs::EntityId spawnSphere(
    engine::ecs::World& world,
    const glm::vec3& position,
    const float diameter,
    const bool isStatic,
    const float mass) {
    const engine::ecs::EntityId entity = world.createEntity();

    engine::ecs::Transform transform{};
    transform.position = position;
    transform.scale = glm::vec3(diameter);

    engine::ecs::Collider collider{};
    collider.type = engine::ecs::ColliderType::Sphere;
    collider.sphere.radius = 0.5F;

    engine::ecs::Rigidbody rigidbody{};
    rigidbody.isStatic = isStatic;
    rigidbody.mass = mass;
    rigidbody.friction = 0.92F;
    rigidbody.recalculateMassProperties();

    world.addComponent<engine::ecs::Transform>(entity, transform);
    world.addComponent<engine::ecs::Collider>(entity, collider);
    world.addComponent<engine::ecs::Rigidbody>(entity, rigidbody);
    return entity;
}

bool runFloorCollisionTest() {
    engine::ecs::World world;
    engine::core::EventBus eventBus;
    engine::ecs::PhysicsSystem physics;

    int enterCount = 0;
    int exitCount = 0;
    eventBus.subscribe<engine::ecs::CollisionEnterEvent>(
        [&enterCount](const engine::ecs::CollisionEnterEvent&) { ++enterCount; });
    eventBus.subscribe<engine::ecs::CollisionExitEvent>(
        [&exitCount](const engine::ecs::CollisionExitEvent&) { ++exitCount; });

    const auto floor = spawnBox(world, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(8.0F, 1.0F, 8.0F), true, 1.0F);
    const auto dynamicBox = spawnBox(world, glm::vec3(0.0F, 3.0F, 0.0F), glm::vec3(1.0F, 1.0F, 1.0F), false, 1.0F);
    (void)floor;

    for (int step = 0; step < 240; ++step) {
        physics.update(world, 1.0 / 60.0, eventBus);
    }

    const auto* boxTransform = world.getComponent<engine::ecs::Transform>(dynamicBox);
    const auto* boxBody = world.getComponent<engine::ecs::Rigidbody>(dynamicBox);
    if (!expect(boxTransform != nullptr, "dynamic box transform missing")) {
        return false;
    }
    if (!expect(boxBody != nullptr, "dynamic box rigidbody missing")) {
        return false;
    }
    if (!expect(std::abs(boxTransform->position.y - 0.5F) < 0.15F, "dynamic box did not settle on the floor")) {
        return false;
    }
    if (!expect(std::abs(boxBody->velocity.y) < 0.1F, "dynamic box retained inward floor velocity")) {
        return false;
    }
    if (!expect(enterCount > 0, "collision enter event was not emitted")) {
        return false;
    }

    world.getComponent<engine::ecs::Transform>(dynamicBox)->position.y = 4.0F;
    world.getComponent<engine::ecs::Rigidbody>(dynamicBox)->velocity = glm::vec3(0.0F);
    physics.update(world, 1.0 / 60.0, eventBus);
    return expect(exitCount > 0, "collision exit event was not emitted");
}

bool runDynamicMassResolutionTest() {
    engine::ecs::World world;
    engine::core::EventBus eventBus;
    engine::ecs::PhysicsSystem physics;

    auto settings = physics.settings();
    settings.gravity = glm::vec3(0.0F);
    settings.solverIterations = 1;
    physics.setSettings(settings);

    const auto lightBox = spawnBox(world, glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F), false, 1.0F);
    const auto heavyBox = spawnBox(world, glm::vec3(0.4F, 0.0F, 0.0F), glm::vec3(1.0F), false, 3.0F);

    physics.update(world, 1.0 / 60.0, eventBus);

    const auto* lightTransform = world.getComponent<engine::ecs::Transform>(lightBox);
    const auto* heavyTransform = world.getComponent<engine::ecs::Transform>(heavyBox);
    if (!expect(lightTransform != nullptr && heavyTransform != nullptr, "dynamic box transform missing")) {
        return false;
    }

    const float lightDisplacement = std::abs(lightTransform->position.x - 0.0F);
    const float heavyDisplacement = std::abs(heavyTransform->position.x - 0.4F);
    if (!expect(lightTransform->position.x < 0.0F, "light body should move left during separation")) {
        return false;
    }
    if (!expect(heavyTransform->position.x > 0.4F, "heavy body should move right during separation")) {
        return false;
    }
    return expect(lightDisplacement > heavyDisplacement, "lighter body should move more than heavier body");
}

bool runAngularCrashResponseTest() {
    engine::ecs::World world;
    engine::core::EventBus eventBus;
    engine::ecs::PhysicsSystem physics;

    auto settings = physics.settings();
    settings.gravity = glm::vec3(0.0F);
    settings.solverIterations = 4;
    physics.setSettings(settings);

    const auto striker = spawnBox(world, glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(1.0F), false, 1.0F);
    const auto target = spawnBox(world, glm::vec3(0.85F, 0.45F, 0.0F), glm::vec3(1.0F), false, 1.4F);

    auto* strikerBody = world.getComponent<engine::ecs::Rigidbody>(striker);
    auto* targetBody = world.getComponent<engine::ecs::Rigidbody>(target);
    if (!expect(strikerBody != nullptr && targetBody != nullptr, "rigidbody missing for angular crash test")) {
        return false;
    }

    strikerBody->velocity = glm::vec3(5.0F, 0.0F, 0.0F);
    targetBody->velocity = glm::vec3(0.0F);

    physics.update(world, 1.0 / 60.0, eventBus);

    if (!expect(std::abs(strikerBody->angularVelocity.z) > 0.01F, "offset impact should spin striker")) {
        return false;
    }
    return expect(std::abs(targetBody->angularVelocity.z) > 0.01F, "offset impact should spin target");
}

bool runSphereRollingTest() {
    engine::ecs::World world;
    engine::core::EventBus eventBus;
    engine::ecs::PhysicsSystem physics;

    auto settings = physics.settings();
    settings.solverIterations = 4;
    physics.setSettings(settings);

    const auto floor = spawnBox(world, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(14.0F, 1.0F, 14.0F), true, 1.0F);
    const auto sphere = spawnSphere(world, glm::vec3(-2.0F, 0.52F, 0.0F), 1.0F, false, 1.2F);
    (void)floor;

    auto* sphereBody = world.getComponent<engine::ecs::Rigidbody>(sphere);
    auto* sphereTransform = world.getComponent<engine::ecs::Transform>(sphere);
    if (!expect(sphereBody != nullptr && sphereTransform != nullptr, "sphere test components missing")) {
        return false;
    }

    sphereBody->velocity = glm::vec3(4.0F, 0.0F, 0.0F);

    for (int step = 0; step < 180; ++step) {
        physics.update(world, 1.0 / 60.0, eventBus);
    }

    if (!expect(sphereTransform->position.x > 1.0F, "sphere did not travel across the floor")) {
        return false;
    }
    if (!expect(std::abs(sphereTransform->position.y - 0.5F) < 0.12F, "sphere did not stay supported by the floor")) {
        return false;
    }
    return expect(std::abs(sphereBody->angularVelocity.z) > 0.2F, "sphere should gain spin while rolling");
}

bool runParallelEquivalenceTest() {
    engine::ecs::World serialWorld;
    engine::ecs::World parallelWorld;
    std::vector<engine::ecs::EntityId> serialEntities;
    std::vector<engine::ecs::EntityId> parallelEntities;
    serialEntities.reserve(256U);
    parallelEntities.reserve(256U);

    for (int row = 0; row < 16; ++row) {
        for (int column = 0; column < 16; ++column) {
            const glm::vec3 position(
                static_cast<float>(column) * 3.0F,
                5.0F + static_cast<float>((row + column) % 3),
                static_cast<float>(row) * 3.0F);
            serialEntities.push_back(spawnBox(serialWorld, position, glm::vec3(0.5F), false, 1.0F));
            parallelEntities.push_back(spawnBox(parallelWorld, position, glm::vec3(0.5F), false, 1.0F));
        }
    }

    engine::core::EventBus serialEvents;
    engine::core::EventBus parallelEvents;
    engine::ecs::PhysicsSystem serialPhysics;
    engine::ecs::PhysicsSystem parallelPhysics;
    engine::core::JobSystem jobs(true, 3U);
    parallelPhysics.setJobSystem(&jobs);

    serialPhysics.update(serialWorld, 1.0 / 60.0, serialEvents);
    parallelPhysics.update(parallelWorld, 1.0 / 60.0, parallelEvents);

    for (std::size_t index = 0; index < serialEntities.size(); ++index) {
        const auto* serialTransform = serialWorld.getComponent<engine::ecs::Transform>(serialEntities[index]);
        const auto* parallelTransform = parallelWorld.getComponent<engine::ecs::Transform>(parallelEntities[index]);
        const auto* serialBody = serialWorld.getComponent<engine::ecs::Rigidbody>(serialEntities[index]);
        const auto* parallelBody = parallelWorld.getComponent<engine::ecs::Rigidbody>(parallelEntities[index]);
        if (!expect(serialTransform != nullptr && parallelTransform != nullptr, "parallel transform is missing") ||
            !expect(serialBody != nullptr && parallelBody != nullptr, "parallel rigidbody is missing")) {
            return false;
        }
        if (!expect(
                glm::length(serialTransform->position - parallelTransform->position) < 1e-6F,
                "parallel integration changed a transform") ||
            !expect(
                glm::length(serialBody->velocity - parallelBody->velocity) < 1e-6F,
                "parallel integration changed a velocity")) {
            return false;
        }
    }

    return expect(serialPhysics.bodyCount() == parallelPhysics.bodyCount(), "parallel proxy count differs");
}

bool runParallelCollisionHierarchyAndRecycleTest() {
    using namespace engine;
    ecs::World serialWorld;
    ecs::World parallelWorld;
    const auto serialParent = serialWorld.createEntity();
    const auto parallelParent = parallelWorld.createEntity();
    ecs::Transform parentTransform;
    parentTransform.position = glm::vec3(10.0F, -4.0F, 2.0F);
    parentTransform.rotationEulerRadians = glm::vec3(0.0F, 0.1F, 0.0F);
    serialWorld.addComponent<ecs::Transform>(serialParent, parentTransform);
    parallelWorld.addComponent<ecs::Transform>(parallelParent, parentTransform);
    std::vector<ecs::EntityId> serialIds;
    std::vector<ecs::EntityId> parallelIds;
    for (int pair = 0; pair < 128; ++pair) {
        const glm::vec3 position(static_cast<float>(pair % 16) * 4.0F,
                                 static_cast<float>(pair / 16) * 4.0F, 0.0F);
        for (int member = 0; member < 2; ++member) {
            const auto local = position + glm::vec3(static_cast<float>(member) * 0.7F, 0.0F, 0.0F);
            const auto serial = spawnBox(serialWorld, local, glm::vec3(1.0F), false, 1.0F + static_cast<float>(member));
            const auto parallel = spawnBox(parallelWorld, local, glm::vec3(1.0F), false, 1.0F + static_cast<float>(member));
            serialWorld.addComponent<ecs::Hierarchy>(serial, ecs::Hierarchy{serialParent});
            parallelWorld.addComponent<ecs::Hierarchy>(parallel, ecs::Hierarchy{parallelParent});
            const auto velocity = glm::vec3(member == 0 ? 0.2F : -0.1F, 0.0F, 0.0F);
            serialWorld.getComponent<ecs::Rigidbody>(serial)->velocity = velocity;
            parallelWorld.getComponent<ecs::Rigidbody>(parallel)->velocity = velocity;
            serialIds.push_back(serial);
            parallelIds.push_back(parallel);
        }
    }
    core::EventBus serialEvents;
    core::EventBus parallelEvents;
    std::array<std::size_t, 3> serialCounts{};
    std::array<std::size_t, 3> parallelCounts{};
    serialEvents.subscribe<ecs::CollisionEnterEvent>([&](const auto&) { ++serialCounts[0]; });
    serialEvents.subscribe<ecs::CollisionStayEvent>([&](const auto&) { ++serialCounts[1]; });
    serialEvents.subscribe<ecs::CollisionExitEvent>([&](const auto&) { ++serialCounts[2]; });
    parallelEvents.subscribe<ecs::CollisionEnterEvent>([&](const auto&) { ++parallelCounts[0]; });
    parallelEvents.subscribe<ecs::CollisionStayEvent>([&](const auto&) { ++parallelCounts[1]; });
    parallelEvents.subscribe<ecs::CollisionExitEvent>([&](const auto&) { ++parallelCounts[2]; });
    ecs::PhysicsSystem serialPhysics;
    ecs::PhysicsSystem parallelPhysics;
    auto settings = serialPhysics.settings();
    settings.gravity = glm::vec3(0.0F);
    serialPhysics.setSettings(settings);
    parallelPhysics.setSettings(settings);
    core::JobSystem jobs(true, 3U);
    parallelPhysics.setJobSystem(&jobs);

    const auto compare = [&]() {
        if (!expect(serialPhysics.bodyCount() == parallelPhysics.bodyCount() &&
                    serialPhysics.activeCollisionCount() == parallelPhysics.activeCollisionCount() &&
                    serialPhysics.broadphasePairCount() == parallelPhysics.broadphasePairCount() &&
                    serialCounts == parallelCounts, "parallel collision counts/events differ")) {
            return false;
        }
        for (std::size_t index = 0; index < serialIds.size(); ++index) {
            const auto* serialTransform = serialWorld.getComponent<ecs::Transform>(serialIds[index]);
            const auto* parallelTransform = parallelWorld.getComponent<ecs::Transform>(parallelIds[index]);
            const auto* serialBody = serialWorld.getComponent<ecs::Rigidbody>(serialIds[index]);
            const auto* parallelBody = parallelWorld.getComponent<ecs::Rigidbody>(parallelIds[index]);
            if (!expect((serialTransform != nullptr) == (parallelTransform != nullptr) &&
                        (serialBody != nullptr) == (parallelBody != nullptr), "parallel sparse components differ")) {
                return false;
            }
            if (serialTransform != nullptr && !expect(
                    glm::length(serialTransform->position - parallelTransform->position) < 1e-5F &&
                    glm::length(serialTransform->rotationEulerRadians - parallelTransform->rotationEulerRadians) < 1e-5F,
                    "parallel hierarchy collision changed a transform")) {
                return false;
            }
            if (serialBody != nullptr && !expect(
                    glm::length(serialBody->velocity - parallelBody->velocity) < 1e-5F &&
                    glm::length(serialBody->angularVelocity - parallelBody->angularVelocity) < 1e-5F,
                    "parallel hierarchy collision changed a body velocity")) {
                return false;
            }
        }
        return true;
    };
    const auto update = [&]() {
        serialPhysics.update(serialWorld, 1.0 / 60.0, serialEvents);
        parallelPhysics.update(parallelWorld, 1.0 / 60.0, parallelEvents);
        return compare();
    };
    if (!update() || !expect(serialCounts[0] > 0U && serialPhysics.activeCollisionCount() > 0U,
                            "Hierarchy collision fixture generated no contacts") ||
        !expect(jobs.stats().submitted > 0U, "Physics fixture did not submit jobs")) {
        return false;
    }

    const auto serialDeleted = serialIds.front();
    const auto parallelDeleted = parallelIds.front();
    serialPhysics.forgetEntity(serialDeleted, serialEvents);
    parallelPhysics.forgetEntity(parallelDeleted, parallelEvents);
    serialWorld.destroyEntity(serialDeleted);
    parallelWorld.destroyEntity(parallelDeleted);
    serialIds.front() = spawnBox(serialWorld, glm::vec3(-100.0F), glm::vec3(1.0F), false, 1.0F);
    parallelIds.front() = spawnBox(parallelWorld, glm::vec3(-100.0F), glm::vec3(1.0F), false, 1.0F);
    if (!expect(serialIds.front() != serialDeleted && parallelIds.front() != parallelDeleted &&
                entt::to_entity(static_cast<entt::entity>(serialIds.front() - 1U)) ==
                entt::to_entity(static_cast<entt::entity>(serialDeleted - 1U)),
                "Physics fixture did not reuse a slot with a new generation")) {
        return false;
    }
    serialWorld.destroyEntity(serialDeleted);
    parallelWorld.destroyEntity(parallelDeleted);
    serialWorld.removeComponent<ecs::Collider>(serialDeleted);
    parallelWorld.removeComponent<ecs::Collider>(parallelDeleted);
    serialWorld.removeComponent<ecs::Collider>(serialIds[2]);
    parallelWorld.removeComponent<ecs::Collider>(parallelIds[2]);
    serialWorld.removeComponent<ecs::Rigidbody>(serialIds[3]);
    parallelWorld.removeComponent<ecs::Rigidbody>(parallelIds[3]);
    serialWorld.removeComponent<ecs::Transform>(serialIds[4]);
    parallelWorld.removeComponent<ecs::Transform>(parallelIds[4]);
    if (!update() || !expect(serialWorld.isAlive(serialIds.front()) && parallelWorld.isAlive(parallelIds.front()),
                            "Stale contact handle affected a replacement body")) {
        return false;
    }

    serialWorld.destroyEntity(serialParent);
    parallelWorld.destroyEntity(parallelParent);
    const auto newSerialParent = serialWorld.createEntity();
    const auto newParallelParent = parallelWorld.createEntity();
    ecs::Transform replacementParent;
    replacementParent.position = glm::vec3(1000.0F);
    serialWorld.addComponent<ecs::Transform>(newSerialParent, replacementParent);
    parallelWorld.addComponent<ecs::Transform>(newParallelParent, replacementParent);
    if (!expect(newSerialParent != serialParent && newParallelParent != parallelParent,
                "Recreated hierarchy parent retained its stale handle")) {
        return false;
    }
    for (int step = 0; step < 3; ++step) {
        if (!update()) { return false; }
    }
    return true;
}

} // namespace

int main() {
    if (!runFloorCollisionTest()) {
        return EXIT_FAILURE;
    }
    if (!runDynamicMassResolutionTest()) {
        return EXIT_FAILURE;
    }
    if (!runAngularCrashResponseTest()) {
        return EXIT_FAILURE;
    }
    if (!runSphereRollingTest()) {
        return EXIT_FAILURE;
    }
    if (!runParallelEquivalenceTest()) {
        return EXIT_FAILURE;
    }
    if (!runParallelCollisionHierarchyAndRecycleTest()) {
        return EXIT_FAILURE;
    }

    std::cout << "PhysicsSystem tests passed\n";
    return EXIT_SUCCESS;
}
