#pragma once

#include <cstdint>

namespace engine::resources {

enum class ResourceLoadState : std::uint8_t {
    Unloaded = 0,
    Loading,
    Loaded,
    Failed,
    Reloading
};

} // namespace engine::resources
