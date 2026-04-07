#include "engine/renderer/Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include "engine/core/Log.h"
#include "engine/platform/Events.h"
#include "engine/platform/Window.h"
#include "engine/rhi_diligent/DiligentDevice.h"
#include "engine/resources/loaders.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/GeometryPrimitives.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h"
#include "third_party/DiligentEngine/DiligentTools/Imgui/interface/ImGuiImplDiligent.hpp"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace engine::renderer {

namespace {

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
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::filesystem::file_time_type::min();
    }

    const auto writeTime = std::filesystem::last_write_time(path, ec);
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
        case platform::KeyCode::Enter:
            return ImGuiKey_Enter;
        case platform::KeyCode::Escape:
            return ImGuiKey_Escape;
        case platform::KeyCode::Left:
            return ImGuiKey_LeftArrow;
        case platform::KeyCode::Right:
            return ImGuiKey_RightArrow;
        case platform::KeyCode::Up:
            return ImGuiKey_UpArrow;
        case platform::KeyCode::Down:
            return ImGuiKey_DownArrow;
        default:
            return std::nullopt;
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
    startAssetWorkers();

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

void Renderer::startAssetWorkers() {
    stopAssetWorkers();
    m_assetWorkersStopping = false;

    unsigned int workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0U) {
        workerCount = 2U;
    }
    workerCount = std::clamp(workerCount > 1U ? workerCount - 1U : 1U, 1U, 4U);

    m_assetWorkers.reserve(workerCount);
    for (unsigned int index = 0; index < workerCount; ++index) {
        m_assetWorkers.emplace_back([this]() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_assetTaskMutex);
                    m_assetTaskCv.wait(lock, [this]() { return m_assetWorkersStopping || !m_assetTasks.empty(); });
                    if (m_assetWorkersStopping && m_assetTasks.empty()) {
                        return;
                    }

                    task = std::move(m_assetTasks.front());
                    m_assetTasks.pop_front();
                }

                if (task) {
                    task();
                }
            }
        });
    }
}

void Renderer::stopAssetWorkers() {
    {
        std::lock_guard<std::mutex> lock(m_assetTaskMutex);
        m_assetWorkersStopping = true;
        m_assetTasks.clear();
    }
    m_assetTaskCv.notify_all();

    for (auto& worker : m_assetWorkers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_assetWorkers.clear();

    {
        std::lock_guard<std::mutex> lock(m_mainThreadTaskMutex);
        m_mainThreadTasks.clear();
    }
}

void Renderer::enqueueBackgroundTask(std::function<void()> task) {
    if (!task) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_assetTaskMutex);
        if (m_assetWorkersStopping) {
            return;
        }
        m_assetTasks.push_back(std::move(task));
    }
    m_assetTaskCv.notify_one();
}

void Renderer::enqueueMainThreadTask(std::function<void()> task) {
    if (!task) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mainThreadTaskMutex);
    m_mainThreadTasks.push_back(std::move(task));
}

void Renderer::processPendingMainThreadTasks() {
    std::deque<std::function<void()>> pendingTasks;
    {
        std::lock_guard<std::mutex> lock(m_mainThreadTaskMutex);
        pendingTasks.swap(m_mainThreadTasks);
    }

    for (auto& task : pendingTasks) {
        if (task) {
            task();
        }
    }
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

    const std::string vertexPath = "assets/shaders_hlsl/basic.vert.hlsl";
    const std::string fragmentPath = "assets/shaders_hlsl/basic.frag.hlsl";

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
        ImGui::StyleColorsDark();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
        "assets/shaders_hlsl/textured.vert.hlsl",
        "assets/shaders_hlsl/textured.frag.hlsl");
    if (!shader) {
        ENGINE_LOG_ERROR("Failed to create fallback shader program");
        return nullptr;
    }
    return shader;
}

std::shared_ptr<resources::Mesh> Renderer::requestMeshLoadFromDisk(const std::string& path) {
    if (path == kProceduralCubeMeshId) {
        return m_unitCubeMesh;
    }
    if (path == kProceduralPyramidMeshId) {
        return m_unitPyramidMesh;
    }
    if (path == kProceduralSphereMeshId) {
        return m_unitSphereMesh;
    }

    auto mesh = std::make_shared<resources::Mesh>();
    mesh->path = path;
    mesh->setLoadState(resources::ResourceLoadState::Loading);

    enqueueBackgroundTask([this, mesh, path]() {
        resources::MeshData meshData;
        std::string errorMessage;
        const bool loaded = resources::loadMeshData(path, meshData, &errorMessage);

        enqueueMainThreadTask([this, mesh, path, meshData = std::move(meshData), errorMessage = std::move(errorMessage), loaded]() mutable {
            if (!loaded) {
                mesh->lastError = errorMessage;
                mesh->setLoadState(resources::ResourceLoadState::Failed);
                ENGINE_LOG_ERROR("Mesh load failed '{}': {}", path, mesh->lastError);
                return;
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
                return;
            }

            mesh->lastError.clear();
            mesh->setLoadState(resources::ResourceLoadState::Loaded);
            ENGINE_LOG_INFO(
                "Mesh loaded '{}': vertices={}, indices={}",
                path,
                mesh->vertexCount,
                mesh->indexCount);
        });
    });

    return mesh;
}

