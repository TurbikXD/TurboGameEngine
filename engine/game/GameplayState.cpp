#include "engine/game/GameplayState.h"

#include <algorithm>
#include <memory>

#include "engine/core/Log.h"
#include "engine/game/PauseState.h"
#include "engine/game/StateStack.h"
#include "engine/platform/Input.h"
#include "engine/renderer/Renderer.h"

namespace engine::game {

GameplayState::GameplayState(StateStack& stack) : IGameState(stack) {}

void GameplayState::onEnter() {
    ENGINE_LOG_INFO("Entered GameplayState (Esc = pause/resume)");
}

void GameplayState::onExit() {
    ENGINE_LOG_INFO("Exited GameplayState");
}

void GameplayState::handleEvent(const platform::Event& event) {
    if (event.type == platform::EventType::KeyPressed && !event.repeat &&
        event.key == platform::KeyCode::Escape) {
        stack().push(std::make_unique<PauseState>(stack()));
    }
}

void GameplayState::update(double dt) {
    const float delta = static_cast<float>(dt);
    if (platform::Input::isKeyDown(platform::KeyCode::Left)) {
        m_transform.position.x -= m_moveSpeed * delta;
    }
    if (platform::Input::isKeyDown(platform::KeyCode::Right)) {
        m_transform.position.x += m_moveSpeed * delta;
    }
    if (platform::Input::isKeyDown(platform::KeyCode::Up)) {
        m_transform.position.y += m_moveSpeed * delta;
    }
    if (platform::Input::isKeyDown(platform::KeyCode::Down)) {
        m_transform.position.y -= m_moveSpeed * delta;
    }

    if (platform::Input::isMouseButtonDown(platform::MouseButton::Left)) {
        const float scaleDelta = m_scaleSpeed * delta;
        m_transform.scale += glm::vec2(scaleDelta, scaleDelta);
    }

    if (platform::Input::isMouseButtonDown(platform::MouseButton::Right)) {
        m_transform.rotationRadians += m_rotationSpeed * delta;
    }

    m_transform.scale.x = std::clamp(m_transform.scale.x, 0.2F, 3.0F);
    m_transform.scale.y = std::clamp(m_transform.scale.y, 0.2F, 3.0F);
}

void GameplayState::render(renderer::Renderer& rendererInstance) {
    rendererInstance.drawTestPrimitive(m_transform);
}

} // namespace engine::game
