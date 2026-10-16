#include "engine/core/AtomicFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace engine::core {
namespace {
struct TemporaryFile final {
    std::filesystem::path path;
    ~TemporaryFile() {
        // Record ownership only after CREATE_NEW/O_EXCL succeeds. Clear it
        // after replacement, so a new file at the vacated name is not removed.
        if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); }
    }
};

#if defined(_WIN32)
std::string nativeError(const char* action, const DWORD failure) {
    return std::string(action) + " (Windows error " + std::to_string(failure) + ")";
}
#else
std::string nativeError(const char* action, const int failure) {
    return std::string(action) + ": " + std::generic_category().message(failure);
}
#endif
} // namespace

bool writeFileAtomically(const std::filesystem::path& target, const std::string& contents,
                         std::string& error) {
    error.clear();
    if (target.empty() || target.filename().empty()) { error = "File target is empty."; return false; }
    TemporaryFile temporary;
    try {
        static std::atomic<unsigned long long> sequence{0};
        for (int attempt = 0; attempt < 128; ++attempt) {
            const auto name = ".tge-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1));
            auto candidate = target.parent_path() / name;
#if defined(_WIN32)
            const HANDLE file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                const DWORD failure = GetLastError();
                if (failure == ERROR_FILE_EXISTS || failure == ERROR_ALREADY_EXISTS) { continue; }
                error = nativeError("Cannot create temporary file", failure); return false;
            }
            temporary.path = std::move(candidate);
            DWORD failure = ERROR_SUCCESS;
            std::size_t offset = 0;
            while (offset < contents.size()) {
                DWORD written = 0;
                const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(65536U, contents.size() - offset));
                if (!WriteFile(file, contents.data() + offset, requested, &written, nullptr)) { failure = GetLastError(); break; }
                if (written == 0) { failure = ERROR_WRITE_FAULT; break; }
                offset += written;
            }
            if (failure == ERROR_SUCCESS && !FlushFileBuffers(file)) { failure = GetLastError(); }
            if (!CloseHandle(file) && failure == ERROR_SUCCESS) { failure = GetLastError(); }
            if (failure != ERROR_SUCCESS) { error = nativeError("Cannot write temporary file", failure); return false; }
            if (!MoveFileExW(temporary.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                error = nativeError("Cannot replace target file", GetLastError()); return false;
            }
#else
            const int file = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (file < 0) {
                const int failure = errno;
                if (failure == EEXIST) { continue; }
                error = nativeError("Cannot create temporary file", failure); return false;
            }
            temporary.path = std::move(candidate);
            int failure = 0;
            std::size_t offset = 0;
            while (offset < contents.size()) {
                const auto written = ::write(file, contents.data() + offset, std::min<std::size_t>(65536U, contents.size() - offset));
                if (written < 0 && errno == EINTR) { continue; }
                if (written < 0) { failure = errno; break; }
                if (written == 0) { failure = EIO; break; }
                offset += static_cast<std::size_t>(written);
            }
            if (failure == 0) {
                int result;
                do { result = ::fsync(file); } while (result < 0 && errno == EINTR);
                if (result < 0) { failure = errno; }
            }
            if (::close(file) < 0 && failure == 0) { failure = errno; }
            if (failure != 0) { error = nativeError("Cannot write temporary file", failure); return false; }
            std::error_code replaced;
            std::filesystem::rename(temporary.path, target, replaced);
            if (replaced) { error = "Cannot replace target file: " + replaced.message(); return false; }
#endif
            temporary.path.clear();
            return true;
        }
        error = "Cannot create a unique temporary file.";
    } catch (const std::exception& failure) {
        error = std::string("Cannot save file: ") + failure.what();
    }
    return false;
}

} // namespace engine::core
