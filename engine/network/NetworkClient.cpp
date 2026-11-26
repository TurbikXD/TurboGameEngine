#include "engine/network/NetworkProtocol.h"

#include <chrono>
#include <cmath>
#include <deque>
#include <numbers>
#include <random>
#include <utility>

namespace engine::net {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t TimeoutTicks = 5 * TickRate;
constexpr uint64_t SteamConnectTimeoutTicks = 20 * TickRate;
constexpr uint64_t RetryHandshakeTicks = TickRate / 4;
constexpr uint64_t RetryRpcTicks = TickRate / 6;
constexpr uint64_t RpcTimeoutTicks = 3 * TickRate;
constexpr uint64_t PingTicks = TickRate / 2;
constexpr auto ProbeDeadline = std::chrono::seconds(2);
constexpr auto RttLifetime = std::chrono::seconds(2);
constexpr size_t SnapshotHistory = 64;
constexpr size_t InputHistory = 256;
constexpr size_t MaxRpcResults = 32;
constexpr size_t ReceiveBudget = 256;
constexpr double SnapshotInterval = double(TickRate) / SnapshotRate;
constexpr double MinInterpolationDelay = SnapshotInterval * 2;
constexpr double MaxInterpolationDelay = SnapshotInterval * 5;
constexpr double MaxPlaybackAge = MaxInterpolationDelay * 2;
constexpr float RadiansPerYawUnit = 2 * std::numbers::pi_v<float> / 65536;

struct RecordedInput {
    uint64_t sequence;
    wire::InputSample sample;
};

struct PendingRpc {
    uint32_t request;
    RpcCommand command;
    uint64_t deadline;
    uint64_t retry;
};

struct PeerIdentity {
    EntityId id{};
    std::string name;
    uint64_t steamId{};
};

struct PeerNamePage {
    uint64_t revision{};
    std::vector<PeerIdentity> entries;
};

struct ClosingSession {
    Endpoint endpoint;
    uint64_t token;
    uint64_t deadline;
    uint64_t retry;
};

struct Ping {
    uint64_t sequence{};
    Clock::time_point sent{};
    bool accounted{};
};

VisualEntity visual(const EntityState& state) {
    return {state.id, state.x * 0.001F, state.y * 0.001F, state.z * 0.001F,
        state.yaw * RadiansPerYawUnit, state.value, state.flags,
        state.pitch * RadiansPerYawUnit, state.roll * RadiansPerYawUnit};
}

float blendAngle(float from, float to, float fraction) {
    return from + std::remainder(to - from, 2 * std::numbers::pi_v<float>) * fraction;
}

VisualEntity blend(const EntityState& before, const EntityState& after, float fraction) {
    auto left = visual(before);
    const auto right = visual(after);
    left.x = std::lerp(left.x, right.x, fraction);
    left.y = std::lerp(left.y, right.y, fraction);
    left.z = std::lerp(left.z, right.z, fraction);
    left.yaw = blendAngle(left.yaw, right.yaw, fraction);
    left.pitch = blendAngle(left.pitch, right.pitch, fraction);
    left.roll = blendAngle(left.roll, right.roll, fraction);
    if (fraction == 1.0F) left.sampledFlags = right.sampledFlags;
    return left;
}

}

struct NetworkClient::State {
    explicit State(SimulatePlayer simulation) : simulate(simulation) {}

    void finishProbe(Ping& ping, bool expired) {
        ping.accounted = true;
        ++stats.probes.packets;
        if (expired) ++stats.probes.dropped;
    }

    void expireProbes(Clock::time_point at) {
        for (auto& ping : pings) {
            if (ping.sequence && !ping.accounted && at - ping.sent >= ProbeDeadline) finishProbe(ping, true);
        }
    }

