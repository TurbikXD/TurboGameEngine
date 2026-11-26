#include "engine/core/CrashReport.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <ShlObj.h>
#include <process.h>

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#endif

namespace engine::core::CrashReport {
namespace {
namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr DWORD FatalCode = 0xE0544701;
constexpr DWORD AbortCode = 0x40000015;
constexpr DWORD ReportTimeoutMs = 10000;
constexpr DWORD RunLogLimit = 5U * 1024U * 1024U;

struct Context {
    std::array<char, 512> scene{};
    std::array<char, 128> action{};
};

struct Fault {
    EXCEPTION_RECORD record{};
    CONTEXT registers{};
    SYSTEMTIME utc{};
    DWORD threadId{};
    Context context;
    std::array<char, 512> detail{};
};

struct Reporter {
    std::string buildId;
    std::string commitSha;
    std::string reportId;
    fs::path directory;
    fs::path dumpPath;
    fs::path metadataPath;
    fs::path pendingMetadata;
    fs::path reportLog;
    fs::path legacyLog;
    fs::path logsDirectory;
    fs::path dialogExecutable;
    fs::path dialogWorkingDirectory;
    std::wstring dialogCommand;
    std::atomic<bool> dialogReady{};
    std::string logHeader;
    HANDLE runLog{INVALID_HANDLE_VALUE};
    DWORD runLogBytes{};
    HMODULE dbgHelp{};
    decltype(&MiniDumpWriteDump) dump{};
    HANDLE work{};
    HANDLE worker{};
    DWORD workerId{};
    std::atomic<bool> stopping{};
    std::atomic_flag reported{};
    SRWLOCK contextLock = SRWLOCK_INIT;
    Context context;
    Fault fault;
} reporter;

template<std::size_t N>
void copyText(std::array<char, N>& destination, std::string_view source) noexcept {
    const auto size = std::min(source.size(), N - 1);
    std::copy_n(source.begin(), size, destination.begin());
    destination[size] = '\0';
}

std::string utcString(const SYSTEMTIME& utc) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        unsigned(utc.wYear), unsigned(utc.wMonth), unsigned(utc.wDay), unsigned(utc.wHour),
        unsigned(utc.wMinute), unsigned(utc.wSecond), unsigned(utc.wMilliseconds));
    return text;
}

fs::path reportRoot() {
    if (const auto size = GetEnvironmentVariableW(L"TGE_CRASH_REPORT_DIR", nullptr, 0)) {
        std::wstring value(size, L'\0');
        const auto length = GetEnvironmentVariableW(L"TGE_CRASH_REPORT_DIR", value.data(), size);
        if (length && length < size) {
            value.resize(length);
            fs::path requested(value);
            if (requested.is_absolute()) return requested;
        }
    }
    PWSTR directory{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &directory))) {
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(directory, &CoTaskMemFree);
        return fs::path(owned.get()) / "TurboGameEngine" / "crash-reports";
    }
    return fs::temp_directory_path() / "TurboGameEngine" / "crash-reports";
}

bool writeBytes(HANDLE file, const void* data, DWORD count) noexcept {
    DWORD written{};
    return WriteFile(file, data, count, &written, nullptr) && written == count;
}

bool writeFile(const wchar_t* path, std::string_view contents, DWORD mode) noexcept {
    const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, mode,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = writeBytes(file, contents.data(), static_cast<DWORD>(contents.size()));
    CloseHandle(file);
    return written;
}

void writeLegacy(const Fault& fault) noexcept {
    CreateDirectoryW(reporter.logsDirectory.empty() ? L"logs" : reporter.logsDirectory.c_str(), nullptr);
    char summary[1024]{};
    const char* detail = fault.detail[0] ? fault.detail.data() : "Unhandled native exception";
    std::snprintf(summary, sizeof(summary),
        "TurboGameEngine fatal failure\r\nDetail: %s (0x%08lX)\r\n"
        "Process: %lu Thread: %lu\r\nBuildId: %s\r\nReportId: %s\r\n",
        detail, fault.record.ExceptionCode, GetCurrentProcessId(), fault.threadId,
        reporter.buildId.c_str(), reporter.reportId.c_str());
    writeFile(reporter.legacyLog.empty() ? L"logs/crash-last.txt" : reporter.legacyLog.c_str(),
        summary, CREATE_ALWAYS);
}

