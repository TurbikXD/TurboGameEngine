#pragma once

#include <string_view>

namespace engine::core::CrashReport {

void install(std::string_view buildId, std::string_view commitSha) noexcept;
void shutdown() noexcept;
void setScene(std::string_view scene) noexcept;
void setAction(std::string_view action) noexcept;
void fatal(const char* detail) noexcept;

[[nodiscard]] bool hasRunLog() noexcept;
void appendLog(std::string_view message) noexcept;

}
