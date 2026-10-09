#include "engine/rhi_diligent/DiligentDevice.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Profiling.h"
#include "engine/rhi/UploadQueue.h"
#include "engine/platform/Window.h"
#include "engine/rhi/CommandBuffer.h"
#include "engine/rhi/Pipeline.h"
#include "engine/rhi/Resources.h"
#include "engine/rhi/Swapchain.h"

#ifndef NOMINMAX
#    define NOMINMAX
#endif

#if defined(_WIN32)
#    define GLFW_EXPOSE_NATIVE_WIN32 1
#endif

#if defined(__linux__)
#    define GLFW_EXPOSE_NATIVE_X11 1
#endif

#if defined(__APPLE__)
#    define GLFW_EXPOSE_NATIVE_COCOA 1
#endif

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "third_party/DiligentEngine/DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Fence.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/InputLayout.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Shader.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Texture.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsTools/interface/MapHelper.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngineD3D11/interface/EngineFactoryD3D11.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngineOpenGL/interface/EngineFactoryOpenGL.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngineWebGPU/interface/EngineFactoryWebGPU.h"

#ifndef D3D11_SUPPORTED
#    define D3D11_SUPPORTED 0
#endif
#ifndef D3D12_SUPPORTED
#    define D3D12_SUPPORTED 0
#endif
#ifndef GL_SUPPORTED
#    define GL_SUPPORTED 0
#endif
#ifndef VULKAN_SUPPORTED
#    define VULKAN_SUPPORTED 0
#endif
#ifndef WEBGPU_SUPPORTED
#    define WEBGPU_SUPPORTED 0
#endif

