#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/EventBus.h"
#include "engine/core/JobSystem.h"
#include "engine/ecs/components.h"
#include "engine/ecs/physics_system.h"
#include "engine/ecs/transform_utils.h"
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

bool runContainedAabbTest() {
    using namespace engine;
    ecs::WorldColliderShape outer, inner;
    outer.aabb = {glm::vec3(-5), glm::vec3(5)};
    inner.aabb = {glm::vec3(-0.5F), glm::vec3(0.5F)};
    ecs::CollisionContact contact;
    if (!expect(ecs::intersectColliders(1, outer, 2, inner, contact) &&
        std::abs(contact.penetrationDepth - 5.5F) < 1e-6F,
        "Contained AABB penetration was measured as overlap length")) return false;
    inner.aabb.min.x += 1;
    inner.aabb.max.x += 1;
    ecs::CollisionContact reverse;
    if (!expect(ecs::intersectColliders(1, outer, 2, inner, contact) &&
        ecs::intersectColliders(2, inner, 1, outer, reverse) &&
        std::abs(contact.penetrationDepth - 4.5F) < 1e-6F &&
        contact.penetrationDepth == reverse.penetrationDepth && contact.normal == -reverse.normal,
        "Contained AABB contact lost the nearest separating face or pair symmetry")) return false;
    ecs::World world;
    core::EventBus events;
    ecs::PhysicsSystem physics;
    auto settings = physics.settings();
    settings.gravity = glm::vec3(0);
    settings.solverIterations = 1;
    physics.setSettings(settings);
    spawnBox(world, glm::vec3(0), glm::vec3(10), true, 1);
    const auto enclosed = spawnBox(world, glm::vec3(0), glm::vec3(0.1F), false, 1);
    physics.update(world, 1.0 / 60.0, events);
    return expect(std::abs(glm::length(world.getComponent<ecs::Transform>(enclosed)->position) - 0.119F) < 1e-5F,
        "Contained AABB solver used the small object's width as correction depth");
}

bool runInvalidTimestepPreservationTest() {
    using namespace engine;
    ecs::World world;
    core::EventBus events;
    ecs::PhysicsSystem physics;
    auto settings = physics.settings();
    settings.gravity = glm::vec3(0);
    physics.setSettings(settings);
    spawnBox(world, glm::vec3(0), glm::vec3(2), true, 1);
    const auto entity = spawnBox(world, glm::vec3(0.9F, 0, 0), glm::vec3(1), false, 1);
    int enters = 0, stays = 0, exits = 0;
    events.subscribe<ecs::CollisionEnterEvent>([&](const auto&) { ++enters; });
    events.subscribe<ecs::CollisionStayEvent>([&](const auto&) { ++stays; });
    events.subscribe<ecs::CollisionExitEvent>([&](const auto&) { ++exits; });
    physics.update(world, 1.0 / 60.0, events);
    auto& transform = *world.getComponent<ecs::Transform>(entity);
    auto& body = *world.getComponent<ecs::Rigidbody>(entity);
    const auto position = transform.position;
    const auto rotation = transform.rotationEulerRadians;
    body.velocity = {1, 2, 3};
    body.angularVelocity = {2, 1, 3};
    body.accumulatedForce = {4, 5, 6};
    body.accumulatedTorque = {3, 2, 1};
    body.sleepTimer = 0.25F;
    const auto previousBodies = physics.bodyCount();
    const auto previousPairs = physics.broadphasePairCount();
    const auto previousContacts = physics.activeCollisionCount();
    const std::array timesteps{0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min()};
    for (const double dt : timesteps) {
        physics.update(world, dt, events);
        if (!expect(transform.position == position && transform.rotationEulerRadians == rotation &&
            body.velocity == glm::vec3(1, 2, 3) && body.angularVelocity == glm::vec3(2, 1, 3) &&
            body.accumulatedForce == glm::vec3(4, 5, 6) && body.accumulatedTorque == glm::vec3(3, 2, 1) &&
            body.sleepTimer == 0.25F && enters == 1 && stays == 0 && exits == 0 &&
            physics.bodyCount() == previousBodies && physics.broadphasePairCount() == previousPairs &&
            physics.activeCollisionCount() == previousContacts,
            "Invalid timestep mutated physics state, cleared forces or dispatched contacts")) return false;
    }
    return true;
}

