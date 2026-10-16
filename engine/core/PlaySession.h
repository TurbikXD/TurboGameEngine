#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include "engine/core/ProjectManager.h"

namespace engine::core {

struct PlayLaunch final {
    Project project;
    int width{1280};
    int height{720};
    bool vsync{true};
    std::string device{"auto"};
};

// Owns an isolated preview process and its temporary scene, never the project files.
class PlaySession final {
public:
    PlaySession();
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;
    bool start(const std::filesystem::path& executable, const std::string& scene,
               const PlayLaunch& options, std::string& error);
    void stop();
    [[nodiscard]] bool running() const;
    [[nodiscard]] const std::filesystem::path& directory() const;
    static std::optional<PlayLaunch> load(const std::filesystem::path& manifest, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace engine::core
