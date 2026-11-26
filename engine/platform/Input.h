#pragma once

#include <array>
#include <cstddef>
#include <utility>

#include "engine/platform/Events.h"

namespace engine::platform {

class InputManager final {
public:
    static void reset();
    static void beginFrame();
    static void beginFixedTick();
    static void endFixedTick();
    static bool fixedTickActive();
    static void onEvent(const Event& event);
    static void setCaptureState(bool keyboardCaptured, bool mouseCaptured);

    static bool isKeyDown(KeyCode key);
    static bool isKeyDownRaw(KeyCode key);
    static bool wasKeyPressed(KeyCode key);
    static bool wasKeyPressedRaw(KeyCode key);
    static bool wasKeyReleased(KeyCode key);
    static bool wasKeyReleasedRaw(KeyCode key);
    static bool wasKeyPressedForTickRaw(KeyCode key);
    static bool wasKeyReleasedForTickRaw(KeyCode key);
    static void clearTickKeyEdges();
    static bool IsKeyDown(const KeyCode key) {
        return isKeyDown(key);
    }
    static bool WasKeyPressed(const KeyCode key) {
        return wasKeyPressed(key);
    }
    static bool WasKeyReleased(const KeyCode key) {
        return wasKeyReleased(key);
    }

    static bool isMouseButtonDown(MouseButton button);
    static bool isMouseButtonDownRaw(MouseButton button);
    static bool wasMouseButtonPressed(MouseButton button);
    static bool wasMouseButtonReleased(MouseButton button);
    static bool wasMouseButtonPressedForTickRaw(MouseButton button);
    static bool IsMouseButtonDown(const MouseButton button) {
        return isMouseButtonDown(button);
    }

    static std::pair<double, double> mousePosition();
    static std::pair<double, double> mouseDelta();
    static std::pair<double, double> mouseDeltaRaw();
    static std::pair<double, double> mouseDeltaForTickRaw();
    static void resetMouseDelta();
    static std::pair<double, double> scrollDelta();
    static std::pair<double, double> GetMousePosition() {
        return mousePosition();
    }
    static std::pair<double, double> GetMouseDelta() {
        return mouseDelta();
    }
    static std::pair<double, double> GetScrollDelta() {
        return scrollDelta();
    }

private:
    static bool isValidKey(KeyCode key);
    static bool isValidMouseButton(MouseButton button);
    static void clearTickPointerInput();

    static std::array<bool, static_cast<std::size_t>(KeyCode::Count)> s_currentKeyStates;
    static std::array<bool, static_cast<std::size_t>(KeyCode::Count)> s_previousKeyStates;
    static std::array<bool, static_cast<std::size_t>(KeyCode::Count)> s_tickKeyPresses;
    static std::array<bool, static_cast<std::size_t>(KeyCode::Count)> s_tickKeyReleases;
    static std::array<bool, static_cast<std::size_t>(MouseButton::Count)> s_currentMouseStates;
    static std::array<bool, static_cast<std::size_t>(MouseButton::Count)> s_previousMouseStates;
    static std::array<bool, static_cast<std::size_t>(MouseButton::Count)> s_tickMousePresses;
    static double s_mouseX;
    static double s_mouseY;
    static bool s_mousePositionKnown;
    static double s_mouseDeltaX;
    static double s_mouseDeltaY;
    static double s_tickMouseDeltaX;
    static double s_tickMouseDeltaY;
    static double s_scrollDeltaX;
    static double s_scrollDeltaY;
    static bool s_keyboardCaptured;
    static bool s_mouseCaptured;
    static bool s_fixedTickActive;
};

class Input final {
public:
    static void reset() {
        InputManager::reset();
    }

    static void beginFrame() {
        InputManager::beginFrame();
    }

    static void onEvent(const Event& event) {
        InputManager::onEvent(event);
    }

    static bool isKeyDown(const KeyCode key) {
        return InputManager::isKeyDown(key);
    }

    static bool isMouseButtonDown(const MouseButton button) {
        return InputManager::isMouseButtonDown(button);
    }

    static std::pair<double, double> mousePosition() {
        return InputManager::mousePosition();
    }

private:
    Input() = delete;
};

}
