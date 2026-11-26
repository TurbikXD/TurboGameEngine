#include "engine/network/SteamTransport.h"

#include <charconv>
#include <utility>

#ifdef ENGINE_ENABLE_STEAMWORKS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <steam/steam_api.h>
#include <steam/isteamnetworkingutils.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <unordered_set>

#include "engine/network/NetworkSession.h"
#endif

namespace engine::net {
namespace {

std::optional<std::uint64_t> parseInteger(std::string_view value) {
    if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == '0')) return {};
    std::uint64_t result{};
    const auto* end = value.data() + value.size();
    const auto [position, error] = std::from_chars(value.data(), end, result);
    if (error != std::errc{} || position != end) return {};
    return result;
}

}

std::string encodeSteamJoin(SteamJoin join) {
    if (!validSteamUserId(join.hostId)) return {};
    return "1:" + std::to_string(join.hostId) + ":" + std::to_string(join.compatibilityId);
}

std::optional<SteamJoin> parseSteamJoin(std::string_view value) {
    if (!value.starts_with("1:") || value.size() > 43) return {};
    value.remove_prefix(2);
    const auto divider = value.find(':');
    if (divider == std::string_view::npos) return {};
    const auto host = parseInteger(value.substr(0, divider));
    const auto compatibility = parseInteger(value.substr(divider + 1));
    if (!host || !validSteamUserId(*host) || !compatibility) return {};
    return SteamJoin{*host, *compatibility};
}

#ifdef ENGINE_ENABLE_STEAMWORKS
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto IncomingIdleTime = std::chrono::seconds(15);
constexpr auto IncomingCapacity = MaxPeers + 8;
constexpr int InitialTimeoutMs = 20000;

class SteamSdk final {
public:
    ~SteamSdk() { stop(); }

    bool start(std::string& error) {
        if (m_active) {
            error.clear();
            return true;
        }
        if (s_users == 0) {
            if (!s_module) {
                std::array<wchar_t, 32768> executable{};
                const auto length = GetModuleFileNameW(nullptr, executable.data(),
                    static_cast<DWORD>(executable.size()));
                if (length == 0 || length >= executable.size()) {
                    error = "Could not find the executable directory for Steamworks";
                    return false;
                }
                const auto dll = std::filesystem::path(executable.data()).parent_path() / L"steam_api64.dll";
                s_module = LoadLibraryExW(dll.c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!s_module) {
                    error = "Could not load steam_api64.dll beside the executable (Windows error " +
                        std::to_string(GetLastError()) + ")";
                    return false;
                }
            }
            if (!SteamAPI_IsSteamRunning()) {
                error = "Start the Steam client and sign in before using Steam networking";
                return false;
            }
            if (!SteamAPI_Init()) {
                error = "Steamworks initialization failed. Check the App ID and Steam account access";
                return false;
            }
        }
        ++s_users;
        m_active = true;
        error.clear();
        return true;
    }

    void stop() {
        if (!std::exchange(m_active, false)) return;
        if (--s_users == 0) SteamAPI_Shutdown();
    }

    void poll() const {
        if (m_active) SteamAPI_RunCallbacks();
    }

    std::uint64_t localUserId() const {
        auto* user = m_active ? SteamUser() : nullptr;
        return user ? user->GetSteamID().ConvertToUint64() : 0;
    }

private:
    inline static HMODULE s_module{};
    inline static unsigned s_users{};
    bool m_active{};
};

Endpoint steamEndpoint(std::uint64_t id) {
    return {0, SteamVirtualPort, id};
}

std::string inviteCommand(SteamJoin join) {
    const auto value = encodeSteamJoin(join);
    return value.empty() ? std::string{} : std::string(SteamJoinCommand) + " " + value;
}

std::optional<SteamJoin> parseInviteCommand(std::string_view command) {
    if (!command.starts_with(SteamJoinCommand) || command.size() <= SteamJoinCommand.size() ||
        command[SteamJoinCommand.size()] != ' ') return {};
    return parseSteamJoin(command.substr(SteamJoinCommand.size() + 1));
}

constexpr char LobbyBuildKey[] = "tge_session_v1";
constexpr char LobbyHostKey[] = "tge_host";
constexpr char LobbyNameKey[] = "tge_name";
constexpr char LobbyPlayersKey[] = "tge_players";
constexpr char LobbyCapacityKey[] = "tge_capacity";
constexpr char LobbyVisibilityKey[] = "tge_visibility";
constexpr char LobbyJoinableKey[] = "tge_joinable";
constexpr std::string_view LobbyPropertyPrefix = "tge_prop_";
constexpr std::size_t MaxLobbyProperties = 16;
constexpr unsigned MaxLobbyResults = 50;
constexpr auto LobbyTimeout = std::chrono::seconds(20);

bool validLobbyId(std::uint64_t id) {
    const CSteamID lobby(id);
    return lobby.IsLobby() && lobby.IsValid() && lobby.GetEUniverse() == k_EUniversePublic;
}

bool validLobbyText(std::string_view text, std::size_t maxBytes) {
    return text.size() <= maxBytes &&
        std::ranges::none_of(text, [](unsigned char byte) { return byte < 32 || byte == 127; }) &&
        (text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0) > 0);
}

bool validLobbyKey(std::string_view key, std::size_t maxBytes) {
    return !key.empty() && key.size() <= maxBytes &&
        key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string_view::npos;
}

ELobbyType sdkLobbyType(SteamLobbyVisibility visibility) {
    switch (visibility) {
        case SteamLobbyVisibility::Public: return k_ELobbyTypePublic;
        case SteamLobbyVisibility::Friends: return k_ELobbyTypeFriendsOnly;
        case SteamLobbyVisibility::Private: return k_ELobbyTypePrivate;
    }
    return k_ELobbyTypePrivate;
}

