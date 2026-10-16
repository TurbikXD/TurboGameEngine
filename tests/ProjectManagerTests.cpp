#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include "engine/core/ProjectManager.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#endif

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using engine::core::ProjectManager;

void require(const bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "Cannot read fixture");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary); file << text;
    require(static_cast<bool>(file), "Cannot write fixture");
}
std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string(); return {value.begin(), value.end()};
}
void directoryLink(const fs::path& target, const fs::path& link, std::error_code& error) {
    fs::create_directory_symlink(target, link, error);
#ifdef _WIN32
    if (!error) { return; }
    // Junctions test the same canonical-containment boundary without admin/Developer Mode.
    if (!fs::create_directory(link)) { return; }
    const auto print = fs::canonical(target).wstring();
    const auto substitute = L"\\??\\" + print;
    struct MountPoint final {
        DWORD tag{IO_REPARSE_TAG_MOUNT_POINT};
        USHORT dataLength{}, reserved{}, substituteOffset{}, substituteLength{}, printOffset{}, printLength{};
        WCHAR paths[2048]{};
    } point;
    if (substitute.size() + print.size() + 2U >= std::size(point.paths)) { error = std::make_error_code(std::errc::filename_too_long); return; }
    point.substituteLength = static_cast<USHORT>(substitute.size() * sizeof(WCHAR));
    point.printOffset = static_cast<USHORT>((substitute.size() + 1U) * sizeof(WCHAR));
    point.printLength = static_cast<USHORT>(print.size() * sizeof(WCHAR));
    std::copy(substitute.begin(), substitute.end(), point.paths);
    std::copy(print.begin(), print.end(), point.paths + substitute.size() + 1U);
    point.dataLength = static_cast<USHORT>(8U + (substitute.size() + print.size() + 2U) * sizeof(WCHAR));
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        DWORD returned{};
        const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &point, 8U + point.dataLength,
            nullptr, 0, &returned, nullptr) != 0;
        const DWORD failure = GetLastError(); CloseHandle(handle);
        if (ok) { error.clear(); return; }
        error = std::error_code(static_cast<int>(failure), std::system_category());
    } else { error = std::error_code(static_cast<int>(GetLastError()), std::system_category()); }
    std::error_code ignored; fs::remove(link, ignored); // Newly created empty fixture only.
#endif
}

