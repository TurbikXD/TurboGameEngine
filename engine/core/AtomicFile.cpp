#include "engine/core/AtomicFile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstddef>
#include <cstring>
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
bool stageFile(const std::filesystem::path& target, const std::string& contents, TemporaryFile& temporary,
               std::string& error) {
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
#endif
        return true;
    }
    error = "Cannot create a unique temporary file.";
    return false;
}

#if defined(_WIN32)
class PendingPublication final {
public:
    PendingPublication(const TemporaryFile& temporary, const std::filesystem::path& destination) {
        const auto absolute = std::filesystem::absolute(destination).native();
        const auto nameBytes = absolute.size() * sizeof(wchar_t);
        m_size = static_cast<DWORD>(sizeof(FILE_RENAME_INFO) + nameBytes);
        m_buffer.resize((m_size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(m_buffer.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(info->FileName, absolute.data(), nameBytes);
        m_file = CreateFileW(temporary.path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) throw std::runtime_error(nativeError("Cannot open staged file", GetLastError()));
    }
    ~PendingPublication() { CloseHandle(m_file); }
    PendingPublication(const PendingPublication&) = delete;
    PendingPublication& operator=(const PendingPublication&) = delete;

    DWORD publish() {
        return SetFileInformationByHandle(m_file, FileRenameInfo, m_buffer.data(), m_size) ? ERROR_SUCCESS : GetLastError();
    }
    DWORD rollback() {
        FILE_DISPOSITION_INFO disposition{TRUE};
        return SetFileInformationByHandle(m_file, FileDispositionInfo, &disposition, sizeof(disposition)) ? ERROR_SUCCESS : GetLastError();
    }

private:
    HANDLE m_file{INVALID_HANDLE_VALUE};
    DWORD m_size{};
    std::vector<std::max_align_t> m_buffer;
};
#endif

bool targetOccupied(const std::filesystem::path& target) {
    return std::filesystem::exists(std::filesystem::symlink_status(target));
}

constexpr const char* ReportCollision = "A report file already exists. Choose a different name.";
}

bool writeFileAtomically(const std::filesystem::path& target, const std::string& contents, std::string& error) {
    error.clear();
    if (target.empty() || target.filename().empty()) { error = "File target is empty."; return false; }
    TemporaryFile temporary;
    try {
        if (!stageFile(target, contents, temporary, error)) return false;
#if defined(_WIN32)
        if (!MoveFileExW(temporary.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = nativeError("Cannot replace target file", GetLastError()); return false;
        }
#else
        std::error_code replaced;
        std::filesystem::rename(temporary.path, target, replaced);
        if (replaced) { error = "Cannot replace target file: " + replaced.message(); return false; }
#endif
        temporary.path.clear();
        return true;
    } catch (const std::exception& failure) {
        error = std::string("Cannot save file: ") + failure.what();
    }
    return false;
}

bool writeNewFiles(std::span<const NewFileContents> files, std::string& error) {
    error.clear();
    if (files.empty()) {
        error = "No report files to save.";
        return false;
    }
    try {
        for (const auto& file : files) {
            if (file.path.empty() || file.path.filename().empty()) { error = "Report target is empty."; return false; }
            if (targetOccupied(file.path)) { error = ReportCollision; return false; }
        }
        std::vector<TemporaryFile> staged(files.size());
        for (std::size_t index = 0; index < files.size(); ++index) {
            if (!stageFile(files[index].path, files[index].contents, staged[index], error)) return false;
        }
        std::size_t published = 0;
#if defined(_WIN32)
        std::vector<std::unique_ptr<PendingPublication>> pending;
        pending.reserve(files.size());
        for (std::size_t index = 0; index < files.size(); ++index) {
            pending.push_back(std::make_unique<PendingPublication>(staged[index], files[index].path));
        }
        DWORD failure = ERROR_SUCCESS;
        for (; published < files.size(); ++published) {
            failure = pending[published]->publish();
            if (failure != ERROR_SUCCESS) break;
            staged[published].path.clear();
        }
        if (published == files.size()) return true;
        DWORD rollbackFailure = ERROR_SUCCESS;
        while (published > 0) {
            const auto result = pending[--published]->rollback();
            if (result != ERROR_SUCCESS) rollbackFailure = result;
        }
        error = failure == ERROR_ALREADY_EXISTS || failure == ERROR_FILE_EXISTS ? ReportCollision : nativeError("Cannot publish report file", failure);
        if (rollbackFailure != ERROR_SUCCESS) error += ". " + nativeError("Could not remove the partially saved report", rollbackFailure);
#else
        std::error_code failure;
        for (; published < files.size(); ++published) {
            std::filesystem::create_hard_link(staged[published].path, files[published].path, failure);
            if (failure) break;
        }
        if (published == files.size()) return true;
        std::error_code rollbackFailure;
        while (published > 0) {
            --published;
            std::error_code result;
            const auto& target = files[published].path;
            if (std::filesystem::equivalent(staged[published].path, target, result)) std::filesystem::remove(target, result);
            if (result && result != std::errc::no_such_file_or_directory) rollbackFailure = result;
        }
        error = failure == std::errc::file_exists ? ReportCollision : "Cannot publish report file: " + failure.message();
        if (rollbackFailure) error += ". Could not remove the partially saved report: " + rollbackFailure.message();
#endif
    } catch (const std::exception& failure) {
        error = std::string("Cannot save report: ") + failure.what();
    }
    return false;
}

bool writeNewFilePair(const std::filesystem::path& first, const std::string& firstContents,
                      const std::filesystem::path& second, const std::string& secondContents,
                      std::string& error) {
    try {
        const std::array files{NewFileContents{first, firstContents}, NewFileContents{second, secondContents}};
        return writeNewFiles(files, error);
    } catch (const std::exception& failure) {
        error = std::string("Cannot save report: ") + failure.what();
        return false;
    }
}

}
