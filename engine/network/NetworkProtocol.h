#pragma once

#include "engine/network/NetworkSession.h"
#include "engine/network/SteamFriend.h"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <type_traits>
#include <utility>

namespace engine::net::wire {

inline constexpr uint32_t Magic = 0x31454754;
inline constexpr uint8_t Version = 6;
inline constexpr size_t HeaderBytes = 16;
inline constexpr size_t MaxInputBatch = 8;
inline constexpr size_t FullSnapshotHeaderBytes = HeaderBytes + 4 * sizeof(uint64_t) + 1;
inline constexpr size_t FullEntityBytes = 26;
static_assert(FullSnapshotHeaderBytes + MaxReplicas * FullEntityBytes <= MaxDatagramBytes);
inline constexpr size_t ControlSampleBytes = 9 + ControlActionCount * sizeof(uint16_t);
static_assert(HeaderBytes + 2 * sizeof(uint64_t) + 1 + MaxInputBatch * ControlSampleBytes <= MaxDatagramBytes);
inline constexpr size_t PeerNamePageSize = 8;
inline constexpr size_t PeerNamePages = (MaxPeers + PeerNamePageSize - 1) / PeerNamePageSize;
static_assert(HeaderBytes + 2 + sizeof(uint64_t) +
    PeerNamePageSize * (sizeof(EntityId) + sizeof(uint64_t) + 1 + MaxPeerNameBytes) <= MaxDatagramBytes);
static_assert(MaxPeerNameBytes <= UINT8_MAX);

enum class Kind : uint8_t {
    Hello = 1, Challenge, Confirm, Accept, Reject, Goodbye, Input, Snapshot, Ping, Pong, Rpc, RpcResult, GoodbyeAck,
    PeerNames
};

enum class Reject : uint8_t { Incompatible = 1, Full, ContentMismatch };

class Writer {
public:
    Writer(Kind kind, uint64_t session) {
        put(Magic);
        put(Version);
        put(static_cast<uint8_t>(kind));
        put(uint16_t{});
        put(session);
    }

    template<class T>
    void put(T value) {
        using Unsigned = std::make_unsigned_t<T>;
        const auto bits = std::bit_cast<Unsigned>(value);
        if (m_size + sizeof(T) > m_bytes.size()) {
            m_overflow = true;
            return;
        }
        for (size_t i = 0; i < sizeof(T); ++i) {
            m_bytes[m_size++] = static_cast<uint8_t>(bits >> (i * 8));
        }
    }

    void putText(std::string_view text) {
        if (text.size() > MaxPeerNameBytes) {
            m_overflow = true;
            return;
        }
        put(static_cast<uint8_t>(text.size()));
        for (const unsigned char byte : text) put(byte);
    }

    std::span<const uint8_t> finish() {
        if (m_overflow) return {};
        const auto payload = m_size - HeaderBytes;
        m_bytes[6] = static_cast<uint8_t>(payload);
        m_bytes[7] = static_cast<uint8_t>(payload >> 8);
        return {m_bytes.data(), m_size};
    }

private:
    std::array<uint8_t, MaxDatagramBytes> m_bytes{};
    size_t m_size{};
    bool m_overflow{};
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : m_bytes(bytes) {}

    template<class T>
    T get() {
        using Unsigned = std::make_unsigned_t<T>;
        if (m_bytes.size() < sizeof(T)) {
            m_invalid = true;
            return {};
        }
        uint64_t bits{};
        for (size_t i = 0; i < sizeof(T); ++i) {
            bits |= uint64_t{m_bytes[i]} << (i * 8);
        }
        m_bytes = m_bytes.subspan(sizeof(T));
        return std::bit_cast<T>(static_cast<Unsigned>(bits));
    }

    std::optional<std::string_view> getText() {
        const auto size = get<uint8_t>();
        if (m_invalid || size > MaxPeerNameBytes || m_bytes.size() < size) return {};
        const std::string_view text{reinterpret_cast<const char*>(m_bytes.data()), size};
        m_bytes = m_bytes.subspan(size);
        return text;
    }

    bool done() const { return !m_invalid && m_bytes.empty(); }
    size_t remaining() const { return m_bytes.size(); }

private:
    std::span<const uint8_t> m_bytes;
    bool m_invalid{};
};

struct Header {
    uint8_t version{};
    Kind kind{};
    uint64_t session{};
};

inline bool readHeader(Reader& reader, Header& header) {
    if (reader.remaining() < HeaderBytes || reader.get<uint32_t>() != Magic) return false;
    header.version = reader.get<uint8_t>();
    const auto kind = reader.get<uint8_t>();
    if (kind < static_cast<uint8_t>(Kind::Hello) || kind > static_cast<uint8_t>(Kind::PeerNames)) return false;
    header.kind = static_cast<Kind>(kind);
    const auto length = reader.get<uint16_t>();
    header.session = reader.get<uint64_t>();
    return length == reader.remaining();
}

struct PeerNameEntry {
    EntityId id{};
    std::string_view name;
    uint64_t steamId{};

