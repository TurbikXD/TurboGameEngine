#pragma once

#include "engine/network/DatagramTransport.h"
#include "engine/network/NetworkReplica.h"
#include "engine/network/PeerName.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::net {

inline constexpr unsigned TickRate = 60;
inline constexpr unsigned SnapshotRate = 20;
inline constexpr uint16_t DefaultPort = 27020;
inline constexpr size_t MaxPeers = 32;
inline constexpr size_t MaxReplicas = 40;

using EntityId = uint32_t;
inline constexpr EntityId KeyedReplicaBit = MaxReplicaKey + 1U;

constexpr EntityId keyedReplicaId(uint32_t key) {
    return key && key <= MaxReplicaKey ? KeyedReplicaBit | key : 0;
}

constexpr uint32_t replicaKey(EntityId id) {
    return (id & KeyedReplicaBit) ? id & MaxReplicaKey : 0;
}

struct PlayerInput {
    int8_t x{};
    int8_t z{};
    uint16_t yaw{};
    int16_t pitch{};
    uint16_t buttons{};
    uint8_t pressed{};
    bool hasControls{};
};

inline constexpr int MaxControlAxis = 127;
inline constexpr int MaxControlPitch = 16384;
inline constexpr unsigned ControlActionCount = 8;
inline constexpr uint8_t PeerMotionValid = 0x80;

struct EntityState {
    EntityId id{};
    int32_t x{};
    int32_t y{};
    int32_t z{};
    uint16_t yaw{};
    uint16_t value{};
    uint8_t flags{};
    uint16_t pitch{};
    uint16_t roll{};
};

struct AuthoritativeReplica {
    EntityState entity;
    uint64_t tick{};
    uint64_t inputAck{};
};

struct VisualEntity {
    EntityId id{};
    float x{};
    float y{};
    float z{};
    float yaw{};
    uint16_t value{};
    uint8_t flags{};
    float pitch{};
    float roll{};
    uint8_t sampledFlags{flags};
};

struct RpcCommand {
    uint8_t action{};
    EntityId target{};
    int32_t argument{};
    std::string text{};
};

enum class RpcStatus : uint8_t {
    Accepted,
    UnknownAction,
    InvalidArgument,
    Forbidden,
    RateLimited,
    TimedOut,
    Disconnected
};

struct RpcResult {
    uint32_t request{};
    RpcStatus status{};
    uint16_t value{};
};

std::string_view rpcStatusName(RpcStatus status);

using SimulatePlayer = void (*)(EntityState&, PlayerInput);

class ServerGame {
public:
    virtual ~ServerGame() = default;
    virtual EntityState spawn(EntityId id, size_t slot) = 0;
    virtual RpcStatus rpc(EntityState& player, const RpcCommand& command) = 0;
    virtual void despawn(EntityId) {}
    virtual void input(EntityId, PlayerInput) {}
};

struct ServerConfig {
    Endpoint bind{0x7f000001, DefaultPort};
    uint8_t maxPeers{16};
    int32_t relevanceRadiusMm{};
    bool deltaSnapshots{true};
    uint64_t compatibility{};
    bool deferSnapshots{};
};

struct NetworkStats {
    TransportStats transport{};
    uint32_t rttMs{};
    uint32_t invalidMessages{};
    uint64_t fullSnapshotBytes{};
    uint64_t snapshotBytes{};
    uint64_t snapshotsSent{};
    uint64_t snapshotsReceived{};
    uint32_t corrections{};
    float lastCorrectionMm{};
    size_t relevantEntities{};
    std::optional<uint32_t> recentRttMs;
    NetworkLossCounters probes{};
};

enum class ClientPhase { Disconnected, Connecting, Connected, Rejected, TimedOut };

class NetworkServer {
public:
    NetworkServer(ServerGame& game, SimulatePlayer simulate);
    ~NetworkServer();

    bool start(const ServerConfig& config, std::string& error);
    void setSteamLobby(uint64_t lobbyId);
    void stop();
    void tick();
    void publishSnapshots();
    Endpoint localEndpoint() const;
    size_t peerCount() const;
    bool setPeerValue(EntityId peer, uint16_t value);
    bool setPeerMotion(EntityId peer, int32_t vx, int32_t vy, int32_t vz, uint16_t jumpBufferMs, uint16_t coyoteMs);
    std::optional<uint16_t> peerValue(EntityId peer) const;
    bool setLocalSteamIdentity(EntityId id);
    uint64_t peerSteamId(EntityId peer) const;
    bool setPeerName(EntityId peer, std::string_view name);
    std::string_view peerName(EntityId peer) const;
    EntityId createReplica(EntityState initial);
    bool createKeyedReplica(uint32_t key, EntityState initial);
    bool setReplica(const EntityState& state);
    bool destroyReplica(EntityId id);
    void setEmulation(NetworkEmulation emulation);
    bool setPeerEmulation(uint64_t connectionNonce, NetworkEmulation emulation);
    EntityId loopbackPeerForNonce(uint64_t connectionNonce) const;
    std::optional<NetworkLossCounters> peerEmulationCounters(uint64_t connectionNonce) const;
    NetworkStats stats() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

class NetworkClient {
public:
    explicit NetworkClient(SimulatePlayer simulate);
    ~NetworkClient();

    bool connect(Endpoint server, std::string& error, uint64_t compatibility = 0);
    void disconnect();
    void tick(PlayerInput input);
    ClientPhase phase() const;
    std::string_view diagnostic() const;
    EntityId localEntity() const;
    uint64_t connectionNonce() const;
    uint64_t nextInputSequence() const;
    std::optional<uint16_t> peerValue(EntityId peer) const;
    uint64_t peerSteamId(EntityId peer) const;
    std::string_view peerName(EntityId peer) const;
    std::optional<AuthoritativeReplica> latestReplica(EntityId id) const;
    std::vector<VisualEntity> presentation(float fraction = 0.0F) const;
    uint32_t call(RpcCommand command);
    bool takeResult(RpcResult& result);
    void setEmulation(NetworkEmulation emulation, bool includeSessionControl = true);
    void resetProbeMeasurements();
    void setPrediction(bool enabled);
    void setInterpolation(bool enabled);
    NetworkStats stats() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

}
