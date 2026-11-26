#include "engine/resources/material.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "engine/core/AtomicFile.h"
#include "engine/resources/material_graph.h"

namespace engine::resources {
namespace {
constexpr std::size_t maximumFileBytes = 256U * 1024U;
constexpr std::size_t maximumMaterials = 256;
constexpr std::size_t maximumInheritance = 16;

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string readBounded(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("cannot open material"); }
    std::string text(maximumFileBytes + 1U, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto count = input.gcount();
    if (count > static_cast<std::streamsize>(maximumFileBytes)) {
        throw std::runtime_error("material exceeds 256 KiB");
    }
    if (input.bad()) { throw std::runtime_error("cannot read material"); }
    text.resize(static_cast<std::size_t>(count));
    return text;
}

bool equal(const MaterialDefinition& a, const MaterialDefinition& b) {
    return a.shader == b.shader && a.texture == b.texture && a.tint == b.tint &&
           a.uvScale == b.uvScale && a.blend == b.blend && a.depthTest == b.depthTest &&
           a.depthWrite == b.depthWrite && a.graph == b.graph;
}

template<int Count, typename Vector>
void readVector(const nlohmann::json& data, const char* key, Vector& result) {
    const auto found = data.find(key);
    if (found == data.end()) { return; }
    if (!found->is_array() || found->size() != Count) {
        throw std::runtime_error(std::string(key) + " must contain " + std::to_string(Count) + " numbers");
    }
    for (int index = 0; index < Count; ++index) {
        const auto& item = (*found)[static_cast<std::size_t>(index)];
        if (!item.is_number()) { throw std::runtime_error(std::string(key) + " must contain numbers"); }
        const double value = item.template get<double>();
        if (!std::isfinite(value) || std::abs(value) > 1000000.0) {
            throw std::runtime_error(std::string(key) + " contains a non-finite or excessive value");
        }
        result[index] = static_cast<float>(value);
    }
}

void applyGraphTextures(nlohmann::json& graph, const nlohmann::json& textures) {
    if (!textures.is_object()) { throw std::runtime_error("graphTextures must map texture node IDs to paths"); }
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) {
        throw std::runtime_error("graphTextures requires a material graph");
    }
    auto& nodes = graph["nodes"];
    if (nodes.size() > maxMaterialGraphNodes || textures.size() > nodes.size()) {
        throw std::runtime_error("graph texture overrides exceed the graph node limit");
    }
    for (const auto& [id, path] : textures.items()) {
        if (!path.is_string()) { throw std::runtime_error("graph texture override must be a path string: " + id); }
        const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto& candidate) {
            return candidate.is_object() && candidate.contains("id") && candidate["id"].is_number_integer() &&
                   candidate["id"].dump() == id && candidate.contains("type") && candidate["type"] == "texture";
        });
        if (node == nodes.end()) { throw std::runtime_error("graph texture override refers to an unknown texture node: " + id); }
        (*node)["texture"] = path;
    }
}
}

MaterialLibrary::MaterialLibrary(std::filesystem::path root) { setRoot(std::move(root)); }

void MaterialLibrary::setBuiltinShaderRoot(std::filesystem::path root) {
    m_builtinShaderRoot = root.empty() ? std::filesystem::path{} : std::filesystem::weakly_canonical(std::filesystem::absolute(root));
    m_entries.clear();
    m_diagnostics.clear();
}

void MaterialLibrary::setRoot(std::filesystem::path root) {
    if (root.empty()) { root = std::filesystem::current_path() / "assets"; }
    m_root = std::filesystem::weakly_canonical(std::filesystem::absolute(std::move(root)));
    m_entries.clear();
    m_diagnostics.clear();
    m_nextPoll = {};
}

std::filesystem::path MaterialLibrary::containedPath(const std::string& text,
                                                    const std::filesystem::path& relativeTo) const {
    if (text.empty() || text.size() > 4096 || text.find('\0') != std::string::npos) {
        throw std::runtime_error("asset path is empty or invalid");
    }
    auto path = std::filesystem::u8path(text);
    if (!path.is_absolute()) {
        if (*path.begin() == "assets") { path = path.lexically_relative("assets"); }
        if (path.empty() || path == ".") { throw std::runtime_error("asset path must name a file"); }
        const bool assetRootReference = *path.begin() == "materials" || *path.begin() == "shaders_hlsl" ||
                                        *path.begin() == "textures";
        path = (relativeTo.empty() || assetRootReference ? m_root : relativeTo) / path;
    }
    path = std::filesystem::weakly_canonical(path);
    const auto relative = path.lexically_relative(m_root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." || relative == ".") {
        throw std::runtime_error("asset path escapes the project asset root: " + text);
    }
    return path;
}

