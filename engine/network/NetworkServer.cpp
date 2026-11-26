#include "engine/network/NetworkProtocol.h"

#include <cmath>
#include <deque>
#include <random>
#include <utility>

namespace engine::net {
namespace {

constexpr uint64_t TimeoutTicks = 5 * TickRate;
constexpr uint64_t ChallengeTicks = 3 * TickRate;
constexpr size_t SnapshotHistory = 64;
constexpr size_t InputWindow = 512;
constexpr uint64_t BufferedControlLead = 3;
constexpr size_t RpcWindow = 64;
constexpr size_t MaxChallenges = 64;
constexpr std::ptrdiff_t MaxChallengesPerAddress = 16;
constexpr size_t ReceiveBudget = 256;
constexpr auto SessionControlPolicy = EmulationPolicy::Bypass;

struct InputSlot {
    uint64_t sequence{};
    wire::InputSample sample{};
};

bool canCoalesceControls(const wire::InputSample& a, const wire::InputSample& b,
                         const std::array<uint16_t, ControlActionCount>& appliedActions) {
    const auto& left = a.input;
    const auto& right = b.input;
    if (left.hasControls != right.hasControls || left.x != right.x || left.z != right.z) return false;
    return !left.hasControls || (left.buttons == right.buttons && a.actionCounts == b.actionCounts &&
        ((left.yaw == right.yaw && left.pitch == right.pitch) || a.actionCounts == appliedActions));
}

struct Challenge {
    Endpoint endpoint;
    uint64_t nonce;
    uint64_t token;
    uint64_t deadline;
};

struct Departed {
    Endpoint endpoint;
    uint64_t token;
};

struct Peer {
    Endpoint endpoint{};
    uint64_t nonce{};
    std::optional<NetworkEmulation> emulation;
    NetworkLossCounters emulationCounters{};
    uint64_t token{};
    uint64_t lastHeard{};
    size_t slot{};
    EntityState player{};
    std::string name;
    std::deque<wire::Snapshot> snapshots;
    uint64_t snapshotAck{};
    std::array<InputSlot, InputWindow> inputs{};
    uint64_t highestInput{};
    uint64_t inputAck{};
    unsigned gapTicks{};
    PlayerInput heldInputGrace{};
    std::array<uint16_t, ControlActionCount> actionCounts{};
    std::array<RpcResult, RpcWindow> results{};
    uint32_t highestRpc{};
    uint64_t nextRpcTick{};
};

template<class Peers>
auto findPeerByNonce(Peers& peers, uint64_t nonce) {
    if (!nonce) return peers.end();
    const auto found = std::ranges::find(peers, nonce, &Peer::nonce);
    if (found == peers.end() || std::ranges::find(found + 1, peers.end(), nonce, &Peer::nonce) != peers.end()) {
        return peers.end();
    }
    return found;
}

}

struct NetworkServer::State {
    State(ServerGame& gameRef, SimulatePlayer simulation) : game(gameRef), simulate(simulation) {}

    bool sendGame(Peer& peer, std::span<const uint8_t> bytes) {
        const auto result = transport.sendWithResult(peer.endpoint, bytes, EmulationPolicy::Apply, peer.emulation);
        if (result.consideredByEmulator) ++peer.emulationCounters.packets;
        if (result.emulatedDrop) ++peer.emulationCounters.dropped;
        return result.accepted;
    }

    template<class... Fields>
    void sendGame(Peer& peer, wire::Kind kind, Fields... fields) {
        wire::Writer writer(kind, peer.token);
        (writer.put(fields), ...);
        sendGame(peer, writer.finish());
    }

    uint64_t makeToken() {
        uint64_t token{};
        do {
            token = (uint64_t{random()} << 32) | random();
        } while (!token || std::ranges::any_of(peers, [token](const Peer& peer) {
            return peer.token == token;
        }) || std::ranges::any_of(challenges, [token](const Challenge& challenge) {
            return challenge.token == token;
        }));
        return token;
    }

