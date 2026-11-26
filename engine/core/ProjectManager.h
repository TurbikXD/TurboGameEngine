#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine::core {

struct Project final {
    std::string name;
    std::filesystem::path root;
    std::filesystem::path manifestPath;
    std::filesystem::path assetRoot;
    std::filesystem::path scenePath;
    std::filesystem::path layoutPath;
    std::filesystem::path liveScenePath;
    unsigned networkPlayerLimit{};
};

struct RecentProject final {
    std::string name;
    std::filesystem::path manifestPath;
};

class ProjectManager final {
public:
    explicit ProjectManager(std::filesystem::path registryPath = defaultRegistryPath());

    static std::filesystem::path defaultRegistryPath();
    static std::filesystem::path defaultProjectsDirectory();
    static std::optional<Project> load(const std::filesystem::path& manifestOrDirectory,
                                       std::string& error, std::uint64_t* networkCompatibility = nullptr,
                                       std::string* startupSceneContents = nullptr);

    std::optional<Project> createBlank(const std::filesystem::path& parentDirectory,
                                     const std::string& name, std::string& error);
    std::optional<Project> open(const std::filesystem::path& manifestOrDirectory,
                               std::string& error);
    const std::vector<RecentProject>& recent() const;
    void refresh();

private:
    bool persist(std::string& error) const;
    void remember(const Project& project, std::string& error);

    std::filesystem::path m_registryPath;
    std::vector<RecentProject> m_recent;
};

}
