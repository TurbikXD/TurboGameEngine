#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <entt/entity/entity.hpp>

#include "engine/core/JobSystem.h"
#include "engine/ecs/components.h"
#include "engine/ecs/render_interpolation.h"
#include "engine/ecs/transform_batch.h"
#include "engine/ecs/frustum.h"
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include "engine/ecs/transform_utils.h"
#include "engine/ecs/world.h"

namespace {
bool sameMatrix(const glm::mat4& actual, const glm::mat4& expected) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(actual[column][row]) || std::abs(actual[column][row] - expected[column][row]) > 1e-5F) return false;
        }
    }
    return true;
}

bool renderPoses(engine::core::JobSystem& jobs) {
    using namespace engine::ecs;
    World world;
    const auto parent = world.createEntity();
    const auto child = world.createEntity();
    auto& parentPose = world.addComponent<Transform>(parent);
    auto& childPose = world.addComponent<Transform>(child);
    auto& camera = world.addComponent<Camera>(child, Camera{1.0F});
    world.addComponent<Hierarchy>(child, Hierarchy{parent});
    childPose.rotationEulerRadians.y = glm::radians(179.0F);
    world.addComponent<RenderPoseHistory>(parent, captureRenderPose(world, parent, parentPose));
    auto& childHistory = world.addComponent<RenderPoseHistory>(child, captureRenderPose(world, child, childPose));

    parentPose.position.x = 12.0F;
    childPose.position.z = 4.0F;
    childPose.rotationEulerRadians.y = glm::radians(-179.0F);
    childPose.scale.x = 2.0F;
    camera.verticalFovRadians = 2.0F;
    Transform expectedParent, expectedChild;
    expectedParent.position.x = 6.0F;
    expectedChild.position.z = 2.0F;
    expectedChild.rotationEulerRadians.y = glm::radians(180.0F);
    expectedChild.scale.x = 1.5F;
    const auto interpolated = expectedParent.toMatrix() * expectedChild.toMatrix();
    const std::vector<EntityId> entities(1024U, child);
    std::vector<glm::mat4> matrices(entities.size());
    prepareWorldMatrices(world, entities, matrices, &jobs, 0.5F);
    for (const auto& matrix : matrices) {
        if (!sameMatrix(matrix, interpolated)) {
            std::cerr << "Render batch lost interpolation through the parent or the short rotation arc\n";
            return false;
        }
    }
    if (!sameMatrix(computeWorldMatrix(world, child), parentPose.toMatrix() * childPose.toMatrix()) ||
        childPose.position.z != 4.0F || camera.verticalFovRadians != 2.0F ||
        sampleRenderCameraFov(world, child, camera.verticalFovRadians, 0.5F) != 1.5F) {
        std::cerr << "Render interpolation changed simulation poses or failed to blend FOV\n";
        return false;
    }

    childPose.position.z = 100.0F;
    childHistory = captureRenderPose(world, child, childPose);
    const auto snapped = computeWorldMatrix(world, child, 0.25F);
    if (std::abs(snapped[3].z - 100.0F) > 1e-5F ||
        sampleRenderCameraFov(world, child, camera.verticalFovRadians, 0.25F) != 2.0F) {
        std::cerr << "Snapped render pose blended across a deliberate teleport\n";
        return false;
    }
    return true;
}

bool hierarchyCyclesAndDepth(engine::core::JobSystem& jobs) {
    using namespace engine::ecs;
    World world;
    const auto self = world.createEntity();
    Transform local;
    local.position = {3, 4, 5};
    local.scale = {2, 2, 2};
    world.addComponent<Transform>(self, local);
    world.addComponent<Hierarchy>(self, Hierarchy{self});
    if (!sameMatrix(computeWorldMatrix(world, self), local.toMatrix()) ||
        !sameMatrix(computeParentWorldMatrix(world, self), glm::mat4(1.0F)) ||
        isHierarchyParentValid(world, self, self)) {
        std::cerr << "Self-parent applied its local transform more than once\n";
        return false;
    }

    const auto first = world.createEntity();
    const auto second = world.createEntity();
    const auto child = world.createEntity();
    Transform firstTransform, secondTransform;
    firstTransform.position = {2, 0, 0};
    secondTransform.rotationEulerRadians.z = 0.5F;
    world.addComponent<Transform>(first, firstTransform);
    world.addComponent<Transform>(second, secondTransform);
    world.addComponent<Transform>(child, local);
    world.addComponent<Hierarchy>(first, Hierarchy{second});
    world.addComponent<Hierarchy>(second, Hierarchy{first});
    world.addComponent<Hierarchy>(child, Hierarchy{first});
    if (!sameMatrix(computeWorldMatrix(world, first), secondTransform.toMatrix() * firstTransform.toMatrix()) ||
        !sameMatrix(computeWorldMatrix(world, child), secondTransform.toMatrix() * firstTransform.toMatrix() * local.toMatrix()) ||
        isHierarchyParentValid(world, child, first) || isHierarchyParentValid(world, second, first)) {
        std::cerr << "Cyclic hierarchy repeatedly multiplied an ancestor transform\n";
        return false;
    }

    World deep;
    EntityId leaf = kInvalidEntity;
    EntityId root = kInvalidEntity;
    for (int depth = 0; depth < 192; ++depth) {
        const auto entity = deep.createEntity();
        Transform transform;
        transform.position.x = 1.0F;
        deep.addComponent<Transform>(entity, transform);
        deep.addComponent<Hierarchy>(entity, Hierarchy{leaf});
        if (depth == 0) root = entity;
        leaf = entity;
    }
    Transform expected;
    expected.position.x = 192.0F;
    if (!sameMatrix(computeWorldMatrix(deep, leaf), expected.toMatrix())) {
        std::cerr << "Valid hierarchy lost ancestors beyond the old 64-parent limit\n";
        return false;
    }
    if (isHierarchyParentValid(deep, root, leaf) || isHierarchyParentValid(deep, root, root) ||
        !isHierarchyParentValid(deep, leaf, root) || !isHierarchyParentValid(deep, leaf, kInvalidEntity)) {
        std::cerr << "Parent validation accepted a distant descendant/self or rejected a valid parent\n";
        return false;
    }
    std::vector<EntityId> entities(1024U, leaf);
    std::vector<glm::mat4> serial(entities.size()), parallel(entities.size());
    prepareWorldMatrices(deep, entities, serial, nullptr);
    prepareWorldMatrices(deep, entities, parallel, &jobs);
    for (std::size_t index = 0; index < entities.size(); ++index) {
        if (!sameMatrix(serial[index], expected.toMatrix()) || !sameMatrix(parallel[index], expected.toMatrix())) {
            std::cerr << "Serial/parallel preparation truncated the deep hierarchy\n";
            return false;
        }
    }
    const auto outsider = deep.createEntity();
    deep.getComponent<Hierarchy>(root)->parent = leaf;
    if (isHierarchyParentValid(deep, outsider, leaf)) {
        std::cerr << "Parent validation admitted an existing ancestor cycle beyond64levels\n";
        return false;
    }
    return true;
}

