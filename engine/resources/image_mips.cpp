#include "engine/resources/image_mips.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "engine/core/Profiling.h"

namespace engine::resources {

std::vector<ImageMip> buildRgba8MipTail(std::uint32_t width, std::uint32_t height,
                                     std::span<const std::uint8_t> pixels) {
    ENGINE_PROFILE_ZONE("Upload Build CPU Mips");
    if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height >
            std::numeric_limits<std::size_t>::max() / 4U ||
        pixels.size() != static_cast<std::size_t>(width) * height * 4U) {
        throw std::invalid_argument("Invalid RGBA8 mip source");
    }
    std::vector<ImageMip> levels;
    // Reserve first: spans into earlier levels must not be invalidated.
    levels.reserve(32);
    while (width > 1 || height > 1) {
        ImageMip mip;
        mip.width = std::max(1U, width / 2U);
        mip.height = std::max(1U, height / 2U);
        mip.pixels.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
        for (std::uint32_t y = 0; y < mip.height; ++y) {
            const auto firstY = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * height / mip.height);
            const auto endY = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1U) * height / mip.height);
            for (std::uint32_t x = 0; x < mip.width; ++x) {
                const auto firstX = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * width / mip.width);
                const auto endX = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1U) * width / mip.width);
                const auto samples = (endX - firstX) * (endY - firstY);
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    std::uint32_t sum = 0;
                    for (auto sy = firstY; sy < endY; ++sy) {
                        for (auto sx = firstX; sx < endX; ++sx) {
                            sum += pixels[(static_cast<std::size_t>(sy) * width + sx) * 4U + channel];
                        }
                    }
                    mip.pixels[(static_cast<std::size_t>(y) * mip.width + x) * 4U + channel] =
                        static_cast<std::uint8_t>((sum + samples / 2U) / samples);
                }
            }
        }
        levels.push_back(std::move(mip));
        width = levels.back().width;
        height = levels.back().height;
        pixels = levels.back().pixels;
    }
    return levels;
}

} // namespace engine::resources
