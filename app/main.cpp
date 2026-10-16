#include "engine/core/Application.h"
#include "engine/core/Log.h"
#include "engine/rhi/RHI.h"
#include "engine/core/PlaySession.h"
#include <iostream>
#include <cstdio>
#include <csignal>
#include <exception>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
// Keep fatal reporting independent of the logger, renderer and allocator-backed
// engine state: an abort may come from any thread, including during shutdown.
void recordFatal(const char* category, const char* detail, const void* instruction = nullptr) noexcept {
    static std::atomic_flag reporting = ATOMIC_FLAG_INIT;
    if (reporting.test_and_set(std::memory_order_relaxed)) return;
#if defined(_WIN32)
    CreateDirectoryW(L"logs", nullptr);
#endif
    FILE* file = nullptr;
#if defined(_MSC_VER)
    fopen_s(&file, "logs/crash-last.txt", "w");
#else
    file = std::fopen("logs/crash-last.txt", "w");
#endif
    std::fprintf(stderr, "Fatal engine failure (%s): %s\n", category, detail);
    if (!file) return;
    std::fprintf(file, "TurboGameEngine fatal failure\nCategory: %s\nDetail: %.8192s\n", category, detail);
#if defined(_WIN32)
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::fprintf(file, "UTC: %04u-%02u-%02u %02u:%02u:%02u\nProcess: %lu Thread: %lu\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        GetCurrentProcessId(), GetCurrentThreadId());
    const auto address = [&](const void* value) {
        HMODULE module{};
        wchar_t name[MAX_PATH]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(value), &module)) {
            GetModuleFileNameW(module, name, MAX_PATH);
            name[MAX_PATH - 1] = L'\0';
            const auto offset = reinterpret_cast<std::uintptr_t>(value) - reinterpret_cast<std::uintptr_t>(module);
            std::fprintf(file, "  %p %ls + 0x%llx\n", value, name, static_cast<unsigned long long>(offset));
        } else std::fprintf(file, "  %p\n", value);
    };
    if (instruction) { std::fprintf(file, "Fault instruction:\n"); address(instruction); }
    void* frames[48]{};
    const auto count = CaptureStackBackTrace(0, 48, frames, nullptr);
    std::fprintf(file, "Reporting stack (module offsets, use matching build PDB):\n");
    for (unsigned i = 0; i < count; ++i) address(frames[i]);
#else
    (void)instruction;
#endif
    std::fclose(file);
}

void installFatalDiagnostics() {
    std::signal(SIGABRT, [](int) { recordFatal("abort", "CRT abort/assertion; inspect the final engine.log entries."); });
    std::set_terminate([] {
        const char* detail = "std::terminate without an active C++ exception";
        try {
            if (const auto error = std::current_exception()) std::rethrow_exception(error);
        } catch (const std::exception& error) {
            recordFatal("unhandled C++ exception", error.what());
            std::abort();
        } catch (...) { detail = "Unhandled non-standard C++ exception"; }
        recordFatal("std::terminate", detail);
        std::abort();
    });
#if defined(_WIN32)
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* error) -> LONG {
        char detail[80]{};
        std::snprintf(detail, sizeof(detail), "Windows exception 0x%08lx", error->ExceptionRecord->ExceptionCode);
        recordFatal("native exception", detail, error->ExceptionRecord->ExceptionAddress);
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    installFatalDiagnostics();
    try {
    std::optional<engine::core::PlayLaunch> standalone;
    if (argc > 1) {
        if (argc != 3 || std::filesystem::path(argv[1]) != "--play-session") {
            std::cerr << "Usage: app [--play-session manifest]\n"; return 2;
        }
        std::string error;
        standalone = engine::core::PlaySession::load(std::filesystem::path(argv[2]), error);
        if (!standalone) { std::cerr << "Invalid Play session: " << error << '\n'; return 2; }
    }
    constexpr const char* backendName = ENGINE_BACKEND_NAME;
    const engine::rhi::BackendType backend = engine::rhi::backendFromString(backendName);

    engine::core::Application app(backend, std::move(standalone));
    if (!app.init()) {
        ENGINE_LOG_ERROR("App initialization failed");
        return 1;
    }

    return app.run();
    } catch (const std::exception& error) {
        recordFatal("unhandled main-thread exception", error.what());
        return 1;
    } catch (...) {
        recordFatal("unhandled main-thread exception", "Non-standard C++ exception");
        return 1;
    }
}
