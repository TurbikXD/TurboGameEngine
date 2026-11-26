#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/network/NetworkTestHistory.h"
#include "engine/network/NetworkTestSummary.h"

namespace engine::core {

inline constexpr std::string_view NetworkTestCsvHeader =
    "client_index,elapsed_ms,break_before,profile_revision,configured_latency_ms,configured_loss_percent,"
    "rtt_ms,uplink_loss_percent,downlink_loss_percent,probe_loss_percent,"
    "upload_bytes_per_second,upload_packets_per_second,download_bytes_per_second,download_packets_per_second";

struct NetworkTestCsvRun {
    std::map<unsigned, net::NetworkTestHistory> clients;
};

[[nodiscard]] std::optional<NetworkTestCsvRun> readNetworkTestCsv(const std::filesystem::path& path, std::string& error);

[[nodiscard]] bool isNetworkTestCsvSource(const std::filesystem::path& target, const std::filesystem::path& runA,
                                          const std::filesystem::path& runB);

struct NetworkTestSummaryCsv {
    unsigned clientIndex{};
    std::uint64_t rangeStartMs{};
    std::uint64_t rangeEndMs{};
    std::string_view rangeLabel;
    std::filesystem::path runAPath;
    std::filesystem::path runBPath;
    net::NetworkTestSummary runA;
    net::NetworkTestSummary runB;
};

[[nodiscard]] bool writeNetworkTestSummaryCsv(const std::filesystem::path& target, const NetworkTestSummaryCsv& summary,
                                             std::string& error);

[[nodiscard]] bool writeNetworkTestComparisonReport(const std::filesystem::path& pngPath, const std::string& pngBytes,
                                                   const NetworkTestSummaryCsv& summary, std::string& error);

struct NetworkTestComparisonReport {
    NetworkTestSummaryCsv summary;
    std::string pngBytes;
    std::array<std::vector<net::NetworkTestHistoryPoint>, 2> chartSamples{};
};

[[nodiscard]] bool writeNetworkTestComparisonReports(const std::filesystem::path& basePngPath,
                                                    std::span<NetworkTestComparisonReport> reports, std::string& error);

[[nodiscard]] bool writeNetworkTestComparisonArchive(const std::filesystem::path& zipPath,
                                                    std::span<NetworkTestComparisonReport> reports, std::string& error);

}
