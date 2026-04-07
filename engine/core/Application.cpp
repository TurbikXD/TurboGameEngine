#include "engine/core/Application.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "engine/core/Log.h"
#include "engine/core/Time.h"
#include "engine/ecs/components.h"
#include "engine/game/GameplayState.h"
#include "engine/game/MenuState.h"
#include "engine/game/PauseState.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/rhi/RHI.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace {

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

int modeIndexFromString(const std::string& mode) {
    const std::string lowered = toLowerCopy(mode);
    if (lowered == "d3d11" || lowered == "dx11") {
        return 0;
    }
    if (lowered == "d3d12" || lowered == "dx12") {
        return 1;
    }
    if (lowered == "vk" || lowered == "vulkan") {
        return 2;
    }
    if (lowered == "gl" || lowered == "opengl" || lowered == "gles") {
        return 3;
    }
    if (lowered == "webgpu" || lowered == "wgpu") {
        return 4;
    }
    return 1;
}

const char* modeNameFromIndex(const int index) {
    static constexpr std::array<const char*, 5> kModeNames{
        "d3d11",
        "d3d12",
        "vk",
        "gl",
        "webgpu"};
    const auto safeIndex = static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(kModeNames.size() - 1U)));
    return kModeNames[safeIndex];
}

std::filesystem::path getExecutableDirectory() {
#if defined(_WIN32)
    std::wstring pathBuffer(static_cast<std::size_t>(32768), L'\0');
    const DWORD copied = GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (copied > 0U && copied < pathBuffer.size()) {
        pathBuffer.resize(copied);
        return std::filesystem::path(pathBuffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path findProjectRootFrom(std::filesystem::path startDir) {
    std::error_code ec;
    for (int i = 0; i < 10; ++i) {
        if (startDir.empty()) {
            break;
        }

        const auto diligentSamplesDir = startDir / "third_party" / "DiligentEngine" / "DiligentSamples";
        if (std::filesystem::exists(diligentSamplesDir / "Tutorials", ec) &&
            std::filesystem::exists(diligentSamplesDir / "Samples", ec)) {
            return startDir;
        }

        const auto parent = startDir.parent_path();
        if (parent == startDir) {
            break;
        }
        startDir = parent;
    }
    return {};
}

std::filesystem::path resolveSampleBinaryPath(const std::filesystem::path& executableDir, const std::string& sampleName) {
#if defined(_WIN32)
    constexpr const char* ext = ".exe";
#else
    constexpr const char* ext = "";
#endif
    const std::array<std::filesystem::path, 4> candidates{
        executableDir / (sampleName + ext),
        executableDir / "Debug" / (sampleName + ext),
        executableDir / "Release" / (sampleName + ext),
        executableDir.parent_path() / (sampleName + ext)};

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }

    const std::array<std::filesystem::path, 2> nestedCandidates{
        executableDir / "third_party" / "DiligentEngine" / "DiligentSamples" / "Samples" / sampleName / (sampleName + ext),
        executableDir / "third_party" / "DiligentEngine" / "DiligentSamples" / "Tutorials" / sampleName / (sampleName + ext)};
    for (const auto& candidate : nestedCandidates) {
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }

    const auto diligentSamplesBinRoot = executableDir / "third_party" / "DiligentEngine" / "DiligentSamples";
    if (std::filesystem::exists(diligentSamplesBinRoot, ec) && !ec) {
        const auto expectedFilename = sampleName + ext;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(diligentSamplesBinRoot, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec) || ec) {
                continue;
            }
            if (entry.path().filename().string() == expectedFilename) {
                return entry.path();
            }
        }
    }

    return candidates[0];
}

