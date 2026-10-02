#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

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
    engine::ecs::prepareWorldMatrices(world, {}, {}, &jobs);
    try {
        engine::ecs::prepareWorldMatrices(world, ids, {}, &jobs);
        return EXIT_FAILURE;
    } catch (const std::invalid_argument&) {}
    std::cout << "Transform batches: 4096 hierarchy matrices match serial/reference; bounds checks passed\n";
    return EXIT_SUCCESS;
}
