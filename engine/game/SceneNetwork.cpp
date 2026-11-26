#include "engine/game/SceneNetwork.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <utility>

#include "engine/core/AtomicFile.h"
#include "engine/core/Log.h"
#include "engine/core/NetworkTestControl.h"
#include "engine/core/PlaySession.h"
#include "engine/ecs/components.h"
#include "engine/ecs/world.h"
#include "engine/game/SceneSteam.h"
#include "engine/network/NetworkReplica.h"
#include "engine/network/PeerName.h"
#include "engine/network/SteamTransport.h"
#include "engine/scripting/ScriptingSystem.h"

namespace engine::game {
namespace {

static_assert(net::MaxNetworkTestClients <= net::MaxPeers);
constexpr double RadiansPerUnit = 2 * std::numbers::pi / 65536;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void noPlayerSimulation(net::EntityState&, net::PlayerInput) {}

int32_t millimeters(float value) {
    const auto scaled = std::round(double(value) * 1000);
    return static_cast<int32_t>(std::clamp(scaled, double(std::numeric_limits<int32_t>::min()),
        double(std::numeric_limits<int32_t>::max())));
}

uint16_t angle(float value) {
    const auto normalized = std::remainder(double(value), 2 * std::numbers::pi);
    return static_cast<uint16_t>(std::lround(normalized / RadiansPerUnit));
}

int8_t inputAxis(float value) {
    return static_cast<int8_t>(std::lround(std::clamp(value, -1.0F, 1.0F) * net::MaxControlAxis));
}

scripting::ScriptReplicaState toScriptState(const net::VisualEntity& entity) {
    return {entity.x, entity.y, entity.z, entity.yaw, entity.value, entity.flags, entity.pitch, entity.roll, entity.sampledFlags};
}

scripting::ScriptReplicaState toScriptState(const net::EntityState& data) {
    return {data.x * 0.001F, data.y * 0.001F, data.z * 0.001F, float(data.yaw * RadiansPerUnit),
        data.value, data.flags, float(data.pitch * RadiansPerUnit), float(data.roll * RadiansPerUnit)};
}

net::NetworkTestLinkState linkState(net::ClientPhase phase) {
    switch (phase) {
    case net::ClientPhase::Connecting: return net::NetworkTestLinkState::Connecting;
    case net::ClientPhase::Connected: return net::NetworkTestLinkState::Connected;
    case net::ClientPhase::TimedOut: return net::NetworkTestLinkState::TimedOut;
    case net::ClientPhase::Rejected: return net::NetworkTestLinkState::Rejected;
    case net::ClientPhase::Disconnected: return net::NetworkTestLinkState::Disconnected;
    }
    return net::NetworkTestLinkState::Disconnected;
}

uint64_t readHostNonce(const std::filesystem::path& path) {
    char bytes[21];
    std::ifstream input(path, std::ios::binary);
    input.read(bytes, sizeof(bytes));
    if (input.bad() || input.gcount() == sizeof(bytes)) return 0;
    const auto* end = bytes + input.gcount();
    uint64_t nonce{};
    const auto parsed = std::from_chars(bytes, end, nonce);
    return parsed.ec == std::errc{} && parsed.ptr == end ? nonce : 0;
}

}

struct SceneNetwork::LiveControl {
    std::filesystem::path directory;
    net::Endpoint serverEndpoint;
    unsigned clientIndex{};
    std::vector<net::NetworkTestProfile> profiles;
    std::vector<std::optional<net::NetworkTestAppliedProfile>> serverPeerProfiles;
    std::vector<std::optional<net::NetworkTestAppliedProfile>> publishedServerProfiles;
    std::optional<net::NetworkTestClientStatus> publishedClientStatus;
    Clock::time_point nextPoll{};
    Clock::time_point nextReconnect{};
    Clock::time_point nextStatusWrite{};
    Clock::time_point telemetryStart{Clock::now()};
    Clock::time_point nextTelemetry{};
    Clock::time_point nextTelemetryWarning{};
    std::uint64_t telemetryNonce{};
    std::uint32_t telemetryRevision{};
};

struct SceneNetwork::RemoteClient {
    net::Endpoint endpoint;
    uint64_t compatibility{};
    Clock::time_point retryAt{};
    std::chrono::seconds retryDelay{1};
    std::string error;
};

struct SceneNetwork::HostControl {
    std::filesystem::path file;
    Clock::time_point nextPoll{};
};

SceneNetwork::SceneNetwork(ecs::World& world, scripting::ScriptingSystem& scripts,
                           std::function<void(ecs::EntityId)> destroy)
    : m_world(world), m_scripts(scripts), m_destroy(std::move(destroy)) {}

SceneNetwork::~SceneNetwork() {
    m_emitPeerEvents = false;
    m_steam.reset();
    if (m_server) m_server->stop();
}

bool SceneNetwork::start(const core::PlayLaunch& launch, std::string& error) {
    m_options = launch.networkOptions;
    m_projectPlayerLimit = launch.project.networkPlayerLimit;
    if (!m_options.valid()) {
        error = "Invalid Network Test options.";
        return false;
    }
    m_conditions = m_options.conditions();
    if (launch.networkSteam) {
        if (!launch.steamRuntime) { error = "Steam runtime was not initialized."; return false; }
        m_steam = std::make_unique<SceneSteam>(m_scripts, *this, *launch.steamRuntime, launch.networkCompatibility);
    }
    if (!launch.networkControlDirectory.empty()) {
        const auto count = static_cast<unsigned>(m_options.clientCount);
        const auto index = launch.networkClientIndex;
        if (launch.networkRole == core::SceneNetworkRole::Client && (index == 0 || index > count)) {
            error = "Invalid Network Test client index.";
            return false;
        }
        auto profiles = core::readNetworkTestProfiles(launch.networkControlDirectory, count);
        if (!profiles) {
            error = "Could not read the live Network Test profiles.";
            return false;
        }
        m_liveControl = std::make_unique<LiveControl>();
        m_liveControl->directory = launch.networkControlDirectory;
        m_liveControl->serverEndpoint = launch.networkEndpoint;
        m_liveControl->clientIndex = index;
        m_liveControl->profiles = std::move(*profiles);
        m_liveControl->serverPeerProfiles.resize(count);
        m_liveControl->publishedServerProfiles.resize(count);
        if (launch.networkRole == core::SceneNetworkRole::Client) {
            m_conditions = m_liveControl->profiles[index - 1].conditions;
        }
    }
    const bool remote = launch.networkMaxPeers != 0;
    if (remote && !launch.networkSteam && !launch.networkHostControlDirectory.empty() &&
        (launch.networkRole == core::SceneNetworkRole::Server || (launch.networkEndpoint.address >> 24) == 127)) {
        m_hostControl = std::make_unique<HostControl>(launch.networkHostControlDirectory / "owner-peer");
    }
    m_clientCapacity = remote ? launch.networkMaxPeers : m_options.clientCount;
    if (m_clientCapacity > net::MaxPeers) {
        error = "Invalid scene player limit.";
        return false;
    }
    m_bindingLimit = net::MaxReplicas - (remote && launch.networkRole == core::SceneNetworkRole::Client
        ? 1U : m_clientCapacity);
    if (launch.networkRole == core::SceneNetworkRole::Server) {
        m_server = std::make_unique<net::NetworkServer>(static_cast<net::ServerGame&>(*this), noPlayerSimulation);
        net::ServerConfig config;
        config.bind = launch.networkEndpoint;
        config.maxPeers = static_cast<uint8_t>(m_clientCapacity);
        config.compatibility = launch.networkCompatibility;
        config.deferSnapshots = true;
        if (!m_server->start(config, error)) return false;
        m_server->setEmulation(m_conditions.emulation());
    } else if (launch.networkRole == core::SceneNetworkRole::Client) {
        m_client = std::make_unique<net::NetworkClient>(noPlayerSimulation);
        m_client->setPrediction(false);
        m_client->setEmulation(m_conditions.emulation(), false);
        if (remote) {
            m_remoteClient = std::make_unique<RemoteClient>();
            m_remoteClient->endpoint = launch.networkEndpoint;
            m_remoteClient->compatibility = launch.networkCompatibility;
        }
    } else {
        error = "Missing network scene role.";
        return false;
    }

    m_world.forEach<net::NetworkReplica>([&](ecs::EntityId entity, const net::NetworkReplica& replica) {
        if (error.empty() && !bind(entity, replica.key)) {
            error = "Cannot bind Network Replica key " + std::to_string(replica.key) +
                ". Check for duplicate keys, an invalid Transform or too many replicas.";
        }
    });
    if (!error.empty()) return false;
    const bool waitsForInvite = launch.networkSteam && !launch.networkEndpoint.steamId;
    if (m_client && !waitsForInvite &&
        !m_client->connect(launch.networkEndpoint, error, launch.networkCompatibility)) return false;
    if (remote) {
        ENGINE_LOG_INFO("Network scene session: role={} endpoint={}", role(),
            waitsForInvite ? "Waiting for a Steam invitation" : net::formatEndpoint(launch.networkEndpoint));
    } else {
        publishLiveClientStatus();
        ENGINE_LOG_INFO("Network Test settings: role={} clients={} one_way_latency_ms={} packet_loss_percent={}",
            role(), m_options.clientCount, m_conditions.latencyMs, m_conditions.lossPercent);
    }
    return true;
}

void SceneNetwork::beginTick() {
    for (const auto entity : std::exchange(m_pendingDespawn, {})) {
        if (m_world.isAlive(entity)) m_destroy(entity);
    }
    if (m_steam) m_steam->tick();
    pollLiveControl();
    if (m_server) {
        pollHostControl();
        m_server->tick();
        pollSteamNames();
        if (m_localPlayer.id) {
            m_peerInputs[m_localPlayer.id] = {};
            for (const auto& [request, command] : std::exchange(m_localRpcs, {})) {
                const auto status = rpc(m_localPlayer, command);
                m_scripts.queueEvent("networkRpcResult", {{"request", request}, {"status", net::rpcStatusName(status)},
                    {"value", m_localPlayer.value}, {"accepted", status == net::RpcStatus::Accepted}});
            }
            m_server->setReplica(m_localPlayer);
        }
        publishLiveTelemetry();
        return;
    }
    if (!m_client) return;
    m_client->tick(m_input);
    m_input = {};
    publishLiveClientStatus();
    publishLiveTelemetry();
    const auto phase = m_client->phase();
    if (phase != m_lastClientPhase) {
        ENGINE_LOG_INFO("Network scene client: {} (peer {})", m_client->diagnostic(), m_client->localEntity());
        m_lastClientPhase = phase;
        m_nextLinkLog = {};
    }
    if (phase == net::ClientPhase::Connected && m_remoteClient && Clock::now() >= m_nextLinkLog) {
        const auto stats = m_client->stats();
        if (stats.recentRttMs) {
            const auto local = m_client->latestReplica(m_client->localEntity());
            const auto next = m_client->nextInputSequence();
            const auto pending = local && local->inputAck < next ? next - 1 - local->inputAck : 0;
            ENGINE_LOG_INFO("Network scene link: transport={} rtt_ms={} probe_losses={}/{} pending_inputs={} snapshots={} invalid={} server_tick={} input_ack={} receive_drops={} send_drops={}",
                m_steam ? "Steam" : "UDP", *stats.recentRttMs, stats.probes.dropped, stats.probes.packets,
                pending, stats.snapshotsReceived, stats.invalidMessages, local ? local->tick : 0,
                local ? local->inputAck : 0, stats.transport.receiveQueueDrops, stats.transport.sendQueueDrops);
            m_nextLinkLog = Clock::now() + 5s;
        }
    }
    sampleClient();
    applyClientPoses();
    net::RpcResult result;
    while (m_client->takeResult(result)) {
        m_scripts.queueEvent("networkRpcResult", {
            {"request", result.request}, {"status", net::rpcStatusName(result.status)}, {"value", result.value},
            {"accepted", result.status == net::RpcStatus::Accepted}
        });
    }
    pollRemoteClient();
    pollHostControl();
}

void SceneNetwork::pollSteamNames() {
    if (!m_steam) return;
    const auto now = Clock::now();
    if (now < m_nextSteamNamePoll) return;
    m_nextSteamNamePoll = now + 1s;
    const auto publish = [this](net::EntityId peer, std::string_view persona) {
        if (const auto name = net::normalizePeerName(persona, true); name && !name->empty()) {
            m_server->setPeerName(peer, *name);
        }
    };
    if (m_localPlayer.id) publish(m_localPlayer.id, m_steam->localName());
    for (const auto& [peer, input] : m_peerInputs) {
        if (peer == m_localPlayer.id || !m_server->peerName(peer).empty()) continue;
        if (const auto steamId = m_server->peerSteamId(peer)) {
            publish(peer, m_steam->userName(steamId));
        }
    }
}

void SceneNetwork::pollHostControl() {
    if (!m_hostControl) return;
    auto& control = *m_hostControl;
    const auto now = Clock::now();
    if (now < control.nextPoll) return;
    control.nextPoll = now + 100ms;
    if (m_server) {
        m_hostNonce = readHostNonce(control.file);
    } else if (m_client) {
        const auto nonce = m_client->connectionNonce();
        if (nonce == m_hostNonce) return;
        std::string error;
        if (core::writeFileAtomically(control.file, std::to_string(nonce), error)) m_hostNonce = nonce;
    }
}

void SceneNetwork::pollRemoteClient() {
    if (!m_remoteClient) return;
    auto& remote = *m_remoteClient;
    if (!remote.endpoint.address && !remote.endpoint.steamId) return;
    const auto now = Clock::now();
    const auto phase = m_client->phase();
    if (phase == net::ClientPhase::Connected) {
        remote.retryAt = {};
        remote.retryDelay = 1s;
        remote.error.clear();
    } else if (phase == net::ClientPhase::TimedOut || phase == net::ClientPhase::Disconnected) {
        if (remote.retryAt == Clock::time_point{}) {
            remote.retryAt = now + remote.retryDelay;
        } else if (now >= remote.retryAt) {
            remote.retryDelay = std::min(2 * remote.retryDelay, 8s);
            remote.retryAt = {};
            if (!m_client->connect(remote.endpoint, remote.error, remote.compatibility)) {
                ENGINE_LOG_WARN("Could not reconnect to {}: {}", net::formatEndpoint(remote.endpoint), remote.error);
            }
        }
    }
}

bool SceneNetwork::canHostSteam() const {
    return m_server || (m_client && m_remoteClient && !m_remoteClient->endpoint.steamId);
}

bool SceneNetwork::startSteamAuthority(uint64_t hostId, uint64_t lobbyId, unsigned maxPlayers, std::string& error) {
    if (!m_steam || !hostId || !lobbyId || maxPlayers < 2 || maxPlayers > net::MaxPeers) {
        error = "Invalid Steam host settings.";
        return false;
    }
    if (m_projectPlayerLimit && maxPlayers > m_projectPlayerLimit) {
        error = "The lobby exceeds this project's player limit.";
        return false;
    }
    const auto remoteSlots = maxPlayers - 1;
    if (m_server) {
        if (remoteSlots > m_clientCapacity) { error = "Lobby exceeds the server capacity."; return false; }
        m_server->setSteamLobby(lobbyId);
        return true;
    }
    if (!m_client || !m_remoteClient || m_remoteClient->endpoint.steamId) {
        error = "Leave the connected game before hosting a Steam lobby.";
        return false;
    }
    const auto replicaLimit = net::MaxReplicas - maxPlayers;
    if (m_bindings.size() > replicaLimit) { error = "The player limit leaves too few scene replica slots."; return false; }
    auto server = std::make_unique<net::NetworkServer>(static_cast<net::ServerGame&>(*this), noPlayerSimulation);
    net::ServerConfig config;
    config.bind = {0, net::SteamVirtualPort, hostId};
    config.maxPeers = static_cast<uint8_t>(remoteSlots);
    config.compatibility = m_remoteClient->compatibility;
    config.deferSnapshots = true;
    if (!server->start(config, error)) return false;
    server->setSteamLobby(lobbyId);
    const auto local = server->createReplica({});
    if (!local || !server->setLocalSteamIdentity(local)) {
        error = "Could not reserve the local host player.";
        return false;
    }
    for (const auto& [key, binding] : m_bindings) {
        if (!m_world.isAlive(binding.entity) && !binding.prefab.empty()) continue;
        const auto snapshot = capture(key, binding);
        if (!snapshot || !server->createKeyedReplica(key, *snapshot)) {
            error = "Could not bind a Steam scene replica.";
            return false;
        }
    }
    m_client.reset();
    m_server = std::move(server);
    m_localPlayer.id = local;
    m_clientCapacity = remoteSlots;
    m_bindingLimit = replicaLimit;
    m_peerInputs.emplace(local, net::PlayerInput{});
    m_nextSteamNamePoll = {};
    pollSteamNames();
    return true;
}

bool SceneNetwork::connectSteamHost(uint64_t hostId, std::string& error) {
    if (!m_steam || !m_remoteClient || !m_client || !net::validSteamUserId(hostId)) {
        error = "A Steam client must be running to join a host.";
        return false;
    }
    auto& remote = *m_remoteClient;
    const net::Endpoint endpoint{0, net::SteamVirtualPort, hostId};
    if (!m_client->connect(endpoint, error, remote.compatibility)) return false;
    remote.endpoint = endpoint;
    remote.retryAt = {};
    remote.retryDelay = 1s;
    remote.error.clear();
    return true;
}

void SceneNetwork::leaveSteamSession(bool notify) {
    if (m_localPlayer.id) {
        const auto peer = m_localPlayer.id;
        m_server->stop();
        m_server.reset();
        m_peerInputs.clear();
        m_localPlayer = {};
        m_localRpcs.clear();
        m_client = std::make_unique<net::NetworkClient>(noPlayerSimulation);
        m_client->setPrediction(false);
        m_clientCapacity = static_cast<unsigned>(net::MaxPeers);
        m_bindingLimit = net::MaxReplicas - 1;
        if (notify) {
            m_scripts.queueEvent("networkPeerLeft", {{"peer", peer}});
            m_scripts.queueEvent("networkRoleChanged", {{"role", "client"}});
        }
    } else if (m_server) {
        m_server->setSteamLobby(0);
    }
    if (!m_client || !m_remoteClient) return;
    m_client->disconnect();
    m_remoteClient->endpoint = {0, net::SteamVirtualPort, 0};
    m_remoteClient->retryAt = {};
    m_remoteClient->error.clear();
}

void SceneNetwork::pollLiveControl() {
    if (!m_liveControl) return;
    auto& control = *m_liveControl;
    const auto now = Clock::now();
    if (now < control.nextPoll) return;
    control.nextPoll = now + 100ms;
    const auto count = static_cast<unsigned>(m_options.clientCount);
    if (auto profiles = core::readNetworkTestProfiles(control.directory, count)) {
        control.profiles = std::move(*profiles);
    }

    if (m_client) {
        const auto conditions = control.profiles[control.clientIndex - 1].conditions;
        if (conditions != m_conditions) {
            m_client->setEmulation(conditions.emulation(), false);
            m_client->resetProbeMeasurements();
            m_conditions = conditions;
            ENGINE_LOG_INFO("Network Test link updated: client={} one_way_latency_ms={} packet_loss_percent={}",
                control.clientIndex, conditions.latencyMs, conditions.lossPercent);
        }
        if (m_client->phase() == net::ClientPhase::TimedOut &&
            m_conditions.lossPercent < net::MaxNetworkTestLossPercent && now >= control.nextReconnect) {
            control.nextReconnect = now + 1s;
            std::string error;
            if (!m_client->connect(control.serverEndpoint, error)) {
                ENGINE_LOG_WARN("Network Test client {} could not reconnect: {}", control.clientIndex, error);
            }
        }
        return;
    }

    std::vector<std::optional<net::NetworkTestAppliedProfile>> applied(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto client = core::readNetworkTestClientStatus(control.directory, i + 1);
        const auto& profile = control.profiles[i];
        if (i == 0) m_hostNonce = client ? client->nonce : 0;
        if (client && client->nonce && m_server->setPeerEmulation(client->nonce, profile.conditions.emulation())) {
            applied[i] = net::NetworkTestAppliedProfile{client->nonce, profile.revision};
        }
    }
    control.serverPeerProfiles = applied;
    if (applied == control.publishedServerProfiles || now < control.nextStatusWrite) return;
    std::string error;
    if (core::writeNetworkTestAppliedProfiles(control.directory, applied, error)) {
        for (unsigned i = 0; i < count; ++i) {
            if (applied[i] && applied[i] != control.publishedServerProfiles[i]) {
                const auto conditions = control.profiles[i].conditions;
                ENGINE_LOG_INFO("Network Test server link updated: client={} one_way_latency_ms={} packet_loss_percent={}",
                    i + 1, conditions.latencyMs, conditions.lossPercent);
            }
        }
        control.publishedServerProfiles = std::move(applied);
    } else {
        control.nextStatusWrite = now + 1s;
        ENGINE_LOG_WARN("Could not report Network Test server profiles: {}", error);
    }
}

void SceneNetwork::publishLiveClientStatus() {
    if (!m_liveControl || !m_client) return;
    auto& control = *m_liveControl;
    const auto now = Clock::now();
    if (now < control.nextStatusWrite) return;
    const net::NetworkTestClientStatus status{m_client->connectionNonce(),
        control.profiles[control.clientIndex - 1].revision, linkState(m_client->phase())};
    if (control.publishedClientStatus == status) return;
    std::string error;
    if (core::writeNetworkTestClientStatus(control.directory, control.clientIndex, status, error)) {
        control.publishedClientStatus = status;
    } else {
        control.nextStatusWrite = now + 1s;
        ENGINE_LOG_WARN("Could not report Network Test client {} profile: {}", control.clientIndex, error);
    }
}

void SceneNetwork::publishLiveTelemetry() {
    if (!m_liveControl) return;
    auto& control = *m_liveControl;
    if (m_client) {
        if (const auto nonce = m_client->connectionNonce()) {
            control.telemetryNonce = nonce;
            control.telemetryRevision = control.profiles[control.clientIndex - 1].revision;
        }
    }
    const auto now = Clock::now();
    if (now < control.nextTelemetry) return;
    control.nextTelemetry = now + 250ms;
    const auto elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - control.telemetryStart).count());
    std::string error;

    if (m_client) {
        if (!control.telemetryNonce ||
            control.telemetryRevision != control.profiles[control.clientIndex - 1].revision) return;
        const auto stats = m_client->stats();
        const auto& transport = stats.transport;
        const net::NetworkTestTelemetrySample sample{control.telemetryNonce, control.telemetryRevision, elapsedMs,
            stats.recentRttMs, transport.txPackets, transport.rxPackets, transport.txBytes, transport.rxBytes,
            {transport.emulatedPackets, transport.emulatedDrops}, stats.probes};
        if (core::writeNetworkTestClientTelemetry(control.directory, control.clientIndex, sample, error)) return;
    } else {
        std::vector<std::optional<net::NetworkTestTelemetrySample>> samples(control.serverPeerProfiles.size());
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto& profile = control.serverPeerProfiles[i];
            if (!profile) continue;
            const auto counters = m_server->peerEmulationCounters(profile->nonce);
            if (!counters) continue;
            net::NetworkTestTelemetrySample sample{};
            sample.nonce = profile->nonce;
            sample.revision = profile->revision;
            sample.elapsedMs = elapsedMs;
            sample.outbound = *counters;
            samples[i] = sample;
        }
        if (core::writeNetworkTestServerTelemetry(control.directory, samples, error)) return;
    }
    if (now >= control.nextTelemetryWarning) {
        control.nextTelemetryWarning = now + 1s;
        ENGINE_LOG_WARN("Could not publish Network Test telemetry: {}", error);
    }
}

