#pragma once

#include <span>
#include <glm/mat4x4.hpp>
#include "engine/ecs/entity.h"

namespace engine::core { class JobSystem; }
namespace engine::ecs {
class World;

// Read-only phase: no component/hierarchy changes until this function returns.
// Each range owns disjoint output matrices. GPU submission follows on the caller.
void prepareWorldMatrices(const World& world, std::span<const EntityId> entities,
                          std::span<glm::mat4> matrices, core::JobSystem* jobs);
}