    void changePhase(ClientPhase next, std::string_view reason) {
        if (phase == ClientPhase::Connected && next != ClientPhase::Connected) {
            expireProbes(Clock::now());
            pings = {};
        }
        phase = next;
        diagnostic = reason;
        if (phase == ClientPhase::Connected || phase == ClientPhase::Connecting) return;
        for (const auto& pending : pendingRpcs) {
            results.push_back({pending.request, RpcStatus::Disconnected, 0});
        }
        pendingRpcs.clear();
        snapshots.clear();
        peerNames = {};
        inputs.clear();
        local = 0;
        token = 0;
        predicted = {};
        stats.relevantEntities = 0;
    }

    void sendHandshake() {
        wire::send(transport, server, sessionControlPolicy, wire::Kind::Hello, 0, nonce);
        if (token) wire::send(transport, server, sessionControlPolicy, wire::Kind::Confirm, token, nonce, compatibility);
        nextHandshake = now + RetryHandshakeTicks;
    }

    void beginClose() {
        if (!token) return;
        wire::send(transport, server, sessionControlPolicy, wire::Kind::Goodbye, token);
        closing = ClosingSession{server, token, now + TickRate / 2, now + 6};
    }

    bool handshake(const wire::Header& header, wire::Reader& reader) {
        if (reader.get<uint64_t>() != nonce) return false;
        if (header.kind == wire::Kind::Reject) {
            const auto reason = static_cast<wire::Reject>(reader.get<uint8_t>());
            if (!reader.done()) return false;
            if (reason == wire::Reject::Incompatible && !header.session) {
                changePhase(ClientPhase::Rejected, "Incompatible network protocol version");
                return true;
            }
            if (!token || header.session != token || header.version != wire::Version) return false;
            if (reason == wire::Reject::Full) {
                changePhase(ClientPhase::Rejected, "Server is full");
                return true;
            }
            if (reason == wire::Reject::ContentMismatch) {
                changePhase(ClientPhase::Rejected, "Incompatible game or scene build");
                return true;
            }
            return false;
        }
        if (header.version != wire::Version || !header.session) return false;
        if (header.kind == wire::Kind::Challenge && reader.done()) {
            if (token != header.session) {
                token = header.session;
                wire::send(transport, server, sessionControlPolicy, wire::Kind::Confirm, token, nonce, compatibility);
                nextHandshake = now + RetryHandshakeTicks;
            }
            return true;
        }
        if (header.kind == wire::Kind::Accept && header.session == token) {
            const auto id = reader.get<EntityId>();
            if (!id || !reader.done()) return false;
            local = id;
            changePhase(ClientPhase::Connected, "Connected");
            lastHeard = now;
            lastPong = now;
            nextPing = now;
            return true;
        }
        return false;
    }

    void reconcile(const EntityState& authority, uint64_t inputAck) {
        while (!inputs.empty() && inputs.front().sequence <= inputAck) inputs.pop_front();
        auto updated = authority;
        if (predict) {
            for (const auto& command : inputs) simulate(updated, command.sample.input);
        }
        updated.id = authority.id;
        if (predict && predicted.id) {
            const auto distance = std::hypot(double(updated.x) - predicted.x,
                double(updated.y) - predicted.y, double(updated.z) - predicted.z);
            if (distance != 0 || updated.yaw != predicted.yaw ||
                updated.pitch != predicted.pitch || updated.roll != predicted.roll) {
                ++stats.corrections;
                stats.lastCorrectionMm = static_cast<float>(distance);
            }
        }
        predicted = updated;
    }