bool validLobbyOptions(const SteamLobbyOptions& options) {
    if (options.name.empty() || !validLobbyText(options.name, 96) || options.maxPlayers < 2 ||
        options.maxPlayers > MaxPeers || options.properties.size() > MaxLobbyProperties ||
        static_cast<unsigned>(options.visibility) > static_cast<unsigned>(SteamLobbyVisibility::Public)) return false;
    return std::ranges::all_of(options.properties, [](const auto& pair) {
        return validLobbyKey(pair.first, 32) && !pair.second.empty() && validLobbyText(pair.second, 192);
    });
}

bool lobbyHasMember(ISteamMatchmaking& api, CSteamID lobby, CSteamID member) {
    const int count = std::min(api.GetNumLobbyMembers(lobby), static_cast<int>(MaxPeers));
    for (int i = 0; i < count; ++i) {
        if (api.GetLobbyMemberByIndex(lobby, i) == member) return true;
    }
    return false;
}

std::optional<SteamLobbyInfo> readLobby(ISteamMatchmaking& api, CSteamID id, std::uint64_t compatibility, bool member) {
    if (!compatibility || !validLobbyId(id.ConvertToUint64()) ||
        parseInteger(api.GetLobbyData(id, LobbyBuildKey)) != compatibility) return {};
    const auto host = parseInteger(api.GetLobbyData(id, LobbyHostKey));
    const auto visibility = parseInteger(api.GetLobbyData(id, LobbyVisibilityKey));
    const auto joinable = parseInteger(api.GetLobbyData(id, LobbyJoinableKey));
    const auto players = parseInteger(api.GetLobbyData(id, LobbyPlayersKey));
    const auto capacity = parseInteger(api.GetLobbyData(id, LobbyCapacityKey));
    if (!host || !validSteamUserId(*host) || !visibility || *visibility > static_cast<unsigned>(SteamLobbyVisibility::Public) ||
        !joinable || *joinable > 1 || !players || *players == 0 || *players > MaxPeers || !capacity ||
        *capacity < 2 || *capacity > MaxPeers || (!member && *players > *capacity)) return {};
    const auto limit = static_cast<int>(*capacity);
    if (member && api.GetLobbyOwner(id).ConvertToUint64() != *host) return {};
    SteamLobbyInfo result;
    result.id = id.ConvertToUint64();
    result.hostId = *host;
    const auto playerCount = member ? api.GetNumLobbyMembers(id) : static_cast<int>(*players);
    if (playerCount < 1 || playerCount > limit) return {};
    result.players = static_cast<unsigned>(playerCount);
    result.options.name = api.GetLobbyData(id, LobbyNameKey);
    result.options.visibility = static_cast<SteamLobbyVisibility>(*visibility);
    result.options.maxPlayers = static_cast<unsigned>(limit);
    result.options.joinable = *joinable != 0;
    const int count = std::min(api.GetLobbyDataCount(id), 64);
    for (int i = 0; i < count; ++i) {
        std::array<char, 256> key{};
        std::array<char, 193> value{};
        if (!api.GetLobbyDataByIndex(id, i, key.data(), static_cast<int>(key.size()), value.data(),
                                   static_cast<int>(value.size()))) continue;
        const std::string_view name(key.data(), static_cast<std::size_t>(std::find(key.begin(), key.end(), '\0') - key.begin()));
        if (!name.starts_with(LobbyPropertyPrefix)) continue;
        const std::string_view property(value.data(), static_cast<std::size_t>(std::find(value.begin(), value.end(), '\0') - value.begin()));
        if (result.options.properties.size() == MaxLobbyProperties || !validLobbyKey(name.substr(LobbyPropertyPrefix.size()), 32) ||
            property.empty() || !validLobbyText(property, 192)) continue;
        result.options.properties.emplace(name.substr(LobbyPropertyPrefix.size()), property);
    }
    if (!validLobbyOptions(result.options)) return {};
    return result;
}

}

struct SteamTransport::Impl {
    struct Peer {
        HSteamNetConnection connection{k_HSteamNetConnection_Invalid};
        std::uint64_t id{};
        Clock::time_point lastDatagram{};
        bool connected{};
    };

    SteamSdk sdk;
    ISteamNetworkingSockets* sockets{};
    std::optional<CCallback<Impl, SteamNetConnectionStatusChangedCallback_t>> statusCallback;
    std::array<Peer, IncomingCapacity> peers{};
    HSteamNetPollGroup pollGroup{k_HSteamNetPollGroup_Invalid};
    HSteamListenSocket listen{k_HSteamListenSocket_Invalid};
    Endpoint local{};
    std::uint64_t lobbyId{};
    Clock::time_point nextLobbyCheck{};

    void close(Peer& peer) {
        if (peer.connection != k_HSteamNetConnection_Invalid)
            sockets->CloseConnection(peer.connection, 0, nullptr, false);
        peer = {};
    }

    void stop() {
        statusCallback.reset();
        if (sockets) {
            for (auto& peer : peers) close(peer);
            if (listen != k_HSteamListenSocket_Invalid) sockets->CloseListenSocket(listen);
            if (pollGroup != k_HSteamNetPollGroup_Invalid) sockets->DestroyPollGroup(pollGroup);
        }
        sockets = nullptr;
        listen = k_HSteamListenSocket_Invalid;
        pollGroup = k_HSteamNetPollGroup_Invalid;
        local = {};
        lobbyId = 0;
        nextLobbyCheck = {};
        sdk.stop();
    }

    bool fail(std::string_view reason, std::string& error) {
        stop();
        error = reason;
        return false;
    }

    bool start(std::string& error) {
        stop();
        if (!sdk.start(error)) return false;
        auto* utils = SteamNetworkingUtils();
        sockets = SteamNetworkingSockets();
        const auto id = sdk.localUserId();
        if (!sockets || !utils || !validSteamUserId(id))
            return fail("Steam networking requires a signed-in Steam account", error);
        utils->InitRelayNetworkAccess();
        pollGroup = sockets->CreatePollGroup();
        if (pollGroup == k_HSteamNetPollGroup_Invalid) return fail("Could not create Steam poll group", error);
        statusCallback.emplace(this, &Impl::onConnection);
        local = steamEndpoint(id);
        return true;
    }

