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
    std::string write(const std::string_view contents) const {
        const auto path = directory / "fixture.image"; // Detection must not depend on extension.
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        expect(static_cast<bool>(output), "Cannot write texture test fixture");
        return path.string();
    }
    std::filesystem::path directory;
};

void testValidImages(const Fixtures& fixtures) {
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

    TextureData png;
    expect(engine::resources::loadTextureDataRgba8(
               std::string{ENGINE_SOURCE_ROOT} + "/assets/textures/DGLogo.png", png),
           "Existing PNG loader regressed");
    expect(png.width > 0 && png.height > 0 && png.channels == 4 &&
           png.pixels.size() == static_cast<std::size_t>(png.width) * png.height * 4U,
           "PNG RGBA8 dimensions differ");
}

void testInvalidImages(const Fixtures& fixtures) {
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
        std::string_view{"P3\n1 1 255\n0 0 0 1"}
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
} // namespace

int main() {
    try {
        const Fixtures fixtures;
        testValidImages(fixtures);
        testInvalidImages(fixtures);
        testSceneTexturesOnJobs();
        std::cout << "Texture loaders: P3/P6/PNG, comments, scaling, malformed input and 30 scene jobs passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Texture loader failure: " << error.what() << '\n';
        return 1;
    }
}