    bool receiveSnapshot(wire::Reader& reader, size_t bytes) {
        wire::Snapshot snapshot;
        snapshot.sequence = reader.get<uint64_t>();
        snapshot.tick = reader.get<uint64_t>();
        snapshot.baseline = reader.get<uint64_t>();
        snapshot.inputAck = reader.get<uint64_t>();
        if (!snapshot.sequence || !snapshot.tick || snapshot.baseline >= snapshot.sequence ||
            snapshot.inputAck > inputSequence) return false;
        if (!snapshots.empty() && snapshot.sequence <= snapshots.back().sequence) return true;
        if (!snapshots.empty() &&
            (snapshot.tick <= snapshots.back().tick || snapshot.inputAck < snapshots.back().inputAck)) {
            return false;
        }
        const wire::Snapshot* baseline = nullptr;
        if (snapshot.baseline) {
            const auto found = std::ranges::find(snapshots, snapshot.baseline, &wire::Snapshot::sequence);
            if (found == snapshots.end()) return false;
            baseline = &*found;
        }
        if (!wire::decodeEntities(reader, snapshot, baseline)) return false;
        const auto* authority = wire::findEntity(snapshot, local);
        if (!authority) return false;

        reconcile(*authority, snapshot.inputAck);
        const auto offset = static_cast<double>(snapshot.tick) - static_cast<double>(now);
        if (snapshots.empty()) {
            sourceClockOffset = offset;
            interpolationDelay = MinInterpolationDelay;
            playbackTick = nextPlaybackTick = static_cast<double>(snapshot.tick) - interpolationDelay;
        } else {
            sourceClockOffset += std::clamp(offset - sourceClockOffset, -1.0, 1.0) * 0.125;
            const auto arrivalGap = static_cast<double>(now - lastSnapshotArrival);
            interpolationDelay = std::max(interpolationDelay,
                std::clamp(arrivalGap + 1.0, MinInterpolationDelay, MaxInterpolationDelay));
        }
        lastSnapshotArrival = now;
        stats.snapshotBytes += bytes;
        stats.fullSnapshotBytes += wire::FullSnapshotHeaderBytes + snapshot.entities.size() * wire::FullEntityBytes;
        ++stats.snapshotsReceived;
        stats.relevantEntities = snapshot.entities.size();
        snapshots.push_back(std::move(snapshot));
        if (snapshots.size() > SnapshotHistory) snapshots.pop_front();
        lastHeard = now;
        return true;
    }

    bool receivePeerNames(wire::Reader& reader) {
        wire::PeerNamePage incoming;
        if (!wire::readPeerNames(reader, incoming)) return false;
        const auto entries = std::span(incoming.entries).first(incoming.count);
        if (!server.steamId && std::ranges::any_of(entries, [](const auto& entry) { return entry.steamId != 0; })) {
            return false;
        }
        auto& current = peerNames[incoming.index];
        if (incoming.revision <= current.revision) return true;
        for (const auto& entry : entries) {
            for (const auto& page : peerNames) {
                if (&page == &current) continue;
                if (std::ranges::find(page.entries, entry.id, &PeerIdentity::id) != page.entries.end()) return false;
            }
        }
        current.entries.clear();
        current.entries.reserve(entries.size());
        for (const auto& [id, name, steamId] : entries) {
            current.entries.push_back({id, std::string(name), steamId});
        }
        current.revision = incoming.revision;
        return true;
    }

    const PeerIdentity* peerIdentity(EntityId id) const {
        for (const auto& page : peerNames) {
            const auto found = std::ranges::find(page.entries, id, &PeerIdentity::id);
            if (found != page.entries.end()) return &*found;
        }
        return nullptr;
    }

