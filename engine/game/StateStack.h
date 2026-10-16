#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <string>
#include <memory>
#include <utility>
#include <vector>

#include "engine/game/IGameState.h"

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
}

namespace engine::game {

class StateStack final {
public:
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
    void setProjectSelectorCallback(std::function<void()> callback) { m_projectSelectorCallback = std::move(callback); }
    void requestProjectSelector() { if (m_projectSelectorCallback) { m_projectSelectorCallback(); } }
    void setLabElapsedSeconds(double seconds) { m_labElapsedSeconds = seconds; }
    [[nodiscard]] double labElapsedSeconds() const { return m_labElapsedSeconds; }

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
    bool m_standalone{false};
    std::function<bool(const std::string&, const std::filesystem::path&, std::string&)> m_launchPlayWindow;
    std::function<void()> m_stopPlayWindow;
    std::function<bool()> m_playWindowRunning;
    std::function<void()> m_projectSelectorCallback;
    double m_labElapsedSeconds{0.0};
};

} // namespace engine::game
