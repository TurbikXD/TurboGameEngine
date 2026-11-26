#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <ranges>
#include <vector>

#include "engine/network/NetworkTestHistory.h"

namespace engine::net {

struct NetworkTestSummary {
    std::optional<double> meanRttMs;
    std::optional<double> p95RttMs;
    std::optional<double> meanUplinkLossPercent;
    std::optional<double> meanDownlinkLossPercent;
    std::optional<double> meanProbeLossPercent;
    std::optional<double> meanUploadBytesPerSecond;
    std::optional<double> meanDownloadBytesPerSecond;
    std::optional<double> meanUploadPacketsPerSecond;
    std::optional<double> meanDownloadPacketsPerSecond;
};

struct NetworkTestSummaryMetric {
    struct Values {
        std::optional<double> runA;
        std::optional<double> runB;
        std::optional<double> delta;
    };

    const char* id;
    const char* label;
    std::optional<double> NetworkTestSummary::*member;
    double divisor;
    const char* unit;
    const char* deltaUnit;

    [[nodiscard]] Values values(const NetworkTestSummary& runA, const NetworkTestSummary& runB) const {
        const auto& a = runA.*member;
        const auto& b = runB.*member;
        return {
            a ? std::optional{*a / divisor} : std::nullopt,
            b ? std::optional{*b / divisor} : std::nullopt,
            a && b ? std::optional{(*b - *a) / divisor} : std::nullopt,
        };
    }
};

inline constexpr NetworkTestSummaryMetric NetworkTestSummaryMetrics[]{
    {"mean_rtt", "Mean RTT (ms)", &NetworkTestSummary::meanRttMs, 1.0, "ms", "ms"},
    {"p95_rtt", "p95 RTT (ms)", &NetworkTestSummary::p95RttMs, 1.0, "ms", "ms"},
    {"mean_uplink_loss", "Mean up loss", &NetworkTestSummary::meanUplinkLossPercent, 1.0, "%", "pp"},
    {"mean_downlink_loss", "Mean down loss", &NetworkTestSummary::meanDownlinkLossPercent, 1.0, "%", "pp"},
    {"mean_probe_loss", "Mean probe loss", &NetworkTestSummary::meanProbeLossPercent, 1.0, "%", "pp"},
    {"mean_upload_kib_per_second", "Mean upload (KiB/s)", &NetworkTestSummary::meanUploadBytesPerSecond, 1024.0, "KiB/s", "KiB/s"},
    {"mean_download_kib_per_second", "Mean download (KiB/s)", &NetworkTestSummary::meanDownloadBytesPerSecond, 1024.0, "KiB/s", "KiB/s"},
    {"mean_upload_packets_per_second", "Mean upload (pkt/s)", &NetworkTestSummary::meanUploadPacketsPerSecond, 1.0, "pkt/s", "pkt/s"},
    {"mean_download_packets_per_second", "Mean download (pkt/s)", &NetworkTestSummary::meanDownloadPacketsPerSecond, 1.0, "pkt/s", "pkt/s"},
};

[[nodiscard]] inline NetworkTestSummary summarizeNetworkTestHistory(
    std::ranges::subrange<std::deque<NetworkTestHistoryPoint>::const_iterator> points) {
    struct SampleMean {
        double value{};
        std::size_t count{};

        void add(double sample) {
            if (!std::isfinite(sample) || sample < 0.0) return;
            value += (sample - value) / static_cast<double>(++count);
        }

        void add(std::optional<double> sample) {
            if (sample) add(*sample);
        }

        [[nodiscard]] std::optional<double> result() const {
            if (count == 0) return std::nullopt;
            return value;
        }
    };

    SampleMean rtt, uplinkLoss, downlinkLoss, probeLoss, uploadBytes, downloadBytes, uploadPackets, downloadPackets;
    std::vector<std::uint32_t> rttSamples;
    rttSamples.reserve(points.size());
    for (const auto& point : points) {
        const auto& metrics = point.metrics;
        if (metrics.rttMs) {
            rtt.add(*metrics.rttMs);
            rttSamples.push_back(*metrics.rttMs);
        }
        uplinkLoss.add(metrics.uplinkLossPercent);
        downlinkLoss.add(metrics.downlinkLossPercent);
        probeLoss.add(metrics.probeLossPercent);
        if (metrics.upload) {
            uploadBytes.add(metrics.upload->bytesPerSecond);
            uploadPackets.add(metrics.upload->packetsPerSecond);
        }
        if (metrics.download) {
            downloadBytes.add(metrics.download->bytesPerSecond);
            downloadPackets.add(metrics.download->packetsPerSecond);
        }
    }

    std::optional<double> p95;
    if (!rttSamples.empty()) {
        const auto rank = rttSamples.size() - rttSamples.size() / 20;
        const auto percentile = rttSamples.begin() + static_cast<std::ptrdiff_t>(rank - 1);
        std::ranges::nth_element(rttSamples, percentile);
        p95 = *percentile;
    }

    return {
        .meanRttMs = rtt.result(),
        .p95RttMs = p95,
        .meanUplinkLossPercent = uplinkLoss.result(),
        .meanDownlinkLossPercent = downlinkLoss.result(),
        .meanProbeLossPercent = probeLoss.result(),
        .meanUploadBytesPerSecond = uploadBytes.result(),
        .meanDownloadBytesPerSecond = downloadBytes.result(),
        .meanUploadPacketsPerSecond = uploadPackets.result(),
        .meanDownloadPacketsPerSecond = downloadPackets.result(),
    };
}

}
