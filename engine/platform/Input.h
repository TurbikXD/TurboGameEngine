#pragma once

#include <array>
#include <utility>

#include "engine/platform/Events.h"

namespace engine::platform {

class Input final {
public:
    static void reset();
    static void onEvent(const Event& event);

    static bool isKeyDown(KeyCode key);
    static bool isMouseButtonDown(MouseButton button);
    static std::pair<double, double> mousePosition();

private:
    static std::array<bool, static_cast<std::size_t>(KeyCode::Count)> s_keyStates;
    static std::array<bool, static_cast<std::size_t>(MouseButton::Count)> s_mouseStates;
    static double s_mouseX;
    static double s_mouseY;
};

} // namespace engine::platform