void SceneNetwork::endTick() {
    if (m_server) {
        publishServerPoses();
        m_server->publishSnapshots();
    } else if (m_client) applyClientPoses();
}

uint16_t SceneNetwork::localPort() const {
    return m_server ? m_server->localEndpoint().port : 0;
}

bool SceneNetwork::isClient() const { return m_client != nullptr; }
bool SceneNetwork::acceptsLocalInput() const { return m_client || m_localPlayer.id; }

scripting::ScriptSteam* SceneNetwork::steam() const { return m_steam.get(); }

std::filesystem::path SceneNetwork::steamAvatar(std::string_view image) {
    return m_steam ? m_steam->avatarPath(image) : std::filesystem::path{};
}

std::string_view SceneNetwork::connectionNotice() const {
    if (!m_remoteClient || connected()) return {};
    if (!m_remoteClient->error.empty()) return m_remoteClient->error;
    if (m_steam && !m_remoteClient->endpoint.steamId) return "Ready to host or join a Steam session";
    return m_client->diagnostic();
}

std::string_view SceneNetwork::role() const {
    if (m_server) return "server";
    if (m_client) return "client";
    return "offline";
}

bool SceneNetwork::connected() const {
    return m_server || (m_client && m_client->phase() == net::ClientPhase::Connected);
}