    bool receive(const Datagram& datagram) {
        if (closing && datagram.peer == closing->endpoint) {
            wire::Reader reader(std::span(datagram.data).first(datagram.size));
            wire::Header header;
            if (wire::readHeader(reader, header) && header.version == wire::Version &&
                header.kind == wire::Kind::GoodbyeAck && header.session == closing->token && reader.done()) {
                closing.reset();
                return true;
            }
        }
        if (datagram.peer != server) return true;
        if (phase != ClientPhase::Connecting && phase != ClientPhase::Connected) return true;
        wire::Reader reader(std::span(datagram.data).first(datagram.size));
        wire::Header header;
        if (!wire::readHeader(reader, header)) return false;
        if (phase == ClientPhase::Connecting) {
            if (header.kind == wire::Kind::Snapshot || header.kind == wire::Kind::PeerNames) return true;
            return handshake(header, reader);
        }
        if (header.version != wire::Version) return false;
        if (header.session != token) return true;

        switch (header.kind) {
        case wire::Kind::Goodbye:
            if (!reader.done()) return false;
            changePhase(ClientPhase::Disconnected, "Server shut down");
            return true;
        case wire::Kind::Snapshot:
            return receiveSnapshot(reader, datagram.size);
        case wire::Kind::PeerNames:
            if (!receivePeerNames(reader)) return false;
            break;
        case wire::Kind::RpcResult: {
            const auto request = reader.get<uint32_t>();
            const auto status = static_cast<RpcStatus>(reader.get<uint8_t>());
            const auto value = reader.get<uint16_t>();
            if (!request || status > RpcStatus::TimedOut || !reader.done()) return false;
            const auto found = std::ranges::find(pendingRpcs, request, &PendingRpc::request);
            if (found == pendingRpcs.end()) return true;
            results.push_back({request, status, value});
            pendingRpcs.erase(found);
            break;
        }
        case wire::Kind::Pong: {
            const auto stamp = reader.get<uint64_t>();
            if (!stamp || !reader.done()) return false;
            auto& ping = pings[stamp % pings.size()];
            if (ping.sequence != stamp) return true;
            const auto receivedAt = Clock::now();
            const auto elapsed = receivedAt - ping.sent;
            const bool expired = elapsed >= ProbeDeadline;
            const bool freshMeasurement = !ping.accounted && !expired;
            if (!ping.accounted) finishProbe(ping, expired);
            ping = {};
            if (stamp <= lastPongSequence) return true;
            stats.rttMs = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
            lastPong = now;
            lastPongSequence = stamp;
            if (freshMeasurement) {
                stats.recentRttMs = stats.rttMs;
                rttMeasuredAt = receivedAt;
            }
            break;
        }
        case wire::Kind::Accept:
        case wire::Kind::Challenge:
            return true;
        default:
            return false;
        }
        lastHeard = now;
        return true;
    }

    void sendInputs(PlayerInput input) {
        input.x = std::max(input.x, static_cast<int8_t>(-MaxControlAxis));
        input.z = std::max(input.z, static_cast<int8_t>(-MaxControlAxis));
        if (input.hasControls) {
            for (unsigned action = 0; action < ControlActionCount; ++action) {
                if (input.pressed & (1U << action)) ++actionCounts[action];
            }
        }
        inputs.push_back({++inputSequence, {input, actionCounts}});
        if (inputs.size() > InputHistory) inputs.pop_front();
        if (predict && predicted.id) {
            simulate(predicted, input);
            predicted.id = local;
        }
        const auto count = std::min(inputs.size(), wire::MaxInputBatch);
        wire::Writer writer(wire::Kind::Input, token);
        writer.put(snapshots.empty() ? uint64_t{} : snapshots.back().sequence);
        writer.put(inputSequence);
        writer.put(static_cast<uint8_t>(count));
        for (auto it = inputs.end() - count; it != inputs.end(); ++it) {
            wire::putInput(writer, it->sample);
        }
        transport.send(server, writer.finish());
    }

    void sendPing() {
        nextPing = now + PingTicks;
        const auto sequence = pingSequence + 1;
        auto& ping = pings[sequence % pings.size()];
        if (ping.sequence && !ping.accounted) return;
        const auto sent = Clock::now();
        if (wire::send(transport, server, wire::Kind::Ping, token, sequence)) {
            ping = {sequence, sent};
            pingSequence = sequence;
        }
    }

    void advancePlayback() {
        if (snapshots.empty()) return;
        const auto latest = static_cast<double>(snapshots.back().tick);
        playbackTick = latest - nextPlaybackTick > MaxPlaybackAge
            ? latest - MaxInterpolationDelay : nextPlaybackTick;
        const auto observedOffset = latest - static_cast<double>(lastSnapshotArrival);
        const bool clockBehind = observedOffset - sourceClockOffset > SnapshotInterval;
        const auto estimate = static_cast<double>(now) + sourceClockOffset;
        const auto target = (clockBehind ? std::max(estimate, latest) : estimate) - interpolationDelay;
        const auto error = target - playbackTick;
        const auto steadyRate = std::abs(error) <= 0.25 ? 1.0 : std::clamp(1.0 + error * 0.05, 0.9, 1.1);
        const auto catchUpRate = 1.0 + (error - SnapshotInterval) / SnapshotInterval;
        const auto rate = clockBehind ? std::clamp(catchUpRate, steadyRate, 4.0) : steadyRate;
        nextPlaybackTick = std::min(latest, playbackTick + rate);
        interpolationDelay = std::max(MinInterpolationDelay, interpolationDelay - 0.02);
    }

