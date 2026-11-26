#pragma once

#include "engine/network/NetworkTestTelemetry.h"

namespace engine::net {

inline constexpr int MaxNetworkTestClients = 32;
inline constexpr int MaxNetworkTestLatencyMs = 500;
inline constexpr int MaxNetworkTestLossPercent = 100;

struct NetworkTestConditions {
    int latencyMs{};
    int lossPercent{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return latencyMs >= 0 && latencyMs <= MaxNetworkTestLatencyMs &&
            lossPercent >= 0 && lossPercent <= MaxNetworkTestLossPercent;
    }

    [[nodiscard]] constexpr NetworkEmulation emulation() const noexcept {
        return {static_cast<std::uint16_t>(latencyMs), static_cast<std::uint8_t>(lossPercent)};
    }

    bool operator==(const NetworkTestConditions&) const = default;
};

struct NetworkTestOptions {
    int clientCount{2};
    int latencyMs{};
    int lossPercent{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return clientCount >= 1 && clientCount <= MaxNetworkTestClients && conditions().valid();
    }

    [[nodiscard]] constexpr NetworkTestConditions conditions() const noexcept {
        return {latencyMs, lossPercent};
    }

    [[nodiscard]] constexpr NetworkEmulation emulation() const noexcept {
        return conditions().emulation();
    }
};

struct NetworkTestProfile {
    NetworkTestConditions conditions;
    std::uint32_t revision{1};

    bool operator==(const NetworkTestProfile&) const = default;
};

enum class NetworkTestLinkState { Starting, Connecting, Connected, TimedOut, Disconnected, Rejected, Closed };

struct NetworkTestClientStatus {
    std::uint64_t nonce{};
    std::uint32_t revision{};
    NetworkTestLinkState state{NetworkTestLinkState::Starting};

    bool operator==(const NetworkTestClientStatus&) const = default;
};

struct NetworkTestAppliedProfile {
    std::uint64_t nonce{};
    std::uint32_t revision{};

    bool operator==(const NetworkTestAppliedProfile&) const = default;
};

struct NetworkTestClientView {
    unsigned index{};
    NetworkTestConditions conditions;
    NetworkTestLinkState state{NetworkTestLinkState::Starting};
    bool editable{};
    bool applying{};
    NetworkTestMetrics metrics;
};

}