    void accept(const SteamNetConnectionStatusChangedCallback_t& event) {
        const auto id = event.m_info.m_identityRemote.GetSteamID64();
        const auto available = std::find_if(peers.begin(), peers.end(), [](const Peer& peer) { return peer.id == 0; });
        const bool duplicate = std::any_of(peers.begin(), peers.end(), [=](const Peer& peer) { return peer.id == id; });
        if (!validSteamUserId(id) || duplicate || available == peers.end() || !allows(id) ||
            sockets->AcceptConnection(event.m_hConn) != k_EResultOK ||
            !sockets->SetConnectionPollGroup(event.m_hConn, pollGroup)) {
            sockets->CloseConnection(event.m_hConn, 0, nullptr, false);
            return;
        }
        *available = {event.m_hConn, id, Clock::now(), false};
    }

    bool allows(std::uint64_t id) const {
        const CSteamID peer(id);
        if (!lobbyId) {
            auto* friends = SteamFriends();
            return friends && friends->GetFriendRelationship(peer) == k_EFriendRelationshipFriend;
        }
        const CSteamID lobby(lobbyId);
        auto* matches = ownedLobby(lobby);
        return matches && lobbyHasMember(*matches, lobby, peer);
    }

    ISteamMatchmaking* ownedLobby(CSteamID lobby) const {
        auto* api = SteamMatchmaking();
        if (!api || !validLobbyId(lobbyId) || api->GetLobbyOwner(lobby).ConvertToUint64() != local.steamId ||
            parseInteger(api->GetLobbyData(lobby, LobbyHostKey)) != local.steamId) return nullptr;
        return api;
    }

    void revokeUnauthorizedPeers() {
        if (listen == k_HSteamListenSocket_Invalid) return;
        if (!lobbyId) {
            for (auto& peer : peers) {
                if (peer.id && !allows(peer.id)) close(peer);
            }
            return;
        }
        std::array<std::uint64_t, MaxPeers> members{};
        const CSteamID lobby(lobbyId);
        if (auto* api = ownedLobby(lobby)) {
            const int count = std::min(api->GetNumLobbyMembers(lobby), static_cast<int>(members.size()));
            for (int i = 0; i < count; ++i) {
                members[static_cast<std::size_t>(i)] = api->GetLobbyMemberByIndex(lobby, i).ConvertToUint64();
            }
        }
        for (auto& peer : peers) {
            if (peer.id && std::ranges::find(members, peer.id) == members.end()) close(peer);
        }
    }

    void onConnection(SteamNetConnectionStatusChangedCallback_t* event) {
        const auto state = event->m_info.m_eState;
        if (listen != k_HSteamListenSocket_Invalid && event->m_info.m_hListenSocket == listen &&
            event->m_eOldState == k_ESteamNetworkingConnectionState_None &&
            state == k_ESteamNetworkingConnectionState_Connecting) {
            accept(*event);
            return;
        }
        const auto peer = std::find_if(peers.begin(), peers.end(), [&](const Peer& item) {
            return item.connection == event->m_hConn;
        });
        if (peer == peers.end()) return;
        switch (state) {
        case k_ESteamNetworkingConnectionState_Connected:
            if (peer->id != event->m_info.m_identityRemote.GetSteamID64()) {
                close(*peer);
                return;
            }
            peer->connected = true;
            peer->lastDatagram = Clock::now();
            break;
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
            close(*peer);
            break;
        default:
            break;
        }
    }
};

struct SteamInvites::Impl {
    struct PendingLobby {
        SteamLobbyAction action{};
        std::uint64_t compatibility{};
        SteamLobbyOptions options;
        unsigned resultLimit{};
        Clock::time_point deadline;
        bool abandoned{};
    };

    SteamSdk sdk;
    std::optional<CCallback<Impl, GameRichPresenceJoinRequested_t>> callback;
    std::optional<CCallback<Impl, GameLobbyJoinRequested_t>> lobbyInviteCallback;
    CCallResult<Impl, LobbyCreated_t> createResult;
    CCallResult<Impl, LobbyMatchList_t> findResult;
    CCallResult<Impl, LobbyEnter_t> joinResult;
    std::optional<SteamJoin> join;
    std::optional<PendingLobby> pending;
    std::optional<SteamLobbyInfo> activeLobby;
    std::optional<SteamLobbyOptions> hostOptions;
    std::vector<SteamLobbyInfo> foundLobbies;
    std::deque<SteamLobbyEvent> lobbyEvents;
    std::unordered_set<std::string> richPresenceKeys;
    std::uint64_t compatibility{};
    bool advertised{};
    Clock::time_point nextLobbyPoll{};

    void pushEvent(SteamLobbyAction action, bool ok, std::uint64_t lobbyId = 0, std::uint64_t hostId = 0,
                   std::string error = {}) {
        constexpr std::size_t EventLimit = 16;
        if (lobbyEvents.size() == EventLimit) lobbyEvents.pop_front();
        lobbyEvents.push_back({action, ok, lobbyId, hostId, std::move(error)});
    }

    ISteamMatchmaking* matchmaking(std::string& error) const {
        auto* user = callback ? SteamUser() : nullptr;
        auto* api = user && user->BLoggedOn() ? SteamMatchmaking() : nullptr;
        if (!api) error = "Steam matchmaking requires a signed-in Steam account";
        return api;
    }

    bool canBegin(bool membershipOperation, std::string& error) const {
        if (pending) {
            error = "A Steam lobby operation is still pending";
            return false;
        }
        if (membershipOperation && activeLobby) {
            error = "Leave the current Steam lobby first";
            return false;
        }
        return true;
    }