namespace engine::rhi::diligent {

namespace {

constexpr std::string_view kVsConstantsName = "Constants";
constexpr std::string_view kPsConstantsName = "PixelConstants";
constexpr std::string_view kTextureName = "g_Texture";

struct DiligentVertexConstants final {
    float modelViewProjection[16]{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
    float model[16]{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
    float uvScaleBias[4]{1.0F, 1.0F, 0.0F, 0.0F};
};

struct DiligentPixelConstants final {
    float tint[4]{1.0F, 1.0F, 1.0F, 1.0F};
    float lightDirection[4]{-0.45F, -0.8F, 0.35F, 0.0F};
    float ambientColor[4]{0.47F, 0.5F, 0.58F, 0.18F};
};

struct DiligentPushConstants final {
    DiligentVertexConstants vertex{};
    DiligentPixelConstants pixel{};
};

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

std::optional<VertexLayout> inferVertexLayout(const std::string& shaderPath) {
    std::ifstream input(shaderPath, std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }

    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (contains(source, "float2 Position : ATTRIB0") && contains(source, "float3 Color : ATTRIB1")) {
        return VertexLayout::Position2Color3;
    }
    if (contains(source, "float3 Position : ATTRIB0") && contains(source, "float3 Normal : ATTRIB1") &&
        contains(source, "float2 TexCoord : ATTRIB2")) {
        return VertexLayout::Position3Normal3Uv2;
    }

    return std::nullopt;
}

bool shaderUsesTexture(std::string_view path) {
    const std::string lowered = toLower(std::string(path));
    return contains(lowered, "textured") || contains(lowered, "texture");
}

Diligent::SHADER_TYPE toDiligentShaderType(const ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:
            return Diligent::SHADER_TYPE_VERTEX;
        case ShaderStage::Fragment:
            return Diligent::SHADER_TYPE_PIXEL;
        default:
            return Diligent::SHADER_TYPE_UNKNOWN;
    }
}

Diligent::PRIMITIVE_TOPOLOGY toDiligentTopology(const PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::TriangleList:
        default:
            return Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

Diligent::TEXTURE_FORMAT toDiligentFormat(const ImageFormat format) {
    switch (format) {
        case ImageFormat::RGBA8:
        default:
            return Diligent::TEX_FORMAT_RGBA8_UNORM;
    }
}

void fillNativeWindow(platform::Window& window, Diligent::NativeWindow& nativeWindow) {
    auto* glfwWindow = static_cast<GLFWwindow*>(window.nativeHandle());
    if (glfwWindow == nullptr) {
        throw std::runtime_error("GLFW native window handle is null");
    }

#if defined(_WIN32)
    nativeWindow.hWnd = glfwGetWin32Window(glfwWindow);
#elif defined(__linux__)
    nativeWindow.WindowId = glfwGetX11Window(glfwWindow);
    nativeWindow.pDisplay = glfwGetX11Display();
#elif defined(__APPLE__)
    nativeWindow.pNSView = glfwGetCocoaWindow(glfwWindow);
#else
#    error Unsupported platform for Diligent native window binding.
#endif
}

void configureCommonEngineOptions(Diligent::EngineCreateInfo& createInfo, const bool enableValidation) {
    createInfo.EnableValidation = enableValidation ? Diligent::True : Diligent::False;
}

class DiligentSemaphore final : public ISemaphore {};

class DiligentFence final : public IFence {
public:
    explicit DiligentFence(const bool signaled) : m_signaled(signaled) {}

    void wait() override {
        m_signaled = true;
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

class DiligentBuffer final : public IBuffer {
public:
    DiligentBuffer(BufferDesc desc, Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer) :
        m_desc(std::move(desc)),
        m_buffer(std::move(buffer)) {}

    std::size_t size() const override {
        return m_desc.size;
    }

    BufferUsage usage() const override {
        return m_desc.usage;
    }

    ResourceHandle handle() const override {
        return static_cast<ResourceHandle>(reinterpret_cast<std::uintptr_t>(m_buffer.RawPtr()));
    }

    Diligent::IBuffer* nativeBuffer() const {
        return m_buffer;
    }

private:
    BufferDesc m_desc{};
    Diligent::RefCntAutoPtr<Diligent::IBuffer> m_buffer;
};

class DiligentImage final : public IImage {
public:
    DiligentImage(
        ImageDesc desc,
        Diligent::RefCntAutoPtr<Diligent::ITexture> texture,
        Diligent::RefCntAutoPtr<Diligent::ITextureView> srv) :
        m_desc(std::move(desc)),
        m_texture(std::move(texture)),
        m_srv(std::move(srv)) {}

    std::uint32_t width() const override {
        return m_desc.width;
    }

    std::uint32_t height() const override {
        return m_desc.height;
    }

    ResourceHandle handle() const override {
        return static_cast<ResourceHandle>(reinterpret_cast<std::uintptr_t>(m_texture.RawPtr()));
    }

    Diligent::ITextureView* shaderResourceView() const {
        return m_srv;
    }

private:
    ImageDesc m_desc{};
    Diligent::RefCntAutoPtr<Diligent::ITexture> m_texture;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> m_srv;
};

class DiligentShaderModule final : public IShaderModule {
public:
    DiligentShaderModule(
        ShaderModuleDesc desc,
        Diligent::RefCntAutoPtr<Diligent::IShader> shader,
        const std::optional<VertexLayout> inferredLayout,
        const bool usesTexture) :
        m_desc(std::move(desc)),
        m_shader(std::move(shader)),
        m_inferredLayout(inferredLayout),
        m_usesTexture(usesTexture) {}

    ShaderStage stage() const override {
        return m_desc.stage;
    }

    Diligent::IShader* nativeShader() const {
        return m_shader;
    }

    const std::optional<VertexLayout>& inferredLayout() const {
        return m_inferredLayout;
    }

    bool usesTexture() const {
        return m_usesTexture;
    }

private:
    ShaderModuleDesc m_desc{};
    Diligent::RefCntAutoPtr<Diligent::IShader> m_shader;
    std::optional<VertexLayout> m_inferredLayout;
    bool m_usesTexture{false};
};

class DiligentPipelineLayout final : public IPipelineLayout {};
class DiligentBindGroupLayout final : public IBindGroupLayout {};
class DiligentBindGroup final : public IBindGroup {};

class DiligentGraphicsPipeline final : public IGraphicsPipeline {
public:
    DiligentGraphicsPipeline(
        Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipelineState,
        Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> shaderBinding,
        Diligent::RefCntAutoPtr<Diligent::IBuffer> transformBuffer,
        Diligent::RefCntAutoPtr<Diligent::IBuffer> tintBuffer,
        const bool expectsTexture) :
        m_pipelineState(std::move(pipelineState)),
        m_shaderBinding(std::move(shaderBinding)),
        m_transformBuffer(std::move(transformBuffer)),
        m_tintBuffer(std::move(tintBuffer)),
        m_expectsTexture(expectsTexture) {}

    Diligent::IPipelineState* pipelineState() const {
        return m_pipelineState;
    }

    void updateConstants(Diligent::IDeviceContext& context, const void* data, const std::size_t size) const {
        DiligentPushConstants constants{};
        if (data != nullptr && size > 0U) {
            const auto* bytes = static_cast<const std::byte*>(data);
            const auto copyChunk = [&](void* destination, const std::size_t destinationSize, const std::size_t offset) {
                if (offset >= size) {
                    return;
                }
                const std::size_t bytesToCopy = std::min(destinationSize, size - offset);
                std::memcpy(destination, bytes + offset, bytesToCopy);
            };

            std::size_t offset = 0U;
            copyChunk(constants.vertex.modelViewProjection, sizeof(constants.vertex.modelViewProjection), offset);
            offset += sizeof(constants.vertex.modelViewProjection);
            copyChunk(constants.vertex.model, sizeof(constants.vertex.model), offset);
            offset += sizeof(constants.vertex.model);
            copyChunk(constants.vertex.uvScaleBias, sizeof(constants.vertex.uvScaleBias), offset);
            offset += sizeof(constants.vertex.uvScaleBias);
            copyChunk(constants.pixel.tint, sizeof(constants.pixel.tint), offset);
            offset += sizeof(constants.pixel.tint);
            copyChunk(constants.pixel.lightDirection, sizeof(constants.pixel.lightDirection), offset);
            offset += sizeof(constants.pixel.lightDirection);
            copyChunk(constants.pixel.ambientColor, sizeof(constants.pixel.ambientColor), offset);
        }

        if (m_transformBuffer) {
            Diligent::MapHelper<std::byte> mappedTransform(
                &context,
                m_transformBuffer,
                Diligent::MAP_WRITE,
                Diligent::MAP_FLAG_DISCARD);
            if (mappedTransform != nullptr) {
                std::memcpy(
                    static_cast<std::byte*>(mappedTransform),
                    &constants.vertex,
                    sizeof(constants.vertex));
            }
        }

        if (m_tintBuffer) {
            Diligent::MapHelper<std::byte> mappedTint(
                &context,
                m_tintBuffer,
                Diligent::MAP_WRITE,
                Diligent::MAP_FLAG_DISCARD);
            if (mappedTint != nullptr) {
                std::memcpy(
                    static_cast<std::byte*>(mappedTint),
                    &constants.pixel,
                    sizeof(constants.pixel));
            }
        }
    }

    void bindTextureIfNeeded(Diligent::ITextureView* srv) const {
        if (!m_expectsTexture || !m_shaderBinding || srv == nullptr) {
            return;
        }
        auto* variable = m_shaderBinding->GetVariableByName(Diligent::SHADER_TYPE_PIXEL, kTextureName.data());
        if (variable != nullptr) {
            variable->Set(srv);
        }
    }

    void commit(Diligent::IDeviceContext& context) const {
        if (m_shaderBinding) {
            context.CommitShaderResources(m_shaderBinding, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

private:
    Diligent::RefCntAutoPtr<Diligent::IPipelineState> m_pipelineState;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> m_shaderBinding;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> m_transformBuffer;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> m_tintBuffer;
    bool m_expectsTexture{false};
};

class DiligentUploadTicket final : public IUploadTicket {
public:
    DiligentUploadTicket(Diligent::IFence* fence, Diligent::IDeviceContext* graphics, std::uint64_t value) :
        m_fence(fence), m_graphics(graphics), m_value(value) {}
    ~DiligentUploadTicket() override {
        // Usually already complete. Covers discarded submissions as well.
        if (submitted) { wait(); }
    }
    bool isComplete() const override { return submitted && m_fence->GetCompletedValue() >= m_value; }
    std::uint64_t value() const override { return m_value; }
    void wait() const override { if (submitted) { m_fence->Wait(m_value); } }
    void acquire() override {
        ENGINE_PROFILE_ZONE("Upload Graphics Acquire");
        if (!isComplete()) { throw std::logic_error("Upload acquired before GPU completion"); }
        if (!m_acquired) {
            // CPU observation alone is not a Vulkan cross-queue memory dependency.
            // This is a GPU-side wait, not a CPU stall; next graphics use supplies
            // the normal COPY_DEST/COMMON -> shader/vertex/index state barrier.
            m_graphics->DeviceWaitForFence(m_fence, m_value);
            m_acquired = true;
        }
    }
    bool submitted{false}; // published with AsyncLoadQueue's release/acquire flag
    std::vector<Diligent::RefCntAutoPtr<Diligent::IDeviceObject>> destinations;
private:
    Diligent::RefCntAutoPtr<Diligent::IFence> m_fence;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> m_graphics;
    std::uint64_t m_value;
    bool m_acquired{false}; // renderer thread only
};

class DiligentUploadQueue final : public IUploadQueue {
public:
    DiligentUploadQueue(Diligent::IRenderDevice* device, Diligent::IDeviceContext* graphics,
                       Diligent::IDeviceContext* transfer) :
        m_device(device), m_graphics(graphics), m_transfer(transfer) {
        Diligent::FenceDesc desc;
        desc.Name = "TGE upload timeline";
        desc.Type = Diligent::FENCE_TYPE_GENERAL;
        m_device->CreateFence(desc, &m_fence);
        if (!m_fence) { throw std::runtime_error("Unable to create GPU upload fence"); }
    }
    ~DiligentUploadQueue() override { waitIdle(); }
    UploadSubmission submit(const UploadRequest& request) override {
        ENGINE_PROFILE_ZONE("Upload Transfer Submit");
        if (request.buffers.empty() && request.imageMips.empty()) {
            throw std::invalid_argument("Empty GPU upload batch");
        }
        // One immediate context must never be recorded concurrently by jobs.
        std::unique_lock lock(m_mutex);
        {
        ENGINE_PROFILE_ZONE("Upload Transfer Record");
        UploadSubmission result;
        auto ticket = std::make_shared<DiligentUploadTicket>(m_fence, m_graphics, m_value + 1U);
        ticket->destinations.reserve(request.buffers.size() + 1U);
        result.buffers.reserve(request.buffers.size());
        const auto mask = (Diligent::Uint64{1} << m_graphics->GetDesc().ContextId) |
                          (Diligent::Uint64{1} << m_transfer->GetDesc().ContextId);
        std::vector<Diligent::StateTransitionDesc> releaseBarriers;
        releaseBarriers.reserve(request.buffers.size() + 1U);
        const bool d3d12 = m_device->GetDeviceInfo().Type == Diligent::RENDER_DEVICE_TYPE_D3D12;
        try {
            for (const auto& upload : request.buffers) {
                if (upload.desc.dynamic || upload.desc.size == 0 || upload.data.size() != upload.desc.size ||
                    upload.desc.size % 4U != 0) {
                    throw std::invalid_argument("Upload expects a nonempty static, 4-byte-aligned buffer");
                }
                Diligent::BufferDesc desc;
                desc.Name = "TGE streamed mesh buffer";
                desc.Size = upload.desc.size;
                desc.Usage = Diligent::USAGE_DEFAULT;
                desc.ImmediateContextMask = mask;
                desc.BindFlags = upload.desc.usage == BufferUsage::Vertex ? Diligent::BIND_VERTEX_BUFFER :
                                 upload.desc.usage == BufferUsage::Index ? Diligent::BIND_INDEX_BUFFER :
                                 Diligent::BIND_UNIFORM_BUFFER;
                Diligent::RefCntAutoPtr<Diligent::IBuffer> buffer;
                m_device->CreateBuffer(desc, nullptr, &buffer);
                if (!buffer) { throw std::runtime_error("GPU upload buffer allocation failed"); }
                ticket->destinations.emplace_back(buffer.RawPtr());
                m_transfer->UpdateBuffer(buffer, 0, desc.Size, upload.data.data(), Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                if (d3d12) {
                    releaseBarriers.emplace_back(buffer, Diligent::RESOURCE_STATE_UNKNOWN,
                        Diligent::RESOURCE_STATE_COMMON, Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE);
                }
                result.buffers.push_back(std::make_unique<DiligentBuffer>(upload.desc, std::move(buffer)));
                result.bytes += upload.data.size();
            }
            if (!request.imageMips.empty()) {
                Diligent::TextureDesc desc;
                desc.Name = "TGE streamed texture";
                desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
                desc.Width = request.imageDesc.width;
                desc.Height = request.imageDesc.height;
                desc.Format = toDiligentFormat(request.imageDesc.format);
                desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
                desc.Usage = Diligent::USAGE_DEFAULT;
                desc.ImmediateContextMask = mask;
                desc.MipLevels = static_cast<Diligent::Uint32>(request.imageMips.size());
                auto width = desc.Width;
                auto height = desc.Height;
                std::size_t expectedLevels = 1;
                if (request.imageDesc.generateMipmaps) {
                    while (width > 1 || height > 1) {
                        width = std::max(1U, width / 2U); height = std::max(1U, height / 2U); ++expectedLevels;
                    }
                }
                if (desc.Width == 0 || desc.Height == 0 || expectedLevels != request.imageMips.size()) {
                    throw std::invalid_argument("Incomplete GPU texture mip chain");
                }
                Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
                m_device->CreateTexture(desc, nullptr, &texture);
                if (!texture) { throw std::runtime_error("GPU upload texture allocation failed"); }
                ticket->destinations.emplace_back(texture.RawPtr());
                width = desc.Width; height = desc.Height;
                for (Diligent::Uint32 mip = 0; mip < desc.MipLevels; ++mip) {
                    const auto& level = request.imageMips[mip];
                    if (level.width != width || level.height != height ||
                        level.pixels.size() != static_cast<std::uint64_t>(width) * height * 4U) {
                        throw std::invalid_argument("Invalid GPU texture mip dimensions/data");
                    }
                    Diligent::TextureSubResData data;
                    data.pData = level.pixels.data();
                    data.Stride = static_cast<Diligent::Uint64>(width) * 4U;
                    const Diligent::Box region{0, width, 0, height, 0, 1};
                    m_transfer->UpdateTexture(texture, mip, 0, region, data,
                        Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                    result.bytes += level.pixels.size();
                    width = std::max(1U, width / 2U); height = std::max(1U, height / 2U);
                }
                if (d3d12) {
                    releaseBarriers.emplace_back(texture, Diligent::RESOURCE_STATE_UNKNOWN,
                        Diligent::RESOURCE_STATE_COMMON, Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE);
                }
                auto* srv = texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
                if (!srv) { throw std::runtime_error("GPU texture SRV allocation failed"); }
                result.image = std::make_unique<DiligentImage>(request.imageDesc, std::move(texture),
                    Diligent::RefCntAutoPtr<Diligent::ITextureView>{srv});
            }
            // D3D12 requires COMMON when transferring ownership to graphics.
            // UpdateTexture temporarily transitions individual mips and restores
            // their previous state; unlike UpdateBuffer it does not leave the
            // tracked resource in COPY_DEST. Let Diligent use the tracked state.
            if (!releaseBarriers.empty()) {
                m_transfer->TransitionResourceStates(static_cast<Diligent::Uint32>(releaseBarriers.size()), releaseBarriers.data());
            }
            m_transfer->EnqueueSignal(m_fence, ++m_value);
            ticket->submitted = true;
            m_transfer->Flush();
            // Separate contexts are not retired by swapchain Present().
            m_transfer->FinishFrame();
            result.ticket = std::move(ticket);
            return result;
        } catch (...) {
            // Partial recording must not leave resources or a fence unsubmitted.
            m_transfer->WaitForIdle();
            m_transfer->FinishFrame();
            throw;
        }
        }
    }
    void waitIdle() override {
        ENGINE_PROFILE_ZONE("Upload Transfer Shutdown");
        std::lock_guard lock(m_mutex);
        m_transfer->WaitForIdle();
        m_transfer->FinishFrame();
    }
private:
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> m_device;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> m_graphics;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> m_transfer;
    Diligent::RefCntAutoPtr<Diligent::IFence> m_fence;
    std::mutex m_mutex;
    std::uint64_t m_value{0};
};

// Same mechanism as Diligent Tutorial23_CommandQueues. Never manufacture a
// "copy queue" from an ordinary graphics context when the adapter lacks one.
bool configureUploadContexts(Diligent::IEngineFactory* factory, Diligent::EngineCreateInfo& info,
                             std::array<Diligent::ImmediateContextCreateInfo, 2>& contexts) {
    Diligent::Uint32 count = 0;
    factory->EnumerateAdapters(info.GraphicsAPIVersion, count, nullptr);
    if (count == 0) { return false; }
    std::vector<Diligent::GraphicsAdapterInfo> adapters(count);
    factory->EnumerateAdapters(info.GraphicsAPIVersion, count, adapters.data());
    const auto adapterId = info.AdapterId == Diligent::DEFAULT_ADAPTER_ID ? 0U : info.AdapterId;
    if (adapterId >= count) { return false; }
    const auto& adapter = adapters[adapterId];
    int graphics = -1;
    int transfer = -1;
    for (Diligent::Uint32 q = 0; q < adapter.NumQueues; ++q) {
        if (adapter.Queues[q].MaxDeviceContexts == 0) { continue; }
        const auto type = adapter.Queues[q].QueueType & Diligent::COMMAND_QUEUE_TYPE_PRIMARY_MASK;
        if (type == Diligent::COMMAND_QUEUE_TYPE_GRAPHICS && graphics < 0) { graphics = static_cast<int>(q); }
        if (type == Diligent::COMMAND_QUEUE_TYPE_TRANSFER && transfer < 0) { transfer = static_cast<int>(q); }
    }
    if (graphics < 0 || transfer < 0) { return false; }
    contexts[0] = {"TGE Graphics", static_cast<Diligent::Uint8>(graphics)};
    contexts[1] = {"TGE Upload Transfer", static_cast<Diligent::Uint8>(transfer)};
    info.AdapterId = adapterId;
    info.pImmediateContextInfo = contexts.data();
    info.NumImmediateContexts = 2;
    info.Features.NativeFence = Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
    return true;
}

} // namespace

struct DiligentDevice::Impl {
    platform::Window* window{nullptr};

    Diligent::RENDER_DEVICE_TYPE deviceType{Diligent::RENDER_DEVICE_TYPE_UNDEFINED};
    Diligent::NativeWindow nativeWindow{};

    Diligent::IEngineFactory* baseFactory{nullptr};
    Diligent::IEngineFactoryD3D11* d3d11Factory{nullptr};
    Diligent::IEngineFactoryD3D12* d3d12Factory{nullptr};
    Diligent::IEngineFactoryVk* vkFactory{nullptr};
    Diligent::IEngineFactoryOpenGL* glFactory{nullptr};
    Diligent::IEngineFactoryWebGPU* webgpuFactory{nullptr};

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> renderDevice;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> immediateContext;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> transferContext;
    Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain;
    Diligent::RefCntAutoPtr<Diligent::IShaderSourceInputStreamFactory> shaderSourceFactory;

    std::string selectedDeviceName{"unknown"};
    std::unique_ptr<IUploadQueue> uploads; // destroyed before contexts/device
};

namespace {

Diligent::RENDER_DEVICE_TYPE chooseAutoDeviceType() {
    Diligent::RENDER_DEVICE_TYPE result = Diligent::RENDER_DEVICE_TYPE_UNDEFINED;

#if defined(_WIN32)
#    if D3D12_SUPPORTED
    result = Diligent::RENDER_DEVICE_TYPE_D3D12;
#    elif VULKAN_SUPPORTED
    result = Diligent::RENDER_DEVICE_TYPE_VULKAN;
#    elif D3D11_SUPPORTED
    result = Diligent::RENDER_DEVICE_TYPE_D3D11;
#    elif GL_SUPPORTED
    result = Diligent::RENDER_DEVICE_TYPE_GL;
#    elif WEBGPU_SUPPORTED
    result = Diligent::RENDER_DEVICE_TYPE_WEBGPU;
#    endif
#else
#    if VULKAN_SUPPORTED
    result = Diligent::RENDER_DEVICE_TYPE_VULKAN;
#    elif GL_SUPPORTED
    result = Diligent::RENDER_DEVICE_TYPE_GL;
#    elif WEBGPU_SUPPORTED
    result = Diligent::RENDER_DEVICE_TYPE_WEBGPU;
#    endif
#endif
    return result;
}

Diligent::RENDER_DEVICE_TYPE parseRequestedDeviceType(std::string value) {
    value = toLower(std::move(value));
    if (value.empty() || value == "auto") {
        return chooseAutoDeviceType();
    }
    if (value == "d3d12" || value == "dx12") {
        return Diligent::RENDER_DEVICE_TYPE_D3D12;
    }
    if (value == "d3d11" || value == "dx11") {
        return Diligent::RENDER_DEVICE_TYPE_D3D11;
    }
    if (value == "vk" || value == "vulkan") {
        return Diligent::RENDER_DEVICE_TYPE_VULKAN;
    }
    if (value == "gl" || value == "opengl" || value == "gles") {
        return Diligent::RENDER_DEVICE_TYPE_GL;
    }
    if (value == "webgpu" || value == "wgpu") {
        return Diligent::RENDER_DEVICE_TYPE_WEBGPU;
    }
    return chooseAutoDeviceType();
}

std::string deviceTypeToString(const Diligent::RENDER_DEVICE_TYPE deviceType) {
    switch (deviceType) {
        case Diligent::RENDER_DEVICE_TYPE_D3D11:
            return "d3d11";
        case Diligent::RENDER_DEVICE_TYPE_D3D12:
            return "d3d12";
        case Diligent::RENDER_DEVICE_TYPE_VULKAN:
            return "vk";
        case Diligent::RENDER_DEVICE_TYPE_GL:
            return "gl";
        case Diligent::RENDER_DEVICE_TYPE_WEBGPU:
            return "webgpu";
        default:
            return "unknown";
    }
}

Diligent::SwapChainDesc makeSwapChainDesc(const SwapchainDesc& desc) {
    Diligent::SwapChainDesc swapChainDesc{};
    swapChainDesc.Width = static_cast<Diligent::Uint32>(desc.width);
    swapChainDesc.Height = static_cast<Diligent::Uint32>(desc.height);
    swapChainDesc.ColorBufferFormat = Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    swapChainDesc.DepthBufferFormat = Diligent::TEX_FORMAT_D32_FLOAT;
    return swapChainDesc;
}

std::array<Diligent::LayoutElement, 2> getPositionColorLayout() {
    return {
        Diligent::LayoutElement{0U, 0U, 2U, Diligent::VT_FLOAT32, Diligent::False},
        Diligent::LayoutElement{1U, 0U, 3U, Diligent::VT_FLOAT32, Diligent::False}};
}

std::array<Diligent::LayoutElement, 3> getPositionNormalUvLayout() {
    return {
        Diligent::LayoutElement{0U, 0U, 3U, Diligent::VT_FLOAT32, Diligent::False},
        Diligent::LayoutElement{1U, 0U, 3U, Diligent::VT_FLOAT32, Diligent::False},
        Diligent::LayoutElement{2U, 0U, 2U, Diligent::VT_FLOAT32, Diligent::False}};
}

} // namespace

class DiligentSwapchain final : public ISwapchain {
public:
    DiligentSwapchain(DiligentDevice::Impl& impl, const SwapchainDesc& desc) : m_impl(&impl) {
        m_extent.width = desc.width;
        m_extent.height = desc.height;
        m_vsync = desc.vsync;

        if (m_impl != nullptr && m_impl->swapChain) {
            const auto& nativeDesc = m_impl->swapChain->GetDesc();
            m_extent.width = nativeDesc.Width;
            m_extent.height = nativeDesc.Height;
        }
    }

    std::uint32_t acquireNextImage(ISemaphore* imageAvailable) override {
        (void)imageAvailable;
        return 0;
    }

    void present(IQueue& queue, ISemaphore* renderFinished) override {
        (void)queue;
        (void)renderFinished;
        if (m_impl != nullptr && m_impl->swapChain) {
            m_impl->swapChain->Present(m_vsync ? 1U : 0U);
        }
    }

    void resize(const std::uint32_t width, const std::uint32_t height) override {
        if (m_impl == nullptr || !m_impl->swapChain || width == 0 || height == 0) {
            return;
        }

        m_impl->swapChain->Resize(
            static_cast<Diligent::Uint32>(width),
            static_cast<Diligent::Uint32>(height),
            Diligent::SURFACE_TRANSFORM_OPTIMAL);

        const auto& nativeDesc = m_impl->swapChain->GetDesc();
        m_extent.width = nativeDesc.Width;
        m_extent.height = nativeDesc.Height;
    }

    Extent2D extent() const override {
        return m_extent;
    }

private:
    DiligentDevice::Impl* m_impl{nullptr};
    Extent2D m_extent{};
    bool m_vsync{true};
};

class DiligentCommandBuffer final : public ICommandBuffer {
public:
    explicit DiligentCommandBuffer(DiligentDevice::Impl& impl) : m_impl(&impl) {}

    void begin() override {
        m_pipeline = nullptr;
        m_boundTexture = nullptr;
        m_pushConstantSize = 0;
    }

    void end() override {}

    void beginRenderPass(const RenderPassDesc& desc) override {
        if (m_impl == nullptr || !m_impl->immediateContext || !m_impl->swapChain) {
            return;
        }

        auto* rtv = m_impl->swapChain->GetCurrentBackBufferRTV();
        auto* dsv = m_impl->swapChain->GetDepthBufferDSV();

        if (rtv != nullptr) {
            m_impl->immediateContext->SetRenderTargets(
                1U,
                &rtv,
                dsv,
                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        } else {
            m_impl->immediateContext->SetRenderTargets(
                0U,
                nullptr,
                dsv,
                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }

        if (desc.clearColorTarget && rtv != nullptr) {
            const std::array<float, 4> clearColor{
                desc.clearColor.r,
                desc.clearColor.g,
                desc.clearColor.b,
                desc.clearColor.a};
            m_impl->immediateContext->ClearRenderTarget(
                rtv,
                clearColor.data(),
                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }

        if (dsv != nullptr) {
            m_impl->immediateContext->ClearDepthStencil(
                dsv,
                Diligent::CLEAR_DEPTH_FLAG,
                1.0F,
                0U,
                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

    void endRenderPass() override {}

    void setViewport(const Viewport& viewport) override {
        if (m_impl == nullptr || !m_impl->immediateContext) {
            return;
        }

        Diligent::Viewport nativeViewport{};
        nativeViewport.TopLeftX = viewport.x;
        nativeViewport.TopLeftY = viewport.y;
        nativeViewport.Width = viewport.width;
        nativeViewport.Height = viewport.height;
        nativeViewport.MinDepth = viewport.minDepth;
        nativeViewport.MaxDepth = viewport.maxDepth;
        m_impl->immediateContext->SetViewports(1U, &nativeViewport, 0U, 0U);
    }

    void setScissor(const Rect2D& scissor) override {
        if (m_impl == nullptr || !m_impl->immediateContext) {
            return;
        }

        Diligent::Rect rect{};
        rect.left = scissor.x;
        rect.top = scissor.y;
        rect.right = scissor.x + static_cast<Diligent::Int32>(scissor.width);
        rect.bottom = scissor.y + static_cast<Diligent::Int32>(scissor.height);
        m_impl->immediateContext->SetScissorRects(1U, &rect, 0U, 0U);
    }

    void bindPipeline(IGraphicsPipeline& pipeline) override {
        auto* diligentPipeline = dynamic_cast<DiligentGraphicsPipeline*>(&pipeline);
        if (diligentPipeline == nullptr || m_impl == nullptr || !m_impl->immediateContext) {
            return;
        }

        m_pipeline = diligentPipeline;
        m_impl->immediateContext->SetPipelineState(m_pipeline->pipelineState());
    }

    void bindVertexBuffer(IBuffer& buffer) override {
        auto* diligentBuffer = dynamic_cast<DiligentBuffer*>(&buffer);
        if (diligentBuffer == nullptr || m_impl == nullptr || !m_impl->immediateContext) {
            return;
        }

        const Diligent::Uint64 offset = 0U;
        Diligent::IBuffer* nativeBuffer[] = {diligentBuffer->nativeBuffer()};
        m_impl->immediateContext->SetVertexBuffers(
            0U,
            1U,
            nativeBuffer,
            &offset,
            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
            Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
    }

    void bindIndexBuffer(IBuffer& buffer) override {
        auto* diligentBuffer = dynamic_cast<DiligentBuffer*>(&buffer);
        if (diligentBuffer == nullptr || m_impl == nullptr || !m_impl->immediateContext) {
            return;
        }

        m_impl->immediateContext->SetIndexBuffer(
            diligentBuffer->nativeBuffer(),
            0U,
            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    void bindImage(const std::uint32_t slot, IImage& image) override {
        if (slot != 0U) {
            return;
        }

        m_boundTexture = dynamic_cast<DiligentImage*>(&image);
    }

    void bindBindGroup(const std::uint32_t slot, IBindGroup& bindGroup) override {
        (void)slot;
        (void)bindGroup;
    }

    void pushConstants(const void* data, const std::size_t size) override {
        if (data == nullptr || size == 0U) {
            m_pushConstantSize = 0U;
            return;
        }

        m_pushConstantSize = std::min(size, m_pushConstants.size());
        std::memcpy(m_pushConstants.data(), data, m_pushConstantSize);
    }

    void draw(const std::uint32_t vertexCount, const std::uint32_t firstVertex) override {
        if (m_impl == nullptr || !m_impl->immediateContext || m_pipeline == nullptr) {
            return;
        }

        applyPipelineResources();

        Diligent::DrawAttribs drawAttrs{};
        drawAttrs.NumVertices = vertexCount;
        drawAttrs.StartVertexLocation = firstVertex;
        drawAttrs.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        m_impl->immediateContext->Draw(drawAttrs);
    }

    void drawIndexed(const std::uint32_t indexCount, const std::uint32_t firstIndex) override {
        if (m_impl == nullptr || !m_impl->immediateContext || m_pipeline == nullptr) {
            return;
        }

        applyPipelineResources();

        Diligent::DrawIndexedAttribs drawAttrs{};
        drawAttrs.NumIndices = indexCount;
        drawAttrs.FirstIndexLocation = firstIndex;
        drawAttrs.IndexType = Diligent::VT_UINT32;
        drawAttrs.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
        m_impl->immediateContext->DrawIndexed(drawAttrs);
    }

    void barrier(const BarrierDesc& barrierDesc) override {
        (void)barrierDesc;
    }

private:
    void applyPipelineResources() {
        if (m_impl == nullptr || !m_impl->immediateContext || m_pipeline == nullptr) {
            return;
        }

        m_pipeline->updateConstants(
            *m_impl->immediateContext,
            m_pushConstantSize > 0U ? static_cast<const void*>(m_pushConstants.data()) : nullptr,
            m_pushConstantSize);

        m_pipeline->bindTextureIfNeeded(m_boundTexture != nullptr ? m_boundTexture->shaderResourceView() : nullptr);
        m_pipeline->commit(*m_impl->immediateContext);
    }

private:
    DiligentDevice::Impl* m_impl{nullptr};
    DiligentGraphicsPipeline* m_pipeline{nullptr};
    DiligentImage* m_boundTexture{nullptr};
    std::array<std::byte, sizeof(DiligentPushConstants)> m_pushConstants{};
    std::size_t m_pushConstantSize{0U};
};

DiligentGraphicsQueue::DiligentGraphicsQueue(DiligentDevice& device) : m_device(&device) {}

void DiligentGraphicsQueue::submit(
    ICommandBuffer& commandBuffer,
    ISemaphore* waitSemaphore,
    ISemaphore* signalSemaphore,
    IFence* signalFence) {
    (void)commandBuffer;
    (void)waitSemaphore;
    (void)signalSemaphore;

    if (m_device == nullptr || m_device->m_impl == nullptr || !m_device->m_impl->immediateContext) {
        return;
    }

    m_device->m_impl->immediateContext->Flush();
    if (signalFence != nullptr) {
        signalFence->signal();
    }
}

DiligentDevice::DiligentDevice(const DeviceCreateDesc& desc) : m_impl(std::make_unique<Impl>()), m_queue(*this) {
    if (desc.window == nullptr) {
        throw std::runtime_error("Diligent backend requires a valid window");
    }

    m_impl->window = desc.window;
    m_impl->deviceType = parseRequestedDeviceType(desc.diligentDeviceType);
    if (m_impl->deviceType == Diligent::RENDER_DEVICE_TYPE_UNDEFINED) {
        throw std::runtime_error("No supported Diligent backend is available on this platform/build");
    }

    fillNativeWindow(*desc.window, m_impl->nativeWindow);
    m_impl->selectedDeviceName = deviceTypeToString(m_impl->deviceType);

    ENGINE_LOG_INFO("Initializing Diligent backend with device '{}'", m_impl->selectedDeviceName);

    switch (m_impl->deviceType) {
#if D3D11_SUPPORTED
        case Diligent::RENDER_DEVICE_TYPE_D3D11: {
            m_impl->d3d11Factory = Diligent::LoadAndGetEngineFactoryD3D11();
            if (m_impl->d3d11Factory == nullptr) {
                throw std::runtime_error("Failed to get Diligent D3D11 engine factory");
            }

            m_impl->baseFactory = m_impl->d3d11Factory;
            Diligent::EngineD3D11CreateInfo createInfo{};
            configureCommonEngineOptions(createInfo, desc.enableValidation);
            m_impl->d3d11Factory->CreateDeviceAndContextsD3D11(createInfo, &m_impl->renderDevice, &m_impl->immediateContext);
            break;
        }
#endif
#if D3D12_SUPPORTED
        case Diligent::RENDER_DEVICE_TYPE_D3D12: {
            m_impl->d3d12Factory = Diligent::LoadAndGetEngineFactoryD3D12();
            if (m_impl->d3d12Factory == nullptr) {
                throw std::runtime_error("Failed to get Diligent D3D12 engine factory");
            }

            m_impl->baseFactory = m_impl->d3d12Factory;
            Diligent::EngineD3D12CreateInfo createInfo{};
            configureCommonEngineOptions(createInfo, desc.enableValidation);
            // Dynamic SRV/CBV descriptors are consumed for every draw and kept
            // until the GPU retires the frame. The SDK default (8192) exhausts
            // on the 4096-object scene. Reserve headroom for the supported
            // 16384-object lab scene and multiple frames in flight (~8 MiB).
            createInfo.GPUDescriptorHeapDynamicSize[0] = 262144U;
            std::array<Diligent::ImmediateContextCreateInfo, 2> contextInfo{};
            if (desc.enableUploadQueue && m_impl->d3d12Factory->LoadD3D12()) {
                // CreateDevice clamps an unspecified API version to 11.0;
                // EnumerateAdapters does not. Use the same effective minimum.
                createInfo.GraphicsAPIVersion = std::max(createInfo.GraphicsAPIVersion, Diligent::Version{11, 0});
                configureUploadContexts(m_impl->baseFactory, createInfo, contextInfo);
            }
            Diligent::IDeviceContext* contexts[2]{};
            m_impl->d3d12Factory->CreateDeviceAndContextsD3D12(createInfo, &m_impl->renderDevice, contexts);
            m_impl->immediateContext.Attach(contexts[0]);
            m_impl->transferContext.Attach(contexts[1]);
            break;
        }
#endif
#if VULKAN_SUPPORTED
        case Diligent::RENDER_DEVICE_TYPE_VULKAN: {
            m_impl->vkFactory = Diligent::LoadAndGetEngineFactoryVk();
            if (m_impl->vkFactory == nullptr) {
                throw std::runtime_error("Failed to get Diligent Vulkan engine factory");
            }

            m_impl->baseFactory = m_impl->vkFactory;
            Diligent::EngineVkCreateInfo createInfo{};
            configureCommonEngineOptions(createInfo, desc.enableValidation);
            std::array<Diligent::ImmediateContextCreateInfo, 2> contextInfo{};
            if (desc.enableUploadQueue) { configureUploadContexts(m_impl->baseFactory, createInfo, contextInfo); }
            Diligent::IDeviceContext* contexts[2]{};
            m_impl->vkFactory->CreateDeviceAndContextsVk(createInfo, &m_impl->renderDevice, contexts);
            m_impl->immediateContext.Attach(contexts[0]);
            m_impl->transferContext.Attach(contexts[1]);
            break;
        }
#endif
#if GL_SUPPORTED
        case Diligent::RENDER_DEVICE_TYPE_GL: {
            if (!m_impl->window->hasOpenGLContext()) {
                throw std::runtime_error(
                    "OpenGL mode requires a GLFW window created with OpenGL context (set diligentDevice=gl)");
            }

            m_impl->glFactory = Diligent::LoadAndGetEngineFactoryOpenGL();
            if (m_impl->glFactory == nullptr) {
                throw std::runtime_error("Failed to get Diligent OpenGL engine factory");
            }

            m_impl->baseFactory = m_impl->glFactory;

            Diligent::EngineGLCreateInfo createInfo{};
            configureCommonEngineOptions(createInfo, desc.enableValidation);
            createInfo.Window = m_impl->nativeWindow;

            Diligent::SwapChainDesc swapChainDesc{};
            swapChainDesc.Width = static_cast<Diligent::Uint32>(std::max(1, m_impl->window->width()));
            swapChainDesc.Height = static_cast<Diligent::Uint32>(std::max(1, m_impl->window->height()));

            m_impl->glFactory->CreateDeviceAndSwapChainGL(
                createInfo,
                &m_impl->renderDevice,
                &m_impl->immediateContext,
                swapChainDesc,
                &m_impl->swapChain);
            break;
        }
#endif
#if WEBGPU_SUPPORTED
        case Diligent::RENDER_DEVICE_TYPE_WEBGPU: {
            m_impl->webgpuFactory = Diligent::LoadAndGetEngineFactoryWebGPU();
            if (m_impl->webgpuFactory == nullptr) {
                throw std::runtime_error("Failed to get Diligent WebGPU engine factory");
            }

            m_impl->baseFactory = m_impl->webgpuFactory;
            Diligent::EngineWebGPUCreateInfo createInfo{};
            configureCommonEngineOptions(createInfo, desc.enableValidation);
            m_impl->webgpuFactory->CreateDeviceAndContextsWebGPU(
                createInfo,
                &m_impl->renderDevice,
                &m_impl->immediateContext);
            break;
        }
#endif
        default:
            throw std::runtime_error("Requested Diligent device type is not supported by this build");
    }

    if (!m_impl->renderDevice || !m_impl->immediateContext) {
        throw std::runtime_error("Diligent failed to create render device or immediate context");
    }
    if (m_impl->transferContext) {
        m_impl->uploads = std::make_unique<DiligentUploadQueue>(m_impl->renderDevice,
            m_impl->immediateContext, m_impl->transferContext);
        ENGINE_LOG_INFO("GPU_UPLOAD_QUEUE enabled backend={} graphics_context={} transfer_context={}",
            m_impl->selectedDeviceName, m_impl->immediateContext->GetDesc().ContextId,
            m_impl->transferContext->GetDesc().ContextId);
    } else {
        ENGINE_LOG_INFO("GPU_UPLOAD_QUEUE disabled backend={} requested={} (bounded main-thread fallback)",
            m_impl->selectedDeviceName, desc.enableUploadQueue);
    }
}

DiligentDevice::~DiligentDevice() = default;

IUploadQueue* DiligentDevice::uploadQueue() { return m_impl->uploads.get(); }

std::unique_ptr<ISwapchain> DiligentDevice::createSwapchain(const SwapchainDesc& desc) {
    if (!m_impl || !m_impl->renderDevice || !m_impl->immediateContext || desc.width == 0 || desc.height == 0) {
        return nullptr;
    }

    const auto nativeDesc = makeSwapChainDesc(desc);

    if (!m_impl->swapChain) {
        switch (m_impl->deviceType) {
#if D3D11_SUPPORTED
            case Diligent::RENDER_DEVICE_TYPE_D3D11: {
                Diligent::FullScreenModeDesc fsDesc{};
                m_impl->d3d11Factory->CreateSwapChainD3D11(
                    m_impl->renderDevice,
                    m_impl->immediateContext,
                    nativeDesc,
                    fsDesc,
                    m_impl->nativeWindow,
                    &m_impl->swapChain);
                break;
            }
#endif
#if D3D12_SUPPORTED
            case Diligent::RENDER_DEVICE_TYPE_D3D12: {
                Diligent::FullScreenModeDesc fsDesc{};
                m_impl->d3d12Factory->CreateSwapChainD3D12(
                    m_impl->renderDevice,
                    m_impl->immediateContext,
                    nativeDesc,
                    fsDesc,
                    m_impl->nativeWindow,
                    &m_impl->swapChain);
                break;
            }
#endif
#if VULKAN_SUPPORTED
            case Diligent::RENDER_DEVICE_TYPE_VULKAN: {
                m_impl->vkFactory->CreateSwapChainVk(
                    m_impl->renderDevice,
                    m_impl->immediateContext,
                    nativeDesc,
                    m_impl->nativeWindow,
                    &m_impl->swapChain);
                break;
            }
#endif
#if WEBGPU_SUPPORTED
            case Diligent::RENDER_DEVICE_TYPE_WEBGPU: {
                m_impl->webgpuFactory->CreateSwapChainWebGPU(
                    m_impl->renderDevice,
                    m_impl->immediateContext,
                    nativeDesc,
                    m_impl->nativeWindow,
                    &m_impl->swapChain);
                break;
            }
#endif
            case Diligent::RENDER_DEVICE_TYPE_GL:
                break;
            default:
                break;
        }
    } else {
        m_impl->swapChain->Resize(
            static_cast<Diligent::Uint32>(desc.width),
            static_cast<Diligent::Uint32>(desc.height),
            Diligent::SURFACE_TRANSFORM_OPTIMAL);
    }

    if (!m_impl->swapChain) {
        ENGINE_LOG_ERROR("Diligent failed to create swapchain for '{}'", m_impl->selectedDeviceName);
        return nullptr;
    }

    if (m_impl->baseFactory) {
        m_impl->baseFactory->CreateDefaultShaderSourceStreamFactory(".", &m_impl->shaderSourceFactory);
    }

    return std::make_unique<DiligentSwapchain>(*m_impl, desc);
}

std::unique_ptr<ICommandBuffer> DiligentDevice::createCommandBuffer() {
    if (!m_impl || !m_impl->immediateContext) {
        return nullptr;
    }
    return std::make_unique<DiligentCommandBuffer>(*m_impl);
}

std::unique_ptr<IBuffer> DiligentDevice::createBuffer(const BufferDesc& desc, const void* initialData) {
    if (!m_impl || !m_impl->renderDevice || desc.size == 0U) {
        return nullptr;
    }

    Diligent::BufferDesc nativeDesc{};
    nativeDesc.Size = static_cast<Diligent::Uint64>(desc.size);
    nativeDesc.Usage = (desc.dynamic ? Diligent::USAGE_DYNAMIC : (initialData ? Diligent::USAGE_IMMUTABLE : Diligent::USAGE_DEFAULT));
    nativeDesc.CPUAccessFlags = desc.dynamic ? Diligent::CPU_ACCESS_WRITE : Diligent::CPU_ACCESS_NONE;

    switch (desc.usage) {
        case BufferUsage::Vertex:
            nativeDesc.BindFlags = Diligent::BIND_VERTEX_BUFFER;
            break;
        case BufferUsage::Index:
            nativeDesc.BindFlags = Diligent::BIND_INDEX_BUFFER;
            break;
        case BufferUsage::Uniform:
            nativeDesc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
            break;
        default:
            nativeDesc.BindFlags = Diligent::BIND_NONE;
            break;
    }

    Diligent::BufferData nativeData{};
    Diligent::BufferData* nativeDataPtr = nullptr;
    if (initialData != nullptr) {
        nativeData.pData = initialData;
        nativeData.DataSize = nativeDesc.Size;
        nativeDataPtr = &nativeData;
    }

    Diligent::RefCntAutoPtr<Diligent::IBuffer> nativeBuffer;
    m_impl->renderDevice->CreateBuffer(nativeDesc, nativeDataPtr, &nativeBuffer);
    if (!nativeBuffer) {
        return nullptr;
    }

    return std::make_unique<DiligentBuffer>(desc, std::move(nativeBuffer));
}

std::unique_ptr<IImage> DiligentDevice::createImage(const ImageDesc& desc, const void* initialData) {
    if (!m_impl || !m_impl->renderDevice || desc.width == 0U || desc.height == 0U) {
        return nullptr;
    }

    const bool buildMipChain = desc.generateMipmaps;

    Diligent::TextureDesc nativeDesc{};
    nativeDesc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    nativeDesc.Width = static_cast<Diligent::Uint32>(desc.width);
    nativeDesc.Height = static_cast<Diligent::Uint32>(desc.height);
    nativeDesc.Format = toDiligentFormat(desc.format);
    nativeDesc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    nativeDesc.Usage = Diligent::USAGE_DEFAULT;
    nativeDesc.MipLevels = buildMipChain ? 0U : 1U;

    if (buildMipChain) {
        nativeDesc.BindFlags |= Diligent::BIND_RENDER_TARGET;
        nativeDesc.MiscFlags |= Diligent::MISC_TEXTURE_FLAG_GENERATE_MIPS;
    }

    Diligent::TextureSubResData subresource{};
    Diligent::TextureData textureData{};
    Diligent::TextureData* textureDataPtr = nullptr;
    if (initialData != nullptr && !buildMipChain) {
        subresource.pData = initialData;
        subresource.Stride = static_cast<Diligent::Uint64>(desc.width) * 4U;
        textureData.pSubResources = &subresource;
        textureData.NumSubresources = 1U;
        textureDataPtr = &textureData;
    }

    Diligent::RefCntAutoPtr<Diligent::ITexture> nativeTexture;
    m_impl->renderDevice->CreateTexture(nativeDesc, textureDataPtr, &nativeTexture);
    if (!nativeTexture) {
        return nullptr;
    }

    auto* srv = nativeTexture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (buildMipChain && initialData != nullptr && m_impl->immediateContext != nullptr) {
        Diligent::TextureSubResData topMip{};
        topMip.pData = initialData;
        topMip.Stride = static_cast<Diligent::Uint64>(desc.width) * 4U;

        Diligent::Box updateRegion{};
        updateRegion.MinX = 0U;
        updateRegion.MaxX = static_cast<Diligent::Uint32>(desc.width);
        updateRegion.MinY = 0U;
        updateRegion.MaxY = static_cast<Diligent::Uint32>(desc.height);
        updateRegion.MinZ = 0U;
        updateRegion.MaxZ = 1U;

        m_impl->immediateContext->UpdateTexture(
            nativeTexture,
            0U,
            0U,
            updateRegion,
            topMip,
            Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE,
            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        if (srv != nullptr) {
            m_impl->immediateContext->GenerateMips(srv);
        }
    }

    return std::make_unique<DiligentImage>(
        desc,
        std::move(nativeTexture),
        Diligent::RefCntAutoPtr<Diligent::ITextureView>{srv});
}

std::unique_ptr<IShaderModule> DiligentDevice::createShaderModule(const ShaderModuleDesc& desc) {
    if (!m_impl || !m_impl->renderDevice) {
        return nullptr;
    }

    Diligent::ShaderCreateInfo createInfo{};
    createInfo.FilePath = desc.path.c_str();
    createInfo.EntryPoint = desc.entryPoint.c_str();
    createInfo.Desc.ShaderType = toDiligentShaderType(desc.stage);
    createInfo.Desc.Name = desc.path.c_str();
    createInfo.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    createInfo.Desc.UseCombinedTextureSamplers = Diligent::True;
    createInfo.pShaderSourceStreamFactory = m_impl->shaderSourceFactory;

    Diligent::RefCntAutoPtr<Diligent::IShader> nativeShader;
    m_impl->renderDevice->CreateShader(createInfo, &nativeShader);
    if (!nativeShader) {
        ENGINE_LOG_ERROR("Diligent shader creation failed for '{}'", desc.path);
        return nullptr;
    }

    std::optional<VertexLayout> inferredLayout;
    if (desc.stage == ShaderStage::Vertex) {
        inferredLayout = inferVertexLayout(desc.path);
    }

    const bool usesTexture = shaderUsesTexture(desc.path);
    return std::make_unique<DiligentShaderModule>(desc, std::move(nativeShader), inferredLayout, usesTexture);
}

std::unique_ptr<IPipelineLayout> DiligentDevice::createPipelineLayout() {
    return std::make_unique<DiligentPipelineLayout>();
}

std::unique_ptr<IGraphicsPipeline> DiligentDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto* vertexShader = dynamic_cast<DiligentShaderModule*>(desc.vertexShader);
    auto* fragmentShader = dynamic_cast<DiligentShaderModule*>(desc.fragmentShader);
    if (vertexShader == nullptr || fragmentShader == nullptr || !m_impl || !m_impl->renderDevice || !m_impl->swapChain) {
        return nullptr;
    }

    const VertexLayout vertexLayout = vertexShader->inferredLayout().value_or(VertexLayout::Position3Normal3Uv2);

    Diligent::GraphicsPipelineStateCreateInfo createInfo{};
    createInfo.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;
    createInfo.PSODesc.Name = "TurboGameEngine PSO";
    createInfo.GraphicsPipeline.NumRenderTargets = 1U;
    createInfo.GraphicsPipeline.RTVFormats[0] = m_impl->swapChain->GetDesc().ColorBufferFormat;
    createInfo.GraphicsPipeline.DSVFormat = m_impl->swapChain->GetDesc().DepthBufferFormat;
    createInfo.GraphicsPipeline.PrimitiveTopology = toDiligentTopology(desc.topology);

    const bool isMeshPipeline = vertexLayout == VertexLayout::Position3Normal3Uv2;
    createInfo.GraphicsPipeline.RasterizerDesc.CullMode =
        isMeshPipeline ? Diligent::CULL_MODE_BACK : Diligent::CULL_MODE_NONE;
    createInfo.GraphicsPipeline.RasterizerDesc.FrontCounterClockwise = Diligent::True;
    createInfo.GraphicsPipeline.DepthStencilDesc.DepthEnable = isMeshPipeline ? Diligent::True : Diligent::False;
    createInfo.GraphicsPipeline.DepthStencilDesc.DepthWriteEnable =
        isMeshPipeline ? Diligent::True : Diligent::False;
    createInfo.GraphicsPipeline.DepthStencilDesc.DepthFunc = Diligent::COMPARISON_FUNC_LESS_EQUAL;
    createInfo.pVS = vertexShader->nativeShader();
    createInfo.pPS = fragmentShader->nativeShader();

    const auto positionColorLayout = getPositionColorLayout();
    const auto positionNormalUvLayout = getPositionNormalUvLayout();
    if (vertexLayout == VertexLayout::Position2Color3) {
        createInfo.GraphicsPipeline.InputLayout.LayoutElements = positionColorLayout.data();
        createInfo.GraphicsPipeline.InputLayout.NumElements = static_cast<Diligent::Uint32>(positionColorLayout.size());
    } else {
        createInfo.GraphicsPipeline.InputLayout.LayoutElements = positionNormalUvLayout.data();
        createInfo.GraphicsPipeline.InputLayout.NumElements =
            static_cast<Diligent::Uint32>(positionNormalUvLayout.size());
    }

    createInfo.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

    const bool expectsTexture = fragmentShader->usesTexture();
    Diligent::ShaderResourceVariableDesc variableDesc{};
    Diligent::ImmutableSamplerDesc immutableSampler{};
    if (expectsTexture) {
        variableDesc.ShaderStages = Diligent::SHADER_TYPE_PIXEL;
        variableDesc.Name = kTextureName.data();
        variableDesc.Type = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        createInfo.PSODesc.ResourceLayout.Variables = &variableDesc;
        createInfo.PSODesc.ResourceLayout.NumVariables = 1U;

        Diligent::SamplerDesc samplerDesc{};
        samplerDesc.MinFilter = Diligent::FILTER_TYPE_LINEAR;
        samplerDesc.MagFilter = Diligent::FILTER_TYPE_LINEAR;
        samplerDesc.MipFilter = Diligent::FILTER_TYPE_LINEAR;
        samplerDesc.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressV = Diligent::TEXTURE_ADDRESS_WRAP;
        samplerDesc.AddressW = Diligent::TEXTURE_ADDRESS_WRAP;
        immutableSampler.ShaderStages = Diligent::SHADER_TYPE_PIXEL;
        immutableSampler.SamplerOrTextureName = kTextureName.data();
        immutableSampler.Desc = samplerDesc;

        createInfo.PSODesc.ResourceLayout.ImmutableSamplers = &immutableSampler;
        createInfo.PSODesc.ResourceLayout.NumImmutableSamplers = 1U;
    }

    Diligent::RefCntAutoPtr<Diligent::IPipelineState> pipelineState;
    m_impl->renderDevice->CreateGraphicsPipelineState(createInfo, &pipelineState);
    if (!pipelineState) {
        return nullptr;
    }

    Diligent::BufferDesc transformDesc{};
    transformDesc.Name = "Turbo VS Constants";
    transformDesc.Size = sizeof(DiligentVertexConstants);
    transformDesc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    transformDesc.Usage = Diligent::USAGE_DYNAMIC;
    transformDesc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;

    Diligent::BufferDesc tintDesc{};
    tintDesc.Name = "Turbo PS Constants";
    tintDesc.Size = sizeof(DiligentPixelConstants);
    tintDesc.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
    tintDesc.Usage = Diligent::USAGE_DYNAMIC;
    tintDesc.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;

    Diligent::RefCntAutoPtr<Diligent::IBuffer> transformBuffer;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> tintBuffer;
    m_impl->renderDevice->CreateBuffer(transformDesc, nullptr, &transformBuffer);
    m_impl->renderDevice->CreateBuffer(tintDesc, nullptr, &tintBuffer);
    if (!transformBuffer || !tintBuffer) {
        return nullptr;
    }

    if (auto* var = pipelineState->GetStaticVariableByName(Diligent::SHADER_TYPE_VERTEX, kVsConstantsName.data())) {
        var->Set(transformBuffer);
    }
    if (auto* var = pipelineState->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, kPsConstantsName.data())) {
        var->Set(tintBuffer);
    }

    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> shaderBinding;
    pipelineState->CreateShaderResourceBinding(&shaderBinding, true);

    return std::make_unique<DiligentGraphicsPipeline>(
        std::move(pipelineState),
        std::move(shaderBinding),
        std::move(transformBuffer),
        std::move(tintBuffer),
        expectsTexture);
}

std::unique_ptr<IBindGroupLayout> DiligentDevice::createBindGroupLayout() {
    return std::make_unique<DiligentBindGroupLayout>();
}

std::unique_ptr<IBindGroup> DiligentDevice::createBindGroup() {
    return std::make_unique<DiligentBindGroup>();
}

std::unique_ptr<ISemaphore> DiligentDevice::createSemaphore() {
    return std::make_unique<DiligentSemaphore>();
}

std::unique_ptr<IFence> DiligentDevice::createFence(const bool signaled) {
    return std::make_unique<DiligentFence>(signaled);
}

IQueue& DiligentDevice::graphicsQueue() {
    return m_queue;
}

BackendType DiligentDevice::backendType() const {
    return BackendType::Diligent;
}

Diligent::IRenderDevice* DiligentDevice::nativeRenderDevice() const {
    if (!m_impl) {
        return nullptr;
    }
    return m_impl->renderDevice;
}

Diligent::IDeviceContext* DiligentDevice::nativeImmediateContext() const {
    if (!m_impl) {
        return nullptr;
    }
    return m_impl->immediateContext;
}

Diligent::ISwapChain* DiligentDevice::nativeSwapChain() const {
    if (!m_impl) {
        return nullptr;
    }
    return m_impl->swapChain;
}

std::unique_ptr<IDevice> createDiligentDevice(const DeviceCreateDesc& desc, std::string* errorMessage) {
    try {
        return std::make_unique<DiligentDevice>(desc);
    } catch (const std::exception& ex) {
        if (errorMessage != nullptr) {
            *errorMessage = ex.what();
        }
        return nullptr;
    }
}

} // namespace engine::rhi::diligent
