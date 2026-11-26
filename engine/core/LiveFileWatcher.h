#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/core/JobSystem.h"

namespace engine::core {
class LiveFileWatcher final {
public:
    using Clock = std::chrono::steady_clock;
    struct Change { std::filesystem::path path; std::string contents, error; };
    LiveFileWatcher();
    ~LiveFileWatcher();
    void initialize(JobSystem* jobs);
    void clear();
    void watch(const std::filesystem::path& path);
    std::vector<Change> poll(Clock::time_point now = Clock::now());
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
