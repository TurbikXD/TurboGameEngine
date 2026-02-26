#include "engine/rhi_opengl/GLSwapchain.h"

#include <GLFW/glfw3.h>

#include "engine/platform/Window.h"
#include "engine/rhi_opengl/GLUtils.h"

namespace engine::rhi::gl {

GLSwapchain::GLSwapchain(platform::Window& window, const SwapchainDesc& desc)
    : m_window(window), m_extent{desc.width, desc.height}, m_vsync(desc.vsync) {
    m_window.setVSync(m_vsync);
}

std::uint32_t GLSwapchain::acquireNextImage(ISemaphore* imageAvailable) {
    (void)imageAvailable;
    return 0;
}

void GLSwapchain::present(IQueue& queue, ISemaphore* renderFinished) {
    (void)queue;
    (void)renderFinished;
    auto* glfwWindow = static_cast<GLFWwindow*>(m_window.nativeHandle());
    glfwSwapBuffers(glfwWindow);
}

void GLSwapchain::resize(std::uint32_t width, std::uint32_t height) {
    m_extent = Extent2D{width, height};
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

Extent2D GLSwapchain::extent() const {
    return m_extent;
}

} // namespace engine::rhi::gl