MaterialLibrary::Stamp MaterialLibrary::stamp(const std::filesystem::path& path) {
    Stamp result;
    result.path = path;
    std::error_code error;
    result.exists = std::filesystem::is_regular_file(path, error);
    if (result.exists) {
        result.time = std::filesystem::last_write_time(path, error);
        if (error) { result.exists = false; }
        result.size = std::filesystem::file_size(path, error);
        if (error) { result.exists = false; }
    }
    return result;
}

bool MaterialLibrary::changed(const Stamp& previous) {
    const auto current = stamp(previous.path);
    return current.exists != previous.exists || current.time != previous.time || current.size != previous.size;
}

MaterialDefinition MaterialLibrary::resolve(const std::filesystem::path& path, std::vector<Stamp>& dependencies,
                                            std::vector<std::filesystem::path>& stack,
                                            const nlohmann::json* replacement,
                                            const std::filesystem::path& replacementPath) const {
    try {
        dependencies.push_back(stamp(path));
        if (stack.size() >= maximumInheritance) { throw std::runtime_error("base inheritance exceeds 16 levels"); }
        if (std::find(stack.begin(), stack.end(), path) != stack.end()) {
            throw std::runtime_error("base inheritance cycle");
        }
        stack.push_back(path);
        const auto data = replacement != nullptr && path == replacementPath
            ? *replacement : nlohmann::json::parse(readBounded(path));
        if (!data.is_object()) { throw std::runtime_error("material must be a JSON object"); }
        static constexpr std::array allowed{"version", "name", "base", "shader", "texture", "tint", "uvScale",
                                             "blend", "depthTest", "depthWrite", "graph", "graphTextures"};
        for (auto item = data.begin(); item != data.end(); ++item) {
            if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) {
                throw std::runtime_error("unknown material property: " + item.key());
            }
        }
        if (const auto version = data.find("version"); version != data.end() && *version != 1 && *version != 2) {
            throw std::runtime_error("unsupported material version");
        }
        MaterialDefinition definition;
        if (const auto base = data.find("base"); base != data.end()) {
            if (!base->is_string()) { throw std::runtime_error("base must be a path string"); }
            const auto basePath = containedPath(base->get<std::string>(), path.parent_path());
            definition = resolve(basePath, dependencies, stack, replacement, replacementPath);
        }
        const auto reference = [&](const char* key, std::string& value, const bool optional) {
            const auto item = data.find(key);
            if (item == data.end()) { return; }
            if (!item->is_string()) { throw std::runtime_error(std::string(key) + " must be a path string"); }
            value = item->get<std::string>();
            if (value.empty() && optional) { return; }
            const auto resourcePath = containedPath(value);
            dependencies.push_back(stamp(resourcePath));
            if (!dependencies.back().exists && std::string_view(key) == "shader" &&
                utf8(resourcePath.lexically_relative(m_root)) == "shaders_hlsl/textured.shader.json" && !m_builtinShaderRoot.empty()) {
                dependencies.push_back(stamp(m_builtinShaderRoot / "shaders_hlsl/textured.shader.json"));
                if (dependencies.back().exists) { value = "shaders_hlsl/textured.shader.json"; return; }
            }
            if (!dependencies.back().exists) { throw std::runtime_error(std::string(key) + " file is missing: " + value); }
            value = utf8(resourcePath.lexically_relative(m_root));
        };
        const auto graph = data.find("graph");
        const auto graphTextures = data.find("graphTextures");
        if (graph != data.end()) {
            if (!graph->is_object()) { throw std::runtime_error("graph must be an object"); }
            definition.graph = *graph;
        }
        if (graphTextures != data.end()) { applyGraphTextures(definition.graph, *graphTextures); }
        if (definition.graph.is_null()) {
            reference("shader", definition.shader, false);
            reference("texture", definition.texture, true);
            if (definition.shader.empty()) { throw std::runtime_error("shader or graph is required (directly or through base)"); }
        } else if (graph != data.end() || graphTextures != data.end()) {
            auto program = compileMaterialGraph(definition.graph);
            for (auto& node : definition.graph["nodes"]) {
                const auto texture = node.find("texture");
                if (texture == node.end() || texture->get_ref<const std::string&>().empty()) { continue; }
                *texture = utf8(containedPath(texture->get_ref<const std::string&>()).lexically_relative(m_root));
            }
            for (auto& texture : program.textures) {
                if (texture.empty()) { continue; }
                const auto resourcePath = containedPath(texture);
                dependencies.push_back(stamp(resourcePath));
                if (!dependencies.back().exists) { throw std::runtime_error("graph texture file is missing: " + texture); }
                texture = utf8(resourcePath.lexically_relative(m_root));
            }
            definition.shader.clear();
            definition.texture.clear();
            definition.graphProgram = std::make_shared<const MaterialGraphProgram>(std::move(program));
        }
        readVector<4>(data, "tint", definition.tint);
        readVector<2>(data, "uvScale", definition.uvScale);
        if (const auto blend = data.find("blend"); blend != data.end()) {
            if (!blend->is_string()) { throw std::runtime_error("blend must be opaque, alpha or additive"); }
            const auto value = blend->get<std::string>();
            if (value == "opaque") { definition.blend = MaterialBlend::Opaque; }
            else if (value == "alpha") { definition.blend = MaterialBlend::Alpha; }
            else if (value == "additive") { definition.blend = MaterialBlend::Additive; }
            else { throw std::runtime_error("blend must be opaque, alpha or additive"); }
        }
        for (const auto* key : {"depthTest", "depthWrite"}) {
            const auto value = data.find(key);
            if (value == data.end()) { continue; }
            if (!value->is_boolean()) { throw std::runtime_error(std::string(key) + " must be boolean"); }
            (std::string_view{key} == "depthTest" ? definition.depthTest : definition.depthWrite) = value->get<bool>();
        }
        stack.pop_back();
        return definition;
    } catch (const std::exception& error) {
        throw std::runtime_error(utf8(path.lexically_relative(m_root)) + ": " + error.what());
    }
}

