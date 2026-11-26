#pragma once

#include <cstdint>
#include <optional>

#include "engine/network/DatagramTransport.h"

namespace engine::net {

struct NetworkTestTelemetrySample {
    std::uint64_t nonce{};
    std::uint32_t revision{};
    std::uint64_t elapsedMs{};
    std::optional<std::uint32_t> rttMs;
    std::uint64_t txPackets{};
    std::uint64_t rxPackets{};
    std::uint64_t txBytes{};
    std::uint64_t rxBytes{};
    NetworkLossCounters outbound;
    NetworkLossCounters probes;
};

struct NetworkTestTrafficRate {
    double bytesPerSecond{};
    double packetsPerSecond{};
};

struct NetworkTestMetrics {
    std::optional<std::uint32_t> rttMs;
    std::optional<double> uplinkLossPercent;
    std::optional<double> downlinkLossPercent;
    std::optional<double> probeLossPercent;
    std::optional<NetworkTestTrafficRate> upload;
    std::optional<NetworkTestTrafficRate> download;
};

}
