#include "engine/rhi_opengl/GLDevice.h"

#include <stdexcept>
#include <string>

#include <GLFW/glfw3.h>

#include "engine/core/Log.h"
#include "engine/platform/Window.h"
#include "engine/rhi_opengl/GLCommandBuffer.h"
#include "engine/rhi_opengl/GLPipeline.h"
#include "engine/rhi_opengl/GLResources.h"
#include "engine/rhi_opengl/GLShader.h"
#include "engine/rhi_opengl/GLSwapchain.h"
#include "engine/rhi_opengl/GLUtils.h"

namespace engine::rhi::gl {

namespace {

class GLSemaphore final : public ISemaphore {};

class GLFence final : public IFence {
public:
    explicit GLFence(bool signaled) : m_signaled(signaled) {}

    void wait() override {
        if (!m_signaled) {
            glFinish();
            m_signaled = true;
        }
    }

    void reset() override {
        m_signaled = false;
    }

    void signal() override {
        m_signaled = true;
    }

private:
    bool m_signaled{false};
};

} // namespace

GLDevice::GLDevice(const DeviceCreateDesc& desc) : m_window(desc.window) {
    if (m_window == nullptr) {
        throw std::runtime_error("OpenGL device requires a valid window");
    }
    if (!m_window->hasOpenGLContext()) {
        throw std::runtime_error("OpenGL backend requires a window created with an OpenGL context");
    }

    auto* glfwWindow = static_cast<GLFWwindow*>(m_window->nativeHandle());
    glfwMakeContextCurrent(glfwWindow);

    const int loadStatus = gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress));
    if (loadStatus == 0) {
        throw std::runtime_error("Failed to load OpenGL function pointers via glad");
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ENGINE_LOG_INFO("OpenGL context initialized: {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
}

std::unique_ptr<ISwapchain> GLDevice::createSwapchain(const SwapchainDesc& desc) {
    return std::make_unique<GLSwapchain>(*m_window, desc);
}

std::unique_ptr<ICommandBuffer> GLDevice::createCommandBuffer() {
    return std::make_unique<GLCommandBuffer>();
}

std::unique_ptr<IBuffer> GLDevice::createBuffer(const BufferDesc& desc, const void* initialData) {
    return std::make_unique<GLBuffer>(desc, initialData);
}

std::unique_ptr<IShaderModule> GLDevice::createShaderModule(const ShaderModuleDesc& desc) {
    try {
        const std::string source = readTextFile(desc.path);
        const std::uint32_t shaderId = compileShaderFromSource(desc.stage, source, desc.path);
        return std::make_unique<GLShaderModule>(desc.stage, shaderId, desc.path);
    } catch (const std::exception& ex) {
        ENGINE_LOG_ERROR("OpenGL shader module creation failed: {}", ex.what());
        return nullptr;
    }
}

std::unique_ptr<IPipelineLayout> GLDevice::createPipelineLayout() {
    return std::make_unique<GLPipelineLayout>();
}

std::unique_ptr<IGraphicsPipeline> GLDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto* vertexShader = dynamic_cast<GLShaderModule*>(desc.vertexShader);
    auto* fragmentShader = dynamic_cast<GLShaderModule*>(desc.fragmentShader);
    if (vertexShader == nullptr || fragmentShader == nullptr) {
        ENGINE_LOG_ERROR("OpenGL pipeline expects GLShaderModule vertex/fragment stages");
        return nullptr;
    }

    try {
        const std::uint32_t program = linkProgram(vertexShader->id(), fragmentShader->id());
        return std::make_unique<GLGraphicsPipeline>(program);
    } catch (const std::exception& ex) {
        ENGINE_LOG_ERROR("OpenGL pipeline creation failed: {}", ex.what());
        return nullptr;
    }
}

std::unique_ptr<IBindGroupLayout> GLDevice::createBindGroupLayout() {
    return std::make_unique<GLBindGroupLayout>();
}

std::unique_ptr<IBindGroup> GLDevice::createBindGroup() {
    return std::make_unique<GLBindGroup>();
}

std::unique_ptr<ISemaphore> GLDevice::createSemaphore() {
    return std::make_unique<GLSemaphore>();
}

std::unique_ptr<IFence> GLDevice::createFence(bool signaled) {
    return std::make_unique<GLFence>(signaled);
}

IQueue& GLDevice::graphicsQueue() {
    return m_queue;
}

BackendType GLDevice::backendType() const {
    return BackendType::OpenGL;
}

void GLQueue::submit(
    ICommandBuffer& commandBuffer,
    ISemaphore* waitSemaphore,
    ISemaphore* signalSemaphore,
    IFence* signalFence) {
    (void)commandBuffer;
    (void)waitSemaphore;
    (void)signalSemaphore;

    glFlush();
    if (signalFence != nullptr) {
        signalFence->signal();
    }
}

std::unique_ptr<IDevice> createOpenGLDevice(const DeviceCreateDesc& desc, std::string* errorMessage) {
    try {
        return std::make_unique<GLDevice>(desc);
    } catch (const std::exception& ex) {
        if (errorMessage != nullptr) {
            *errorMessage = ex.what();
        }
        return nullptr;
    }
}

} // namespace engine::rhi::gl
