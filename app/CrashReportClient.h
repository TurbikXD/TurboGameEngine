#pragma once

#include <filesystem>

namespace engine::app {

void enableCrashReportDialog() noexcept;
int runCrashReportClient(const std::filesystem::path& reportDirectory, bool sendOnly) noexcept;

}