uint32_t SceneNetwork::localPeer() const { return m_client ? m_client->localEntity() : m_localPlayer.id; }

bool SceneNetwork::usesSteam() const { return m_steam != nullptr; }

bool SceneNetwork::isHost(uint32_t peer) const {
    if (m_server) {
        if (m_localPlayer.id) return !peer || peer == m_localPlayer.id;
        return peer && peer == m_server->loopbackPeerForNonce(m_hostNonce);
    }
    if (peer || !m_client || !connected()) return false;
    const auto nonce = m_client->connectionNonce();
    if (m_hostControl) return nonce && nonce == m_hostNonce;
    return m_liveControl && m_liveControl->clientIndex == 1 && m_liveControl->publishedClientStatus &&
        nonce && nonce == m_liveControl->publishedClientStatus->nonce;
}

std::vector<uint32_t> SceneNetwork::peers() const {
    if (!m_server) return m_clientPeers;
    std::vector<uint32_t> result;
    result.reserve(m_peerInputs.size());
    for (const auto& peer : m_peerInputs) result.push_back(peer.first);
    return result;
}

nlohmann::json SceneNetwork::stats() const {
    const auto data = m_server ? m_server->stats() : m_client ? m_client->stats() : net::NetworkStats{};
    const auto& transport = data.transport;
    return {{"rttMs", data.rttMs}, {"txPackets", transport.txPackets}, {"rxPackets", transport.rxPackets},
        {"txBytes", transport.txBytes}, {"rxBytes", transport.rxBytes}, {"snapshotsSent", data.snapshotsSent},
        {"snapshotsReceived", data.snapshotsReceived}, {"snapshotBytes", data.snapshotBytes},
        {"fullSnapshotBytes", data.fullSnapshotBytes}, {"invalidMessages", data.invalidMessages},
        {"clientCount", m_clientCapacity}, {"latencyMs", m_conditions.latencyMs},
        {"lossPercent", m_conditions.lossPercent}, {"emulatedDrops", transport.emulatedDrops},
        {"diagnostic", m_client ? (m_remoteClient && !connected() ? connectionNotice() : m_client->diagnostic())
                               : (m_server ? "Listening" : "Offline")}};
}

