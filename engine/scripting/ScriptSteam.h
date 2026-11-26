#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace engine::scripting {

enum class ScriptSteamAction {
    CreateSession, FindSessions, JoinSession, LeaveSession, UpdateSession,
    InviteFriend, OpenProfile, OpenInviteOverlay, SetRichPresence
};

struct ScriptSteamOptions final {
    std::string name;
    std::string visibility{"friends"};
    unsigned maxPlayers{8};
    bool joinable{true};
    std::map<std::string, std::string> properties;
};

struct ScriptSteamRequest final {
    ScriptSteamAction action{};
    std::uint64_t id{};
    unsigned limit{32};
    ScriptSteamOptions options;
    std::string key;
    std::string value;
};

class ScriptSteam {
public:
    virtual ~ScriptSteam() = default;

    virtual bool available() const = 0;
    virtual nlohmann::json localUser() const = 0;
    virtual nlohmann::json friends(unsigned offset, unsigned limit) = 0;
    virtual nlohmann::json session() const = 0;
    virtual nlohmann::json sessions(unsigned offset, unsigned limit) const = 0;
    virtual std::string avatar(std::uint64_t friendId) = 0;
    virtual std::uint32_t submit(ScriptSteamRequest request, std::string& error) = 0;
};

}