bool parentBases() {
    using namespace engine::ecs;
    World world;
    const auto root = world.createEntity();
    const auto parent = world.createEntity();
    const auto gap = world.createEntity();
    const auto child = world.createEntity();
    Transform rootTransform, parentTransform, local;
    rootTransform.position = {5, 2, 1};
    rootTransform.rotationEulerRadians.y = 0.3F;
    rootTransform.scale = {2, 3, 4};
    parentTransform.position = {1, 2, -3};
    parentTransform.rotationEulerRadians.x = -0.4F;
    parentTransform.scale = {3, 1, 2};
    local.position = {4, -1, 2};
    world.addComponent<Transform>(root, rootTransform);
    world.addComponent<Transform>(parent, parentTransform);
    world.addComponent<Transform>(child, local);
    world.addComponent<Hierarchy>(parent, Hierarchy{root});
    world.addComponent<Hierarchy>(gap, Hierarchy{parent});
    world.addComponent<Hierarchy>(child, Hierarchy{gap});
    const auto expected = rootTransform.toMatrix() * parentTransform.toMatrix();
    Transform rootRotation, parentRotation;
    rootRotation.rotationEulerRadians = rootTransform.rotationEulerRadians;
    parentRotation.rotationEulerRadians = parentTransform.rotationEulerRadians;
    const auto expectedRotation = rootRotation.toMatrix() * parentRotation.toMatrix();
    if (!sameMatrix(computeParentWorldMatrix(world, child), expected) ||
        !sameMatrix(computeWorldMatrix(world, child), expected * local.toMatrix()) ||
        !sameMatrix(glm::mat4(computeParentWorldRotation(world, child)), expectedRotation)) {
        std::cerr << "Parent bases lost an ancestor through a node without Transform or included scale in rotation\n";
        return false;
    }
    world.getComponent<Transform>(root)->scale.x = 0.0F;
    if (!sameMatrix(computeParentWorldMatrix(world, child), world.getComponent<Transform>(root)->toMatrix() * parentTransform.toMatrix())) {
        std::cerr << "Parent basis silently changed a singular authored transform\n";
        return false;
    }
    world.destroyEntity(parent);
    if (!sameMatrix(computeParentWorldMatrix(world, child), glm::mat4(1.0F)) ||
        !sameMatrix(glm::mat4(computeParentWorldRotation(world, child)), glm::mat4(1.0F)) ||
        isHierarchyParentValid(world, child, parent)) {
        std::cerr << "Parent basis or validation followed a dead parent handle\n";
        return false;
    }
    return true;
}
}

int main() {
    const auto projection = glm::perspectiveRH_ZO(glm::radians(60.0F), 16.0F/9.0F, 0.1F, 100.0F);
    const auto boxVisible = [&](const glm::vec3& center, const glm::vec3& halfExtent = glm::vec3(0.5F)) {
        return engine::ecs::intersectsFrustum(projection, center-halfExtent, center+halfExtent);
    };
    if (!boxVisible({0,0,-3}) || boxVisible({0,0,3}) || boxVisible({100,0,-3}) || boxVisible({0,0,-200}) ||
        !boxVisible({0,0,0}, {2,2,2}) || !boxVisible({0,0,-110}, {30,30,30})) {
        std::cerr << "Conservative frustum culling failed\n"; return EXIT_FAILURE;
    }
    engine::ecs::World world;
    engine::core::JobSystem jobs(true, 3U);
    if (!hierarchyCyclesAndDepth(jobs) || !parentBases() || !renderPoses(jobs)) return EXIT_FAILURE;
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
    flatIds.push_back(flatWorld.createEntity());
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
