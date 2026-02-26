#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/game/IGameState.h"

namespace engine::renderer {
class Renderer;
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
};

} // namespace engine::game
