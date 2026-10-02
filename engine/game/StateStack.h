#pragma once

#include <cstdint>
#include <memory>
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
    double m_labElapsedSeconds{0.0};
};

} // namespace engine::game