    bool operator==(const PeerNameEntry&) const = default;
};

inline Writer encodePeerNames(uint64_t token, uint8_t page, uint64_t revision, std::span<const PeerNameEntry> names) {
    Writer writer(Kind::PeerNames, token);
    writer.put(page);
    writer.put(revision);
    writer.put(static_cast<uint8_t>(names.size()));
    for (const auto& [id, name, steamId] : names) {
        writer.put(id);
        writer.put(steamId);
        writer.putText(name);
    }
    return writer;
}

struct PeerNamePage {
    uint8_t index{};
    uint64_t revision{};
    std::array<PeerNameEntry, PeerNamePageSize> entries{};
    uint8_t count{};
};

inline bool readPeerNames(Reader& reader, PeerNamePage& page) {
    page.index = reader.get<uint8_t>();
    page.revision = reader.get<uint64_t>();
    page.count = reader.get<uint8_t>();
    if (page.index >= PeerNamePages || !page.revision || page.count > PeerNamePageSize) return false;
    for (size_t i = 0; i < page.count; ++i) {
        const auto id = reader.get<EntityId>();
        const auto steamId = reader.get<uint64_t>();
        const auto name = reader.getText();
        const auto prior = std::span(page.entries).first(i);
        if (!id || id > MaxReplicaKey || !name || (name->empty() && !steamId) ||
            (steamId && !validSteamUserId(steamId)) || normalizePeerName(*name) != name ||
            std::ranges::find(prior, id, &PeerNameEntry::id) != prior.end()) {
            return false;
        }
        page.entries[i] = {id, *name, steamId};
    }
    return reader.done();
}

struct InputSample {
    PlayerInput input;
    std::array<uint16_t, ControlActionCount> actionCounts{};
};

inline void putInput(Writer& writer, const InputSample& sample) {
    const auto& input = sample.input;
    writer.put(input.x);
    writer.put(input.z);
    writer.put(static_cast<uint8_t>(input.hasControls));
    if (input.hasControls) {
        writer.put(input.yaw);
        writer.put(input.pitch);
        writer.put(input.buttons);
        for (const auto count : sample.actionCounts) writer.put(count);
    }
}

inline bool readInput(Reader& reader, InputSample& sample) {
    if (reader.remaining() < 3) return false;
    auto& input = sample.input;
    input.x = reader.get<int8_t>();
    input.z = reader.get<int8_t>();
    const auto controls = reader.get<uint8_t>();
    if (input.x < -MaxControlAxis || input.z < -MaxControlAxis || controls > 1) return false;
    if (!controls) return true;
    if (reader.remaining() < ControlSampleBytes - 3 ||
        input.x * input.x + input.z * input.z > (MaxControlAxis + 1) * (MaxControlAxis + 1)) return false;
    input.hasControls = true;
    input.yaw = reader.get<uint16_t>();
    input.pitch = reader.get<int16_t>();
    if (input.pitch < -MaxControlPitch || input.pitch > MaxControlPitch) return false;
    input.buttons = reader.get<uint16_t>();
    for (auto& count : sample.actionCounts) count = reader.get<uint16_t>();
    return true;
}

template<class... Fields>
bool send(DatagramTransport& transport, Endpoint peer, EmulationPolicy policy,
          std::optional<NetworkEmulation> emulationOverride, Kind kind, uint64_t session, Fields... fields) {
    Writer writer(kind, session);
    (writer.put(fields), ...);
    return transport.send(peer, writer.finish(), policy, emulationOverride);
}

template<class... Fields>
bool send(DatagramTransport& transport, Endpoint peer, EmulationPolicy policy, Kind kind, uint64_t session,
          Fields... fields) {
    return send(transport, peer, policy, std::nullopt, kind, session, fields...);
}

template<class... Fields>
bool send(DatagramTransport& transport, Endpoint peer, Kind kind, uint64_t session, Fields... fields) {
    return send(transport, peer, EmulationPolicy::Apply, kind, session, fields...);
}

struct Snapshot {
    uint64_t sequence{};
    uint64_t tick{};
    uint64_t baseline{};
    uint64_t inputAck{};
    std::vector<EntityState> entities;
};

inline const EntityState* findEntity(const Snapshot& snapshot, EntityId id) {
    auto it = std::lower_bound(snapshot.entities.begin(), snapshot.entities.end(), id,
        [](const EntityState& entity, EntityId target) { return entity.id < target; });
    return it != snapshot.entities.end() && it->id == id ? &*it : nullptr;
}

inline constexpr uint8_t X = 1, Y = 2, Z = 4, Yaw = 8, Value = 16, Flags = 32, Pitch = 64, Roll = 128;
inline constexpr uint8_t AllFields = X | Y | Z | Yaw | Value | Flags | Pitch | Roll;

inline uint8_t changedFields(const EntityState& current, const EntityState& baseline) {
    uint8_t mask{};
    if (current.x != baseline.x) mask |= X;
    if (current.y != baseline.y) mask |= Y;
    if (current.z != baseline.z) mask |= Z;
    if (current.yaw != baseline.yaw) mask |= Yaw;
    if (current.value != baseline.value) mask |= Value;
    if (current.flags != baseline.flags) mask |= Flags;
    if (current.pitch != baseline.pitch) mask |= Pitch;
    if (current.roll != baseline.roll) mask |= Roll;
    return mask;
}

inline void writeEntity(Writer& writer, EntityId id, uint8_t mask, const EntityState* state) {
    writer.put(id);
    writer.put(mask);
    if (mask & X) writer.put(state->x);
    if (mask & Y) writer.put(state->y);
    if (mask & Z) writer.put(state->z);
    if (mask & Yaw) writer.put(state->yaw);
    if (mask & Value) writer.put(state->value);
    if (mask & Flags) writer.put(state->flags);
    if (mask & Pitch) writer.put(state->pitch);
    if (mask & Roll) writer.put(state->roll);
}

inline Writer encodeSnapshot(uint64_t session, const Snapshot& snapshot, const Snapshot* baseline) {
    struct Change {
        EntityId id;
        uint8_t mask;
        const EntityState* entity;
    };
    std::array<Change, MaxReplicas * 2> changes{};
    size_t count{};
    if (baseline) {
        for (const auto& prior : baseline->entities) {
            if (!findEntity(snapshot, prior.id)) changes[count++] = {prior.id, 0, nullptr};
        }
    }
    for (const auto& current : snapshot.entities) {
        const auto* prior = baseline ? findEntity(*baseline, current.id) : nullptr;
        const auto mask = prior ? changedFields(current, *prior) : AllFields;
        if (mask) changes[count++] = {current.id, mask, &current};
    }
    std::sort(changes.begin(), changes.begin() + count, [](const Change& left, const Change& right) {
        return left.id < right.id;
    });

    Writer writer(Kind::Snapshot, session);
    writer.put(snapshot.sequence);
    writer.put(snapshot.tick);
    writer.put(snapshot.baseline);
    writer.put(snapshot.inputAck);
    writer.put(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i) {
        const auto& [id, mask, entity] = changes[i];
        writeEntity(writer, id, mask, entity);
    }
    return writer;
}

inline bool decodeEntities(Reader& reader, Snapshot& snapshot, const Snapshot* baseline) {
    const auto count = reader.get<uint8_t>();
    if (count > MaxReplicas * 2) return false;
    if (baseline) snapshot.entities = baseline->entities;
    EntityId previous{};
    for (unsigned i = 0; i < count; ++i) {
        const auto id = reader.get<EntityId>();
        const auto mask = reader.get<uint8_t>();
        if (id <= previous) return false;
        previous = id;
        auto it = std::lower_bound(snapshot.entities.begin(), snapshot.entities.end(), id,
            [](const EntityState& state, EntityId target) { return state.id < target; });
        const bool present = it != snapshot.entities.end() && it->id == id;
        if (mask == 0) {
            if (!present) return false;
            snapshot.entities.erase(it);
            continue;
        }
        if (!present) {
            if (mask != AllFields) return false;
            it = snapshot.entities.insert(it, EntityState{id});
        }
        if (mask & X) it->x = reader.get<int32_t>();
        if (mask & Y) it->y = reader.get<int32_t>();
        if (mask & Z) it->z = reader.get<int32_t>();
        if (mask & Yaw) it->yaw = reader.get<uint16_t>();
        if (mask & Value) it->value = reader.get<uint16_t>();
        if (mask & Flags) it->flags = reader.get<uint8_t>();
        if (mask & Pitch) it->pitch = reader.get<uint16_t>();
        if (mask & Roll) it->roll = reader.get<uint16_t>();
    }
    return snapshot.entities.size() <= MaxReplicas && reader.done();
}

}