    EntityId allocateId() {
        if (nextEntity == MaxReplicaKey) return 0;
        return ++nextEntity;
    }

    void accept(const Peer& peer) {
        wire::send(transport, peer.endpoint, SessionControlPolicy, wire::Kind::Accept, peer.token, peer.nonce,
            peer.player.id);
    }

    bool handshake(Endpoint endpoint, const wire::Header& header, wire::Reader& reader) {
        const auto nonce = reader.get<uint64_t>();
        if (!nonce) return false;
        if (header.version != wire::Version) {
            if (header.kind == wire::Kind::Hello && !header.session && reader.done()) {
                wire::send(transport, endpoint, SessionControlPolicy, wire::Kind::Reject, 0, nonce,
                    static_cast<uint8_t>(wire::Reject::Incompatible));
            }
            return false;
        }
        const auto compatibility = header.kind == wire::Kind::Confirm ? reader.get<uint64_t>() : 0;
        if (!reader.done()) return false;

        auto existing = std::ranges::find(peers, endpoint, &Peer::endpoint);
        if (header.kind == wire::Kind::Hello) {
            if (header.session) return false;
            if (existing != peers.end() && existing->nonce == nonce) {
                wire::send(transport, endpoint, SessionControlPolicy, wire::Kind::Challenge, existing->token, nonce);
                return true;
            }
            auto pending = std::ranges::find(challenges, endpoint, &Challenge::endpoint);
            if (pending == challenges.end()) {
                if (challenges.size() == MaxChallenges) return true;
                if (!endpoint.steamId && std::ranges::count_if(challenges, [&](const Challenge& challenge) {
                        return !challenge.endpoint.steamId && challenge.endpoint.address == endpoint.address;
                    }) >= MaxChallengesPerAddress) {
                    return true;
                }
                challenges.push_back({endpoint, nonce, makeToken(), now + ChallengeTicks});
                pending = std::prev(challenges.end());
            } else if (pending->nonce != nonce) {
                pending->nonce = nonce;
                pending->token = makeToken();
            }
            wire::send(transport, endpoint, SessionControlPolicy, wire::Kind::Challenge, pending->token, nonce);
            return true;
        }

        if (existing != peers.end() && existing->nonce == nonce && existing->token == header.session) {
            if (compatibility != config.compatibility) return false;
            existing->lastHeard = now;
            accept(*existing);
            return true;
        }
        const auto pending = std::ranges::find(challenges, endpoint, &Challenge::endpoint);
        if (pending == challenges.end() || pending->nonce != nonce || pending->token != header.session) {
            return false;
        }
        challenges.erase(pending);
        if (compatibility != config.compatibility) {
            wire::send(transport, endpoint, SessionControlPolicy, wire::Kind::Reject, header.session, nonce,
                static_cast<uint8_t>(wire::Reject::ContentMismatch));
            return true;
        }
        if ((existing == peers.end() && peers.size() == config.maxPeers) || nextEntity == MaxReplicaKey) {
            wire::send(transport, endpoint, SessionControlPolicy, wire::Kind::Reject, header.session, nonce,
                static_cast<uint8_t>(wire::Reject::Full));
            return true;
        }
        if (existing != peers.end()) remove(existing);
        std::array<bool, MaxPeers> used{};
        for (const auto& peer : peers) used[peer.slot] = true;
        const auto slot = static_cast<size_t>(std::ranges::find(used, false) - used.begin());
        const auto id = allocateId();
        auto& peer = peers.emplace_back();
        peer.endpoint = endpoint;
        peer.nonce = nonce;
        peer.token = header.session;
        peer.lastHeard = now;
        peer.slot = slot;
        peer.player = game.spawn(id, slot);
        peer.player.id = id;
        if (peer.endpoint.steamId) ++nameRevisions[slot / wire::PeerNamePageSize];
        accept(peer);
        return true;
    }

