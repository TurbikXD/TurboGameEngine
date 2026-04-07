#include "engine/game/FreeCameraController.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include "engine/ecs/transform_utils.h"
#include "engine/platform/Input.h"

namespace engine::game {

FreeCameraController::FreeCameraController(Settings settings) : m_settings(std::move(settings)) {}

void FreeCameraController::setSettings(const Settings& settings) {
    m_settings = settings;
}

const FreeCameraController::Settings& FreeCameraController::settings() const {
    return m_settings;
}

void FreeCameraController::update(
    engine::ecs::Transform& transform,
    const double dt,
    const bool flyModeActive,
    const bool mouseLookActive) {
    const float dtSeconds = static_cast<float>(dt);

    if (mouseLookActive) {
        const auto [deltaX, deltaY] = platform::InputManager::mouseDeltaRaw();
        transform.rotationEulerRadians.y += static_cast<float>(deltaX) * m_settings.mouseSensitivity;
        transform.rotationEulerRadians.x -= static_cast<float>(deltaY) * m_settings.mouseSensitivity;
        transform.rotationEulerRadians.x =
            std::clamp(transform.rotationEulerRadians.x, -m_settings.pitchLimitRadians, m_settings.pitchLimitRadians);
        transform.rotationEulerRadians.y =
            std::remainder(transform.rotationEulerRadians.y, glm::two_pi<float>());
        transform.rotationEulerRadians.z = 0.0F;
    }

    if (!flyModeActive) {
        return;
    }

    const float yaw = transform.rotationEulerRadians.y;
    const glm::vec3 forward(std::sin(yaw), 0.0F, std::cos(yaw));
    const glm::vec3 worldUp(0.0F, 1.0F, 0.0F);
    const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));

    glm::vec3 movement(0.0F);
    if (platform::InputManager::isKeyDownRaw(platform::KeyCode::W)) {
        movement += forward;
    }
    if (platform::InputManager::isKeyDownRaw(platform::KeyCode::S)) {
        movement -= forward;
    }
    if (platform::InputManager::isKeyDownRaw(platform::KeyCode::A)) {
        movement -= right;
    }
    if (platform::InputManager::isKeyDownRaw(platform::KeyCode::D)) {
        movement += right;
    }
    if (platform::InputManager::isKeyDownRaw(platform::KeyCode::E)) {
        movement += worldUp;
    }
    if (platform::InputManager::isKeyDownRaw(platform::KeyCode::Q)) {
        movement -= worldUp;
    }

    if (glm::dot(movement, movement) > 1e-6F) {
        movement = glm::normalize(movement);
    }

    float speed = m_settings.moveSpeed;
    if (platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftShift) ||
        platform::InputManager::isKeyDownRaw(platform::KeyCode::RightShift)) {
        speed *= m_settings.sprintMultiplier;
    }

    transform.position += movement * speed * dtSeconds;
}

} // namespace engine::game