    SimulatePlayer simulate;
    DatagramTransport transport;
    EmulationPolicy sessionControlPolicy{EmulationPolicy::Apply};
    Endpoint server{};
    ClientPhase phase{ClientPhase::Disconnected};
    std::string_view diagnostic{"Disconnected"};
    NetworkStats stats{};
    uint64_t now{};
    uint64_t nonce{};
    uint64_t token{};
    uint64_t compatibility{};
    uint64_t nextHandshake{};
    uint64_t connectDeadline{};
    uint64_t lastHeard{};
    uint64_t lastPong{};
    uint64_t nextPing{};
    uint64_t pingSequence{};
    uint64_t lastPongSequence{};
    Clock::time_point rttMeasuredAt{};
    std::array<Ping, 16> pings{};
    EntityId local{};
    uint64_t inputSequence{};
    std::array<uint16_t, ControlActionCount> actionCounts{};
    std::deque<RecordedInput> inputs;
    std::deque<wire::Snapshot> snapshots;
    std::array<PeerNamePage, wire::PeerNamePages> peerNames;
    double playbackTick{};
    double nextPlaybackTick{};
    double sourceClockOffset{};
    double interpolationDelay{MinInterpolationDelay};
    uint64_t lastSnapshotArrival{};
    EntityState predicted{};
    bool predict{true};
    bool interpolate{true};
    uint32_t nextRequest{};
    std::vector<PendingRpc> pendingRpcs;
    std::deque<RpcResult> results;
    std::optional<ClosingSession> closing;
};

std::string_view rpcStatusName(RpcStatus status) {
    switch (status) {
    case RpcStatus::Accepted: return "Accepted";
    case RpcStatus::UnknownAction: return "Unknown action";
    case RpcStatus::InvalidArgument: return "Invalid argument";
    case RpcStatus::Forbidden: return "Forbidden";
    case RpcStatus::RateLimited: return "Rate limited";
    case RpcStatus::TimedOut: return "Timed out";
    case RpcStatus::Disconnected: return "Disconnected";
    }
    return "Invalid status";
}

NetworkClient::NetworkClient(SimulatePlayer simulate)
    : m_state(std::make_unique<State>(simulate)) {}

NetworkClient::~NetworkClient() {
    disconnect();
    m_state->transport.flushReady();
}

bool NetworkClient::connect(Endpoint server, std::string& error, uint64_t compatibility) {
    auto& state = *m_state;
    if (!state.simulate || !server.port || (!server.steamId && (!server.address || server.address == 0xffffffff))) {
        error = "Invalid server endpoint or missing player simulation";
        return false;
    }
    disconnect();
    const auto local = state.transport.localEndpoint();
    if (local.port && (local.steamId || server.steamId)) {
        state.transport.flushReady();
        state.transport.stop();
        state.closing.reset();
    }
    if (!state.transport.localEndpoint().port && !state.transport.startClient(server, error)) return false;
    std::random_device random;
    do {
        state.nonce = (uint64_t{random()} << 32) | random();
    } while (!state.nonce);
    state.server = server;
    state.compatibility = compatibility;
    state.connectDeadline = state.now + (server.steamId ? SteamConnectTimeoutTicks : TimeoutTicks);
    state.inputSequence = 0;
    state.actionCounts = {};
    state.pingSequence = 0;
    state.lastPongSequence = 0;
    state.pings = {};
    state.stats = {};
    state.changePhase(ClientPhase::Connecting, "Connecting");
    state.sendHandshake();
    error.clear();
    return true;
}

