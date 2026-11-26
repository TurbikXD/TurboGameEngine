#pragma once

#include <span>
#include <glm/mat4x4.hpp>
#include "engine/ecs/entity.h"

namespace engine::core { class JobSystem; }
namespace engine::ecs {
class World;

void prepareWorldMatrices(const World& world, std::span<const EntityId> entities,
                          std::span<glm::mat4> matrices, core::JobSystem* jobs, float renderFraction = 1.0F);
}
