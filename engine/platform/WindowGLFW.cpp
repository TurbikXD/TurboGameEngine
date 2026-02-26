#include "engine/platform/WindowGLFW.h"

#include <atomic>
#include <stdexcept>
#include <utility>

#include <GLFW/glfw3.h>

#include "engine/core/Log.h"

namespace engine::platform {

namespace {

std::atomic<int> g_windowCount{0};
std::atomic<bool> g_glfwInitialized{false};

KeyCode mapKey(int glfwKey) {
    switch (glfwKey) {
        case GLFW_KEY_ENTER:
            return KeyCode::Enter;
        case GLFW_KEY_ESCAPE:
            return KeyCode::Escape;
        case GLFW_KEY_LEFT:
            return KeyCode::Left;
        case GLFW_KEY_RIGHT:
            return KeyCode::Right;
        case GLFW_KEY_UP:
            return KeyCode::Up;
        case GLFW_KEY_DOWN:
            return KeyCode::Down;
        default:
            return KeyCode::Unknown;
    }
}

MouseButton mapMouseButton(int glfwButton) {
    switch (glfwButton) {
        case GLFW_MOUSE_BUTTON_LEFT:
            return MouseButton::Left;
        case GLFW_MOUSE_BUTTON_RIGHT:
            return MouseButton::Right;
        default:
            return MouseButton::Middle;
    }
}

} // namespace

void WindowGLFW::ensureGlfwInitialized() {
    if (g_glfwInitialized.load()) {
        return;
    }
    if (!glfwInit()) {
        throw std::runtime_error("glfwInit failed");
    }
    g_glfwInitialized.store(true);
}

void WindowGLFW::shutdownGlfwIfUnused() {
    if (g_windowCount.load() == 0 && g_glfwInitialized.load()) {
        glfwTerminate();
        g_glfwInitialized.store(false);
    }
}

WindowGLFW::WindowGLFW(const WindowDesc& desc) : m_width(desc.width), m_height(desc.height) {
    ensureGlfwInitialized();

    m_hasOpenGLContext = desc.useOpenGLContext;
    if (desc.useOpenGLContext) {
#if defined(__APPLE__)
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }

    m_window = glfwCreateWindow(desc.width, desc.height, desc.title.c_str(), nullptr, nullptr);
    if (m_window == nullptr) {
        throw std::runtime_error("glfwCreateWindow failed");
    }

    g_windowCount.fetch_add(1);

    glfwSetWindowUserPointer(m_window, this);

    if (desc.useOpenGLContext) {
        glfwMakeContextCurrent(m_window);
        glfwSwapInterval(desc.vsync ? 1 : 0);
    }

    glfwSetWindowCloseCallback(m_window, [](GLFWwindow* window) {
        auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(window));
        if (self != nullptr) {
            Event event{};
            event.type = EventType::Quit;
            self->dispatch(event);
        }
    });

    glfwSetFramebufferSizeCallback(m_window, [](GLFWwindow* window, int width, int height) {
        auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(window));
        if (self == nullptr) {
            return;
        }
        self->m_width = width;
        self->m_height = height;
        Event event{};
        event.type = EventType::Resize;
        event.width = width;
        event.height = height;
        self->dispatch(event);
    });

    glfwSetKeyCallback(m_window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
        (void)scancode;
        (void)mods;
        auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(window));
        if (self == nullptr) {
            return;
        }
        Event event{};
        event.key = mapKey(key);
        event.repeat = (action == GLFW_REPEAT);
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            event.type = EventType::KeyPressed;
        } else if (action == GLFW_RELEASE) {
            event.type = EventType::KeyReleased;
        } else {
            return;
        }
        self->dispatch(event);
    });

    glfwSetMouseButtonCallback(m_window, [](GLFWwindow* window, int button, int action, int mods) {
        (void)mods;
        auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(window));
        if (self == nullptr) {
            return;
        }
        Event event{};
        event.mouseButton = mapMouseButton(button);
        if (action == GLFW_PRESS) {
            event.type = EventType::MouseButtonPressed;
        } else if (action == GLFW_RELEASE) {
            event.type = EventType::MouseButtonReleased;
        } else {
            return;
        }
        self->dispatch(event);
    });

    glfwSetCursorPosCallback(m_window, [](GLFWwindow* window, double x, double y) {
        auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(window));
        if (self == nullptr) {
            return;
        }
        Event event{};
        event.type = EventType::MouseMoved;
        event.mouseX = x;
        event.mouseY = y;
        self->dispatch(event);
    });
}

WindowGLFW::~WindowGLFW() {
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        g_windowCount.fetch_sub(1);
        shutdownGlfwIfUnused();
    }
}

void WindowGLFW::pollEvents() {
    glfwPollEvents();
}

bool WindowGLFW::shouldClose() const {
    return m_window != nullptr && glfwWindowShouldClose(m_window) != 0;
}

void WindowGLFW::requestClose() {
    if (m_window != nullptr) {
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
    }
}

void* WindowGLFW::nativeHandle() const {
    return m_window;
}

int WindowGLFW::width() const {
    return m_width;
}

int WindowGLFW::height() const {
    return m_height;
}

void WindowGLFW::setTitle(const std::string& title) {
    if (m_window != nullptr) {
        glfwSetWindowTitle(m_window, title.c_str());
    }
}

void WindowGLFW::setVSync(bool enabled) {
    if (m_hasOpenGLContext && m_window != nullptr) {
        glfwMakeContextCurrent(m_window);
        glfwSwapInterval(enabled ? 1 : 0);
    }
}

bool WindowGLFW::hasOpenGLContext() const {
    return m_hasOpenGLContext;
}

void WindowGLFW::setEventCallback(EventCallback callback) {
    m_eventCallback = std::move(callback);
}

void WindowGLFW::dispatch(const Event& event) const {
    if (m_eventCallback) {
        m_eventCallback(event);
    }
}

} // namespace engine::platform