void NetworkClient::disconnect() {
    auto& state = *m_state;
    state.beginClose();
    state.changePhase(ClientPhase::Disconnected, "Disconnected");
}

void NetworkClient::tick(PlayerInput input) {
    auto& state = *m_state;
    ++state.now;
    state.transport.poll();
    if (state.phase == ClientPhase::Connected) state.expireProbes(Clock::now());
    if (state.closing && state.now >= state.closing->deadline) state.closing.reset();
    if (state.closing && state.now >= state.closing->retry) {
        wire::send(state.transport, state.closing->endpoint, state.sessionControlPolicy, wire::Kind::Goodbye,
            state.closing->token);
        state.closing->retry = state.now + 6;
    }
    Datagram datagram;
    for (size_t i = 0; i < ReceiveBudget && state.transport.receive(datagram); ++i) {
        if (!state.receive(datagram)) ++state.stats.invalidMessages;
    }
    if (state.phase == ClientPhase::Connecting) {
        if (state.now >= state.connectDeadline) {
            state.beginClose();
            state.changePhase(ClientPhase::TimedOut, "Connection timed out");
        }
        else if (state.now >= state.nextHandshake) state.sendHandshake();
        return;
    }
    if (state.phase != ClientPhase::Connected) {
        if (!state.closing && state.transport.localEndpoint().port) state.transport.stop();
        return;
    }
    if (state.now - state.lastHeard >= TimeoutTicks || state.now - state.lastPong >= TimeoutTicks) {
        state.beginClose();
        state.changePhase(ClientPhase::TimedOut, "Connection lost");
        return;
    }
    state.sendInputs(input);
    if (state.now >= state.nextPing) state.sendPing();
    for (auto it = state.pendingRpcs.begin(); it != state.pendingRpcs.end();) {
        if (state.now >= it->deadline) {
            state.results.push_back({it->request, RpcStatus::TimedOut, 0});
            it = state.pendingRpcs.erase(it);
            continue;
        }
        if (state.now >= it->retry) {
            const auto& command = it->command;
            wire::Writer writer(wire::Kind::Rpc, state.token);
            writer.put(it->request);
            writer.put(command.action);
            writer.put(command.target);
            writer.put(command.argument);
            writer.putText(command.text);
            state.transport.send(state.server, writer.finish());
            it->retry = state.now + RetryRpcTicks;
        }
        ++it;
    }
    state.advancePlayback();
}

ClientPhase NetworkClient::phase() const { return m_state->phase; }
std::string_view NetworkClient::diagnostic() const { return m_state->diagnostic; }
EntityId NetworkClient::localEntity() const { return m_state->local; }

uint64_t NetworkClient::connectionNonce() const {
    const auto& state = *m_state;
    return state.phase == ClientPhase::Connecting || state.phase == ClientPhase::Connected ? state.nonce : 0;
}

uint64_t NetworkClient::nextInputSequence() const {
    const auto& state = *m_state;
    return state.phase == ClientPhase::Connected ? state.inputSequence + 1U : 0;
}

std::optional<uint16_t> NetworkClient::peerValue(EntityId id) const {
    const auto& state = *m_state;
    if (!id || replicaKey(id) || state.phase != ClientPhase::Connected || state.snapshots.empty()) return {};
    const auto* peer = wire::findEntity(state.snapshots.back(), id);
    if (!peer) return {};
    return peer->value;
}

std::string_view NetworkClient::peerName(EntityId id) const {
    const auto* peer = m_state->peerIdentity(id);
    return peer ? std::string_view(peer->name) : std::string_view{};
}

uint64_t NetworkClient::peerSteamId(EntityId id) const {
    const auto* peer = m_state->peerIdentity(id);
    return peer ? peer->steamId : 0;
}

std::optional<AuthoritativeReplica> NetworkClient::latestReplica(EntityId id) const {
    const auto& state = *m_state;
    if (!id || state.phase != ClientPhase::Connected || state.snapshots.empty()) return {};
    const auto& latest = state.snapshots.back();
    if (const auto* entity = wire::findEntity(latest, id)) {
        return AuthoritativeReplica{*entity, latest.tick, latest.inputAck};
    }
    return {};
}

