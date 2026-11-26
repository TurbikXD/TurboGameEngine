#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/network/NetworkSession.h"

namespace engine::game {

struct ArenaRelay {
    const char* name;
    std::int32_t x;
    std::int32_t z;
};

inline constexpr std::array ArenaRelays{
    ArenaRelay{"West relay", -5200, 0},
    ArenaRelay{"East relay", 5200, 0},
    ArenaRelay{"Central relay", 0, -4500},
    ArenaRelay{"Far east relay", 13500, 4200},
    ArenaRelay{"Far west relay", -13500, 4200},
};

inline constexpr std::int32_t ArenaHalfWidthMm = 16000;
inline constexpr std::int32_t ArenaHalfDepthMm = 8000;
inline constexpr std::int32_t ArenaCaptureRadiusMm = 2000;
inline constexpr std::int32_t ArenaRelevanceMm = 18000;
inline constexpr std::uint8_t ArenaCaptureAction = 1;
inline constexpr std::uint8_t ArenaUnknownAction = 255;

std::size_t nextArenaRelay(std::uint8_t slot, std::uint16_t score);
void simulateArenaPlayer(net::EntityState& player, net::PlayerInput input);

class NetworkArenaRules final : public net::ServerGame {
public:
    net::EntityState spawn(net::EntityId id, std::size_t slot) override;
    net::RpcStatus rpc(net::EntityState& player, const net::RpcCommand& command) override;
};

}
