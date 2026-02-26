#include "engine/game/MenuState.h"

#include <memory>

#include "engine/core/Log.h"
#include "engine/game/GameplayState.h"
#include "engine/game/StateStack.h"
#include "engine/renderer/Renderer.h"

namespace engine::game {

MenuState::MenuState(StateStack& stack) : IGameState(stack) {}

void MenuState::onEnter() {
    ENGINE_LOG_INFO("Entered MenuState (Press Enter to start)");
}

void MenuState::onExit() {
    ENGINE_LOG_INFO("Exited MenuState");
}

void MenuState::handleEvent(const platform::Event& event) {
    if (event.type == platform::EventType::KeyPressed && !event.repeat &&
        event.key == platform::KeyCode::Enter) {
        stack().replace(std::make_unique<GameplayState>(stack()));
    }
}

void MenuState::update(double dt) {
    m_transform.rotationRadians += static_cast<float>(dt * 0.75);
}

void MenuState::render(renderer::Renderer& rendererInstance) {
    rendererInstance.drawTestPrimitive(m_transform);
}

} // namespace engine::game