void lifecycle(const fs::path& fixtures) {
    const auto registry = fixtures / "config" / "projects.json";
    ProjectManager manager(registry);
    require(manager.recent().empty() && !fs::exists(registry), "Startup created or changed the recent registry");
    std::string error;
    const auto first = manager.createBlank(fixtures / "projects", "Alpha", error);
    require(first.has_value() && error.empty(), "First blank project creation failed");
    const auto second = manager.createBlank(fixtures / "projects", "Beta", error);
    require(second.has_value() && error.empty(), "Second blank project creation failed");
    require(first->root.is_absolute() && fs::canonical(first->root) == first->root && first->root != second->root, "Project roots are not separate canonical absolute directories");
    require(first->assetRoot == first->root / "assets" && first->layoutPath == first->root / ".tge" / "editor_layout.json", "Project asset/layout paths are incorrect");
    const auto scene = Json::parse(read(first->scenePath));
    require(scene["version"] == 1 && scene["entities"].is_array() && scene["entities"].empty(), "Blank project seeded gameplay content");
    std::size_t folders = 0U;
    for (const auto& entry : fs::directory_iterator(first->assetRoot)) {
        require(entry.is_directory() && fs::is_empty(entry.path()) == (entry.path().filename() != "scenes"), "Blank asset folders contain unexpected files");
        ++folders;
    }
    require(folders == 5U && !fs::exists(first->layoutPath) && !fs::exists(first->root / ".tge"), "Blank project created extra folders or layout state");
    require(manager.recent().size() == 2U && manager.recent().front().name == "Beta", "Recent project ordering is incorrect");
    const auto registryBefore = read(registry);
    ProjectManager reopened(registry);
    require(read(registry) == registryBefore && reopened.recent().size() == 2U, "Registry reload wrote state or lost projects");
    const auto opened = reopened.open(first->root, error);
    require(opened.has_value() && error.empty() && opened->manifestPath == first->manifestPath, "Directory reopen failed");
    require(reopened.recent().size() == 2U && reopened.recent().front().name == "Alpha", "Reopen duplicated or failed to reorder a recent project");
    require(reopened.open(first->root / "assets" / ".." / "Alpha.tgeproj", error).has_value() && reopened.recent().size() == 2U, "Normalized manifest path produced duplicate recent entries");

    const auto manifestBefore = read(first->manifestPath), sceneBefore = read(first->scenePath);
    require(!manager.createBlank(fixtures / "projects", "Alpha", error).has_value() && !error.empty(), "Existing project directory was accepted");
    require(read(first->manifestPath) == manifestBefore && read(first->scenePath) == sceneBefore, "Existing project was clobbered");
    fs::create_directory(fixtures / "projects" / "Occupied");
    write(fixtures / "projects" / "Occupied" / "sentinel.txt", "keep existing data");
    require(!manager.createBlank(fixtures / "projects", "Occupied", error), "Existing ordinary folder was accepted");
    require(read(fixtures / "projects" / "Occupied" / "sentinel.txt") == "keep existing data", "Existing ordinary folder was altered");
    for (const std::string name : {"", "../escape", "A/B", "A\\B", "CON", "nul.json", "trailing.", " leading", "trailing "}) {
        require(!manager.createBlank(fixtures / "projects", name, error), "Invalid project name was accepted");
    }
    write(fixtures / "parent-file", "keep parent file");
    require(!manager.createBlank(fixtures / "parent-file", "Child", error), "File was accepted as a project parent");
    require(read(fixtures / "parent-file") == "keep parent file", "Parent file was clobbered");

    const std::u8string unicodeName = u8"\u041f\u0440\u043e\u0435\u043a\u0442";
    const auto unicodeParent = fixtures / fs::path(u8"\u0420\u0443\u0441\u0441\u043a\u0430\u044f \u043f\u0430\u043f\u043a\u0430");
    const auto unicode = manager.createBlank(unicodeParent, std::string(unicodeName.begin(), unicodeName.end()), error);
    require(unicode.has_value() && error.empty(), "UTF-8 project/parent names failed");
#ifdef _WIN32
    const auto unicodeOpened = manager.open(fs::path(unicode->manifestPath.wstring()), error);
#else
    const auto unicodeOpened = manager.open(unicode->manifestPath, error);
#endif
    require(unicodeOpened.has_value() && unicodeOpened->name == unicode->name && unicodeOpened->root == unicode->root, "Unicode manifest did not reopen through native filesystem paths");
}

void guards(const fs::path& fixtures) {
    ProjectManager manager(fixtures / "guards-registry.json");
    std::string error;
    const auto project = manager.createBlank(fixtures / "guard-projects", "Guard", error);
    require(project.has_value(), "Cannot create guard fixture");
    const auto original = read(project->manifestPath);
    const Json valid = Json::parse(original);
    const auto reject = [&](Json description) {
        write(project->manifestPath, description.dump());
        require(!manager.open(project->manifestPath, error).has_value() && !error.empty(), "Invalid manifest was accepted");
        write(project->manifestPath, original);
    };
    auto description = valid; description["version"] = 2; reject(description);
    description = valid; description["version"] = "1"; reject(description);
    description = valid; description["name"] = Json::array(); reject(description);
    description = valid; description["assets"] = Json::array(); reject(description);
    description = valid; description["startupScene"] = false; reject(description);
    description = valid; description["assets"] = "../../outside"; reject(description);
    description = valid; description["assets"] = utf8(project->assetRoot); reject(description);
    description = valid; description["startupScene"] = "../../outside.scene.json"; reject(description);
    fs::create_directory(project->root / ".tge");
    write(project->layoutPath, R"({"version":1,"entities":[]})");
    description = valid; description["startupScene"] = "assets/../.tge/editor_layout.json"; reject(description);
    // Only our known fixture file and empty directory are removed to test a junction below.
    fs::remove(project->layoutPath); fs::remove(project->root / ".tge");
    write(project->manifestPath, std::string(64U * 1024U + 1U, ' '));
    require(!manager.open(project->manifestPath, error), "Oversize manifest accepted");
    write(project->manifestPath, original);
    const auto sceneBefore = read(project->scenePath);
    write(project->scenePath, R"({"version":1,"entities":{}})");
    require(!manager.open(project->manifestPath, error), "Malformed startup scene accepted");
    write(project->scenePath, sceneBefore);
    write(project->scenePath, std::string(16U * 1024U * 1024U + 1U, ' '));
    require(!manager.open(project->manifestPath, error), "Oversize startup scene accepted");
    write(project->scenePath, sceneBefore);
    require(manager.open(project->manifestPath, error).has_value(), "Valid project did not recover after invalid manifests");

    fs::create_directory(fixtures / "outside");
    write(fixtures / "outside" / "outside.scene.json", R"({"version":1,"entities":[]})");
    std::error_code symlinkError;
    directoryLink(fixtures / "outside", project->root / "Escape", symlinkError);
    if (!symlinkError) {
        description = valid; description["assets"] = "Escape"; reject(description);
        description = valid; description["startupScene"] = "Escape/outside.scene.json"; reject(description);
        directoryLink(fixtures / "outside", project->root / ".tge", symlinkError);
        require(!symlinkError && !manager.open(project->manifestPath, error), "Layout symlink escape accepted");
    } else {
        std::cout << "Directory symlink guard fixture skipped: " << symlinkError.message() << '\n';
    }
}

