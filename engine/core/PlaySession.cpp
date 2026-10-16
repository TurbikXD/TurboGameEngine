#include "engine/core/PlaySession.h"
#include "engine/core/AtomicFile.h"
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace engine::core {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
}
struct PlaySession::Impl {
    std::filesystem::path directory;
#if defined(_WIN32)
    HANDLE process{nullptr};
    DWORD processId{};
#else
    mutable pid_t process{-1};
#endif
};
PlaySession::PlaySession() : m_impl(std::make_unique<Impl>()) {}
PlaySession::~PlaySession() { stop(); }
const std::filesystem::path& PlaySession::directory() const { return m_impl->directory; }
bool PlaySession::running() const {
#if defined(_WIN32)
    return m_impl->process && WaitForSingleObject(m_impl->process, 0) == WAIT_TIMEOUT;
#else
    if (m_impl->process < 0) return false;
    if (waitpid(m_impl->process, nullptr, WNOHANG) == 0) return true;
    m_impl->process = -1;
    return false;
#endif
}
void PlaySession::stop() {
#if defined(_WIN32)
    if (running()) {
        EnumWindows([](HWND window, LPARAM value) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == static_cast<DWORD>(value)) PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        }, static_cast<LPARAM>(m_impl->processId));
        if (WaitForSingleObject(m_impl->process, 2000) == WAIT_TIMEOUT) {
            // Only this preview process is owned by this session.
            TerminateProcess(m_impl->process, 0);
            WaitForSingleObject(m_impl->process, 2000);
        }
    }
    if (m_impl->process) CloseHandle(m_impl->process);
    m_impl->process = nullptr;
#else
    if (running()) { kill(m_impl->process, SIGTERM); waitpid(m_impl->process, nullptr, 0); }
    m_impl->process = -1;
#endif
    if (!m_impl->directory.empty()) {
        // Assigned only after exclusively creating this directory ourselves.
        std::error_code ignored;
        std::filesystem::remove_all(m_impl->directory, ignored);
        m_impl->directory.clear();
    }
}
bool PlaySession::start(const std::filesystem::path& executable, const std::string& scene,
                        const PlayLaunch& options, std::string& error) {
    error.clear();
    if (running()) { error = "A standalone game is already running."; return false; }
    stop();
    try {
        const auto binary = std::filesystem::absolute(executable);
        if (!std::filesystem::is_regular_file(binary)) { error = "Game executable was not found."; return false; }
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() / ("tge-play-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) { m_impl->directory = candidate; break; }
        }
        if (m_impl->directory.empty()) { error = "Could not create an isolated Play session."; return false; }
        const auto manifest = m_impl->directory / "play.json";
        const nlohmann::json descriptor = {{"version", 1}, {"name", options.project.name},
            {"root", utf8(std::filesystem::absolute(options.project.root))},
            {"assets", utf8(std::filesystem::absolute(options.project.assetRoot))},
            {"liveScene", options.project.scenePath.empty() ? std::string{} : utf8(std::filesystem::absolute(options.project.scenePath))},
            {"width", options.width}, {"height", options.height}, {"vsync", options.vsync}, {"device", options.device}};
        if (!writeFileAtomically(m_impl->directory / "scene.json", scene, error) ||
            !writeFileAtomically(manifest, descriptor.dump(2), error)) { stop(); return false; }
#if defined(_WIN32)
        std::wstring command = L"\"" + binary.wstring() + L"\" --play-session \"" + manifest.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, m_impl->directory.c_str(), &startup, &info)) {
            error = "Could not launch game window, Windows error " + std::to_string(GetLastError());
            stop(); return false;
        }
        CloseHandle(info.hThread);
        m_impl->process = info.hProcess;
        m_impl->processId = info.dwProcessId;
#else
        const auto binaryName = binary.string(), manifestName = manifest.string(), directoryName = m_impl->directory.string();
        m_impl->process = fork();
        if (m_impl->process == 0) {
            if (chdir(directoryName.c_str()) == 0) execl(binaryName.c_str(), binaryName.c_str(), "--play-session", manifestName.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        if (m_impl->process < 0) { error = "Could not launch game process."; stop(); return false; }
#endif
        return true;
    } catch (const std::exception& failure) { error = failure.what(); stop(); return false; }
}
std::optional<PlayLaunch> PlaySession::load(const std::filesystem::path& manifest, std::string& error) {
    error.clear();
    try {
        std::ifstream input(manifest);
        const auto value = nlohmann::json::parse(input);
        if (value.at("version") != 1) { error = "Unsupported Play session version."; return {}; }
        PlayLaunch launch;
        launch.project.name = value.at("name").get<std::string>();
        launch.project.root = std::filesystem::u8path(value.at("root").get<std::string>());
        launch.project.assetRoot = std::filesystem::u8path(value.at("assets").get<std::string>());
        launch.project.scenePath = std::filesystem::absolute(manifest).parent_path() / "scene.json";
        launch.project.liveScenePath = std::filesystem::u8path(value.value("liveScene", std::string{}));
        launch.project.layoutPath = launch.project.scenePath.parent_path() / "editor_layout.json";
        launch.width = value.at("width").get<int>();
        launch.height = value.at("height").get<int>();
        launch.vsync = value.at("vsync").get<bool>();
        launch.device = value.at("device").get<std::string>();
        if (launch.width < 320 || launch.height < 200 || launch.width > 16384 || launch.height > 16384 ||
            !std::filesystem::is_directory(launch.project.assetRoot) || !std::filesystem::is_regular_file(launch.project.scenePath)) {
            error = "Play session has invalid dimensions or missing scene/assets."; return {};
        }
        return launch;
    } catch (const std::exception& failure) { error = failure.what(); return {}; }
}
} // namespace engine::core
