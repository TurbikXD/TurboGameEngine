#include "engine/ecs/transform_batch.h"

#include <stdexcept>
#include "engine/core/JobSystem.h"
#include "engine/core/Profiling.h"
#include "engine/ecs/transform_utils.h"
#include "engine/ecs/world.h"

namespace engine::ecs {
void prepareWorldMatrices(const World& world, const std::span<const EntityId> entities,
                          const std::span<glm::mat4> matrices, core::JobSystem* const jobs) {
    ENGINE_PROFILE_ZONE("Render Prepare Transforms");
    if (entities.size() != matrices.size()) {
        throw std::invalid_argument("Transform batch input/output sizes differ");
    }
    const auto range = [&](const std::size_t begin, const std::size_t end) {
        ENGINE_PROFILE_ZONE("Job Render Transforms");
        for (std::size_t index = begin; index < end; ++index) {
            matrices[index] = computeWorldMatrix(world, entities[index]);
        }
    };
    if (jobs != nullptr && entities.size() >= 1024U) {
        jobs->parallelFor(entities.size(), 256U, range);
    } else {
        range(0U, entities.size());
    }
}
}