void registryFailures(const fs::path& fixtures) {
    std::string error;
    const auto badRegistry = fixtures / "bad-registry.json";
    write(badRegistry, "{ broken registry");
    ProjectManager manager(badRegistry);
    manager.refresh();
    require(manager.recent().empty() && read(badRegistry) == "{ broken registry", "Corrupt registry was not read-only and graceful");
    write(badRegistry, std::string(128U * 1024U + 1U, ' '));
    manager.refresh(); require(manager.recent().empty(), "Oversize registry was accepted");
    Json records{{"version", 1}, {"projects", Json::array()}};
    for (int index = 0; index < 25; ++index) {
        const auto path = fixtures / ("Recent" + std::to_string(index) + ".tgeproj");
        records["projects"].push_back({{"name", "Recent" + std::to_string(index)}, {"manifestPath", utf8(path)}});
        if (index == 0) { records["projects"].push_back({{"name", "Duplicate"}, {"manifestPath", utf8(path)}}); }
    }
    records["projects"].push_back({{"name", 1}, {"manifestPath", false}});
    write(badRegistry, records.dump()); manager.refresh();
    require(manager.recent().size() == 20U && manager.recent()[1].name == "Recent1", "Recents were not bounded and deduplicated");

    fs::create_directory(fixtures / "registry-is-directory");
    ProjectManager unavailable(fixtures / "registry-is-directory");
    const auto usable = unavailable.createBlank(fixtures / "usable-projects", "Usable", error);
    require(usable.has_value() && !error.empty() && fs::exists(usable->manifestPath), "Registry failure invalidated a newly created project");
    require(unavailable.open(usable->root, error).has_value() && !error.empty(), "Registry failure prevented opening a usable project");
    require(fs::is_directory(fixtures / "registry-is-directory"), "Registry persistence clobbered a directory");

    write(fixtures / "registry-target.json", "preserve target");
    std::error_code symlinkError;
    fs::create_symlink(fixtures / "registry-target.json", fixtures / "registry-link.json", symlinkError);
    if (!symlinkError) {
        ProjectManager linked(fixtures / "registry-link.json");
        require(linked.recent().empty() && linked.open(usable->root, error).has_value() && !error.empty(), "Registry symlink was followed or invalidated project open");
        require(read(fixtures / "registry-target.json") == "preserve target", "Registry symlink target was clobbered");
    }
}
} // namespace

int main() {
    try {
        const auto fixtures = fs::temp_directory_path() / ("tge-project-manager-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directory(fixtures);
        lifecycle(fixtures); guards(fixtures); registryFailures(fixtures);
        require(ProjectManager::defaultRegistryPath().is_absolute() && ProjectManager::defaultProjectsDirectory().is_absolute(), "Default user paths are not absolute");
    } catch (const std::exception& error) { std::cerr << "ProjectManager test failure: " << error.what() << '\n'; return 1; }
    std::cout << "ProjectManager blank projects, UTF-8 paths, reopen/recents, containment and no-clobber checks passed\n";
    return 0;
}
