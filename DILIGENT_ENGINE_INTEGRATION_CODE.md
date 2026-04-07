# DiligentEngine: релевантный код интеграции в TurboGameEngine

Этот файл собирает ключевые части проекта, связанные с DiligentEngine:

1. Подключение Diligent в CMake.
2. RHI-слой, где Diligent является единственным бэкендом.
3. Реализация `engine/rhi_diligent` (`DiligentDevice`).
4. Использование Diligent в `Renderer`.
5. Выбор `diligentDevice` через `config.json` и интеграция в `Application`.
6. Встроенный Diligent Samples Hub в UI.

---

## 1) CMake: Diligent как основной backend

### `CMakeLists.txt` (фрагменты)

```cmake
set(ENGINE_BACKEND "diligent" CACHE STRING "Runtime backend (only diligent is supported)" FORCE)
set_property(CACHE ENGINE_BACKEND PROPERTY STRINGS diligent)

set(DILIGENT_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/DiligentEngine")
if(NOT EXISTS "${DILIGENT_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "DiligentEngine sources were not found at ${DILIGENT_ENGINE_DIR}. Clone with submodules or run: git submodule update --init --recursive")
endif()

set(DILIGENT_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_FX ON CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_SAMPLES ${ENGINE_BUILD_DILIGENT_SAMPLES} CACHE BOOL "" FORCE)
set(DILIGENT_BUILD_SAMPLE_BASE_ONLY OFF CACHE BOOL "" FORCE)
add_subdirectory("${DILIGENT_ENGINE_DIR}")
```

```cmake
add_library(engine_rhi_diligent STATIC
    engine/rhi_diligent/DiligentDevice.cpp
)
target_include_directories(engine_rhi_diligent PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")

set(DILIGENT_RHI_LIBS)
foreach(_diligent_target IN ITEMS
    Diligent-GraphicsEngineD3D11-static
    Diligent-GraphicsEngineD3D12-static
    Diligent-GraphicsEngineOpenGL-static
    Diligent-GraphicsEngineVk-static
    Diligent-GraphicsEngineWebGPU-static
)
    if(TARGET ${_diligent_target})
        list(APPEND DILIGENT_RHI_LIBS ${_diligent_target})
    endif()
endforeach()

target_link_libraries(engine_rhi_diligent
PUBLIC
    engine_core
    Diligent-BuildSettings
PRIVATE
    glfw
    ${DILIGENT_RHI_LIBS}
)
target_compile_definitions(engine_rhi_diligent PRIVATE ENGINE_ENABLE_DILIGENT_BACKEND=1)
```

```cmake
add_library(engine_rhi STATIC
    engine/rhi/RHI.cpp
)
target_compile_definitions(engine_rhi PRIVATE ENGINE_ENABLE_DILIGENT_BACKEND=1)
target_link_libraries(engine_rhi PUBLIC engine_core PRIVATE engine_rhi_diligent)
```

---

## 2) RHI: единая точка входа в Diligent

### `engine/rhi/RHI.cpp`

```cpp
#include "engine/rhi/RHI.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "engine/rhi_diligent/DiligentDevice.h"

namespace engine::rhi {

std::unique_ptr<IDevice> createDevice(
    BackendType backend,
    const DeviceCreateDesc& desc,
    std::string* errorMessage) {
    if (backend != BackendType::Diligent) {
        if (errorMessage != nullptr) {
            *errorMessage =
                "Legacy backends were removed. TurboGameEngine now supports only BackendType::Diligent";
        }
        return nullptr;
    }

#if ENGINE_ENABLE_DILIGENT_BACKEND
    return diligent::createDiligentDevice(desc, errorMessage);
#else
    if (errorMessage != nullptr) {
        *errorMessage = "Diligent backend is disabled at build time";
    }
    return nullptr;
#endif
}

BackendType backendFromString(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lowered.empty() || lowered == "diligent") {
        return BackendType::Diligent;
    }

    return BackendType::Diligent;
}

const char* toString(BackendType backend) {
    switch (backend) {
        case BackendType::OpenGL:
            return "OpenGL (removed)";
        case BackendType::Vulkan:
            return "Vulkan (removed)";
        case BackendType::D3D12:
            return "D3D12 (removed)";
        case BackendType::Diligent:
            return "Diligent";
        default:
            return "Unknown";
    }
}

} // namespace engine::rhi
```

### `engine/rhi/Types.h` (параметры выбора Diligent device)

```cpp
struct DeviceCreateDesc final {
    platform::Window* window{nullptr};
    bool enableValidation{false};
    std::string diligentDeviceType{"auto"};
};
```

