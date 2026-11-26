#include "engine/core/NetworkTestControl.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <thread>

#include "engine/core/AtomicFile.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace engine::core {
namespace {

using Json = nlohmann::json;

constexpr std::size_t MaxControlBytes = 4096;
constexpr std::size_t MaxServerTelemetryBytes = 8192;
constexpr auto MaxClients = static_cast<unsigned>(net::MaxNetworkTestClients);
constexpr auto MaxRevision = std::numeric_limits<std::uint32_t>::max();
constexpr auto MaxState = static_cast<unsigned>(net::NetworkTestLinkState::Closed);

bool validSlot(unsigned index) {
    return index >= 1 && index <= MaxClients;
}

std::filesystem::path statusPath(const std::filesystem::path& directory, unsigned index) {
    return directory / ("client-" + std::to_string(index) + ".json");
}

std::filesystem::path clientTelemetryPath(const std::filesystem::path& directory, unsigned index) {
    return directory / ("client-" + std::to_string(index) + "-telemetry.json");
}

template<std::size_t MaxBytes = MaxControlBytes>
Json readControlFile(const std::filesystem::path& path) {
    std::array<char, MaxBytes + 1> bytes;
    std::size_t size{};
#if defined(_WIN32)
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    DWORD count{};
    const BOOL read = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr);
    CloseHandle(file);
    if (!read) return {};
    size = count;
#else
    std::ifstream input(path, std::ios::binary);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad()) return {};
    size = static_cast<std::size_t>(input.gcount());
#endif
    if (size == 0 || size > MaxBytes) return {};
    return Json::parse(bytes.data(), bytes.data() + size, nullptr, false);
}

bool writeControlFile(const std::filesystem::path& path, const Json& document, std::string& error) {
    const auto bytes = document.dump();
#if defined(_WIN32)
    for (int retry = 0; retry < 2; ++retry) {
        if (writeFileAtomically(path, bytes, error)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
    return writeFileAtomically(path, bytes, error);
}

std::optional<std::uint64_t> unsignedField(const Json& object, const char* key,
                                          std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer() ||
        (!found->is_number_unsigned() && found->get<std::int64_t>() < 0)) return {};
    const auto value = found->get<std::uint64_t>();
    return value <= maximum ? std::optional{value} : std::nullopt;
}

bool invalidControl(std::string& error) {
    error = "Invalid Network Test control data.";
    return false;
}

bool validLoss(const net::NetworkLossCounters& counters) {
    return counters.dropped <= counters.packets;
}

Json encodeLoss(const net::NetworkLossCounters& counters) {
    return {{"packets", counters.packets}, {"dropped", counters.dropped}};
}

std::optional<net::NetworkLossCounters> decodeLoss(const Json& document, const char* key) {
    const auto field = document.find(key);
    if (field == document.end()) return {};
    const auto packets = unsignedField(*field, "packets");
    const auto dropped = unsignedField(*field, "dropped");
    if (!packets || !dropped || *dropped > *packets) return {};
    return net::NetworkLossCounters{*packets, *dropped};
}

Json encodeTelemetry(const net::NetworkTestTelemetrySample& sample) {
    return {{"nonce", sample.nonce}, {"revision", sample.revision}, {"elapsedMs", sample.elapsedMs},
            {"outbound", encodeLoss(sample.outbound)}};
}

std::optional<net::NetworkTestTelemetrySample> decodeTelemetry(const Json& document) {
    const auto nonce = unsignedField(document, "nonce");
    const auto revision = unsignedField(document, "revision", MaxRevision);
    const auto elapsed = unsignedField(document, "elapsedMs");
    const auto outbound = decodeLoss(document, "outbound");
    if (!nonce || !*nonce || !revision || !*revision || !elapsed || !outbound) return {};
    net::NetworkTestTelemetrySample sample{};
    sample.nonce = *nonce;
    sample.revision = static_cast<std::uint32_t>(*revision);
    sample.elapsedMs = *elapsed;
    sample.outbound = *outbound;
    return sample;
}

}

