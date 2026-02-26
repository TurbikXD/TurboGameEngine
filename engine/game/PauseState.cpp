#include "engine/game/PauseState.h"

#include "engine/core/Log.h"
#include "engine/game/StateStack.h"

namespace engine::game {

PauseState::PauseState(StateStack& stack) : IGameState(stack) {}

void PauseState::onEnter() {
    ENGINE_LOG_INFO("Entered PauseState");
}

void PauseState::onExit() {
    ENGINE_LOG_INFO("Exited PauseState");
}

void PauseState::handleEvent(const platform::Event& event) {
    if (event.type == platform::EventType::KeyPressed && !event.repeat &&
        event.key == platform::KeyCode::Escape) {
        stack().pop();
    }
}

void PauseState::update(double dt) {
    (void)dt;
}

void PauseState::render(renderer::Renderer& renderer) {
    (void)renderer;
}

} // namespace engine::game
