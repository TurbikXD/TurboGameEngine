#pragma once

#include <filesystem>
#include <string_view>

namespace engine::core::CrashReport {

void install(std::string_view buildId, std::string_view commitSha) noexcept;
void shutdown() noexcept;
void setScene(std::string_view scene) noexcept;
void setAction(std::string_view action) noexcept;
void fatal(const char* detail) noexcept;
#if defined(_WIN32)
void enableDialog(const std::filesystem::path& executable) noexcept;
#endif

[[nodiscard]] bool hasRunLog() noexcept;
void appendLog(std::string_view message) noexcept;

}
