#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "engine/resources/material.h"
#include "engine/resources/material_graph.h"

namespace {
using engine::resources::MaterialBlend;
using engine::resources::MaterialDefinition;
using engine::resources::MaterialLibrary;
using Json = nlohmann::json;

void expect(const bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

class Fixtures final {
public:
    Fixtures() {
        root = std::filesystem::temp_directory_path() /
            ("tge-material-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "materials");
        write("shaders_hlsl/test.shader.json", "{}");
        write("textures/white.ppm", "P3\n1 1\n255\n255 255 255\n");
        write("textures/other.ppm", "P3\n1 1\n255\n255 0 0\n");
    }
    ~Fixtures() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void write(const std::string& path, const std::string& contents) const {
        const auto target = root / std::filesystem::u8path(path);
        std::filesystem::create_directories(target.parent_path());
        std::ofstream stream(target, std::ios::binary);
        stream << contents;
        expect(static_cast<bool>(stream), "Cannot write material fixture");
    }
    void json(const std::string& path, const Json& data) const { write(path, data.dump()); }
    std::string read(const std::string& path) const {
        std::ifstream stream(root / path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
    std::filesystem::path root;
};

Json base() {
    return {{"version", 1}, {"shader", "shaders_hlsl/test.shader.json"},
            {"texture", "textures/white.ppm"}, {"tint", {1.0, 0.5, 0.25, 1.0}},
            {"uvScale", {2.0, 3.0}}, {"blend", "opaque"}, {"depthTest", true}, {"depthWrite", true}};
}

void inheritanceAndReload(const Fixtures& fixture) {
    fixture.json("materials/base.json", base());
    fixture.json("materials/child.json", {{"base", "base.json"}, {"tint", {0.1, 0.2, 0.3, 0.4}},
                                          {"blend", "alpha"}, {"depthWrite", false}});
    fixture.json("materials/grandchild.json", {{"base", "materials/child.json"}, {"texture", "textures/other.ppm"}});
    MaterialLibrary library(fixture.root);
    const auto child = library.load("materials/child.json");
    const auto grandchild = library.load("assets/materials/grandchild.json");
    expect(child->ready && grandchild->ready, "Base/instance chain did not resolve");
    expect(child->definition.shader == "shaders_hlsl/test.shader.json" &&
           child->definition.texture == "textures/white.ppm" && child->definition.uvScale == glm::vec2(2, 3) &&
           child->definition.tint == glm::vec4(0.1F, 0.2F, 0.3F, 0.4F) &&
           child->definition.blend == MaterialBlend::Alpha && !child->definition.depthWrite,
           "Inherited or overridden material values were lost");
    expect(grandchild->definition.texture == "textures/other.ppm" &&
           grandchild->definition.tint == child->definition.tint, "Multi-level override precedence is incorrect");
    expect(library.load((fixture.root / "materials/child.json").string()) == child,
           "Equivalent material paths bypass the cache");
    auto changedBase = base();
    changedBase["uvScale"] = {7, 8};
    fixture.json("materials/base.json", changedBase);
    const auto revision = child->revision;
    library.poll(true);
    expect(child->revision > revision && child->definition.uvScale == glm::vec2(7, 8) &&
           grandchild->definition.uvScale == glm::vec2(7, 8), "Base hot reload did not reach dependent instances");
    expect(child->definition.tint == glm::vec4(0.1F, 0.2F, 0.3F, 0.4F), "Base reload erased child overrides");
    const auto goodRevision = child->revision;
    fixture.write("materials/base.json", "{broken");
    library.poll(true);
    expect(child->ready && child->revision == goodRevision && child->definition.uvScale == glm::vec2(7, 8) &&
           child->lastError.find("materials/base.json") != std::string::npos,
           "Broken reload replaced last-good data or lost the diagnostic file");
    changedBase["uvScale"] = {4, 5};
    fixture.json("materials/base.json", changedBase);
    library.poll(true);
    expect(child->ready && child->lastError.empty() && child->definition.uvScale == glm::vec2(4, 5),
           "Material did not recover after repairing its base");
}

void invalidDataAndRecovery(const Fixtures& fixture) {
    MaterialLibrary library(fixture.root);
    fixture.json("materials/cycle-a.json", {{"base", "cycle-b.json"}});
    fixture.json("materials/cycle-b.json", {{"base", "cycle-a.json"}});
    const auto cycle = library.load("materials/cycle-a.json");
    expect(!cycle->ready && cycle->lastError.find("cycle") != std::string::npos, "Inheritance cycle was accepted");
    auto invalid = base();
    invalid["tint"] = {1, 2};
    fixture.json("materials/invalid.json", invalid);
    expect(!library.load("materials/invalid.json")->ready, "Malformed vector was accepted");
    invalid = base();
    invalid["depthTest"] = "true";
    fixture.json("materials/invalid-bool.json", invalid);
    expect(!library.load("materials/invalid-bool.json")->ready, "Malformed state was accepted");
    invalid = base();
    invalid["texture"] = "textures/later.ppm";
    fixture.json("materials/missing.json", invalid);
    const auto missing = library.load("materials/missing.json");
    expect(!missing->ready && missing->lastError.find("textures/later.ppm") != std::string::npos,
           "Missing resource was silently accepted");
    fixture.write("textures/later.ppm", "P3\n1 1\n255\n1 2 3\n");
    library.poll(true);
    expect(missing->ready && missing->lastError.empty(), "New resource did not repair a failed material");
    fixture.write("materials/huge.json", std::string(256U * 1024U + 1U, ' '));
    expect(!library.load("materials/huge.json")->ready, "Oversized material was accepted");
    expect(!library.load("../outside.json")->ready && !library.load("assets")->ready,
           "Escaping or empty asset path was accepted");
    for (int index = 0; index < 17; ++index) {
        auto definition = index == 16 ? base() : Json{{"base", "depth-" + std::to_string(index + 1) + ".json"}};
        fixture.json("materials/depth-" + std::to_string(index) + ".json", definition);
    }
    expect(!library.load("materials/depth-0.json")->ready, "Unbounded base chain was accepted");
}

void atomicSaveAndIsolation(const Fixtures& fixture) {
    MaterialLibrary library(fixture.root);
    std::string error;
    expect(library.save("materials/saved.json", base(), &error) && error.empty(), "Valid material could not be saved");
    const auto saved = library.load("materials/saved.json");
    const auto before = fixture.read("materials/saved.json");
    auto invalid = base();
    invalid["blend"] = "typo";
    expect(!library.save("materials/saved.json", invalid, &error) && !error.empty() &&
           fixture.read("materials/saved.json") == before && saved->ready, "Invalid save damaged the material file");
    auto changed = base();
    changed["tint"] = {0.3, 0.6, 0.9, 1};
    expect(library.save("materials/saved.json", changed, &error) &&
           saved->definition.tint == glm::vec4(0.3F, 0.6F, 0.9F, 1), "Atomic save did not update cached live material");
    expect(!library.save("../outside.json", base(), &error), "Save escaped the asset root");
    const auto otherRoot = fixture.root / "second-project";
    std::filesystem::create_directory(otherRoot);
    library.setRoot(otherRoot);
    expect(!library.load("materials/saved.json")->ready && saved->ready,
           "Project switch reused the previous project's material cache");
    expect(!library.diagnostics().empty(), "Material failure diagnostics are empty");
}

void unsavedPreviewKeepsDiskAndLiveState(const Fixtures& fixture) {
    auto alternate = base();
    alternate["texture"] = "textures/other.ppm";
    alternate["tint"] = {0.25, 0.5, 0.75, 0.6};
    alternate["uvScale"] = {6, 7};
    alternate["blend"] = "alpha";
    alternate["depthWrite"] = false;
    fixture.json("materials/preview-a.json", base());
    fixture.json("materials/preview-b.json", alternate);
    const Json original = {{"base", "preview-a.json"}, {"tint", {0.8, 0.7, 0.6, 1}}};
    fixture.json("materials/preview-child.json", original);
    MaterialLibrary library(fixture.root);
    const MaterialLibrary& previews = library;
    const auto material = library.load("materials/preview-child.json");
    expect(material->ready, "Preview fixture did not resolve its initial material");
    const auto before = material->definition;
    const auto revision = material->revision;
    const auto childBytes = fixture.read("materials/preview-child.json");
    const auto baseBytes = fixture.read("materials/preview-a.json");
    const auto childTime = std::filesystem::last_write_time(fixture.root / "materials/preview-child.json");
    const auto diagnostics = library.diagnostics();
    MaterialDefinition result;
    std::string error = "previous failure";
    auto draft = original;
    draft["tint"] = {0.1, 0.2, 0.3, 0.4};
    expect(previews.preview(material->path, draft, result, &error) && error.empty() &&
           result.tint == glm::vec4(0.1F, 0.2F, 0.3F, 0.4F) && result.uvScale == glm::vec2(2, 3),
           "Unsaved preview did not resolve draft overrides");
    draft.erase("tint");
    expect(previews.preview(material->path, draft, result, &error) && result.tint == glm::vec4(1, 0.5F, 0.25F, 1),
           "Clearing an override did not immediately preview its inherited value");
    draft["base"] = "materials/preview-b.json";
    expect(previews.preview(material->path, draft, result, &error) &&
           result.tint == glm::vec4(0.25F, 0.5F, 0.75F, 0.6F) && result.uvScale == glm::vec2(6, 7) &&
           result.texture == "textures/other.ppm" && result.blend == MaterialBlend::Alpha && !result.depthWrite,
           "Changing an unsaved base did not refresh inherited fields");
    expect(previews.preview("preview-not-created/deep/unsaved.json", draft, result, &error) &&
           !std::filesystem::exists(fixture.root / "preview-not-created"),
           "Preview created the destination directory or required an existing file");
    const auto resolved = result;
    const auto unchanged = [&] {
        return result.shader == resolved.shader && result.texture == resolved.texture && result.tint == resolved.tint &&
               result.uvScale == resolved.uvScale && result.blend == resolved.blend &&
               result.depthTest == resolved.depthTest && result.depthWrite == resolved.depthWrite;
    };
    auto invalid = draft;
    invalid["texture"] = "textures/missing-preview.ppm";
    expect(!previews.preview(material->path, invalid, result, &error) && unchanged() &&
           error.find("missing-preview.ppm") != std::string::npos,
           "Invalid preview changed output or lost its resource diagnostic");
    invalid = {{"base", "preview-child.json"}};
    expect(!previews.preview(material->path, invalid, result, &error) && unchanged() &&
           error.find("cycle") != std::string::npos, "Unsaved self-inheritance was accepted");
    fixture.json("materials/preview-descendant.json", {{"base", "preview-child.json"}});
    invalid = {{"base", "preview-descendant.json"}};
    expect(!previews.preview(material->path, invalid, result, &error) && unchanged(),
           "Unsaved preview missed a cycle through an existing descendant");
    expect(!previews.preview("../preview-outside.json", draft, result, &error) && unchanged(),
           "Preview accepted an escaping destination path");
    invalid = draft;
    invalid["base"] = "../../preview-outside-base.json";
    expect(!previews.preview(material->path, invalid, result, &error) && unchanged(),
           "Preview accepted an escaping base reference");
    invalid = draft;
    invalid["name"] = std::string(256U * 1024U, 'x');
    expect(!previews.preview(material->path, invalid, result, &error) && unchanged(),
           "Preview bypassed the saved material size limit");
    for (int index = 0; index < 300; ++index)
        expect(previews.preview("preview-not-created/candidate-" + std::to_string(index) + ".json", draft, result),
               "Unsaved previews consumed the tracked material cache budget");
    expect(!std::filesystem::exists(fixture.root / "preview-not-created") &&
           fixture.read("materials/preview-child.json") == childBytes && fixture.read("materials/preview-a.json") == baseBytes &&
           std::filesystem::last_write_time(fixture.root / "materials/preview-child.json") == childTime,
           "Preview modified files or their timestamps");
    expect(library.load(material->path) == material && material->revision == revision &&
           material->definition.tint == before.tint && material->definition.texture == before.texture &&
           material->definition.uvScale == before.uvScale && material->lastError.empty() && library.diagnostics() == diagnostics,
           "Preview polluted cached material state or runtime diagnostics");
}

void blankProjectBuiltinShader(const Fixtures& fixture) {
    fixture.write("shaders_hlsl/textured.shader.json", "{}");
    const auto project = fixture.root / "blank-project";
    std::filesystem::create_directory(project);
    MaterialLibrary library(project);
    library.setBuiltinShaderRoot(fixture.root);
    std::string error;
    expect(library.save("materials/new.json", {{"shader", "shaders_hlsl/textured.shader.json"}}, &error),
           "Blank project could not use the sanctioned built-in lit shader");
    const auto material = library.load("materials/new.json");
    expect(material->ready && material->definition.shader == "shaders_hlsl/textured.shader.json",
           "Built-in shader resolution changed the portable material reference");
    const auto fallbackRevision = material->revision;
    fixture.write("blank-project/shaders_hlsl/textured.shader.json", "{}");
    library.poll(true);
    expect(material->revision > fallbackRevision && material->definition.shader == "shaders_hlsl/textured.shader.json",
           "Adding a project shader override did not invalidate the fallback binding");
    expect(!library.save("materials/other.json", {{"shader", "shaders_hlsl/missing.shader.json"}}, &error),
           "Built-in fallback accepted an arbitrary missing shader");
}

void graphInstancesAndReload(const Fixtures& fixture) {
    const Json graph = {
        {"nodes", Json::array({{{"id", 1}, {"type", "texture"}, {"texture", "assets/textures/white.ppm"}}})},
        {"outputs", {{"baseColor", 1}}}
    };
    const Json base = {{"version", 2}, {"graph", graph}};
    const Json child = {{"base", "materials/graph-base.json"}, {"graphTextures", {{"1", "textures/other.ppm"}}}};
    fixture.json("materials/graph-base.json", base);
    fixture.json("materials/graph-child.json", child);
    MaterialLibrary library(fixture.root);
    const auto parent = library.load("materials/graph-base.json");
    const auto instance = library.load("materials/graph-child.json");
    expect(parent->ready && instance->ready && parent->definition.graphProgram && instance->definition.graphProgram,
           "Material graph or its instance failed to resolve");
    expect(parent->definition.graph["nodes"][0]["texture"] == "textures/white.ppm" &&
           instance->definition.graph["nodes"][0]["texture"] == "textures/other.ppm" &&
           instance->definition.graphProgram->textures == std::vector<std::string>{"textures/other.ppm"} &&
           instance->definition.graphProgram->pixelSource == parent->definition.graphProgram->pixelSource,
           "Graph texture override did not preserve topology and shader sharing");

    auto draft = child;
    draft["graphTextures"]["1"] = "";
    MaterialDefinition preview;
    std::string error;
    expect(library.preview(instance->path, draft, preview, &error) && preview.graphProgram &&
           preview.graphProgram->textures == std::vector<std::string>{""} &&
           preview.graphProgram->pixelSource == instance->definition.graphProgram->pixelSource,
           "Graph preview did not use the white texture without changing the shader");
    const auto revision = instance->revision;
    draft["graphTextures"]["1"] = "../outside.ppm";
    expect(!library.save(instance->path, draft, &error) && instance->revision == revision &&
           fixture.read(instance->path) == child.dump(), "Invalid graph texture override changed disk or live state");
    draft["graphTextures"] = {{"3", "textures/white.ppm"}};
    expect(!library.preview(instance->path, draft, preview, &error), "A missing texture node accepted an override");

    auto edited = base;
    edited["graph"]["nodes"][0]["position"] = {150, 80};
    fixture.json(parent->path, edited);
    library.poll(true);
    expect(instance->revision > revision && instance->definition.graph["nodes"][0]["position"] == Json::array({150, 80}) &&
           instance->definition.graphProgram->textures == std::vector<std::string>{"textures/other.ppm"},
           "Inherited graph edit lost the instance texture override");
    const auto goodRevision = instance->revision;
    edited["graph"]["outputs"]["baseColor"] = 42;
    fixture.json(parent->path, edited);
    library.poll(true);
    expect(instance->ready && instance->revision == goodRevision && !instance->lastError.empty(),
           "Broken graph reload replaced the last working graph");
}
}

int main() {
    try {
        const Fixtures fixture;
        inheritanceAndReload(fixture);
        invalidDataAndRecovery(fixture);
        atomicSaveAndIsolation(fixture);
        unsavedPreviewKeepsDiskAndLiveState(fixture);
        blankProjectBuiltinShader(fixture);
        graphInstancesAndReload(fixture);
        std::cout << "Material data, instances, reload, validation and atomic save tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Material tests failed: " << error.what() << '\n';
        return 1;
    }
}