    bool writeOptions(ISteamMatchmaking& api, CSteamID id, const SteamLobbyOptions& options, bool publish,
                      const std::map<std::string, std::string>& previous) {
        if (!api.SetLobbyJoinable(id, false) ||
            !api.SetLobbyMemberLimit(id, static_cast<int>(options.maxPlayers)) ||
            !api.SetLobbyData(id, LobbyCapacityKey, std::to_string(options.maxPlayers).c_str()) ||
            !api.SetLobbyData(id, LobbyNameKey, options.name.c_str()) ||
            !api.SetLobbyData(id, LobbyVisibilityKey, std::to_string(static_cast<unsigned>(options.visibility)).c_str())) return false;
        for (const auto& [key, value] : previous) {
            (void)value;
            if (!options.properties.contains(key)) {
                const auto dataKey = std::string(LobbyPropertyPrefix) + key;
                if (*api.GetLobbyData(id, dataKey.c_str()) && !api.DeleteLobbyData(id, dataKey.c_str())) return false;
            }
        }
        for (const auto& [key, value] : options.properties) {
            if (!api.SetLobbyData(id, (std::string(LobbyPropertyPrefix) + key).c_str(), value.c_str())) return false;
        }
        const bool joinable = publish && options.joinable;
        return api.SetLobbyData(id, LobbyJoinableKey, joinable ? "1" : "0") &&
            api.SetLobbyType(id, publish ? sdkLobbyType(options.visibility) : k_ELobbyTypePrivate) &&
            (!joinable || api.SetLobbyJoinable(id, true));
    }

    void leaveCurrentLobby() {
        if (activeLobby) {
            if (auto* api = SteamMatchmaking()) {
                const CSteamID id(activeLobby->id);
                if (hostOptions && api->GetLobbyOwner(id).ConvertToUint64() == activeLobby->hostId) {
                    api->SetLobbyJoinable(id, false);
                    api->SetLobbyData(id, LobbyJoinableKey, "0");
                }
                api->LeaveLobby(id);
            }
        }
        activeLobby.reset();
        hostOptions.reset();
        compatibility = 0;
        advertised = false;
    }

    void onCreate(LobbyCreated_t* result, bool ioFailure) {
        const auto request = std::exchange(pending, {});
        if (!request) return;
        auto* api = SteamMatchmaking();
        if (!api || ioFailure || result->m_eResult != k_EResultOK || !validLobbyId(result->m_ulSteamIDLobby)) {
            if (!request->abandoned) pushEvent(request->action, false, 0, 0, "Steam could not create the lobby");
            return;
        }
        const CSteamID id(result->m_ulSteamIDLobby);
        const auto local = sdk.localUserId();
        if (request->abandoned || !validSteamUserId(local) || api->GetLobbyOwner(id).ConvertToUint64() != local ||
            !api->SetLobbyJoinable(id, false) ||
            !api->SetLobbyData(id, LobbyHostKey, std::to_string(local).c_str()) ||
            !api->SetLobbyData(id, LobbyBuildKey, std::to_string(request->compatibility).c_str()) ||
            !api->SetLobbyData(id, LobbyPlayersKey, "1") || !writeOptions(*api, id, request->options, false, {})) {
            api->LeaveLobby(id);
            if (!request->abandoned) pushEvent(request->action, false, 0, 0, "Could not initialize the Steam lobby");
            return;
        }
        compatibility = request->compatibility;
        hostOptions = request->options;
        activeLobby = SteamLobbyInfo{result->m_ulSteamIDLobby, local, 1, request->options};
        activeLobby->options.joinable = false;
        nextLobbyPoll = Clock::now() + std::chrono::milliseconds(250);
        pushEvent(request->action, true, activeLobby->id, local);
    }

    void onFind(LobbyMatchList_t* result, bool ioFailure) {
        const auto request = std::exchange(pending, {});
        if (!request || request->abandoned) return;
        auto* api = SteamMatchmaking();
        if (!api || ioFailure) {
            pushEvent(request->action, false, 0, 0, "Steam lobby search failed");
            return;
        }
        const auto count = std::min(request->resultLimit, result->m_nLobbiesMatching);
        foundLobbies.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            const auto lobby = readLobby(*api, api->GetLobbyByIndex(static_cast<int>(i)), request->compatibility, false);
            if (lobby && lobby->options.joinable && lobby->options.visibility == SteamLobbyVisibility::Public &&
                lobby->players < lobby->options.maxPlayers) foundLobbies.push_back(*lobby);
        }
        pushEvent(request->action, true);
    }

    void onEnter(LobbyEnter_t* result, bool ioFailure) {
        const auto request = std::exchange(pending, {});
        if (!request) return;
        auto* api = SteamMatchmaking();
        if (!api || ioFailure || result->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess ||
            !validLobbyId(result->m_ulSteamIDLobby)) {
            if (!request->abandoned) pushEvent(request->action, false, 0, 0, "Steam rejected the lobby join request");
            return;
        }
        const CSteamID id(result->m_ulSteamIDLobby);
        auto lobby = request->abandoned ? std::optional<SteamLobbyInfo>{} : readLobby(*api, id, request->compatibility, true);
        if (!lobby || !lobby->options.joinable || lobby->hostId == sdk.localUserId()) {
            api->LeaveLobby(id);
            if (!request->abandoned) pushEvent(request->action, false, 0, 0, "The lobby is closed or uses a different game build");
            return;
        }
        compatibility = request->compatibility;
        activeLobby = std::move(lobby);
        nextLobbyPoll = Clock::now() + std::chrono::milliseconds(250);
        pushEvent(request->action, true, activeLobby->id, activeLobby->hostId);
    }

    void onLobbyInvite(GameLobbyJoinRequested_t* event) {
        const auto id = event->m_steamIDLobby.ConvertToUint64();
        if (validLobbyId(id) && (!activeLobby || activeLobby->id != id))
            pushEvent(SteamLobbyAction::JoinRequested, true, id);
    }

    void refreshLobby() {
        const auto now = Clock::now();
        if (pending && !pending->abandoned && now >= pending->deadline) {
            pending->abandoned = true;
            pushEvent(pending->action, false, 0, 0, "Steam lobby operation timed out");
        }
        if (!activeLobby || now < nextLobbyPoll) return;
        nextLobbyPoll = now + std::chrono::milliseconds(250);
        auto* api = SteamMatchmaking();
        const CSteamID id(activeLobby->id);
        if (!api || api->GetLobbyOwner(id).ConvertToUint64() != activeLobby->hostId ||
            !lobbyHasMember(*api, id, CSteamID(sdk.localUserId()))) {
            pushEvent(SteamLobbyAction::Lost, false, activeLobby->id, activeLobby->hostId, "The Steam lobby host left");
            leaveCurrentLobby();
            return;
        }
        auto snapshot = readLobby(*api, id, compatibility, true);
        if (!snapshot || snapshot->hostId != activeLobby->hostId) return;
        if (hostOptions && snapshot->players != activeLobby->players) {
            if (!api->SetLobbyData(id, LobbyPlayersKey, std::to_string(snapshot->players).c_str())) {
                snapshot->players = activeLobby->players;
            }
        }
        activeLobby = std::move(snapshot);
    }

    void onJoin(GameRichPresenceJoinRequested_t* event) {
        auto* friends = SteamFriends();
        const auto who = event->m_steamIDFriend;
        if (!friends || !validSteamUserId(who.ConvertToUint64()) ||
            friends->GetFriendRelationship(who) != k_EFriendRelationshipFriend) return;
        const auto begin = std::begin(event->m_rgchConnect);
        const auto end = std::find(begin, std::end(event->m_rgchConnect), char{});
        if (const auto requested = parseInviteCommand({begin, static_cast<std::size_t>(end - begin)});
            requested && requested->hostId == who.ConvertToUint64())
            join = requested;
    }
};

