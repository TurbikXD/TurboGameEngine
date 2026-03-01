#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/rhi/Resources.h"

namespace engine::resources {

struct TextureData final {
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t channels{4};
    std::vector<std::uint8_t> pixels;
};

class Texture final {
public:
    std::string path;
    TextureData data;
    std::unique_ptr<rhi::IImage> image;

    [[nodiscard]] bool isReady() const {
        return image != nullptr;
    }
};

} // namespace engine::resources
