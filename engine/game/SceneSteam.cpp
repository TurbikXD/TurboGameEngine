#include "engine/game/SceneSteam.h"

#include <algorithm>
#include <charconv>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/AtomicFile.h"
#include "engine/game/SceneNetwork.h"
#include "engine/scripting/ScriptingSystem.h"

namespace engine::game {
namespace {

using Action = scripting::ScriptSteamAction;
using Json = nlohmann::json;
using namespace std::chrono_literals;
constexpr std::string_view AvatarPrefix = "steamavatar://";

std::string_view actionName(Action action) {
    switch (action) {
    case Action::CreateSession: return "createSession";
    case Action::FindSessions: return "findSessions";
    case Action::JoinSession: return "joinSession";
    case Action::LeaveSession: return "leaveSession";
    case Action::UpdateSession: return "updateSession";
    case Action::InviteFriend: return "inviteFriend";
    case Action::OpenProfile: return "openProfile";
    case Action::OpenInviteOverlay: return "openInviteOverlay";
    case Action::SetRichPresence: return "setRichPresence";
    }
    return {};
}

std::string_view presenceName(net::SteamFriendPresence presence) {
    switch (presence) {
    case net::SteamFriendPresence::Online: return "online";
    case net::SteamFriendPresence::Away: return "away";
    case net::SteamFriendPresence::Busy: return "busy";
    case net::SteamFriendPresence::Offline: return "offline";
    }
    return "offline";
}

net::SteamLobbyOptions lobbyOptions(scripting::ScriptSteamOptions options) {
    const auto visibility = options.visibility == "public" ? net::SteamLobbyVisibility::Public :
        options.visibility == "private" ? net::SteamLobbyVisibility::Private : net::SteamLobbyVisibility::Friends;
    if (options.name.empty()) options.name = "TurboGameEngine";
    return {std::move(options.name), visibility, options.maxPlayers, options.joinable, std::move(options.properties)};
}

Json lobbyValue(const net::SteamLobbyInfo& lobby) {
    const auto& options = lobby.options;
    const auto visibility = options.visibility == net::SteamLobbyVisibility::Public ? "public" :
        options.visibility == net::SteamLobbyVisibility::Private ? "private" : "friends";
    return {{"id", std::to_string(lobby.id)}, {"hostId", std::to_string(lobby.hostId)}, {"name", options.name},
        {"visibility", visibility}, {"maxPlayers", options.maxPlayers}, {"players", lobby.players},
        {"joinable", options.joinable}, {"properties", options.properties}};
}

template<class Row, class ToJson>
Json page(const std::vector<Row>& rows, unsigned offset, unsigned limit, ToJson value) {
    Json out = Json::array();
    const auto begin = std::min<std::size_t>(offset, rows.size());
    const auto count = std::min<std::size_t>(std::min(limit, 32U), rows.size() - begin);
    for (std::size_t i = begin; i < begin + count; ++i) out.push_back(value(rows[i]));
    return out;
}

}

SceneSteam::SceneSteam(scripting::ScriptingSystem& scripts, SceneNetwork& network,
                       net::SteamInvites& steam, uint64_t compatibility)
    : m_scripts(scripts), m_network(network), m_steam(steam), m_compatibility(compatibility) {
    refreshFriends(Clock::now());
}

SceneSteam::~SceneSteam() {
    m_steam.leaveLobby();
    if (!m_avatarDirectory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(m_avatarDirectory, ignored);
    }
}

bool SceneSteam::available() const { return m_localId != 0; }

std::string SceneSteam::userName(uint64_t id) const { return m_steam.userName(id); }
std::string_view SceneSteam::localName() const { return m_localName; }

Json SceneSteam::localUser() const {
    return available() ? Json{{"id", std::to_string(m_localId)}, {"name", m_localName}} : Json{};
}

Json SceneSteam::friends(unsigned offset, unsigned limit) {
    return page(m_friends, offset, limit, [](const net::SteamFriend& entry) {
        return Json{{"id", std::to_string(entry.id)}, {"name", entry.name},
            {"status", presenceName(entry.presence)}, {"online", entry.online()}};
    });
}

Json SceneSteam::session() const { return m_lobby ? lobbyValue(*m_lobby) : Json{}; }

Json SceneSteam::sessions(unsigned offset, unsigned limit) const {
    return page(m_steam.lobbies(), offset, limit, lobbyValue);
}

bool SceneSteam::knows(uint64_t id) const {
    return id && (id == m_localId || std::ranges::find(m_friends, id, &net::SteamFriend::id) != m_friends.end());
}

std::string SceneSteam::avatar(uint64_t id) {
    return knows(id) ? std::string(AvatarPrefix) + std::to_string(id) : std::string{};
}

std::string SceneSteam::peerAvatar(uint64_t id) const {
    return available() && net::validSteamUserId(id) && m_network.knowsSteamPeer(id)
        ? std::string(AvatarPrefix) + std::to_string(id) : std::string{};
}

uint32_t SceneSteam::submit(scripting::ScriptSteamRequest request, std::string& error) {
    if (!available()) { error = "Steam is unavailable."; return 0; }
    if (m_commands.size() >= 32) { error = "Too many pending Steam actions."; return 0; }
    if (++m_nextRequest == 0 || m_nextRequest > 0x7fffffffU) m_nextRequest = 1;
    m_commands.push_back({m_nextRequest, std::move(request)});
    return m_nextRequest;
}

void SceneSteam::result(const Command& command, bool ok, std::string_view error, uint64_t lobbyId) {
    Json event{{"action", actionName(command.request.action)}, {"request", command.id}, {"ok", ok}, {"error", error}};
    if (lobbyId) event["lobbyId"] = std::to_string(lobbyId);
    m_scripts.queueEvent("steamSession", std::move(event));
}

void SceneSteam::execute(Command command) {
    auto& request = command.request;
    std::string error;
    bool pending = false;
    bool ok = false;
    switch (request.action) {
    case Action::CreateSession: {
        if (!m_network.canHostSteam()) {
            error = "Leave the connected game before hosting a Steam lobby.";
            break;
        }
        pending = m_steam.createLobby(m_compatibility, lobbyOptions(std::move(request.options)), error);
        break;
    }
    case Action::FindSessions:
        pending = m_steam.findLobbies(m_compatibility, request.limit, error);
        break;
    case Action::JoinSession:
        if (m_network.isClient()) pending = m_steam.joinLobby(request.id, m_compatibility, error);
        else error = "Leave the hosted session before joining another Steam lobby.";
        break;
    case Action::LeaveSession:
        if (m_pending) {
            result(*m_pending, false, "Steam action canceled.");
            m_pending.reset();
        }
        m_steam.leaveLobby();
        m_network.leaveSteamSession();
        ok = true;
        break;
    case Action::UpdateSession:
        if (m_network.m_server && request.options.maxPlayers - 1 > m_network.m_clientCapacity)
            error = "Lobby exceeds the server capacity.";
        else ok = m_steam.updateLobby(lobbyOptions(std::move(request.options)), error);
        break;
    case Action::InviteFriend:
        if (m_steam.lobby()) ok = m_steam.inviteLobbyFriend(request.id, error);
        else if (m_network.m_server) ok = m_steam.invite(request.id, {m_localId, m_compatibility}, error);
        else error = "Join or host a Steam session before inviting a friend.";
        break;
    case Action::OpenProfile: ok = m_steam.openProfile(request.id, error); break;
    case Action::OpenInviteOverlay: ok = m_steam.openInviteOverlay(error); break;
    case Action::SetRichPresence: ok = m_steam.setRichPresence(request.key, request.value, error); break;
    }
    if (pending) m_pending = std::move(command);
    else result(command, ok, error);
}

void SceneSteam::handleLobbyEvent(net::SteamLobbyEvent event) {
    if (event.action == net::SteamLobbyAction::JoinRequested) {
        m_scripts.queueEvent("steamJoinRequested", {{"lobbyId", std::to_string(event.lobbyId)}});
        if (!m_pending && m_commands.empty() && !m_steam.lobby() && m_network.isClient() && m_network.canHostSteam()) {
            scripting::ScriptSteamRequest request;
            request.action = Action::JoinSession;
            request.id = event.lobbyId;
            execute({0, std::move(request)});
        }
        return;
    }
    if (event.action == net::SteamLobbyAction::Lost) {
        m_network.leaveSteamSession();
        m_scripts.queueEvent("steamSession", {{"action", "sessionLost"}, {"request", 0},
            {"ok", false}, {"error", event.error}, {"lobbyId", std::to_string(event.lobbyId)}});
        return;
    }
    if (!m_pending) return;
    const auto expected = m_pending->request.action;
    const auto actual = event.action == net::SteamLobbyAction::Create ? Action::CreateSession :
        event.action == net::SteamLobbyAction::Join ? Action::JoinSession : Action::FindSessions;
    if (expected != actual) return;
    auto command = std::exchange(m_pending, std::nullopt);
    if (event.ok && event.action == net::SteamLobbyAction::Create) {
        const auto lobby = m_steam.lobby();
        if (!lobby) { event.ok = false; event.error = "Steam did not supply lobby settings."; }
        else if (m_network.startSteamAuthority(m_localId, event.lobbyId, lobby->options.maxPlayers, event.error)) {
            event.ok = m_steam.activateLobby(event.error);
            if (!event.ok) m_network.leaveSteamSession(false);
            else if (m_network.m_localPlayer.id) {
                m_scripts.queueEvent("networkRoleChanged", {{"role", "server"}});
                m_scripts.queueEvent("networkPeerJoined", {{"peer", m_network.m_localPlayer.id}});
            }
        } else event.ok = false;
        if (!event.ok) m_steam.leaveLobby();
    } else if (event.ok && event.action == net::SteamLobbyAction::Join) {
        event.ok = m_network.connectSteamHost(event.hostId, event.error);
        if (!event.ok) m_steam.leaveLobby();
    }
    result(*command, event.ok, event.error, event.lobbyId);
}

void SceneSteam::refreshFriends(Clock::time_point now) {
    if (now < m_nextFriends) return;
    m_nextFriends = now + 2s;
    m_localId = m_steam.localUserId();
    m_localName = m_steam.localUserName();
    m_friends = m_steam.friends().value_or(std::vector<net::SteamFriend>{});
}

void SceneSteam::tick() {
    m_steam.poll();
    for (unsigned i = 0; i < 16; ++i) {
        auto event = m_steam.takeLobbyEvent();
        if (!event) break;
        handleLobbyEvent(std::move(*event));
    }
    if (const auto invite = m_steam.takeJoin()) {
        std::string error;
        if (invite->compatibilityId != m_compatibility) error = "Steam invitation is for a different project or scene build.";
        else if (!m_network.isClient() || m_steam.lobby() || m_pending) error = "Leave the current Steam session before joining another host.";
        const bool ok = error.empty() && m_network.connectSteamHost(invite->hostId, error);
        m_scripts.queueEvent("steamJoinRequested", {{"hostId", std::to_string(invite->hostId)}, {"ok", ok}, {"error", error}});
    }
    if (!m_commands.empty() && (!m_pending || m_commands.front().request.action == Action::LeaveSession)) {
        auto command = std::move(m_commands.front());
        m_commands.pop_front();
        execute(std::move(command));
    }
    m_lobby = m_steam.lobby();
    const auto now = Clock::now();
    refreshFriends(now);
    updateAvatars(now);
}

std::filesystem::path SceneSteam::avatarPath(std::string_view image) {
    if (!image.starts_with(AvatarPrefix)) return {};
    image.remove_prefix(AvatarPrefix.size());
    if (image.empty() || image.size() > 20) return {};
    uint64_t id{};
    const auto end = image.data() + image.size();
    const auto parsed = std::from_chars(image.data(), end, id);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !net::validSteamUserId(id) ||
        (!knows(id) && !m_network.knowsSteamPeer(id))) return {};
    if (m_avatarRequests.size() < net::MaxSteamAvatarRequests && std::ranges::find(m_avatarRequests, id) == m_avatarRequests.end())
        m_avatarRequests.push_back(id);
    return m_avatarHandles.contains(id) ? m_avatarDirectory / net::steamAvatarFileName(id) : std::filesystem::path{};
}

