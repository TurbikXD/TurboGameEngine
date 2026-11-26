#pragma once

#include <map>
#include <vector>

#include "engine/network/DatagramTransport.h"
#include "engine/network/SteamFriend.h"

namespace engine::net {

inline constexpr std::uint16_t SteamVirtualPort = 1;
inline constexpr std::string_view SteamJoinCommand = "+tge_join";

struct SteamJoin {
    std::uint64_t hostId{};
    std::uint64_t compatibilityId{};
    bool operator==(const SteamJoin&) const = default;
};

std::string encodeSteamJoin(SteamJoin join);
std::optional<SteamJoin> parseSteamJoin(std::string_view value);

enum class SteamLobbyVisibility { Private, Friends, Public };

struct SteamLobbyOptions {
    std::string name;
    SteamLobbyVisibility visibility{SteamLobbyVisibility::Friends};
    unsigned maxPlayers{8};
    bool joinable{true};
    std::map<std::string, std::string> properties;
};

struct SteamLobbyInfo {
    std::uint64_t id{};
    std::uint64_t hostId{};
    unsigned players{};
    SteamLobbyOptions options;
};

enum class SteamLobbyAction { Create, Find, Join, JoinRequested, Lost };

struct SteamLobbyEvent {
    SteamLobbyAction action{};
    bool ok{};
    std::uint64_t lobbyId{};
    std::uint64_t hostId{};
    std::string error;
};

class SteamTransport final {
public:
    SteamTransport();
    ~SteamTransport();
    SteamTransport(const SteamTransport&) = delete;
    SteamTransport& operator=(const SteamTransport&) = delete;

    bool startServer(std::string& error);
    bool startClient(Endpoint host, std::string& error);
    void setLobby(std::uint64_t lobbyId);
    void stop();
    void poll();
    Endpoint localEndpoint() const;
    bool send(Endpoint peer, std::span<const std::uint8_t> data);
    bool receive(Datagram& datagram);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class SteamInvites final {
public:
    SteamInvites();
    ~SteamInvites();
    SteamInvites(const SteamInvites&) = delete;
    SteamInvites& operator=(const SteamInvites&) = delete;

    bool start(std::string& error);
    void stop();
    void poll();
    std::uint64_t localUserId() const;
    std::optional<std::vector<SteamFriend>> friends() const;
    std::optional<int> smallAvatarHandle(std::uint64_t friendId) const;
    std::optional<int> smallPeerAvatarHandle(std::uint64_t peerId, bool requestInformation) const;
    std::string smallAvatarTga(int image) const;
    bool invite(std::uint64_t friendId, SteamJoin join, std::string& error);
    std::optional<SteamJoin> takeJoin();

    bool createLobby(std::uint64_t compatibility, const SteamLobbyOptions& options, std::string& error);
    bool activateLobby(std::string& error);
    bool updateLobby(const SteamLobbyOptions& options, std::string& error);
    bool findLobbies(std::uint64_t compatibility, unsigned limit, std::string& error);
    bool joinLobby(std::uint64_t lobbyId, std::uint64_t compatibility, std::string& error);
    void leaveLobby();
    std::optional<SteamLobbyInfo> lobby() const;
    const std::vector<SteamLobbyInfo>& lobbies() const;
    std::optional<SteamLobbyEvent> takeLobbyEvent();
    bool inviteLobbyFriend(std::uint64_t friendId, std::string& error);
    std::string localUserName() const;
    std::string userName(std::uint64_t steamId) const;
    bool setRichPresence(std::string_view key, std::string_view value, std::string& error);
    bool openProfile(std::uint64_t steamId, std::string& error);
    bool openInviteOverlay(std::string& error);

private:
    std::optional<int> avatarHandle(std::uint64_t id, bool allowPeer, bool requestInformation) const;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