#else

namespace {
constexpr std::string_view BuildUnavailable = "This build has no Steamworks support";
}
struct SteamTransport::Impl {};
struct SteamInvites::Impl {};

#endif

SteamTransport::SteamTransport() : m_impl(std::make_unique<Impl>()) {}
SteamTransport::~SteamTransport() { stop(); }

bool SteamTransport::startServer(std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!self.start(error)) return false;
    self.listen = self.sockets->CreateListenSocketP2P(SteamVirtualPort, 0, nullptr);
    if (self.listen == k_HSteamListenSocket_Invalid)
        return self.fail("Could not listen on the Steam virtual port", error);
    return true;
#else
    error = BuildUnavailable;
    return false;
#endif
}

bool SteamTransport::startClient(Endpoint host, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (host.address || host.port != SteamVirtualPort || !validSteamUserId(host.steamId)) {
        error = "Invalid Steam host ID or virtual port";
        return false;
    }
    if (!self.start(error)) return false;
    if (self.local.steamId == host.steamId)
        return self.fail("Steam requires a different account for the client and host", error);
    SteamNetworkingIdentity identity{};
    identity.SetSteamID64(host.steamId);
    SteamNetworkingConfigValue_t timeout;
    timeout.SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, InitialTimeoutMs);
    const auto connection = self.sockets->ConnectP2P(identity, SteamVirtualPort, 1, &timeout);
    if (connection == k_HSteamNetConnection_Invalid)
        return self.fail("Could not start a Steam connection to the host", error);
    self.peers.front() = {connection, host.steamId, Clock::now(), false};
    if (!self.sockets->SetConnectionPollGroup(connection, self.pollGroup))
        return self.fail("Could not receive Steam connection messages", error);
    return true;
#else
    (void)host;
    error = BuildUnavailable;
    return false;
#endif
}

void SteamTransport::setLobby(std::uint64_t lobbyId) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (std::exchange(self.lobbyId, lobbyId) != lobbyId) {
        self.revokeUnauthorizedPeers();
        self.nextLobbyCheck = Clock::now() + std::chrono::seconds(1);
    }
#else
    (void)lobbyId;
#endif
}

void SteamTransport::stop() {
#ifdef ENGINE_ENABLE_STEAMWORKS
    m_impl->stop();
#endif
}

void SteamTransport::poll() {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    self.sdk.poll();
    if (self.listen == k_HSteamListenSocket_Invalid) return;
    const auto now = Clock::now();
    if (self.lobbyId && now >= self.nextLobbyCheck) {
        self.nextLobbyCheck = now + std::chrono::seconds(1);
        self.revokeUnauthorizedPeers();
    }
    for (auto& peer : self.peers) {
        if (peer.id && now - peer.lastDatagram >= IncomingIdleTime) self.close(peer);
    }
#endif
}

Endpoint SteamTransport::localEndpoint() const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    return m_impl->local;
#else
    return {};
#endif
}

bool SteamTransport::send(Endpoint peer, std::span<const std::uint8_t> data) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!self.sockets || peer.address || peer.port != SteamVirtualPort || data.empty() ||
        data.size() > MaxDatagramBytes) return false;
    const auto target = std::find_if(self.peers.begin(), self.peers.end(), [=](const Impl::Peer& item) {
        return item.id == peer.steamId && item.connected;
    });
    if (target == self.peers.end()) return false;
    return self.sockets->SendMessageToConnection(target->connection, data.data(),
        static_cast<std::uint32_t>(data.size()), k_nSteamNetworkingSend_UnreliableNoDelay, nullptr) == k_EResultOK;
#else
    (void)peer;
    (void)data;
    return false;
#endif
}

bool SteamTransport::receive(Datagram& datagram) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!self.sockets) return false;
    for (unsigned i = 0; i < 64; ++i) {
        SteamNetworkingMessage_t* received{};
        if (self.sockets->ReceiveMessagesOnPollGroup(self.pollGroup, &received, 1) != 1) return false;
        const auto from = std::find_if(self.peers.begin(), self.peers.end(), [&](const Impl::Peer& item) {
            return item.connection == received->m_conn && item.connected;
        });
        const bool valid = from != self.peers.end() && received->m_cbSize > 0 &&
            static_cast<std::size_t>(received->m_cbSize) <= MaxDatagramBytes;
        if (valid) {
            datagram.peer = steamEndpoint(from->id);
            datagram.size = static_cast<std::size_t>(received->m_cbSize);
            std::copy_n(static_cast<const std::uint8_t*>(received->m_pData), datagram.size, datagram.data.begin());
            from->lastDatagram = Clock::now();
        }
        received->Release();
        if (valid) return true;
    }