bool runColliderNumericBoundariesTest() {
    using namespace engine;
    ecs::WorldColliderShape first, second;
    first.type = second.type = ecs::ColliderType::Sphere;
    first.sphere = {glm::vec3(0), 1e20F};
    second.sphere = {glm::vec3(0, 1e20F, 0), 1e20F};
    ecs::CollisionContact contact;
    if (!expect(ecs::intersectColliders(1, first, 2, second, contact) &&
        glm::length(contact.normal - glm::vec3(0, 1, 0)) < 1e-6F &&
        std::abs(contact.penetrationDepth / 1e20F - 1) < 1e-5F,
        "Large finite spheres lost contact through squared-distance overflow")) return false;
    first.sphere.radius = second.sphere.radius = 1e-10F;
    second.sphere.center = {0, 1e-10F, 0};
    if (!expect(ecs::intersectColliders(1, first, 2, second, contact) &&
        glm::length(contact.normal - glm::vec3(0, 1, 0)) < 1e-6F,
        "Small sphere contacts used a fixed-axis normal instead of their separation")) return false;
    first.type = ecs::ColliderType::Aabb;
    first.aabb = {glm::vec3(-1e20F), glm::vec3(1e20F)};
    second.sphere = {glm::vec3(1.5e20F, 0, 0), 1e20F};
    if (!expect(ecs::intersectColliders(1, first, 2, second, contact) &&
        glm::length(contact.normal - glm::vec3(1, 0, 0)) < 1e-6F &&
        std::abs(contact.penetrationDepth / 1e20F - 0.5F) < 1e-5F,
        "Large finite AABB/sphere contact produced invalid depth or normal")) return false;
    first.aabb = {glm::vec3(-1), glm::vec3(1)};
    second.sphere = {glm::vec3(1.0001F, 1.0001F, 0), 0.1F};
    if (!expect(ecs::intersectColliders(1, first, 2, second, contact) &&
        glm::length(contact.normal - glm::normalize(glm::vec3(1, 1, 0))) < 1e-5F,
        "Near-corner sphere contact was mistaken for a sphere inside the AABB")) return false;
    contact.penetrationDepth = 123;
    first.aabb.min.x = std::numeric_limits<float>::quiet_NaN();
    if (!expect(!ecs::intersectColliders(1, first, 2, second, contact) && contact.penetrationDepth == 123,
        "Invalid collider produced a contact or overwrote the previous destination")) return false;
    ecs::World world;
    const auto entity = spawnSphere(world, glm::vec3(0), 1e20F, true, 1);
    ecs::WorldColliderShape transformed;
    if (!expect(ecs::computeWorldColliderShape(world, entity, *world.getComponent<ecs::Collider>(entity), transformed) &&
        std::isfinite(transformed.sphere.radius) && std::abs(transformed.sphere.radius / 1e20F - 0.5F) < 1e-5F,
        "Finite large sphere scale overflowed during world-shape construction")) return false;
    return true;
}

bool runInvalidColliderIsolationTest() {
    using namespace engine;
    ecs::World world;
    core::EventBus events;
    ecs::PhysicsSystem physics;
    auto settings = physics.settings();
    settings.gravity = glm::vec3(0);
    physics.setSettings(settings);
    const auto invalid = spawnBox(world, glm::vec3(0), glm::vec3(1), true, 1);
    world.getComponent<ecs::Transform>(invalid)->position.x = std::numeric_limits<float>::quiet_NaN();
    const auto healthy = spawnBox(world, glm::vec3(0), glm::vec3(1), false, 1);
    auto& body = *world.getComponent<ecs::Rigidbody>(healthy);
    body.velocity = {1, 2, 3};
    body.linearDamping = 0;
    body.canSleep = false;
    physics.update(world, 0.01, events);
    return expect(physics.bodyCount() == 1 && physics.activeCollisionCount() == 0 &&
        glm::length(world.getComponent<ecs::Transform>(healthy)->position - glm::vec3(0.01F, 0.02F, 0.03F)) < 1e-6F &&
        body.velocity == glm::vec3(1, 2, 3),
        "An invalid collider contaminated a healthy body's position or velocity");
}

