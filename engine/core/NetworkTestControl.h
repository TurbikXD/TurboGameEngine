#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/network/NetworkTestOptions.h"
#include "engine/network/NetworkTestTelemetry.h"

namespace engine::core {

bool writeNetworkTestProfiles(const std::filesystem::path& directory,
                              std::span<const net::NetworkTestProfile> profiles, std::string& error);
std::optional<std::vector<net::NetworkTestProfile>> readNetworkTestProfiles(
    const std::filesystem::path& directory, unsigned clientCount);

bool writeNetworkTestClientStatus(const std::filesystem::path& directory, unsigned index,
                                  const net::NetworkTestClientStatus& status, std::string& error);
std::optional<net::NetworkTestClientStatus> readNetworkTestClientStatus(
    const std::filesystem::path& directory, unsigned index);

bool writeNetworkTestAppliedProfiles(const std::filesystem::path& directory,
                                     std::span<const std::optional<net::NetworkTestAppliedProfile>> profiles,
                                     std::string& error);
std::optional<std::vector<std::optional<net::NetworkTestAppliedProfile>>> readNetworkTestAppliedProfiles(
    const std::filesystem::path& directory, unsigned clientCount);

bool writeNetworkTestClientTelemetry(const std::filesystem::path& directory, unsigned index,
                                     const net::NetworkTestTelemetrySample& sample, std::string& error);
std::optional<net::NetworkTestTelemetrySample> readNetworkTestClientTelemetry(
    const std::filesystem::path& directory, unsigned index);

bool writeNetworkTestServerTelemetry(const std::filesystem::path& directory,
                                     std::span<const std::optional<net::NetworkTestTelemetrySample>> samples,
                                     std::string& error);
std::optional<std::vector<std::optional<net::NetworkTestTelemetrySample>>> readNetworkTestServerTelemetry(
    const std::filesystem::path& directory, unsigned clientCount);

}