std::filesystem::file_time_type getFileWriteTimeOrMin(const std::filesystem::path& path) {
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

#if defined(_WIN32)
bool launchDetachedProcess(
    const std::filesystem::path& executablePath,
    const std::string& backendMode,
    const std::filesystem::path& workingDirectory,
    std::string& outError) {
    std::wstring commandLine = L"\"" + executablePath.wstring() + L"\" --mode ";
    commandLine += std::wstring(backendMode.begin(), backendMode.end());

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    const BOOL result = CreateProcessW(
        nullptr,
        mutableCmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP,
        nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.wstring().c_str(),
        &startupInfo,
        &processInfo);
    if (result == FALSE) {
        outError = "CreateProcessW failed, code=" + std::to_string(static_cast<unsigned long>(GetLastError()));
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}
#else
bool launchDetachedProcess(
    const std::filesystem::path& executablePath,
    const std::string& backendMode,
    const std::filesystem::path& workingDirectory,
    std::string& outError) {
    (void)workingDirectory;
    const std::string command = "\"" + executablePath.string() + "\" --mode " + backendMode + " &";
    if (std::system(command.c_str()) != 0) {
        outError = "Failed to spawn process via system()";
        return false;
    }
    return true;
}
#endif

} // namespace

namespace engine::core {

Application::Application(rhi::BackendType backend) : m_backend(backend) {}

Application::~Application() {
    shutdown();
}

bool Application::init() {
    Log::init();
    m_logInitialized = true;
    ENGINE_LOG_INFO("Application init started");

    m_config = Config::load("config.json");
    initializeConfigHotReload();

    platform::WindowDesc windowDesc{};
    windowDesc.width = m_config.width;
    windowDesc.height = m_config.height;
    windowDesc.title = m_config.title;
    windowDesc.vsync = m_config.vsync;
    const std::string deviceTypeLower = [&]() {
        std::string value = m_config.diligentDevice;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }();
    windowDesc.useOpenGLContext = (deviceTypeLower == "gl" || deviceTypeLower == "opengl" ||
                                   deviceTypeLower == "gles");

    try {
        m_window = platform::Window::create(windowDesc);
    } catch (const std::exception& ex) {
        ENGINE_LOG_ERROR("Window creation failed: {}", ex.what());
        return false;
    }

    platform::Input::reset();
    m_window->setEventCallback([this](const platform::Event& event) { onEvent(event); });
    m_stateStack.setWindow(m_window.get());

    if (!m_renderer.init(*m_window, m_backend, m_config.vsync, m_config.diligentDevice)) {
        ENGINE_LOG_ERROR("Renderer failed to initialize");
        return false;
    }

    if (m_config.initialState == "gameplay") {
        m_stateStack.push(std::make_unique<game::GameplayState>(m_stateStack));
    } else if (m_config.initialState == "pause") {
        m_stateStack.push(std::make_unique<game::GameplayState>(m_stateStack));
        m_stateStack.push(std::make_unique<game::PauseState>(m_stateStack));
    } else {
        m_stateStack.push(std::make_unique<game::MenuState>(m_stateStack, m_config.diligentDevice));
    }
    m_stateStack.applyPendingChanges();
    m_selectedSampleBackendMode = modeIndexFromString(m_config.diligentDevice);
    discoverDiligentSamples();

    m_running = true;
    m_initialized = true;
    ENGINE_LOG_INFO("Application init complete, backend={}", rhi::toString(m_backend));
    return true;
}

int Application::run() {
    if (!m_initialized) {
        return 1;
    }

    FrameTimer timer;
    timer.reset();

    constexpr double fixedDt = 1.0 / 60.0;
    double accumulator = 0.0;
    TimeStatsAggregator stats(1.0);

    while (m_running && m_window && !m_window->shouldClose()) {
        platform::InputManager::beginFrame();
        m_window->pollEvents();
        pollRuntimeHotReload();

        double frameDt = timer.tickSeconds();
        frameDt = std::min(frameDt, 0.25);
        m_workspaceSampleTime += frameDt;

        if (m_workspaceMode == WorkspaceMode::EngineStates) {
            accumulator += frameDt;
            while (accumulator >= fixedDt) {
                m_stateStack.update(fixedDt);
                m_stateStack.applyPendingChanges();
                accumulator -= fixedDt;
            }
        }

        const glm::vec4 clearColor{
            m_config.clearColor[0], m_config.clearColor[1], m_config.clearColor[2], m_config.clearColor[3]};
        m_renderer.beginFrame(clearColor);
        m_renderer.beginImGuiFrame(static_cast<float>(frameDt));
        renderWorkspace(frameDt);
        renderWorkspaceUi(frameDt);
        m_renderer.renderImGui();
        m_renderer.endFrame();

        stats.sample(frameDt);
        TimeStats sampled{};
        if (stats.consume(sampled)) {
            ENGINE_LOG_INFO(
                "Frame dt stats: avg={:.3f}ms min={:.3f}ms max={:.3f}ms samples={}",
                sampled.avgMs,
                sampled.minMs,
                sampled.maxMs,
                sampled.sampleCount);
        }

        if (m_workspaceMode == WorkspaceMode::EngineStates && m_stateStack.empty()) {
            ENGINE_LOG_WARN("State stack empty, exiting");
            m_running = false;
        }
    }

    shutdown();
    return 0;
}

void Application::shutdown() {
    if (!m_initialized && m_window == nullptr && !m_logInitialized) {
        return;
    }

    if (m_initialized) {
        ENGINE_LOG_INFO("Application shutdown started");
    }
    m_running = false;
    m_stateStack.clear();
    m_stateStack.applyPendingChanges();
    m_stateStack.setWindow(nullptr);
    m_tutorial03Mesh.reset();
    m_tutorial03Texture.reset();
    m_tutorial03Shader.reset();
    m_renderer.shutdown();
    m_window.reset();
    m_initialized = false;
    if (m_logInitialized) {
        ENGINE_LOG_INFO("Application shutdown complete");
        Log::shutdown();
        m_logInitialized = false;
    }
}

void Application::renderWorkspace(const double frameDtSeconds) {
    switch (m_workspaceMode) {
        case WorkspaceMode::EngineStates:
            m_stateStack.render(m_renderer);
            break;

        case WorkspaceMode::Tutorial01HelloTriangle:
            renderTutorial01HelloTriangle();
            break;

        case WorkspaceMode::Tutorial03Texturing:
            renderTutorial03Texturing(frameDtSeconds);
            break;

        case WorkspaceMode::ImGuiDemo:
            if (m_renderer.isImGuiEnabled() && m_showImGuiDemoWindow) {
                ImGui::ShowDemoWindow(&m_showImGuiDemoWindow);
            } else {
                m_renderer.drawTestPrimitive(renderer::Transform2D{});
            }
            break;

        case WorkspaceMode::DiligentSamplesHub:
            m_renderer.drawTestPrimitive(renderer::Transform2D{});
            break;
    }
}

void Application::renderWorkspaceUi(const double frameDtSeconds) {
    if (!m_renderer.isImGuiEnabled()) {
        return;
    }

    static constexpr std::array<const char*, 5> kWorkspaceItems{
        "Engine States",
        "Diligent Tutorial01: Hello Triangle",
        "Diligent Tutorial03: Texturing",
        "Diligent Sample: ImGui Demo",
        "Diligent Samples Hub (all demos)"};
    static constexpr std::array<const char*, 5> kSampleBackendModes{
        "d3d11",
        "d3d12",
        "vk",
        "gl",
        "webgpu"};

    int selectedMode = static_cast<int>(m_workspaceMode);

    ImGui::SetNextWindowPos(ImVec2(12.0F, 12.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0F, 0.0F), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("TurboGameEngine Workspace")) {
        ImGui::TextUnformatted("Single-entry workflow from one app process.");
        ImGui::Separator();
        ImGui::Text("Backend: %s", m_config.diligentDevice.c_str());
        ImGui::TextUnformatted("Diligent Samples Hub can launch every discovered demo target.");
        ImGui::Text("Frame dt: %.3f ms", frameDtSeconds * 1000.0);

        if (ImGui::Combo(
                "Active View",
                &selectedMode,
                kWorkspaceItems.data(),
                static_cast<int>(kWorkspaceItems.size()))) {
            m_workspaceMode = static_cast<WorkspaceMode>(selectedMode);
            if (m_window != nullptr && m_workspaceMode != WorkspaceMode::EngineStates) {
                m_window->setCursorMode(platform::CursorMode::Normal);
            }
            ENGINE_LOG_INFO("Workspace mode switched to '{}'", workspaceModeLabel(m_workspaceMode));
        }

        if (m_workspaceMode == WorkspaceMode::EngineStates) {
            ImGui::TextUnformatted("Engine state controls:");
            ImGui::BulletText("Menu: Enter -> Gameplay");
            ImGui::BulletText("Gameplay: Hold Right Mouse + WASDQE for UE-style camera");
            ImGui::BulletText("Gameplay: Mouse Wheel adjusts move speed, Shift boosts fly speed");
            ImGui::BulletText("Gameplay: Start physics from UI, Space launches the striker");
            ImGui::BulletText("Gameplay: F1 toggles collider debug");
            ImGui::BulletText("Gameplay: Esc -> Pause");
            ImGui::BulletText("Pause: Esc -> Resume");
            m_stateStack.renderUi(m_renderer);
        } else if (m_workspaceMode == WorkspaceMode::Tutorial01HelloTriangle) {
            ImGui::TextWrapped("Renders a rotating triangle using the engine pipeline (Tutorial01-style).");
        } else if (m_workspaceMode == WorkspaceMode::Tutorial03Texturing) {
            ImGui::TextWrapped("Renders textured geometry using assets/shaders_hlsl/textured.* (Tutorial03-style).");
        } else if (m_workspaceMode == WorkspaceMode::ImGuiDemo) {
            ImGui::Checkbox("Show Dear ImGui Demo Window", &m_showImGuiDemoWindow);
        } else if (m_workspaceMode == WorkspaceMode::DiligentSamplesHub) {
            if (ImGui::Button("Refresh Diligent demo list")) {
                discoverDiligentSamples();
            }
            ImGui::Combo(
                "Sample backend (--mode)",
                &m_selectedSampleBackendMode,
                kSampleBackendModes.data(),
                static_cast<int>(kSampleBackendModes.size()));

            ImGui::TextWrapped("%s", m_diligentSamplesStatus.c_str());

            ImGui::BeginChild("DiligentSampleList", ImVec2(0.0F, 260.0F), true);
            for (std::size_t i = 0; i < m_diligentSamples.size(); ++i) {
                const auto& sample = m_diligentSamples[i];
                std::string label = sample.name + " [" + sample.category + "]";
                if (!sample.executableExists) {
                    label += " (not built)";
                }
                const bool selected = (i == m_selectedDiligentSample);
                if (ImGui::Selectable(label.c_str(), selected)) {
                    m_selectedDiligentSample = i;
                }
            }
            ImGui::EndChild();

            if (ImGui::Button("Run selected demo")) {
                launchSelectedDiligentSample();
            }

            if (!m_sampleLaunchStatus.empty()) {
                ImGui::TextWrapped("%s", m_sampleLaunchStatus.c_str());
            }

            if (!m_diligentSamples.empty() && m_selectedDiligentSample < m_diligentSamples.size()) {
                const auto& sample = m_diligentSamples[m_selectedDiligentSample];
                ImGui::Separator();
                ImGui::Text("Selected: %s", sample.name.c_str());
                ImGui::TextWrapped("Executable: %s", sample.executablePath.c_str());
                if (!sample.executableExists) {
                    ImGui::TextWrapped(
                        "Build this demo first, then run from this hub. Recommended: build_diligent_all_samples target.");
                }
            }
        }
    }
    ImGui::End();
}

void Application::renderTutorial01HelloTriangle() {
    const float angle = static_cast<float>(m_workspaceSampleTime * 1.1);
    glm::mat4 transform(1.0F);
    transform = glm::rotate(transform, angle, glm::vec3(0.0F, 0.0F, 1.0F));
    transform = glm::scale(transform, glm::vec3(1.15F, 1.15F, 1.0F));
    m_renderer.drawPrimitive(ecs::PrimitiveType::Triangle, transform, glm::vec4(1.0F, 1.0F, 1.0F, 1.0F));
}

void Application::renderTutorial03Texturing(const double frameDtSeconds) {
    (void)frameDtSeconds;
    ensureTutorial03Resources();
    if (!m_tutorial03Mesh || !m_tutorial03Texture || !m_tutorial03Shader || !m_tutorial03Mesh->isReady() ||
        !m_tutorial03Texture->isReady() || !m_tutorial03Shader->isReady()) {
        m_renderer.drawTestPrimitive(renderer::Transform2D{});
        return;
    }

    const glm::mat4 projection = glm::ortho(-2.0F, 2.0F, -1.6F, 1.6F, -5.0F, 5.0F);

    for (int i = 0; i < 3; ++i) {
        const float offsetX = static_cast<float>(i - 1) * 1.15F;
        glm::mat4 model(1.0F);
        model = glm::translate(model, glm::vec3(offsetX, -0.15F, 0.0F));
        model = glm::rotate(
            model,
            static_cast<float>(m_workspaceSampleTime * 0.9 + static_cast<double>(i) * 0.6),
            glm::vec3(0.0F, 0.0F, 1.0F));
        model = glm::scale(model, glm::vec3(0.85F, 0.85F, 1.0F));

        m_renderer.drawMesh(
            *m_tutorial03Mesh,
            *m_tutorial03Shader,
            *m_tutorial03Texture,
            model,
            projection,
            glm::vec4(1.0F, 1.0F, 1.0F, 1.0F));
    }
}

void Application::ensureTutorial03Resources() {
    if (!m_tutorial03Mesh) {
        m_tutorial03Mesh = m_renderer.loadMesh("assets/models/plane.obj");
    }
    if (!m_tutorial03Texture) {
        m_tutorial03Texture = m_renderer.loadTexture("assets/textures/DGLogo.png");
    }
    if (!m_tutorial03Texture) {
        m_tutorial03Texture = m_renderer.loadTexture("assets/textures/checker.png");
    }
    if (!m_tutorial03Shader) {
        m_tutorial03Shader = m_renderer.loadShaderProgram("assets/shaders_hlsl/textured.shader.json");
    }
}

void Application::initializeConfigHotReload() {
    m_configWriteTime = getFileWriteTimeOrMin(std::filesystem::path{"config.json"});
    m_configWatchActive = (m_configWriteTime != std::filesystem::file_time_type::min());
}

void Application::pollRuntimeHotReload() {
    m_renderer.pollHotReload();

    const auto configPath = std::filesystem::path{"config.json"};
    const auto currentWriteTime = getFileWriteTimeOrMin(configPath);
    if (currentWriteTime == std::filesystem::file_time_type::min()) {
        return;
    }

    if (!m_configWatchActive) {
        m_configWatchActive = true;
    }

    if (currentWriteTime == m_configWriteTime) {
        return;
    }

    m_configWriteTime = currentWriteTime;
    const EngineConfig reloadedConfig = Config::load(configPath.string());
    const bool titleChanged = reloadedConfig.title != m_config.title;
    const bool vsyncChanged = reloadedConfig.vsync != m_config.vsync;
    const bool deviceChanged = toLowerCopy(reloadedConfig.diligentDevice) != toLowerCopy(m_config.diligentDevice);

    m_config = reloadedConfig;
    if (m_window != nullptr) {
        if (titleChanged) {
            m_window->setTitle(m_config.title);
        }
        if (vsyncChanged) {
            m_window->setVSync(m_config.vsync);
        }
    }

    if (deviceChanged) {
        ENGINE_LOG_WARN(
            "Config hot-reload: diligentDevice changed to '{}'; restart app to recreate renderer backend",
            m_config.diligentDevice);
    }

    ENGINE_LOG_INFO(
        "Config hot-reloaded: title='{}', vsync={}, clearColor=({}, {}, {}, {})",
        m_config.title,
        m_config.vsync,
        m_config.clearColor[0],
        m_config.clearColor[1],
        m_config.clearColor[2],
        m_config.clearColor[3]);
}

void Application::discoverDiligentSamples() {
    std::string selectedSampleName;
    if (!m_diligentSamples.empty() && m_selectedDiligentSample < m_diligentSamples.size()) {
        selectedSampleName = m_diligentSamples[m_selectedDiligentSample].name;
    }

    m_diligentSamples.clear();
    m_sampleLaunchStatus.clear();

    const std::filesystem::path executableDir = getExecutableDirectory();
    std::filesystem::path projectRoot = findProjectRootFrom(executableDir);
    if (projectRoot.empty()) {
        projectRoot = findProjectRootFrom(std::filesystem::current_path());
    }
    if (projectRoot.empty()) {
        m_diligentSamplesStatus =
            "Diligent sample source tree was not found. Expected: third_party/DiligentEngine/DiligentSamples";
        return;
    }

    const auto samplesRoot = projectRoot / "third_party" / "DiligentEngine" / "DiligentSamples";
    auto appendSamplesFromDirectory =
        [this, &executableDir](const std::filesystem::path& sourceDir, const char* category, const bool tutorialsOnly) {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(sourceDir, ec)) {
                if (ec) {
                    continue;
                }
                if (!entry.is_directory(ec) || ec) {
                    continue;
                }

                const std::string sampleName = entry.path().filename().string();
                if (sampleName.empty() || sampleName == "CMakeFiles") {
                    continue;
                }
                if (tutorialsOnly && sampleName.rfind("Tutorial", 0U) != 0U) {
                    continue;
                }

                const std::filesystem::path binaryPath = resolveSampleBinaryPath(executableDir, sampleName);
                const bool exists = std::filesystem::exists(binaryPath, ec) && !ec;

                DiligentSampleEntry sample{};
                sample.name = sampleName;
                sample.category = category;
                sample.executablePath = binaryPath.string();
                sample.executableExists = exists;
                m_diligentSamples.push_back(std::move(sample));
            }
        };

    appendSamplesFromDirectory(samplesRoot / "Samples", "Sample", false);
    appendSamplesFromDirectory(samplesRoot / "Tutorials", "Tutorial", true);

    std::sort(
        m_diligentSamples.begin(),
        m_diligentSamples.end(),
        [](const DiligentSampleEntry& lhs, const DiligentSampleEntry& rhs) {
            if (lhs.category != rhs.category) {
                return lhs.category < rhs.category;
            }
            return lhs.name < rhs.name;
        });

    if (!selectedSampleName.empty()) {
        const auto it = std::find_if(
            m_diligentSamples.begin(),
            m_diligentSamples.end(),
            [&selectedSampleName](const DiligentSampleEntry& entry) { return entry.name == selectedSampleName; });
        if (it != m_diligentSamples.end()) {
            m_selectedDiligentSample = static_cast<std::size_t>(std::distance(m_diligentSamples.begin(), it));
        } else {
            m_selectedDiligentSample = 0;
        }
    } else {
        m_selectedDiligentSample = 0;
    }

    const auto builtCount = static_cast<std::size_t>(std::count_if(
        m_diligentSamples.begin(),
        m_diligentSamples.end(),
        [](const DiligentSampleEntry& entry) { return entry.executableExists; }));
    m_diligentSamplesStatus = "Discovered " + std::to_string(m_diligentSamples.size()) +
                              " Diligent demos. Built in current binary dir: " + std::to_string(builtCount) +
                              ". Scan root: " + samplesRoot.string();
}

bool Application::launchSelectedDiligentSample() {
    if (m_diligentSamples.empty()) {
        m_sampleLaunchStatus = "No Diligent demos discovered.";
        return false;
    }
    if (m_selectedDiligentSample >= m_diligentSamples.size()) {
        m_selectedDiligentSample = 0;
    }

    const DiligentSampleEntry& sample = m_diligentSamples[m_selectedDiligentSample];
    if (!sample.executableExists) {
        m_sampleLaunchStatus = "Sample '" + sample.name + "' is not built yet.";
        ENGINE_LOG_WARN("Requested sample '{}' is not built: {}", sample.name, sample.executablePath);
        return false;
    }

    std::string launchError;
    const std::string mode = modeNameFromIndex(m_selectedSampleBackendMode);
    const std::filesystem::path executablePath{sample.executablePath};
    if (!launchDetachedProcess(executablePath, mode, executablePath.parent_path(), launchError)) {
        m_sampleLaunchStatus = "Failed to launch '" + sample.name + "': " + launchError;
        ENGINE_LOG_ERROR("Failed to launch sample '{}': {}", sample.name, launchError);
        return false;
    }

    m_sampleLaunchStatus = "Launched '" + sample.name + "' with backend mode '" + mode + "'.";
    ENGINE_LOG_INFO("Launched Diligent sample '{}', mode={}", sample.name, mode);
    return true;
}

const char* Application::workspaceModeLabel(const WorkspaceMode mode) {
    switch (mode) {
        case WorkspaceMode::EngineStates:
            return "Engine States";
        case WorkspaceMode::Tutorial01HelloTriangle:
            return "Diligent Tutorial01";
        case WorkspaceMode::Tutorial03Texturing:
            return "Diligent Tutorial03";
        case WorkspaceMode::ImGuiDemo:
            return "Diligent ImGui Demo";
        case WorkspaceMode::DiligentSamplesHub:
            return "Diligent Samples Hub";
        default:
            return "Unknown";
    }
}

void Application::onEvent(const platform::Event& event) {
    platform::InputManager::onEvent(event);
    m_renderer.onEvent(event);
    platform::InputManager::setCaptureState(m_renderer.wantsKeyboardCapture(), m_renderer.wantsMouseCapture());

    if (event.type == platform::EventType::Quit) {
        ENGINE_LOG_INFO("Quit event received");
        m_running = false;
    } else if (event.type == platform::EventType::Resize) {
        ENGINE_LOG_INFO("Window resized to {}x{}", event.width, event.height);
        m_renderer.onResize(static_cast<std::uint32_t>(event.width), static_cast<std::uint32_t>(event.height));
    }

    const bool isKeyboardEvent =
        event.type == platform::EventType::KeyPressed || event.type == platform::EventType::KeyReleased;
    const bool isMouseEvent = event.type == platform::EventType::MouseMoved ||
                              event.type == platform::EventType::MouseButtonPressed ||
                              event.type == platform::EventType::MouseButtonReleased;
    const bool gameplayLookOverride =
        m_workspaceMode == WorkspaceMode::EngineStates &&
        platform::InputManager::isMouseButtonDownRaw(platform::MouseButton::Right);
    const bool consumedByImGui =
        !gameplayLookOverride &&
        ((isKeyboardEvent && m_renderer.wantsKeyboardCapture()) || (isMouseEvent && m_renderer.wantsMouseCapture()));

    if (m_workspaceMode == WorkspaceMode::EngineStates && !consumedByImGui) {
        m_stateStack.handleEvent(event);
        m_stateStack.applyPendingChanges();
    }
}

} // namespace engine::core