---

## 3) `engine/rhi_diligent`: создание device/swapchain/pipeline

### `engine/rhi_diligent/DiligentDevice.h` (API-слой)

```cpp
namespace engine::rhi::diligent {

class DiligentDevice final : public IDevice {
public:
    explicit DiligentDevice(const DeviceCreateDesc& desc);
    ~DiligentDevice() override;

    std::unique_ptr<ISwapchain> createSwapchain(const SwapchainDesc& desc) override;
    std::unique_ptr<ICommandBuffer> createCommandBuffer() override;
    std::unique_ptr<IBuffer> createBuffer(const BufferDesc& desc, const void* initialData) override;
    std::unique_ptr<IImage> createImage(const ImageDesc& desc, const void* initialData) override;
    std::unique_ptr<IShaderModule> createShaderModule(const ShaderModuleDesc& desc) override;
    std::unique_ptr<IPipelineLayout> createPipelineLayout() override;
    std::unique_ptr<IGraphicsPipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    std::unique_ptr<IBindGroupLayout> createBindGroupLayout() override;
    std::unique_ptr<IBindGroup> createBindGroup() override;
    std::unique_ptr<ISemaphore> createSemaphore() override;
    std::unique_ptr<IFence> createFence(bool signaled) override;
    IQueue& graphicsQueue() override;
    BackendType backendType() const override;

    Diligent::IRenderDevice* nativeRenderDevice() const;
    Diligent::IDeviceContext* nativeImmediateContext() const;
    Diligent::ISwapChain* nativeSwapChain() const;
};

std::unique_ptr<IDevice> createDiligentDevice(
    const DeviceCreateDesc& desc,
    std::string* errorMessage = nullptr);

} // namespace engine::rhi::diligent
```

### `engine/rhi_diligent/DiligentDevice.cpp`: выбор low-level API внутри Diligent

```cpp
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
    if (value == "d3d12" || value == "dx12") return Diligent::RENDER_DEVICE_TYPE_D3D12;
    if (value == "d3d11" || value == "dx11") return Diligent::RENDER_DEVICE_TYPE_D3D11;
    if (value == "vk" || value == "vulkan") return Diligent::RENDER_DEVICE_TYPE_VULKAN;
    if (value == "gl" || value == "opengl" || value == "gles") return Diligent::RENDER_DEVICE_TYPE_GL;
    if (value == "webgpu" || value == "wgpu") return Diligent::RENDER_DEVICE_TYPE_WEBGPU;
    return chooseAutoDeviceType();
}
```

### `engine/rhi_diligent/DiligentDevice.cpp`: инициализация DiligentDevice

```cpp
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
        // D3D11 / D3D12 / Vulkan / OpenGL / WebGPU:
        // LoadAndGetEngineFactory* + CreateDeviceAndContexts*
        // ...
        default:
            throw std::runtime_error("Requested Diligent device type is not supported by this build");
    }

    if (!m_impl->renderDevice || !m_impl->immediateContext) {
        throw std::runtime_error("Diligent failed to create render device or immediate context");
    }
}
```

### `engine/rhi_diligent/DiligentDevice.cpp`: swapchain + shader + pipeline

```cpp
std::unique_ptr<ISwapchain> DiligentDevice::createSwapchain(const SwapchainDesc& desc) {
    if (!m_impl || !m_impl->renderDevice || !m_impl->immediateContext || desc.width == 0 || desc.height == 0) {
        return nullptr;
    }

    const auto nativeDesc = makeSwapChainDesc(desc);
    if (!m_impl->swapChain) {
        // CreateSwapChainD3D11 / D3D12 / Vk / WebGPU
        // OpenGL path creates swapchain during device creation
        // ...
    } else {
        m_impl->swapChain->Resize(
            static_cast<Diligent::Uint32>(desc.width),
            static_cast<Diligent::Uint32>(desc.height),
            Diligent::SURFACE_TRANSFORM_OPTIMAL);
    }

    if (!m_impl->swapChain) {
        return nullptr;
    }

    if (m_impl->baseFactory) {
        m_impl->baseFactory->CreateDefaultShaderSourceStreamFactory(".", &m_impl->shaderSourceFactory);
    }

    return std::make_unique<DiligentSwapchain>(*m_impl, desc);
}
```

