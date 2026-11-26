#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <atomic>

#include <glm/glm.hpp>

#include "engine/rhi/Resources.h"
#include "engine/resources/resource_state.h"

namespace engine::resources {

struct MeshVertex final {
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::vec3 normal{0.0F, 0.0F, 1.0F};
    glm::vec2 uv{0.0F, 0.0F};
};

struct MeshData final {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

class Mesh final {
public:
    std::string path;
    MeshData data;
    std::unique_ptr<rhi::IBuffer> vertexBuffer;
    std::unique_ptr<rhi::IBuffer> indexBuffer;
    std::uint32_t vertexCount{0};
    std::uint32_t indexCount{0};
    std::string lastError;
    glm::vec3 boundsMinimum{0.0F}, boundsMaximum{0.0F};
    bool hasBounds{false};

    [[nodiscard]] ResourceLoadState loadState() const {
        return m_loadState.load(std::memory_order_relaxed);
    }

    void setLoadState(const ResourceLoadState state) {
        if (state == ResourceLoadState::Loaded && !data.vertices.empty()) {
            boundsMinimum = boundsMaximum = data.vertices.front().position;
            for (const auto& vertex : data.vertices) {
                boundsMinimum = glm::min(boundsMinimum, vertex.position);
                boundsMaximum = glm::max(boundsMaximum, vertex.position);
            }
            hasBounds = true;
        }
        m_loadState.store(state, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isReady() const {
        if (loadState() != ResourceLoadState::Loaded) {
            return false;
        }
        if (!vertexBuffer) {
            return false;
        }
        if (!data.indices.empty() && !indexBuffer) {
            return false;
        }
        return true;
    }

private:
    std::atomic<ResourceLoadState> m_loadState{ResourceLoadState::Unloaded};
};

}