bool copyRunLog() noexcept {
    if (reporter.runLog == INVALID_HANDLE_VALUE) return false;
    constexpr auto share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const auto source = ReOpenFile(reporter.runLog, GENERIC_READ, share, 0);
    if (source == INVALID_HANDLE_VALUE) return false;
    const auto target = CreateFileW(reporter.reportLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (target == INVALID_HANDLE_VALUE) {
        CloseHandle(source);
        return false;
    }

    std::array<char, 32 * 1024> buffer{};
    DWORD bytes{};
    DWORD remaining = RunLogLimit;
    bool copied = true;
    while (remaining > 0) {
        const auto requested = std::min(remaining, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(source, buffer.data(), requested, &bytes, nullptr)) {
            copied = false;
            break;
        }
        if (bytes == 0) break;
        if (!writeBytes(target, buffer.data(), bytes)) {
            copied = false;
            break;
        }
        remaining -= bytes;
    }
    CloseHandle(target);
    CloseHandle(source);
    return copied;
}

DWORD invokeDump(HANDLE file, MINIDUMP_EXCEPTION_INFORMATION* exception) noexcept {
    __try {
        const auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo);
        if (reporter.dump(GetCurrentProcess(), GetCurrentProcessId(), file, flags,
                exception, nullptr, nullptr)) return ERROR_SUCCESS;
        const auto error = GetLastError();
        return error ? error : ERROR_WRITE_FAULT;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

DWORD writeDump(Fault& fault) noexcept {
    if (!reporter.dump) return ERROR_PROC_NOT_FOUND;
    const auto file = CreateFileW(reporter.dumpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    EXCEPTION_POINTERS pointers{&fault.record, &fault.registers};
    MINIDUMP_EXCEPTION_INFORMATION exception{fault.threadId, &pointers, FALSE};
    const auto error = invokeDump(file, &exception);
    CloseHandle(file);
    if (error) DeleteFileW(reporter.dumpPath.c_str());
    return error;
}

void launchDialog() noexcept {
    if (!reporter.dialogReady.load(std::memory_order_acquire)) return;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(reporter.dialogExecutable.c_str(), reporter.dialogCommand.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, reporter.dialogWorkingDirectory.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

unsigned __stdcall reportWorker(void*) noexcept {
    if (WaitForSingleObject(reporter.work, INFINITE) != WAIT_OBJECT_0 ||
        reporter.stopping.load(std::memory_order_acquire)) return 0;
    auto& fault = reporter.fault;
    writeLegacy(fault);
    if (!CreateDirectoryW(reporter.directory.c_str(), nullptr)) return 0;
    const bool logWritten = copyRunLog();
    const auto dumpError = writeDump(fault);
    try {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lX", fault.record.ExceptionCode);
        const nlohmann::json metadata{
            {"schemaVersion", 1}, {"reportId", reporter.reportId}, {"utc", utcString(fault.utc)},
            {"buildId", reporter.buildId}, {"commitSha", reporter.commitSha},
            {"processId", GetCurrentProcessId()}, {"threadId", fault.threadId}, {"exceptionCode", code},
            {"scene", fault.context.scene.data()}, {"lastAction", fault.context.action.data()},
            {"detail", fault.detail.data()}, {"dumpWritten", dumpError == ERROR_SUCCESS},
            {"dumpError", dumpError}, {"logWritten", logWritten}
        };
        const auto json = metadata.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        if (writeFile(reporter.pendingMetadata.c_str(), json, CREATE_NEW) &&
            MoveFileExW(reporter.pendingMetadata.c_str(), reporter.metadataPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
            launchDialog();
        }
    } catch (...) {
    }
    return 0;
}

LONG capture(EXCEPTION_POINTERS* pointers, const char* detail) noexcept {
    const DWORD threadId = GetCurrentThreadId();
    if (!reporter.reported.test_and_set(std::memory_order_acq_rel)) {
        auto& fault = reporter.fault;
        fault.record = *pointers->ExceptionRecord;
        fault.record.ExceptionRecord = nullptr;
        fault.registers = *pointers->ContextRecord;
        fault.threadId = threadId;
        GetSystemTime(&fault.utc);
        if (detail) {
            std::size_t length = 0;
            while (length < fault.detail.size() - 1 && detail[length] != '\0') ++length;
            copyText(fault.detail, std::string_view(detail, length));
        }
        if (TryAcquireSRWLockShared(&reporter.contextLock)) {
            fault.context = reporter.context;
            ReleaseSRWLockShared(&reporter.contextLock);
        }
        if (reporter.worker) SetEvent(reporter.work);
        else writeLegacy(fault);
    }
    if (reporter.worker && threadId != reporter.workerId) {
        WaitForSingleObject(reporter.worker, ReportTimeoutMs);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI unhandledException(EXCEPTION_POINTERS* pointers) noexcept {
    return capture(pointers, nullptr);
}

void reportFailure(DWORD code, const char* detail) noexcept {
    __try {
        RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    } __except (capture(GetExceptionInformation(), detail)) {
    }
}

void prepare(std::string_view buildId, std::string_view commitSha) {
    reporter.buildId = buildId;
    reporter.commitSha = commitSha;
    reporter.logsDirectory = fs::absolute("logs");
    reporter.legacyLog = reporter.logsDirectory / "crash-last.txt";
    const auto root = reportRoot();
    fs::create_directories(root);

    GUID id{};
    wchar_t formatted[39]{};
    if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, formatted, 39) != 39) return;
    reporter.reportId.resize(36);
    std::transform(formatted + 1, formatted + 37, reporter.reportId.begin(),
        [](wchar_t byte) { return static_cast<char>(byte); });
    reporter.directory = root / reporter.reportId;
    reporter.dumpPath = reporter.directory / "crash.dmp";
    reporter.reportLog = reporter.directory / "engine.log";
    reporter.metadataPath = reporter.directory / "report.json";
    reporter.pendingMetadata = reporter.directory / "report.json.tmp";
    SYSTEMTIME now{};
    GetSystemTime(&now);
    reporter.logHeader = "TurboGameEngine launch " + utcString(now) + " buildId=" + reporter.buildId +
        " commitSha=" + reporter.commitSha + " processId=" + std::to_string(GetCurrentProcessId()) + "\r\n";
    const auto runPath = root / (".run-" + reporter.reportId + ".log");
    reporter.runLog = CreateFileW(runPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    appendLog(reporter.logHeader);

    reporter.dbgHelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (reporter.dbgHelp) {
        reporter.dump = std::bit_cast<decltype(reporter.dump)>(GetProcAddress(reporter.dbgHelp, "MiniDumpWriteDump"));
    }
    reporter.work = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (reporter.work) {
        unsigned workerId{};
        reporter.worker = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &reportWorker, nullptr, 0, &workerId));
        reporter.workerId = workerId;
    }
}
#else
std::atomic_flag reported{};

void writeLegacy(const char* detail) noexcept {
    if (reported.test_and_set(std::memory_order_relaxed)) return;
    try {
        std::error_code ignored;
        fs::create_directories("logs", ignored);
        if (auto* file = std::fopen("logs/crash-last.txt", "w")) {
            std::fprintf(file, "TurboGameEngine fatal failure\nDetail: %.8192s\n", detail);
            std::fclose(file);
        }
    } catch (...) {
    }
}
#endif
}

void install(std::string_view buildId, std::string_view commitSha) noexcept {
#if defined(_WIN32)
    try {
        prepare(buildId, commitSha);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Crash reporter initialization failed: %s\n", error.what());
    } catch (...) {
        std::fputs("Crash reporter initialization failed.\n", stderr);
    }
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    ULONG reservedStack = 64 * 1024;
    SetThreadStackGuarantee(&reservedStack);
    SetUnhandledExceptionFilter(&unhandledException);
    std::signal(SIGABRT, [](int) {
        reportFailure(AbortCode, "CRT abort or assertion");
        TerminateProcess(GetCurrentProcess(), 3);
    });
#else
    (void)buildId;
    (void)commitSha;
    std::signal(SIGABRT, [](int) { writeLegacy("CRT abort or assertion"); });
#endif
    std::set_terminate([] {
        try {
            if (const auto error = std::current_exception()) std::rethrow_exception(error);
        } catch (const std::exception& error) {
            fatal(error.what());
            std::abort();
        } catch (...) {
            fatal("Unhandled non-standard C++ exception");
            std::abort();
        }
        fatal("std::terminate without an active C++ exception");
        std::abort();
    });
}

void shutdown() noexcept {
#if defined(_WIN32)
    reporter.stopping.store(true, std::memory_order_release);
    if (reporter.work) SetEvent(reporter.work);
    if (reporter.worker) {
        if (WaitForSingleObject(reporter.worker, ReportTimeoutMs) != WAIT_OBJECT_0) return;
        CloseHandle(reporter.worker);
        reporter.worker = nullptr;
    }
    if (reporter.work) {
        CloseHandle(reporter.work);
        reporter.work = nullptr;
    }
    if (hasRunLog()) {
        CloseHandle(reporter.runLog);
        reporter.runLog = INVALID_HANDLE_VALUE;
    }
    if (reporter.dbgHelp) {
        FreeLibrary(reporter.dbgHelp);
        reporter.dbgHelp = nullptr;
        reporter.dump = nullptr;
    }
#endif
}

void setScene(std::string_view scene) noexcept {
#if defined(_WIN32)
    AcquireSRWLockExclusive(&reporter.contextLock);
    copyText(reporter.context.scene, scene);
    ReleaseSRWLockExclusive(&reporter.contextLock);
#else
    (void)scene;
#endif
}

void setAction(std::string_view action) noexcept {
#if defined(_WIN32)
    AcquireSRWLockExclusive(&reporter.contextLock);
    copyText(reporter.context.action, action);
    ReleaseSRWLockExclusive(&reporter.contextLock);
#else
    (void)action;
#endif
}

#if defined(_WIN32)
void enableDialog(const std::filesystem::path& executable) noexcept {
    if (!reporter.worker || !executable.is_absolute() || reporter.dialogReady.load(std::memory_order_acquire)) return;
    try {
        reporter.dialogWorkingDirectory = executable.parent_path();
        reporter.dialogExecutable = executable;
        reporter.dialogCommand = L"\"" + executable.native() + L"\" --crash-report \"" + reporter.directory.native() + L"\"";
        reporter.dialogReady.store(true, std::memory_order_release);
    } catch (...) {
    }
}
#endif

void fatal(const char* detail) noexcept {
#if defined(_WIN32)
    reportFailure(FatalCode, detail);
#else
    writeLegacy(detail);
#endif
}

bool hasRunLog() noexcept {
#if defined(_WIN32)
    return reporter.runLog != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
}

void appendLog(std::string_view message) noexcept {
#if defined(_WIN32)
    if (!hasRunLog() || message.size() > RunLogLimit - reporter.logHeader.size()) return;
    const auto bytes = static_cast<DWORD>(message.size());
    if (reporter.runLogBytes + bytes > RunLogLimit) {
        if (!SetFilePointerEx(reporter.runLog, {}, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(reporter.runLog)) return;
        reporter.runLogBytes = static_cast<DWORD>(reporter.logHeader.size());
        writeBytes(reporter.runLog, reporter.logHeader.data(), reporter.runLogBytes);
    }
    if (writeBytes(reporter.runLog, message.data(), bytes)) reporter.runLogBytes += bytes;
#else
    (void)message;
#endif
}

}
