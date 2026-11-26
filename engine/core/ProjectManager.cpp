#include "engine/core/ProjectManager.h"
#include "engine/network/NetworkSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShlObj.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace engine::core {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t kManifestLimit = 64U * 1024U;
constexpr std::size_t kRegistryLimit = 128U * 1024U;
constexpr std::size_t kSceneLimit = 16U * 1024U * 1024U;
constexpr std::size_t kRecentLimit = 20U;

std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
fs::path pathFromUtf8(const std::string& value) { return fs::u8path(value); }

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}
bool validName(const std::string& name) {
    if (name.empty() || name.size() > 80U || name == "." || name == ".." || name.back() == '.' ||
        std::isspace(static_cast<unsigned char>(name.front())) || std::isspace(static_cast<unsigned char>(name.back()))) { return false; }
    for (const unsigned char character : name) {
        if (character < 32U || character == 127U || std::string("<>:\"/\\|?*").find(static_cast<char>(character)) != std::string::npos) { return false; }
    }
    const auto device = lowercase(name.substr(0, name.find('.')));
    if (device == "con" || device == "prn" || device == "aux" || device == "nul") { return false; }
    if (device.size() == 4U && (device.starts_with("com") || device.starts_with("lpt")) && device[3] >= '1' && device[3] <= '9') { return false; }
    try { (void)Json(name).dump(); } catch (const std::exception&) { return false; }
    return true;
}
bool samePath(const fs::path& first, const fs::path& second) {
#ifdef _WIN32
    return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return first == second;
#endif
}
bool within(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
fs::path containedPath(const fs::path& root, const std::string& value) {
    const auto relative = pathFromUtf8(value);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        throw std::invalid_argument("Project paths must be nonempty relative paths");
    }
    const auto resolved = fs::weakly_canonical(root / relative);
    if (!within(root, resolved)) { throw std::invalid_argument("Project path escapes its root: " + value); }
    return resolved;
}
std::string readText(const fs::path& path, const std::size_t limit) {
    if (!fs::is_regular_file(path)) { throw std::invalid_argument("Expected a regular file: " + utf8(path)); }
    const auto size = fs::file_size(path);
    if (size > limit) { throw std::invalid_argument("File exceeds project size limit: " + utf8(path)); }
    std::ifstream file(path, std::ios::binary);
    if (!file) { throw std::runtime_error("Cannot read " + utf8(path)); }
    std::string text(static_cast<std::size_t>(size), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (!file.eof() && file.peek() != std::char_traits<char>::eof()) {
        throw std::invalid_argument("File changed while reading: " + utf8(path));
    }
    return text;
}
Json readJson(const fs::path& path, const std::size_t limit) {
    return Json::parse(readText(path, limit));
}
void requireVersion(const Json& value, const char* label) {
    if (!value.is_object() || !value.contains("version") || !value["version"].is_number_integer() || value["version"] != 1) {
        throw std::invalid_argument(std::string(label) + " must be a version 1 JSON object");
    }
}
std::string requiredString(const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string() || found->get_ref<const std::string&>().empty()) {
        throw std::invalid_argument(std::string("Project '") + key + "' must be a nonempty string");
    }
    return found->get<std::string>();
}

bool writeExclusive(const fs::path& path, const std::string& text, bool& created, std::string& error) {
    created = false;
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "Cannot exclusively create " + utf8(path) + " (Windows error " + std::to_string(GetLastError()) + ")"; return false; }
    created = true;
    DWORD written{};
    const bool ok = text.size() <= std::numeric_limits<DWORD>::max() &&
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 && written == text.size() && FlushFileBuffers(file) != 0;
    const DWORD failure = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) { error = "Cannot write " + utf8(path) + " (Windows error " + std::to_string(failure) + ")"; }
    return ok;
#else
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0) { error = "Cannot exclusively create " + utf8(path) + ": " + std::generic_category().message(errno); return false; }
    created = true;
    std::size_t offset = 0;
    bool ok = true;
    while (offset < text.size()) {
        const auto written = ::write(file, text.data() + offset, text.size() - offset);
        if (written < 0 && errno == EINTR) { continue; }
        if (written <= 0) { ok = false; break; }
        offset += static_cast<std::size_t>(written);
    }
    if (ok) { ok = ::fsync(file) == 0; }
    const int failure = errno;
    if (::close(file) != 0) { ok = false; }
    if (!ok) { error = "Cannot write " + utf8(path) + ": " + std::generic_category().message(failure); }
    return ok;