#else
    (void)datagram;
#endif
    return false;
}

SteamInvites::SteamInvites() : m_impl(std::make_unique<Impl>()) {}
SteamInvites::~SteamInvites() { stop(); }

bool SteamInvites::start(std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (self.callback) {
        error.clear();
        return true;
    }
    if (!self.sdk.start(error)) return false;
    if (!SteamFriends() || !validSteamUserId(localUserId())) {
        self.sdk.stop();
        error = "Steam invitations require a signed-in Steam account";
        return false;
    }
    self.callback.emplace(&self, &Impl::onJoin);
    self.lobbyInviteCallback.emplace(&self, &Impl::onLobbyInvite);
    if (auto* apps = SteamApps()) {
        std::array<char, 128> buffer{};
        const auto length = apps->GetLaunchCommandLine(buffer.data(), static_cast<int>(buffer.size()));
        if (length > 0 && static_cast<std::size_t>(length) < buffer.size()) {
            const auto end = std::find(buffer.begin(), buffer.begin() + length, char{});
            const std::string_view command(buffer.data(), static_cast<std::size_t>(end - buffer.begin()));
            self.join = parseInviteCommand(command);
            constexpr std::string_view LobbyCommand = "+connect_lobby ";
            if (command.starts_with(LobbyCommand)) {
                const auto lobby = parseInteger(command.substr(LobbyCommand.size()));
                if (lobby && validLobbyId(*lobby)) self.pushEvent(SteamLobbyAction::JoinRequested, true, *lobby);
            }
        }
    }
    return true;
#else
    error = BuildUnavailable;
    return false;
#endif
}

void SteamInvites::stop() {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (self.callback) {
        self.leaveCurrentLobby();
        self.createResult.Cancel();
        self.findResult.Cancel();
        self.joinResult.Cancel();
        if (auto* friends = SteamFriends()) {
            for (const auto& key : self.richPresenceKeys) friends->SetRichPresence(key.c_str(), nullptr);
        }
    }
    self.lobbyInviteCallback.reset();
    self.callback.reset();
    self.pending.reset();
    self.join.reset();
    self.foundLobbies.clear();
    self.lobbyEvents.clear();
    self.richPresenceKeys.clear();
    self.sdk.stop();
#endif
}

void SteamInvites::poll() {
#ifdef ENGINE_ENABLE_STEAMWORKS
    m_impl->sdk.poll();
    m_impl->refreshLobby();
#endif
}

std::uint64_t SteamInvites::localUserId() const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    return m_impl->sdk.localUserId();
#else
    return 0;
#endif
}

std::optional<std::vector<SteamFriend>> SteamInvites::friends() const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    if (!m_impl->callback) return {};
    auto* user = SteamUser();
    auto* api = SteamFriends();
    if (!user || !user->BLoggedOn() || !api) return {};
    const auto count = api->GetFriendCount(k_EFriendFlagImmediate);
    if (count < 0) return {};
    const auto limit = std::min(count, static_cast<int>(MaxSteamFriends));
    std::vector<SteamFriend> snapshot;
    snapshot.reserve(static_cast<std::size_t>(limit));
    std::unordered_set<std::uint64_t> seen;
    for (int index = 0; index < limit; ++index) {
        const auto friendId = api->GetFriendByIndex(index, k_EFriendFlagImmediate);
        const auto id = friendId.ConvertToUint64();
        if (!validSteamUserId(id) || !seen.insert(id).second) continue;
        const std::string_view persona = api->GetFriendPersonaName(friendId);
        std::string displayName;
        if (!persona.empty() && persona.size() <= MaxSteamFriendNameBytes &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, persona.data(), static_cast<int>(persona.size()),
                                nullptr, 0) > 0) {
            displayName = persona;
            std::ranges::replace_if(displayName, [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }, ' ');
        }
        if (displayName.find_first_not_of(' ') == std::string::npos) displayName = std::to_string(id);
        auto presence = SteamFriendPresence::Offline;
        switch (api->GetFriendPersonaState(friendId)) {
            case k_EPersonaStateOnline:
            case k_EPersonaStateLookingToTrade:
            case k_EPersonaStateLookingToPlay:
                presence = SteamFriendPresence::Online;
                break;
            case k_EPersonaStateAway:
            case k_EPersonaStateSnooze:
                presence = SteamFriendPresence::Away;
                break;
            case k_EPersonaStateBusy:
                presence = SteamFriendPresence::Busy;
                break;
            default:
                break;
        }
        snapshot.push_back({id, std::move(displayName), presence});
    }
    std::ranges::sort(snapshot, [](const SteamFriend& a, const SteamFriend& b) {
        return a.name == b.name ? a.id < b.id : a.name < b.name;
    });
    return snapshot;
#else
    return {};
#endif
}

std::optional<int> SteamInvites::smallAvatarHandle(std::uint64_t friendId) const {
    return avatarHandle(friendId, false, false);
}

std::optional<int> SteamInvites::smallPeerAvatarHandle(std::uint64_t peerId, bool requestInformation) const {
    return avatarHandle(peerId, true, requestInformation);
}

std::optional<int> SteamInvites::avatarHandle(std::uint64_t id, bool allowPeer, bool requestInformation) const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    if (!m_impl->callback || !validSteamUserId(id)) return {};
    auto* user = SteamUser();
    auto* api = SteamFriends();
    if (!user || !user->BLoggedOn() || !api) return {};
    const CSteamID target(id);
    if (!allowPeer && user->GetSteamID() != target &&
        api->GetFriendRelationship(target) != k_EFriendRelationshipFriend) return {};
    const auto image = api->GetSmallFriendAvatar(target);
    if (image > 0 || !requestInformation) return image;
    if (api->RequestUserInformation(target, false)) return -1;
    return api->GetSmallFriendAvatar(target);
#else
    (void)id;
    (void)allowPeer;
    (void)requestInformation;
    return {};
#endif
}

