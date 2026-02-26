#pragma once

#include "engine/game/IGameState.h"

namespace engine::game {

class PauseState final : public IGameState {
public:
    explicit PauseState(StateStack& stack);

    void onEnter() override;
    void onExit() override;
    void handleEvent(const platform::Event& event) override;
    void update(double dt) override;
    void render(renderer::Renderer& renderer) override;
};

} // namespace engine::game
