#include "engine/renderer/Renderer.h"

#include <array>
#include <string>

#include <glm/glm.hpp>

#include "engine/core/Log.h"
#include "engine/platform/Window.h"

namespace engine::renderer {

namespace {

struct Vertex final {
    glm::vec2 position;
    glm::vec3 color;
};

constexpr std::array<Vertex, 3> kTriangleVertices{
    Vertex{glm::vec2(0.0F, 0.35F), glm::vec3(1.0F, 0.2F, 0.2F)},
    Vertex{glm::vec2(-0.30F, -0.25F), glm::vec3(0.2F, 1.0F, 0.3F)},
    Vertex{glm::vec2(0.30F, -0.25F), glm::vec3(0.2F, 0.4F, 1.0F)},
};

} // namespace

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::init(platform::Window& window, rhi::BackendType backend, bool vsync) {
    m_window = &window;
    m_backend = backend;

    std::string error;
    rhi::DeviceCreateDesc deviceDesc{};
    deviceDesc.window = &window;
#if defined(NDEBUG)
    deviceDesc.enableValidation = false;
#else
    deviceDesc.enableValidation = true;
#endif

    m_device = rhi::createDevice(backend, deviceDesc, &error);
    if (!m_device) {
        ENGINE_LOG_ERROR("Failed to create device for {}: {}", rhi::toString(backend), error);
        return false;
    }

    ENGINE_LOG_INFO("Renderer initialized with backend {}", rhi::toString(backend));

    rhi::SwapchainDesc swapchainDesc{};
    swapchainDesc.width = static_cast<std::uint32_t>(window.width());
    swapchainDesc.height = static_cast<std::uint32_t>(window.height());
    swapchainDesc.vsync = vsync;
    m_swapchain = m_device->createSwapchain(swapchainDesc);
    if (!m_swapchain) {
        ENGINE_LOG_ERROR("Failed to create swapchain");
        return false;
    }

    m_graphicsQueue = &m_device->graphicsQueue();

    constexpr std::size_t framesInFlight = 2;
    m_frames.resize(framesInFlight);
    for (auto& frame : m_frames) {
        frame.commandBuffer = m_device->createCommandBuffer();
        frame.inFlight = m_device->createFence(true);
        frame.imageAvailable = m_device->createSemaphore();
        frame.renderFinished = m_device->createSemaphore();
        if (!frame.commandBuffer || !frame.inFlight || !frame.imageAvailable || !frame.renderFinished) {
            ENGINE_LOG_ERROR("Failed to create frame context synchronization objects");
            return false;
        }
    }

    rhi::BufferDesc vertexBufferDesc{};
    vertexBufferDesc.size = sizeof(kTriangleVertices);
    vertexBufferDesc.usage = rhi::BufferUsage::Vertex;
    m_vertexBuffer = m_device->createBuffer(vertexBufferDesc, kTriangleVertices.data());
    if (!m_vertexBuffer) {
        ENGINE_LOG_ERROR("Failed to create vertex buffer");
        return false;
    }

    const std::string vertexPath = "assets/shaders_gl/basic.vert";
    const std::string fragmentPath = "assets/shaders_gl/basic.frag";

    rhi::ShaderModuleDesc vertexShaderDesc{};
    vertexShaderDesc.stage = rhi::ShaderStage::Vertex;
    vertexShaderDesc.path = vertexPath;
    m_vertexShader = m_device->createShaderModule(vertexShaderDesc);

    rhi::ShaderModuleDesc fragmentShaderDesc{};
    fragmentShaderDesc.stage = rhi::ShaderStage::Fragment;
    fragmentShaderDesc.path = fragmentPath;
    m_fragmentShader = m_device->createShaderModule(fragmentShaderDesc);

    if (!m_vertexShader || !m_fragmentShader) {
        ENGINE_LOG_ERROR("Failed to create shader modules (backend may be non-functional yet)");
        return false;
    }

    m_pipelineLayout = m_device->createPipelineLayout();
    if (!m_pipelineLayout) {
        ENGINE_LOG_ERROR("Failed to create pipeline layout");
        return false;
    }

    rhi::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = m_vertexShader.get();
    pipelineDesc.fragmentShader = m_fragmentShader.get();
    pipelineDesc.topology = rhi::PrimitiveTopology::TriangleList;
    m_pipeline = m_device->createGraphicsPipeline(pipelineDesc);
    if (!m_pipeline) {
        ENGINE_LOG_ERROR("Failed to create graphics pipeline (backend may be stubbed)");
        return false;
    }

    m_initialized = true;
    return true;
}

void Renderer::beginFrame(const glm::vec4& clearColor) {
    if (!m_initialized || !m_swapchain || m_frames.empty()) {
        return;
    }

    FrameContext& frame = m_frames[m_currentFrameIndex];
    frame.inFlight->wait();
    frame.inFlight->reset();

    m_swapchain->acquireNextImage(frame.imageAvailable.get());

    frame.commandBuffer->begin();

    rhi::RenderPassDesc passDesc{};
    passDesc.clearColor = {clearColor.r, clearColor.g, clearColor.b, clearColor.a};
    passDesc.clearColorTarget = true;
    frame.commandBuffer->beginRenderPass(passDesc);

    const rhi::Extent2D extent = m_swapchain->extent();
    rhi::Viewport viewport{};
    viewport.x = 0.0F;
    viewport.y = 0.0F;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    frame.commandBuffer->setViewport(viewport);

    rhi::Rect2D scissor{};
    scissor.x = 0;
    scissor.y = 0;
    scissor.width = extent.width;
    scissor.height = extent.height;
    frame.commandBuffer->setScissor(scissor);

    m_activeCommandBuffer = frame.commandBuffer.get();
}

void Renderer::drawTestPrimitive(const Transform2D& transform) {
    if (!m_activeCommandBuffer || !m_pipeline || !m_vertexBuffer) {
        return;
    }

    m_activeCommandBuffer->bindPipeline(*m_pipeline);
    m_activeCommandBuffer->bindVertexBuffer(*m_vertexBuffer);

    const glm::mat4 transformMatrix = transform.toMatrix();
    m_activeCommandBuffer->pushConstants(&transformMatrix, sizeof(transformMatrix));
    m_activeCommandBuffer->draw(3, 0);
}

void Renderer::endFrame() {
    if (!m_initialized || !m_swapchain || m_frames.empty() || !m_activeCommandBuffer) {
        return;
    }

    FrameContext& frame = m_frames[m_currentFrameIndex];
    m_activeCommandBuffer->endRenderPass();
    m_activeCommandBuffer->end();

    m_graphicsQueue->submit(
        *m_activeCommandBuffer, frame.imageAvailable.get(), frame.renderFinished.get(), frame.inFlight.get());
    m_swapchain->present(*m_graphicsQueue, frame.renderFinished.get());

    m_activeCommandBuffer = nullptr;
    m_currentFrameIndex = (m_currentFrameIndex + 1) % m_frames.size();
}

void Renderer::onResize(std::uint32_t width, std::uint32_t height) {
    if (!m_swapchain || width == 0 || height == 0) {
        return;
    }
    m_swapchain->resize(width, height);
    ENGINE_LOG_INFO("Renderer resized swapchain to {}x{}", width, height);
}

void Renderer::shutdown() {
    if (!m_initialized) {
        return;
    }

    m_activeCommandBuffer = nullptr;
    m_frames.clear();
    m_pipeline.reset();
    m_pipelineLayout.reset();
    m_vertexShader.reset();
    m_fragmentShader.reset();
    m_vertexBuffer.reset();
    m_swapchain.reset();
    m_device.reset();
    m_graphicsQueue = nullptr;
    m_window = nullptr;
    m_initialized = false;
    ENGINE_LOG_INFO("Renderer shutdown complete");
}

} // namespace engine::renderer