std::string SteamInvites::smallAvatarTga(int image) const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    if (!m_impl->callback || image <= 0) return {};
    auto* utils = SteamUtils();
    std::uint32_t width{}, height{};
    if (!utils || !utils->GetImageSize(image, &width, &height) ||
        width != SteamAvatarSide || height != SteamAvatarSide) return {};

    std::array<std::uint8_t, SteamAvatarFileBytes - SteamAvatarTgaHeader.size()> pixels{};
    if (!utils->GetImageRGBA(image, pixels.data(), static_cast<int>(pixels.size()))) return {};
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        std::swap(pixels[offset], pixels[offset + 2]);
    }
    std::string tga(SteamAvatarFileBytes, '\0');
    std::memcpy(tga.data(), SteamAvatarTgaHeader.data(), SteamAvatarTgaHeader.size());
    std::memcpy(tga.data() + SteamAvatarTgaHeader.size(), pixels.data(), pixels.size());
    return tga;
#else
    (void)image;
    return {};
#endif
}

bool SteamInvites::invite(std::uint64_t friendId, SteamJoin join, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    const auto user = localUserId();
    auto* friends = user ? SteamFriends() : nullptr;
    const auto command = inviteCommand(join);
    if (!friends || !validSteamUserId(friendId) || join.hostId != user || command.empty()) {
        error = "Steam invitations require the signed-in host and a valid friend Steam ID";
        return false;
    }
    const CSteamID target(friendId);
    if (friends->GetFriendRelationship(target) != k_EFriendRelationshipFriend) {
        error = "The Steam account is outside your friends list";
        return false;
    }
    if (!friends->InviteUserToGame(target, command.c_str())) {
        error = "Steam did not send the invitation";
        return false;
    }
    error.clear();
    return true;
#else
    (void)friendId;
    (void)join;
    error = BuildUnavailable;
    return false;
#endif
}

std::optional<SteamJoin> SteamInvites::takeJoin() {
#ifdef ENGINE_ENABLE_STEAMWORKS
    return std::exchange(m_impl->join, {});
#else
    return {};
#endif
}

bool SteamInvites::createLobby(std::uint64_t compatibility, const SteamLobbyOptions& options, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!compatibility || !validLobbyOptions(options)) {
        error = "Invalid Steam lobby options or game build";
        return false;
    }
    auto* api = self.matchmaking(error);
    if (!api || !self.canBegin(true, error)) return false;
    const auto call = api->CreateLobby(k_ELobbyTypePrivate, static_cast<int>(options.maxPlayers));
    if (call == k_uAPICallInvalid) {
        error = "Could not submit the Steam lobby create request";
        return false;
    }
    self.pending.emplace(Impl::PendingLobby{SteamLobbyAction::Create, compatibility, options, 0, Clock::now() + LobbyTimeout});
    self.createResult.Set(call, &self, &Impl::onCreate);
    error.clear();
    return true;
#else
    (void)compatibility;
    (void)options;
    error = BuildUnavailable;
    return false;
#endif
}

bool SteamInvites::activateLobby(std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!self.activeLobby || !self.hostOptions) {
        error = "There is no hosted Steam lobby to publish";
        return false;
    }
    if (self.advertised) {
        error.clear();
        return true;
    }
    auto* api = self.matchmaking(error);
    if (!api) return false;
    const CSteamID id(self.activeLobby->id);
    if (api->GetLobbyOwner(id).ConvertToUint64() != self.sdk.localUserId() ||
        !self.writeOptions(*api, id, *self.hostOptions, true, {})) {
        error = "Steam did not publish the lobby";
        self.leaveCurrentLobby();
        return false;
    }
    self.advertised = true;
    self.activeLobby->options = *self.hostOptions;
    error.clear();
    return true;
#else
    error = BuildUnavailable;
    return false;
#endif
}

bool SteamInvites::updateLobby(const SteamLobbyOptions& options, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!self.activeLobby || !self.hostOptions || !validLobbyOptions(options) || options.maxPlayers < self.activeLobby->players) {
        error = "Updating a Steam lobby requires the host and valid lobby settings";
        return false;
    }
    auto* api = self.matchmaking(error);
    if (!api) return false;
    const CSteamID id(self.activeLobby->id);
    if (api->GetLobbyOwner(id).ConvertToUint64() != self.sdk.localUserId()) {
        error = "Only the Steam lobby host can update its settings";
        return false;
    }
    if (!self.writeOptions(*api, id, options, self.advertised, self.hostOptions->properties)) {
        error = "Steam did not update the lobby settings";
        if (!self.writeOptions(*api, id, *self.hostOptions, self.advertised, options.properties)) {
            self.pushEvent(SteamLobbyAction::Lost, false, self.activeLobby->id, self.activeLobby->hostId,
                "Could not restore the Steam lobby settings");
            self.leaveCurrentLobby();
        }
        return false;
    }
    self.hostOptions = options;
    self.activeLobby->options = options;
    self.activeLobby->options.joinable = self.advertised && options.joinable;
    error.clear();
    return true;
#else
    (void)options;
    error = BuildUnavailable;
    return false;
#endif
}

bool SteamInvites::findLobbies(std::uint64_t compatibility, unsigned limit, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!compatibility || !limit || limit > MaxLobbyResults) {
        error = "Steam lobby search requires a game build and 1 to 50 results";
        return false;
    }
    auto* api = self.matchmaking(error);
    if (!api || !self.canBegin(false, error)) return false;
    api->AddRequestLobbyListStringFilter(LobbyBuildKey, std::to_string(compatibility).c_str(), k_ELobbyComparisonEqual);
    api->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
    api->AddRequestLobbyListFilterSlotsAvailable(1);
    api->AddRequestLobbyListResultCountFilter(static_cast<int>(limit));
    const auto call = api->RequestLobbyList();
    if (call == k_uAPICallInvalid) {
        error = "Could not submit the Steam lobby search";
        return false;
    }
    self.foundLobbies.clear();
    self.pending.emplace(Impl::PendingLobby{SteamLobbyAction::Find, compatibility, {}, limit, Clock::now() + LobbyTimeout});
    self.findResult.Set(call, &self, &Impl::onFind);
    error.clear();
    return true;
