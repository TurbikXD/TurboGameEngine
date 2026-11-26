#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/JobSystem.h"
#include "engine/resources/loaders.h"

namespace {
void expect(const bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

class Fixtures final {
public:
    Fixtures() {
        directory = std::filesystem::temp_directory_path() /
            ("tge-texture-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        expect(std::filesystem::create_directory(directory), "Cannot create isolated texture test directory");
    }
    ~Fixtures() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::string write(const std::string_view contents, const std::string_view name="fixture.image") const {
        const auto path = directory / std::filesystem::u8path(name.begin(),name.end());
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        expect(static_cast<bool>(output), "Cannot write texture test fixture");
        const auto utf8=path.generic_u8string();
        return {utf8.begin(),utf8.end()};
    }
    std::filesystem::path directory;
};

void testValidImages(const Fixtures& fixtures) {
    using namespace std::string_view_literals;
    using engine::resources::TextureData;
    const auto check = [&](const std::string_view contents, const int width,
                           const std::vector<std::uint8_t>& expected) {
        TextureData texture;
        std::string error = "stale error";
        expect(engine::resources::loadTextureDataRgba8(fixtures.write(contents), texture, &error),
               "Valid texture failed to decode");
        expect(texture.width == width && texture.height == 1 && texture.channels == 4 &&
               texture.pixels == expected && error.empty(), "Decoded RGBA8 pixels/header differ");
    };
    check("P3\r\n# header comment\r\n2# width\r\n1\t255\n255 0 1 # pixel comment\n0 128 255\n# end",
          2, {255, 0, 1, 255, 0, 128, 255, 255});
    check("P3\n1 1\n100\n0 50 100", 1, {0, 128, 255, 255});
    check("P3\n1 1\n65535\n0 32768 65535\n", 1, {0, 128, 255, 255});
    check("P3\n01 01\n0255\n001\t002\r\n003", 1, {1, 2, 3, 255});
    constexpr char binaryPpm[] = "P6\n2 1\n255\n\x00\x80\xff\xff\x01\x00";
    check(std::string_view{binaryPpm, sizeof(binaryPpm) - 1U}, 2,
          {0, 128, 255, 255, 255, 1, 0, 255});
    check("P6\n1 1\n100\n\x00\x32\x64"sv,1,{0,128,255,255});
    check("P6\n1 1\n65535\n\x00\x00\x80\x00\xff\xff"sv,1,{0,128,255,255});
    check("P6\r\n1 1\r\n255\r\n\x0a\x20\x23"sv,1,{10,32,35,255});
    check("P6\n1 1\n255\r\x0a\x20\x23"sv,1,{10,32,35,255});
    check("P6\n1 1\n65535\r\x0a\xff\x80\x00\xff\xff"sv,1,{11,128,255,255});
    check("P5\n1 1\n15\n\x07"sv,1,{119,119,119,255});
    check("P5\n1 1\n65535\n\x80\x00"sv,1,{128,128,128,255});
    check("P5\n1 1\n255\r\x0a"sv,1,{10,10,10,255});
    check("P5\n1 1\n65535\r\x0a\xff"sv,1,{11,11,11,255});
    check("P5\r\n1 1\r\n65535\r\n\x0a\xff"sv,1,{11,11,11,255});
    TextureData unicode;
    expect(engine::resources::loadTextureDataRgba8(fixtures.write("P6\n1 1\n100\n\x00\x32\x64"sv,"цветной.ppm"),unicode) &&
        unicode.pixels==std::vector<std::uint8_t>{0,128,255,255},"Binary PNM failed under a Unicode filename");

    TextureData png;
    expect(engine::resources::loadTextureDataRgba8(
               std::string{ENGINE_SOURCE_ROOT} + "/assets/textures/DGLogo.png", png),
           "Existing PNG loader regressed");
    expect(png.width > 0 && png.height > 0 && png.channels == 4 &&
           png.pixels.size() == static_cast<std::size_t>(png.width) * png.height * 4U,
           "PNG RGBA8 dimensions differ");
}

void testInvalidImages(const Fixtures& fixtures) {
    using namespace std::string_view_literals;
    constexpr std::array invalid{
        std::string_view{"P3x\n1 1 255\n0 0 0"},
        std::string_view{"P3\n"},
        std::string_view{"P3\n0 1 255\n0 0 0"},
        std::string_view{"P3\n-1 1 255\n0 0 0"},
        std::string_view{"P3\n1 1 0\n0 0 0"},
        std::string_view{"P3\n1 1 65536\n0 0 0"},
        std::string_view{"P3\n4294967296 1 255\n0 0 0"},
        std::string_view{"P3\n16777217 1 255\n0 0 0"},
        std::string_view{"P3\n16777216 16777216 255\n0 0 0"},
        std::string_view{"P3\n100000 1000 255\n0 0 0"},
        std::string_view{"P3\n2 1 255\n0 0 0"},
        std::string_view{"P3\n1 1 255\n0 0"},
        std::string_view{"P3\n1 1 255\n0 -1 0"},
        std::string_view{"P3\n1 1 255\n0 256 0"},
        std::string_view{"P3\n1 1 255\n0 4294967296 0"},
        std::string_view{"P3\n1 1 255\n0 1x 0"},
        std::string_view{"P3\n1 1 255\n0 0 0 1"},
        "P6\n1 1\n0\n\x00\x00\x00"sv,
        "P6\n1 1\n100\n\x00\x65\x00"sv,
        "P6\n1 1\n65535\n\x00\x00\x80\x00\xff"sv,
        "P6\n1 1\n1000\n\x03\xe9\x00\x00\x00\x00"sv,
        "P5\n1 1\n15\n\x10"sv,
        "P5\n1 1\n65535\n\x80"sv
    };
    for (const auto contents : invalid) {
        engine::resources::TextureData texture;
        texture.width = 7;
        texture.height = 9;
        texture.pixels = {1, 2, 3, 4};
        std::string error;
        expect(!engine::resources::loadTextureDataRgba8(fixtures.write(contents), texture, &error),
               "Malformed P3 image was accepted");
        expect(!error.empty(), "Malformed P3 image has no diagnostic");
        expect(texture.width == 7 && texture.height == 9 && texture.pixels == std::vector<std::uint8_t>{1, 2, 3, 4},
               "Failed P3 decoding modified the destination");
    }
    engine::resources::TextureData missing;
    std::string error;
    expect(!engine::resources::loadTextureDataRgba8((fixtures.directory / "missing.png").string(), missing, &error) &&
           !error.empty(), "Missing texture has no failure diagnostic");
}

void testMeshAndShaderLoaderTransactions(const Fixtures& fixtures) {
    engine::resources::MeshData mesh;
    mesh.vertices.resize(1);
    mesh.vertices[0].position={9,8,7};
    mesh.indices={0,0,0};
    const auto lines=fixtures.write("v 0 0 0\nv 1 0 0\nl 1 2\n","lines.obj");
    std::string error;
    expect(!engine::resources::loadMeshData(lines,mesh,&error) && !error.empty(),"Line-only OBJ was accepted as triangle geometry");
    expect(mesh.vertices.size()==1 && mesh.vertices[0].position==glm::vec3(9,8,7) &&
        mesh.indices==std::vector<std::uint32_t>{0,0,0},"Failed mesh decoding replaced the previous destination geometry");
    const auto triangle=fixtures.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n","triangle.obj");
    expect(engine::resources::loadMeshData(triangle,mesh,&error) && mesh.vertices.size()==3 && mesh.indices.size()==3 && error.empty(),
        "Valid triangle failed to replace geometry or retained a stale mesh error");
    engine::resources::ShaderProgramSource source;
    const auto descriptor=fixtures.write(R"({"vertex":"basic.vert.hlsl","fragment":"basic.frag.hlsl"})","valid.shader.json");
    error="stale shader error";
    expect(engine::resources::loadShaderProgramSource(descriptor,source,&error) && error.empty() && source.descriptorIsFile,
        "Successful shader descriptor load retained a stale diagnostic");
}

void testSceneTexturesOnJobs() {
    constexpr std::array names{"arena_floor.ppm", "arena_wall.ppm", "sandstone.ppm"};
    constexpr std::array<std::array<std::uint8_t, 3>, 3> firstPixels{{
        {118, 124, 136}, {98, 106, 120}, {208, 188, 152}
    }};
    engine::core::JobSystem jobs(true, 4U);
    std::vector<engine::core::JobSystem::Handle> handles;
    for (std::size_t run = 0; run < 30U; ++run) {
        handles.push_back(jobs.dispatch([run, &names, &firstPixels]() {
            const auto index = run % names.size();
            engine::resources::TextureData texture;
            expect(engine::resources::loadTextureDataRgba8(
                       std::string{ENGINE_SOURCE_ROOT} + "/assets/textures/" + names[index], texture),
                   "Real scene P3 texture failed on JobSystem");
            expect(texture.width == 8 && texture.height == 8 && texture.channels == 4 &&
                   texture.pixels.size() == 8U * 8U * 4U, "Scene PPM dimensions differ");
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                expect(texture.pixels[channel] == firstPixels[index][channel], "Scene PPM RGB value differs");
            }
            for (std::size_t pixel = 0; pixel < 64U; ++pixel) {
                expect(texture.pixels[pixel * 4U + 3U] == 255U, "Scene PPM alpha is not opaque");
            }
        }));
    }
    for (const auto& handle : handles) { jobs.wait(handle); }
}
}

int main() {
    try {
        const Fixtures fixtures;
        testValidImages(fixtures);
        testInvalidImages(fixtures);
        testMeshAndShaderLoaderTransactions(fixtures);
        testSceneTexturesOnJobs();
        std::cout << "Texture/model/shader loaders: P3/P5/P6/PNG, sample scaling, binary endian/CRLF, failed-load preservation and 30 scene jobs passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Texture loader failure: " << error.what() << '\n';
        return 1;
    }
}
