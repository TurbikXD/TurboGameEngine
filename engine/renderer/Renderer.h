#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/vec4.hpp>

#include "engine/rhi/CommandBuffer.h"
#include "engine/rhi/Device.h"
#include "engine/rhi/Pipeline.h"
#include "engine/rhi/RHI.h"
#include "engine/rhi/Resources.h"
#include "engine/rhi/Swapchain.h"
#include "engine/rhi/Sync.h"
#include "engine/renderer/Transform2D.h"

namespace engine::platform {
class Window;
}

namespace engine::renderer {

class Renderer final {
public:
    Renderer() = default;
    ~Renderer();

    bool init(platform::Window& window, rhi::BackendType backend, bool vsync);
    void beginFrame(const glm::vec4& clearColor);
    void drawTestPrimitive(const Transform2D& transform);
    void endFrame();
    void onResize(std::uint32_t width, std::uint32_t height);
    void shutdown();

private:
    struct FrameContext final {
        std::unique_ptr<rhi::ICommandBuffer> commandBuffer;
        std::unique_ptr<rhi::IFence> inFlight;
        std::unique_ptr<rhi::ISemaphore> imageAvailable;
        std::unique_ptr<rhi::ISemaphore> renderFinished;
    };

    platform::Window* m_window{nullptr};
    rhi::BackendType m_backend{rhi::BackendType::OpenGL};
    std::unique_ptr<rhi::IDevice> m_device;
    std::unique_ptr<rhi::ISwapchain> m_swapchain;
    rhi::IQueue* m_graphicsQueue{nullptr};
    std::unique_ptr<rhi::IShaderModule> m_vertexShader;
    std::unique_ptr<rhi::IShaderModule> m_fragmentShader;
    std::unique_ptr<rhi::IPipelineLayout> m_pipelineLayout;
    std::unique_ptr<rhi::IGraphicsPipeline> m_pipeline;
    std::unique_ptr<rhi::IBuffer> m_vertexBuffer;
    std::vector<FrameContext> m_frames;
    std::size_t m_currentFrameIndex{0};
    rhi::ICommandBuffer* m_activeCommandBuffer{nullptr};
    bool m_initialized{false};
};

} // namespace engine::renderer