bool Renderer::uploadMeshToGpu(resources::Mesh& mesh) const {
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

    enqueueBackgroundTask([this, texture, path]() {
        resources::TextureData textureData;
        std::string errorMessage;
        const bool loaded = resources::loadTextureDataRgba8(path, textureData, &errorMessage);

        enqueueMainThreadTask(
            [this, texture, path, textureData = std::move(textureData), errorMessage = std::move(errorMessage), loaded]() mutable {
                if (!loaded) {
                    texture->lastError = errorMessage;
                    texture->setLoadState(resources::ResourceLoadState::Failed);
                    ENGINE_LOG_ERROR("Texture load failed '{}': {}", path, texture->lastError);
                    return;
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
                    return;
                }

                texture->lastError.clear();
                texture->setLoadState(resources::ResourceLoadState::Loaded);
                ENGINE_LOG_INFO(
                    "Texture loaded '{}': {}x{} channels={}",
                    path,
                    texture->data.width,
                    texture->data.height,
                    texture->data.channels);
            });
    });

    return texture;
}

std::shared_ptr<resources::ShaderProgram> Renderer::createShaderProgram(
    const std::string& key,
    const std::string& vertexPath,
    const std::string& fragmentPath) {
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

    enqueueBackgroundTask([this, program, path]() {
        resources::ShaderProgramSource source{};
        std::string errorMessage;
        const bool loaded = resources::loadShaderProgramSource(path, source, &errorMessage);

        enqueueMainThreadTask(
            [this, program, path, source = std::move(source), errorMessage = std::move(errorMessage), loaded]() mutable {
                if (!loaded) {
                    program->lastError = errorMessage;
                    program->setLoadState(resources::ResourceLoadState::Failed);
                    ENGINE_LOG_ERROR("Shader source load failed '{}': {}", path, program->lastError);
                    return;
                }

                auto compiledProgram = createShaderProgram(path, source.vertexPath, source.fragmentPath);
                if (!compiledProgram) {
                    program->lastError = "Shader program creation failed";
                    program->setLoadState(resources::ResourceLoadState::Failed);
                    return;
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
            });
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
            const auto key = toImGuiKey(event.key);
            if (key.has_value()) {
                io.AddKeyEvent(*key, event.type == platform::EventType::KeyPressed);
            }
            break;
        }

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
    if (!m_initialized || !m_swapchain || m_frames.empty()) {
        return;
    }

    processPendingMainThreadTasks();

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
    auto program = createShaderProgram(key, vertexPath, fragmentPath);
    if (!program) {
        return nullptr;
    }

    resources::ShaderProgramSource source{};
    source.descriptorPath = key;
    {
        std::error_code ec;
        source.descriptorIsFile = std::filesystem::exists(key, ec) && !ec;
    }
    source.vertexPath = vertexPath;
    source.fragmentPath = fragmentPath;
    registerShaderForHotReload(key, program, source);
    return program;
}

std::shared_ptr<resources::Mesh> Renderer::loadMesh(const std::string& path) {
    return m_resourceManager.load<resources::Mesh>(path);
}

std::shared_ptr<resources::Texture> Renderer::loadTexture(const std::string& path) {
    return m_resourceManager.load<resources::Texture>(path);
}

std::shared_ptr<resources::ShaderProgram> Renderer::loadShaderProgram(const std::string& path) {
    return m_resourceManager.load<resources::ShaderProgram>(path);
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
    processPendingMainThreadTasks();

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
            if (!resources::loadShaderProgramSource(entry.descriptorPath, latestSource, &sourceError)) {
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
    if (!m_initialized || !m_swapchain || m_frames.empty() || !m_activeCommandBuffer) {
        m_imguiFrameOpen = false;
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
        stopAssetWorkers();
        return;
    }

    stopAssetWorkers();
    m_activeCommandBuffer = nullptr;
    m_frames.clear();
    m_imguiFrameOpen = false;
    m_imgui.reset();

    m_resourceManager.clear();
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