std::vector<VisualEntity> NetworkClient::presentation(float fraction) const {
    const auto& state = *m_state;
    if (state.snapshots.empty()) return {};
    const auto& newest = state.snapshots.back();
    const auto blendFraction = std::isfinite(fraction) ? std::clamp(fraction, 0.0F, 1.0F) : 0.0F;
    const auto renderTick = std::lerp(state.playbackTick, state.nextPlaybackTick, static_cast<double>(blendFraction));
    const auto after = std::lower_bound(state.snapshots.begin(), state.snapshots.end(), renderTick,
        [](const wire::Snapshot& snapshot, double tick) { return static_cast<double>(snapshot.tick) < tick; });
    const auto right = after != state.snapshots.end() ? after : std::prev(state.snapshots.end());
    const auto left = right != state.snapshots.begin() && after != state.snapshots.end() ? std::prev(right) : right;
    const auto alpha = left == right ? 0.0F : static_cast<float>((renderTick - static_cast<double>(left->tick)) /
        static_cast<double>(right->tick - left->tick));

    std::vector<VisualEntity> entities;
    entities.reserve(newest.entities.size());
    for (const auto& entity : newest.entities) {
        if (entity.id == state.local) {
            entities.push_back(visual(state.predict ? state.predicted : entity));
            continue;
        }
        auto sample = visual(entity);
        if (state.interpolate) {
            const auto* before = wire::findEntity(*left, entity.id);
            const auto* next = wire::findEntity(*right, entity.id);
            if (!before && !next) continue;
            if (before && next) {
                sample = blend(*before, *next, alpha);
            } else {
                sample = visual(next ? *next : *before);
            }
            sample.value = entity.value;
            sample.flags = entity.flags;
        }
        entities.push_back(sample);
    }
    return entities;
}

uint32_t NetworkClient::call(RpcCommand command) {
    auto& state = *m_state;
    if (command.text.size() > MaxPeerNameBytes || !normalizePeerName(command.text) ||
        state.phase != ClientPhase::Connected || state.pendingRpcs.size() + state.results.size() >= MaxRpcResults ||
        state.nextRequest == UINT32_MAX) return 0;
    const auto request = ++state.nextRequest;
    state.pendingRpcs.push_back({request, std::move(command), state.now + RpcTimeoutTicks, state.now});
    return request;
}

bool NetworkClient::takeResult(RpcResult& result) {
    auto& results = m_state->results;
    if (results.empty()) return false;
    result = results.front();
    results.pop_front();
    return true;
}

void NetworkClient::setEmulation(NetworkEmulation emulation, bool includeSessionControl) {
    m_state->transport.setEmulation(emulation);
    m_state->sessionControlPolicy = includeSessionControl ? EmulationPolicy::Apply : EmulationPolicy::Bypass;
}

void NetworkClient::resetProbeMeasurements() {
    auto& state = *m_state;
    state.stats.recentRttMs.reset();
    state.stats.probes = {};
    for (auto& ping : state.pings) {
        if (ping.sequence) ping.accounted = true;
    }
}

void NetworkClient::setInterpolation(bool enabled) { m_state->interpolate = enabled; }

void NetworkClient::setPrediction(bool enabled) {
    auto& state = *m_state;
    if (state.predict == enabled) return;
    state.predict = enabled;
    state.predicted = {};
    if (state.snapshots.empty()) return;
    const auto& latest = state.snapshots.back();
    state.reconcile(*wire::findEntity(latest, state.local), latest.inputAck);
}

NetworkStats NetworkClient::stats() const {
    const auto& state = *m_state;
    auto stats = state.stats;
    stats.transport = state.transport.stats();
    if (state.phase != ClientPhase::Connected || Clock::now() - state.rttMeasuredAt >= RttLifetime)
        stats.recentRttMs.reset();
    return stats;
}

}
