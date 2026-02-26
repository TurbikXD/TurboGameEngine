#pragma once

#include <string>

#include "engine/platform/Window.h"

struct GLFWwindow;

namespace engine::platform {

class WindowGLFW final : public Window {
public:
    explicit WindowGLFW(const WindowDesc& desc);
    ~WindowGLFW() override;

    void pollEvents() override;
    bool shouldClose() const override;
    void requestClose() override;
    void* nativeHandle() const override;
    int width() const override;
    int height() const override;
    void setTitle(const std::string& title) override;
    void setVSync(bool enabled) override;
    bool hasOpenGLContext() const override;
    void setEventCallback(EventCallback callback) override;

private:
    void dispatch(const Event& event) const;
    static void ensureGlfwInitialized();
    static void shutdownGlfwIfUnused();

    GLFWwindow* m_window{nullptr};
    EventCallback m_eventCallback{};
    int m_width{0};
    int m_height{0};
    bool m_hasOpenGLContext{false};
};

} // namespace engine::platform
