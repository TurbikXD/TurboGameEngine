#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <memory>
#include <utility>
#include <vector>

#include "engine/game/IGameState.h"
#include "engine/network/NetworkTestHistory.h"
#include "engine/network/NetworkTestOptions.h"

namespace engine::renderer {
class Renderer;
}

namespace engine::platform {
class Window;
}

namespace engine::core {
class JobSystem;
struct LabOptions;
struct Project;
struct PlayLaunch;
enum class SceneNetworkRole;
struct OnlinePlayStatus;
}

namespace engine::game {

struct OnlinePlayCallbacks {
    std::function<bool(core::SceneNetworkRole, bool, std::string_view, unsigned, std::string&)> start;
    std::function<void(core::SceneNetworkRole)> stop;
    std::function<const core::OnlinePlayStatus&(core::SceneNetworkRole)> status;
    std::function<bool(std::uint64_t, std::string&)> invite;
    std::function<std::filesystem::path(std::uint64_t)> steamAvatar;
};

class StateStack final {
public:
    using LaunchNetworkPlay = std::function<bool(const std::string&, const std::filesystem::path&,
                                                 const net::NetworkTestOptions&, std::string&)>;

    StateStack() = default;

    void push(std::unique_ptr<IGameState> state);
    void pop();
    void replace(std::unique_ptr<IGameState> state);
    void clear();

    void handleEvent(const platform::Event& event);
    void update(double dt);
    void render(renderer::Renderer& renderer);
    void renderUi(renderer::Renderer& renderer);

    void setWindow(platform::Window* window);
    [[nodiscard]] platform::Window* window();
    [[nodiscard]] const platform::Window* window() const;

    void setServices(core::JobSystem* jobs, const core::LabOptions* lab) { m_jobs = jobs; m_lab = lab; }
    [[nodiscard]] core::JobSystem* jobs() const { return m_jobs; }
    [[nodiscard]] const core::LabOptions* labOptions() const { return m_lab; }
    void setProject(const core::Project* project) { m_project = project; }
    [[nodiscard]] const core::Project* project() const { return m_project; }
    void setStandalone(bool standalone) { m_standalone = standalone; }
    [[nodiscard]] bool standalone() const { return m_standalone; }
    void setSceneNetworkLaunch(const core::PlayLaunch* options) { m_sceneNetworkLaunch = options; }
    [[nodiscard]] const core::PlayLaunch* sceneNetworkLaunch() const { return m_sceneNetworkLaunch; }
    void setPlayWindowCallbacks(std::function<bool(const std::string&, const std::filesystem::path&, std::string&)> launch,
                                std::function<void()> stop, std::function<bool()> running) {
        m_launchPlayWindow = std::move(launch); m_stopPlayWindow = std::move(stop); m_playWindowRunning = std::move(running);
    }
    bool launchPlayWindow(const std::string& scene, const std::filesystem::path& assets, std::string& error) {
        if (m_launchPlayWindow) return m_launchPlayWindow(scene, assets, error);
        error = "Standalone Play is unavailable."; return false;
    }
    void stopPlayWindow() { if (m_stopPlayWindow) m_stopPlayWindow(); }
    [[nodiscard]] bool playWindowRunning() const { return m_playWindowRunning && m_playWindowRunning(); }
    void setNetworkPlayCallbacks(LaunchNetworkPlay launch,
                                 std::function<void()> stop, std::function<bool()> running,
                                 std::function<std::string_view()> error) {
        m_launchNetworkPlay = std::move(launch);
        m_stopNetworkPlay = std::move(stop);
        m_networkPlayRunning = std::move(running);
        m_networkPlayError = std::move(error);
    }
    bool launchNetworkPlay(const std::string& scene, const std::filesystem::path& assets,
                           const net::NetworkTestOptions& options, std::string& error) {
        if (m_launchNetworkPlay) return m_launchNetworkPlay(scene, assets, options, error);
        error = "Network Play is unavailable.";
        return false;
    }
    void stopNetworkPlay() { if (m_stopNetworkPlay) m_stopNetworkPlay(); }
    [[nodiscard]] bool networkPlayRunning() const { return m_networkPlayRunning && m_networkPlayRunning(); }
    [[nodiscard]] std::string_view networkPlayError() const { return m_networkPlayError ? m_networkPlayError() : std::string_view{}; }
    void setOnlinePlayCallbacks(OnlinePlayCallbacks callbacks) { m_onlinePlay = std::move(callbacks); }
    [[nodiscard]] const OnlinePlayCallbacks& onlinePlayCallbacks() const { return m_onlinePlay; }
    void setNetworkTestControlCallbacks(std::function<std::vector<net::NetworkTestClientView>()> clients,
                                        std::function<bool(unsigned, net::NetworkTestConditions, std::string&)> setConditions,
                                        std::function<net::NetworkTestHistoryView(unsigned)> history = {},
                                        std::function<bool()> hasHistory = {},
                                        std::function<bool(const std::filesystem::path&, std::string&)> exportCsv = {}) {
        m_networkTestClients = std::move(clients);
        m_setNetworkTestConditions = std::move(setConditions);
        m_networkTestHistory = std::move(history);
        m_hasNetworkTestHistory = std::move(hasHistory);
        m_exportNetworkTestHistory = std::move(exportCsv);
    }
    [[nodiscard]] std::vector<net::NetworkTestClientView> networkTestClients() const {
        return m_networkTestClients ? m_networkTestClients() : std::vector<net::NetworkTestClientView>{};
    }
    [[nodiscard]] net::NetworkTestHistoryView networkTestHistory(unsigned index) const {
        return m_networkTestHistory ? m_networkTestHistory(index) : net::NetworkTestHistoryView{};
    }
    [[nodiscard]] bool hasNetworkTestHistory() const { return m_hasNetworkTestHistory && m_hasNetworkTestHistory(); }
    bool exportNetworkTestHistory(const std::filesystem::path& path, std::string& error) const {
        if (m_exportNetworkTestHistory) return m_exportNetworkTestHistory(path, error);
        error = "Network Test export is unavailable.";
        return false;
    }
    bool setNetworkTestConditions(unsigned index, net::NetworkTestConditions conditions, std::string& error) {
        if (m_setNetworkTestConditions) return m_setNetworkTestConditions(index, conditions, error);
        error = "Live Network Test controls are unavailable.";
        return false;
    }
    void setProjectSelectorCallback(std::function<void()> callback) { m_projectSelectorCallback = std::move(callback); }
    void requestProjectSelector() { if (m_projectSelectorCallback) { m_projectSelectorCallback(); } }
    void setLabElapsedSeconds(double seconds) { m_labElapsedSeconds = seconds; }
    [[nodiscard]] double labElapsedSeconds() const { return m_labElapsedSeconds; }
    void setRenderInterpolationFraction(float fraction) { m_renderInterpolationFraction = fraction; }
    [[nodiscard]] float renderInterpolationFraction(const IGameState& state) const {
        return !m_stack.empty() && m_stack.back().get() == &state ? m_renderInterpolationFraction : 1.0F;
    }