bool writeNetworkTestProfiles(const std::filesystem::path& directory,
                              std::span<const net::NetworkTestProfile> profiles, std::string& error) {
    if (directory.empty() || profiles.empty() || profiles.size() > MaxClients) return invalidControl(error);
    auto document = Json::array();
    for (const auto& profile : profiles) {
        if (!profile.conditions.valid() || !profile.revision) return invalidControl(error);
        document.push_back({{"latencyMs", profile.conditions.latencyMs}, {"lossPercent", profile.conditions.lossPercent},
                            {"revision", profile.revision}});
    }
    return writeControlFile(directory / "profiles.json", document, error);
}

std::optional<std::vector<net::NetworkTestProfile>> readNetworkTestProfiles(
    const std::filesystem::path& directory, unsigned clientCount) {
    if (directory.empty() || !validSlot(clientCount)) return {};
    const auto document = readControlFile(directory / "profiles.json");
    if (!document.is_array() || document.size() != clientCount) return {};
    std::vector<net::NetworkTestProfile> profiles;
    profiles.reserve(clientCount);
    for (const auto& record : document) {
        const auto latency = unsignedField(record, "latencyMs", net::MaxNetworkTestLatencyMs);
        const auto loss = unsignedField(record, "lossPercent", net::MaxNetworkTestLossPercent);
        const auto revision = unsignedField(record, "revision", MaxRevision);
        if (!latency || !loss || !revision || !*revision) return {};
        profiles.push_back({{static_cast<int>(*latency), static_cast<int>(*loss)}, static_cast<std::uint32_t>(*revision)});
    }
    return profiles;
}

bool writeNetworkTestClientStatus(const std::filesystem::path& directory, unsigned index,
                                  const net::NetworkTestClientStatus& status, std::string& error) {
    if (directory.empty() || !validSlot(index) || static_cast<unsigned>(status.state) > MaxState) {
        return invalidControl(error);
    }
    const Json document = {{"nonce", status.nonce}, {"revision", status.revision}, {"state", static_cast<unsigned>(status.state)}};
    return writeControlFile(statusPath(directory, index), document, error);
}

std::optional<net::NetworkTestClientStatus> readNetworkTestClientStatus(
    const std::filesystem::path& directory, unsigned index) {
    if (directory.empty() || !validSlot(index)) return {};
    const auto document = readControlFile(statusPath(directory, index));
    const auto nonce = unsignedField(document, "nonce");
    const auto revision = unsignedField(document, "revision", MaxRevision);
    const auto state = unsignedField(document, "state", MaxState);
    if (!nonce || !revision || !state) return {};
    return net::NetworkTestClientStatus{*nonce, static_cast<std::uint32_t>(*revision),
                                        static_cast<net::NetworkTestLinkState>(*state)};
}

bool writeNetworkTestAppliedProfiles(const std::filesystem::path& directory,
                                     std::span<const std::optional<net::NetworkTestAppliedProfile>> profiles,
                                     std::string& error) {
    if (directory.empty() || profiles.empty() || profiles.size() > MaxClients) return invalidControl(error);
    auto document = Json::array();
    for (const auto& profile : profiles) {
        if (!profile) {
            document.push_back(nullptr);
            continue;
        }
        if (!profile->nonce || !profile->revision) return invalidControl(error);
        document.push_back({{"nonce", profile->nonce}, {"revision", profile->revision}});
    }
    return writeControlFile(directory / "applied.json", document, error);
}

std::optional<std::vector<std::optional<net::NetworkTestAppliedProfile>>> readNetworkTestAppliedProfiles(
    const std::filesystem::path& directory, unsigned clientCount) {
    if (directory.empty() || !validSlot(clientCount)) return {};
    const auto document = readControlFile(directory / "applied.json");
    if (!document.is_array() || document.size() != clientCount) return {};
    std::vector<std::optional<net::NetworkTestAppliedProfile>> profiles;
    profiles.reserve(clientCount);
    for (const auto& record : document) {
        if (record.is_null()) {
            profiles.push_back(std::nullopt);
            continue;
        }
        const auto nonce = unsignedField(record, "nonce");
        const auto revision = unsignedField(record, "revision", MaxRevision);
        if (!nonce || !*nonce || !revision || !*revision) return {};
        profiles.push_back(net::NetworkTestAppliedProfile{*nonce, static_cast<std::uint32_t>(*revision)});
    }
    return profiles;
}

