#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace engine::resources {
struct ImageMip final {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> pixels;
};

// RGBA8 UNORM (not sRGB), like the current renderer. Returns levels 1..last;
// level 0 stays in TextureData. Transfer queues cannot run GenerateMips().
std::vector<ImageMip> buildRgba8MipTail(std::uint32_t width, std::uint32_t height,
                                     std::span<const std::uint8_t> pixels);
} // namespace engine::resources