void SceneSteam::updateAvatars(Clock::time_point now) {
    if (m_avatarRequests.empty() || now < m_nextAvatarUpdate) return;
    m_nextAvatarUpdate = now + 150ms;
    std::erase_if(m_nextPeerAvatarRequest, [&](const auto& request) {
        const auto id = request.first;
        if (m_network.knowsSteamPeer(id)) return false;
        if (!knows(id) && m_avatarHandles.erase(id)) {
            std::error_code ignored;
            std::filesystem::remove(m_avatarDirectory / net::steamAvatarFileName(id), ignored);
        }
        return true;
    });
    for (const auto id : std::exchange(m_avatarRequests, {})) {
        std::optional<int> image;
        if (knows(id)) {
            image = m_steam.smallAvatarHandle(id);
        } else if (m_network.knowsSteamPeer(id)) {
            auto& next = m_nextPeerAvatarRequest[id];
            const bool requestInformation = now >= next;
            if (requestInformation) next = now + 5s;
            image = m_steam.smallPeerAvatarHandle(id, requestInformation);
        }
        if (!image || *image < 0) continue;
        const auto previous = m_avatarHandles.find(id);
        if (previous != m_avatarHandles.end() && previous->second == *image) continue;
        if (*image == 0) {
            if (previous != m_avatarHandles.end()) {
                std::error_code error;
                std::filesystem::remove(m_avatarDirectory / net::steamAvatarFileName(id), error);
                if (!error) m_avatarHandles.erase(previous);
            }
            continue;
        }
        if (previous == m_avatarHandles.end() && m_avatarHandles.size() >= net::MaxSteamFriends + net::MaxPeers) continue;
        const auto bytes = m_steam.smallAvatarTga(*image);
        if (bytes.empty()) continue;
        std::error_code error;
        if (m_avatarDirectory.empty()) {
            const auto temp = std::filesystem::temp_directory_path(error);
            if (error) return;
            const auto path = temp / ("tge-play-steam-game-" + std::to_string(now.time_since_epoch().count()));
            if (!std::filesystem::create_directory(path, error)) return;
            m_avatarDirectory = path;
        }
        std::string writeError;
        if (core::writeFileAtomically(m_avatarDirectory / net::steamAvatarFileName(id), bytes, writeError))
            m_avatarHandles.insert_or_assign(id, *image);
    }
}

}
