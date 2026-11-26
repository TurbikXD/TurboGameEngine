#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/NetworkTestCsv.h"
#include "engine/editor/EditorCommands.h"
#include "engine/editor/EditorIcons.h"
#include "engine/network/NetworkTestHistory.h"
#include "engine/network/NetworkTestOptions.h"
#include "engine/network/SteamFriend.h"

namespace engine::editor {

enum class EditorRibbonTab { Home, Model, Test, View, Script };

struct EditorRibbonState {
    EditorRibbonTab tab{EditorRibbonTab::Home};
    bool compact{false};
    bool showLabels{true};
    EditorCommandId createCommand{EditorCommandId::CreatePhysicsCube};
    EditorCommandId playCommand{EditorCommandId::Play};
    float moveSnap{0.5F};
    float rotateSnap{15.0F};
    float scaleSnap{0.1F};
    net::NetworkTestOptions networkTest;
};

struct NetworkTestRibbonActions {
    std::function<std::vector<net::NetworkTestClientView>()> clients;
    std::function<bool(unsigned, net::NetworkTestConditions, std::string&)> apply;
    std::function<net::NetworkTestHistoryView(unsigned)> history;
    std::function<bool()> hasHistory;
    std::function<bool(const std::filesystem::path&, std::string&)> exportCsv;
};

struct MultiplayerRibbonStatus {
    bool canLaunch{};
    bool serverRunning{};
    bool serverReady{};
    bool serverSteam{};
    bool clientRunning{};
    bool invitePending{};
    std::string serverStatus;
    std::string clientStatus;
    std::string serverEndpoint;
    std::string inviteStatus;
    std::shared_ptr<const std::vector<net::SteamFriend>> friends;
};

struct MultiplayerRibbonActions {
    std::function<MultiplayerRibbonStatus()> status;
    std::function<bool(bool, std::string_view, unsigned, std::string&)> startServer;
    std::function<void()> stopServer;
    std::function<bool(bool, std::string_view, std::string&)> connect;
    std::function<void()> stopClient;
    std::function<bool(std::uint64_t, std::string&)> invite;
    std::function<bool(std::string&)> saveScene;
    std::function<EditorIcons::Image(std::uint64_t)> avatar;
};

class EditorRibbon final {
public:
    bool render(EditorCommandRegistry& commands, EditorRibbonState& state, const EditorIcons::Resolver& resolveIcon = {},
        const NetworkTestRibbonActions& networkActions = {}, const MultiplayerRibbonActions& multiplayerActions = {},
        unsigned networkPlayerLimit = 0);
    [[nodiscard]] static float preferredHeight(const EditorRibbonState& state);
    static bool sanitizeState(EditorRibbonState& state) noexcept;

private:
    void renderLiveNetworkOptions(const NetworkTestRibbonActions& actions);
    void renderNetworkStats(const NetworkTestRibbonActions& actions, bool running, bool hasHistory);
    void renderNetworkComparison();
    void renderNetworkComparisonHistory(const core::NetworkTestCsvRun& runA, const core::NetworkTestCsvRun& runB);
    void renderMultiplayer(const MultiplayerRibbonActions& actions, int maxPlayers);

    struct MultiplayerPanel {
        bool open{};
        bool serverSteam{};
        bool clientSteam{};
        int maxPlayers{16};
        char bind[22]{"127.0.0.1:27020"};
        char address[260]{};
        char hostSteamId[21]{};
        std::uint64_t friendSteamId{};
        char friendSearch[net::MaxSteamFriendNameBytes + 1]{};
        bool onlineFriendsOnly{};
        std::shared_ptr<const std::vector<net::SteamFriend>> friendFilterSnapshot;
        std::vector<std::size_t> visibleFriends;
        std::string serverError;
        std::string clientError;
        std::string inviteError;
        std::string saveStatus;
    };
    MultiplayerPanel m_multiplayer;

    struct LiveNetworkOptions {
        unsigned clientIndex{};
        net::NetworkTestConditions profile;
        net::NetworkTestConditions draft;
        std::optional<net::NetworkTestConditions> requested;
        std::string error;
    };
    LiveNetworkOptions m_liveNetworkOptions;
    bool m_networkStatsOpen{};
    bool m_networkWasRunning{};
    unsigned m_historyClientIndex{};
    std::size_t m_historyRangeIndex{};
    std::string m_networkExportMessage;
    bool m_networkExportFailed{};

    struct ComparisonFile {
        std::optional<core::NetworkTestCsvRun> run;
        std::filesystem::path path;
        std::string filename;
        std::string error;
    };
    std::array<ComparisonFile, 2> m_networkComparisonFiles;
    bool m_networkCompare{};
    unsigned m_comparisonClientIndex{};
    std::size_t m_comparisonRangeIndex{};
    std::filesystem::path m_comparisonExportPath;
    std::optional<bool> m_comparisonExportWasPng;
    std::string m_comparisonExportMessage;
    bool m_comparisonExportFailed{};
};

}
