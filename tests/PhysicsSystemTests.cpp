#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "engine/core/EventBus.h"
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

    std::cout << "PhysicsSystem tests passed\n";
    return EXIT_SUCCESS;
}