bool runBroadphaseExtremeBoundsTest() {
    struct Fixture final {
        float cellSize;
        glm::vec3 position;
        glm::vec3 staticScale;
        glm::vec3 dynamicScale;
    };
    const std::array fixtures{
        Fixture{2.6F, glm::vec3(0.0F), glm::vec3(1000000.0F), glm::vec3(1.0F)},
        Fixture{2.6F, glm::vec3(0.0F), glm::vec3(1000000.0F), glm::vec3(1000000.0F)},
        Fixture{std::numeric_limits<float>::denorm_min(), glm::vec3(0.0F), glm::vec3(1.0F), glm::vec3(1.0F)},
        Fixture{1.0F, glm::vec3(10000000000.0F, 0.0F, 0.0F), glm::vec3(8192.0F), glm::vec3(8192.0F)},
        Fixture{1.0F, glm::vec3(-10000000000.0F, 0.0F, 0.0F), glm::vec3(8192.0F), glm::vec3(8192.0F)},
    };
    for (const auto& fixture : fixtures) {
        engine::ecs::World world;
        engine::core::EventBus events;
        engine::ecs::PhysicsSystem physics;
        auto settings = physics.settings();
        settings.gravity = glm::vec3(0.0F);
        settings.broadphaseCellSize = fixture.cellSize;
        settings.solverIterations = 1;
        physics.setSettings(settings);
        int entered = 0, stayed = 0, exited = 0;
        events.subscribe<engine::ecs::CollisionEnterEvent>([&](const auto&) { ++entered; });
        events.subscribe<engine::ecs::CollisionStayEvent>([&](const auto&) { ++stayed; });
        events.subscribe<engine::ecs::CollisionExitEvent>([&](const auto&) { ++exited; });
        spawnBox(world, fixture.position, fixture.staticScale, true, 1.0F);
        const auto dynamic = spawnBox(world, fixture.position, fixture.dynamicScale, false, 1.0F);
        spawnBox(world, fixture.position + glm::vec3(0.0F, 2000000.0F, 0.0F), glm::vec3(1.0F), true, 1.0F);
        physics.update(world, 1.0 / 60.0, events);
        if (!expect(entered == 1 && physics.activeCollisionCount() == 1U,
                    "Extreme broadphase bounds lost or duplicated an overlapping collision")) { return false; }
        if (!expect(physics.broadphasePairCount() <= 2U,
                    "Extreme broadphase bounds duplicated a candidate pair")) { return false; }
        physics.update(world, 1.0 / 60.0, events);
        if (!expect(stayed == 1 && entered == 1,
                    "Extreme broadphase bounds did not preserve collision stay")) { return false; }
        world.getComponent<engine::ecs::Transform>(dynamic)->position.y = 4000000.0F;
        physics.update(world, 1.0 / 60.0, events);
        if (!expect(exited == 1 && physics.activeCollisionCount() == 0U,
                    "Extreme broadphase bounds did not emit collision exit")) { return false; }
    }
    return true;
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

bool runCollisionCallbackCleanupTest() {
    for (const bool onEnter : {true, false}) {
        engine::ecs::World world;
        engine::core::EventBus events;
        engine::ecs::PhysicsSystem physics;
        auto settings = physics.settings();
        settings.gravity = glm::vec3(0.0F);
        settings.solverIterations = 1;
        physics.setSettings(settings);
        spawnBox(world, glm::vec3(0.0F), glm::vec3(1.0F), true, 1.0F);
        const auto dynamic = spawnBox(world, glm::vec3(0.0F), glm::vec3(1.0F), false, 1.0F);
        int entered = 0, stayed = 0, exited = 0;
        const auto remove = [&]() {
            physics.forgetEntity(dynamic, events);
            world.destroyEntity(dynamic);
        };
        events.subscribe<engine::ecs::CollisionEnterEvent>([&](const auto&) {
            ++entered;
            if (onEnter) { remove(); }
        });
        events.subscribe<engine::ecs::CollisionStayEvent>([&](const auto&) {
            ++stayed;
            if (!onEnter) { remove(); }
        });
        events.subscribe<engine::ecs::CollisionExitEvent>([&](const auto&) { ++exited; });
        physics.update(world, 1.0 / 60.0, events);
        if (!onEnter) { physics.update(world, 1.0 / 60.0, events); }
        if (!expect(entered == 1 && stayed == (onEnter ? 0 : 1) && exited == 1 &&
                    !world.isAlive(dynamic) && physics.activeCollisionCount() == 0U,
                    "Collision callback removal was lost or did not emit exactly one exit")) { return false; }
        physics.update(world, 1.0 / 60.0, events);
        if (!expect(exited == 1, "Collision callback removal emitted a duplicate exit on the next update")) { return false; }
    }
    engine::ecs::World world;
    engine::core::EventBus events;
    engine::ecs::PhysicsSystem physics;
    auto settings = physics.settings();
    settings.gravity = glm::vec3(0.0F);
    physics.setSettings(settings);
    std::array<engine::ecs::EntityId, 2> dynamic{};
    for (std::size_t index = 0; index < dynamic.size(); ++index) {
        const glm::vec3 position(static_cast<float>(index) * 10.0F, 0.0F, 0.0F);
        spawnBox(world, position, glm::vec3(1.0F), true, 1.0F);
        dynamic[index] = spawnBox(world, position, glm::vec3(1.0F), false, 1.0F);
    }
    physics.update(world, 1.0 / 60.0, events);
    if (!expect(physics.activeCollisionCount() == 2U, "Collision exit cleanup fixture lacks two contacts")) { return false; }
    int exited = 0;
    events.subscribe<engine::ecs::CollisionExitEvent>([&](const auto&) { ++exited; physics.clear(); });
    for (const auto entity : dynamic) { world.getComponent<engine::ecs::Transform>(entity)->position.y = 5.0F; }
    physics.update(world, 1.0 / 60.0, events);
    return expect(exited == 2 && physics.activeCollisionCount() == 0U,
                  "Clearing physics in an exit callback invalidated collision dispatch");
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

bool runCollisionListenerMutationTest() {
    for (const bool clearDuringPublish : {false, true}) {
        engine::ecs::World world;
        engine::core::EventBus events;
        engine::ecs::PhysicsSystem physics;
        auto settings = physics.settings();
        settings.gravity = glm::vec3(0.0F);
        settings.solverIterations = 1;
        physics.setSettings(settings);
        spawnBox(world, glm::vec3(0.0F), glm::vec3(1.0F), true, 1.0F);
        spawnBox(world, glm::vec3(0.0F), glm::vec3(1.0F), false, 1.0F);
        int first = 0, second = 0, added = 0, mutableTotal = 0;
        events.subscribe<engine::ecs::CollisionEnterEvent>([&](const auto&) {
            ++first;
            if (first != 1) { return; }
            if (clearDuringPublish) { events.clear(); }
            for (int index = 0; index < 64; ++index) {
                events.subscribe<engine::ecs::CollisionEnterEvent>([&](const auto&) { ++added; });
            }
        });
        events.subscribe<engine::ecs::CollisionEnterEvent>([&](const auto&) { ++second; });
        events.subscribe<engine::ecs::CollisionEnterEvent>([&, count = 0](const auto&) mutable {
            mutableTotal += ++count;
        });
        physics.update(world, 1.0 / 60.0, events);
        if (!expect(first == 1 && second == 1 && added == 0 && mutableTotal == 1,
                    "Collision subscription or clear invalidated the current listener snapshot")) { return false; }
        physics.clear();
        physics.update(world, 1.0 / 60.0, events);
        if (!expect(added == 64 && first == (clearDuringPublish ? 1 : 2) &&
                    second == (clearDuringPublish ? 1 : 2) && mutableTotal == (clearDuringPublish ? 1 : 3),
                    "Collision listener changes did not start at the next publish or lost mutable handler state")) { return false; }
    }
    return true;
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

bool runParentedWorldSpaceResponseTest() {
    using namespace engine;
    struct Fixture { glm::vec3 rotation, scale; };
    const std::array fixtures{
        Fixture{glm::vec3(0), glm::vec3(1)},
        Fixture{glm::vec3(0, 0, glm::radians(90.0F)), glm::vec3(1)},
        Fixture{glm::vec3(0.2F, -0.3F, 0.4F), glm::vec3(0.5F, 2, 1.5F)},
        Fixture{glm::vec3(0), glm::vec3(100000, 1, 1)},
    };
    core::JobSystem jobs(true, 3);
    for (const auto& fixture : fixtures) {
        ecs::World reference, parented;
        const auto referenceParent = reference.createEntity();
        const auto parent = parented.createEntity();
        reference.addComponent<ecs::Transform>(referenceParent);
        ecs::Transform parentTransform;
        parentTransform.position = {23, -7, 5};
        parentTransform.rotationEulerRadians = fixture.rotation;
        parentTransform.scale = fixture.scale;
        parented.addComponent<ecs::Transform>(parent, parentTransform);
        const auto inverse = glm::inverse(glm::dmat4(parentTransform.toMatrix()));
        const float localDiameter = 1.0F / std::max({fixture.scale.x, fixture.scale.y, fixture.scale.z});
        std::vector<ecs::EntityId> referenceIds, parentedIds;
        for (int index = 0; index < 80; ++index) {
            const glm::vec3 origin((index % 10) * 4.0F, (index / 10) * 4.0F, 0);
            spawnBox(reference, origin + glm::vec3(0, -0.5F, 0), {2, 1, 2}, true, 1);
            spawnBox(parented, origin + glm::vec3(0, -0.5F, 0), {2, 1, 2}, true, 1);
            const auto worldPosition = origin + glm::vec3(0, 0.45F, 0);
            const auto first = spawnSphere(reference, worldPosition, 1, false, 1);
            const auto second = spawnSphere(parented, glm::vec3(inverse * glm::dvec4(worldPosition, 1)), localDiameter, false, 1);
            parented.addComponent<ecs::Hierarchy>(second, ecs::Hierarchy{parent});
            for (auto* body : {reference.getComponent<ecs::Rigidbody>(first), parented.getComponent<ecs::Rigidbody>(second)}) {
                body->velocity = {0, -2, 0}; body->linearDamping = 0; body->angularDamping = 0;
                body->friction = 0; body->restitution = 0; body->canSleep = false;
            }
            referenceIds.push_back(first); parentedIds.push_back(second);
        }
        core::EventBus firstEvents, secondEvents;
        ecs::PhysicsSystem firstPhysics, secondPhysics;
        secondPhysics.setJobSystem(&jobs);
        auto settings = firstPhysics.settings();
        settings.gravity = {0, -2, 0};
        firstPhysics.setSettings(settings); secondPhysics.setSettings(settings);
        for (int step = 0; step < 12; ++step) {
            firstPhysics.update(reference, 1.0 / 60.0, firstEvents);
            secondPhysics.update(parented, 1.0 / 60.0, secondEvents);
            if (!expect(firstPhysics.activeCollisionCount() == secondPhysics.activeCollisionCount() &&
                firstPhysics.activeCollisionCount() == referenceIds.size(), "Parented contact count differs in world space")) return false;
            for (std::size_t index = 0; index < referenceIds.size(); ++index) {
                const auto firstPosition = glm::vec3(ecs::computeWorldMatrix(reference, referenceIds[index])[3]);
                const auto secondPosition = glm::vec3(ecs::computeWorldMatrix(parented, parentedIds[index])[3]);
                const auto& firstBody = *reference.getComponent<ecs::Rigidbody>(referenceIds[index]);
                const auto& secondBody = *parented.getComponent<ecs::Rigidbody>(parentedIds[index]);
                if (!expect(glm::length(firstPosition - secondPosition) < 0.001F &&
                    glm::length(firstBody.velocity - secondBody.velocity) < 0.001F &&
                    glm::length(firstBody.angularVelocity - secondBody.angularVelocity) < 0.001F,
                    "Parent translation/rotation/scale changed world-space gravity or contact response")) return false;
            }
        }
    }
    return expect(jobs.stats().submitted > 0, "Parented world-space fixture did not execute physics jobs");
}

bool runMovingParentPoseSnapshotTest() {
    using namespace engine;
    ecs::World world;
    core::EventBus events;
    core::JobSystem jobs(true, 3);
    ecs::PhysicsSystem physics;
    physics.setJobSystem(&jobs);
    auto settings = physics.settings(); settings.gravity = {0, -2, 0}; physics.setSettings(settings);
    const auto parent = world.createEntity();
    ecs::Transform parentTransform; parentTransform.position = {3, 4, 5}; parentTransform.scale = {0.5F, 2, 1.5F};
    world.addComponent<ecs::Transform>(parent, parentTransform);
    ecs::Rigidbody parentBody; parentBody.velocity = {2, 1, -1}; parentBody.angularVelocity = {0, 0, 0.3F};
    parentBody.useGravity = false; parentBody.linearDamping = 0; parentBody.angularDamping = 0; parentBody.canSleep = false;
    world.addComponent<ecs::Rigidbody>(parent, parentBody);
    std::vector<ecs::EntityId> children;
    std::vector<glm::vec3> initial;
    for (int index = 0; index < 96; ++index) {
        const auto child = world.createEntity();
        ecs::Transform transform; transform.position = {static_cast<float>(index), 3, 2};
        transform.rotationEulerRadians = {0.1F, 0.2F, -0.3F};
        world.addComponent<ecs::Transform>(child, transform);
        ecs::Rigidbody body; body.velocity = {1, 0, 0}; body.linearDamping = 0; body.angularDamping = 0; body.canSleep = false;
        world.addComponent<ecs::Rigidbody>(child, body);
        world.addComponent<ecs::Hierarchy>(child, ecs::Hierarchy{parent});
        children.push_back(child); initial.push_back(glm::vec3(ecs::computeWorldMatrix(world, child)[3]));
    }
    constexpr double dt = 1.0 / 60.0;
    physics.update(world, dt, events);
    for (std::size_t index = 0; index < children.size(); ++index) {
        const auto actual = glm::vec3(ecs::computeWorldMatrix(world, children[index])[3]);
        const auto expected = initial[index] + glm::vec3(static_cast<float>(dt), static_cast<float>(-2 * dt * dt), 0);
        if (!expect(glm::length(actual - expected) < 0.0001F,
            "Moving parent was double-applied to an independently integrated child")) return false;
        const auto rotation = ecs::computeParentWorldRotation(world, children[index]) *
            glm::mat3(ecs::Transform{{}, world.getComponent<ecs::Transform>(children[index])->rotationEulerRadians}.toMatrix());
        const auto original = glm::mat3(ecs::Transform{{}, glm::vec3(0.1F, 0.2F, -0.3F)}.toMatrix());
        for (int axis = 0; axis < 3; ++axis) if (!expect(glm::length(rotation[axis] - original[axis]) < 1e-5F,
            "Moving parent changed a child's absolute physics orientation")) return false;
    }
    world.getComponent<ecs::Transform>(parent)->scale.x = 0;
    const auto previous = world.getComponent<ecs::Transform>(children.front())->position;
    world.getComponent<ecs::Rigidbody>(children.front())->accumulatedForce = {1, 2, 3};
    physics.update(world, dt, events);
    return expect(world.getComponent<ecs::Transform>(children.front())->position == previous &&
        world.getComponent<ecs::Rigidbody>(children.front())->accumulatedForce == glm::vec3(1, 2, 3),
        "Singular parent corrupted its child's local pose or consumed pending forces");
}

}

int main() {
    if (!runFloorCollisionTest()) {
        return EXIT_FAILURE;
    }
    if (!runBroadphaseExtremeBoundsTest()) {
        return EXIT_FAILURE;
    }
    if (!runContainedAabbTest() || !runInvalidTimestepPreservationTest() ||
        !runColliderNumericBoundariesTest() || !runInvalidColliderIsolationTest()) {
        return EXIT_FAILURE;
    }
    if (!runDynamicMassResolutionTest()) {
        return EXIT_FAILURE;
    }
    if (!runCollisionCallbackCleanupTest()) {
        return EXIT_FAILURE;
    }
    if (!runAngularCrashResponseTest()) {
        return EXIT_FAILURE;
    }
    if (!runCollisionListenerMutationTest()) {
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
    if (!runParentedWorldSpaceResponseTest() || !runMovingParentPoseSnapshotTest()) return EXIT_FAILURE;

    std::cout << "PhysicsSystem tests passed\n";
    return EXIT_SUCCESS;
}