bool writeNetworkTestClientTelemetry(const std::filesystem::path& directory, unsigned index,
                                     const net::NetworkTestTelemetrySample& sample, std::string& error) {
    if (directory.empty() || !validSlot(index) || !sample.nonce || !sample.revision ||
        !validLoss(sample.outbound) || !validLoss(sample.probes)) return invalidControl(error);
    auto document = encodeTelemetry(sample);
    document["rttMs"] = sample.rttMs ? Json(*sample.rttMs) : Json(nullptr);
    document["txPackets"] = sample.txPackets;
    document["rxPackets"] = sample.rxPackets;
    document["txBytes"] = sample.txBytes;
    document["rxBytes"] = sample.rxBytes;
    document["probes"] = encodeLoss(sample.probes);
    return writeControlFile(clientTelemetryPath(directory, index), document, error);
}

std::optional<net::NetworkTestTelemetrySample> readNetworkTestClientTelemetry(
    const std::filesystem::path& directory, unsigned index) {
    if (directory.empty() || !validSlot(index)) return {};
    const auto document = readControlFile(clientTelemetryPath(directory, index));
    auto sample = decodeTelemetry(document);
    if (!sample) return {};
    const auto rtt = document.find("rttMs");
    if (rtt == document.end()) return {};
    if (!rtt->is_null()) {
        const auto milliseconds = unsignedField(document, "rttMs", std::numeric_limits<std::uint32_t>::max());
        if (!milliseconds) return {};
        sample->rttMs = static_cast<std::uint32_t>(*milliseconds);
    }
    const auto txPackets = unsignedField(document, "txPackets");
    const auto rxPackets = unsignedField(document, "rxPackets");
    const auto txBytes = unsignedField(document, "txBytes");
    const auto rxBytes = unsignedField(document, "rxBytes");
    const auto probes = decodeLoss(document, "probes");
    if (!txPackets || !rxPackets || !txBytes || !rxBytes || !probes) return {};
    sample->txPackets = *txPackets;
    sample->rxPackets = *rxPackets;
    sample->txBytes = *txBytes;
    sample->rxBytes = *rxBytes;
    sample->probes = *probes;
    return sample;
}

bool writeNetworkTestServerTelemetry(const std::filesystem::path& directory,
                                     std::span<const std::optional<net::NetworkTestTelemetrySample>> samples,
                                     std::string& error) {
    if (directory.empty() || samples.empty() || samples.size() > MaxClients) return invalidControl(error);
    auto document = Json::array();
    for (const auto& sample : samples) {
        if (!sample) {
            document.push_back(nullptr);
            continue;
        }
        if (!sample->nonce || !sample->revision || !validLoss(sample->outbound)) return invalidControl(error);
        document.push_back(encodeTelemetry(*sample));
    }
    return writeControlFile(directory / "server-telemetry.json", document, error);
}

std::optional<std::vector<std::optional<net::NetworkTestTelemetrySample>>> readNetworkTestServerTelemetry(
    const std::filesystem::path& directory, unsigned clientCount) {
    if (directory.empty() || !validSlot(clientCount)) return {};
    const auto document = readControlFile<MaxServerTelemetryBytes>(directory / "server-telemetry.json");
    if (!document.is_array() || document.size() != clientCount) return {};
    std::vector<std::optional<net::NetworkTestTelemetrySample>> samples;
    samples.reserve(clientCount);
    for (const auto& record : document) {
        if (record.is_null()) {
            samples.push_back(std::nullopt);
            continue;
        }
        auto sample = decodeTelemetry(record);
        if (!sample) return {};
        samples.push_back(std::move(sample));
    }
    return samples;
}

}