```cpp
std::unique_ptr<IShaderModule> DiligentDevice::createShaderModule(const ShaderModuleDesc& desc) {
    Diligent::ShaderCreateInfo createInfo{};
    createInfo.FilePath = desc.path.c_str();
    createInfo.EntryPoint = desc.entryPoint.c_str();
    createInfo.Desc.ShaderType = toDiligentShaderType(desc.stage);
    createInfo.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    createInfo.Desc.UseCombinedTextureSamplers = Diligent::True;
    createInfo.pShaderSourceStreamFactory = m_impl->shaderSourceFactory;
    // ...
}
```

```cpp
std::unique_ptr<IGraphicsPipeline> DiligentDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    // Вытягивает Diligent shader objects, собирает PSO, input layout,
    // static constant buffers, texture sampler bindings и SRB.
    // Возвращает DiligentGraphicsPipeline.
    // ...
}
```

### `engine/rhi_diligent/DiligentDevice.cpp`: фабрика устройства

```cpp
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
```

---

## 4) `Renderer`: использование Diligent через RHI

### `engine/renderer/Renderer.cpp` (инициализация)

```cpp
bool Renderer::init(
    platform::Window& window,
    rhi::BackendType backend,
    bool vsync,
    const std::string& diligentDeviceType) {
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

    rhi::SwapchainDesc swapchainDesc{};
    swapchainDesc.width = static_cast<std::uint32_t>(window.width());
    swapchainDesc.height = static_cast<std::uint32_t>(window.height());
    swapchainDesc.vsync = vsync;
    m_swapchain = m_device->createSwapchain(swapchainDesc);
    // ...
}
```

### `engine/renderer/Renderer.cpp` (ImGui на Diligent)

```cpp
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

    m_imgui = std::unique_ptr<Diligent::ImGuiImplDiligent, ImGuiImplDiligentDeleter>(
        new Diligent::ImGuiImplDiligent(Diligent::ImGuiDiligentCreateInfo{nativeDevice, nativeSwapChain->GetDesc()}));
    // ...
}
```

---

## 5) `Application`: выбор device и игровой цикл

### `engine/core/Config.h` / `Config.cpp`

```cpp
struct EngineConfig final {
    // ...
    std::string diligentDevice{
#if defined(_WIN32)
        "d3d12"
#else
        "vk"
#endif
    };
    // ...
};
```

```cpp
config.diligentDevice = jsonData.value("diligentDevice", config.diligentDevice);
```

### `engine/core/Application.cpp` (инициализация)

```cpp
bool Application::init() {
    m_config = Config::load("config.json");

    platform::WindowDesc windowDesc{};
    windowDesc.width = m_config.width;
    windowDesc.height = m_config.height;
    windowDesc.title = m_config.title;
    windowDesc.vsync = m_config.vsync;

    const std::string deviceTypeLower = toLowerCopy(m_config.diligentDevice);
    windowDesc.useOpenGLContext = (deviceTypeLower == "gl" || deviceTypeLower == "opengl" || deviceTypeLower == "gles");

    m_window = platform::Window::create(windowDesc);
    if (!m_renderer.init(*m_window, m_backend, m_config.vsync, m_config.diligentDevice)) {
        return false;
    }

    m_selectedSampleBackendMode = modeIndexFromString(m_config.diligentDevice);
    discoverDiligentSamples();
    // ...
}
```

### `app/main.cpp`

```cpp
int main() {
    constexpr const char* backendName = ENGINE_BACKEND_NAME;
    const engine::rhi::BackendType backend = engine::rhi::backendFromString(backendName);

    engine::core::Application app(backend);
    if (!app.init()) {
        return 1;
    }

    return app.run();
}
```

---

## 6) Diligent Samples Hub внутри приложения

### `engine/core/Application.cpp` (UI-фрагмент)

```cpp
if (m_workspaceMode == WorkspaceMode::DiligentSamplesHub) {
    if (ImGui::Button("Refresh Diligent demo list")) {
        discoverDiligentSamples();
    }
    ImGui::Combo("Sample backend (--mode)", &m_selectedSampleBackendMode, ...);
    // список demo + кнопка запуска
    if (ImGui::Button("Run selected demo")) {
        launchSelectedDiligentSample();
    }
}
```

### `engine/core/Application.cpp` (поиск и запуск демо)

```cpp
void Application::discoverDiligentSamples() {
    const auto samplesRoot = projectRoot / "third_party" / "DiligentEngine" / "DiligentSamples";
    // сканирует Samples и Tutorials, формирует список доступных исполняемых файлов
}

bool Application::launchSelectedDiligentSample() {
    const std::string mode = modeNameFromIndex(m_selectedSampleBackendMode);
    // запускает выбранный demo с аргументом: --mode <d3d11|d3d12|vk|gl|webgpu>
}
```
