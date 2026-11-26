#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace engine::net {

inline constexpr std::size_t MaxSteamFriends = 2048;
inline constexpr std::size_t MaxSteamFriendNameBytes = 128;
inline constexpr char SteamFriendListFile[] = "steam-friends.json";
inline constexpr char SteamAvatarRequestFile[] = "steam-avatar-requests.json";
inline constexpr std::size_t MaxSteamAvatarRequests = 48;
inline constexpr std::uint8_t SteamAvatarSide = 32;
inline constexpr std::array<std::uint8_t, 18> SteamAvatarTgaHeader{
    0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, SteamAvatarSide, 0, SteamAvatarSide, 0, 32, 0x28};
inline constexpr std::size_t SteamAvatarFileBytes = SteamAvatarTgaHeader.size() + SteamAvatarSide * SteamAvatarSide * 4;

inline std::string steamAvatarFileName(std::uint64_t id) {
    return "steam-avatar-" + std::to_string(id) + ".tga";
}

enum class SteamFriendPresence : std::uint8_t { Offline, Online, Away, Busy };

struct SteamFriend {
    std::uint64_t id{};
    std::string name;
    SteamFriendPresence presence{SteamFriendPresence::Offline};

    bool online() const noexcept { return presence != SteamFriendPresence::Offline; }

    bool operator==(const SteamFriend&) const = default;
};

constexpr bool validSteamUserId(std::uint64_t id) {
    constexpr std::uint32_t PublicIndividualDesktop = 0x01100001;
    return (id >> 32) == PublicIndividualDesktop && static_cast<std::uint32_t>(id) != 0;
}

}
