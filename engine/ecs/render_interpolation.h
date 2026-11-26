#pragma once

#include <cmath>
#include <optional>

#include <glm/gtc/constants.hpp>

#include "engine/ecs/components.h"
#include "engine/ecs/world.h"

namespace engine::ecs {

struct RenderPoseHistory final {
    Transform previous;
    std::optional<float> previousFov;
};

inline RenderPoseHistory captureRenderPose(const World& world, EntityId entity, const Transform& transform) {
    const auto* camera = world.getComponent<Camera>(entity);
    return {transform, camera ? std::optional<float>{camera->verticalFovRadians} : std::nullopt};
}

inline Transform sampleRenderTransform(const World& world, EntityId entity, const Transform& current, float fraction) {
    if (fraction >= 1.0F) return current;
    const auto* history = world.getComponent<RenderPoseHistory>(entity);
    if (!history) return current;
    const Transform& previous = history->previous;
    Transform sampled;
    sampled.position = glm::mix(previous.position, current.position, fraction);
    sampled.scale = glm::mix(previous.scale, current.scale, fraction);
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        const float delta = current.rotationEulerRadians[axis] - previous.rotationEulerRadians[axis];
        sampled.rotationEulerRadians[axis] = previous.rotationEulerRadians[axis] +
            std::remainder(delta, glm::two_pi<float>()) * fraction;
    }
    return sampled;
}

inline float sampleRenderCameraFov(const World& world, EntityId entity, float currentFov, float fraction) {
    if (fraction >= 1.0F) return currentFov;
    const auto* history = world.getComponent<RenderPoseHistory>(entity);
    return history && history->previousFov ? std::lerp(*history->previousFov, currentFov, fraction) : currentFov;
}

}
