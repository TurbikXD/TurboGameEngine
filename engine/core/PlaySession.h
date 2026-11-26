#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "engine/core/ProjectManager.h"
#include "engine/network/DatagramTransport.h"
#include "engine/network/NetworkTestHistory.h"
#include "engine/network/NetworkTestOptions.h"
#include "engine/network/SteamFriend.h"

namespace engine::net { class SteamInvites; }

namespace engine::core {

enum class SceneNetworkRole { Offline, Server, Client };

struct PlayLaunch final {
    Project project;
    int width{1280};
    int height{720};
    bool vsync{true};
    std::string device{"auto"};
    SceneNetworkRole networkRole{SceneNetworkRole::Offline};
    net::Endpoint networkEndpoint{0x7f000001, 0};
    std::string networkHost;
    unsigned networkClientIndex{};
    unsigned networkMaxPeers{};
    std::uint64_t networkCompatibility{};
    bool networkSteam{};
    std::uint64_t networkSteamInvite{};
    net::NetworkTestOptions networkOptions{};
    std::filesystem::path networkReadyPath;
    std::filesystem::path networkStopPath;
    std::filesystem::path networkControlDirectory;
    std::filesystem::path networkHostControlDirectory;
    std::filesystem::path reactPackage;
    net::SteamInvites* steamRuntime{};
};

struct OnlinePlayStatus final {
    SceneNetworkRole role{SceneNetworkRole::Offline};
    net::Endpoint endpoint{};
    std::uint64_t compatibility{};
    bool ready{};
    bool invitePending{};
    std::shared_ptr<const std::vector<net::SteamFriend>> friends;
    std::string inviteStatus;
    std::string error;
};

class PlaySession final {
public:
    PlaySession();
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;
    bool start(const std::filesystem::path& executable, const std::string& scene,
               const PlayLaunch& options, std::string& error);
    bool startNetwork(const std::filesystem::path& executable, const std::string& scene,
                      const PlayLaunch& options, std::string& error);
    bool startOnline(const std::filesystem::path& executable, const std::string& scene,
                     PlayLaunch options, std::string& error);
    void pollOnline();
    [[nodiscard]] const OnlinePlayStatus& onlineStatus() const;
    bool inviteOnlineFriend(std::uint64_t friendId, std::string& error);
    std::filesystem::path steamFriendAvatar(std::uint64_t friendId);
    void pollNetwork();
    void stop();
    [[nodiscard]] bool running() const;
    [[nodiscard]] const std::filesystem::path& directory() const;
    [[nodiscard]] const std::string& networkError() const;
    [[nodiscard]] std::vector<net::NetworkTestClientView> networkTestClients() const;
    [[nodiscard]] net::NetworkTestHistoryView networkTestHistory(unsigned index) const;
    [[nodiscard]] bool hasNetworkTestHistory() const;
    bool exportNetworkTestHistory(const std::filesystem::path& target, std::string& error) const;
    void clearNetworkTestHistory();
    bool setNetworkTestConditions(unsigned index, net::NetworkTestConditions conditions, std::string& error);
    static std::optional<PlayLaunch> load(const std::filesystem::path& manifest, std::string& error);
private:
    bool prepare(const std::filesystem::path& executable, const std::string& scene, std::string& error);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