bool SceneNetwork::bind(ecs::EntityId id, uint32_t key) {
    if (!net::keyedReplicaId(key) || !m_world.hasComponent<ecs::Transform>(id) || (!m_server && !m_client)) {
        return false;
    }
    const auto existing = m_bindings.find(key);
    const bool reuse = m_server && existing != m_bindings.end() && existing->second.entity == ecs::kInvalidEntity &&
        !existing->second.prefab.empty();
    if (existing != m_bindings.end() && !reuse) return existing->second.entity == id;
    if ((!reuse && m_bindings.size() >= m_bindingLimit) || std::ranges::any_of(m_bindings,
            [id](const auto& entry) { return entry.second.entity == id; })) return false;
    Binding binding;
    binding.entity = id;
    if (m_server) {
        const auto snapshot = capture(key, binding);
        if (!snapshot || !m_server->createKeyedReplica(key, *snapshot)) return false;
    }
    m_bindings.insert_or_assign(key, std::move(binding));
    return true;
}

bool SceneNetwork::unbind(uint32_t key) {
    const auto found = m_bindings.find(key);
    if (found == m_bindings.end()) return false;
    auto& binding = found->second;
    if (m_server) m_server->destroyReplica(net::keyedReplicaId(key));
    if (!binding.prefab.empty() && m_world.isAlive(binding.entity)) m_pendingDespawn.push_back(binding.entity);
    restoreVisibility(binding);
    m_bindings.erase(found);
    return true;
}

