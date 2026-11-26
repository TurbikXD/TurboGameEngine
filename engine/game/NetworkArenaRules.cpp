#include "engine/game/NetworkArenaRules.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace engine::game {

std::size_t nextArenaRelay(const std::uint8_t slot, const std::uint16_t score) {
    static constexpr std::array<std::size_t, 5> westRoute{0, 2, 3, 1, 4};
    static constexpr std::array<std::size_t, 5> eastRoute{1, 2, 4, 0, 3};
    const auto& route = slot % 2 == 0 ? westRoute : eastRoute;
    return route[score % route.size()];
}

void simulateArenaPlayer(net::EntityState& player, const net::PlayerInput input) {
    if (input.x == 0 && input.z == 0) return;
    constexpr int speedMmPerTick = 7200 / static_cast<int>(net::TickRate);
    const int x = input.x;
    const int z = input.z;
    const int length = std::max(127, static_cast<int>(std::ceil(std::sqrt(x*x + z*z))));
    const auto advance = [length](const std::int32_t position, const int axis, const std::int32_t limit) {
        const auto next = static_cast<std::int64_t>(position) + speedMmPerTick * axis / length;
        return static_cast<std::int32_t>(std::clamp(next, -static_cast<std::int64_t>(limit), static_cast<std::int64_t>(limit)));
    };
    player.x = advance(player.x, x, ArenaHalfWidthMm);
    player.z = advance(player.z, z, ArenaHalfDepthMm);
    const double radians = std::atan2(static_cast<double>(x), static_cast<double>(z));
    constexpr double turnUnits = 65536.0 / (2.0 * std::numbers::pi);
    player.yaw = static_cast<std::uint16_t>(std::lround(radians * turnUnits));
}

net::EntityState NetworkArenaRules::spawn(const net::EntityId id, const std::size_t slot) {
    net::EntityState player{};
    player.id = id;
    player.x = slot % 2 == 0 ? -3900 : 3900;
    player.z = static_cast<std::int32_t>(slot / 2) * 1100;
    player.yaw = static_cast<std::uint16_t>(slot % 2 == 0 ? 49152 : 16384);
    player.flags = static_cast<std::uint8_t>(slot);
    return player;
}

net::RpcStatus NetworkArenaRules::rpc(net::EntityState& player, const net::RpcCommand& command) {
    if (command.action != ArenaCaptureAction) return net::RpcStatus::UnknownAction;
    if (command.target != player.id) return net::RpcStatus::Forbidden;
    if (command.argument < 0 || static_cast<std::size_t>(command.argument) >= ArenaRelays.size()) {
        return net::RpcStatus::InvalidArgument;
    }
    const auto relayIndex = static_cast<std::size_t>(command.argument);
    if (relayIndex != nextArenaRelay(player.flags, player.value)) return net::RpcStatus::InvalidArgument;
    const auto& relay = ArenaRelays[relayIndex];
    const auto dx = static_cast<std::int64_t>(player.x) - relay.x;
    const auto dz = static_cast<std::int64_t>(player.z) - relay.z;
    if (dx*dx + dz*dz > static_cast<std::int64_t>(ArenaCaptureRadiusMm) * ArenaCaptureRadiusMm ||
        player.value == std::numeric_limits<std::uint16_t>::max()) {
        return net::RpcStatus::Forbidden;
    }
    ++player.value;
    return net::RpcStatus::Accepted;
}

}
