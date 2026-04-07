#pragma once

#include <glm/glm.hpp>

namespace engine::renderer {
class RenderAdapter;
}

namespace engine::ecs {

class World;

class DebugRenderSystem final {
public:
    void render(World& world, renderer::RenderAdapter& renderer, const glm::mat4& viewProjectionMatrix);
};

} // namespace engine::ecs
