#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/JobSystem.h"
#include "engine/ecs/components.h"
#include "engine/ecs/transform_batch.h"
#include "engine/ecs/transform_utils.h"
#include "engine/ecs/world.h"

int main() {
    engine::ecs::World world;
    engine::core::JobSystem jobs(true, 3U);
    const auto root = world.createEntity();
    engine::ecs::Transform rootTransform;
    rootTransform.position = glm::vec3(2.0F, -3.0F, 1.0F);
    rootTransform.rotationEulerRadians = glm::vec3(0.2F, -0.6F, 0.8F);
    world.addComponent<engine::ecs::Transform>(root, rootTransform);
    std::vector<engine::ecs::EntityId> ids;
    for (std::size_t i = 0; i < 4096U; ++i) {
        const auto entity = world.createEntity();
        engine::ecs::Transform transform;
        transform.position = glm::vec3(static_cast<float>(i % 31U), static_cast<float>(i % 17U), -2.0F);
        transform.rotationEulerRadians = glm::vec3(0.1F, static_cast<float>(i) * 0.01F, 0.2F);
        transform.scale = glm::vec3(0.5F, 1.2F, 0.8F);
        world.addComponent<engine::ecs::Transform>(entity, transform);
        world.addComponent<engine::ecs::Hierarchy>(entity, engine::ecs::Hierarchy{i > 0U && i % 8U != 0U ? ids.back() : root});
        ids.push_back(entity);
    }
    std::vector<glm::mat4> serial(ids.size()), parallel(ids.size());
    engine::ecs::prepareWorldMatrices(world, ids, serial, nullptr);
    engine::ecs::prepareWorldMatrices(world, ids, parallel, &jobs);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto reference = engine::ecs::computeWorldMatrix(world, ids[i]);
        for (int col = 0; col < 4; ++col) {
            if (glm::length(serial[i][col] - parallel[i][col]) > 1e-5F ||
                glm::length(reference[col] - parallel[i][col]) > 1e-5F) {
                std::cerr << "Parallel hierarchy transform mismatch at " << i << '\n';
                return EXIT_FAILURE;
            }
        }
    }

    world.destroyEntity(root);
    const auto replacementRoot = world.createEntity();
    if (replacementRoot == root || world.isAlive(root) ||
        entt::to_entity(static_cast<entt::entity>(replacementRoot - 1U)) !=
        entt::to_entity(static_cast<entt::entity>(root - 1U))) {
        std::cerr << "Parent fixture did not reuse a slot with a new generation\n";
        return EXIT_FAILURE;
    }
    engine::ecs::Transform replacementTransform;
    replacementTransform.position = glm::vec3(1000.0F, 2000.0F, -3000.0F);
    world.addComponent<engine::ecs::Transform>(replacementRoot, replacementTransform);
    engine::ecs::prepareWorldMatrices(world, ids, serial, nullptr);
    engine::ecs::prepareWorldMatrices(world, ids, parallel, &jobs);
    const auto firstLocal = world.getComponent<engine::ecs::Transform>(ids.front())->toMatrix();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto reference = engine::ecs::computeWorldMatrix(world, ids[i]);
        for (int col = 0; col < 4; ++col) {
            if (glm::length(serial[i][col] - parallel[i][col]) > 1e-5F ||
                glm::length(reference[col] - parallel[i][col]) > 1e-5F ||
                (i == 0U && glm::length(firstLocal[col] - parallel[i][col]) > 1e-5F)) {
                std::cerr << "Stale parent inherited the replacement transform at " << i << '\n';
                return EXIT_FAILURE;
            }
        }
    }

    // Hierarchy has never been stored in this world. Worker reads must not create its pool.
    engine::ecs::World flatWorld;
    std::vector<engine::ecs::EntityId> flatIds;
    for (std::size_t i = 0; i < 2048U; ++i) {
        const auto entity = flatWorld.createEntity();
        engine::ecs::Transform transform;
        transform.position = glm::vec3(static_cast<float>(i % 23U), -2.0F, static_cast<float>(i % 7U));
        transform.rotationEulerRadians = glm::vec3(0.2F, -0.3F, 0.4F);
        flatWorld.addComponent<engine::ecs::Transform>(entity, transform);
        flatIds.push_back(entity);
    }
    const auto deleted = flatWorld.createEntity();
    flatWorld.addComponent<engine::ecs::Transform>(deleted);
    flatWorld.destroyEntity(deleted);
    flatIds.push_back(deleted);
    flatIds.push_back(engine::ecs::kInvalidEntity);
    flatIds.push_back(flatWorld.createEntity()); // Live entity without a Transform.
    std::vector<glm::mat4> flatMatrices(flatIds.size());
    const auto beforeFlat = jobs.stats().submitted;
    engine::ecs::prepareWorldMatrices(flatWorld, flatIds, flatMatrices, &jobs);
    if (jobs.stats().submitted <= beforeFlat) {
        std::cerr << "Flat transform fixture did not dispatch parallel work\n";
        return EXIT_FAILURE;
    }
    for (std::size_t i = 0; i < flatIds.size(); ++i) {
        const auto* transform = flatWorld.getComponent<engine::ecs::Transform>(flatIds[i]);
        const auto expected = transform != nullptr ? transform->toMatrix() : glm::mat4(1.0F);
        for (int col = 0; col < 4; ++col) {
            if (glm::length(expected[col] - flatMatrices[i][col]) > 1e-5F) {
                std::cerr << "Missing hierarchy pool or transform changed a flat matrix at " << i << '\n';
                return EXIT_FAILURE;
            }
        }
    }
    flatWorld.addComponent<engine::ecs::Hierarchy>(flatIds.front());
    engine::ecs::prepareWorldMatrices(world, {}, {}, &jobs);
    try {
        engine::ecs::prepareWorldMatrices(world, ids, {}, &jobs);
        return EXIT_FAILURE;
    } catch (const std::invalid_argument&) {}
    std::cout << "Transform batches: hierarchy/flat matrices, recycled parents, absent pools and bounds passed\n";
    return EXIT_SUCCESS;
}
