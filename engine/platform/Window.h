#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "engine/platform/Events.h"

namespace engine::platform {

struct WindowDesc final {
    int width{1280};
    int height{720};
    std::string title{"TurboGameEngine"};
    bool useOpenGLContext{true};
    bool vsync{true};
};

enum class CursorMode : std::uint8_t {
    Normal = 0,
    Hidden,
    Disabled
};

class Window {
public:
    using EventCallback = std::function<void(const Event&)>;

    virtual ~Window() = default;

    static std::unique_ptr<Window> create(const WindowDesc& desc);

    virtual void pollEvents() = 0;
    virtual bool shouldClose() const = 0;
    virtual void requestClose() = 0;

    virtual void* nativeHandle() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;

    virtual void setTitle(const std::string& title) = 0;
    virtual void setVSync(bool enabled) = 0;
    virtual void setCursorMode(CursorMode mode) = 0;
    [[nodiscard]] virtual CursorMode cursorMode() const = 0;
    virtual bool hasOpenGLContext() const = 0;

    virtual void setEventCallback(EventCallback callback) = 0;
};

} // namespace engine::platform