    bool receiveInput(Peer& peer, wire::Reader& reader) {
        const auto snapshotAck = reader.get<uint64_t>();
        const auto latest = reader.get<uint64_t>();
        const auto count = reader.get<uint8_t>();
        if (count == 0 || count > wire::MaxInputBatch || latest < count || latest == UINT64_MAX) {
            return false;
        }
        std::array<wire::InputSample, wire::MaxInputBatch> batch{};
        for (size_t i = 0; i < count; ++i) {
            if (!wire::readInput(reader, batch[i])) return false;
        }
        if (!reader.done()) return false;

        if (snapshotAck > peer.snapshotAck && std::ranges::find(
                peer.snapshots, snapshotAck, &wire::Snapshot::sequence) != peer.snapshots.end()) {
            peer.snapshotAck = snapshotAck;
        }
        if (latest > peer.inputAck && latest - peer.inputAck > InputWindow) {
            peer.inputAck = latest - count;
            peer.gapTicks = 0;
        }
        for (size_t i = 0; i < count; ++i) {
            const auto sequence = latest - count + i + 1;
            if (sequence <= peer.inputAck) continue;
            auto& slot = peer.inputs[sequence % InputWindow];
            if (slot.sequence != sequence) slot = {sequence, batch[i]};
        }
        peer.highestInput = std::max(peer.highestInput, latest);
        return true;
    }

    bool receiveRpc(Peer& peer, wire::Reader& reader) {
        const auto request = reader.get<uint32_t>();
        RpcCommand command{reader.get<uint8_t>(), reader.get<EntityId>(), reader.get<int32_t>()};
        const auto text = reader.getText();
        if (!request || !text || !reader.done() || !normalizePeerName(*text)) return false;
        command.text = *text;

        RpcResult result{request, RpcStatus::TimedOut, peer.player.value};
        auto& previous = peer.results[request % RpcWindow];
        if (previous.request == request) {
            result = previous;
        } else if (request > peer.highestRpc || peer.highestRpc - request < RpcWindow) {
            result.status = RpcStatus::RateLimited;
            if (now >= peer.nextRpcTick) {
                peer.nextRpcTick = now + TickRate / 10;
                const auto id = peer.player.id;
                result.status = game.rpc(peer.player, command);
                peer.player.id = id;
                if (result.status > RpcStatus::RateLimited) result.status = RpcStatus::InvalidArgument;
                result.value = peer.player.value;
            }
            previous = result;
            peer.highestRpc = std::max(peer.highestRpc, request);
        }
        sendGame(peer, wire::Kind::RpcResult, result.request, static_cast<uint8_t>(result.status), result.value);
        return true;
    }

    auto remove(std::vector<Peer>::iterator it) -> std::vector<Peer>::iterator {
        departed.push_back({it->endpoint, it->token});
        if (departed.size() > 2 * MaxPeers) departed.pop_front();
        game.despawn(it->player.id);
        if (!it->name.empty() || it->endpoint.steamId) ++nameRevisions[it->slot / wire::PeerNamePageSize];
        return peers.erase(it);
    }

