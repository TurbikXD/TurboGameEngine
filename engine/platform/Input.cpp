#include "engine/platform/Input.h"

namespace engine::platform {

std::array<bool, static_cast<std::size_t>(KeyCode::Count)> Input::s_keyStates{};
std::array<bool, static_cast<std::size_t>(MouseButton::Count)> Input::s_mouseStates{};
double Input::s_mouseX = 0.0;
double Input::s_mouseY = 0.0;

void Input::reset() {
    s_keyStates.fill(false);
    s_mouseStates.fill(false);
    s_mouseX = 0.0;
    s_mouseY = 0.0;
}

void Input::onEvent(const Event& event) {
    switch (event.type) {
        case EventType::KeyPressed:
            if (event.key != KeyCode::Unknown) {
                s_keyStates[static_cast<std::size_t>(event.key)] = true;
            }
            break;
        case EventType::KeyReleased:
            if (event.key != KeyCode::Unknown) {
                s_keyStates[static_cast<std::size_t>(event.key)] = false;
            }
            break;
        case EventType::MouseButtonPressed:
            s_mouseStates[static_cast<std::size_t>(event.mouseButton)] = true;
            break;
        case EventType::MouseButtonReleased:
            s_mouseStates[static_cast<std::size_t>(event.mouseButton)] = false;
            break;
        case EventType::MouseMoved:
            s_mouseX = event.mouseX;
            s_mouseY = event.mouseY;
            break;
        default:
            break;
    }
}

bool Input::isKeyDown(KeyCode key) {
    if (key == KeyCode::Unknown) {
        return false;
    }
    return s_keyStates[static_cast<std::size_t>(key)];
}

bool Input::isMouseButtonDown(MouseButton button) {
    return s_mouseStates[static_cast<std::size_t>(button)];
}

std::pair<double, double> Input::mousePosition() {
    return {s_mouseX, s_mouseY};
}

} // namespace engine::platform
