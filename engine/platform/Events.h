#pragma once

#include <cstdint>

namespace engine::platform {

enum class KeyCode : std::uint16_t {
    Unknown = 0,
    Enter,
    Escape,
    W,
    A,
    S,
    D,
    Q,
    E,
    Space,
    LeftShift,
    RightShift,
    F1,
    Left,
    Right,
    Up,
    Down,
    Count
};

enum class MouseButton : std::uint8_t {
    Left = 0,
    Right,
    Middle,
    Count
};

enum class EventType : std::uint8_t {
    None = 0,
    Quit,
    KeyPressed,
    KeyReleased,
    MouseButtonPressed,
    MouseButtonReleased,
    MouseMoved,
    MouseScrolled,
    Resize
};

struct Event final {
    EventType type{EventType::None};
    KeyCode key{KeyCode::Unknown};
    MouseButton mouseButton{MouseButton::Left};
    bool repeat{false};
    double mouseX{0.0};
    double mouseY{0.0};
    double scrollX{0.0};
    double scrollY{0.0};
    int width{0};
    int height{0};
};

} // namespace engine::platform
