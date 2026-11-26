#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

#include "engine/network/NetworkTestOptions.h"

namespace engine::net {

inline constexpr std::uint64_t NetworkTestHistoryWindowMs = 300'000;
inline constexpr std::size_t MaxNetworkTestHistoryPoints = 1'201;

struct NetworkTestHistoryPoint {
    std::uint64_t elapsedMs{};
    NetworkTestMetrics metrics;
    bool breakBefore{};
    NetworkTestProfile profile{};
};

class NetworkTestHistory final {
public:
    void append(NetworkTestHistoryPoint point) {
        const auto elapsedMs = point.elapsedMs;
        if (!m_points.empty()) {
            const auto previous = m_points.back().elapsedMs;
            if (elapsedMs <= previous) return;
            if (elapsedMs - previous > 1'500) point.breakBefore = true;
        }
        m_points.push_back(std::move(point));
        prune(elapsedMs);
    }

    void prune(std::uint64_t elapsedMs) {
        const auto oldest = elapsedMs > NetworkTestHistoryWindowMs ? elapsedMs - NetworkTestHistoryWindowMs : 0;
        while (!m_points.empty() && (m_points.size() > MaxNetworkTestHistoryPoints || m_points.front().elapsedMs < oldest)) {
            m_points.pop_front();
        }
    }

    [[nodiscard]] const std::deque<NetworkTestHistoryPoint>& points() const noexcept { return m_points; }

private:
    std::deque<NetworkTestHistoryPoint> m_points;
};

struct NetworkTestHistoryView {
    std::uint64_t elapsedMs{};
    const NetworkTestHistory* history{};
};

}