bool SceneNetwork::registerPrefab(uint32_t key, std::string_view path) {
    if (!m_client || !net::keyedReplicaId(key) || path.empty() || path.size() > 1024 ||
        path.find('\0') != std::string_view::npos) return false;
    if (const auto it = m_bindings.find(key); it != m_bindings.end()) return it->second.prefab == path;
    if (m_bindings.size() >= m_bindingLimit) return false;
    m_bindings[key].prefab = path;
    return true;
}

ecs::EntityId SceneNetwork::entity(uint32_t key) const {
    const auto it = m_bindings.find(key);
    return it != m_bindings.end() && m_world.isAlive(it->second.entity) ? it->second.entity : ecs::kInvalidEntity;
}

bool SceneNetwork::setState(uint32_t key, uint16_t value, uint8_t flags) {
    if (!m_server || !entity(key)) return false;
    auto& binding = m_bindings.at(key);
    binding.value = value;
    binding.flags = flags;
    return true;
}

std::optional<scripting::ScriptReplicaState> SceneNetwork::state(uint32_t key) const {
    const auto it = m_bindings.find(key);
    if (it == m_bindings.end()) return {};
    if (m_client) return it->second.received;
    const auto captured = capture(key, it->second);
    if (!captured) return {};
    return toScriptState(*captured);
}

