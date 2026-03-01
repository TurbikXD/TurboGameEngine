#pragma once

#include <string>

#include "engine/game/IGameState.h"
#include "engine/renderer/Transform2D.h"

namespace engine::game {

class MenuState final : public IGameState {
public:
    explicit MenuState(StateStack& stack, std::string sampleMode);

    void onEnter() override;
    void onExit() override;
    void handleEvent(const platform::Event& event) override;
    void update(double dt) override;
    void render(renderer::Renderer& renderer) override;

private:
    renderer::Transform2D m_transform{};
    std::string m_sampleMode;
};

} // namespace engine::game
