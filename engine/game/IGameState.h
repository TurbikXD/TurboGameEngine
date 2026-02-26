#pragma once

#include "engine/platform/Events.h"

namespace engine::renderer {
class Renderer;
}

namespace engine::game {

class StateStack;

class IGameState {
public:
    explicit IGameState(StateStack& stack);
    virtual ~IGameState() = default;

    virtual void onEnter() {}
    virtual void onExit() {}
    virtual void handleEvent(const platform::Event& event) = 0;
    virtual void update(double dt) = 0;
    virtual void render(renderer::Renderer& renderer) = 0;

protected:
    StateStack& stack();
    const StateStack& stack() const;

private:
    StateStack& m_stateStack;
};

inline IGameState::IGameState(StateStack& stackRef) : m_stateStack(stackRef) {}

inline StateStack& IGameState::stack() {
    return m_stateStack;
}

inline const StateStack& IGameState::stack() const {
    return m_stateStack;
}

} // namespace engine::game