std::optional<scripting::ScriptReplicaState> SceneNetwork::rawState(uint32_t key) const {
    if (!m_client || !m_bindings.contains(key)) return {};
    const auto replica = m_client->latestReplica(net::keyedReplicaId(key));
    return replica ? std::optional{toScriptState(replica->entity)} : std::nullopt;
}

std::optional<scripting::ScriptPeerMotion> SceneNetwork::localMotion() const {
    if (!m_client) return {};
    const auto replica = m_client->latestReplica(m_client->localEntity());
    if (!replica || !(replica->entity.flags & net::PeerMotionValid)) return {};
    const auto& data = replica->entity;
    return scripting::ScriptPeerMotion{data.x * 0.001F, data.y * 0.001F, data.z * 0.001F,
        data.yaw * 0.001F, data.pitch * 0.001F, replica->tick, replica->inputAck};
}

uint64_t SceneNetwork::nextInputSequence() const { return m_client ? m_client->nextInputSequence() : 0; }

bool SceneNetwork::setPeerMotion(uint32_t peer, scripting::ScriptPeerMotion motion) {
    if (!m_server || !std::isfinite(motion.x) || !std::isfinite(motion.y) || !std::isfinite(motion.z) ||
        !(motion.jumpBuffer >= 0 && motion.jumpBuffer <= 65.535F) ||
        !(motion.coyote >= 0 && motion.coyote <= 65.535F)) return false;
    return m_server->setPeerMotion(peer, millimeters(motion.x), millimeters(motion.y), millimeters(motion.z),
        static_cast<uint16_t>(millimeters(motion.jumpBuffer)), static_cast<uint16_t>(millimeters(motion.coyote)));
}

