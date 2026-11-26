#include "engine/editor/ScriptFormatter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace engine::editor {
namespace {
constexpr std::size_t kMaxSource = 2U * 1024U * 1024U;
constexpr std::size_t kMaxDiagnostics = 64U * 1024U;
constexpr auto kProcessTimeout = std::chrono::seconds(5);

bool isFile(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

std::filesystem::path findExecutable() {
#if defined(_WIN32)
    wchar_t buffer[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, buffer, 32768);
    if (moduleLength > 0 && moduleLength < 32768) {
        const auto folder = std::filesystem::path(buffer).parent_path();
        for (const auto& path : {folder / "tools" / "stylua.exe", folder / "stylua.exe"}) {
            if (isFile(path)) return path;
        }
    }
#if defined(ENGINE_STYLUA_EXECUTABLE)
    const std::filesystem::path bundled(ENGINE_STYLUA_EXECUTABLE);
    if (isFile(bundled)) return bundled;
#endif
    if (isFile("tools/stylua.exe")) return std::filesystem::absolute("tools/stylua.exe");
    const DWORD pathLength = SearchPathW(nullptr, L"stylua.exe", nullptr, 32768, buffer, nullptr);
    if (pathLength > 0 && pathLength < 32768) return buffer;
#endif
    return {};
}

#if defined(_WIN32)
struct Handle final {
    HANDLE value = nullptr;
    ~Handle() { if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct ProcessCleanup final {
    HANDLE& process;
    HANDLE& job;
    ~ProcessCleanup() {
        if (process != nullptr && WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
            if (job != nullptr) TerminateJobObject(job, 1);
            WaitForSingleObject(process, 500);
        }
    }
};

struct TemporaryDocument final {
    std::filesystem::path directory;
    std::filesystem::path source;
    std::filesystem::path config;
    ~TemporaryDocument() {
        std::error_code error;
        if (!source.empty()) std::filesystem::remove(source, error);
        if (!config.empty()) std::filesystem::remove(config, error);
        if (!directory.empty()) std::filesystem::remove(directory, error);
    }
    bool create(const std::string& text) {
        wchar_t buffer[32768]{};
        const DWORD length = GetTempPathW(32768, buffer);
        if (length == 0 || length >= 32768) return false;
        static std::atomic<unsigned long long> sequence{0};
        const std::filesystem::path root(buffer);
        for (int attempt = 0; attempt < 100; ++attempt) {
            LARGE_INTEGER tick{};
            QueryPerformanceCounter(&tick);
            const auto candidate = root / ("tge-stylua-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(tick.QuadPart) + "-" + std::to_string(sequence.fetch_add(1)));
            if (CreateDirectoryW(candidate.c_str(), nullptr)) { directory = candidate; break; }
        }
        if (directory.empty()) return false;
        source = directory / "document.luau";
        config = directory / "stylua.toml";
        std::ofstream input(source, std::ios::binary);
        input.write(text.data(), static_cast<std::streamsize>(text.size()));
        input.close();
        if (!input) return false;
        std::ofstream settings(config, std::ios::binary);
        settings << "syntax = \"Luau\"\nindent_type = \"Spaces\"\nindent_width = 4\nline_endings = \"Unix\"\n";
        settings.close();
        return static_cast<bool>(settings);
    }
};

ScriptFormatter::Result formatDocument(const std::filesystem::path& executable, std::string path,
                                       std::string source, const std::atomic<bool>& cancelled) {
    ScriptFormatter::Result result;
    result.documentPath = std::move(path);
    result.original = std::move(source);
    if (result.original.size() > kMaxSource) { result.error = "Script exceeds the 2 MiB formatter limit."; return result; }
    if (cancelled.load()) { result.error = "Formatting cancelled."; return result; }
    TemporaryDocument temporary;
    if (!temporary.create(result.original)) { result.error = "Could not create the temporary formatting document."; return result; }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle readPipe, writePipe, nullInput, job, process, processThread;
    ProcessCleanup cleanup{process.value, job.value};
    if (!CreatePipe(&readPipe.value, &writePipe.value, &security, 65536) ||
        !SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0)) {
        result.error = "Could not create the formatter diagnostic pipe."; return result;
    }
    nullInput.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (nullInput.value == INVALID_HANDLE_VALUE) { result.error = "Could not create formatter input."; return result; }
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job.value == nullptr || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        result.error = "Could not contain the formatter process."; return result;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput.value;
    startup.StartupInfo.hStdOutput = writePipe.value;
    startup.StartupInfo.hStdError = writePipe.value;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributes(attributeBytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) {
        result.error = "Could not initialize formatter handles."; return result;
    }
    HANDLE inherited[] = {nullInput.value, writePipe.value};
    const bool attributeReady = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
    std::wstring command = L"\"" + executable.wstring() + L"\" --syntax Luau --verify --indent-type Spaces --indent-width 4"
        L" --line-endings Unix --no-editorconfig --config-path \"" + temporary.config.wstring() + L"\" \"" + temporary.source.wstring() + L"\"";
    PROCESS_INFORMATION info{};
    const bool created = attributeReady && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, temporary.directory.c_str(), &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) { result.error = "Could not start StyLua. Check the executable path."; return result; }
    process.value = info.hProcess;
    processThread.value = info.hThread;
    if (!AssignProcessToJobObject(job.value, process.value)) {
        TerminateProcess(process.value, 1);
        result.error = "Could not contain the StyLua process."; return result;
    }
    if (ResumeThread(processThread.value) == static_cast<DWORD>(-1)) { result.error = "Could not resume StyLua."; return result; }
    const auto deadline = std::chrono::steady_clock::now() + kProcessTimeout;
    std::string diagnostics;
    bool finished = false;
    while (!finished) {
        if (cancelled.load() || std::chrono::steady_clock::now() >= deadline) {
            result.error = cancelled.load() ? "Formatting cancelled." : "StyLua exceeded the 5 second time limit.";
            return result;
        }
        DWORD available = 0;
        if (PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            char buffer[8192];
            DWORD count = 0;
            if (ReadFile(readPipe.value, buffer, std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer))), &count, nullptr)) {
                if (diagnostics.size() + count > kMaxDiagnostics) { result.error = "StyLua diagnostic output exceeded its size limit."; return result; }
                diagnostics.append(buffer, count);
            }
        }
        finished = WaitForSingleObject(process.value, 10) == WAIT_OBJECT_0;
    }
    DWORD available = 0;
    while (PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        char buffer[8192];
        DWORD count = 0;
        if (!ReadFile(readPipe.value, buffer, std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer))), &count, nullptr) || count == 0) break;
        if (diagnostics.size() + count > kMaxDiagnostics) { result.error = "StyLua diagnostic output exceeded its size limit."; return result; }
        diagnostics.append(buffer, count);
    }
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.value, &exitCode) || exitCode != 0) {
        result.error = diagnostics.empty() ? "StyLua could not format this document." : diagnostics;
        return result;
    }
    std::error_code error;
    if (std::filesystem::file_size(temporary.source, error) > kMaxSource || error) {
        result.error = "Formatted output is missing or exceeds 2 MiB."; return result;
    }
    std::ifstream formatted(temporary.source, std::ios::binary);
    if (!formatted) { result.error = "Could not read the formatted document."; return result; }
    result.formatted.assign(std::istreambuf_iterator<char>(formatted), std::istreambuf_iterator<char>());
    result.success = true;
    return result;
}
#endif
}

