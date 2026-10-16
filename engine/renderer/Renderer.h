#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <chrono>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
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
#include "engine/resources/async_load_queue.h"
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

struct ImDrawList;

namespace engine::renderer {

class UiCanvasCompositor;

using AssetLoadingStats = resources::AssetLoadingStats;

struct ImGuiImplDiligentDeleter final {
    void operator()(Diligent::ImGuiImplDiligent* ptr) const;
};

class Renderer final {
public:
    struct UiImage final {
        void* textureId{nullptr};
        float width{0}, height{0};
    };
    Renderer();
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
        const resources::Texture* texture,
        const glm::mat4& modelMatrix,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& tint,
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F));
    void drawMesh(
        const resources::Mesh& mesh,
        const resources::ShaderProgram& shader,
        const resources::Texture& texture,
        const glm::mat4& modelMatrix,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& tint,
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F));
    void drawDebugAabb(
        const glm::vec3& min,
        const glm::vec3& max,
        const glm::mat4& viewProjectionMatrix,
        const glm::vec4& tint,
        float lineThickness = 0.04F);
    std::unique_ptr<rhi::IBuffer> createGpuBuffer(const rhi::BufferDesc& desc, const void* initialData);
    std::unique_ptr<rhi::IImage> createGpuImage(const rhi::ImageDesc& desc, const void* initialData);
    std::shared_ptr<resources::ShaderProgram> createShaderProgramFromFiles(
        const std::string& key,
        const std::string& vertexPath,
        const std::string& fragmentPath);
    std::shared_ptr<resources::Mesh> loadMesh(const std::string& path);
    std::shared_ptr<resources::Texture> loadTexture(const std::string& path);
    // Project-local asynchronous UI images. Keeps borrowed views alive through
    // submission, deduplicates aliases, and bounds requests/cache growth.
    UiImage resolveUiImage(std::string_view image);
    // Host-only built-in editor assets, independent of the open project's root.
    UiImage resolveEditorIcon(std::string_view image);
    UiImage resolveUiCanvas(const void* key, const ImDrawList& list,
                           float x, float y, float width, float height);
    std::shared_ptr<resources::ShaderProgram> loadShaderProgram(const std::string& path);
    // Empty selects the engine's legacy assets. Project paths are resolved before
    // cache lookup and worker dispatch; only known built-in assets may fall back.
    void setProjectAssetRoot(std::filesystem::path assetsRoot);
    [[nodiscard]] std::string resolveResourcePath(const std::string& id) const;
    // The shared scheduler must outlive the renderer. Configure before init().
    void setJobSystem(core::JobSystem* jobs);
    void setAsyncLoadingEnabled(bool enabled);
    void setGpuUploadEnabled(bool enabled); // before init(), controls device contexts
    void setUploadBudget(std::size_t maximumUploads, double milliseconds);
    void setAssetInFlightLimit(std::size_t maximumAssets);
    [[nodiscard]] AssetLoadingStats assetLoadingStats() const;
    resources::ResourceManager& resourceManager();
    const resources::ResourceManager& resourceManager() const;
    [[nodiscard]] rhi::Extent2D frameExtent() const;
    void pollHotReload();
    void endFrame();
    void onResize(std::uint32_t width, std::uint32_t height);
    void shutdown();