    bool receive(const Datagram& datagram) {
        wire::Reader reader(std::span(datagram.data).first(datagram.size));
        wire::Header header;
        if (!wire::readHeader(reader, header)) return false;
        if (header.kind == wire::Kind::Hello || header.kind == wire::Kind::Confirm) {
            return handshake(datagram.peer, header, reader);
        }
        if (header.version != wire::Version || !header.session) return false;
        const auto peer = std::ranges::find(peers, datagram.peer, &Peer::endpoint);
        if (peer == peers.end() || peer->token != header.session) {
            const auto prior = std::ranges::find_if(departed, [&](const Departed& departedPeer) {
                return departedPeer.endpoint == datagram.peer && departedPeer.token == header.session;
            });
            if (header.kind == wire::Kind::Goodbye && reader.done()) {
                if (prior == departed.end()) {
                    const auto pending = std::ranges::find(challenges, datagram.peer, &Challenge::endpoint);
                    if (pending == challenges.end() || pending->token != header.session) return false;
                    departed.push_back({pending->endpoint, pending->token});
                    if (departed.size() > 2 * MaxPeers) departed.pop_front();
                    challenges.erase(pending);
                }
                wire::send(transport, datagram.peer, SessionControlPolicy, wire::Kind::GoodbyeAck, header.session);
                return true;
            }
            return prior != departed.end();
        }

        switch (header.kind) {
        case wire::Kind::Goodbye:
            if (!reader.done()) return false;
            wire::send(transport, datagram.peer, SessionControlPolicy, wire::Kind::GoodbyeAck, header.session);
            remove(peer);
            return true;
        case wire::Kind::Input:
            if (!receiveInput(*peer, reader)) return false;
            break;
        case wire::Kind::Ping: {
            const auto stamp = reader.get<uint64_t>();
            if (!stamp || !reader.done()) return false;
            sendGame(*peer, wire::Kind::Pong, stamp);
            break;
        }
        case wire::Kind::Rpc:
            if (!receiveRpc(*peer, reader)) return false;
            break;
        default:
            return false;
        }
        peer->lastHeard = now;
        return true;
    }

    PlayerInput applyInput(Peer& peer, PlayerInput input) const {
        const auto id = peer.player.id;
        simulate(peer.player, input);
        peer.player.id = id;
        return input;
    }

    PlayerInput missingInput(Peer& peer) const {
        const auto held = std::exchange(peer.heldInputGrace, {});
        return held.hasControls ? applyInput(peer, held) : PlayerInput{};
    }

    PlayerInput advance(Peer& peer) const {
        auto next = peer.inputAck + 1;
        if (next > peer.highestInput) return missingInput(peer);
        if (peer.inputs[next % InputWindow].sequence != next) {
            if (++peer.gapTicks <= 2) return missingInput(peer);
            while (peer.inputs[next % InputWindow].sequence != next) ++next;
        }
        peer.gapTicks = 0;
        for (size_t skipped = 0; skipped < wire::MaxInputBatch && peer.highestInput - next > BufferedControlLead; ++skipped) {
            const auto& following = peer.inputs[(next + 1) % InputWindow];
            if (following.sequence != next + 1 ||
                !canCoalesceControls(peer.inputs[next % InputWindow].sample, following.sample, peer.actionCounts)) break;
            ++next;
        }
        const auto& sample = peer.inputs[next % InputWindow].sample;
        auto input = sample.input;
        if (input.hasControls) {
            for (unsigned action = 0; action < ControlActionCount; ++action) {
                if (sample.actionCounts[action] != peer.actionCounts[action]) {
                    input.pressed |= static_cast<uint8_t>(1U << action);
                }
            }
            peer.actionCounts = sample.actionCounts;
        }
        peer.heldInputGrace = input.hasControls ? input : PlayerInput{};
        peer.heldInputGrace.pressed = 0;
        peer.inputAck = next;
        return applyInput(peer, input);
    }

    bool relevant(const EntityState& owner, const EntityState& candidate) const {
        if (owner.id == candidate.id || config.relevanceRadiusMm == 0) return true;
        const double dx = double(candidate.x) - owner.x;
        const double dy = double(candidate.y) - owner.y;
        const double dz = double(candidate.z) - owner.z;
        const double radius = config.relevanceRadiusMm;
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }

