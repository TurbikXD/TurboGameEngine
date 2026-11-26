#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include "engine/ecs/entity.h"

namespace engine::core { class JobSystem; }
namespace engine::resources { struct Material; }

namespace engine::renderer {
class RenderAdapter;
}

namespace engine::ecs {

class World;
struct MeshRenderer;

class RenderSystem final {
public:
    void setJobSystem(core::JobSystem* jobs) { m_jobs = jobs; }
    void render(World& world, renderer::RenderAdapter& renderer, const glm::mat4& viewProjectionMatrix,
        EntityId previewEntity = kInvalidEntity, const std::shared_ptr<resources::Material>& preview = {},
        float renderFraction = 1.0F);
    [[nodiscard]] std::size_t submittedCount() const { return m_submitted; }
    [[nodiscard]] std::size_t culledCount() const { return m_culled; }
private:
    std::size_t m_submitted{}, m_culled{};
    core::JobSystem* m_jobs{nullptr};
    std::vector<EntityId> m_entities;
    std::vector<MeshRenderer*> m_renderers;
    std::vector<glm::mat4> m_matrices;
    std::vector<std::size_t> m_order;
};

}
