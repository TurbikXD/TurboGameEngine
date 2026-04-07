#pragma once

#include "engine/ecs/components.h"

namespace engine::game {

class FreeCameraController final {
public:
    struct Settings final {
        float moveSpeed{12.0F};
        float sprintMultiplier{3.0F};
        float mouseSensitivity{0.0021F};
        float pitchLimitRadians{glm::radians(88.0F)};
    };

    FreeCameraController() = default;
    explicit FreeCameraController(Settings settings);

    void setSettings(const Settings& settings);
    [[nodiscard]] const Settings& settings() const;

    void update(engine::ecs::Transform& transform, double dt, bool flyModeActive, bool mouseLookActive);

private:
    Settings m_settings{};
};

} // namespace engine::game
