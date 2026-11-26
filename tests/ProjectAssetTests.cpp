#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "engine/renderer/Renderer.h"
#include "engine/renderer/RenderAdapter.h"
#include "engine/resources/loaders.h"

namespace {
std::string utf8Path(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}
void expect(const bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
class Fixtures final {
public:
    Fixtures() {
        root = std::filesystem::temp_directory_path() /
            ("tge-project-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        first = root / "first" / "assets";
        second = root / "second" / "assets";
        std::filesystem::create_directories(first);
        std::filesystem::create_directories(second);
    }
    ~Fixtures() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    static std::string write(const std::filesystem::path& path, const std::string& contents) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << contents;
        expect(static_cast<bool>(file), "Cannot write isolated project fixture");
        return utf8Path(std::filesystem::weakly_canonical(path));
    }
    std::filesystem::path root, first, second;
};

class UiTestImage final : public engine::rhi::IImage {
public:
    std::uint32_t width() const override { return 1; }
    std::uint32_t height() const override { return 1; }
    engine::rhi::ResourceHandle handle() const override { return 123; }
    void* uiShaderResourceView() const override { return reinterpret_cast<void*>(456); }
};

void testUiImageRouting(const Fixtures& fixtures) {
    engine::renderer::Renderer renderer;
    const auto first = Fixtures::write(fixtures.first / "textures/ui.ppm", "P3\n1 1\n255\n32 64 128\n");
    const auto second = Fixtures::write(fixtures.second / "textures/ui.ppm", "P3\n1 1\n255\n255 64 128\n");
    Fixtures::write(fixtures.first / "ui_images.json", R"({"rbxassetid://42":"textures/ui.ppm"})");
    Fixtures::write(fixtures.second / "ui_images.json", R"({"rbxassetid://42":"textures/ui.ppm"})");
    Fixtures::write(fixtures.first / "textures/oversized.ppm", "P3\n8192 8192\n255\n0 0 0\n");
    int loads = 0;
    std::string loadedPath;
    renderer.resourceManager().registerLoader<engine::resources::Texture>([&](const std::string& path) {
        ++loads; loadedPath = path;
        auto texture = std::make_shared<engine::resources::Texture>();
        texture->path = path; texture->image = std::make_unique<UiTestImage>();
        texture->setLoadState(engine::resources::ResourceLoadState::Loaded);
        return texture;
    });
    renderer.setProjectAssetRoot(fixtures.first);
    const auto a = renderer.resolveUiImage("rbxassetid://42");
    expect(a.textureId == reinterpret_cast<void*>(456) && a.width == 1 && a.height == 1 && loadedPath == first,
           "UI image did not expose the borrowed SRV through its local alias");
    expect(renderer.resolveUiImage("assets/textures/ui.ppm").textureId == a.textureId && loads == 1,
           "UI alias and local path did not share a retained resource");
    const auto previousStamp = std::filesystem::last_write_time(std::filesystem::u8path(first));
    Fixtures::write(std::filesystem::u8path(first), "P3\n1 1\n255\n99 22 11\n");
    std::filesystem::last_write_time(std::filesystem::u8path(first), previousStamp + std::chrono::seconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    expect(renderer.resolveUiImage("rbxassetid://42").textureId == a.textureId && loads == 2 &&
               renderer.resolveUiImage("rbxassetid://42").textureId == a.textureId,
           "UI same-path image change did not reload while retaining the last good image");
    expect(renderer.resolveUiImage("../outside.png").textureId == nullptr &&
               renderer.resolveUiImage("https://example.com/icon.png").textureId == nullptr &&
               renderer.resolveUiImage("textures/oversized.ppm").textureId == nullptr && loads == 2,
           "UI images escaped assets, used the network, or decoded an oversized header");
    renderer.setProjectAssetRoot(fixtures.second);
    expect(renderer.resolveUiImage("rbxassetid://42").textureId && loadedPath == second && loads == 3,
           "UI image cache/aliases survived a project switch");
    std::int32_t width = 0, height = 0;
    expect(!engine::resources::inspectTextureRgba8(
               utf8Path(fixtures.first / "textures/oversized.ppm"), width, height),
           "Texture preflight allowed a 256 MiB decoded image allocation");
}

void testProjectNamespaces(const Fixtures& fixtures) {
    engine::renderer::Renderer renderer;
    const auto workingDirectory = std::filesystem::current_path();
    const auto engineLogo = renderer.resolveResourcePath("assets/textures/DGLogo.png");
    const auto engineFloor = renderer.resolveResourcePath("assets/textures/arena_floor.ppm");
    const auto firstTexture = Fixtures::write(fixtures.first / "textures/shared.ppm", "first project");
    const auto secondTexture = Fixtures::write(fixtures.second / "textures/shared.ppm", "second project");
    const auto projectFloor = Fixtures::write(fixtures.first / "textures/arena_floor.ppm", "project override");
    int loads = 0;
    renderer.resourceManager().registerLoader<engine::resources::Texture>([&](const std::string& path) {
        ++loads;
        auto texture = std::make_shared<engine::resources::Texture>();
        texture->path = path;
        return texture;
    });
    auto fallback = std::make_shared<std::string>("registered fallback");
    renderer.resourceManager().registerFallback<std::string>(fallback);
    renderer.setProjectAssetRoot(fixtures.first);
    expect(renderer.resolveResourcePath("assets/textures/shared.ppm") == firstTexture,
           "Logical asset does not resolve beneath first project");
    expect(renderer.resolveResourcePath("textures/shared.ppm") == firstTexture,
           "Assets prefix produces a second cache identity");
    expect(renderer.resolveResourcePath("assets/textures/arena_floor.ppm") == projectFloor,
           "Existing project texture does not override the built-in fallback");
    expect(renderer.resolveResourcePath("assets/textures/DGLogo.png") == engineLogo,
           "Missing known built-in texture does not use engine assets");
    expect(renderer.resolveResourcePath("assets/textures/sandstone.ppm") ==
               std::filesystem::weakly_canonical(fixtures.first / "textures/sandstone.ppm").generic_string(),
           "Missing custom texture fell back to legacy source assets");
    expect(renderer.resolveResourcePath("assets/models/plane.obj") ==
               std::filesystem::weakly_canonical(fixtures.first / "models/plane.obj").generic_string(),
           "Missing custom mesh fell back to legacy source assets");
    expect(renderer.resolveResourcePath("assets/scripts/demo.luau") ==
               std::filesystem::weakly_canonical(fixtures.first / "scripts/demo.luau").generic_string(),
           "Project script resolution escaped to engine assets");
    expect(renderer.resolveResourcePath("__diligent_cube__") == "__diligent_cube__" &&
               renderer.resolveResourcePath("builtin:cube") == "builtin:cube",
           "Procedural mesh id was interpreted as a file path");

    engine::renderer::RenderAdapter adapter(renderer);
    const auto first = adapter.loadTexture("assets/textures/shared.ppm");
    expect(first && first->path == firstTexture && loads == 1, "Renderer did not route the loader's concrete path");
    expect(adapter.loadTexture("textures/shared.ppm") == first && loads == 1,
           "Equivalent ids missed the canonical resource cache");
    renderer.setProjectAssetRoot(fixtures.second);
    const auto second = adapter.loadTexture("assets/textures/shared.ppm");
    expect(second && second != first && second->path == secondTexture && loads == 2,
           "Project switch reused a resource cached by the first project's relative id");
    expect(renderer.resourceManager().load<std::string>("no-loader") == fallback,
           "Switch removed fallback registration");
    expect(renderer.resolveResourcePath("assets/textures/arena_floor.ppm") == engineFloor,
           "Project switch retained the previous project's built-in override");

    bool rejected = false;
    try { (void)renderer.resolveResourcePath("../escape.ppm"); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "Parent traversal escaped the selected project's assets");
    rejected = false;
    try { (void)renderer.resolveResourcePath(firstTexture); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "Absolute file from an inactive project was accepted");
    renderer.setProjectAssetRoot({});
    expect(renderer.resolveResourcePath("assets/textures/DGLogo.png") == engineLogo,
           "Empty project root did not restore engine asset resolution");
    expect(std::filesystem::current_path() == workingDirectory, "Project routing changed the process working directory");
}

void testShaderManifestPaths(const Fixtures& fixtures) {
    using engine::resources::ShaderProgramSource;
    const auto vertex = Fixtures::write(fixtures.first / "shaders/custom/effect.vert.hlsl", "void main() {}\n");
    const auto fragment = Fixtures::write(fixtures.first / "shaders/effect.frag.hlsl", "void main() {}\n");
    const auto manifest = Fixtures::write(fixtures.first / "shaders/custom/effect.shader.json",
        R"({"vertex":"effect.vert.hlsl","fragment":"../effect.frag.hlsl"})");
    ShaderProgramSource source;
    std::string error;
    expect(engine::resources::loadShaderProgramSource(manifest, source, &error, fixtures.first),
           "Cannot parse project shader manifest");
    expect(source.descriptorIsFile && source.descriptorPath == manifest && source.vertexPath == vertex &&
               source.fragmentPath == fragment, "Shader paths were resolved against cwd instead of the manifest directory");
    const auto prefixed = Fixtures::write(fixtures.first / "shaders/custom/prefixed.shader.json",
        R"({"vertex":"assets/shaders/custom/effect.vert.hlsl","fragment":"assets/shaders/effect.frag.hlsl"})");
    expect(engine::resources::loadShaderProgramSource(prefixed, source, &error, fixtures.first) &&
               source.vertexPath == vertex && source.fragmentPath == fragment,
           "Assets-prefixed manifest sources were resolved against the shader directory");
    const auto pair = vertex + '|' + fragment;
    expect(engine::resources::loadShaderProgramSource(pair, source, &error, fixtures.first) &&
               !source.descriptorIsFile && source.vertexPath == vertex && source.fragmentPath == fragment,
           "Absolute vertex/fragment descriptor pair regressed");
    engine::renderer::Renderer renderer;
    renderer.setProjectAssetRoot(fixtures.first);
    expect(renderer.resolveResourcePath("assets/shaders/custom/effect.vert.hlsl|assets/shaders/effect.frag.hlsl") == pair,
           "Descriptor pair did not receive concrete project cache paths");
}

void testCyrillicFiles(const Fixtures& fixtures) {
    const auto assets = fixtures.root / std::filesystem::path(u8"Проект_Луа") / "assets";
    const auto texture = Fixtures::write(assets / "textures" / std::filesystem::path(u8"цвет.ppm"),
        "P3\n1 1\n255\n17 23 31\n");
    engine::renderer::Renderer renderer;
    const auto engineLogo = renderer.resolveResourcePath("assets/textures/DGLogo.png");
    renderer.setProjectAssetRoot(assets);
    expect(renderer.resolveResourcePath(utf8Path(std::filesystem::path(u8"assets/textures/цвет.ppm"))) == texture,
           "Cyrillic resource id was converted through the Windows ANSI code page");
    engine::resources::TextureData pixels;
    expect(engine::resources::loadTextureDataRgba8(texture, pixels) && pixels.pixels.size() == 4 &&
               pixels.pixels[0] == 17 && pixels.pixels[1] == 23 && pixels.pixels[2] == 31,
           "Cannot decode PPM from Cyrillic project/file path");
    const auto png = assets / "textures" / std::filesystem::path(u8"логотип.png");
    std::filesystem::copy_file(std::filesystem::u8path(engineLogo), png);
    expect(engine::resources::loadTextureDataRgba8(utf8Path(png), pixels) && pixels.width > 0,
           "stb_image cannot decode PNG from a Cyrillic project/file path");
    const auto mesh = Fixtures::write(assets / "models" / std::filesystem::path(u8"треугольник.obj"),
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    engine::resources::MeshData geometry;
    expect(engine::resources::loadMeshData(mesh, geometry) && geometry.indices.size() == 3,
           "Assimp cannot open UTF-8 mesh paths on Windows");
    const auto vertexName = std::filesystem::path(u8"вершина.hlsl");
    const auto fragmentName = std::filesystem::path(u8"пиксель.hlsl");
    const auto vertex = Fixtures::write(assets / "shaders" / vertexName, "void main() {}\n");
    const auto fragment = Fixtures::write(assets / "shaders" / fragmentName, "void main() {}\n");
    const auto manifest = Fixtures::write(assets / "shaders" / std::filesystem::path(u8"материал.shader.json"),
        "{\"vertex\":\"" + utf8Path(vertexName) + "\",\"fragment\":\"" + utf8Path(fragmentName) + "\"}");
    engine::resources::ShaderProgramSource source;
    expect(engine::resources::loadShaderProgramSource(manifest, source, nullptr, assets) &&
               source.vertexPath == vertex && source.fragmentPath == fragment,
           "Shader manifest lost UTF-8 directory or source filenames");
}
}

int main() {
    try {
        const Fixtures fixtures;
        testProjectNamespaces(fixtures);
        testUiImageRouting(fixtures);
        testShaderManifestPaths(fixtures);
        testCyrillicFiles(fixtures);
        std::cout << "Project asset tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Project asset tests failed: " << error.what() << '\n';
        return 1;
    }
}