void MaterialLibrary::report(const std::string& message) {
    if (m_diagnostics.size() == 64) { m_diagnostics.erase(m_diagnostics.begin()); }
    m_diagnostics.push_back(message);
}

void MaterialLibrary::update(Entry& entry) {
    std::vector<Stamp> dependencies;
    std::vector<std::filesystem::path> stack;
    try {
        auto definition = resolve(containedPath(entry.material->path), dependencies, stack);
        const bool resolutionChanged = entry.dependencies.size() != dependencies.size() ||
            !std::equal(entry.dependencies.begin(), entry.dependencies.end(), dependencies.begin(), dependencies.end(),
                [](const Stamp& a, const Stamp& b) { return a.path == b.path && a.exists == b.exists; });
        if (!entry.material->ready || !equal(entry.material->definition, definition) || resolutionChanged) {
            entry.material->definition = std::move(definition);
            ++entry.material->revision;
        }
        entry.material->ready = true;
        entry.material->lastError.clear();
    } catch (const std::exception& error) {
        if (entry.material->lastError != error.what()) { report(error.what()); }
        entry.material->lastError = error.what();
    }
    entry.dependencies = std::move(dependencies);
}

std::shared_ptr<Material> MaterialLibrary::load(const std::string& path) {
    std::string key;
    try { key = utf8(containedPath(path).lexically_relative(m_root)); }
    catch (const std::exception& error) {
        auto invalid = std::make_shared<Material>();
        invalid->path = path;
        invalid->lastError = error.what();
        report(path + ": " + error.what());
        return invalid;
    }
    if (const auto found = m_entries.find(key); found != m_entries.end()) { return found->second.material; }
    auto material = std::make_shared<Material>();
    material->path = key;
    if (m_entries.size() >= maximumMaterials) {
        material->lastError = key + ": material cache limit (256) reached";
        report(material->lastError);
        return material;
    }
    auto& entry = m_entries[key];
    entry.material = material;
    update(entry);
    return material;
}

void MaterialLibrary::poll(const bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < m_nextPoll) { return; }
    m_nextPoll = now + std::chrono::milliseconds(250);
    for (auto& [key, entry] : m_entries) {
        (void)key;
        if (force || std::any_of(entry.dependencies.begin(), entry.dependencies.end(), changed)) { update(entry); }
    }
}

bool MaterialLibrary::preview(const std::string& path, const nlohmann::json& data,
                              MaterialDefinition& result, std::string* error) const {
    if (error != nullptr) { error->clear(); }
    try {
        const auto target = containedPath(path);
        if (data.dump(2).size() + 1U > maximumFileBytes) { throw std::runtime_error("material exceeds 256 KiB"); }
        std::vector<Stamp> dependencies;
        std::vector<std::filesystem::path> stack;
        auto definition = resolve(target, dependencies, stack, &data, target);
        result = std::move(definition);
        return true;
    } catch (const std::exception& failure) {
        if (error != nullptr) { *error = path + ": " + failure.what(); }
        return false;
    }
}

bool MaterialLibrary::save(const std::string& path, const nlohmann::json& data, std::string* error) {
    if (error != nullptr) { error->clear(); }
    try {
        const auto target = containedPath(path);
        const auto contents = data.dump(2) + '\n';
        if (contents.size() > maximumFileBytes) { throw std::runtime_error("material exceeds 256 KiB"); }
        std::vector<Stamp> dependencies;
        std::vector<std::filesystem::path> stack;
        (void)resolve(target, dependencies, stack, &data, target);
        std::filesystem::create_directories(target.parent_path());
        std::string failure;
        if (!core::writeFileAtomically(target, contents, failure)) { throw std::runtime_error(failure); }
        poll(true);
        return true;
    } catch (const std::exception& failure) {
        const std::string message = path + ": " + failure.what();
        report(message);
        if (error != nullptr) { *error = message; }
        return false;
    }
}

}
