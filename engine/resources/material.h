#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>

namespace engine::resources {

enum class MaterialBlend : std::uint8_t { Opaque, Alpha, Additive };
struct MaterialGraphProgram;

struct MaterialDefinition final {
    std::string shader;
    std::string texture;
    glm::vec4 tint{1.0F};
    glm::vec2 uvScale{1.0F};
    MaterialBlend blend{MaterialBlend::Opaque};
    bool depthTest{true};
    bool depthWrite{true};
    nlohmann::json graph;
    std::shared_ptr<const MaterialGraphProgram> graphProgram;
};

struct Material final {
    std::string path;
    MaterialDefinition definition;
    std::uint64_t revision{0};
    std::string lastError;
    bool ready{false};
    std::string runtimeError;
};

class MaterialLibrary final {
public:
    explicit MaterialLibrary(std::filesystem::path root = {});
    void setRoot(std::filesystem::path root);
    void setBuiltinShaderRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& root() const { return m_root; }
    std::shared_ptr<Material> load(const std::string& path);
    void poll(bool force = false);
    bool preview(const std::string& path, const nlohmann::json& data, MaterialDefinition& result,
                 std::string* error = nullptr) const;
    bool save(const std::string& path, const nlohmann::json& data, std::string* error = nullptr);
    [[nodiscard]] std::vector<std::string> diagnostics() const { return m_diagnostics; }

private:
    struct Stamp final {
        std::filesystem::path path;
        std::filesystem::file_time_type time{};
        std::uintmax_t size{0};
        bool exists{false};
    };
    struct Entry final {
        std::shared_ptr<Material> material;
        std::vector<Stamp> dependencies;
    };
    std::filesystem::path m_root;
    std::filesystem::path m_builtinShaderRoot;
    std::unordered_map<std::string, Entry> m_entries;
    std::vector<std::string> m_diagnostics;
    std::chrono::steady_clock::time_point m_nextPoll{};

    std::filesystem::path containedPath(const std::string& path,
                                      const std::filesystem::path& relativeTo = {}) const;
    static Stamp stamp(const std::filesystem::path& path);
    static bool changed(const Stamp& previous);
    MaterialDefinition resolve(const std::filesystem::path& path, std::vector<Stamp>& dependencies,
                               std::vector<std::filesystem::path>& stack,
                               const nlohmann::json* replacement = nullptr,
                               const std::filesystem::path& replacementPath = {}) const;
    void update(Entry& entry);
    void report(const std::string& message);
};

}
