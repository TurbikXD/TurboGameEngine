#pragma once

#include <cstdint>

namespace engine::ecs {

// Opaque World-local handle: the full EnTT entity/version value plus one.
// Keep zero as the scene-file/editor sentinel; never use handles as array indices.
using EntityId = std::uint32_t;

constexpr EntityId kInvalidEntity = 0;

} // namespace engine::ecs