#else
    (void)compatibility;
    (void)limit;
    error = BuildUnavailable;
    return false;
#endif
}

bool SteamInvites::joinLobby(std::uint64_t lobbyId, std::uint64_t compatibility, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!compatibility || !validLobbyId(lobbyId)) {
        error = "Invalid Steam lobby ID or game build";
        return false;
    }
    auto* api = self.matchmaking(error);
    if (!api || !self.canBegin(true, error)) return false;
    const auto call = api->JoinLobby(CSteamID(lobbyId));
    if (call == k_uAPICallInvalid) {
        error = "Could not submit the Steam lobby join request";
        return false;
    }
    self.pending.emplace(Impl::PendingLobby{SteamLobbyAction::Join, compatibility, {}, 0, Clock::now() + LobbyTimeout});
    self.joinResult.Set(call, &self, &Impl::onEnter);
    error.clear();
    return true;
#else
    (void)lobbyId;
    (void)compatibility;
    error = BuildUnavailable;
    return false;
#endif
}

void SteamInvites::leaveLobby() {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (self.pending && !self.pending->abandoned) {
        self.pending->abandoned = true;
        self.pushEvent(self.pending->action, false, 0, 0, "Steam lobby operation canceled");
    }
    self.leaveCurrentLobby();
#endif
}

std::optional<SteamLobbyInfo> SteamInvites::lobby() const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    return m_impl->activeLobby;
#else
    return {};
#endif
}

const std::vector<SteamLobbyInfo>& SteamInvites::lobbies() const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    return m_impl->foundLobbies;
#else
    static const std::vector<SteamLobbyInfo> empty;
    return empty;
#endif
}

std::optional<SteamLobbyEvent> SteamInvites::takeLobbyEvent() {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& events = m_impl->lobbyEvents;
    if (!events.empty()) {
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
#endif
    return {};
}

bool SteamInvites::inviteLobbyFriend(std::uint64_t friendId, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    if (!self.activeLobby || !self.activeLobby->options.joinable || !validSteamUserId(friendId)) {
        error = "A joinable Steam lobby and a friend SteamID are required";
        return false;
    }
    auto* api = self.matchmaking(error);
    if (!api) return false;
    auto* friends = SteamFriends();
    const CSteamID target(friendId);
    if (!friends || friends->GetFriendRelationship(target) != k_EFriendRelationshipFriend) {
        error = "The Steam account is outside your friends list";
        return false;
    }
    if (!api->InviteUserToLobby(CSteamID(self.activeLobby->id), target)) {
        error = "Steam could not send the lobby invitation";
        return false;
    }
    error.clear();
    return true;
#else
    (void)friendId;
    error = BuildUnavailable;
    return false;
#endif
}

std::string SteamInvites::localUserName() const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    if (auto* api = m_impl->callback ? SteamFriends() : nullptr) {
        const std::string_view name = api->GetPersonaName();
        if (validLobbyText(name, MaxSteamFriendNameBytes)) return std::string(name);
    }
#endif
    return {};
}

std::string SteamInvites::userName(std::uint64_t steamId) const {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto* api = m_impl->callback ? SteamFriends() : nullptr;
    if (!api || !validSteamUserId(steamId)) return {};
    if (steamId == localUserId()) return localUserName();
    const CSteamID user(steamId);
    const std::string_view name = api->GetFriendPersonaName(user);
    if (name.empty() || name == "[unknown]") {
        api->RequestUserInformation(user, true);
        return {};
    }
    if (name.find_first_not_of(' ') != std::string_view::npos && validLobbyText(name, MaxSteamFriendNameBytes)) {
        return std::string(name);
    }
#else
    (void)steamId;
#endif
    return {};
}

bool SteamInvites::setRichPresence(std::string_view key, std::string_view value, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto& self = *m_impl;
    auto* api = self.callback ? SteamFriends() : nullptr;
    if (!api || !validLobbyKey(key, k_cchMaxRichPresenceKeyLength - 1) || key == "connect" ||
        !validLobbyText(value, k_cchMaxRichPresenceValueLength - 1)) {
        error = "Invalid Steam rich presence key or value";
        return false;
    }
    const std::string name(key);
    if ((!value.empty() && !self.richPresenceKeys.contains(name) && self.richPresenceKeys.size() >= k_cchMaxRichPresenceKeys) ||
        !api->SetRichPresence(name.c_str(), value.empty() ? nullptr : std::string(value).c_str())) {
        error = "Steam did not update rich presence";
        return false;
    }
    if (value.empty()) self.richPresenceKeys.erase(name);
    else self.richPresenceKeys.insert(name);
    error.clear();
    return true;
#else
    (void)key;
    (void)value;
    error = BuildUnavailable;
    return false;
#endif
}

bool SteamInvites::openProfile(std::uint64_t steamId, std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    auto* friends = m_impl->callback ? SteamFriends() : nullptr;
    auto* utils = friends ? SteamUtils() : nullptr;
    if (!utils || !utils->IsOverlayEnabled() || !validSteamUserId(steamId)) {
        error = "Steam overlay or the requested player profile is unavailable";
        return false;
    }
    friends->ActivateGameOverlayToUser("steamid", CSteamID(steamId));
    error.clear();
    return true;
#else
    (void)steamId;
    error = BuildUnavailable;
    return false;
#endif
}

bool SteamInvites::openInviteOverlay(std::string& error) {
#ifdef ENGINE_ENABLE_STEAMWORKS
    const auto& self = *m_impl;
    auto* friends = self.callback ? SteamFriends() : nullptr;
    auto* utils = friends ? SteamUtils() : nullptr;
    if (!utils || !utils->IsOverlayEnabled() || !self.activeLobby || !self.activeLobby->options.joinable) {
        error = "Steam overlay or a joinable lobby is unavailable";
        return false;
    }
    friends->ActivateGameOverlayInviteDialog(CSteamID(self.activeLobby->id));
    error.clear();
    return true;
#else
    error = BuildUnavailable;
    return false;
#endif
}

}