    void sendSnapshots() {
        for (auto& peer : peers) {
            wire::Snapshot snapshot;
            snapshot.sequence = now / (TickRate / SnapshotRate);
            snapshot.tick = now;
            snapshot.inputAck = peer.inputAck;
            snapshot.entities.reserve(peers.size() + replicas.size());
            for (const auto& candidate : peers) {
                if (relevant(peer.player, candidate.player)) snapshot.entities.push_back(candidate.player);
            }
            for (const auto& replica : replicas) {
                if (relevant(peer.player, replica)) snapshot.entities.push_back(replica);
            }
            std::ranges::sort(snapshot.entities, {}, &EntityState::id);
            const auto found = std::ranges::find(peer.snapshots, peer.snapshotAck, &wire::Snapshot::sequence);
            const auto* baseline = config.deltaSnapshots && found != peer.snapshots.end() ? &*found : nullptr;
            if (baseline) snapshot.baseline = baseline->sequence;
            const auto fullBytes = wire::FullSnapshotHeaderBytes + snapshot.entities.size() * wire::FullEntityBytes;
            auto packet = wire::encodeSnapshot(peer.token, snapshot, baseline);
            if (baseline && (packet.finish().empty() || packet.finish().size() >= fullBytes)) {
                snapshot.baseline = 0;
                packet = wire::encodeSnapshot(peer.token, snapshot, nullptr);
            }
            const auto bytes = packet.finish();
            if (sendGame(peer, bytes)) {
                stats.snapshotBytes += bytes.size();
                stats.fullSnapshotBytes += fullBytes;
                ++stats.snapshotsSent;
                peer.snapshots.push_back(std::move(snapshot));
                if (peer.snapshots.size() > SnapshotHistory) peer.snapshots.pop_front();
            }
        }
    }

    void sendPeerNames() {
        for (size_t page = 0; page < wire::PeerNamePages; ++page) {
            const auto revision = nameRevisions[page];
            if (!revision || (revision == publishedNames[page] && now % (TickRate / 2) != 0)) continue;
            std::array<wire::PeerNameEntry, wire::PeerNamePageSize> entries{};
            size_t count = 0;
            for (const auto& peer : peers) {
                if ((!peer.name.empty() || peer.endpoint.steamId) && peer.slot / wire::PeerNamePageSize == page) {
                    entries[count++] = {peer.player.id, peer.name, peer.endpoint.steamId};
                }
            }
            if (page == wire::PeerNamePages - 1 && localPeerId) {
                entries[count++] = {localPeerId, localName, localSteamId};
            }
            for (auto& peer : peers) {
                auto packet = wire::encodePeerNames(peer.token, static_cast<uint8_t>(page), revision,
                    std::span(entries).first(count));
                sendGame(peer, packet.finish());
            }
            publishedNames[page] = revision;
        }
    }

