#pragma once

#include "engine/network/NetworkSession.h"
#include "engine/network/NetworkTestOptions.h"
#include "engine/scripting/ScriptNetwork.h"

#include <chrono>
#include <functional>
#include <filesystem>
#include <map>
#include <string>

namespace engine::core { struct PlayLaunch; }
namespace engine::ecs { class World; }
namespace engine::scripting { class ScriptSteam; class ScriptingSystem; }

namespace engine::game {

class SceneSteam;

class SceneNetwork final : public scripting::ScriptNetwork, private net::ServerGame {
public:
    SceneNetwork(ecs::World& world, scripting::ScriptingSystem& scripts,
                 std::function<void(ecs::EntityId)> destroy);
    ~SceneNetwork() override;

    bool start(const core::PlayLaunch& launch, std::string& error);
    void beginTick();
    void endTick();
    uint16_t localPort() const;
    bool isClient() const;
    bool acceptsLocalInput() const;
    std::string_view connectionNotice() const;
    scripting::ScriptSteam* steam() const;
    std::filesystem::path steamAvatar(std::string_view image);

    std::string_view role() const override;
    bool connected() const override;
    uint32_t localPeer() const override;
    bool isHost(uint32_t peer) const override;
    bool usesSteam() const override;
    std::vector<uint32_t> peers() const override;
    nlohmann::json stats() const override;
    bool bind(ecs::EntityId entity, uint32_t key) override;
    bool unbind(uint32_t key) override;
    bool registerPrefab(uint32_t key, std::string_view path) override;
    ecs::EntityId entity(uint32_t key) const override;
    bool setState(uint32_t key, uint16_t value, uint8_t flags) override;
    std::optional<scripting::ScriptReplicaState> state(uint32_t key) const override;
    std::optional<scripting::ScriptReplicaState> rawState(uint32_t key) const override;
    std::optional<scripting::ScriptPeerMotion> localMotion() const override;
    uint64_t nextInputSequence() const override;
    bool setPeerMotion(uint32_t peer, scripting::ScriptPeerMotion motion) override;
    bool setInput(float x, float z) override;
    std::optional<std::array<float, 2>> input(uint32_t peer) const override;
    bool setControls(scripting::ScriptPlayerControls controls) override;
    std::optional<scripting::ScriptPlayerControls> controls(uint32_t peer) const override;
    bool setPeerValue(uint32_t peer, uint16_t value) override;
    std::optional<uint16_t> peerValue(uint32_t peer) const override;
    bool setPeerName(uint32_t peer, std::string_view name) override;
    std::string_view peerName(uint32_t peer) const override;
    std::string peerAvatar(uint32_t peer) const override;
    uint32_t request(uint32_t key, uint8_t action, int32_t argument, std::string_view text) override;

private:
    friend class SceneSteam;
    struct LiveControl;
    struct RemoteClient;
    struct HostControl;

    struct Binding {
        ecs::EntityId entity{};
        uint16_t value{};
        uint8_t flags{};
        std::string prefab;
        std::optional<scripting::ScriptReplicaState> received;
        std::optional<bool> savedVisibility;
        bool spawnFailed{};
    };

    net::EntityState spawn(net::EntityId id, size_t slot) override;
    net::RpcStatus rpc(net::EntityState& player, const net::RpcCommand& command) override;
    void despawn(net::EntityId id) override;
    void input(net::EntityId id, net::PlayerInput input) override;
    void sampleClient();
    void applyClientPoses();
    void restoreVisibility(Binding& binding);
    void publishServerPoses();
    void pollLiveControl();
    void pollHostControl();
    void pollRemoteClient();
    void pollSteamNames();
    bool knowsSteamPeer(uint64_t steamId) const;
    bool canHostSteam() const;
    bool startSteamAuthority(uint64_t hostId, uint64_t lobbyId, unsigned maxPlayers, std::string& error);
    bool connectSteamHost(uint64_t hostId, std::string& error);
    void leaveSteamSession(bool notify = true);
    void publishLiveClientStatus();
    void publishLiveTelemetry();
    std::optional<net::EntityState> capture(uint32_t key, const Binding& binding) const;

    ecs::World& m_world;
    scripting::ScriptingSystem& m_scripts;
    std::function<void(ecs::EntityId)> m_destroy;
    std::map<uint32_t, Binding> m_bindings;
    std::map<net::EntityId, net::PlayerInput> m_peerInputs;
    std::vector<net::EntityId> m_clientPeers;
    std::vector<ecs::EntityId> m_pendingDespawn;
    net::NetworkTestOptions m_options;
    net::NetworkTestConditions m_conditions;
    unsigned m_clientCapacity{};
    unsigned m_projectPlayerLimit{};
    size_t m_bindingLimit{};
    net::PlayerInput m_input{};
    net::ClientPhase m_lastClientPhase{net::ClientPhase::Disconnected};
    std::chrono::steady_clock::time_point m_nextLinkLog{};
    bool m_emitPeerEvents{true};
    std::unique_ptr<net::NetworkServer> m_server;
    std::unique_ptr<net::NetworkClient> m_client;
    std::unique_ptr<LiveControl> m_liveControl;
    std::unique_ptr<RemoteClient> m_remoteClient;
    std::unique_ptr<HostControl> m_hostControl;
    uint64_t m_hostNonce{};
    std::unique_ptr<SceneSteam> m_steam;
    std::chrono::steady_clock::time_point m_nextSteamNamePoll{};
    net::EntityState m_localPlayer;
    uint32_t m_nextLocalRpc{};
    std::vector<std::pair<uint32_t, net::RpcCommand>> m_localRpcs;
};

}
