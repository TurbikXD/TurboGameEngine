#pragma once

#include <glm/glm.hpp>
#include <vector>
#include "engine/ecs/entity.h"

namespace engine::core { class JobSystem; }

namespace engine::renderer {
class RenderAdapter;
}

namespace engine::ecs {

class World;
struct MeshRenderer;

class RenderSystem final {
public:
    void setJobSystem(core::JobSystem* jobs) { m_jobs = jobs; }
    void render(World& world, renderer::RenderAdapter& renderer, const glm::mat4& viewProjectionMatrix);
private:
    core::JobSystem* m_jobs{nullptr};
    std::vector<EntityId> m_entities;
    std::vector<MeshRenderer*> m_renderers;
    std::vector<glm::mat4> m_matrices;
};

} // namespace engine::ecs