bool SceneNetwork::setInput(float x, float z) {
    if (!acceptsLocalInput() || !connected() || !std::isfinite(x) || !std::isfinite(z)) return false;
    if (m_localPlayer.id) m_peerInputs[m_localPlayer.id] = {inputAxis(x), inputAxis(z)};
    else m_input = {inputAxis(x), inputAxis(z)};
    return true;
}

std::optional<std::array<float, 2>> SceneNetwork::input(uint32_t peer) const {
    const auto it = m_peerInputs.find(peer);
    if (it == m_peerInputs.end()) return {};
    return std::array{it->second.x / float(net::MaxControlAxis), it->second.z / float(net::MaxControlAxis)};
}

bool SceneNetwork::setControls(scripting::ScriptPlayerControls controls) {
    if (!acceptsLocalInput() || !connected() || !std::isfinite(controls.x) || !std::isfinite(controls.z) ||
        !std::isfinite(controls.yaw) || !std::isfinite(controls.pitch) ||
        std::abs(controls.pitch) > std::numbers::pi_v<float> * 0.5F) return false;
    const auto size = std::max(1.0F, std::hypot(controls.x, controls.z));
    const net::PlayerInput input{inputAxis(controls.x / size), inputAxis(controls.z / size), angle(controls.yaw),
        static_cast<int16_t>(std::lround(controls.pitch / RadiansPerUnit)), controls.buttons, controls.pressed, true};
    auto& pending = m_localPlayer.id ? m_peerInputs[m_localPlayer.id] : m_input;
    const auto pressed = static_cast<uint8_t>(pending.pressed | input.pressed);
    pending = input;
    pending.pressed = pressed;
    return true;
}

std::optional<scripting::ScriptPlayerControls> SceneNetwork::controls(uint32_t peer) const {
    const auto it = m_peerInputs.find(peer);
    if (it == m_peerInputs.end() || !it->second.hasControls) return {};
    const auto& input = it->second;
    const auto size = std::max(float(net::MaxControlAxis), std::hypot(float(input.x), float(input.z)));
    return scripting::ScriptPlayerControls{input.x / size, input.z / size,
        float(std::remainder(input.yaw * RadiansPerUnit, 2 * std::numbers::pi)),
        float(input.pitch * RadiansPerUnit), input.buttons, input.pressed};
}

bool SceneNetwork::setPeerValue(uint32_t peer, uint16_t value) {
    if (!m_server || !peer) return false;
    if (peer != m_localPlayer.id) return m_server->setPeerValue(peer, value);
    m_localPlayer.value = value;
    return m_server->setReplica(m_localPlayer);
}

std::optional<uint16_t> SceneNetwork::peerValue(uint32_t peer) const {
    if (!peer) return {};
    if (m_client) return m_client->peerValue(peer);
    if (!m_server) return {};
    if (peer == m_localPlayer.id) return m_localPlayer.value;
    return m_server->peerValue(peer);
}

bool SceneNetwork::setPeerName(uint32_t peer, std::string_view name) {
    return m_server && !m_steam && m_peerInputs.contains(peer) && m_server->setPeerName(peer, name);
}

std::string_view SceneNetwork::peerName(uint32_t peer) const {
    if (!peer || !connected()) return {};
    const auto name = m_client ? m_client->peerName(peer) : m_server->peerName(peer);
    if (!name.empty() || !m_steam || peer != localPeer()) return name;
    return net::normalizePeerName(m_steam->localName(), true).value_or(std::string_view{});
}

std::string SceneNetwork::peerAvatar(uint32_t peer) const {
    if (!peer || !m_steam || !connected()) return {};
    if (m_server) return m_steam->peerAvatar(m_server->peerSteamId(peer));
    if (std::ranges::find(m_clientPeers, peer) == m_clientPeers.end()) return {};
    return m_steam->peerAvatar(m_client->peerSteamId(peer));
}

bool SceneNetwork::knowsSteamPeer(uint64_t steamId) const {
    if (!steamId || !m_steam || !connected()) return false;
    if (m_server) {
        return std::ranges::any_of(m_peerInputs, [&](const auto& peer) {
            return m_server->peerSteamId(peer.first) == steamId;
        });
    }
    return std::ranges::any_of(m_clientPeers, [&](const auto peer) {
        return m_client->peerSteamId(peer) == steamId;
    });
}