private:
    struct FrameContext final {
        std::vector<std::shared_ptr<resources::Texture>> uiImages;
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
    std::filesystem::path m_engineAssetRoot;
    std::filesystem::path m_projectAssetRoot;
    struct UiImageEntry final {
        std::shared_ptr<resources::Texture> texture;
        std::shared_ptr<resources::Texture> pending;
        std::uint64_t lastUsedFrame{0};
        std::uint64_t reservedBytes{0};
        std::uint64_t pendingBytes{0};
        std::filesystem::file_time_type fileStamp{};
        std::chrono::steady_clock::time_point nextPoll{};
    };
    std::unordered_map<std::string, UiImageEntry> m_uiImages;
    std::unordered_map<std::string, std::string> m_uiImageAliases;
    std::unordered_map<std::string, std::string> m_uiImagePaths;
    std::unordered_map<std::string, std::string> m_editorIconPaths;
    std::chrono::steady_clock::time_point m_uiAliasNextPoll{};
    std::filesystem::file_time_type m_uiAliasWriteTime{};
    std::uint64_t m_uiImageFrame{0};
    unsigned m_uiImageRequests{0};
    bool m_uiImageAliasesLoaded{false};
    std::shared_ptr<resources::Mesh> m_placeholderMesh;
    std::shared_ptr<resources::Mesh> m_unitCubeMesh;
    std::shared_ptr<resources::Mesh> m_unitPyramidMesh;
    std::shared_ptr<resources::Mesh> m_unitSphereMesh;
    std::shared_ptr<resources::Texture> m_whiteTexture;
    std::shared_ptr<resources::ShaderProgram> m_fallbackShader;
    std::vector<FrameContext> m_frames;
    std::size_t m_currentFrameIndex{0};
    rhi::ICommandBuffer* m_activeCommandBuffer{nullptr};
    std::unique_ptr<Diligent::ImGuiImplDiligent, ImGuiImplDiligentDeleter> m_imgui;
    std::unique_ptr<UiCanvasCompositor> m_uiCanvases;
    bool m_imguiFrameOpen{false};
    struct ShaderHotReloadEntry final {
        std::weak_ptr<resources::ShaderProgram> program;
        std::string descriptorPath;
        bool descriptorIsFile{false};
        std::string vertexPath;
        std::string fragmentPath;
        std::filesystem::file_time_type appliedDescriptorWriteTime{};
        std::filesystem::file_time_type appliedVertexWriteTime{};
        std::filesystem::file_time_type appliedFragmentWriteTime{};
        std::filesystem::file_time_type pendingDescriptorWriteTime{};
        std::filesystem::file_time_type pendingVertexWriteTime{};
        std::filesystem::file_time_type pendingFragmentWriteTime{};
        std::filesystem::file_time_type failedDescriptorWriteTime{};
        std::filesystem::file_time_type failedVertexWriteTime{};
        std::filesystem::file_time_type failedFragmentWriteTime{};
        std::chrono::steady_clock::time_point pendingObservedAt{};
        bool reloadPending{false};
    };
    std::unordered_map<std::string, ShaderHotReloadEntry> m_shaderHotReloadEntries;
    core::JobSystem* m_jobSystem{nullptr};
    resources::AsyncLoadQueue m_assetLoads;
    bool m_gpuUploadEnabled{true};
    std::chrono::milliseconds m_hotReloadDebounce{175};
    bool m_initialized{false};

    bool initializePrimitivePipeline();
    bool initializeResourceSubsystem();
    bool initializeImGui();
    void loadUiImageAliases();
    UiImage resolveUiImagePath(const std::string& path);
    bool reserveUiImageBytes(std::uint64_t bytes, const std::string& protectedKey, bool newEntry);
    std::shared_ptr<resources::Mesh> requestMeshLoadFromDisk(const std::string& path);
    std::shared_ptr<resources::Texture> requestTextureLoadFromDisk(const std::string& path);
    std::shared_ptr<resources::ShaderProgram> requestShaderProgramLoadFromManifest(const std::string& path);
    std::shared_ptr<resources::Mesh> createPlaceholderMesh();
    std::shared_ptr<resources::Mesh> createUnitCubeMesh();
    std::shared_ptr<resources::Mesh> createUnitPyramidMesh();
    std::shared_ptr<resources::Mesh> createUnitSphereMesh();
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
    std::filesystem::path shaderAssetRoot(const std::string& descriptor) const;
    bool resolveShaderSources(resources::ShaderProgramSource& source, std::string* error) const;
};

} // namespace engine::renderer
