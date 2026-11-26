#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/network/SteamTransport.h"
#include "engine/scripting/ScriptSteam.h"

namespace engine::scripting { class ScriptingSystem; }

namespace engine::game {

class SceneNetwork;

class SceneSteam final : public scripting::ScriptSteam {
public:
    SceneSteam(scripting::ScriptingSystem& scripts, SceneNetwork& network, net::SteamInvites& steam, uint64_t compatibility);
    ~SceneSteam() override;
    void tick();
    std::filesystem::path avatarPath(std::string_view image);
    std::string peerAvatar(uint64_t id) const;
    std::string userName(uint64_t id) const;
    std::string_view localName() const;

    bool available() const override;
    nlohmann::json localUser() const override;
    nlohmann::json friends(unsigned offset, unsigned limit) override;
    nlohmann::json session() const override;
    nlohmann::json sessions(unsigned offset, unsigned limit) const override;
    std::string avatar(uint64_t friendId) override;
    uint32_t submit(scripting::ScriptSteamRequest request, std::string& error) override;

private:
    using Clock = std::chrono::steady_clock;
    struct Command { uint32_t id{}; scripting::ScriptSteamRequest request; };

    void execute(Command command);
    void handleLobbyEvent(net::SteamLobbyEvent event);
    void result(const Command& command, bool ok, std::string_view error = {}, uint64_t lobbyId = 0);
    bool knows(uint64_t id) const;
    void refreshFriends(Clock::time_point now);
    void updateAvatars(Clock::time_point now);

    scripting::ScriptingSystem& m_scripts;
    SceneNetwork& m_network;
    net::SteamInvites& m_steam;
    uint64_t m_compatibility;
    uint64_t m_localId{};
    std::string m_localName;
    std::vector<net::SteamFriend> m_friends;
    std::optional<net::SteamLobbyInfo> m_lobby;
    std::deque<Command> m_commands;
    std::optional<Command> m_pending;
    uint32_t m_nextRequest{};
    Clock::time_point m_nextFriends{};
    Clock::time_point m_nextAvatarUpdate{};
    std::filesystem::path m_avatarDirectory;
    std::map<uint64_t, int> m_avatarHandles;
    std::map<uint64_t, Clock::time_point> m_nextPeerAvatarRequest;
    std::vector<uint64_t> m_avatarRequests;
};

}