#endif
}
fs::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? pathFromUtf8(value) : fs::path{};
}
#ifdef _WIN32
fs::path knownFolder(const KNOWNFOLDERID& id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value))) { return {}; }
    fs::path path(value); CoTaskMemFree(value); return path;
}
#endif
fs::path homeDirectory() {
#ifdef _WIN32
    auto home = environmentPath("USERPROFILE");
#else
    auto home = environmentPath("HOME");
#endif
    return home.empty() ? fs::current_path() : home;
}
void cleanupCreated(const fs::path& root, const std::vector<fs::path>& files, const std::vector<fs::path>& directories) noexcept {
    try {
        if (fs::is_symlink(root) || !samePath(fs::weakly_canonical(root), root)) { return; }
        for (auto iterator = files.rbegin(); iterator != files.rend(); ++iterator) {
            if (within(root, fs::weakly_canonical(iterator->parent_path()))) { std::error_code ignored; fs::remove(*iterator, ignored); }
        }
        for (auto iterator = directories.rbegin(); iterator != directories.rend(); ++iterator) {
            if (samePath(*iterator, root) || within(root, fs::weakly_canonical(*iterator))) {
                std::error_code ignored; fs::remove(*iterator, ignored);
            }
        }
    } catch (...) {}
}
}

ProjectManager::ProjectManager(fs::path registryPath) : m_registryPath(std::move(registryPath)) {
    if (m_registryPath.empty()) { m_registryPath = defaultRegistryPath(); }
    try {
        const auto absolute = fs::absolute(m_registryPath).lexically_normal();
        m_registryPath = fs::weakly_canonical(absolute.parent_path()) / absolute.filename();
    } catch (const std::exception&) { }
    refresh();
}
fs::path ProjectManager::defaultRegistryPath() {
#ifdef _WIN32
    auto base = knownFolder(FOLDERID_LocalAppData);
    if (base.empty()) { base = environmentPath("LOCALAPPDATA"); }
    if (base.empty()) { base = homeDirectory() / "AppData" / "Local"; }
#else
    auto base = environmentPath("XDG_DATA_HOME");
    if (base.empty() || !base.is_absolute()) { base = homeDirectory() / ".local" / "share"; }
#endif
    return fs::absolute(base / "TurboGameEngine" / "projects.json").lexically_normal();
}
fs::path ProjectManager::defaultProjectsDirectory() {
#ifdef _WIN32
    auto documents = knownFolder(FOLDERID_Documents);
    if (documents.empty()) { documents = homeDirectory() / "Documents"; }
#else
    auto documents = environmentPath("XDG_DOCUMENTS_DIR");
    if (documents.empty() || !documents.is_absolute()) { documents = homeDirectory() / "Documents"; }
#endif
    return fs::absolute(documents / "TurboGameEngineProjects").lexically_normal();
}
std::optional<Project> ProjectManager::createBlank(const fs::path& parentDirectory, const std::string& name, std::string& error) {
    error.clear();
    std::vector<fs::path> files, directories;
    fs::path root;
    try {
        if (!validName(name)) { throw std::invalid_argument("Use a project name of 1-80 bytes without reserved names, path separators or trailing spaces/dots"); }
        if (parentDirectory.empty()) { throw std::invalid_argument("Select a project parent directory"); }
        fs::create_directories(parentDirectory);
        const auto parent = fs::canonical(parentDirectory);
        if (!fs::is_directory(parent)) { throw std::invalid_argument("Project parent must be a directory"); }
        root = parent / pathFromUtf8(name);
        if (!fs::create_directory(root)) { throw std::invalid_argument("Project directory already exists; choose a new name"); }
        root = fs::canonical(root); directories.push_back(root);
        const auto createDirectory = [&](const fs::path& path) {
            if (!within(root, fs::weakly_canonical(path)) || !fs::create_directory(path)) { throw std::runtime_error("Cannot exclusively create project directory " + utf8(path)); }
            directories.push_back(path);
        };
        createDirectory(root / "assets");
        for (const char* directory : {"scripts", "scenes", "prefabs", "models", "textures"}) { createDirectory(root / "assets" / directory); }
        Project project{name, root, root / pathFromUtf8(name + ".tgeproj"), root / "assets",
                        root / "assets" / "scenes" / "main.scene.json", root / ".tge" / "editor_layout.json"};
        const Json scene{{"version", 1}, {"entities", Json::array()}};
        const Json manifest{{"version", 1}, {"name", name}, {"assets", "assets"}, {"startupScene", "assets/scenes/main.scene.json"}};
        const auto write = [&](const fs::path& path, const Json& data) {
            bool created{}; std::string failure;
            const bool ok = writeExclusive(path, data.dump(4) + "\n", created, failure);
            if (created) { files.push_back(path); }
            if (!ok) { throw std::runtime_error(failure); }
        };
        write(project.scenePath, scene); write(project.manifestPath, manifest);
        remember(project, error);
        return project;
    } catch (const std::exception& failure) {
        error = failure.what(); if (!directories.empty()) { cleanupCreated(root, files, directories); }
        return std::nullopt;
    }
}
std::optional<Project> ProjectManager::open(const fs::path& manifestOrDirectory, std::string& error) {
    try {
        auto project = load(manifestOrDirectory, error);
        if (project) remember(*project, error);
        return project;
    } catch (const std::exception& failure) { error = failure.what(); return std::nullopt; }
}

