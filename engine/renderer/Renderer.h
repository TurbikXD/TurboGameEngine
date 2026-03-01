#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "engine/ecs/components.h"
#include "engine/rhi/CommandBuffer.h"
#include "engine/rhi/Device.h"
#include "engine/rhi/Pipeline.h"
#include "engine/rhi/RHI.h"
#include "engine/rhi/Resources.h"
#include "engine/rhi/Swapchain.h"
#include "engine/rhi/Sync.h"
#include "engine/resources/mesh.h"
#include "engine/resources/loaders.h"
#include "engine/resources/resource_manager.h"
#include "engine/resources/shader_program.h"
#include "engine/resources/texture.h"
#include "engine/renderer/Transform2D.h"

namespace engine::platform {
struct Event;
class Window;
}

namespace Diligent {
class ImGuiImplDiligent;
} // namespace Diligent

namespace engine::renderer {

struct ImGuiImplDiligentDeleter final {
    void operator()(Diligent::ImGuiImplDiligent* ptr) const;
};

class Renderer final {
public:
    Renderer() = default;
    ~Renderer();

    bool init(
        platform::Window& window,
        rhi::BackendType backend,
        bool vsync,
        const std::string& diligentDeviceType = "auto");
    void beginFrame(const glm::vec4& clearColor);
    void onEvent(const platform::Event& event);
    void beginImGuiFrame(float deltaSeconds);
    void renderImGui();
    bool isImGuiEnabled() const;
    bool wantsKeyboardCapture() const;
    bool wantsMouseCapture() const;
    void drawTestPrimitive(const Transform2D& transform);
    void drawPrimitive(ecs::PrimitiveType primitiveType, const glm::mat4& transformMatrix, const glm::vec4& tint);
    void drawMesh(
        const resources::Mesh& mesh,
        const resources::ShaderProgram& shader,
        const resources::Texture& texture,
        const glm::mat4& transformMatrix,
        const glm::vec4& tint);
    std::unique_ptr<rhi::IBuffer> createGpuBuffer(const rhi::BufferDesc& desc, const void* initialData);
    std::unique_ptr<rhi::IImage> createGpuImage(const rhi::ImageDesc& desc, const void* initialData);
    std::shared_ptr<resources::ShaderProgram> createShaderProgramFromFiles(
        const std::string& key,
        const std::string& vertexPath,
        const std::string& fragmentPath);
    std::shared_ptr<resources::Mesh> loadMesh(const std::string& path);
    std::shared_ptr<resources::Texture> loadTexture(const std::string& path);
    std::shared_ptr<resources::ShaderProgram> loadShaderProgram(const std::string& path);
    resources::ResourceManager& resourceManager();
    const resources::ResourceManager& resourceManager() const;
    void pollHotReload();
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
    std::unique_ptr<rhi::IShaderModule> m_primitiveVertexShader;
    std::unique_ptr<rhi::IShaderModule> m_primitiveFragmentShader;
    std::unique_ptr<rhi::IPipelineLayout> m_primitivePipelineLayout;
    std::unique_ptr<rhi::IGraphicsPipeline> m_primitivePipeline;
    std::unique_ptr<rhi::IBuffer> m_primitiveVertexBuffer;
    resources::ResourceManager m_resourceManager;
    std::shared_ptr<resources::Mesh> m_placeholderMesh;
    std::shared_ptr<resources::Texture> m_whiteTexture;
    std::shared_ptr<resources::ShaderProgram> m_fallbackShader;
    std::vector<FrameContext> m_frames;
    std::size_t m_currentFrameIndex{0};
    rhi::ICommandBuffer* m_activeCommandBuffer{nullptr};
    std::unique_ptr<Diligent::ImGuiImplDiligent, ImGuiImplDiligentDeleter> m_imgui;
    bool m_imguiFrameOpen{false};
    struct ShaderHotReloadEntry final {
        std::weak_ptr<resources::ShaderProgram> program;
        std::string descriptorPath;
        bool descriptorIsFile{false};
        std::string vertexPath;
        std::string fragmentPath;
        std::filesystem::file_time_type descriptorWriteTime{};
        std::filesystem::file_time_type vertexWriteTime{};
        std::filesystem::file_time_type fragmentWriteTime{};
    };
    std::unordered_map<std::string, ShaderHotReloadEntry> m_shaderHotReloadEntries;
    bool m_initialized{false};

    bool initializePrimitivePipeline();
    bool initializeResourceSubsystem();
    bool initializeImGui();
    std::shared_ptr<resources::Mesh> loadMeshFromDisk(const std::string& path);
    std::shared_ptr<resources::Texture> loadTextureFromDisk(const std::string& path);
    std::shared_ptr<resources::ShaderProgram> loadShaderProgramFromManifest(const std::string& path);
    std::shared_ptr<resources::Mesh> createPlaceholderMesh();
    std::shared_ptr<resources::Texture> createWhiteTexture();
    std::shared_ptr<resources::ShaderProgram> createFallbackShader();
    bool uploadMeshToGpu(resources::Mesh& mesh) const;
    std::shared_ptr<resources::ShaderProgram> createShaderProgram(
        const std::string& key,
        const std::string& vertexPath,
        const std::string& fragmentPath);
    void registerShaderForHotReload(
        const std::string& cacheKey,
        const std::shared_ptr<resources::ShaderProgram>& program,
        const resources::ShaderProgramSource& source);
    bool reloadShaderInPlace(resources::ShaderProgram& program, const resources::ShaderProgramSource& source);
};

} // namespace engine::renderer
