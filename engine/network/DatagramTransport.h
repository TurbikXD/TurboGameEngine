#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::net {

inline constexpr std::size_t MaxDatagramBytes = 1200;

struct Endpoint {
    std::uint32_t address{0x7f000001};
    std::uint16_t port{};
    std::uint64_t steamId{};

    bool operator==(const Endpoint&) const = default;
};

std::optional<Endpoint> parseEndpoint(std::string_view text, std::uint16_t defaultPort);
std::optional<Endpoint> resolveConnectEndpoint(std::string_view text, std::uint16_t defaultPort,
                                               std::string& error);
std::string formatEndpoint(Endpoint endpoint);

struct Datagram {
    Endpoint peer;
    std::array<std::uint8_t, MaxDatagramBytes> data{};
    std::size_t size{};
};

struct NetworkLossCounters {
    std::uint64_t packets{};
    std::uint64_t dropped{};
};

struct TransportStats {
    std::uint64_t txPackets{};
    std::uint64_t rxPackets{};
    std::uint64_t txBytes{};
    std::uint64_t rxBytes{};
    std::uint64_t emulatedDrops{};
    std::uint64_t sendQueueDrops{};
    std::uint64_t receiveQueueDrops{};
    std::uint64_t socketErrors{};
    std::uint64_t oversizedPackets{};
    std::uint64_t emulatedPackets{};
};

struct NetworkEmulation {
    std::uint16_t latencyMs{};
    std::uint8_t lossPercent{};
};

enum class EmulationPolicy { Apply, Bypass };

struct DatagramSendResult {
    bool accepted{};
    bool consideredByEmulator{};
    bool emulatedDrop{};
};

class DatagramTransport final {
public:
    DatagramTransport();
    ~DatagramTransport();

    DatagramTransport(const DatagramTransport&) = delete;
    DatagramTransport& operator=(const DatagramTransport&) = delete;

    bool start(Endpoint bind, std::string& error);
    bool startClient(Endpoint server, std::string& error);
    void poll();
    void setSteamLobby(std::uint64_t lobbyId);
    void flushReady();
    void stop();
    Endpoint localEndpoint() const;
    bool send(Endpoint peer, std::span<const std::uint8_t> data,
              EmulationPolicy policy = EmulationPolicy::Apply,
              std::optional<NetworkEmulation> emulationOverride = std::nullopt);
    DatagramSendResult sendWithResult(Endpoint peer, std::span<const std::uint8_t> data,
                                      EmulationPolicy policy = EmulationPolicy::Apply,
                                      std::optional<NetworkEmulation> emulationOverride = std::nullopt);
    bool receive(Datagram& datagram);
    TransportStats stats() const;
    void setEmulation(NetworkEmulation settings);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
