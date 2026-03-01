#pragma once

#include <cstdint>

namespace engine::ecs {

using EntityId = std::uint32_t;

constexpr EntityId kInvalidEntity = 0;

} // namespace engine::ecs