    void applyPendingChanges();
    bool empty() const;

private:
    enum class Action : std::uint8_t { Push = 0, Pop, Replace, Clear };

    struct PendingChange final {
        Action action{Action::Push};
        std::unique_ptr<IGameState> state;
    };

    std::vector<std::unique_ptr<IGameState>> m_stack;
    std::vector<PendingChange> m_pending;
    platform::Window* m_window{nullptr};
    core::JobSystem* m_jobs{nullptr};
    const core::LabOptions* m_lab{nullptr};
    const core::Project* m_project{nullptr};
    const core::PlayLaunch* m_sceneNetworkLaunch{nullptr};
    bool m_standalone{false};
    std::function<bool(const std::string&, const std::filesystem::path&, std::string&)> m_launchPlayWindow;
    std::function<void()> m_stopPlayWindow;
    std::function<bool()> m_playWindowRunning;
    LaunchNetworkPlay m_launchNetworkPlay;
    std::function<void()> m_stopNetworkPlay;
    std::function<bool()> m_networkPlayRunning;
    std::function<std::string_view()> m_networkPlayError;
    OnlinePlayCallbacks m_onlinePlay;
    std::function<std::vector<net::NetworkTestClientView>()> m_networkTestClients;
    std::function<bool(unsigned, net::NetworkTestConditions, std::string&)> m_setNetworkTestConditions;
    std::function<net::NetworkTestHistoryView(unsigned)> m_networkTestHistory;
    std::function<bool()> m_hasNetworkTestHistory;
    std::function<bool(const std::filesystem::path&, std::string&)> m_exportNetworkTestHistory;
    std::function<void()> m_projectSelectorCallback;
    double m_labElapsedSeconds{0.0};
    float m_renderInterpolationFraction{1.0F};
};

}
