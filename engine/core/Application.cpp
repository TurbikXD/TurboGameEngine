#include "engine/core/Application.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif
#if defined(__linux__)
#include <unistd.h>
#endif

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "app/Lab5Session.h"
#include "engine/core/Log.h"
#include "engine/core/AtomicFile.h"
#include "engine/core/CrashReport.h"
#include "engine/core/Profiling.h"
#include "engine/core/Time.h"
#include "engine/ecs/components.h"
#include "engine/editor/ProjectLauncher.h"
#include "engine/game/GameplayState.h"
#include "engine/game/MenuState.h"
#include "engine/game/NetworkArena.h"
#include "engine/game/PauseState.h"
#include "engine/network/NetworkSession.h"
#include "engine/network/SteamTransport.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/rhi/RHI.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace {

std::optional<std::uint64_t> parseSteamUserId(std::string_view text) {
    if (text.empty()) return {};
    std::uint64_t id{};
    const auto* end = text.data() + text.size();
    const auto [position, error] = std::from_chars(text.data(), end, id);
    if (error != std::errc{} || position != end || !engine::net::validSteamUserId(id)) return {};
    return id;
}

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
#elif defined(__linux__)
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return executable.parent_path();
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

}

namespace engine::core {

Application::Application(rhi::BackendType backend, std::optional<PlayLaunch> standalone,
                         std::optional<net::Endpoint> networkArena, Lab5Session* lab5)
    : m_backend(backend), m_labOptions(LabOptions::fromEnvironment()), m_standalone(std::move(standalone)),
      m_networkArena(networkArena), m_lab5(lab5),
      m_jobs(true, m_labOptions.jobWorkers) {}

Application::~Application() {
    shutdown();
}

bool Application::init() {
    ENGINE_PROFILE_ZONE("Application Init");
    Log::init();
    m_logInitialized = true;
    ENGINE_LOG_INFO("Application init started");

    m_config = Config::load("config.json");
    const bool networkClient = m_standalone && m_standalone->networkRole == SceneNetworkRole::Client;
    if (m_standalone) {
        m_labOptions.scene.clear(); m_labOptions.stressScene = false; m_labOptions.waitForTracy = false;
        if (m_lab5) m_labOptions.durationSeconds = 0.0;
        m_config.width = m_standalone->width; m_config.height = m_standalone->height;
        m_config.vsync = m_standalone->vsync; m_config.diligentDevice = m_standalone->device;
        m_config.title = m_standalone->project.name + " - Play";
        if (networkClient) {
            m_config.title = m_standalone->project.name + " - Client";
            if (m_standalone->networkClientIndex) {
                m_config.title += " " + std::to_string(m_standalone->networkClientIndex);
            }
            m_labOptions.durationSeconds = 0.0;
            if (m_standalone->networkSteam) {
                m_steamRuntime = std::make_unique<net::SteamInvites>();
                std::string error;
                if (!m_steamRuntime->start(error)) {
                    ENGINE_LOG_ERROR("Steam initialization failed: {}", error);
                    return false;
                }
                m_standalone->steamRuntime = m_steamRuntime.get();
            }
            m_stateStack.setSceneNetworkLaunch(&*m_standalone);
        }
        m_activeProject = m_standalone->project;
        m_stateStack.setStandalone(true);
        m_stateStack.setProject(&*m_activeProject);
        m_renderer.setProjectAssetRoot(m_activeProject->assetRoot);
    }
    if (m_networkArena) {
        m_labOptions.scene.clear();
        m_labOptions.stressScene = false;
        m_labOptions.waitForTracy = false;
        m_config.title = "TurboGameEngine - Relay Arena";
        m_config.clearColor = {0.025F, 0.039F, 0.062F, 1.0F};
    }
    m_stateStack.setServices(&m_jobs, &m_labOptions);
    m_renderer.setJobSystem(&m_jobs);
    m_renderer.setAsyncLoadingEnabled(m_labOptions.asyncLoading);
    m_renderer.setGpuUploadEnabled(m_labOptions.gpuUpload);
    m_renderer.setUploadBudget(m_labOptions.uploadsPerFrame, m_labOptions.uploadBudgetMilliseconds);
    m_renderer.setAssetInFlightLimit(m_labOptions.assetInFlightLimit);
    if (m_labOptions.active() || m_labOptions.stressActive()) {
        m_config.width = 1280;
        m_config.height = 720;
        m_config.vsync = false;
        m_config.initialState = "gameplay";
    }
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

    m_stateStack.setProjectSelectorCallback([this] { m_projectSelectorRequested = true; });
#if defined(_WIN32)
    const auto executable = getExecutableDirectory() / "app.exe";
#else
    const auto executable = getExecutableDirectory() / "app";
#endif
    m_stateStack.setPlayWindowCallbacks(
        [this, executable](const std::string& scene, const std::filesystem::path& assets, std::string& error) {
            return m_playSession.start(executable, scene, playLaunchOptions(assets), error);
        }, [this] { m_playSession.stop(); }, [this] { return m_playSession.running(); });
    m_stateStack.setNetworkPlayCallbacks(
        [this, executable](const std::string& scene, const std::filesystem::path& assets,
                           const net::NetworkTestOptions& networkOptions, std::string& error) {
            auto options = playLaunchOptions(assets);
            options.networkOptions = networkOptions;
            return m_networkPlaySession.startNetwork(executable, scene, options, error);
        }, [this] { m_networkPlaySession.stop(); }, [this] { return m_networkPlaySession.running(); },
        [this]() -> std::string_view { return m_networkPlaySession.networkError(); });
    m_stateStack.setOnlinePlayCallbacks({
        [this, executable](SceneNetworkRole role, bool steam, std::string_view address, unsigned maxPeers,
                           std::string& error) {
            return startOnlinePlay(executable, role, steam, address, maxPeers, error);
        },
        [this](SceneNetworkRole role) { onlinePlay(role).stop(); },
        [this](SceneNetworkRole role) -> const OnlinePlayStatus& { return onlinePlay(role).onlineStatus(); },
        [this](std::uint64_t friendId, std::string& error) { return m_onlineServer.inviteOnlineFriend(friendId, error); },
        [this](std::uint64_t friendId) { return m_onlineServer.steamFriendAvatar(friendId); }
    });
    m_stateStack.setNetworkTestControlCallbacks(
        [this] { return m_networkPlaySession.networkTestClients(); },
        [this](unsigned index, net::NetworkTestConditions conditions, std::string& error) {
            return m_networkPlaySession.setNetworkTestConditions(index, conditions, error);
        },
        [this](unsigned index) { return m_networkPlaySession.networkTestHistory(index); },
        [this] { return m_networkPlaySession.hasNetworkTestHistory(); },
        [this](const std::filesystem::path& path, std::string& error) {
            return m_networkPlaySession.exportNetworkTestHistory(path, error);
        });
    m_projectSelectorVisible = !m_networkArena && !m_standalone && !m_labOptions.active() && !m_labOptions.stressActive();
    const game::GameplayState* networkState = nullptr;
    if (m_networkArena) {
        m_stateStack.push(game::makeNetworkArena(m_stateStack, *m_networkArena));
    } else if (m_projectSelectorVisible) {
        m_projectLauncher = std::make_unique<editor::ProjectLauncher>(m_projects,
            [this](Project project) {
                m_pendingProject = std::move(project);
                m_projectLaunchRequested = true;
            },
            [this] { m_pendingProject.reset(); m_projectLaunchRequested = true; });
        m_window->setTitle(m_config.title + " - Projects");
    } else {
        auto gameplay = std::make_unique<game::GameplayState>(m_stateStack);
        if (networkClient) networkState = gameplay.get();
        if (m_lab5) m_lab5->attach(*gameplay);
        m_stateStack.push(std::move(gameplay));
    }
    m_stateStack.applyPendingChanges();
    if (networkState && !networkState->networkSceneReady()) {
        const auto error = networkState->networkSceneError();
        ENGINE_LOG_ERROR("Network Play client: {}", error.empty() ? "Could not initialize the scene." : error);
        return false;
    }
    if (networkState && !m_standalone->networkReadyPath.empty()) {
        std::string error;
        if (!writeFileAtomically(m_standalone->networkReadyPath,
                std::to_string(m_standalone->networkEndpoint.port) + "\n", error)) {
            ENGINE_LOG_ERROR("Could not report that the multiplayer window is ready: {}", error);
            return false;
        }
    }
    m_selectedSampleBackendMode = modeIndexFromString(m_config.diligentDevice);
    if (!m_networkArena) discoverDiligentSamples();

    m_running = true;
    m_initialized = true;
    ENGINE_LOG_INFO("Application init complete, backend={}", rhi::toString(m_backend));
    return true;
}

PlayLaunch Application::playLaunchOptions(const std::filesystem::path& assets) const {
    PlayLaunch options;
    if (m_activeProject) options.project = *m_activeProject;
    else { options.project.name = "Engine Demo"; options.project.root = assets.parent_path(); }
    options.project.assetRoot = std::filesystem::absolute(assets);
    options.width = std::max(320, m_window->width());
    options.height = std::max(200, m_window->height());
    options.vsync = m_config.vsync;
    options.device = m_config.diligentDevice;
    return options;
}

PlaySession& Application::onlinePlay(SceneNetworkRole role) {
    return role == SceneNetworkRole::Server ? m_onlineServer : m_onlineClient;
}

bool Application::startOnlinePlay(const std::filesystem::path& executable, SceneNetworkRole role, bool steam,
                                  std::string_view address, unsigned maxPeers, std::string& error) {
    if (!m_activeProject || m_activeProject->manifestPath.empty()) {
        error = "Open a saved project before starting multiplayer.";
        return false;
    }
    const bool hosting = role == SceneNetworkRole::Server;
    if (hosting && (maxPeers == 0 || maxPeers > net::MaxPeers)) {
        error = "Player limit must be between 1 and " + std::to_string(net::MaxPeers) + ".";
        return false;
    }
    auto options = playLaunchOptions(m_activeProject->assetRoot);
    options.networkRole = role;
    options.networkSteam = steam;
    options.networkMaxPeers = hosting ? maxPeers : static_cast<unsigned>(net::MaxPeers);
    if (steam) {
        options.networkEndpoint = {0, net::SteamVirtualPort};
        if (!hosting && !address.empty()) {
            const auto id = parseSteamUserId(address);
            if (!id) { error = "Enter a host SteamID64 or leave it empty to wait for an invitation."; return false; }
            options.networkEndpoint.steamId = *id;
        }
    } else if (hosting) {
        const auto endpoint = net::parseEndpoint(address, net::DefaultPort);
        if (!endpoint || !endpoint->port || endpoint->address >= 0xe0000000U) {
            error = "Enter a local IPv4 bind address and a UDP port from 1 to 65535.";
            return false;
        }
        options.networkEndpoint = *endpoint;
    } else {
        if (address.empty() || address.size() > 259) {
            error = "Enter a server IPv4 address or DNS hostname with an optional port.";
            return false;
        }
        options.networkHost = address;
    }
    std::string scene;
    auto project = ProjectManager::load(m_activeProject->manifestPath, error, &options.networkCompatibility, &scene);
    if (!project) return false;
    if (project->scenePath != m_activeProject->scenePath || project->assetRoot != m_activeProject->assetRoot) {
        error = "Reopen the project to apply the changed startup scene or asset directory.";
        return false;
    }
    options.project = std::move(*project);
    if (!hosting && !steam) {
        const auto& server = m_onlineServer.onlineStatus();
        const auto endpoint = net::parseEndpoint(address, net::DefaultPort);
        if (server.ready && server.compatibility == options.networkCompatibility && !server.endpoint.steamId &&
            endpoint && (endpoint->address >> 24) == 127 && endpoint->port == server.endpoint.port &&
            (!server.endpoint.address || endpoint->address == server.endpoint.address)) {
            options.networkHostControlDirectory = m_onlineServer.directory();
        }
    }
    return onlinePlay(role).startOnline(executable, scene, std::move(options), error);
}

void Application::applyProjectSelection() {
    if (!m_projectLaunchRequested && !m_projectSelectorRequested) { return; }
    m_playSession.stop();
    m_networkPlaySession.stop();
    m_onlineServer.stop();
    m_onlineClient.stop();
    m_networkPlaySession.clearNetworkTestHistory();
    m_stateStack.clear();
    m_stateStack.applyPendingChanges();
    m_stateStack.setProject(nullptr);
    m_activeProject.reset();
    m_tutorial03Mesh.reset();
    m_tutorial03Texture.reset();
    m_tutorial03Shader.reset();
    m_workspaceMode = WorkspaceMode::EngineStates;
    if (m_projectSelectorRequested) {
        CrashReport::setScene({});
        CrashReport::setAction("project-selector");
        m_renderer.setProjectAssetRoot({});
        m_projectSelectorVisible = true;
        m_projects.refresh();
        m_window->setTitle(m_config.title + " - Projects");
    } else {
        m_activeProject = std::move(m_pendingProject);
        m_renderer.setProjectAssetRoot(m_activeProject ? m_activeProject->assetRoot : std::filesystem::path{});
        m_stateStack.setProject(m_activeProject ? &*m_activeProject : nullptr);
        m_projectSelectorVisible = false;
        m_stateStack.push(std::make_unique<game::GameplayState>(m_stateStack));
        m_stateStack.applyPendingChanges();
        m_window->setTitle(m_config.title + " - " + (m_activeProject ? m_activeProject->name : "Engine Demo"));
    }
    m_pendingProject.reset();
    m_projectLaunchRequested = false;
    m_projectSelectorRequested = false;
    platform::Input::reset();
}

int Application::run() {
    if (!m_initialized) {
        return 1;
    }

    if (m_labOptions.waitForTracy) {
        ENGINE_LOG_INFO("LAB_READY_FOR_TRACY");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ENGINE_PROFILE_CONNECTED() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!ENGINE_PROFILE_CONNECTED()) {
            ENGINE_LOG_ERROR("LAB: Tracy did not connect within 10 seconds");
            shutdown();
            return 2;
        }
    }
    ENGINE_PROFILE_MESSAGE("LAB_RUN_START");
    const auto runStart = std::chrono::steady_clock::now();
    ENGINE_LOG_INFO("LAB_RUN_START scene={} parallel_ecs={} async_loading={} entities={} workers={} asset_in_flight={}",
        m_labOptions.scene, m_labOptions.parallelEcs, m_labOptions.asyncLoading,
        m_labOptions.entityCount, m_jobs.workerCount(), m_renderer.assetLoadingStats().inFlightLimit);
    FrameTimer timer;
    timer.reset();

