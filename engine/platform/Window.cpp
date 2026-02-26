#include "engine/platform/Window.h"

#include "engine/platform/WindowGLFW.h"

namespace engine::platform {

std::unique_ptr<Window> Window::create(const WindowDesc& desc) {
    return std::make_unique<WindowGLFW>(desc);
}

} // namespace engine::platform