std::optional<Project> ProjectManager::load(const fs::path& manifestOrDirectory, std::string& error,
                                            std::uint64_t* networkCompatibility, std::string* startupSceneContents) {
    error.clear();
    if (networkCompatibility) *networkCompatibility = 0;
    if (startupSceneContents) startupSceneContents->clear();
    try {
        if (manifestOrDirectory.empty()) { throw std::invalid_argument("Select a .tgeproj manifest or project directory"); }
        fs::path manifest = fs::absolute(manifestOrDirectory).lexically_normal();
        if (fs::is_directory(manifest)) {
            fs::path found;
            for (const auto& entry : fs::directory_iterator(manifest)) {
                if (lowercase(utf8(entry.path().extension())) != ".tgeproj" || !entry.is_regular_file()) { continue; }
                if (!found.empty()) { throw std::invalid_argument("Directory contains multiple .tgeproj files; select one manifest"); }
                found = entry.path();
            }
            if (found.empty()) { throw std::invalid_argument("Directory contains no .tgeproj manifest"); }
            manifest = found;
        }
        if (lowercase(utf8(manifest.extension())) != ".tgeproj") { throw std::invalid_argument("Select a .tgeproj manifest"); }
        const auto root = fs::canonical(manifest.parent_path());
        manifest = fs::canonical(manifest);
        if (!within(root, manifest)) { throw std::invalid_argument("Manifest symlink escapes project root"); }
        const Json description = readJson(manifest, kManifestLimit);
        requireVersion(description, "Project manifest");
        const std::string name = requiredString(description, "name");
        if (!validName(name)) { throw std::invalid_argument("Invalid project name in manifest"); }
        const auto assets = containedPath(root, requiredString(description, "assets"));
        const auto scene = containedPath(root, requiredString(description, "startupScene"));
        const auto layout = containedPath(root, ".tge/editor_layout.json");
        std::error_code identityError;
        if (samePath(scene, layout) || fs::equivalent(scene, layout, identityError)) {
            throw std::invalid_argument("Startup scene must be separate from editor layout state");
        }
        if (!fs::is_directory(assets)) { throw std::invalid_argument("Project assets directory is missing"); }
        if (lowercase(utf8(scene.extension())) != ".json") { throw std::invalid_argument("Startup scene must be a JSON scene file"); }
        auto sceneText = readText(scene, kSceneLimit);
        const auto sceneData = Json::parse(sceneText);
        requireVersion(sceneData, "Startup scene");
        if (!sceneData.contains("entities") || !sceneData["entities"].is_array()) { throw std::invalid_argument("Startup scene entities must be an array"); }
        Project project{name, root, manifest, assets, scene, layout};
        if (const auto limit = description.find("networkPlayerLimit"); limit != description.end()) {
            if (!limit->is_number_unsigned() || !limit->get<std::uint64_t>() ||
                limit->get<std::uint64_t>() > net::MaxPeers) {
                throw std::invalid_argument("networkPlayerLimit must be an integer from 1 to " + std::to_string(net::MaxPeers));
            }
            project.networkPlayerLimit = limit->get<unsigned>();
        }
        if (networkCompatibility) {
            std::uint64_t hash = 14695981039346656037ULL;
            const auto append = [&](std::string_view value) {
                for (const unsigned char byte : value) {
                    hash ^= byte;
                    hash *= 1099511628211ULL;
                }
                hash *= 1099511628211ULL;
            };
            append(name);
            append(utf8(scene.lexically_relative(root)));
            append(sceneText);
            if (project.networkPlayerLimit) append(std::to_string(project.networkPlayerLimit));
            *networkCompatibility = hash ? hash : 1;
        }
        if (startupSceneContents) *startupSceneContents = std::move(sceneText);
        return project;
    } catch (const std::exception& failure) { error = failure.what(); return std::nullopt; }
}
const std::vector<RecentProject>& ProjectManager::recent() const { return m_recent; }
void ProjectManager::refresh() {
    m_recent.clear();
    try {
        if (!fs::exists(m_registryPath) || fs::is_symlink(m_registryPath)) { return; }
        const auto data = readJson(m_registryPath, kRegistryLimit);
        requireVersion(data, "Recent registry");
        if (!data.contains("projects") || !data["projects"].is_array()) { return; }
        for (const auto& entry : data["projects"]) {
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() || !entry.contains("manifestPath") || !entry["manifestPath"].is_string()) { continue; }
            const auto name = entry["name"].get<std::string>();
            const auto path = pathFromUtf8(entry["manifestPath"].get<std::string>());
            if (!validName(name) || !path.is_absolute() || lowercase(utf8(path.extension())) != ".tgeproj") { continue; }
            const auto normalized = fs::weakly_canonical(path);
            if (std::any_of(m_recent.begin(), m_recent.end(), [&](const auto& recent) { return samePath(recent.manifestPath, normalized); })) { continue; }
            m_recent.push_back({name, normalized});
            if (m_recent.size() == kRecentLimit) { break; }
        }
    } catch (const std::exception&) { m_recent.clear(); }
}
void ProjectManager::remember(const Project& project, std::string& error) {
    std::erase_if(m_recent, [&](const auto& entry) { return samePath(entry.manifestPath, project.manifestPath); });
    m_recent.insert(m_recent.begin(), {project.name, project.manifestPath});
    if (m_recent.size() > kRecentLimit) { m_recent.resize(kRecentLimit); }
    std::string warning;
    if (!persist(warning)) { error = "Project is ready, but recent-project registry could not be saved: " + warning; }
}
bool ProjectManager::persist(std::string& error) const {
    fs::path temporary;
    bool created{};
    try {
        if (fs::is_symlink(m_registryPath) || (fs::exists(m_registryPath) && !fs::is_regular_file(m_registryPath))) {
            throw std::invalid_argument("Registry target must be a regular file, not a directory or symlink");
        }
        fs::create_directories(m_registryPath.parent_path());
        Json data{{"version", 1}, {"projects", Json::array()}};
        for (const auto& project : m_recent) { data["projects"].push_back({{"name", project.name}, {"manifestPath", utf8(project.manifestPath)}}); }
        static std::atomic_uint64_t sequence{};
        temporary = m_registryPath.parent_path() / pathFromUtf8(utf8(m_registryPath.filename()) + ".tmp-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence.fetch_add(1U)));
        if (!writeExclusive(temporary, data.dump(4) + "\n", created, error)) { throw std::runtime_error(error); }
#ifdef _WIN32
        if (MoveFileExW(temporary.c_str(), m_registryPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            throw std::runtime_error("Cannot atomically replace recent registry (Windows error " + std::to_string(GetLastError()) + ")");
        }
#else
        fs::rename(temporary, m_registryPath);
#endif
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        if (created) { std::error_code ignored; fs::remove(temporary, ignored); }
        return false;
    }
}

}
