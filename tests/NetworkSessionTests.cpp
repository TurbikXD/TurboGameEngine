#include "engine/network/NetworkProtocol.h"
#include "engine/network/NetworkTestHistory.h"
#include "engine/network/NetworkTestSummary.h"
#include "engine/network/SteamTransport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace engine::net;
using namespace std::chrono_literals;

constexpr uint32_t Loopback = 0x7f000001;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void steamInvitePayloadIsValidated() {
    const SteamJoin join{0x01100001000004d2ULL, std::numeric_limits<uint64_t>::max()};
    const auto payload = encodeSteamJoin(join);
    require(parseSteamJoin(payload) == join, "Steam invitation lost host or build identity");
    require(!parseSteamJoin(payload + " extra") && !parseSteamJoin("2:" + payload.substr(2)) &&
        !parseSteamJoin("1:42:7") && encodeSteamJoin({0, 7}).empty(), "Invalid Steam invitation was accepted");
}

void chartHistoryRetainsBoundedSamples() {
    NetworkTestHistory history;
    NetworkTestMetrics healthy;
    healthy.rttMs = 0;
    healthy.uplinkLossPercent = 0.0;
    healthy.upload = NetworkTestTrafficRate{0.0, 0.0};
    history.append({0, healthy});
    history.append({250, {}});
    history.append({250, healthy});
    history.append({500, healthy, true});
    history.append({2'001, healthy});
    const auto& points = history.points();
    require(points.size() == 4 && points.front().metrics.rttMs == 0U && points.front().metrics.upload &&
        points.front().metrics.upload->bytesPerSecond == 0.0 && !points[1].metrics.rttMs,
        "Chart history merged missing measurements with valid zeros or accepted a duplicate timestamp");
    require(points[2].breakBefore && points[3].breakBefore,
        "Chart history connected samples across a profile change or a telemetry gap");
    history.prune(NetworkTestHistoryWindowMs + 500);
    require(points.size() == 2 && points.front().elapsedMs == 500,
        "Chart history did not retain the exact left edge of its time window");
    history.prune(NetworkTestHistoryWindowMs + 2'002);
    require(points.empty(), "Closed-client chart points did not age out");

    for (std::size_t i = 0; i < MaxNetworkTestHistoryPoints + 100; ++i) history.append({i, healthy});
    require(points.size() == MaxNetworkTestHistoryPoints && points.front().elapsedMs == 100,
        "Chart history exceeded its sample capacity");
}

void chartSummaryUsesVisibleMeasurements() {
    NetworkTestHistory history;
    NetworkTestMetrics zero;
    zero.rttMs = 0;
    zero.uplinkLossPercent = 0.0;
    zero.download = NetworkTestTrafficRate{0.0, 0.0};
    history.append({0, zero});

    NetworkTestMetrics outlier;
    outlier.rttMs = 1'000;
    outlier.uplinkLossPercent = 20.0;
    outlier.downlinkLossPercent = 0.0;
    outlier.upload = NetworkTestTrafficRate{1'024.0, 32.0};
    history.append({250, outlier});

    NetworkTestMetrics invalid;
    invalid.uplinkLossPercent = std::numeric_limits<double>::quiet_NaN();
    invalid.downlinkLossPercent = std::numeric_limits<double>::infinity();
    invalid.probeLossPercent = -1.0;
    invalid.download = NetworkTestTrafficRate{std::numeric_limits<double>::infinity(), -1.0};
    history.append({500, invalid});

    for (unsigned rtt = 18; rtt > 0; --rtt) {
        NetworkTestMetrics metrics;
        metrics.rttMs = rtt;
        if (rtt == 18) {
            metrics.uplinkLossPercent = 40.0;
            metrics.probeLossPercent = 25.0;
            metrics.upload = NetworkTestTrafficRate{3'072.0, 0.0};
            metrics.download = NetworkTestTrafficRate{1'024.0, 8.0};
        }
        history.append({(21U - rtt) * 250U, metrics});
    }

    const auto& points = history.points();
    const auto all = summarizeNetworkTestHistory({points.begin(), points.end()});
    require(all.meanRttMs && std::abs(*all.meanRttMs - 58.55) < 1e-9 && all.p95RttMs == 18.0,
        "Summary did not use nearest-rank p95 and the mean of available RTT samples");
    require(all.meanUplinkLossPercent == 20.0 && all.meanDownlinkLossPercent == 0.0 && all.meanProbeLossPercent == 25.0,
        "Summary did not average loss independently for each series");
    require(all.meanUploadBytesPerSecond == 2'048.0 && all.meanUploadPacketsPerSecond == 16.0 &&
        all.meanDownloadBytesPerSecond == 512.0 && all.meanDownloadPacketsPerSecond == 4.0,
        "Summary did not average directional traffic independently");

    const auto interval = summarizeNetworkTestHistory({points.begin(), points.begin() + 3});
    require(interval.meanRttMs == 500.0 && interval.p95RttMs == 1'000.0 && interval.meanUplinkLossPercent == 10.0 &&
        !interval.meanProbeLossPercent && interval.meanDownloadBytesPerSecond == 0.0,
        "Summary used samples outside the selected interval or replaced missing measurements");
    const auto empty = summarizeNetworkTestHistory({points.end(), points.end()});
    require(!empty.meanRttMs && !empty.p95RttMs && !empty.meanUplinkLossPercent && !empty.meanDownloadBytesPerSecond,
        "Empty summary reported measurements");
}

void codecRoundTrip() {
    const wire::Snapshot baseline{10, 30, 0, 21, {
        EntityState{7, -4000, -10, 1357, 65535, 42, 1, 1024, 61440}, EntityState{9, 802, 7, 0, 0, 7, 2}}};
    const wire::Snapshot next{11, 33, 10, 23, {
        EntityState{7, -4000, -10, 1279, 0, 42, 1, 2048, 32768}, EntityState{12, 641, -3, 777, 17, 2, 4}}};
    auto writer = wire::encodeSnapshot(0x0807060504030201ULL, next, &baseline);
    const auto bytes = writer.finish();
    require(bytes.size() > wire::HeaderBytes, "Delta packet has no payload");
    require(bytes[0] == 'T' && bytes[1] == 'G' && bytes[2] == 'E' && bytes[3] == '1' &&
        bytes[8] == 1 && bytes[15] == 8, "Header endian or magic changed");
    wire::Reader reader(bytes);
    wire::Header header;
    require(wire::readHeader(reader, header) && header.kind == wire::Kind::Snapshot &&
        header.version == wire::Version && header.session == 0x0807060504030201ULL, "Bad snapshot header");
    wire::Snapshot restored;
    restored.sequence = reader.get<uint64_t>();
    restored.tick = reader.get<uint64_t>();
    restored.baseline = reader.get<uint64_t>();
    restored.inputAck = reader.get<uint64_t>();
    require(restored.sequence == 11 && restored.tick == 33 && restored.baseline == 10 && restored.inputAck == 23,
        "Wrong delta metadata");
    require(wire::decodeEntities(reader, restored, &baseline) && restored.entities.size() == 2,
        "Delta could not reconstruct the entity set");
    require(wire::findEntity(restored, 9) == nullptr, "Delta tombstone did not remove entity");
    for (size_t i = 0; i < next.entities.size(); ++i) {
        const auto& state = restored.entities[i];
        require(state.id == next.entities[i].id && wire::changedFields(state, next.entities[i]) == 0,
            "Delta changed or lost entity fields");
    }
    wire::Reader truncated(bytes.first(bytes.size() - 1));
    require(!wire::readHeader(truncated, header), "Accepted a truncated datagram");
    const std::array<uint8_t, 1> impossibleCount{255};
    wire::Reader bogusCount(impossibleCount);
    require(!wire::decodeEntities(bogusCount, restored, nullptr), "Accepted an out-of-range entity count");

    wire::Writer overflow(wire::Kind::Snapshot, 1);
    for (size_t i = 0; i < MaxDatagramBytes; ++i) overflow.put(uint8_t{1});
    require(overflow.finish().empty(), "Encoded datagram exceeded the MTU");
}

void controlCodec() {
    const auto roundTrip = [](const wire::InputSample& sample, wire::InputSample& received, bool truncated = false) {
        wire::Writer writer(wire::Kind::Input, 1);
        wire::putInput(writer, sample);
        const auto bytes = writer.finish();
        wire::Reader reader(truncated ? bytes.first(bytes.size() - 1) : bytes);
        wire::Header header;
        return wire::readHeader(reader, header) && header.kind == wire::Kind::Input &&
            wire::readInput(reader, received) && reader.done();
    };
    wire::InputSample sent{{-78, 97, 61021, -15891, 0x8411, 0x81, true}, {0, 9, 11, 65432, 15, 21, 34, 55}};
    wire::InputSample received{};
    require(roundTrip(sent, received) && received.input.hasControls && received.input.x == -78 &&
        received.input.z == 97 && received.input.yaw == 61021 && received.input.pitch == -15891 &&
        received.input.buttons == 0x8411 && received.input.pressed == 0 && received.actionCounts == sent.actionCounts,
        "Control codec lost aim, normalized movement, held buttons, or cumulative action counts");
    require(!roundTrip(sent, received, true), "Control codec accepted an incomplete command");
    sent.input.pitch = MaxControlPitch + 1;
    require(!roundTrip(sent, received), "Control codec accepted a pitch outside the supported range");
    sent.input.pitch = 0;
    sent.input.x = 127;
    sent.input.z = 127;
    require(!roundTrip(sent, received), "Control codec accepted excessive movement magnitude");
    sent.input.hasControls = false;
    received = {};
    require(roundTrip(sent, received) && !received.input.hasControls && received.input.x == 127 && received.input.z == 127,
        "Legacy input changed its wire semantics");
}

void peerNameCodec() {
    constexpr std::string_view name = "\xD0\x96\xE7\x8C\xAB\xF0\x9F\x9A\x80";
    const auto padded = "  " + std::string(name) + ' ';
    require(normalizePeerName(padded) == name && normalizePeerName("   ") == "",
        "Peer names lost UTF-8 or retained surrounding spaces");
    const auto tooLong = std::string(MaxPeerNameBytes - 1, 'a') + std::string(name);
    require(!normalizePeerName(tooLong) &&
        normalizePeerName(tooLong, true) == std::string_view(tooLong).substr(0, MaxPeerNameBytes - 1),
        "Peer name limit split UTF-8");
    require(!normalizePeerName(std::string_view("nick\0suffix", 11)) &&
        !normalizePeerName("nick\nsuffix") && !normalizePeerName("\xE2\x80\xAE") &&
        !normalizePeerName("\xC0\x80") && !normalizePeerName("\xED\xA0\x80") &&
        !normalizePeerName("\xF4\x90\x80\x80") && !normalizePeerName("\xF0\x9F"),
        "Peer name accepted malformed UTF-8, controls, or directional overrides");

    const auto fullName = std::string(MaxPeerNameBytes - name.size(), 'a') + std::string(name);
    constexpr uint64_t steamUser = 0x011000010000002a;
    std::array<wire::PeerNameEntry, wire::PeerNamePageSize> entries;
    for (size_t i = 0; i < entries.size(); ++i) entries[i] = {static_cast<EntityId>(i + 1), fullName, steamUser + i};
    const auto read = [](std::span<const uint8_t> bytes, wire::PeerNamePage& page) {
        wire::Reader reader(bytes);
        wire::Header header;
        return wire::readHeader(reader, header) && header.kind == wire::Kind::PeerNames &&
            wire::readPeerNames(reader, page);
    };
    auto packet = wire::encodePeerNames(42, 3, 23, entries);
    const auto bytes = packet.finish();
    wire::PeerNamePage decoded;
    require(!bytes.empty() && bytes.size() <= MaxDatagramBytes && read(bytes, decoded) &&
        decoded.index == 3 && decoded.revision == 23 && decoded.count == entries.size() &&
        decoded.entries == entries, "Maximum-sized peer names and Steam IDs could not round trip within a datagram");
    require(!read(bytes.first(bytes.size() - 1), decoded), "Accepted a truncated peer name page");
    entries.back().id = entries[0].id;
    auto duplicate = wire::encodePeerNames(42, 3, 24, entries);
    require(!read(duplicate.finish(), decoded), "Accepted duplicate peer names within a page");
    auto invalidPage = wire::encodePeerNames(42, static_cast<uint8_t>(wire::PeerNamePages), 1, {});
    require(!read(invalidPage.finish(), decoded), "Accepted an out-of-bounds peer name page");

    std::array<wire::PeerNameEntry, 2> pending{{{42, {}, steamUser}, {43, name, 0}}};
    auto identities = wire::encodePeerNames(42, 3, 25, pending);
    require(read(identities.finish(), decoded) && decoded.count == pending.size() &&
        decoded.entries[0] == pending[0] && decoded.entries[1] == pending[1],
        "Avatar identity could not replicate independently of persona name or alongside UDP nicknames");
    pending[0].steamId = 0;
    auto empty = wire::encodePeerNames(42, 3, 26, pending);
    require(!read(empty.finish(), decoded), "Accepted a peer identity with neither name nor Steam ID");
    pending[0] = {42, fullName, 42};
    auto invalidIdentity = wire::encodePeerNames(42, 3, 27, pending);
    require(!read(invalidIdentity.finish(), decoded), "Accepted an invalid Steam identity alongside a valid name");
}

void transportEmulationCounters() {
    DatagramTransport sender;
    DatagramTransport receiver;
    std::string error;
    require(sender.start({Loopback, 0}, error) && receiver.start({Loopback, 0}, error),
        "Could not bind transports for emulation accounting");
    const std::array<uint8_t, 1> data{42};
    sender.setEmulation({0, 100});
    const auto invalid = sender.sendWithResult({Loopback, 0}, data);
    const auto dropped = sender.sendWithResult(receiver.localEndpoint(), data);
    const auto bypass = sender.sendWithResult(receiver.localEndpoint(), data, EmulationPolicy::Bypass);
    const auto passed = sender.sendWithResult(receiver.localEndpoint(), data, EmulationPolicy::Apply,
        NetworkEmulation{});
    const auto stats = sender.stats();
    require(!invalid.accepted && !invalid.consideredByEmulator && !invalid.emulatedDrop &&
        dropped.accepted && dropped.consideredByEmulator && dropped.emulatedDrop &&
        bypass.accepted && !bypass.consideredByEmulator && !bypass.emulatedDrop &&
        passed.accepted && passed.consideredByEmulator && !passed.emulatedDrop &&
        stats.emulatedPackets == 2 && stats.emulatedDrops == 1,
        "Transport emulator accounting mixed invalid, bypassed, ideal, or dropped sends");
}

class Game final : public ServerGame {
public:
    EntityState spawn(EntityId id, size_t slot) override {
        return {id, slot == 0 ? -2000 : 2000, 0, 0, 0, 0, static_cast<uint8_t>(slot)};
    }

    RpcStatus rpc(EntityState& player, const RpcCommand& command) override {
        ++rpcCalls;
        if (command.action != 1) return RpcStatus::UnknownAction;
        if (command.target != player.id) return RpcStatus::Forbidden;
        if (command.argument != 123) return RpcStatus::InvalidArgument;
        lastRpcText = command.text;
        ++player.value;
        return RpcStatus::Accepted;
    }

    void despawn(EntityId id) override { despawned.push_back(id); }

    void input(EntityId id, PlayerInput input) override {
        if (id != controlledPeer) return;
        lastInput = input;
        for (unsigned action = 0; action < ControlActionCount; ++action) {
            if (input.pressed & (1U << action)) ++actionCounts[action];
        }
    }

    size_t rpcCalls{};
    std::string lastRpcText;
    std::vector<EntityId> despawned;
    EntityId controlledPeer{};
    PlayerInput lastInput{};
    std::array<unsigned, ControlActionCount> actionCounts{};
};

void simulateServer(EntityState& player, PlayerInput input) {
    player.x += static_cast<int32_t>(input.x) * 100 / 127;
    player.z += static_cast<int32_t>(input.z) * 100 / 127;
    if (input.x != 0) player.yaw = input.x > 0 ? 16384 : 49152;
}

void simulateClient(EntityState& player, PlayerInput input) {
    simulateServer(player, input);
    if (input.x > 0) player.x += 40;
}

std::optional<VisualEntity> entity(const NetworkClient& client, EntityId id) {
    const auto entities = client.presentation();
    const auto found = std::ranges::find(entities, id, &VisualEntity::id);
    if (found == entities.end()) return std::nullopt;
    return *found;
}

class Fixture {
public:
    Fixture() {
        ServerConfig config;
        config.bind = {Loopback, 0};
        config.maxPeers = 2;
        config.relevanceRadiusMm = 6000;
        require(server.start(config, error), "Failed to bind a loopback server");
        require(first.connect(server.localEndpoint(), error), "Failed to connect first client");
        until([&] { return entity(first, first.localEntity()).has_value(); }, "First client did not join");
        require(second.connect(server.localEndpoint(), error), "Failed to connect second client");
        until([&] { return first.presentation().size() == 2 && second.presentation().size() == 2; },
            "Two clients did not see one another");
    }

    void tick(PlayerInput firstInput = {}, PlayerInput secondInput = {}) {
        first.tick(firstInput);
        second.tick(secondInput);
        server.tick();
        std::this_thread::sleep_for(1ms);
    }

    template<class Check>
    void until(Check check, std::string_view message, size_t ticks = 200) {
        for (size_t i = 0; i < ticks; ++i) {
            if (check()) return;
            tick();
        }
        require(check(), message);
    }

    RpcResult awaitRpc(uint32_t request) {
        require(request != 0, "Connected client could not queue RPC");
        RpcResult result;
        until([&] {
            if (result.request == request) return true;
            return first.takeResult(result) && result.request == request;
        }, "RPC completion was not delivered", 220);
        return result;
    }

    Game game;
    NetworkServer server{game, &simulateServer};
    NetworkClient first{&simulateClient};
    NetworkClient second{&simulateServer};
    std::string error;
};

void peerNameReplication() {
    Fixture fixture;
    const auto firstId = fixture.first.localEntity();
    const auto secondId = fixture.second.localEntity();
    const auto localHost = fixture.server.createReplica({});
    const auto nickname = "  \xD0\x96\xE7\x8C\xAB\xF0\x9F\x9A\x80  ";
    const auto invalid = std::string(MaxPeerNameBytes + 1, 'a');
    require(fixture.first.call({1, firstId, 123, invalid}) == 0 &&
        fixture.first.call({1, firstId, 123, "Nick\n"}) == 0 &&
        !fixture.server.setPeerName(firstId, invalid) &&
        !fixture.server.setPeerName(keyedReplicaId(1), "Reserved") &&
        !fixture.server.setPeerName(0, "Unknown"), "Accepted an invalid nickname or an invalid slot");
    const auto result = fixture.awaitRpc(fixture.first.call({1, firstId, 123, nickname}));
    require(result.status == RpcStatus::Accepted && fixture.game.lastRpcText == nickname,
        "UTF-8 RPC text was lost or otherwise modified");
    require(!fixture.server.setLocalSteamIdentity(localHost) && fixture.server.peerSteamId(localHost) == 0 &&
        fixture.server.peerSteamId(firstId) == 0 &&
        fixture.server.setPeerName(localHost, "Listen host") &&
        fixture.server.setPeerName(firstId, fixture.game.lastRpcText) &&
        !fixture.server.setPeerName(secondId, "Bad\nname"), "Server could not apply a validated nickname");
    fixture.until([&] {
        return fixture.first.peerName(firstId) == normalizePeerName(nickname) &&
            fixture.second.peerName(firstId) == fixture.server.peerName(firstId) &&
            fixture.second.peerName(localHost) == "Listen host";
    }, "Remote and listen-host names did not replicate across pages");
    require(fixture.second.peerSteamId(firstId) == 0 && fixture.second.peerSteamId(localHost) == 0,
        "UDP peer metadata contained a Steam identity");
    require(fixture.server.setPeerEmulation(fixture.second.connectionNonce(), {0, 100}) &&
        fixture.server.setPeerName(firstId, "Updated"), "Could not configure peer name loss scenario");
    for (size_t i = 0; i < TickRate / 2; ++i) fixture.tick();
    require(fixture.first.peerName(firstId) == "Updated" && fixture.second.peerName(firstId) != "Updated" &&
        fixture.server.setPeerEmulation(fixture.second.connectionNonce(), {}), "Name ignored per-peer packet loss");
    fixture.until([&] { return fixture.second.peerName(firstId) == "Updated"; },
        "Peer nickname did not recover after packet loss");

    fixture.first.disconnect();
    require(fixture.first.peerName(firstId).empty() && fixture.first.peerName(localHost).empty(),
        "Disconnected client retained names");
    require(fixture.server.destroyReplica(localHost), "Could not retire the listen host replica");
    fixture.until([&] {
        return fixture.server.peerName(firstId).empty() && fixture.second.peerName(firstId).empty() &&
            fixture.second.peerName(localHost).empty();
    }, "Retired peer names remained visible on a connected client");
    require(fixture.first.connect(fixture.server.localEndpoint(), fixture.error), "Nickname test could not reconnect");
    fixture.until([&] { return fixture.first.phase() == ClientPhase::Connected; }, "Nickname test reconnect timed out");
    require(fixture.first.localEntity() != firstId && fixture.first.peerName(firstId).empty() &&
        fixture.first.peerName(fixture.first.localEntity()).empty(), "Reconnect reused a departed player's nickname");
}

void remotePlaybackStaysContinuousAcrossSnapshotGaps() {
    Game game;
    NetworkServer server(game, &simulateServer);
    NetworkClient client(&simulateServer);
    const ServerConfig config{.bind = {Loopback, 0}, .maxPeers = 1, .deferSnapshots = true};
    std::string error;
    require(server.start(config, error) && client.connect(server.localEndpoint(), error),
        "Could not start remote playback fixture");
    const auto replica = server.createReplica({});
    int32_t position = 0;
    uint8_t flags = 0;
    const auto step = [&] {
        client.tick({});
        server.tick();
        position += 100;
        require(server.setReplica({.id = replica, .x = position, .flags = flags}), "Could not move the remote fixture replica");
        server.publishSnapshots();
        std::this_thread::sleep_for(1ms);
    };
    const auto presented = [&](float fraction) -> std::optional<float> {
        const auto rows = client.presentation(fraction);
        const auto found = std::ranges::find(rows, replica, &VisualEntity::id);
        if (found == rows.end()) return {};
        return found->x;
    };
    for (unsigned i = 0; i < 150 && !presented(0); ++i) step();
    require(presented(0).has_value(), "No remote playback replica after join");
    for (unsigned i = 0; i < 24; ++i) step();

    auto previous = *presented(0);
    unsigned steadySteps = 0;
    const auto sample = [&] {
        const auto frameEnd = presented(1);
        step();
        const auto current = presented(0);
        require(frameEnd && current && *current >= *frameEnd - 0.0001F,
            "Remote interpolation played backwards across a tick boundary");
        const auto distance = *current - previous;
        require(distance >= 0 && distance <= 0.111F, "Remote interpolation jumped across simulation ticks");
        if (std::abs(distance - 0.1F) < 0.005F) ++steadySteps;
        previous = *current;
    };
    for (unsigned i = 0; i < 42; ++i) sample();
    require(steadySteps >= 28, "Clean 20 Hz snapshots failed to produce steady movement across 60 Hz ticks");

    const auto peer = client.connectionNonce();
    require(server.setPeerEmulation(peer, {0, 100}), "Could not introduce a snapshot gap");
    for (unsigned i = 0; i < 24; ++i) sample();
    require(server.setPeerEmulation(peer, {}), "Could not resume snapshot delivery");
    const auto beforeRecovery = previous;
    for (unsigned i = 0; i < 40; ++i) sample();
    require(previous > beforeRecovery + 2.5F, "Remote playback did not recover after a burst of packet loss");

    flags = 2;
    bool observedBufferedFlags = false;
    bool observedNewSample = false;
    for (unsigned i = 0; i < 50 && !observedNewSample; ++i) {
        step();
        const auto rows = client.presentation();
        const auto found = std::ranges::find(rows, replica, &VisualEntity::id);
        if (found == rows.end() || found->flags != flags) continue;
        observedBufferedFlags |= found->sampledFlags == 0;
        observedNewSample = found->sampledFlags == flags;
    }
    require(observedBufferedFlags && observedNewSample,
        "Interpolated pose flags did not stay on the old sample until playback reached the new one");
}

void remotePlaybackStaysNearLatestOnSlowClient() {
    Game game;
    NetworkServer server(game, &simulateServer);
    NetworkClient client(&simulateServer);
    const ServerConfig config{.bind = {Loopback, 0}, .maxPeers = 1, .deferSnapshots = true};
    std::string error;
    require(server.start(config, error) && client.connect(server.localEndpoint(), error),
        "Could not start slow playback fixture");
    constexpr auto replica = keyedReplicaId(1);
    constexpr int32_t movementMmPerTick = 100;
    constexpr auto maxPlaybackLagTicks = TickRate * 300 / 1000;
    constexpr auto maxPlaybackDistance = maxPlaybackLagTicks * movementMmPerTick * 0.001F;
    require(server.createKeyedReplica(1, {}), "Could not create the slow playback fixture replica");
    int32_t position = 0;
    const auto step = [&](bool updateClient) {
        if (updateClient) client.tick({});
        server.tick();
        position += movementMmPerTick;
        require(server.setReplica({.id = replica, .x = position}), "Could not move the slow playback fixture replica");
        server.publishSnapshots();
        std::this_thread::sleep_for(1ms);
    };
    const auto checkFreshness = [&] {
        const auto latest = client.latestReplica(replica);
        require(latest && latest->entity.x >= position - 1000, "Slow client stopped receiving fresh snapshots");
        const auto rendered = entity(client, replica);
        require(rendered && latest->entity.x * 0.001F - rendered->x <= maxPlaybackDistance + 0.0001F,
            "Remote playback fell more than 300 ms behind fresh snapshots");
    };

    for (unsigned i = 0; i < TickRate; ++i) step(true);
    checkFreshness();
    auto previous = entity(client, replica)->x;
    const auto stepObserver = [&] {
        step(false);
        step(true);
        const auto rendered = entity(client, replica);
        require(rendered && rendered->x >= previous, "Slow client played remote movement backwards");
        previous = rendered->x;
    };
    for (unsigned i = 0; i < 4 * TickRate; ++i) stepObserver();
    checkFreshness();

    for (unsigned i = 0; i < TickRate / 2; ++i) step(false);
    for (unsigned i = 0; i < TickRate / 4; ++i) stepObserver();
    checkFreshness();
}

void controlsAndPeerValueReplication() {
    Fixture fixture;
    const auto id = fixture.first.localEntity();
    fixture.game.controlledPeer = id;
    require(fixture.server.peerValue(id) == 0 && fixture.first.peerValue(id) == 0,
        "A new peer did not begin with an unassigned value");
    require(fixture.server.setPeerValue(id, 7) && fixture.server.peerValue(id) == 7 &&
        !fixture.server.setPeerValue(keyedReplicaId(7), 7), "Could not set the exact peer's authoritative value");
    fixture.until([&] { return fixture.first.peerValue(id) == 7 && fixture.second.peerValue(id) == 7; },
        "Latest peer value did not replicate to the owner and another client");

    const PlayerInput controls{64, -97, 40791, -8192, 0x20, 0, true};
    for (int tick = 0; tick < 12; ++tick) fixture.tick(controls);
    require(fixture.game.lastInput.hasControls && fixture.game.lastInput.x == 64 && fixture.game.lastInput.z == -97 &&
        fixture.game.lastInput.yaw == 40791 && fixture.game.lastInput.pitch == -8192 &&
        fixture.game.lastInput.buttons == 0x20, "Live delivered controls lost a field");

    fixture.first.setEmulation({0, 100});
    auto pressed = controls;
    pressed.pressed = 0x81;
    fixture.tick(pressed);
    for (unsigned tick = 0; tick < wire::MaxInputBatch + 4; ++tick) fixture.tick(controls);
    require(!fixture.game.lastInput.hasControls && fixture.game.actionCounts[0] == 0 &&
        fixture.game.actionCounts[7] == 0, "Dropped commands left stale held input or fired an action prematurely");
    fixture.first.setEmulation({});
    for (int tick = 0; tick < 36; ++tick) fixture.tick(controls);
    require(fixture.game.lastInput.hasControls && fixture.game.actionCounts[0] == 1 &&
        fixture.game.actionCounts[7] == 1, "Action recovery failed or replayed an edge across a lost input batch");

    fixture.first.disconnect();
    fixture.until([&] { return !fixture.server.peerValue(id) && !fixture.second.peerValue(id); },
        "A disconnected peer remained readable");
    require(fixture.first.connect(fixture.server.localEndpoint(), fixture.error), "Could not reconnect the control client");
    fixture.until([&] { return fixture.first.peerValue(fixture.first.localEntity()) == 0; },
        "A fresh connection inherited a previous peer value");
    require(fixture.first.localEntity() != id, "A reconnect reused a departed peer's identity");
    fixture.game.controlledPeer = fixture.first.localEntity();
    for (int tick = 0; tick < 12; ++tick) fixture.tick(controls);
    require(fixture.game.actionCounts[0] == 1 && fixture.game.actionCounts[7] == 1,
        "A fresh connection replayed the previous connection's action counts");
}

void holdsControlsAcrossOneMissingTick() {
    Fixture fixture;
    fixture.game.controlledPeer = fixture.first.localEntity();
    const PlayerInput moveAndJump{127, 0, 8192, 0, 0x101, 1, true};
    fixture.first.tick(moveAndJump);
    for (int i = 0; i < 60 && !fixture.game.lastInput.pressed; ++i) {
        std::this_thread::sleep_for(1ms);
        fixture.server.tick();
    }
    require(fixture.game.lastInput.pressed == 1, "The initial input did not reach the server");
    fixture.server.tick();
    const auto held = fixture.game.lastInput;
    require(held.hasControls && held.x == 127 && held.buttons == moveAndJump.buttons && held.yaw == moveAndJump.yaw &&
        held.pressed == 0 && fixture.game.actionCounts[0] == 1,
        "One missing sample interrupted held controls or repeated a jump");
    fixture.server.tick();
    require(!fixture.game.lastInput.hasControls && fixture.game.lastInput.x == 0,
        "Held controls did not expire after the grace tick");
}

void deferredSnapshotMatchesProcessedInput() {
    Game game;
    const auto noSimulation = [](EntityState&, PlayerInput) {};
    NetworkServer server(game, noSimulation);
    NetworkClient client(noSimulation);
    ServerConfig config;
    config.bind = {Loopback, 0};
    config.maxPeers = 1;
    config.deferSnapshots = true;
    std::string error;
    require(server.start(config, error), "Could not start deferred scene server");
    const auto pawn = keyedReplicaId(42);
    require(server.createKeyedReplica(42, {.x = 1500}) && client.nextInputSequence() == 0 &&
        !client.latestReplica(pawn), "Deferred scene exposed state before connecting");
    require(client.connect(server.localEndpoint(), error), "Could not connect deferred scene client");
    const auto beginTick = [&](PlayerInput input = {}) {
        client.tick(input);
        const auto sent = server.stats().snapshotsSent;
        server.tick();
        require(server.stats().snapshotsSent == sent, "Deferred server published before the gameplay tick");
    };
    const auto endTick = [&] {
        server.publishSnapshots();
        const auto sent = server.stats().snapshotsSent;
        server.publishSnapshots();
        require(server.stats().snapshotsSent == sent, "Deferred server published twice in a tick");
        std::this_thread::sleep_for(1ms);
    };
    for (unsigned i = 0; i < 200 && !client.latestReplica(pawn); ++i) {
        beginTick();
        endTick();
    }
    const auto initial = client.latestReplica(pawn);
    const auto peer = client.localEntity();
    require(initial && initial->entity.x == 1500 && initial->tick > 0 && peer > 0,
        "Deferred scene client did not receive raw keyed state");
    require(!client.latestReplica(keyedReplicaId(43)) && server.setPeerValue(peer, 7) &&
        !server.setPeerMotion(pawn, 0, 0, 0, 0, 0), "Peer motion accepted a non-peer replica");
    game.controlledPeer = peer;
    const auto input = client.nextInputSequence();
    require(input > initial->inputAck, "The next input must follow the authoritative acknowledgement");
    bool sawProcessedSnapshot = false;
    for (unsigned tick = 0; tick < 200; ++tick) {
        beginTick(tick == 0 ? PlayerInput{127, 0, 0, 0, 0, 1, true} : PlayerInput{});
        require(client.nextInputSequence() == input + tick + 1, "The outgoing sequence did not track each client tick");
        if (game.lastInput.pressed == 1) {
            require(server.setReplica({.id = pawn, .x = 2137}) &&
                server.setPeerMotion(peer, 6432, -1100, -2450, 75, 100),
                "Could not publish gameplay movement after processing input");
        }
        const auto latest = client.latestReplica(pawn);
        if (latest && latest->inputAck >= input) {
            const auto motion = client.latestReplica(peer);
            require(latest->tick > initial->tick && latest->entity.x == 2137 && motion &&
                motion->inputAck == latest->inputAck && motion->tick == latest->tick &&
                motion->entity.x == 6432 && motion->entity.y == -1100 && motion->entity.z == -2450 &&
                motion->entity.yaw == 75 && motion->entity.pitch == 100 &&
                motion->entity.value == 7 && (motion->entity.flags & PeerMotionValid),
                "Acknowledgement reached the client ahead of its authoritative pawn and motion");
            sawProcessedSnapshot = true;
        }
        endTick();
        if (sawProcessedSnapshot) break;
    }
    require(sawProcessedSnapshot && game.actionCounts[0] == 1, "Deferred snapshot did not confirm the input exactly once");
    client.disconnect();
    require(client.nextInputSequence() == 0 && !client.latestReplica(pawn),
        "A disconnected client kept its raw state or input sequence");
}

void temporaryDelayClearsRedundantControlBacklog() {
    Fixture fixture;
    const auto peer = fixture.first.localEntity();
    fixture.game.controlledPeer = peer;
    const PlayerInput held{65, 0, 1200, 500, 0x20, 0, true};
    unsigned turns{};
    const auto turning = [&] {
        auto input = held;
        input.yaw += static_cast<uint16_t>(++turns * 13);
        return input;
    };
    std::vector<PlayerInput> edges;
    auto deadline = std::chrono::steady_clock::now();
    const auto step = [&](PlayerInput input) {
        fixture.tick(input);
        const auto last = fixture.game.lastInput;
        if (last.pressed) edges.push_back(last);
        deadline += 16667us;
        std::this_thread::sleep_until(deadline);
    };
    const auto commandAge = [&] {
        const auto snapshot = fixture.first.latestReplica(peer);
        const auto next = fixture.first.nextInputSequence();
        require(snapshot && next > snapshot->inputAck, "Cannot measure command age without an acknowledgement");
        return next - 1 - snapshot->inputAck;
    };
    for (int i = 0; i < 24; ++i) step(turning());
    uint64_t baseline = 0;
    for (int i = 0; i < 18; ++i) {
        step(turning());
        baseline += commandAge();
    }
    fixture.first.setEmulation({120, 0});
    for (int i = 0; i < 24; ++i) step(turning());
    fixture.first.setEmulation({});
    auto first = held;
    first.x = -45;
    first.buttons = 0x80;
    first.pressed = 1;
    step(first);
    step(turning());
    auto second = held;
    second.yaw = 9100;
    second.pitch = -600;
    second.pressed = 1;
    step(second);
    for (int i = 0; i < 24; ++i) step(turning());
    uint64_t recovered = 0;
    for (int i = 0; i < 18; ++i) {
        step(turning());
        recovered += commandAge();
    }
    require(recovered <= baseline + 4 * 18, "Redundant commands retained the temporary 120 ms latency");
    require(edges.size() == 2 && fixture.game.actionCounts[0] == 2 &&
        edges[0].x == first.x && edges[0].buttons == first.buttons &&
        edges[1].yaw == second.yaw && edges[1].pitch == second.pitch,
        "Control coalescing dropped, replayed or changed a one-shot input");
}

void slowerServerKeepsAcceptingInput() {
    Fixture fixture;
    const auto peer = fixture.first.localEntity();
    fixture.game.controlledPeer = peer;

    const auto snapshot = [&] {
        const auto state = fixture.first.latestReplica(peer);
        require(state && fixture.first.phase() == ClientPhase::Connected,
            "Tick skew lost the connected player's snapshot");
        return *state;
    };
    const auto slowerServerTick = [&](const std::array<PlayerInput, 3>& inputs) {
        for (auto input : inputs) fixture.first.tick(input);
        fixture.second.tick({});
        std::this_thread::sleep_for(1ms);
        fixture.server.tick();
    };

    const PlayerInput right{127, 0};
    const PlayerInput left{-127, 0};
    const auto before = snapshot();
    for (auto tick = 0; tick < 300; ++tick) slowerServerTick({right, right, right});
    const auto moved = snapshot();
    require(fixture.server.stats().invalidMessages == 0,
        "A slower server started rejecting normal input based on its cumulative tick count");
    require(moved.inputAck + 24 >= fixture.first.nextInputSequence() && moved.entity.x > before.entity.x,
        "A slower server stopped applying recent input or accumulated latency on a held direction");
    require(moved.entity.x - before.entity.x <= 300 * 100,
        "Input bursts made the server run more than one movement command per simulation tick");

    auto previousAck = moved.inputAck;
    auto largestAckStep = uint64_t{};
    for (auto tick = 0; tick < 300 && largestAckStep <= 80; ++tick) {
        slowerServerTick(tick % 2 == 0 ? std::array{left, right, left} : std::array{right, left, right});
        const auto updated = snapshot().inputAck;
        largestAckStep = std::max(largestAckStep, updated - previousAck);
        previousAck = updated;
    }
    require(largestAckStep > 80, "A backlog larger than the input ring did not rebase to recent commands");
    for (auto tick = 0; tick < 90; ++tick) fixture.tick(left);
    const auto recovered = snapshot();
    require(recovered.inputAck + 24 >= fixture.first.nextInputSequence() && fixture.game.lastInput.x == -127 &&
        fixture.server.stats().invalidMessages == 0,
        "Normal movement did not recover in the same session after the enlarged backlog");
}

void rejectedPacketsAndCapacity(Fixture& fixture) {
    DatagramTransport probe;
    require(probe.start({Loopback, 0}, fixture.error), "Could not bind protocol probe");
    wire::Writer hello(wire::Kind::Hello, 0);
    constexpr uint64_t nonce = 123;
    hello.put(nonce);
    const auto encoded = hello.finish();
    std::vector<uint8_t> packet(encoded.begin(), encoded.end());
    packet[4] = static_cast<uint8_t>(wire::Version == 255 ? 1 : wire::Version + 1);
    require(probe.send(fixture.server.localEndpoint(), packet), "Could not send version mismatch");

    Datagram reply;
    fixture.until([&] { return reply.size != 0 || probe.receive(reply); }, "Incompatible version had no reply");
    wire::Reader reader(std::span(reply.data).first(reply.size));
    wire::Header header;
    require(wire::readHeader(reader, header) && header.kind == wire::Kind::Reject && !header.session &&
        reader.get<uint64_t>() == nonce && reader.get<uint8_t>() == static_cast<uint8_t>(wire::Reject::Incompatible) &&
        reader.done(), "Incompatible version was not explicitly rejected");

    const auto invalidBefore = fixture.server.stats().invalidMessages;
    const std::array<uint8_t, 5> truncated{'T', 'G', 'E', '1', wire::Version};
    require(probe.send(fixture.server.localEndpoint(), truncated), "Could not send malformed frame");
    fixture.until([&] { return fixture.server.stats().invalidMessages > invalidBefore; },
        "Malformed datagram was not counted");

    NetworkClient extra(&simulateServer);
    require(extra.connect(fixture.server.localEndpoint(), fixture.error), "Could not start third connection");
    fixture.until([&] {
        extra.tick({});
        return extra.phase() == ClientPhase::Rejected;
    }, "Full server did not reject third connection");
    require(extra.diagnostic().find("full") != std::string_view::npos && fixture.server.peerCount() == 2,
        "Capacity rejection changed the existing connections");
}

void movementAndReplication(Fixture& fixture, EntityId firstId, EntityId secondId) {
    const auto start = entity(fixture.first, firstId).value();
    fixture.first.tick({127, 0});
    const auto predicted = entity(fixture.first, firstId).value();
    require(predicted.x > start.x + 0.1F && predicted.yaw > 1.5F, "Local prediction did not apply input immediately");
    fixture.first.setPrediction(false);
    require(std::abs(entity(fixture.first, firstId).value().x - start.x) < 0.001F,
        "Prediction toggle did not present authoritative state");
    fixture.first.setPrediction(true);
    fixture.until([&] { return fixture.first.stats().corrections != 0; }, "Missing authoritative reconciliation");
    require(std::abs(entity(fixture.first, firstId).value().x - (start.x + 0.1F)) < 0.001F,
        "Reconciliation did not restore authoritative position");

    bool observedInterpolation = false;
    for (int i = 0; i < 16; ++i) {
        fixture.tick({}, {127, 0});
        const auto smoothed = entity(fixture.first, secondId).value();
        fixture.first.setInterpolation(false);
        const auto discrete = entity(fixture.first, secondId).value();
        fixture.first.setInterpolation(true);
        observedInterpolation |= discrete.x - smoothed.x > 0.01F;
    }
    require(observedInterpolation, "Interpolation toggle produced no difference during remote motion");
    for (int i = 0; i < 20; ++i) fixture.tick({}, {127, 0});
    fixture.until([&] { return !entity(fixture.first, secondId) && !entity(fixture.second, firstId); },
        "Out-of-range replicas were not removed");
    for (int i = 0; i < 36; ++i) fixture.tick({}, {-127, 0});
    fixture.until([&] { return entity(fixture.first, secondId) && entity(fixture.second, firstId); },
        "Replicas did not return on reentry to interest radius");

    const auto stats = fixture.server.stats();
    require(stats.snapshotsSent > 0 && stats.snapshotBytes < stats.fullSnapshotBytes &&
        fixture.first.stats().snapshotsReceived > 0, "Acknowledged baseline did not reduce encoded bytes");
}

void serverOwnedReplicas(Fixture& fixture, EntityId firstId) {
    EntityState relay{firstId, -6500, 0, 0, 0, 5, 50};
    require(!fixture.server.setReplica(relay) && !fixture.server.destroyReplica(firstId),
        "Server-owned replica API accepted a peer-owned id");
    constexpr auto key = 123456U;
    require(fixture.server.createKeyedReplica(key, relay) && !fixture.server.createKeyedReplica(key, relay) &&
        !fixture.server.createKeyedReplica(0, relay) && !fixture.server.createKeyedReplica(MaxReplicaKey + 1, relay),
        "Keyed replica did not enforce reserved or duplicate keys");
    relay.id = keyedReplicaId(key);
    require(relay.id != firstId && replicaKey(relay.id) == key && !replicaKey(firstId),
        "Keyed replica id overlapped a peer");
    fixture.until([&] { return entity(fixture.first, relay.id).has_value(); }, "New replica did not appear");
    require(!entity(fixture.second, relay.id), "Server replicated an out-of-range entity to the second peer");

    relay.x = 0;
    relay.y = 300;
    relay.pitch = 16384;
    relay.roll = 32768;
    relay.value = 10;
    require(fixture.server.setReplica(relay), "Could not update a server-owned replica");
    fixture.until([&] {
        return entity(fixture.first, relay.id).value_or(VisualEntity{}).value == 10 &&
            entity(fixture.second, relay.id).value_or(VisualEntity{}).value == 10;
    }, "Updated replica did not enter the second peer's interest radius");
    require(std::abs(entity(fixture.second, relay.id).value().y - 0.3F) < 0.001F, "Replica height was not updated");
    require(std::abs(entity(fixture.second, relay.id).value().pitch - 1.5708F) < 0.001F &&
        std::abs(entity(fixture.second, relay.id).value().roll - 3.14159F) < 0.001F,
        "Replica pitch or roll was not updated");
    require(fixture.server.destroyReplica(relay.id) && !fixture.server.setReplica(relay),
        "Destroyed replica could still be updated");
    fixture.until([&] { return !entity(fixture.first, relay.id) && !entity(fixture.second, relay.id); },
        "Destroyed server-owned replica was still visible");

    std::array<EntityId, MaxReplicas - 2> replicas{};
    for (auto& id : replicas) {
        id = fixture.server.createReplica({});
        require(id != 0, "Replica budget rejected a valid entity");
    }
    require(fixture.server.createReplica({}) == 0, "Replica budget did not reserve room for peers");
    fixture.until([&] {
        return fixture.first.presentation().size() == MaxReplicas && fixture.second.presentation().size() == MaxReplicas;
    }, "Maximum replica set did not fit in a packet");
    for (int i = 0; i < 12; ++i) fixture.tick();
    const auto previousReplica = replicas.front();
    for (const auto id : replicas) require(fixture.server.destroyReplica(id), "Failed to retire old replica set");
    for (auto& id : replicas) {
        id = fixture.server.createReplica({});
        require(id != 0, "Failed to replace replica set");
    }
    fixture.until([&] {
        return fixture.first.presentation().size() == MaxReplicas && entity(fixture.first, replicas.back()) &&
            !entity(fixture.first, previousReplica);
    }, "A delta larger than the MTU did not fall back to a complete snapshot");
    for (const auto id : replicas) require(fixture.server.destroyReplica(id), "Failed to clear replica budget fixture");
    fixture.until([&] { return fixture.first.presentation().size() == 2 && fixture.second.presentation().size() == 2; },
        "Bulk despawn did not clear replicas");
}

void rpcRetries(Fixture& fixture, EntityId firstId, EntityId secondId) {
    const auto drops = fixture.first.stats().transport.emulatedDrops;
    const auto before = fixture.game.rpcCalls;
    fixture.first.setEmulation({0, 100});
    const auto request = fixture.first.call({1, firstId, 123});
    fixture.first.tick({});
    fixture.first.setEmulation({12, 0});
    for (int i = 0; i < 36; ++i) {
        fixture.first.tick({});
        std::this_thread::sleep_for(1ms);
    }
    fixture.first.setEmulation({});
    const auto accepted = fixture.awaitRpc(request);
    require(accepted.status == RpcStatus::Accepted && accepted.value == 1 && fixture.game.rpcCalls == before + 1 &&
        fixture.first.stats().transport.emulatedDrops > drops, "Dropped and retried RPC was not applied exactly once");
    fixture.until([&] { return entity(fixture.second, firstId).value_or(VisualEntity{}).value == 1; },
        "Other client did not receive accepted RPC state change");

    const auto reject = [&](RpcCommand command, RpcStatus expected) {
        for (int i = 0; i < 12; ++i) fixture.tick();
        const auto result = fixture.awaitRpc(fixture.first.call(command));
        require(result.status == expected && result.value == 1, "Invalid RPC did not give the expected controlled refusal");
    };
    reject({99, firstId, 123}, RpcStatus::UnknownAction);
    reject({1, secondId, 123}, RpcStatus::Forbidden);
    reject({1, firstId, -1}, RpcStatus::InvalidArgument);
}

void disconnectTimeoutAndReconnect(Fixture& fixture, EntityId firstId, EntityId secondId) {
    fixture.second.disconnect();
    fixture.until([&] { return fixture.server.peerCount() == 1 && !entity(fixture.first, secondId); },
        "Graceful disconnect left an active remote replica", 90);
    require(fixture.second.presentation().empty(), "Local replica survived disconnect");
    require(fixture.second.connect(fixture.server.localEndpoint(), fixture.error), "Could not reconnect second client");
    fixture.until([&] { return fixture.second.localEntity() != 0 && fixture.first.presentation().size() == 2; },
        "Second client did not return after graceful disconnect");
    const auto newSecond = fixture.second.localEntity();
    require(newSecond != secondId, "Reconnect reused the disconnected entity id");

    fixture.first.setEmulation({0, 100});
    const auto result = fixture.awaitRpc(fixture.first.call({1, firstId, 123}));
    require(result.status == RpcStatus::TimedOut, "Total loss did not yield an RPC timeout");
    fixture.until([&] { return fixture.server.peerCount() == 1 && !entity(fixture.second, firstId); },
        "Server did not clean up the disconnected client after traffic loss", 200);
    fixture.until([&] { return fixture.first.phase() == ClientPhase::TimedOut; },
        "Client did not time out after losing snapshots", 350);
    require(fixture.first.presentation().empty() && fixture.second.localEntity() == newSecond &&
        fixture.second.phase() == ClientPhase::Connected, "Traffic loss disturbed a healthy peer or left stale replicas");
    require(std::ranges::count(fixture.game.despawned, firstId) == 1 &&
        std::ranges::count(fixture.game.despawned, secondId) == 1, "Disconnected peer despawned incorrectly");

    fixture.first.setEmulation({});
    require(fixture.first.connect(fixture.server.localEndpoint(), fixture.error), "Could not reconnect after timeout");
    fixture.until([&] { return fixture.first.presentation().size() == 2 && fixture.second.presentation().size() == 2; },
        "Game did not recover after loss and reconnect");
    require(fixture.first.localEntity() != firstId && fixture.second.localEntity() == newSecond,
        "Reconnect failed to preserve the unaffected peer");
}

void gracefulServerShutdown(Fixture& fixture) {
    const auto firstId = fixture.first.localEntity();
    const auto secondId = fixture.second.localEntity();
    const auto request = fixture.first.call({1, firstId, 123});
    require(request != 0, "Could not queue a pending RPC before server stop");
    fixture.server.stop();
    fixture.until([&] {
        return fixture.first.phase() == ClientPhase::Disconnected && fixture.second.phase() == ClientPhase::Disconnected;
    }, "Server stop did not promptly notify both active clients", 90);
    require(fixture.server.peerCount() == 0 && fixture.first.presentation().empty() && fixture.second.presentation().empty() &&
        std::ranges::count(fixture.game.despawned, firstId) == 1 &&
        std::ranges::count(fixture.game.despawned, secondId) == 1, "Server stop did not release peer state");
    RpcResult result;
    require(fixture.first.takeResult(result) && result.request == request && result.status == RpcStatus::Disconnected,
        "Server stop left a pending RPC unresolved");
}

void symmetricNetworkTestEmulation() {
    Game game;
    NetworkServer server(game, simulateServer);
    ServerConfig config;
    config.bind = {Loopback, 0};
    std::string error;
    require(server.start(config, error), "Could not start the emulated server");

    constexpr uint16_t latencyMs = 24;
    server.setEmulation({latencyMs, 25});
    NetworkClient client(simulateServer);
    client.setEmulation({latencyMs, 25}, false);
    require(client.connect(server.localEndpoint(), error), "Could not start the emulated client");
    const auto until = [&](auto condition, const char* message) {
        for (int i = 0; i < 240 && !condition(); ++i) {
            client.tick({});
            server.tick();
            std::this_thread::sleep_for(2ms);
        }
        require(condition(), message);
    };
    until([&] {
        const auto stats = client.stats();
        return stats.snapshotsReceived > 4 && stats.rttMs >= 2 * latencyMs &&
            stats.transport.emulatedDrops > 0 && server.stats().transport.emulatedDrops > 0;
    }, "Game traffic did not show two-way delay and loss");

    const auto request = client.call({1, client.localEntity(), 123});
    require(request != 0, "Could not request an RPC under two-way loss");
    RpcResult result;
    until([&] { return result.request != 0 || client.takeResult(result); }, "RPC did not finish under two-way loss");
    require(result.request == request && result.status == RpcStatus::Accepted && game.rpcCalls == 1,
        "RPC did not deliver a single successful result under two-way loss");
    client.disconnect();
    until([&] { return server.peerCount() == 0; }, "Graceful departure did not bypass emulation");

    server.setEmulation({500, 100});
    client.setEmulation({500, 100}, false);
    require(client.connect(server.localEndpoint(), error), "Could not start the total-loss test");
    until([&] { return client.phase() == ClientPhase::Connected && server.peerCount() == 1; },
        "Total gameplay loss blocked the connection handshake");
    const auto snapshots = client.stats().snapshotsReceived;
    const auto previousDrops = server.stats().transport.emulatedDrops;
    until([&] { return server.stats().transport.emulatedDrops > previousDrops; },
        "Total-loss profile did not drop server gameplay traffic");
    require(client.stats().snapshotsReceived == snapshots, "A snapshot leaked through total gameplay loss");

    client.disconnect();
    until([&] { return server.peerCount() == 0; }, "Total gameplay loss blocked the graceful goodbye");
    require(client.connect(server.localEndpoint(), error), "Could not reconnect during total loss");
    until([&] { return client.phase() == ClientPhase::Connected; }, "Could not rejoin during total gameplay loss");
    server.stop();
    until([&] { return client.phase() == ClientPhase::Disconnected; }, "Total gameplay loss blocked server stop");
}

void perPeerEgressEmulation() {
    Fixture fixture;
    const auto firstId = fixture.first.localEntity();
    const auto secondId = fixture.second.localEntity();
    const auto firstNonce = fixture.first.connectionNonce();
    const auto secondNonce = fixture.second.connectionNonce();
    require(firstNonce && secondNonce && firstNonce != secondNonce &&
        !fixture.server.setPeerEmulation(0, {}) && !fixture.server.peerEmulationCounters(0),
        "Connected peers did not expose unique nonzero nonces");
    require(fixture.server.loopbackPeerForNonce(firstNonce) == firstId &&
        fixture.server.loopbackPeerForNonce(secondNonce) == secondId && !fixture.server.loopbackPeerForNonce(0),
        "Launch-owned nonce did not identify its loopback peer");
    const auto egress = [&](uint64_t nonce) {
        const auto counters = fixture.server.peerEmulationCounters(nonce);
        require(counters.has_value(), "Connected peer had no egress counters");
        return *counters;
    };

    constexpr uint16_t slowLatency = 100;
    constexpr uint16_t healthyLatency = 35;
    fixture.server.setEmulation({0, 100});
    require(fixture.server.setPeerEmulation(firstNonce, {slowLatency, 0}) &&
        fixture.server.setPeerEmulation(secondNonce, {healthyLatency, 0}), "Could not set distinct egress profiles");
    const auto initialFirstSnapshots = fixture.first.stats().snapshotsReceived;
    const auto initialSecondSnapshots = fixture.second.stats().snapshotsReceived;
    fixture.until([&] {
        const auto firstStats = fixture.first.stats();
        const auto secondStats = fixture.second.stats();
        return firstStats.rttMs >= slowLatency && firstStats.snapshotsReceived > initialFirstSnapshots &&
            secondStats.rttMs >= healthyLatency && secondStats.rttMs < slowLatency &&
            secondStats.snapshotsReceived > initialSecondSnapshots;
    }, "Distinct egress profiles did not override global loss and isolate RTT", 240);

    const auto firstIdeal = egress(firstNonce);
    const auto secondIdeal = egress(secondNonce);
    const auto clientIdeal = fixture.first.stats().transport;
    require(firstIdeal.packets && !firstIdeal.dropped && secondIdeal.packets && !secondIdeal.dropped &&
        firstIdeal.packets + secondIdeal.packets == fixture.server.stats().transport.emulatedPackets,
        "Ideal egress links did not count their eligible packets exactly");
    const auto drops = fixture.server.stats().transport.emulatedDrops;
    require(fixture.server.setPeerEmulation(firstNonce, {0, 100}), "Could not cut one live egress link");
    const auto firstRequest = fixture.first.call({1, firstId, 123});
    const auto secondRequest = fixture.second.call({1, secondId, 123});
    require(firstRequest && secondRequest, "Could not queue both gameplay requests");
    RpcResult result{};
    fixture.until([&] { return result.request || fixture.second.takeResult(result); },
        "Healthy peer did not receive an RPC result", 100);
    require(result.request == secondRequest && result.status == RpcStatus::Accepted && result.value == 1,
        "Other link's total loss affected the healthy peer's RPC");
    require(fixture.awaitRpc(firstRequest).status == RpcStatus::TimedOut && fixture.game.rpcCalls == 2,
        "The impaired RPC reply was not lost or the request executed more than once");

    const auto cutSnapshots = fixture.first.stats().snapshotsReceived;
    const auto healthySnapshots = fixture.second.stats().snapshotsReceived;
    fixture.until([&] { return fixture.second.stats().snapshotsReceived > healthySnapshots + 2; },
        "Healthy peer stopped receiving snapshots");
    require(fixture.first.stats().snapshotsReceived == cutSnapshots && fixture.second.stats().rttMs < slowLatency &&
        fixture.server.stats().transport.emulatedDrops > drops, "Live total loss did not isolate the first egress link");
    const auto firstCut = egress(firstNonce);
    const auto secondCut = egress(secondNonce);
    const auto clientCut = fixture.first.stats().transport;
    require(firstCut.packets > firstIdeal.packets &&
        firstCut.packets - firstIdeal.packets == firstCut.dropped - firstIdeal.dropped &&
        secondCut.packets > secondIdeal.packets && secondCut.dropped == secondIdeal.dropped &&
        clientCut.emulatedPackets > clientIdeal.emulatedPackets &&
        clientCut.emulatedDrops == clientIdeal.emulatedDrops,
        "Directional counters did not distinguish total egress loss from the healthy links");
    require(fixture.server.setPeerEmulation(firstNonce, {}), "Could not restore live egress");
    fixture.until([&] { return fixture.first.stats().snapshotsReceived > cutSnapshots; },
        "Restored egress did not resume snapshots");
    require(fixture.first.localEntity() == firstId && fixture.second.localEntity() == secondId,
        "Changing egress settings replaced a live session");

    require(fixture.server.setPeerEmulation(firstNonce, {0, 100}), "Could not restore total loss before departure");
    fixture.first.disconnect();
    require(!fixture.first.connectionNonce(), "Disconnected client still exposes its old nonce");
    fixture.until([&] { return fixture.server.peerCount() == 1; }, "Total egress loss blocked graceful departure");
    require(!fixture.server.setPeerEmulation(firstNonce, {}) && !fixture.server.peerEmulationCounters(firstNonce),
        "Server retained emulation state for a departed peer");
    require(!fixture.server.loopbackPeerForNonce(firstNonce), "Departed peer remained eligible as a local host");
    fixture.server.setEmulation({});
    require(fixture.first.connect(fixture.server.localEndpoint(), fixture.error), "Could not rejoin after egress loss");
    require(fixture.first.phase() == ClientPhase::Connecting && fixture.first.connectionNonce() &&
        fixture.first.connectionNonce() != firstNonce, "Reconnecting client did not expose a fresh nonce");
    fixture.until([&] { return fixture.first.stats().snapshotsReceived > 0; },
        "Rejoined peer inherited the old egress override");
    require(fixture.first.localEntity() != firstId && fixture.second.localEntity() == secondId &&
        !fixture.server.setPeerEmulation(firstNonce, {0, 100}), "Departed nonce affected a rejoined peer");
}

void probeMeasurement() {
    Fixture fixture;
    fixture.until([&] {
        const auto stats = fixture.first.stats();
        return stats.recentRttMs && stats.probes.packets > 0 && stats.probes.dropped == 0;
    }, "A live Pong did not produce one fresh RTT and probe completion");
    require(fixture.server.setPeerEmulation(fixture.first.connectionNonce(), {0, 100}),
        "Could not drop the test client's heartbeat replies");
    for (unsigned i = 0; i < TickRate + 10; ++i) fixture.tick();
    const auto pending = fixture.first.stats();
    std::this_thread::sleep_for(2100ms);
    const auto stale = fixture.first.stats();
    require(!stale.recentRttMs && stale.rttMs == pending.rttMs,
        "Stale RTT remained visible or overwrote the legacy sample");
    fixture.tick();
    const auto expired = fixture.first.stats().probes;
    require(expired.dropped > pending.probes.dropped &&
        expired.packets - pending.probes.packets == expired.dropped - pending.probes.dropped,
        "Expired probes did not count once as loss");

    require(fixture.server.setPeerEmulation(fixture.first.connectionNonce(), {}),
        "Could not restore the test client's heartbeat replies");
    fixture.until([&] {
        const auto stats = fixture.first.stats();
        return stats.recentRttMs && stats.probes.packets > expired.packets && stats.probes.dropped == expired.dropped;
    }, "Probe measurement did not recover after restoring replies");

    constexpr uint16_t oldLatencyMs = 160;
    fixture.first.setEmulation({oldLatencyMs, 0}, false);
    for (unsigned i = 0; i < TickRate / 2; ++i) fixture.first.tick({});
    const auto legacyRtt = fixture.first.stats().rttMs;
    fixture.first.resetProbeMeasurements();
    const auto reset = fixture.first.stats();
    require(!reset.recentRttMs && !reset.probes.packets && !reset.probes.dropped && reset.rttMs == legacyRtt,
        "Profile reset kept old measurements or cleared legacy RTT");

    fixture.first.setEmulation({0, 100}, false);
    std::this_thread::sleep_for(180ms);
    fixture.until([&] { return fixture.first.stats().rttMs >= oldLatencyMs; },
        "A pre-reset Pong could not refresh legacy RTT");
    const auto oldReply = fixture.first.stats();
    require(!oldReply.recentRttMs && !oldReply.probes.packets && !oldReply.probes.dropped,
        "A pre-reset Pong leaked into new measurements");

    fixture.first.setEmulation({}, false);
    fixture.until([&] {
        const auto stats = fixture.first.stats();
        return stats.recentRttMs && stats.probes.packets > 0 && !stats.probes.dropped;
    }, "Post-reset probes did not restart measurement");
    fixture.first.disconnect();
    require(!fixture.first.stats().recentRttMs, "Disconnected client still exposes a fresh RTT");
}

void unavailableEndpoint() {
    DatagramTransport unanswered;
    std::string error;
    require(unanswered.start({Loopback, 0}, error), "Could not create an unanswered endpoint");
    NetworkClient client(&simulateServer);
    require(client.connect(unanswered.localEndpoint(), error), "Could not start timeout scenario");
    for (unsigned i = 0; i <= TickRate * 5; ++i) client.tick({});
    require(client.phase() == ClientPhase::TimedOut && client.diagnostic() == "Connection timed out",
        "Unavailable session did not yield a diagnostic");
}

void independentServerCompatibility() {
    Game game;
    NetworkServer server(game, &simulateServer);
    ServerConfig config;
    config.bind = {0, 0};
    config.compatibility = 0x123456789abcdef0ULL;
    std::string error;
    require(server.start(config, error), "Could not start a wildcard-bound server");
    const auto bound = server.localEndpoint();
    require(bound.address == 0 && bound.port != 0, "Server did not bind its configured interface");
    const auto serverEndpoint = resolveConnectEndpoint("LOCALHOST:" + std::to_string(bound.port), DefaultPort, error);
    require(serverEndpoint == Endpoint{Loopback, bound.port} && error.empty(), "Hostname did not resolve with its port");
    require(!resolveConnectEndpoint("0.0.0.0", bound.port, error) && !error.empty(), "Wildcard is not a join address");

    NetworkClient client(&simulateServer);
    const auto waitUntil = [&](auto ready) {
        for (int tick = 0; tick < 200; ++tick) {
            if (ready()) return true;
            client.tick({});
            server.tick();
            std::this_thread::sleep_for(1ms);
        }
        return ready();
    };
    require(client.connect(*serverEndpoint, error, config.compatibility ^ 1), "Could not try a mismatched build");
    require(waitUntil([&] { return client.phase() == ClientPhase::Rejected; }) && server.peerCount() == 0 &&
        client.diagnostic() == "Incompatible game or scene build", "Mismatched build was not explicitly rejected");
    require(client.connect(*serverEndpoint, error, config.compatibility), "Could not join with matching build");
    require(waitUntil([&] { return entity(client, client.localEntity()).has_value(); }) &&
        client.phase() == ClientPhase::Connected && server.peerCount() == 1,
        "Matching build did not receive a snapshot from the wildcard-bound server");
    const auto request = client.call({1, client.localEntity(), 123});
    RpcResult result;
    require(request && waitUntil([&] { return client.takeResult(result); }) &&
        result.request == request && result.status == RpcStatus::Accepted,
        "Wildcard-bound server did not process an RPC from the matching client");
}

}

int main() {
    try {
        chartHistoryRetainsBoundedSamples();
        chartSummaryUsesVisibleMeasurements();
        codecRoundTrip();
        peerNameCodec();
        controlCodec();
        transportEmulationCounters();
        controlsAndPeerValueReplication();
        holdsControlsAcrossOneMissingTick();
        deferredSnapshotMatchesProcessedInput();
        temporaryDelayClearsRedundantControlBacklog();
        slowerServerKeepsAcceptingInput();
        peerNameReplication();
        remotePlaybackStaysContinuousAcrossSnapshotGaps();
        remotePlaybackStaysNearLatestOnSlowClient();
        Fixture fixture;
        const auto firstId = fixture.first.localEntity();
        const auto secondId = fixture.second.localEntity();
        require(firstId != secondId && fixture.server.peerCount() == 2, "Two clients did not get unique avatars");
        rejectedPacketsAndCapacity(fixture);
        movementAndReplication(fixture, firstId, secondId);
        serverOwnedReplicas(fixture, firstId);
        rpcRetries(fixture, firstId, secondId);
        disconnectTimeoutAndReconnect(fixture, firstId, secondId);
        gracefulServerShutdown(fixture);
        symmetricNetworkTestEmulation();
        perPeerEgressEmulation();
        probeMeasurement();
        unavailableEndpoint();
        independentServerCompatibility();
        steamInvitePayloadIsValidated();
        std::cout << "Network session: codec, replication, prediction, interpolation, RPC, per-peer loss and reconnect passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
