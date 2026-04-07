#include "engine/platform/Input.h"

namespace engine::platform {

std::array<bool, static_cast<std::size_t>(KeyCode::Count)> InputManager::s_currentKeyStates{};
std::array<bool, static_cast<std::size_t>(KeyCode::Count)> InputManager::s_previousKeyStates{};
std::array<bool, static_cast<std::size_t>(MouseButton::Count)> InputManager::s_currentMouseStates{};
std::array<bool, static_cast<std::size_t>(MouseButton::Count)> InputManager::s_previousMouseStates{};
double InputManager::s_mouseX = 0.0;
double InputManager::s_mouseY = 0.0;
double InputManager::s_mouseDeltaX = 0.0;
double InputManager::s_mouseDeltaY = 0.0;
double InputManager::s_scrollDeltaX = 0.0;
double InputManager::s_scrollDeltaY = 0.0;
bool InputManager::s_keyboardCaptured = false;
bool InputManager::s_mouseCaptured = false;

bool InputManager::isValidKey(const KeyCode key) {
    return key != KeyCode::Unknown && static_cast<std::size_t>(key) < s_currentKeyStates.size();
}

bool InputManager::isValidMouseButton(const MouseButton button) {
    return static_cast<std::size_t>(button) < s_currentMouseStates.size();
}

void InputManager::reset() {
    s_currentKeyStates.fill(false);
    s_previousKeyStates.fill(false);
    s_currentMouseStates.fill(false);
    s_previousMouseStates.fill(false);
    s_mouseX = 0.0;
    s_mouseY = 0.0;
    s_mouseDeltaX = 0.0;
    s_mouseDeltaY = 0.0;
    s_scrollDeltaX = 0.0;
    s_scrollDeltaY = 0.0;
    s_keyboardCaptured = false;
    s_mouseCaptured = false;
}

void InputManager::beginFrame() {
    s_previousKeyStates = s_currentKeyStates;
    s_previousMouseStates = s_currentMouseStates;
    s_mouseDeltaX = 0.0;
    s_mouseDeltaY = 0.0;
    s_scrollDeltaX = 0.0;
    s_scrollDeltaY = 0.0;
}

void InputManager::onEvent(const Event& event) {
    switch (event.type) {
        case EventType::KeyPressed:
            if (isValidKey(event.key)) {
                s_currentKeyStates[static_cast<std::size_t>(event.key)] = true;
            }
            break;
        case EventType::KeyReleased:
            if (isValidKey(event.key)) {
                s_currentKeyStates[static_cast<std::size_t>(event.key)] = false;
            }
            break;
        case EventType::MouseButtonPressed:
            if (isValidMouseButton(event.mouseButton)) {
                s_currentMouseStates[static_cast<std::size_t>(event.mouseButton)] = true;
            }
            break;
        case EventType::MouseButtonReleased:
            if (isValidMouseButton(event.mouseButton)) {
                s_currentMouseStates[static_cast<std::size_t>(event.mouseButton)] = false;
            }
            break;
        case EventType::MouseMoved:
            s_mouseDeltaX += event.mouseX - s_mouseX;
            s_mouseDeltaY += event.mouseY - s_mouseY;
            s_mouseX = event.mouseX;
            s_mouseY = event.mouseY;
            break;
        case EventType::MouseScrolled:
            s_scrollDeltaX += event.scrollX;
            s_scrollDeltaY += event.scrollY;
            break;
        default:
            break;
    }
}

void InputManager::setCaptureState(const bool keyboardCaptured, const bool mouseCaptured) {
    s_keyboardCaptured = keyboardCaptured;
    s_mouseCaptured = mouseCaptured;
}

bool InputManager::isKeyDown(const KeyCode key) {
    if (!isValidKey(key) || s_keyboardCaptured) {
        return false;
    }
    return s_currentKeyStates[static_cast<std::size_t>(key)];
}

bool InputManager::isKeyDownRaw(const KeyCode key) {
    if (!isValidKey(key)) {
        return false;
    }
    return s_currentKeyStates[static_cast<std::size_t>(key)];
}

bool InputManager::wasKeyPressed(const KeyCode key) {
    if (!isValidKey(key) || s_keyboardCaptured) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(key);
    return s_currentKeyStates[index] && !s_previousKeyStates[index];
}

bool InputManager::wasKeyReleased(const KeyCode key) {
    if (!isValidKey(key) || s_keyboardCaptured) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(key);
    return !s_currentKeyStates[index] && s_previousKeyStates[index];
}

bool InputManager::isMouseButtonDown(const MouseButton button) {
    if (!isValidMouseButton(button) || s_mouseCaptured) {
        return false;
    }
    return s_currentMouseStates[static_cast<std::size_t>(button)];
}

bool InputManager::isMouseButtonDownRaw(const MouseButton button) {
    if (!isValidMouseButton(button)) {
        return false;
    }
    return s_currentMouseStates[static_cast<std::size_t>(button)];
}

bool InputManager::wasMouseButtonPressed(const MouseButton button) {
    if (!isValidMouseButton(button) || s_mouseCaptured) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(button);
    return s_currentMouseStates[index] && !s_previousMouseStates[index];
}

bool InputManager::wasMouseButtonReleased(const MouseButton button) {
    if (!isValidMouseButton(button) || s_mouseCaptured) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(button);
    return !s_currentMouseStates[index] && s_previousMouseStates[index];
}

std::pair<double, double> InputManager::mousePosition() {
    return {s_mouseX, s_mouseY};
}

std::pair<double, double> InputManager::mouseDelta() {
    if (s_mouseCaptured) {
        return {0.0, 0.0};
    }
    return {s_mouseDeltaX, s_mouseDeltaY};
}

std::pair<double, double> InputManager::mouseDeltaRaw() {
    return {s_mouseDeltaX, s_mouseDeltaY};
}

std::pair<double, double> InputManager::scrollDelta() {
    if (s_mouseCaptured) {
        return {0.0, 0.0};
    }
    return {s_scrollDeltaX, s_scrollDeltaY};
}

} // namespace engine::platform