    ServerGame& game;
    SimulatePlayer simulate;
    DatagramTransport transport;
    ServerConfig config{};
    NetworkStats stats{};
    std::vector<Peer> peers;
    std::vector<EntityState> replicas;
    std::array<uint64_t, wire::PeerNamePages> nameRevisions{};
    std::array<uint64_t, wire::PeerNamePages> publishedNames{};
    EntityId localPeerId{};
    uint64_t localSteamId{};
    std::string localName;
    std::vector<Challenge> challenges;
    std::deque<Departed> departed;
    std::random_device random;
    EntityId nextEntity{};
    uint64_t now{};
    uint64_t lastSnapshotTick{};
    bool running{};
};

NetworkServer::NetworkServer(ServerGame& game, SimulatePlayer simulate)
    : m_state(std::make_unique<State>(game, simulate)) {}

NetworkServer::~NetworkServer() {
    stop();
}

void NetworkServer::setSteamLobby(uint64_t lobbyId) { m_state->transport.setSteamLobby(lobbyId); }

bool NetworkServer::start(const ServerConfig& config, std::string& error) {
    auto& state = *m_state;
    if (state.running || !state.simulate || config.maxPeers == 0 || config.maxPeers > MaxPeers ||
        config.relevanceRadiusMm < 0) {
        error = state.running ? "Server already running" : "Invalid server configuration";
        return false;
    }
    state.peers.reserve(config.maxPeers);
    state.challenges.reserve(MaxChallenges);
    if (!state.transport.start(config.bind, error)) return false;
    state.config = config;
    state.stats = {};
    state.now = 0;
    state.lastSnapshotTick = 0;
    state.running = true;
    return true;
}

void NetworkServer::stop() {
    auto& state = *m_state;
    if (!state.running) return;
    for (const auto& peer : state.peers) {
        wire::send(state.transport, peer.endpoint, SessionControlPolicy, wire::Kind::Goodbye, peer.token);
        state.game.despawn(peer.player.id);
    }
    state.peers.clear();
    state.replicas.clear();
    state.nameRevisions = {};
    state.publishedNames = {};
    state.localPeerId = 0;
    state.localSteamId = 0;
    state.localName.clear();
    state.challenges.clear();
    state.departed.clear();
    state.transport.flushReady();
    state.transport.stop();
    state.running = false;
}

void NetworkServer::tick() {
    auto& state = *m_state;
    if (!state.running) return;
    ++state.now;
    state.transport.poll();
    std::erase_if(state.challenges, [&](const Challenge& challenge) { return state.now >= challenge.deadline; });
    Datagram datagram;
    for (size_t i = 0; i < ReceiveBudget && state.transport.receive(datagram); ++i) {
        if (!state.receive(datagram)) ++state.stats.invalidMessages;
    }
    for (auto it = state.peers.begin(); it != state.peers.end();) {
        if (state.now - it->lastHeard >= TimeoutTicks) it = state.remove(it);
        else ++it;
    }
    for (auto& peer : state.peers) state.game.input(peer.player.id, state.advance(peer));
    if (!state.config.deferSnapshots) publishSnapshots();
    state.sendPeerNames();
}

void NetworkServer::publishSnapshots() {
    auto& state = *m_state;
    if (!state.running || state.now == state.lastSnapshotTick || state.now % (TickRate / SnapshotRate) != 0) return;
    state.lastSnapshotTick = state.now;
    state.sendSnapshots();
}

Endpoint NetworkServer::localEndpoint() const { return m_state->transport.localEndpoint(); }
size_t NetworkServer::peerCount() const { return m_state->peers.size(); }

bool NetworkServer::setPeerValue(EntityId id, uint16_t value) {
    auto& peers = m_state->peers;
    const auto peer = std::ranges::find(peers, id, [](const Peer& candidate) { return candidate.player.id; });
    if (peer == peers.end()) return false;
    peer->player.value = value;
    return true;
}

bool NetworkServer::setPeerMotion(EntityId id, int32_t vx, int32_t vy, int32_t vz, uint16_t jumpBufferMs,
                                  uint16_t coyoteMs) {
    auto& peers = m_state->peers;
    const auto peer = std::ranges::find(peers, id, [](const Peer& candidate) { return candidate.player.id; });
    if (peer == peers.end()) return false;
    auto& player = peer->player;
    player.x = vx;
    player.y = vy;
    player.z = vz;
    player.yaw = jumpBufferMs;
    player.pitch = coyoteMs;
    player.flags |= PeerMotionValid;
    return true;
}

std::optional<uint16_t> NetworkServer::peerValue(EntityId id) const {
    const auto& peers = m_state->peers;
    const auto peer = std::ranges::find(peers, id, [](const Peer& candidate) { return candidate.player.id; });
    if (peer == peers.end()) return {};
    return peer->player.value;
}

bool NetworkServer::setLocalSteamIdentity(EntityId id) {
    auto& state = *m_state;
    const auto steamId = state.transport.localEndpoint().steamId;
    if (!state.running || !id || id > MaxReplicaKey || !validSteamUserId(steamId) ||
        state.config.maxPeers == MaxPeers || (state.localPeerId && state.localPeerId != id) ||
        std::ranges::find(state.replicas, id, &EntityState::id) == state.replicas.end()) {
        return false;
    }
    if (state.localSteamId != steamId) {
        state.localPeerId = id;
        state.localSteamId = steamId;
        ++state.nameRevisions.back();
    }
    return true;
}

uint64_t NetworkServer::peerSteamId(EntityId id) const {
    const auto& state = *m_state;
    if (id && id == state.localPeerId) return state.localSteamId;
    const auto& peers = state.peers;
    const auto peer = std::ranges::find(peers, id, [](const Peer& candidate) { return candidate.player.id; });
    return peer == peers.end() ? 0 : peer->endpoint.steamId;
}

bool NetworkServer::setPeerName(EntityId id, std::string_view name) {
    const auto normalized = normalizePeerName(name);
    if (!normalized || !id || id > MaxReplicaKey) return false;
    auto& state = *m_state;
    const auto peer = std::ranges::find(state.peers, id, [](const Peer& candidate) { return candidate.player.id; });
    if (peer != state.peers.end()) {
        if (peer->name != *normalized) {
            peer->name = *normalized;
            ++state.nameRevisions[peer->slot / wire::PeerNamePageSize];
        }
        return true;
    }
    if (state.config.maxPeers == MaxPeers || (state.localPeerId && state.localPeerId != id) ||
        std::ranges::find(state.replicas, id, &EntityState::id) == state.replicas.end()) {
        return false;
    }
    if (state.localName != *normalized) {
        state.localPeerId = normalized->empty() && !state.localSteamId ? 0 : id;
        state.localName = *normalized;
        ++state.nameRevisions.back();
    }
    return true;
}

std::string_view NetworkServer::peerName(EntityId id) const {
    const auto& state = *m_state;
    if (id && id == state.localPeerId) return state.localName;
    const auto peer = std::ranges::find(state.peers, id, [](const Peer& candidate) { return candidate.player.id; });
    return peer == state.peers.end() ? std::string_view{} : std::string_view(peer->name);
}

EntityId NetworkServer::createReplica(EntityState initial) {
    auto& state = *m_state;
    if (!state.running || state.replicas.size() >= MaxReplicas - state.config.maxPeers) return 0;
    initial.id = state.allocateId();
    if (initial.id) state.replicas.push_back(initial);
    return initial.id;
}

bool NetworkServer::createKeyedReplica(uint32_t key, EntityState initial) {
    auto& state = *m_state;
    const auto id = keyedReplicaId(key);
    if (!id || !state.running || state.replicas.size() >= MaxReplicas - state.config.maxPeers ||
        std::ranges::find(state.replicas, id, &EntityState::id) != state.replicas.end()) return false;
    initial.id = id;
    state.replicas.push_back(initial);
    return true;
}

bool NetworkServer::setReplica(const EntityState& replica) {
    const auto found = std::ranges::find(m_state->replicas, replica.id, &EntityState::id);
    if (found == m_state->replicas.end()) return false;
    *found = replica;
    return true;
}

bool NetworkServer::destroyReplica(EntityId id) {
    auto& state = *m_state;
    if (id && id == state.localPeerId) {
        state.localPeerId = 0;
        state.localSteamId = 0;
        state.localName.clear();
        ++state.nameRevisions.back();
    }
    return std::erase_if(state.replicas, [id](const EntityState& replica) { return replica.id == id; }) != 0;
}

void NetworkServer::setEmulation(NetworkEmulation emulation) { m_state->transport.setEmulation(emulation); }

bool NetworkServer::setPeerEmulation(uint64_t connectionNonce, NetworkEmulation emulation) {
    auto& peers = m_state->peers;
    const auto found = findPeerByNonce(peers, connectionNonce);
    if (found == peers.end()) return false;
    found->emulation = emulation;
    return true;
}

EntityId NetworkServer::loopbackPeerForNonce(uint64_t connectionNonce) const {
    const auto& peers = m_state->peers;
    const auto found = findPeerByNonce(peers, connectionNonce);
    if (found == peers.end() || found->endpoint.steamId || (found->endpoint.address >> 24) != 127) return 0;
    return found->player.id;
}

std::optional<NetworkLossCounters> NetworkServer::peerEmulationCounters(uint64_t connectionNonce) const {
    const auto& peers = m_state->peers;
    const auto found = findPeerByNonce(peers, connectionNonce);
    if (found == peers.end()) return std::nullopt;
    return found->emulationCounters;
}

NetworkStats NetworkServer::stats() const {
    auto stats = m_state->stats;
    stats.transport = m_state->transport.stats();
    return stats;
}

}
