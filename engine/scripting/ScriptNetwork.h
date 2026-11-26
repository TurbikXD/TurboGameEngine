#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/ecs/entity.h"

namespace engine::scripting {

struct ScriptReplicaState final {
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

struct ScriptRpcReply final {
    bool handled{};
    bool accepted{};
    uint16_t value{};
};

struct ScriptPlayerControls final {
    float x{};
    float z{};
    float yaw{};
    float pitch{};
    uint16_t buttons{};
    uint8_t pressed{};
};

struct ScriptPeerMotion final {
    float x{}, y{}, z{};
    float jumpBuffer{}, coyote{};
    uint64_t tick{}, inputAck{};
};

class ScriptNetwork {
public:
    virtual ~ScriptNetwork() = default;

    virtual std::string_view role() const = 0;
    virtual bool connected() const = 0;
    virtual uint32_t localPeer() const = 0;
    virtual bool isHost(uint32_t) const { return false; }
    virtual bool usesSteam() const { return false; }
    virtual std::vector<uint32_t> peers() const = 0;
    virtual nlohmann::json stats() const = 0;

    virtual bool bind(ecs::EntityId entity, uint32_t key) = 0;
    virtual bool unbind(uint32_t key) = 0;
    virtual bool registerPrefab(uint32_t key, std::string_view path) = 0;
    virtual ecs::EntityId entity(uint32_t key) const = 0;
    virtual bool setState(uint32_t key, uint16_t value, uint8_t flags) = 0;
    virtual std::optional<ScriptReplicaState> state(uint32_t key) const = 0;
    virtual std::optional<ScriptReplicaState> rawState(uint32_t) const { return {}; }
    virtual std::optional<ScriptPeerMotion> localMotion() const { return {}; }
    virtual uint64_t nextInputSequence() const { return 0; }
    virtual bool setPeerMotion(uint32_t, ScriptPeerMotion) { return false; }

    virtual bool setInput(float x, float z) = 0;
    virtual std::optional<std::array<float, 2>> input(uint32_t peer) const = 0;
    virtual bool setControls(ScriptPlayerControls) { return false; }
    virtual std::optional<ScriptPlayerControls> controls(uint32_t) const { return {}; }
    virtual bool setPeerValue(uint32_t, uint16_t) { return false; }
    virtual std::optional<uint16_t> peerValue(uint32_t) const { return {}; }
    virtual bool setPeerName(uint32_t, std::string_view) { return false; }
    virtual std::string_view peerName(uint32_t) const { return {}; }
    virtual std::string peerAvatar(uint32_t) const { return {}; }
    virtual uint32_t request(uint32_t key, uint8_t action, int32_t argument, std::string_view text) = 0;
};

}
