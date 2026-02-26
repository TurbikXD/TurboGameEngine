#pragma once

#include "engine/game/IGameState.h"
#include "engine/renderer/Transform2D.h"

namespace engine::game {

class GameplayState final : public IGameState {
public:
    explicit GameplayState(StateStack& stack);

    void onEnter() override;
    void onExit() override;
    void handleEvent(const platform::Event& event) override;
    void update(double dt) override;
    void render(renderer::Renderer& renderer) override;

private:
    renderer::Transform2D m_transform{};
    float m_moveSpeed{0.8F};
    float m_rotationSpeed{1.8F};
    float m_scaleSpeed{0.7F};
};

} // namespace engine::game