struct ScriptFormatter::Impl final {
    std::filesystem::path executable = findExecutable();
    std::atomic<bool> active{false};
    std::atomic<bool> cancelled{false};
    std::thread worker;
    std::mutex mutex;
    std::optional<Result> result;
    ~Impl() { cancelled = true; if (worker.joinable()) worker.join(); }
};

ScriptFormatter::ScriptFormatter() : m_impl(std::make_unique<Impl>()) {}
ScriptFormatter::~ScriptFormatter() = default;
void ScriptFormatter::setExecutable(std::filesystem::path executable) {
    if (!busy()) m_impl->executable = std::filesystem::absolute(std::move(executable));
}
const std::filesystem::path& ScriptFormatter::executable() const { return m_impl->executable; }
bool ScriptFormatter::available() const { return isFile(m_impl->executable); }
bool ScriptFormatter::busy() const { return m_impl->active.load(); }
bool ScriptFormatter::start(std::string documentPath, std::string source) {
    if (busy() || !available() || source.size() > kMaxSource) return false;
    if (m_impl->worker.joinable()) m_impl->worker.join();
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->result.reset();
    }
    m_impl->cancelled = false;
    m_impl->active = true;
    const auto executable = m_impl->executable;
    Impl* impl = m_impl.get();
    m_impl->worker = std::thread([impl, executable, documentPath = std::move(documentPath), source = std::move(source)]() mutable {
        Result result;
        try {
#if defined(_WIN32)
            result = formatDocument(executable, std::move(documentPath), std::move(source), impl->cancelled);
#else
            result.documentPath = std::move(documentPath); result.original = std::move(source);
            result.error = "Native StyLua formatting is currently available on Windows.";
#endif
        } catch (const std::exception& error) { result.error = std::string("StyLua formatting failed: ") + error.what(); }
        {
            std::lock_guard lock(impl->mutex);
            impl->result = std::move(result);
        }
        impl->active = false;
    });
    return true;
}
std::optional<ScriptFormatter::Result> ScriptFormatter::poll() {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->result) return std::nullopt;
    auto result = std::move(m_impl->result);
    m_impl->result.reset();
    return result;
}
void ScriptFormatter::cancel() { m_impl->cancelled = true; }

}