    constexpr double fixedDt = 1.0 / 60.0;
    double accumulator = 0.0;
    TimeStatsAggregator stats(1.0);
    const bool diagnostics = std::getenv("TGE_FRAME_DIAGNOSTICS") != nullptr;
    std::array<double, 5> stageMs{};
    unsigned diagnosticFrames = 0, updateSteps = 0;
    double discardedSimulationMs = 0.0;
    int exitCode = m_lab5 && m_lab5->verifying() ? 1 : 0;

    while (m_running && m_window && !m_window->shouldClose()) {
        ENGINE_PROFILE_ZONE("Main Frame");
        applyProjectSelection();
        m_onlineServer.pollOnline();
        m_onlineClient.pollOnline();
        if (m_networkPlaySession.running()) {
            m_networkPlaySession.pollNetwork();
            if (!m_networkPlaySession.running() && !m_networkPlaySession.networkError().empty()) {
                ENGINE_LOG_ERROR("{}", m_networkPlaySession.networkError());
            }
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
        m_stateStack.setLabElapsedSeconds(elapsed);
        if (m_labOptions.durationSeconds > 0.0 && elapsed >= m_labOptions.durationSeconds) {
            break;
        }
        auto stageStart = std::chrono::steady_clock::now();
        const auto measureStage = [&](const std::size_t index) {
            const auto now = std::chrono::steady_clock::now();
            if (diagnostics) stageMs[index] += std::chrono::duration<double, std::milli>(now - stageStart).count();
            stageStart = now;
        };
        {
            ENGINE_PROFILE_ZONE("Input and Hot Reload");
            platform::InputManager::beginFrame();
            m_window->pollEvents();
            if (!m_labOptions.active() && !m_networkArena) { pollRuntimeHotReload(); }
        }
        measureStage(0);

        const double rawFrameDt = timer.tickSeconds();
        const double frameDt = std::min(rawFrameDt, 0.25);
        m_workspaceSampleTime += frameDt;

        if (m_workspaceMode == WorkspaceMode::EngineStates) {
            ENGINE_PROFILE_ZONE("Fixed Update");
            accumulator += frameDt;
            unsigned frameSteps = 0;
            const unsigned maxSteps = m_labOptions.active() ? 16U : 4U;
            const auto updateStart = std::chrono::steady_clock::now();
            while (accumulator >= fixedDt && frameSteps < maxSteps) {
                ENGINE_PROFILE_ZONE("State Update Step");
                platform::InputManager::beginFixedTick();
                m_stateStack.update(fixedDt);
                m_stateStack.applyPendingChanges();
                platform::InputManager::endFixedTick();
                ++updateSteps;
                ++frameSteps;
                accumulator -= fixedDt;
                if (!m_labOptions.active() && std::chrono::steady_clock::now() - updateStart >= std::chrono::milliseconds(8)) break;
            }
            if (accumulator >= fixedDt) {
                const double remainder = std::fmod(accumulator, fixedDt);
                discardedSimulationMs += (accumulator - remainder)*1000.0;
                accumulator = remainder;
            }
            m_stateStack.setRenderInterpolationFraction(static_cast<float>(accumulator / fixedDt));
        }
        measureStage(1);

        {
            ENGINE_PROFILE_ZONE("Frame Render");
            const glm::vec4 clearColor{
                m_config.clearColor[0], m_config.clearColor[1], m_config.clearColor[2], m_config.clearColor[3]};
            m_renderer.beginFrame(clearColor);
            m_renderer.beginImGuiFrame(static_cast<float>(frameDt));
            renderWorkspace(frameDt);
            measureStage(2);
            renderWorkspaceUi(frameDt);
            measureStage(3);
            m_renderer.renderImGui();
            m_renderer.endFrame();
            measureStage(4);
        }
        if (m_lab5) {
            if (const auto result = m_lab5->afterFrame()) {
                exitCode = *result;
                break;
            }
        }

        stats.sample(rawFrameDt);
        ++diagnosticFrames;
        TimeStats sampled{};
        if (stats.consume(sampled)) {
            ENGINE_LOG_INFO(
                "Frame dt stats: avg={:.3f}ms min={:.3f}ms max={:.3f}ms samples={}",
                sampled.avgMs,
                sampled.minMs,
                sampled.maxMs,
                sampled.sampleCount);
            if (diagnostics) {
                const double frames = static_cast<double>(diagnosticFrames);
                ENGINE_LOG_INFO("Frame CPU: input={:.2f} update={:.2f} render={:.2f} ui={:.2f} present={:.2f} ms, steps/frame={:.2f}, discarded={:.2f} ms/frame",
                    stageMs[0]/frames, stageMs[1]/frames, stageMs[2]/frames, stageMs[3]/frames, stageMs[4]/frames,
                    static_cast<double>(updateSteps)/frames, discardedSimulationMs/frames);
            }
            stageMs.fill(0.0); diagnosticFrames = 0; updateSteps = 0;
            discardedSimulationMs = 0.0;
        }

        if (!m_projectSelectorVisible && m_workspaceMode == WorkspaceMode::EngineStates && m_stateStack.empty()) {
            ENGINE_LOG_WARN("State stack empty, exiting");
            m_running = false;
        }

        m_jobs.publishStats();
        ENGINE_PROFILE_FRAME_MARK();
    }

    shutdown();
    return exitCode;
}

void Application::shutdown() {
    if (!m_initialized && m_window == nullptr && !m_logInitialized) {
        return;
    }

    if (m_initialized) {
        ENGINE_LOG_INFO("Application shutdown started");
    }
    m_running = false;
    m_playSession.stop();
    m_networkPlaySession.stop();
    m_onlineServer.stop();
    m_onlineClient.stop();
    m_stateStack.setPlayWindowCallbacks({}, {}, {});
    m_stateStack.setNetworkPlayCallbacks({}, {}, {}, {});
    m_stateStack.setOnlinePlayCallbacks({});
    m_stateStack.setNetworkTestControlCallbacks({}, {});
    m_stateStack.clear();
    m_stateStack.applyPendingChanges();
    m_stateStack.setWindow(nullptr);
    m_stateStack.setProject(nullptr);
    m_stateStack.setSceneNetworkLaunch(nullptr);
    m_stateStack.setProjectSelectorCallback({});
    m_projectLauncher.reset();
    m_activeProject.reset();
    m_tutorial03Mesh.reset();
    m_tutorial03Texture.reset();
    m_tutorial03Shader.reset();
    const auto loads = m_renderer.assetLoadingStats();
    ENGINE_LOG_INFO("LAB_SHUTDOWN pending_cpu={} pending_uploads={}", loads.pendingCpu, loads.pendingUploads);
    m_renderer.shutdown();
    m_steamRuntime.reset();
    m_jobs.shutdown();
    m_stateStack.setServices(nullptr, nullptr);
    m_window.reset();
    m_initialized = false;
    if (m_logInitialized) {
        ENGINE_LOG_INFO("Application shutdown complete");
        Log::shutdown();
        m_logInitialized = false;
    }
}

void Application::renderWorkspace(const double frameDtSeconds) {
    if (m_projectSelectorVisible) { return; }
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

    if (m_projectSelectorVisible) {
        if (m_projectLauncher) { m_projectLauncher->render(); }
        return;
    }

    if (m_workspaceMode == WorkspaceMode::EngineStates) {
        m_stateStack.renderUi(m_renderer);
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

        if (m_workspaceMode == WorkspaceMode::Tutorial01HelloTriangle) {
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
    if ((m_standalone || m_networkArena) &&
        event.type == platform::EventType::KeyPressed && event.key == platform::KeyCode::Escape) {
        if (event.repeat) return;
        if (m_standalone && m_window->cursorMode() == platform::CursorMode::Disabled) {
            m_stateStack.handleEvent(event);
        } else {
            m_window->requestClose();
        }
        return;
    }

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
        (m_window->cursorMode() == platform::CursorMode::Disabled ||
            platform::InputManager::isMouseButtonDownRaw(platform::MouseButton::Right));
    const bool consumedByImGui =
        !gameplayLookOverride &&
        ((isKeyboardEvent && m_renderer.wantsKeyboardCapture()) || (isMouseEvent && m_renderer.wantsMouseCapture()));

    if (m_workspaceMode == WorkspaceMode::EngineStates && !consumedByImGui) {
        m_stateStack.handleEvent(event);
        m_stateStack.applyPendingChanges();
    }
}

}