uint32_t SceneNetwork::request(uint32_t key, uint8_t action, int32_t argument, std::string_view text) {
    const auto id = net::keyedReplicaId(key);
    if (!id || text.size() > net::MaxPeerNameBytes) return 0;
    if (m_client) return m_client->call({action, id, argument, std::string(text)});
    if (!m_localPlayer.id || m_localRpcs.size() >= 16) return 0;
    if (++m_nextLocalRpc == 0) ++m_nextLocalRpc;
    m_localRpcs.emplace_back(m_nextLocalRpc, net::RpcCommand{action, id, argument, std::string(text)});
    return m_nextLocalRpc;
}

net::EntityState SceneNetwork::spawn(net::EntityId id, size_t) {
    m_peerInputs.emplace(id, net::PlayerInput{});
    ENGINE_LOG_INFO("Network scene peer joined: {}", id);
    m_scripts.queueEvent("networkPeerJoined", {{"peer", id}});
    return {.id = id};
}

net::RpcStatus SceneNetwork::rpc(net::EntityState& player, const net::RpcCommand& command) {
    const auto target = entity(net::replicaKey(command.target));
    if (!target) return net::RpcStatus::Forbidden;
    const auto reply = m_scripts.invokeNetworkRpc(target, player.id, command.action, command.argument, command.text);
    if (!reply.handled) return net::RpcStatus::UnknownAction;
    if (!reply.accepted) return net::RpcStatus::Forbidden;
    player.value = reply.value;
    return net::RpcStatus::Accepted;
}

void SceneNetwork::despawn(net::EntityId id) {
    m_peerInputs.erase(id);
    if (!m_emitPeerEvents) return;
    ENGINE_LOG_INFO("Network scene peer left: {}", id);
    m_scripts.queueEvent("networkPeerLeft", {{"peer", id}});
}

void SceneNetwork::input(net::EntityId id, net::PlayerInput input) {
    m_peerInputs.at(id) = input;
}

void SceneNetwork::restoreVisibility(Binding& binding) {
    if (binding.savedVisibility) {
        if (auto* mesh = m_world.getComponent<ecs::MeshRenderer>(binding.entity)) mesh->visible = *binding.savedVisibility;
        binding.savedVisibility.reset();
    }
}

void SceneNetwork::sampleClient() {
    const auto presentation = m_client->presentation();
    m_clientPeers.clear();
    for (const auto& snapshot : presentation) {
        if (!net::replicaKey(snapshot.id)) m_clientPeers.push_back(snapshot.id);
    }
    std::vector<ecs::EntityId> retired;
    for (auto& [key, binding] : m_bindings) {
        const auto replica = std::ranges::find(presentation, net::keyedReplicaId(key), &net::VisualEntity::id);
        if (replica != presentation.end()) {
            binding.received = toScriptState(*replica);
            restoreVisibility(binding);
            if (!m_world.isAlive(binding.entity) && !binding.prefab.empty() && !binding.spawnFailed) {
                binding.entity = m_scripts.spawnPrefab(binding.prefab);
                binding.spawnFailed = binding.entity == ecs::kInvalidEntity;
            }
            continue;
        }
        if (!binding.received) continue;
        binding.received.reset();
        binding.spawnFailed = false;
        if (!binding.prefab.empty()) {
            if (m_world.isAlive(binding.entity)) retired.push_back(binding.entity);
            binding.entity = ecs::kInvalidEntity;
        } else if (auto* mesh = m_world.getComponent<ecs::MeshRenderer>(binding.entity)) {
            binding.savedVisibility = mesh->visible;
            mesh->visible = false;
        }
    }
    for (const auto entity : retired) m_destroy(entity);
}

void SceneNetwork::applyClientPoses() {
    for (const auto& [key, binding] : m_bindings) {
        (void)key;
        if (!binding.received) continue;
        auto* transform = m_world.getComponent<ecs::Transform>(binding.entity);
        if (!transform) continue;
        const auto& data = *binding.received;
        transform->position = {data.x, data.y, data.z};
        transform->rotationEulerRadians = {data.pitch, data.yaw, data.roll};
    }
}

std::optional<net::EntityState> SceneNetwork::capture(uint32_t key, const Binding& binding) const {
    const auto* transform = m_world.getComponent<ecs::Transform>(binding.entity);
    if (!transform) return {};
    const auto& p = transform->position;
    const auto& r = transform->rotationEulerRadians;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        !std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)) return {};
    return net::EntityState{net::keyedReplicaId(key), millimeters(p.x), millimeters(p.y), millimeters(p.z),
        angle(r.y), binding.value, binding.flags, angle(r.x), angle(r.z)};
}

void SceneNetwork::publishServerPoses() {
    for (auto it = m_bindings.begin(); it != m_bindings.end();) {
        const auto snapshot = capture(it->first, it->second);
        if (snapshot) {
            m_server->setReplica(*snapshot);
            ++it;
        } else {
            m_server->destroyReplica(net::keyedReplicaId(it->first));
            it = m_bindings.erase(it);
        }
    }
}

}
