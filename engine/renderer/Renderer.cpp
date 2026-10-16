#include "engine/renderer/Renderer.h"
#include "engine/renderer/UiCanvasCompositor.h"
#include "engine/editor/EditorTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/core/Profiling.h"
#include "engine/platform/Events.h"
#include "engine/platform/Window.h"
#include "engine/rhi_diligent/DiligentDevice.h"
#include "engine/resources/loaders.h"
#include "engine/resources/image_mips.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/GeometryPrimitives.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "third_party/DiligentEngine/DiligentTools/Imgui/interface/ImGuiImplDiligent.hpp"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

#ifndef ENGINE_SOURCE_ROOT
#    define ENGINE_SOURCE_ROOT ""
#endif

namespace engine::renderer {

namespace {

std::string utf8Path(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

bool containsPath(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::filesystem::path engineAssetsDirectory() {
    const auto source = std::filesystem::u8path(ENGINE_SOURCE_ROOT);
    std::error_code error;
    if (!source.empty() && source.is_absolute() && std::filesystem::is_directory(source / "assets", error)) {
        return std::filesystem::weakly_canonical(source / "assets");
    }
    std::filesystem::path executable;
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        executable = std::filesystem::path(buffer);
    }
#elif defined(__linux__)
    executable = std::filesystem::read_symlink("/proc/self/exe", error);
#elif defined(__APPLE__)
    std::uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::string buffer(length, '\0');
    if (_NSGetExecutablePath(buffer.data(), &length) == 0) { executable = buffer.c_str(); }
#endif
    if (executable.empty()) { throw std::runtime_error("Cannot locate engine assets beside the executable"); }
    return std::filesystem::weakly_canonical(executable.parent_path() / "assets");
}

bool isBuiltinAsset(const std::filesystem::path& relative) {
    constexpr std::array<std::string_view, 10> names{
        "shaders_hlsl/basic.vert.hlsl", "shaders_hlsl/basic.frag.hlsl", "shaders_hlsl/basic.hlsl",
        "shaders_hlsl/textured.vert.hlsl", "shaders_hlsl/textured.frag.hlsl", "shaders_hlsl/textured.shader.json",
        "textures/arena_floor.ppm", "textures/arena_wall.ppm", "textures/DGLogo.png", "textures/checker.png"};
    const auto name = utf8Path(relative);
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool isProceduralResource(const std::string& id) {
    return id.starts_with("builtin:") || id == "__diligent_cube__" || id == "__procedural_pyramid__" ||
        id == "__diligent_sphere__";
}

struct PrimitiveVertex final {
    glm::vec2 position;
    glm::vec3 color;
};

struct DrawPushConstants final {
    glm::mat4 modelViewProjection{1.0F};
    glm::mat4 model{1.0F};
    glm::vec4 uvScaleBias{1.0F, 1.0F, 0.0F, 0.0F};
    glm::vec4 tint{1.0F, 1.0F, 1.0F, 1.0F};
    glm::vec4 lightDirection{-0.45F, -0.8F, 0.35F, 0.0F};
    glm::vec4 ambientColor{0.47F, 0.5F, 0.58F, 0.18F};
};

constexpr std::array<PrimitiveVertex, 3> kTriangleVertices{
    PrimitiveVertex{glm::vec2(0.0F, 0.35F), glm::vec3(1.0F, 0.2F, 0.2F)},
    PrimitiveVertex{glm::vec2(-0.30F, -0.25F), glm::vec3(0.2F, 1.0F, 0.3F)},
    PrimitiveVertex{glm::vec2(0.30F, -0.25F), glm::vec3(0.2F, 0.4F, 1.0F)},
};

std::filesystem::file_time_type getFileWriteTimeOrMin(const std::string& path) {
    if (path.empty()) {
        return std::filesystem::file_time_type::min();
    }

    std::error_code ec;
    const auto nativePath = std::filesystem::u8path(path);
    if (!std::filesystem::exists(nativePath, ec) || ec) {
        return std::filesystem::file_time_type::min();
    }

    const auto writeTime = std::filesystem::last_write_time(nativePath, ec);
    if (ec) {
        return std::filesystem::file_time_type::min();
    }
    return writeTime;
}

bool shaderTimestampsMatch(
    const std::filesystem::file_time_type descriptorTimeA,
    const std::filesystem::file_time_type vertexTimeA,
    const std::filesystem::file_time_type fragmentTimeA,
    const std::filesystem::file_time_type descriptorTimeB,
    const std::filesystem::file_time_type vertexTimeB,
    const std::filesystem::file_time_type fragmentTimeB) {
    return descriptorTimeA == descriptorTimeB && vertexTimeA == vertexTimeB && fragmentTimeA == fragmentTimeB;
}

std::optional<ImGuiKey> toImGuiKey(const platform::KeyCode key) {
    switch (key) {
        case platform::KeyCode::Tab:
            return ImGuiKey_Tab;
        case platform::KeyCode::Backspace:
            return ImGuiKey_Backspace;
        case platform::KeyCode::Delete:
            return ImGuiKey_Delete;
        case platform::KeyCode::Home:
            return ImGuiKey_Home;
        case platform::KeyCode::End:
            return ImGuiKey_End;
        case platform::KeyCode::Enter:
            return ImGuiKey_Enter;
        case platform::KeyCode::Escape:
            return ImGuiKey_Escape;
        case platform::KeyCode::W:
            return ImGuiKey_W;
        case platform::KeyCode::A:
            return ImGuiKey_A;
        case platform::KeyCode::C:
            return ImGuiKey_C;
        case platform::KeyCode::V:
            return ImGuiKey_V;
        case platform::KeyCode::X:
            return ImGuiKey_X;
        case platform::KeyCode::S:
            return ImGuiKey_S;
        case platform::KeyCode::D:
            return ImGuiKey_D;
        case platform::KeyCode::Q:
            return ImGuiKey_Q;
        case platform::KeyCode::E:
            return ImGuiKey_E;
        case platform::KeyCode::F:
            return ImGuiKey_F;
        case platform::KeyCode::N:
            return ImGuiKey_N;
        case platform::KeyCode::O:
            return ImGuiKey_O;
        case platform::KeyCode::R:
            return ImGuiKey_R;
        case platform::KeyCode::Y:
            return ImGuiKey_Y;
        case platform::KeyCode::Z:
            return ImGuiKey_Z;
        case platform::KeyCode::Left:
            return ImGuiKey_LeftArrow;
        case platform::KeyCode::Right:
            return ImGuiKey_RightArrow;
        case platform::KeyCode::Up:
            return ImGuiKey_UpArrow;
        case platform::KeyCode::Down:
            return ImGuiKey_DownArrow;
        case platform::KeyCode::LeftShift:
            return ImGuiKey_LeftShift;
        case platform::KeyCode::RightShift:
            return ImGuiKey_RightShift;
        case platform::KeyCode::LeftControl:
            return ImGuiKey_LeftCtrl;
        case platform::KeyCode::RightControl:
            return ImGuiKey_RightCtrl;
        case platform::KeyCode::LeftAlt:
            return ImGuiKey_LeftAlt;
        case platform::KeyCode::RightAlt:
            return ImGuiKey_RightAlt;
        case platform::KeyCode::Space:
            return ImGuiKey_Space;
        default:
            return std::nullopt;
    }
}

void addImGuiModifierEvent(ImGuiIO& io, const platform::KeyCode key, const bool down) {
    switch (key) {
        case platform::KeyCode::LeftShift:
        case platform::KeyCode::RightShift:
            io.AddKeyEvent(ImGuiMod_Shift, down);
            break;
        case platform::KeyCode::LeftControl:
        case platform::KeyCode::RightControl:
            io.AddKeyEvent(ImGuiMod_Ctrl, down);
            break;
        case platform::KeyCode::LeftAlt:
        case platform::KeyCode::RightAlt:
            io.AddKeyEvent(ImGuiMod_Alt, down);
            break;
        default:
            break;
    }
}

int toImGuiMouseButton(const platform::MouseButton button) {
    switch (button) {
        case platform::MouseButton::Left:
            return 0;
        case platform::MouseButton::Right:
            return 1;
        case platform::MouseButton::Middle:
        default:
            return 2;
    }
}

constexpr const char* kProceduralCubeMeshId = "__diligent_cube__";
constexpr const char* kProceduralPyramidMeshId = "__procedural_pyramid__";
constexpr const char* kProceduralSphereMeshId = "__diligent_sphere__";

bool populateMeshDataFromGeometryPrimitive(
    const Diligent::GeometryPrimitiveAttributes& attributes,
    resources::MeshData& meshData) {
    Diligent::RefCntAutoPtr<Diligent::IDataBlob> vertexDataBlob;
    Diligent::RefCntAutoPtr<Diligent::IDataBlob> indexDataBlob;
    Diligent::GeometryPrimitiveInfo primitiveInfo{};
    Diligent::CreateGeometryPrimitive(attributes, &vertexDataBlob, &indexDataBlob, &primitiveInfo);

    const std::uint32_t expectedVertexSize =
        Diligent::GetGeometryPrimitiveVertexSize(Diligent::GEOMETRY_PRIMITIVE_VERTEX_FLAG_ALL);
    if (!vertexDataBlob || primitiveInfo.NumVertices == 0U || primitiveInfo.VertexSize != expectedVertexSize) {
        return false;
    }

    meshData.vertices.clear();
    meshData.indices.clear();
    meshData.vertices.resize(primitiveInfo.NumVertices);

    const auto* sourceVertices = vertexDataBlob->GetConstDataPtr<std::uint8_t>();
    for (std::uint32_t vertexIndex = 0; vertexIndex < primitiveInfo.NumVertices; ++vertexIndex) {
        const auto* vertexPtr = sourceVertices + vertexIndex * primitiveInfo.VertexSize;
        const auto* floats = reinterpret_cast<const float*>(vertexPtr);
        meshData.vertices[vertexIndex] = resources::MeshVertex{
            glm::vec3(floats[0], floats[1], floats[2]),
            glm::vec3(floats[3], floats[4], floats[5]),
            glm::vec2(floats[6], floats[7])};
    }

    if (indexDataBlob && primitiveInfo.NumIndices > 0U) {
        const auto* sourceIndices = indexDataBlob->GetConstDataPtr<std::uint32_t>();
        meshData.indices.assign(sourceIndices, sourceIndices + primitiveInfo.NumIndices);
    }

    return true;
}

} // namespace

void ImGuiImplDiligentDeleter::operator()(Diligent::ImGuiImplDiligent* ptr) const {
    delete ptr;
}

Renderer::Renderer() : m_engineAssetRoot(engineAssetsDirectory()) {}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::init(
    platform::Window& window,
    rhi::BackendType backend,
    bool vsync,
    const std::string& diligentDeviceType) {
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
    deviceDesc.diligentDeviceType = diligentDeviceType;
    deviceDesc.enableUploadQueue = m_gpuUploadEnabled;

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
    m_assetLoads.start(m_jobSystem);

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

    if (!initializePrimitivePipeline()) {
        ENGINE_LOG_ERROR("Failed to initialize primitive rendering pipeline");
        return false;
    }

    if (!initializeResourceSubsystem()) {
        ENGINE_LOG_ERROR("Failed to initialize resource subsystem");
        return false;
    }

    if (!initializeImGui()) {
        ENGINE_LOG_WARN("ImGui layer is disabled");
    }

    m_initialized = true;
    return true;
}

void Renderer::setJobSystem(core::JobSystem* jobs) {
    if (m_initialized) {
        throw std::logic_error("Configure Renderer job system before initialization");
    }
    m_jobSystem = jobs;
}

void Renderer::setAsyncLoadingEnabled(const bool enabled) {
    m_assetLoads.setAsyncEnabled(enabled);
}

void Renderer::setGpuUploadEnabled(const bool enabled) {
    if (m_device) { throw std::logic_error("Configure GPU upload queue before Renderer::init"); }
    m_gpuUploadEnabled = enabled;
}

void Renderer::setUploadBudget(const std::size_t maximumUploads, const double milliseconds) {
    m_assetLoads.setUploadBudget(maximumUploads, milliseconds);
}

void Renderer::setAssetInFlightLimit(const std::size_t maximumAssets) {
    m_assetLoads.setInFlightLimit(maximumAssets);
}

AssetLoadingStats Renderer::assetLoadingStats() const {
    auto result = m_assetLoads.stats();
    result.gpuUploadEnabled = result.asyncEnabled && m_device && m_device->uploadQueue();
    return result;
}

bool Renderer::initializePrimitivePipeline() {
    rhi::BufferDesc vertexBufferDesc{};
    vertexBufferDesc.size = sizeof(kTriangleVertices);
    vertexBufferDesc.usage = rhi::BufferUsage::Vertex;
    vertexBufferDesc.vertexLayout = rhi::VertexLayout::Position2Color3;
    m_primitiveVertexBuffer = m_device->createBuffer(vertexBufferDesc, kTriangleVertices.data());
    if (!m_primitiveVertexBuffer) {
        ENGINE_LOG_ERROR("Failed to create primitive vertex buffer");
        return false;
    }

    const std::string vertexPath = utf8Path(m_engineAssetRoot / "shaders_hlsl/basic.vert.hlsl");
    const std::string fragmentPath = utf8Path(m_engineAssetRoot / "shaders_hlsl/basic.frag.hlsl");

    rhi::ShaderModuleDesc vertexShaderDesc{};
    vertexShaderDesc.stage = rhi::ShaderStage::Vertex;
    vertexShaderDesc.path = vertexPath;
    m_primitiveVertexShader = m_device->createShaderModule(vertexShaderDesc);

    rhi::ShaderModuleDesc fragmentShaderDesc{};
    fragmentShaderDesc.stage = rhi::ShaderStage::Fragment;
    fragmentShaderDesc.path = fragmentPath;
    m_primitiveFragmentShader = m_device->createShaderModule(fragmentShaderDesc);

    if (!m_primitiveVertexShader || !m_primitiveFragmentShader) {
        ENGINE_LOG_ERROR("Failed to create primitive shader modules");
        return false;
    }

    m_primitivePipelineLayout = m_device->createPipelineLayout();
    if (!m_primitivePipelineLayout) {
        ENGINE_LOG_ERROR("Failed to create primitive pipeline layout");
        return false;
    }

    rhi::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = m_primitiveVertexShader.get();
    pipelineDesc.fragmentShader = m_primitiveFragmentShader.get();
    pipelineDesc.topology = rhi::PrimitiveTopology::TriangleList;
    m_primitivePipeline = m_device->createGraphicsPipeline(pipelineDesc);
    if (!m_primitivePipeline) {
        ENGINE_LOG_ERROR("Failed to create primitive graphics pipeline");
        return false;
    }

    return true;
}

bool Renderer::initializeResourceSubsystem() {
    m_placeholderMesh = createPlaceholderMesh();
    m_unitCubeMesh = createUnitCubeMesh();
    m_unitPyramidMesh = createUnitPyramidMesh();
    m_unitSphereMesh = createUnitSphereMesh();
    m_whiteTexture = createWhiteTexture();
    m_fallbackShader = createFallbackShader();

    if (!m_placeholderMesh || !m_unitCubeMesh || !m_unitPyramidMesh || !m_unitSphereMesh || !m_whiteTexture ||
        !m_fallbackShader) {
        ENGINE_LOG_ERROR("Failed to create one or more fallback resources");
        return false;
    }

    m_resourceManager.registerFallback<resources::Mesh>(m_placeholderMesh);
    m_resourceManager.registerFallback<resources::Texture>(m_whiteTexture);
    m_resourceManager.registerFallback<resources::ShaderProgram>(m_fallbackShader);

    m_resourceManager.registerLoader<resources::Mesh>(
        [this](const std::string& path) { return requestMeshLoadFromDisk(path); });
    m_resourceManager.registerLoader<resources::Texture>(
        [this](const std::string& path) { return requestTextureLoadFromDisk(path); });
    m_resourceManager.registerLoader<resources::ShaderProgram>(
        [this](const std::string& path) { return requestShaderProgramLoadFromManifest(path); });

    ENGINE_LOG_INFO("Resource subsystem initialized");
    return true;
}

bool Renderer::initializeImGui() {
    auto* diligentDevice = dynamic_cast<rhi::diligent::DiligentDevice*>(m_device.get());
    if (diligentDevice == nullptr) {
        return false;
    }

    auto* nativeDevice = diligentDevice->nativeRenderDevice();
    auto* nativeSwapChain = diligentDevice->nativeSwapChain();
    if (nativeDevice == nullptr || nativeSwapChain == nullptr) {
        return false;
    }

    try {
        m_imgui = std::unique_ptr<Diligent::ImGuiImplDiligent, ImGuiImplDiligentDeleter>(
            new Diligent::ImGuiImplDiligent(Diligent::ImGuiDiligentCreateInfo{nativeDevice, nativeSwapChain->GetDesc()}));
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        editor::applyEditorTheme();
        const auto sourceRoot = std::filesystem::path(ENGINE_SOURCE_ROOT);
        const auto localFonts = sourceRoot.empty() ? std::filesystem::path{} : sourceRoot / ".local" / "editor-fonts";
        if (!editor::configureEditorFonts(io, m_engineAssetRoot, localFonts)) ENGINE_LOG_WARN("Editor font unavailable; using ImGui fallback");
        m_imgui->InvalidateDeviceObjects();
        m_imgui->CreateDeviceObjects();
        try {
            m_uiCanvases = std::make_unique<UiCanvasCompositor>(nativeDevice, m_frames.size());
        } catch (const std::exception& error) {
            ENGINE_LOG_WARN("CanvasGroup GPU composition unavailable: {}", error.what());
        }
        ENGINE_LOG_INFO("ImGui layer initialized");
        return true;
    } catch (const std::exception& ex) {
        ENGINE_LOG_WARN("Failed to initialize ImGui layer: {}", ex.what());
        m_imgui.reset();
        return false;
    }
}

std::shared_ptr<resources::Mesh> Renderer::createPlaceholderMesh() {
    auto mesh = std::make_shared<resources::Mesh>();
    mesh->path = "__placeholder_mesh__";
    mesh->data.vertices = {
        resources::MeshVertex{glm::vec3(-0.5F, -0.5F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F), glm::vec2(0.0F, 0.0F)},
        resources::MeshVertex{glm::vec3(0.5F, -0.5F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F), glm::vec2(1.0F, 0.0F)},
        resources::MeshVertex{glm::vec3(0.5F, 0.5F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F), glm::vec2(1.0F, 1.0F)},
        resources::MeshVertex{glm::vec3(-0.5F, 0.5F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F), glm::vec2(0.0F, 1.0F)},
    };
    mesh->data.indices = {0, 1, 2, 0, 2, 3};

    if (!uploadMeshToGpu(*mesh)) {
        ENGINE_LOG_ERROR("Failed to upload placeholder mesh");
        return nullptr;
    }

    mesh->setLoadState(resources::ResourceLoadState::Loaded);
    ENGINE_LOG_INFO(
        "Placeholder mesh ready: vertices={}, indices={}",
        mesh->vertexCount,
        mesh->indexCount);
    return mesh;
}

std::shared_ptr<resources::Mesh> Renderer::createUnitCubeMesh() {
    auto mesh = std::make_shared<resources::Mesh>();
    mesh->path = kProceduralCubeMeshId;

    const Diligent::CubeGeometryPrimitiveAttributes attributes{
        1.0F,
        Diligent::GEOMETRY_PRIMITIVE_VERTEX_FLAG_ALL,
        1U};
    if (!populateMeshDataFromGeometryPrimitive(attributes, mesh->data)) {
        ENGINE_LOG_ERROR("Failed to generate Diligent cube primitive");
        return nullptr;
    }

    if (!uploadMeshToGpu(*mesh)) {
        ENGINE_LOG_ERROR("Failed to upload debug cube mesh");
        return nullptr;
    }

    mesh->setLoadState(resources::ResourceLoadState::Loaded);
    return mesh;
}

std::shared_ptr<resources::Mesh> Renderer::createUnitPyramidMesh() {
    auto mesh = std::make_shared<resources::Mesh>();
    mesh->path = kProceduralPyramidMeshId;

    const glm::vec3 apex{0.0F, 0.5F, 0.0F};
    const glm::vec3 base0{-0.5F, -0.5F, -0.5F};
    const glm::vec3 base1{0.5F, -0.5F, -0.5F};
    const glm::vec3 base2{0.5F, -0.5F, 0.5F};
    const glm::vec3 base3{-0.5F, -0.5F, 0.5F};

    const auto appendFace = [&](const glm::vec3& a,
                                const glm::vec3& b,
                                const glm::vec3& c,
                                const glm::vec2& uvA,
                                const glm::vec2& uvB,
                                const glm::vec2& uvC) {
        const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
        const std::uint32_t baseIndex = static_cast<std::uint32_t>(mesh->data.vertices.size());
        mesh->data.vertices.push_back(resources::MeshVertex{a, normal, uvA});
        mesh->data.vertices.push_back(resources::MeshVertex{b, normal, uvB});
        mesh->data.vertices.push_back(resources::MeshVertex{c, normal, uvC});
        mesh->data.indices.push_back(baseIndex + 0U);
        mesh->data.indices.push_back(baseIndex + 1U);
        mesh->data.indices.push_back(baseIndex + 2U);
    };

    appendFace(apex, base1, base0, glm::vec2(0.5F, 0.0F), glm::vec2(1.0F, 1.0F), glm::vec2(0.0F, 1.0F));
    appendFace(apex, base2, base1, glm::vec2(0.5F, 0.0F), glm::vec2(1.0F, 1.0F), glm::vec2(0.0F, 1.0F));
    appendFace(apex, base3, base2, glm::vec2(0.5F, 0.0F), glm::vec2(1.0F, 1.0F), glm::vec2(0.0F, 1.0F));
    appendFace(apex, base0, base3, glm::vec2(0.5F, 0.0F), glm::vec2(1.0F, 1.0F), glm::vec2(0.0F, 1.0F));

    const glm::vec3 baseNormal{0.0F, -1.0F, 0.0F};
    const std::uint32_t baseIndex = static_cast<std::uint32_t>(mesh->data.vertices.size());
    mesh->data.vertices.push_back(resources::MeshVertex{base0, baseNormal, glm::vec2(0.0F, 0.0F)});
    mesh->data.vertices.push_back(resources::MeshVertex{base3, baseNormal, glm::vec2(0.0F, 1.0F)});
    mesh->data.vertices.push_back(resources::MeshVertex{base2, baseNormal, glm::vec2(1.0F, 1.0F)});
    mesh->data.vertices.push_back(resources::MeshVertex{base1, baseNormal, glm::vec2(1.0F, 0.0F)});
    mesh->data.indices.insert(
        mesh->data.indices.end(),
        {baseIndex + 0U, baseIndex + 2U, baseIndex + 1U, baseIndex + 0U, baseIndex + 3U, baseIndex + 2U});

    if (!uploadMeshToGpu(*mesh)) {
        ENGINE_LOG_ERROR("Failed to upload procedural pyramid mesh");
        return nullptr;
    }

    mesh->setLoadState(resources::ResourceLoadState::Loaded);
    return mesh;
}

std::shared_ptr<resources::Mesh> Renderer::createUnitSphereMesh() {
    auto mesh = std::make_shared<resources::Mesh>();
    mesh->path = kProceduralSphereMeshId;

    const Diligent::SphereGeometryPrimitiveAttributes attributes{
        0.5F,
        Diligent::GEOMETRY_PRIMITIVE_VERTEX_FLAG_ALL,
        12U};
    if (!populateMeshDataFromGeometryPrimitive(attributes, mesh->data)) {
        ENGINE_LOG_ERROR("Failed to generate Diligent sphere primitive");
        return nullptr;
    }

    if (!uploadMeshToGpu(*mesh)) {
        ENGINE_LOG_ERROR("Failed to upload procedural sphere mesh");
        return nullptr;
    }

    mesh->setLoadState(resources::ResourceLoadState::Loaded);
    return mesh;
}

std::shared_ptr<resources::Texture> Renderer::createWhiteTexture() {
    auto texture = std::make_shared<resources::Texture>();
    texture->path = "__white_texture__";
    texture->data.width = 1;
    texture->data.height = 1;
    texture->data.channels = 4;
    texture->data.pixels = {255, 255, 255, 255};

    rhi::ImageDesc imageDesc{};
    imageDesc.width = 1;
    imageDesc.height = 1;
    imageDesc.format = rhi::ImageFormat::RGBA8;
    imageDesc.generateMipmaps = false;
    texture->image = m_device->createImage(imageDesc, texture->data.pixels.data());
    if (!texture->image) {
        ENGINE_LOG_ERROR("Failed to create white fallback texture");
        return nullptr;
    }

    texture->setLoadState(resources::ResourceLoadState::Loaded);
    ENGINE_LOG_INFO("Fallback white texture ready: 1x1");
    return texture;
}

std::shared_ptr<resources::ShaderProgram> Renderer::createFallbackShader() {
    auto shader = createShaderProgram(
        "__fallback_shader__",
        utf8Path(m_engineAssetRoot / "shaders_hlsl/textured.vert.hlsl"),
        utf8Path(m_engineAssetRoot / "shaders_hlsl/textured.frag.hlsl"));
    if (!shader) {
        ENGINE_LOG_ERROR("Failed to create fallback shader program");
        return nullptr;
    }
    return shader;
}

std::shared_ptr<resources::Mesh> Renderer::requestMeshLoadFromDisk(const std::string& path) {
    if (path == kProceduralCubeMeshId || path == "builtin:cube") {
        return m_unitCubeMesh;
    }
    if (path == kProceduralPyramidMeshId || path == "builtin:pyramid") {
        return m_unitPyramidMesh;
    }
    if (path == kProceduralSphereMeshId || path == "builtin:sphere") {
        return m_unitSphereMesh;
    }
    if (path.starts_with("builtin:")) { return m_placeholderMesh; }

    auto mesh = std::make_shared<resources::Mesh>();
    mesh->path = path;
    mesh->setLoadState(resources::ResourceLoadState::Loading);

    if (auto* uploads = m_device->uploadQueue(); uploads && m_assetLoads.stats().asyncEnabled) {
        m_assetLoads.submitPrepared([uploads, mesh, path]() -> resources::AsyncLoadQueue::Prepared {
            ENGINE_PROFILE_ZONE("Load Mesh Transfer Job");
            struct Payload final {
                resources::MeshData data;
                rhi::UploadSubmission gpu;
            };
            auto payload = std::make_shared<Payload>();
            std::string error;
            if (!resources::loadMeshData(path, payload->data, &error) || payload->data.vertices.empty()) {
                throw std::runtime_error("Mesh decode failed: " + error);
            }
            std::array<rhi::BufferUpload, 2> buffers{};
            buffers[0].desc.size = payload->data.vertices.size() * sizeof(resources::MeshVertex);
            buffers[0].desc.usage = rhi::BufferUsage::Vertex;
            buffers[0].desc.vertexLayout = rhi::VertexLayout::Position3Normal3Uv2;
            buffers[0].data = std::as_bytes(std::span{payload->data.vertices});
            buffers[1].desc.size = payload->data.indices.size() * sizeof(std::uint32_t);
            buffers[1].desc.usage = rhi::BufferUsage::Index;
            buffers[1].data = std::as_bytes(std::span{payload->data.indices});
            rhi::UploadRequest request;
            request.buffers = std::span{buffers}.first(payload->data.indices.empty() ? 1U : 2U);
            payload->gpu = uploads->submit(request);
            return {[mesh, path, payload]() mutable {
                ENGINE_PROFILE_ZONE("Upload Mesh Publish");
                mesh->data = std::move(payload->data);
                mesh->vertexCount = static_cast<std::uint32_t>(mesh->data.vertices.size());
                mesh->indexCount = static_cast<std::uint32_t>(mesh->data.indices.size());
                mesh->vertexBuffer = std::move(payload->gpu.buffers[0]);
                if (payload->gpu.buffers.size() > 1) { mesh->indexBuffer = std::move(payload->gpu.buffers[1]); }
                mesh->lastError.clear();
                mesh->setLoadState(resources::ResourceLoadState::Loaded);
                ENGINE_LOG_INFO("Mesh loaded '{}' via GPU transfer: vertices={}, indices={}", path,
                    mesh->vertexCount, mesh->indexCount);
                return true;
            }, payload->gpu.ticket, payload->gpu.bytes};
        }, [mesh, path](const std::string& error) {
            mesh->lastError = error;
            mesh->setLoadState(resources::ResourceLoadState::Failed);
            ENGINE_LOG_ERROR("Mesh transfer failed '{}': {}", path, error);
        });
        return mesh;
    }

    m_assetLoads.submit([this, mesh, path]() -> resources::AsyncLoadQueue::Finalize {
        ENGINE_PROFILE_ZONE("Load Mesh CPU");
        resources::MeshData meshData;
        std::string errorMessage;
        const bool loaded = resources::loadMeshData(path, meshData, &errorMessage);

        return [this, mesh, path, meshData = std::move(meshData), errorMessage = std::move(errorMessage), loaded]() mutable {
            ENGINE_PROFILE_ZONE("Upload Mesh GPU");
            if (!loaded) {
                mesh->lastError = errorMessage;
                mesh->setLoadState(resources::ResourceLoadState::Failed);
                ENGINE_LOG_ERROR("Mesh load failed '{}': {}", path, mesh->lastError);
                return false;
            }

            mesh->data = std::move(meshData);
            mesh->vertexBuffer.reset();
            mesh->indexBuffer.reset();
            mesh->vertexCount = 0;
            mesh->indexCount = 0;
            if (!uploadMeshToGpu(*mesh)) {
                mesh->lastError = "GPU upload failed";
                mesh->setLoadState(resources::ResourceLoadState::Failed);
                ENGINE_LOG_ERROR("Mesh upload failed '{}'", path);
                return false;
            }

            mesh->lastError.clear();
            mesh->setLoadState(resources::ResourceLoadState::Loaded);
            ENGINE_LOG_INFO(
                "Mesh loaded '{}': vertices={}, indices={}",
                path,
                mesh->vertexCount,
                mesh->indexCount);
            return true;
        };
    }, [mesh, path](const std::string& error) {
        mesh->lastError = error;
        mesh->setLoadState(resources::ResourceLoadState::Failed);
        ENGINE_LOG_ERROR("Mesh loading stopped '{}': {}", path, error);
    });

    return mesh;
}

bool Renderer::uploadMeshToGpu(resources::Mesh& mesh) const {
    ENGINE_PROFILE_ZONE("Create Mesh GPU Buffers");
    if (!m_device || mesh.data.vertices.empty()) {
        return false;
    }

    rhi::BufferDesc vertexBufferDesc{};
    vertexBufferDesc.size = mesh.data.vertices.size() * sizeof(resources::MeshVertex);
    vertexBufferDesc.usage = rhi::BufferUsage::Vertex;
    vertexBufferDesc.vertexLayout = rhi::VertexLayout::Position3Normal3Uv2;
    mesh.vertexBuffer = m_device->createBuffer(vertexBufferDesc, mesh.data.vertices.data());
    if (!mesh.vertexBuffer) {
        return false;
    }

    mesh.vertexCount = static_cast<std::uint32_t>(mesh.data.vertices.size());
    mesh.indexCount = static_cast<std::uint32_t>(mesh.data.indices.size());

    if (!mesh.data.indices.empty()) {
        rhi::BufferDesc indexBufferDesc{};
        indexBufferDesc.size = mesh.data.indices.size() * sizeof(std::uint32_t);
        indexBufferDesc.usage = rhi::BufferUsage::Index;
        mesh.indexBuffer = m_device->createBuffer(indexBufferDesc, mesh.data.indices.data());
        if (!mesh.indexBuffer) {
            return false;
        }
    }

    return true;
}

std::shared_ptr<resources::Texture> Renderer::requestTextureLoadFromDisk(const std::string& path) {
    auto texture = std::make_shared<resources::Texture>();
    texture->path = path;
    texture->setLoadState(resources::ResourceLoadState::Loading);

    if (auto* uploads = m_device->uploadQueue(); uploads && m_assetLoads.stats().asyncEnabled) {
        m_assetLoads.submitPrepared([uploads, texture, path]() -> resources::AsyncLoadQueue::Prepared {
            ENGINE_PROFILE_ZONE("Load Texture Transfer Job");
            struct Payload final {
                resources::TextureData data;
                rhi::UploadSubmission gpu;
            };
            auto payload = std::make_shared<Payload>();
            std::string error;
            if (!resources::loadTextureDataRgba8(path, payload->data, &error)) {
                throw std::runtime_error("Texture decode failed: " + error);
            }
            const auto width = static_cast<std::uint32_t>(payload->data.width);
            const auto height = static_cast<std::uint32_t>(payload->data.height);
            const auto tail = resources::buildRgba8MipTail(width, height, payload->data.pixels);
            std::vector<rhi::ImageMipUpload> mips;
            mips.reserve(tail.size() + 1U);
            mips.push_back({width, height, payload->data.pixels});
            for (const auto& level : tail) { mips.push_back({level.width, level.height, level.pixels}); }
            rhi::UploadRequest request;
            request.imageDesc = {width, height, rhi::ImageFormat::RGBA8, true};
            request.imageMips = mips;
            payload->gpu = uploads->submit(request);
            return {[texture, path, payload]() mutable {
                ENGINE_PROFILE_ZONE("Upload Texture Publish");
                texture->data = std::move(payload->data);
                texture->image = std::move(payload->gpu.image);
                texture->lastError.clear();
                texture->setLoadState(resources::ResourceLoadState::Loaded);
                ENGINE_LOG_INFO("Texture loaded '{}' via GPU transfer: {}x{}", path,
                    texture->data.width, texture->data.height);
                return true;
            }, payload->gpu.ticket, payload->gpu.bytes};
        }, [texture, path](const std::string& error) {
            texture->lastError = error;
            texture->setLoadState(resources::ResourceLoadState::Failed);
            ENGINE_LOG_ERROR("Texture transfer failed '{}': {}", path, error);
        });
        return texture;
    }

    m_assetLoads.submit([this, texture, path]() -> resources::AsyncLoadQueue::Finalize {
        ENGINE_PROFILE_ZONE("Load Texture CPU");
        resources::TextureData textureData;
        std::string errorMessage;
        const bool loaded = resources::loadTextureDataRgba8(path, textureData, &errorMessage);

        return [this, texture, path, textureData = std::move(textureData), errorMessage = std::move(errorMessage), loaded]() mutable {
            ENGINE_PROFILE_ZONE("Upload Texture GPU");
            if (!loaded) {
                texture->lastError = errorMessage;
                texture->setLoadState(resources::ResourceLoadState::Failed);
                ENGINE_LOG_ERROR("Texture load failed '{}': {}", path, texture->lastError);
                return false;
            }

            texture->data = std::move(textureData);
            texture->image.reset();

            rhi::ImageDesc imageDesc{};
            imageDesc.width = static_cast<std::uint32_t>(texture->data.width);
            imageDesc.height = static_cast<std::uint32_t>(texture->data.height);
            imageDesc.format = rhi::ImageFormat::RGBA8;
            imageDesc.generateMipmaps = true;
            texture->image = m_device->createImage(imageDesc, texture->data.pixels.data());
            if (!texture->image) {
                texture->lastError = "GPU upload failed";
                texture->setLoadState(resources::ResourceLoadState::Failed);
                ENGINE_LOG_ERROR("Texture upload failed '{}'", path);
                return false;
            }

            texture->lastError.clear();
            texture->setLoadState(resources::ResourceLoadState::Loaded);
            ENGINE_LOG_INFO(
                "Texture loaded '{}': {}x{} channels={}",
                path,
                texture->data.width,
                texture->data.height,
                texture->data.channels);
            return true;
        };
    }, [texture, path](const std::string& error) {
        texture->lastError = error;
        texture->setLoadState(resources::ResourceLoadState::Failed);
        ENGINE_LOG_ERROR("Texture loading stopped '{}': {}", path, error);
    });

    return texture;
}

std::shared_ptr<resources::ShaderProgram> Renderer::createShaderProgram(
    const std::string& key,
    const std::string& vertexPath,
    const std::string& fragmentPath) {
    ENGINE_PROFILE_ZONE("Create Shader Program");
    auto program = std::make_shared<resources::ShaderProgram>();
    program->key = key;
    program->descriptorPath = key;
    program->vertexPath = vertexPath;
    program->fragmentPath = fragmentPath;

    rhi::ShaderModuleDesc vertexShaderDesc{};
    vertexShaderDesc.stage = rhi::ShaderStage::Vertex;
    vertexShaderDesc.path = vertexPath;
    program->vertexShader = m_device->createShaderModule(vertexShaderDesc);
    if (!program->vertexShader) {
        ENGINE_LOG_ERROR("Shader load failed (vertex): '{}'", vertexPath);
        return nullptr;
    }

    rhi::ShaderModuleDesc fragmentShaderDesc{};
    fragmentShaderDesc.stage = rhi::ShaderStage::Fragment;
    fragmentShaderDesc.path = fragmentPath;
    program->fragmentShader = m_device->createShaderModule(fragmentShaderDesc);
    if (!program->fragmentShader) {
        ENGINE_LOG_ERROR("Shader load failed (fragment): '{}'", fragmentPath);
        return nullptr;
    }

    program->pipelineLayout = m_device->createPipelineLayout();
    if (!program->pipelineLayout) {
        ENGINE_LOG_ERROR("Pipeline layout creation failed for shader '{}'", key);
        return nullptr;
    }

    rhi::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = program->vertexShader.get();
    pipelineDesc.fragmentShader = program->fragmentShader.get();
    pipelineDesc.topology = rhi::PrimitiveTopology::TriangleList;
    program->pipeline = m_device->createGraphicsPipeline(pipelineDesc);
    if (!program->pipeline) {
        ENGINE_LOG_ERROR("Pipeline creation failed for shader '{}'", key);
        return nullptr;
    }

    program->lastError.clear();
    program->setLoadState(resources::ResourceLoadState::Loaded);
    ENGINE_LOG_INFO("Shader program loaded '{}': vert='{}' frag='{}'", key, vertexPath, fragmentPath);
    return program;
}

std::shared_ptr<resources::ShaderProgram> Renderer::requestShaderProgramLoadFromManifest(const std::string& path) {
    auto program = std::make_shared<resources::ShaderProgram>();
    program->key = path;
    program->descriptorPath = path;
    program->setLoadState(resources::ResourceLoadState::Loading);

    const auto assetRoot = shaderAssetRoot(path);
    m_assetLoads.submit([this, program, path, assetRoot]() -> resources::AsyncLoadQueue::Finalize {
        ENGINE_PROFILE_ZONE("Load Shader Manifest CPU");
        resources::ShaderProgramSource source{};
        std::string errorMessage;
        const bool loaded = resources::loadShaderProgramSource(path, source, &errorMessage, assetRoot);

        return [this, program, path, source = std::move(source), errorMessage = std::move(errorMessage), loaded]() mutable {
            ENGINE_PROFILE_ZONE("Upload Shader GPU");
            if (!loaded || !resolveShaderSources(source, &errorMessage)) {
                program->lastError = errorMessage;
                program->setLoadState(resources::ResourceLoadState::Failed);
                ENGINE_LOG_ERROR("Shader source load failed '{}': {}", path, program->lastError);
                return false;
            }

            auto compiledProgram = createShaderProgram(path, source.vertexPath, source.fragmentPath);
            if (!compiledProgram) {
                program->lastError = "Shader program creation failed";
                program->setLoadState(resources::ResourceLoadState::Failed);
                return false;
            }

            program->descriptorPath = source.descriptorPath;
            program->vertexPath = source.vertexPath;
            program->fragmentPath = source.fragmentPath;
            program->vertexShader = std::move(compiledProgram->vertexShader);
            program->fragmentShader = std::move(compiledProgram->fragmentShader);
            program->pipelineLayout = std::move(compiledProgram->pipelineLayout);
            program->pipeline = std::move(compiledProgram->pipeline);
            program->lastError.clear();
            program->setLoadState(resources::ResourceLoadState::Loaded);
            registerShaderForHotReload(path, program, source);
            return true;
        };
    }, [program, path](const std::string& error) {
        program->lastError = error;
        program->setLoadState(resources::ResourceLoadState::Failed);
        ENGINE_LOG_ERROR("Shader loading stopped '{}': {}", path, error);
    });

    return program;
}

void Renderer::onEvent(const platform::Event& event) {
    if (!m_imgui || ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
        case platform::EventType::MouseMoved:
            io.AddMousePosEvent(static_cast<float>(event.mouseX), static_cast<float>(event.mouseY));
            break;

        case platform::EventType::MouseScrolled:
            io.AddMouseWheelEvent(static_cast<float>(event.scrollX), static_cast<float>(event.scrollY));
            break;

        case platform::EventType::MouseButtonPressed:
            io.AddMouseButtonEvent(toImGuiMouseButton(event.mouseButton), true);
            break;

        case platform::EventType::MouseButtonReleased:
            io.AddMouseButtonEvent(toImGuiMouseButton(event.mouseButton), false);
            break;

        case platform::EventType::KeyPressed:
        case platform::EventType::KeyReleased: {
            const bool isDown = event.type == platform::EventType::KeyPressed;
            const auto key = toImGuiKey(event.key);
            if (key.has_value()) {
                io.AddKeyEvent(*key, isDown);
            }
            addImGuiModifierEvent(io, event.key, isDown);
            break;
        }

        case platform::EventType::TextInput:
            if (event.codepoint != 0U) {
                io.AddInputCharacter(event.codepoint);
            }
            break;

        default:
            break;
    }
}

void Renderer::beginImGuiFrame(const float deltaSeconds) {
    if (!m_imgui || !m_swapchain || ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    if (m_imguiFrameOpen) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = deltaSeconds > 0.0F ? deltaSeconds : (1.0F / 60.0F);

    auto* diligentDevice = dynamic_cast<rhi::diligent::DiligentDevice*>(m_device.get());
    if (diligentDevice == nullptr) {
        return;
    }
    auto* nativeSwapChain = diligentDevice->nativeSwapChain();
    if (nativeSwapChain == nullptr) {
        return;
    }

    const auto& swapChainDesc = nativeSwapChain->GetDesc();
    // ImGuiImplDiligent does not provide a platform backend that fills DisplaySize,
    // so set it explicitly before NewFrame() to satisfy Dear ImGui sanity checks.
    io.DisplaySize = ImVec2(static_cast<float>(swapChainDesc.Width), static_cast<float>(swapChainDesc.Height));
    io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);
    m_imgui->NewFrame(swapChainDesc.Width, swapChainDesc.Height, swapChainDesc.PreTransform);
    m_imguiFrameOpen = true;
}

void Renderer::renderImGui() {
    if (!m_imgui || !m_imguiFrameOpen || ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    auto* diligentDevice = dynamic_cast<rhi::diligent::DiligentDevice*>(m_device.get());
    if (diligentDevice != nullptr) {
        if (auto* nativeContext = diligentDevice->nativeImmediateContext()) {
            if (m_uiCanvases && m_uiCanvases->pending()) {
                // Finalize dynamic font textures before rendering captured
                // layers. ImGui::Render is idempotent within the same frame.
                ImGui::Render();
                m_uiCanvases->render(nativeContext);
                if (auto* swapchain = diligentDevice->nativeSwapChain()) {
                    auto* target = swapchain->GetCurrentBackBufferRTV();
                    nativeContext->SetRenderTargets(1, &target, swapchain->GetDepthBufferDSV(),
                        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                }
            }
            m_imgui->Render(nativeContext);
        } else {
            m_imgui->EndFrame();
        }
    } else {
        m_imgui->EndFrame();
    }

    m_imguiFrameOpen = false;
}

bool Renderer::isImGuiEnabled() const {
    return m_imgui != nullptr && ImGui::GetCurrentContext() != nullptr;
}

Renderer::UiImage Renderer::resolveUiCanvas(const void* key, const ImDrawList& list,
                                          float x, float y, float width, float height) {
    if (!m_uiCanvases || !m_imguiFrameOpen || !m_activeCommandBuffer) return {};
    const auto image = m_uiCanvases->enqueue(key, list, x, y, width, height);
    return {image.textureId, image.width, image.height};
}

bool Renderer::wantsKeyboardCapture() const {
    if (!isImGuiEnabled()) {
        return false;
    }
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool Renderer::wantsMouseCapture() const {
    if (!isImGuiEnabled()) {
        return false;
    }
    return ImGui::GetIO().WantCaptureMouse;
}

void Renderer::beginFrame(const glm::vec4& clearColor) {
    ENGINE_PROFILE_ZONE("Renderer Begin Frame");
    if (!m_initialized || !m_swapchain || m_frames.empty()) {
        return;
    }

    m_assetLoads.pump();

    FrameContext& frame = m_frames[m_currentFrameIndex];
    {
        ENGINE_PROFILE_ZONE("Wait Frame Fence");
        frame.inFlight->wait();
        frame.inFlight->reset();
    }
    frame.uiImages.clear();
    if (m_uiCanvases) m_uiCanvases->beginFrame(m_currentFrameIndex);
    ++m_uiImageFrame;
    m_uiImageRequests = 0;

    {
        ENGINE_PROFILE_ZONE("Acquire Swapchain Image");
        m_swapchain->acquireNextImage(frame.imageAvailable.get());
    }

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
    drawPrimitive(ecs::PrimitiveType::Triangle, transform.toMatrix(), glm::vec4(1.0F));
}

void Renderer::drawPrimitive(ecs::PrimitiveType primitiveType, const glm::mat4& transformMatrix, const glm::vec4& tint) {
    if (!m_activeCommandBuffer || !m_primitivePipeline || !m_primitiveVertexBuffer) {
        return;
    }

    switch (primitiveType) {
        case ecs::PrimitiveType::Triangle:
        default:
            break;
    }

    m_activeCommandBuffer->bindPipeline(*m_primitivePipeline);
    m_activeCommandBuffer->bindVertexBuffer(*m_primitiveVertexBuffer);

    DrawPushConstants constants{};
    constants.modelViewProjection = transformMatrix;
    constants.tint = tint;
    m_activeCommandBuffer->pushConstants(&constants, sizeof(constants));
    m_activeCommandBuffer->draw(3, 0);
}

void Renderer::drawMesh(
    const resources::Mesh& mesh,
    const resources::ShaderProgram& shader,
    const resources::Texture* texture,
    const glm::mat4& modelMatrix,
    const glm::mat4& viewProjectionMatrix,
    const glm::vec4& tint,
    const glm::vec2& uvScale) {
    if (!m_activeCommandBuffer) {
        return;
    }

    const resources::Mesh* activeMesh = &mesh;
    const resources::Texture* activeTexture = texture;
    const resources::ShaderProgram* activeShader = &shader;

    if (!activeMesh->isReady()) {
        activeMesh = m_placeholderMesh.get();
    }
    if (activeTexture == nullptr || !activeTexture->isReady()) {
        activeTexture = m_whiteTexture.get();
    }
    if (!activeShader->isReady()) {
        activeShader = m_fallbackShader.get();
    }

    if (activeMesh == nullptr || activeTexture == nullptr || activeShader == nullptr || !activeMesh->isReady() ||
        !activeTexture->isReady() || !activeShader->isReady() || !activeShader->pipeline) {
        return;
    }

    m_activeCommandBuffer->bindPipeline(*activeShader->pipeline);
    m_activeCommandBuffer->bindVertexBuffer(*activeMesh->vertexBuffer);
    m_activeCommandBuffer->bindImage(0, *activeTexture->image);

    DrawPushConstants constants{};
    constants.modelViewProjection = viewProjectionMatrix * modelMatrix;
    constants.model = modelMatrix;
    constants.uvScaleBias = glm::vec4(uvScale.x, uvScale.y, 0.0F, 0.0F);
    constants.tint = tint;
    m_activeCommandBuffer->pushConstants(&constants, sizeof(constants));

    if (activeMesh->indexCount > 0 && activeMesh->indexBuffer) {
        m_activeCommandBuffer->bindIndexBuffer(*activeMesh->indexBuffer);
        m_activeCommandBuffer->drawIndexed(activeMesh->indexCount, 0);
        return;
    }

    m_activeCommandBuffer->draw(activeMesh->vertexCount, 0);
}

void Renderer::drawMesh(
    const resources::Mesh& mesh,
    const resources::ShaderProgram& shader,
    const resources::Texture& texture,
    const glm::mat4& modelMatrix,
    const glm::mat4& viewProjectionMatrix,
    const glm::vec4& tint,
    const glm::vec2& uvScale) {
    drawMesh(mesh, shader, &texture, modelMatrix, viewProjectionMatrix, tint, uvScale);
}

void Renderer::drawDebugAabb(
    const glm::vec3& min,
    const glm::vec3& max,
    const glm::mat4& viewProjectionMatrix,
    const glm::vec4& tint,
    float lineThickness) {
    if (!m_unitCubeMesh || !m_fallbackShader || !m_whiteTexture) {
        return;
    }

    const glm::vec3 size = max - min;
    if (size.x <= 0.0F || size.y <= 0.0F || size.z <= 0.0F) {
        return;
    }

    const glm::vec3 center = (min + max) * 0.5F;
    const glm::vec3 halfSize = size * 0.5F;
    const float thickness = std::clamp(lineThickness, 0.01F, std::max(0.01F, std::min({size.x, size.y, size.z}) * 0.5F));

    auto drawEdge = [&](const glm::vec3& edgeCenter, const glm::vec3& edgeScale) {
        glm::mat4 model(1.0F);
        model = glm::translate(model, edgeCenter);
        model = glm::scale(model, edgeScale);
        drawMesh(*m_unitCubeMesh, *m_fallbackShader, m_whiteTexture.get(), model, viewProjectionMatrix, tint);
    };

    for (const float ySign : {-1.0F, 1.0F}) {
        for (const float zSign : {-1.0F, 1.0F}) {
            drawEdge(center + glm::vec3(0.0F, halfSize.y * ySign, halfSize.z * zSign), glm::vec3(size.x, thickness, thickness));
        }
    }

    for (const float xSign : {-1.0F, 1.0F}) {
        for (const float zSign : {-1.0F, 1.0F}) {
            drawEdge(center + glm::vec3(halfSize.x * xSign, 0.0F, halfSize.z * zSign), glm::vec3(thickness, size.y, thickness));
        }
    }

    for (const float xSign : {-1.0F, 1.0F}) {
        for (const float ySign : {-1.0F, 1.0F}) {
            drawEdge(center + glm::vec3(halfSize.x * xSign, halfSize.y * ySign, 0.0F), glm::vec3(thickness, thickness, size.z));
        }
    }
}

std::unique_ptr<rhi::IBuffer> Renderer::createGpuBuffer(const rhi::BufferDesc& desc, const void* initialData) {
    if (!m_device) {
        return nullptr;
    }
    return m_device->createBuffer(desc, initialData);
}

std::unique_ptr<rhi::IImage> Renderer::createGpuImage(const rhi::ImageDesc& desc, const void* initialData) {
    if (!m_device) {
        return nullptr;
    }
    return m_device->createImage(desc, initialData);
}

std::shared_ptr<resources::ShaderProgram> Renderer::createShaderProgramFromFiles(
    const std::string& key,
    const std::string& vertexPath,
    const std::string& fragmentPath) {
    resources::ShaderProgramSource source{};
    try {
        source.descriptorPath = resolveResourcePath(key);
        source.vertexPath = resolveResourcePath(vertexPath);
        source.fragmentPath = resolveResourcePath(fragmentPath);
    } catch (const std::exception& error) {
        ENGINE_LOG_ERROR("Shader path resolution failed '{}': {}", key, error.what());
        return nullptr;
    }
    auto program = createShaderProgram(source.descriptorPath, source.vertexPath, source.fragmentPath);
    if (!program) {
        return nullptr;
    }

    {
        std::error_code ec;
        source.descriptorIsFile = std::filesystem::exists(std::filesystem::u8path(source.descriptorPath), ec) && !ec;
    }
    registerShaderForHotReload(source.descriptorPath, program, source);
    return program;
}

std::shared_ptr<resources::Mesh> Renderer::loadMesh(const std::string& path) {
    try { return m_resourceManager.load<resources::Mesh>(resolveResourcePath(path)); }
    catch (const std::exception& error) {
        ENGINE_LOG_ERROR("Mesh path resolution failed '{}': {}", path, error.what());
        return m_placeholderMesh;
    }
}

std::shared_ptr<resources::Texture> Renderer::loadTexture(const std::string& path) {
    try { return m_resourceManager.load<resources::Texture>(resolveResourcePath(path)); }
    catch (const std::exception& error) {
        ENGINE_LOG_ERROR("Texture path resolution failed '{}': {}", path, error.what());
        return m_whiteTexture;
    }
}

void Renderer::loadUiImageAliases() {
    const auto now = std::chrono::steady_clock::now();
    if (m_uiImageAliasesLoaded && now < m_uiAliasNextPoll) return;
    m_uiImageAliasesLoaded = true;
    m_uiAliasNextPoll = now + std::chrono::seconds(1);
    const auto& root = m_projectAssetRoot.empty() ? m_engineAssetRoot : m_projectAssetRoot;
    const auto file = root / "ui_images.json";
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(file, error);
    if (error) {
        if (!m_uiImageAliases.empty()) { m_uiImageAliases.clear(); m_uiImagePaths.clear(); }
        m_uiAliasWriteTime = {};
        return;
    }
    if (stamp == m_uiAliasWriteTime) return;
    m_uiAliasWriteTime = stamp; // Report a bad revision once, retaining the last good map.
    try {
        if (std::filesystem::file_size(file) > 65536) throw std::runtime_error("UI image aliases exceed 64 KiB");
        std::ifstream stream(file, std::ios::binary);
        std::string source(65537, '\0');
        stream.read(source.data(), static_cast<std::streamsize>(source.size()));
        const auto length = static_cast<std::size_t>(stream.gcount());
        if (length > 65536) throw std::runtime_error("UI image aliases exceed 64 KiB");
        source.resize(length);
        const auto json = nlohmann::json::parse(source);
        if (!json.is_object() || json.size() > 256) throw std::runtime_error("UI image aliases must be an object with at most 256 entries");
        std::unordered_map<std::string, std::string> aliases;
        for (const auto& [key, value] : json.items()) {
            if (!key.starts_with("rbxassetid://") || key.size() > 128 || !value.is_string())
                throw std::runtime_error("UI image aliases map rbxassetid:// ids to local asset paths");
            auto path = value.get<std::string>();
            if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos || path.find("://") != std::string::npos)
                throw std::runtime_error("Invalid local UI image alias path");
            aliases.emplace(key, std::move(path));
        }
        m_uiImageAliases = std::move(aliases);
        m_uiImagePaths.clear();
    } catch (const std::exception& exception) { ENGINE_LOG_WARN("UI image aliases: {}", exception.what()); }
}

bool Renderer::reserveUiImageBytes(const std::uint64_t bytes, const std::string& protectedKey, const bool newEntry) {
    constexpr std::uint64_t budget = 64 * 1024 * 1024;
    if (bytes > budget) return false;
    std::uint64_t reserved = 0;
    for (auto& [key, entry] : m_uiImages) {
        if (entry.texture && entry.texture->loadState() == resources::ResourceLoadState::Failed && !entry.pending) entry.reservedBytes = 0;
        reserved += entry.reservedBytes;
    }
    while ((newEntry && m_uiImages.size() >= 128) || reserved + bytes > budget) {
        auto victim = m_uiImages.end();
        for (auto candidate = m_uiImages.begin(); candidate != m_uiImages.end(); ++candidate) {
            const auto& entry = candidate->second;
            if (candidate->first == protectedKey || entry.lastUsedFrame == m_uiImageFrame) continue;
            // Pending jobs retain their targets, so they cannot be evicted to
            // make room for an unbounded backlog of orphan decodes.
            if ((entry.pending && entry.pending->loadState() == resources::ResourceLoadState::Loading) ||
                (entry.texture && entry.texture->loadState() == resources::ResourceLoadState::Loading)) continue;
            if (victim == m_uiImages.end() || entry.lastUsedFrame < victim->second.lastUsedFrame) victim = candidate;
        }
        if (victim == m_uiImages.end()) return false;
        reserved -= victim->second.reservedBytes;
        m_uiImages.erase(victim);
    }
    return true;
}

Renderer::UiImage Renderer::resolveUiImage(const std::string_view image) {
    if (image.empty() || image.size() > 4096 || image.find('\0') != std::string_view::npos) return {};
    loadUiImageAliases();
    const std::string id(image);
    auto pathIt = m_uiImagePaths.find(id);
    std::string path;
    if (pathIt != m_uiImagePaths.end()) path = pathIt->second;
    else {
        try {
            std::string local = id;
            if (id.starts_with("rbxassetid://")) {
                const auto alias = m_uiImageAliases.find(id);
                if (alias == m_uiImageAliases.end()) throw std::invalid_argument("Roblox image id needs an assets/ui_images.json alias");
                local = alias->second;
            }
            if (local.find("://") != std::string::npos || local.find('|') != std::string::npos)
                throw std::invalid_argument("UI images use local files beneath assets; network URLs are not image assets");
            path = resolveResourcePath(local);
            if (m_projectAssetRoot.empty() && !containsPath(m_engineAssetRoot, std::filesystem::u8path(path)))
                throw std::invalid_argument("UI image path is outside engine assets");
        } catch (const std::exception& exception) {
            // Negative entries are bounded and avoid per-frame IO/log spam. An
            // added alias resolves to a new canonical key on the next map poll.
            path = "!" + id;
            if (!m_uiImages.contains(path) && m_uiImages.size() < 128) {
                ENGINE_LOG_WARN("UI image unavailable: {}", exception.what());
                m_uiImages.emplace(path, UiImageEntry{});
            }
        }
        if (m_uiImagePaths.size() >= 512) m_uiImagePaths.clear();
        m_uiImagePaths.emplace(id, path);
    }
    return resolveUiImagePath(path);
}

Renderer::UiImage Renderer::resolveEditorIcon(const std::string_view image) {
    if (!image.starts_with("editor/icons/") || image.size() > 160 || image.find('\0') != std::string_view::npos) return {};
    const std::string id(image);
    if (const auto cached = m_editorIconPaths.find(id); cached != m_editorIconPaths.end()) return resolveUiImagePath(cached->second);
    const auto relative = std::filesystem::u8path(image).lexically_normal();
    if (relative.is_absolute() || relative.extension() != ".png") return {};
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(m_engineAssetRoot / relative, error);
    if (error) return {};
    if (!containsPath(m_engineAssetRoot / "editor" / "icons", path)) return {};
    if (m_editorIconPaths.size() >= 128) m_editorIconPaths.clear();
    const auto resolved = utf8Path(path);
    m_editorIconPaths.emplace(id, resolved);
    return resolveUiImagePath(resolved);
}

Renderer::UiImage Renderer::resolveUiImagePath(const std::string& path) {
    if (path.empty()) return {};
    auto found = m_uiImages.find(path);
    if (found == m_uiImages.end()) {
        if (m_uiImageRequests >= 4) return {};
        ++m_uiImageRequests;
        std::int32_t width = 0, height = 0;
        std::string error;
        const bool valid = path.front() != '!' && resources::inspectTextureRgba8(path, width, height, &error);
        const auto bytes = valid ? std::uint64_t(width) * height * 4 : 0;
        if (!reserveUiImageBytes(bytes, path, true)) return {};
        UiImageEntry entry;
        entry.lastUsedFrame = m_uiImageFrame;
        entry.reservedBytes = bytes;
        std::error_code stampError;
        entry.fileStamp = std::filesystem::last_write_time(std::filesystem::u8path(path), stampError);
        if (stampError) entry.fileStamp = {};
        entry.nextPoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        if (valid) entry.texture = m_resourceManager.load<resources::Texture>(path);
        else if (!error.empty()) ENGINE_LOG_WARN("UI image cannot load '{}': {}", path, error);
        if (!entry.texture || entry.texture->path != path || entry.texture->loadState() == resources::ResourceLoadState::Failed)
            entry.reservedBytes = 0;
        found = m_uiImages.emplace(path, std::move(entry)).first;
    }
    auto& entry = found->second;
    if (entry.pending && entry.pending->loadState() != resources::ResourceLoadState::Loading) {
        if (entry.pending->isReady() && entry.pending->path == path) {
            entry.texture = std::move(entry.pending);
            entry.reservedBytes = entry.pendingBytes;
        } else {
            entry.reservedBytes -= entry.pendingBytes;
            entry.pending.reset(); // Retain the old good image on failed edits.
        }
        entry.pendingBytes = 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (path.front() != '!' && !entry.pending && now >= entry.nextPoll && m_uiImageRequests < 4 &&
        (!entry.texture || entry.texture->loadState() != resources::ResourceLoadState::Loading)) {
        entry.nextPoll = now + std::chrono::milliseconds(500);
        std::error_code stampError;
        auto stamp = std::filesystem::last_write_time(std::filesystem::u8path(path), stampError);
        if (stampError) stamp = {};
        if (stamp != entry.fileStamp) {
            std::int32_t width = 0, height = 0;
            std::string error;
            if (resources::inspectTextureRgba8(path, width, height, &error)) {
                const auto bytes = std::uint64_t(width) * height * 4;
                if (reserveUiImageBytes(bytes, path, false)) {
                    ++m_uiImageRequests;
                    entry.pending = m_resourceManager.reload<resources::Texture>(path);
                    if (entry.pending && entry.pending->path != path) entry.pending.reset();
                    if (entry.pending) { entry.pendingBytes = bytes; entry.reservedBytes += bytes; }
                    entry.fileStamp = stamp;
                }
            } else {
                entry.fileStamp = stamp;
                ENGINE_LOG_WARN("UI image edit rejected '{}': {}", path, error);
            }
        }
    }
    const auto& texture = entry.texture;
    const bool firstUse = entry.lastUsedFrame != m_uiImageFrame;
    entry.lastUsedFrame = m_uiImageFrame;
    if (!texture || !texture->isReady() || texture->path != path) return {};
    if (!m_frames.empty()) {
        auto& pins = m_frames[m_currentFrameIndex].uiImages;
        if (firstUse || std::find(pins.begin(), pins.end(), texture) == pins.end()) pins.push_back(texture);
    }
    return { texture->image->uiShaderResourceView(), static_cast<float>(texture->image->width()), static_cast<float>(texture->image->height()) };
}

std::shared_ptr<resources::ShaderProgram> Renderer::loadShaderProgram(const std::string& path) {
    try { return m_resourceManager.load<resources::ShaderProgram>(resolveResourcePath(path)); }
    catch (const std::exception& error) {
        ENGINE_LOG_ERROR("Shader path resolution failed '{}': {}", path, error.what());
        return m_fallbackShader;
    }
}

void Renderer::setProjectAssetRoot(std::filesystem::path assetsRoot) {
    if (!assetsRoot.empty()) { assetsRoot = std::filesystem::weakly_canonical(std::filesystem::absolute(assetsRoot)); }
    if (assetsRoot == m_projectAssetRoot) { return; }
    // stop() joins both CPU work and transfer fences before cancelling owner
    // finalizers, so an old project cannot publish work into the new context.
    m_assetLoads.stop();
    m_shaderHotReloadEntries.clear();
    m_uiImages.clear();
    m_uiImageAliases.clear();
    m_uiImagePaths.clear();
    m_uiImageAliasesLoaded = false;
    m_uiAliasWriteTime = {};
    m_uiImageRequests = 0;
    m_resourceManager.clearCache();
    m_projectAssetRoot = std::move(assetsRoot);
    if (m_initialized) { m_assetLoads.start(m_jobSystem); }
}

std::string Renderer::resolveResourcePath(const std::string& id) const {
    if (id.empty()) { throw std::invalid_argument("Asset id is empty"); }
    if (isProceduralResource(id)) { return id; }
    if (const auto separator = id.find('|'); separator != std::string::npos) {
        return resolveResourcePath(id.substr(0, separator)) + '|' + resolveResourcePath(id.substr(separator + 1));
    }
    const auto& root = m_projectAssetRoot.empty() ? m_engineAssetRoot : m_projectAssetRoot;
    auto relative = std::filesystem::u8path(id);
    std::filesystem::path target;
    if (relative.is_absolute()) {
        target = std::filesystem::weakly_canonical(relative);
        if (m_projectAssetRoot.empty()) { return utf8Path(target); }
        if (containsPath(root, target)) { relative = target.lexically_relative(root); }
        else if (containsPath(m_engineAssetRoot, target) && isBuiltinAsset(target.lexically_relative(m_engineAssetRoot))) {
            return utf8Path(target);
        } else { throw std::invalid_argument("Resource path is outside the active project assets"); }
    } else {
        if (relative.has_root_name() || relative.has_root_directory()) { throw std::invalid_argument("Asset id has an invalid root"); }
        if (*relative.begin() == "assets") { relative = relative.lexically_relative("assets"); }
        target = std::filesystem::weakly_canonical(root / relative);
        if (!containsPath(root, target)) { throw std::invalid_argument("Resource path escapes assets"); }
        relative = target.lexically_relative(root);
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(target, error);
    if (!m_projectAssetRoot.empty() && isBuiltinAsset(relative) && !exists && !error) {
        return utf8Path(std::filesystem::weakly_canonical(m_engineAssetRoot / relative));
    }
    return utf8Path(target);
}

std::filesystem::path Renderer::shaderAssetRoot(const std::string& descriptor) const {
    const auto path = std::filesystem::u8path(descriptor);
    if (path.is_absolute() && containsPath(m_engineAssetRoot, path)) { return m_engineAssetRoot; }
    return m_projectAssetRoot.empty() ? m_engineAssetRoot : m_projectAssetRoot;
}

bool Renderer::resolveShaderSources(resources::ShaderProgramSource& source, std::string* error) const {
    try {
        source.vertexPath = resolveResourcePath(source.vertexPath);
        source.fragmentPath = resolveResourcePath(source.fragmentPath);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) { *error = exception.what(); }
        return false;
    }
}

resources::ResourceManager& Renderer::resourceManager() {
    return m_resourceManager;
}

const resources::ResourceManager& Renderer::resourceManager() const {
    return m_resourceManager;
}

rhi::Extent2D Renderer::frameExtent() const {
    if (m_swapchain) {
        return m_swapchain->extent();
    }

    rhi::Extent2D fallback{};
    if (m_window != nullptr) {
        fallback.width = static_cast<std::uint32_t>(m_window->width());
        fallback.height = static_cast<std::uint32_t>(m_window->height());
        return fallback;
    }

    fallback.width = 1280;
    fallback.height = 720;
    return fallback;
}

void Renderer::registerShaderForHotReload(
    const std::string& cacheKey,
    const std::shared_ptr<resources::ShaderProgram>& program,
    const resources::ShaderProgramSource& source) {
    if (!program) {
        return;
    }

    program->descriptorPath = source.descriptorPath;

    ShaderHotReloadEntry entry{};
    entry.program = program;
    entry.descriptorPath = source.descriptorPath;
    entry.descriptorIsFile = source.descriptorIsFile;
    entry.vertexPath = source.vertexPath;
    entry.fragmentPath = source.fragmentPath;
    entry.appliedDescriptorWriteTime =
        source.descriptorIsFile ? getFileWriteTimeOrMin(source.descriptorPath) : std::filesystem::file_time_type::min();
    entry.appliedVertexWriteTime = getFileWriteTimeOrMin(source.vertexPath);
    entry.appliedFragmentWriteTime = getFileWriteTimeOrMin(source.fragmentPath);
    entry.failedDescriptorWriteTime = std::filesystem::file_time_type::min();
    entry.failedVertexWriteTime = std::filesystem::file_time_type::min();
    entry.failedFragmentWriteTime = std::filesystem::file_time_type::min();
    entry.reloadPending = false;
    m_shaderHotReloadEntries[cacheKey] = std::move(entry);
}

bool Renderer::reloadShaderInPlace(resources::ShaderProgram& program, const resources::ShaderProgramSource& source) {
    auto reloadedProgram = createShaderProgram(program.key, source.vertexPath, source.fragmentPath);
    if (!reloadedProgram) {
        return false;
    }

    program.descriptorPath = source.descriptorPath;
    program.vertexPath = source.vertexPath;
    program.fragmentPath = source.fragmentPath;
    program.vertexShader = std::move(reloadedProgram->vertexShader);
    program.fragmentShader = std::move(reloadedProgram->fragmentShader);
    program.pipelineLayout = std::move(reloadedProgram->pipelineLayout);
    program.pipeline = std::move(reloadedProgram->pipeline);
    program.lastError.clear();
    program.setLoadState(resources::ResourceLoadState::Loaded);
    return true;
}

void Renderer::pollHotReload() {
    if (!m_initialized || m_shaderHotReloadEntries.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    for (auto it = m_shaderHotReloadEntries.begin(); it != m_shaderHotReloadEntries.end();) {
        auto program = it->second.program.lock();
        if (!program) {
            it = m_shaderHotReloadEntries.erase(it);
            continue;
        }

        auto& entry = it->second;
        const auto descriptorWriteTime =
            entry.descriptorIsFile ? getFileWriteTimeOrMin(entry.descriptorPath) : std::filesystem::file_time_type::min();
        const auto vertexWriteTime = getFileWriteTimeOrMin(entry.vertexPath);
        const auto fragmentWriteTime = getFileWriteTimeOrMin(entry.fragmentPath);

        const bool matchesApplied = shaderTimestampsMatch(
            descriptorWriteTime,
            vertexWriteTime,
            fragmentWriteTime,
            entry.appliedDescriptorWriteTime,
            entry.appliedVertexWriteTime,
            entry.appliedFragmentWriteTime);
        if (matchesApplied) {
            entry.reloadPending = false;
            ++it;
            continue;
        }

        const bool matchesFailed = shaderTimestampsMatch(
            descriptorWriteTime,
            vertexWriteTime,
            fragmentWriteTime,
            entry.failedDescriptorWriteTime,
            entry.failedVertexWriteTime,
            entry.failedFragmentWriteTime);
        const bool matchesPending = entry.reloadPending && shaderTimestampsMatch(
            descriptorWriteTime,
            vertexWriteTime,
            fragmentWriteTime,
            entry.pendingDescriptorWriteTime,
            entry.pendingVertexWriteTime,
            entry.pendingFragmentWriteTime);

        if (!entry.reloadPending && !matchesFailed) {
            entry.pendingDescriptorWriteTime = descriptorWriteTime;
            entry.pendingVertexWriteTime = vertexWriteTime;
            entry.pendingFragmentWriteTime = fragmentWriteTime;
            entry.pendingObservedAt = now;
            entry.reloadPending = true;
            ++it;
            continue;
        }

        if (entry.reloadPending && !matchesPending) {
            entry.pendingDescriptorWriteTime = descriptorWriteTime;
            entry.pendingVertexWriteTime = vertexWriteTime;
            entry.pendingFragmentWriteTime = fragmentWriteTime;
            entry.pendingObservedAt = now;
            ++it;
            continue;
        }

        if (!entry.reloadPending || now - entry.pendingObservedAt < m_hotReloadDebounce) {
            ++it;
            continue;
        }

        resources::ShaderProgramSource latestSource{};
        if (entry.descriptorIsFile) {
            std::string sourceError;
            if (!resources::loadShaderProgramSource(entry.descriptorPath, latestSource, &sourceError,
                    shaderAssetRoot(entry.descriptorPath)) || !resolveShaderSources(latestSource, &sourceError)) {
                program->lastError = sourceError;
                entry.failedDescriptorWriteTime = entry.pendingDescriptorWriteTime;
                entry.failedVertexWriteTime = entry.pendingVertexWriteTime;
                entry.failedFragmentWriteTime = entry.pendingFragmentWriteTime;
                entry.reloadPending = false;
                ENGINE_LOG_WARN("Shader hot-reload descriptor parse failed '{}': {}", entry.descriptorPath, sourceError);
                ++it;
                continue;
            }
        } else {
            latestSource.descriptorPath = entry.descriptorPath;
            latestSource.descriptorIsFile = false;
            latestSource.vertexPath = entry.vertexPath;
            latestSource.fragmentPath = entry.fragmentPath;
        }

        if (!reloadShaderInPlace(*program, latestSource)) {
            program->lastError = "Shader program recreation failed";
            entry.failedDescriptorWriteTime = entry.pendingDescriptorWriteTime;
            entry.failedVertexWriteTime = entry.pendingVertexWriteTime;
            entry.failedFragmentWriteTime = entry.pendingFragmentWriteTime;
            entry.reloadPending = false;
            ENGINE_LOG_WARN(
                "Shader hot-reload failed for '{}': vert='{}' frag='{}'",
                entry.descriptorPath,
                latestSource.vertexPath,
                latestSource.fragmentPath);
            ++it;
            continue;
        }

        entry.descriptorPath = latestSource.descriptorPath;
        entry.descriptorIsFile = latestSource.descriptorIsFile;
        entry.vertexPath = latestSource.vertexPath;
        entry.fragmentPath = latestSource.fragmentPath;
        entry.appliedDescriptorWriteTime =
            entry.descriptorIsFile ? getFileWriteTimeOrMin(entry.descriptorPath) : std::filesystem::file_time_type::min();
        entry.appliedVertexWriteTime = getFileWriteTimeOrMin(entry.vertexPath);
        entry.appliedFragmentWriteTime = getFileWriteTimeOrMin(entry.fragmentPath);
        entry.failedDescriptorWriteTime = std::filesystem::file_time_type::min();
        entry.failedVertexWriteTime = std::filesystem::file_time_type::min();
        entry.failedFragmentWriteTime = std::filesystem::file_time_type::min();
        entry.reloadPending = false;
        ENGINE_LOG_INFO(
            "Hot-reloaded shader '{}': vert='{}' frag='{}'",
            it->first,
            entry.vertexPath,
            entry.fragmentPath);

        ++it;
    }
}

void Renderer::endFrame() {
    ENGINE_PROFILE_ZONE("Renderer End Frame");
    if (!m_initialized || !m_swapchain || m_frames.empty() || !m_activeCommandBuffer) {
        m_imguiFrameOpen = false;
        return;
    }

    FrameContext& frame = m_frames[m_currentFrameIndex];
    m_activeCommandBuffer->endRenderPass();
    m_activeCommandBuffer->end();

    {
        ENGINE_PROFILE_ZONE("GPU Submit");
        m_graphicsQueue->submit(
            *m_activeCommandBuffer, frame.imageAvailable.get(), frame.renderFinished.get(), frame.inFlight.get());
    }
    {
        ENGINE_PROFILE_ZONE("Present");
        m_swapchain->present(*m_graphicsQueue, frame.renderFinished.get());
    }

    m_activeCommandBuffer = nullptr;
    m_currentFrameIndex = (m_currentFrameIndex + 1) % m_frames.size();
    m_imguiFrameOpen = false;
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
        m_assetLoads.stop();
        return;
    }

    m_assetLoads.stop();
    if (m_device && m_device->uploadQueue()) { m_device->uploadQueue()->waitIdle(); }
    if (auto* diligent = dynamic_cast<rhi::diligent::DiligentDevice*>(m_device.get())) {
        if (auto* context = diligent->nativeImmediateContext()) context->WaitForIdle();
    }
    m_activeCommandBuffer = nullptr;
    m_frames.clear();
    m_imguiFrameOpen = false;
    m_uiCanvases.reset();
    m_imgui.reset();

    m_resourceManager.clear();
    m_uiImages.clear();
    m_uiImageAliases.clear();
    m_uiImagePaths.clear();
    m_shaderHotReloadEntries.clear();
    m_placeholderMesh.reset();
    m_unitCubeMesh.reset();
    m_unitPyramidMesh.reset();
    m_unitSphereMesh.reset();
    m_whiteTexture.reset();
    m_fallbackShader.reset();

    m_primitivePipeline.reset();
    m_primitivePipelineLayout.reset();
    m_primitiveVertexShader.reset();
    m_primitiveFragmentShader.reset();
    m_primitiveVertexBuffer.reset();

    m_swapchain.reset();
    m_device.reset();
    m_graphicsQueue = nullptr;
    m_window = nullptr;
    m_initialized = false;
    ENGINE_LOG_INFO("Renderer shutdown complete");
}

} // namespace engine::renderer
