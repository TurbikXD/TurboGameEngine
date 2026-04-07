#pragma once

#include <deque>
#include <functional>
#include <string>

#include "engine/core/EventBus.h"
#include "engine/ecs/debug_render_system.h"
#include "engine/ecs/physics_system.h"
#include "engine/ecs/render_system.h"
#include "engine/ecs/world.h"
#include "engine/game/FreeCameraController.h"
#include "engine/game/IGameState.h"

namespace engine::game {

class GameplayState final : public IGameState, public IGameStateUi {
public:
    explicit GameplayState(StateStack& stack);

    void onEnter() override;
    void onExit() override;
    void handleEvent(const platform::Event& event) override;
    void update(double dt) override;
    void render(renderer::Renderer& renderer) override;
    void renderUi(renderer::Renderer& renderer) override;

private:
    void createDemoScene();
    void resetDemoScene();
    void startSimulation(bool relaunchStriker = false);
    void pauseSimulation();
    void launchStriker();
    void spawnDropWave();
    void processPendingSpawns();
    void setCameraLookActive(bool active);
    ecs::EntityId spawnStaticBody(
        const std::string& tag,
        const glm::vec3& position,
        const glm::vec3& scale,
        const glm::vec4& tint,
        const std::string& textureId = "",
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F));
    ecs::EntityId spawnDynamicBody(
        const std::string& tag,
        const glm::vec3& position,
        const glm::vec3& scale,
        const glm::vec4& tint,
        float mass,
        bool pyramidMesh = false,
        const std::string& textureId = "",
        float restitution = 0.06F,
        float friction = 0.82F,
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F));
    ecs::EntityId spawnDynamicSphere(
        const std::string& tag,
        const glm::vec3& position,
        float diameter,
        const glm::vec4& tint,
        float mass,
        float restitution = 0.04F,
        float friction = 0.92F);
    [[nodiscard]] glm::mat4 buildActiveCameraViewProjection(const renderer::Renderer& renderer);
    void bindCollisionEventHandlers();

    ecs::World m_world{};
    ecs::RenderSystem m_renderSystem{};
    ecs::DebugRenderSystem m_debugRenderSystem{};
    ecs::PhysicsSystem m_physicsSystem{};
    core::EventBus m_eventBus{};
    FreeCameraController m_cameraController{};
    std::deque<std::function<void()>> m_pendingSpawnJobs{};
    ecs::EntityId m_cameraEntity{ecs::kInvalidEntity};
    ecs::EntityId m_strikerEntity{ecs::kInvalidEntity};
    ecs::EntityId m_showcaseSphereEntity{ecs::kInvalidEntity};
    bool m_sceneInitialized{false};
    bool m_simulationRunning{false};
    bool m_showColliderDebug{false};
    bool m_cameraLookActive{false};
    bool m_skipNextCameraLookDelta{false};
    bool m_showcaseSphereLaunched{false};
    std::uint32_t m_spawnSequence{0};
    std::uint32_t m_collisionEnterCount{0};
    std::uint32_t m_collisionStayCount{0};
    std::uint32_t m_collisionExitCount{0};
    std::string m_lastCollisionMessage{"No collisions yet."};
};

} // namespace engine::game
