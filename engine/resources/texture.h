#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <atomic>

#include "engine/rhi/Resources.h"
#include "engine/resources/resource_state.h"

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
    std::string lastError;

    [[nodiscard]] ResourceLoadState loadState() const {
        return m_loadState.load(std::memory_order_relaxed);
    }

    void setLoadState(const ResourceLoadState state) {
        m_loadState.store(state, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isReady() const {
        return loadState() == ResourceLoadState::Loaded && image != nullptr;
    }

private:
    std::atomic<ResourceLoadState> m_loadState{ResourceLoadState::Unloaded};
};

} // namespace engine::resources
