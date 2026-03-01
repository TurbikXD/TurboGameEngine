#pragma once

#include "engine/ecs/render_system.h"
#include "engine/ecs/world.h"
#include "engine/game/IGameState.h"

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
    void setupScene(renderer::Renderer& renderer);

    ecs::World m_world{};
    ecs::RenderSystem m_renderSystem{};
    ecs::EntityId m_animatedEntity{ecs::kInvalidEntity};
    double m_elapsedSeconds{0.0};
    bool m_sceneInitialized{false};
    float m_moveSpeed{1.5F};
    float m_rotationSpeed{1.3F};
};

} // namespace engine::game
