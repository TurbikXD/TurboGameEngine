#include "engine/game/GameplayState.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include "engine/core/Log.h"
#include "engine/ecs/components.h"
#include "engine/game/PauseState.h"
#include "engine/game/StateStack.h"
#include "engine/platform/Input.h"
#include "engine/renderer/RenderAdapter.h"
#include "engine/renderer/Renderer.h"

namespace engine::game {

GameplayState::GameplayState(StateStack& stack) : IGameState(stack) {}

void GameplayState::onEnter() {
    ENGINE_LOG_INFO("Entered GameplayState (Left/Right = move logo cube, Esc = pause/resume)");
    m_world.clear();
    m_sceneInitialized = false;
    m_elapsedSeconds = 0.0;
    m_playerEntity = ecs::kInvalidEntity;
}

void GameplayState::onExit() {
    ENGINE_LOG_INFO("Exited GameplayState");
    m_world.clear();
    m_sceneInitialized = false;
    m_playerEntity = ecs::kInvalidEntity;
}

void GameplayState::handleEvent(const platform::Event& event) {
    if (event.type == platform::EventType::KeyPressed && !event.repeat &&
        event.key == platform::KeyCode::Escape) {
        stack().push(std::make_unique<PauseState>(stack()));
    }
}

void GameplayState::setupScene(renderer::Renderer& rendererInstance) {
    if (m_sceneInitialized) {
        return;
    }

    m_world.clear();

    const glm::vec3 primitivePositions[] = {
        glm::vec3(-2.7F, 1.55F, 7.4F),
        glm::vec3(0.0F, 2.2F, 8.2F),
        glm::vec3(2.7F, 1.55F, 7.0F),
        glm::vec3(0.0F, 0.75F, 6.0F),
    };
    const glm::vec3 primitiveRotations[] = {
        glm::vec3(glm::radians(-62.0F), glm::radians(14.0F), glm::radians(-18.0F)),
        glm::vec3(glm::radians(-78.0F), 0.0F, 0.0F),
        glm::vec3(glm::radians(-60.0F), glm::radians(-16.0F), glm::radians(20.0F)),
        glm::vec3(glm::radians(-48.0F), glm::radians(8.0F), 0.0F),
    };
    const glm::vec3 primitiveScales[] = {
        glm::vec3(1.1F, 1.1F, 1.1F),
        glm::vec3(1.15F, 1.15F, 1.15F),
        glm::vec3(1.08F, 1.08F, 1.08F),
        glm::vec3(1.3F, 1.3F, 1.3F),
    };
    const glm::vec4 primitiveTints[] = {
        glm::vec4(1.0F, 0.55F, 0.35F, 1.0F),
        glm::vec4(0.35F, 0.95F, 0.55F, 1.0F),
        glm::vec4(0.35F, 0.55F, 1.0F, 1.0F),
        glm::vec4(1.0F, 1.0F, 0.35F, 1.0F),
    };

    for (int i = 0; i < 4; ++i) {
        const ecs::EntityId entity = m_world.createEntity();

        ecs::Transform transform{};
        transform.position = primitivePositions[i];
        transform.rotationEulerRadians = primitiveRotations[i];
        transform.scale = primitiveScales[i];

        ecs::MeshRenderer rendererComponent{};
        rendererComponent.primitiveType = ecs::PrimitiveType::Triangle;
        rendererComponent.tint = primitiveTints[i];

        m_world.addComponent<ecs::Transform>(entity, transform);
        m_world.addComponent<ecs::MeshRenderer>(entity, rendererComponent);
        m_world.addComponent<ecs::Tag>(entity, ecs::Tag{std::string("primitive_") + std::to_string(i)});
    }

    renderer::RenderAdapter renderAdapter{rendererInstance};

    constexpr std::array<const char*, 3> kModelMeshIds{
        "assets/models/cube.obj",
        "assets/models/cube.obj",
        "assets/models/pyramid.obj"};
    constexpr std::array<const char*, 3> kModelTextureIds{
        "assets/textures/checker.png",
        "assets/textures/DGLogo.png",
        "assets/textures/checker.png"};
    constexpr const char* kTexturedShaderId = "assets/shaders_hlsl/textured.shader.json";

    auto mesh = renderAdapter.loadMesh(kModelMeshIds[0]);
    auto texture = renderAdapter.loadTexture(kModelTextureIds[0]);
    auto shader = renderAdapter.loadShaderProgram(kTexturedShaderId);

    const auto meshCached = renderAdapter.loadMesh(kModelMeshIds[0]);
    const auto textureCached = renderAdapter.loadTexture(kModelTextureIds[0]);
    const auto shaderCached = renderAdapter.loadShaderProgram(kTexturedShaderId);

    ENGINE_LOG_INFO(
        "Resource share check: mesh={} texture={} shader={}",
        mesh.get() == meshCached.get(),
        texture.get() == textureCached.get(),
        shader.get() == shaderCached.get());

    const glm::vec3 modelPositions[] = {
        glm::vec3(-3.35F, -1.0F, 6.2F),
        glm::vec3(0.0F, -1.35F, 4.7F),
        glm::vec3(3.35F, -1.0F, 6.2F),
    };
    const glm::vec3 modelRotations[] = {
        glm::vec3(glm::radians(-18.0F), glm::radians(24.0F), 0.0F),
        glm::vec3(glm::radians(-12.0F), glm::radians(-20.0F), 0.0F),
        glm::vec3(glm::radians(-12.0F), glm::radians(20.0F), 0.0F),
    };
    const glm::vec3 modelScales[] = {
        glm::vec3(1.45F, 1.45F, 1.45F),
        glm::vec3(1.08F, 1.08F, 1.08F),
        glm::vec3(1.55F, 1.55F, 1.55F),
    };
    const glm::vec4 modelTint[] = {
        glm::vec4(1.0F, 1.0F, 1.0F, 1.0F),
        glm::vec4(0.85F, 0.95F, 1.0F, 1.0F),
        glm::vec4(1.0F, 0.9F, 0.9F, 1.0F),
    };

    for (int i = 0; i < 3; ++i) {
        const ecs::EntityId entity = m_world.createEntity();

        ecs::Transform transform{};
        transform.position = modelPositions[i];
        transform.rotationEulerRadians = modelRotations[i];
        transform.scale = modelScales[i];

        ecs::MeshRenderer rendererComponent{};
        rendererComponent.meshId = kModelMeshIds[static_cast<std::size_t>(i)];
        rendererComponent.textureId = kModelTextureIds[static_cast<std::size_t>(i)];
        rendererComponent.shaderId = kTexturedShaderId;
        rendererComponent.tint = modelTint[i];

        if (i == 0) {
            rendererComponent.mesh = mesh;
            rendererComponent.texture = texture;
            rendererComponent.shader = shader;
        }

        m_world.addComponent<ecs::Transform>(entity, transform);
        m_world.addComponent<ecs::MeshRenderer>(entity, rendererComponent);
        if (i == 1) {
            m_playerEntity = entity;
        }
        m_world.addComponent<ecs::Tag>(entity, ecs::Tag{std::string("model_") + std::to_string(i)});
    }

    m_sceneInitialized = true;
    ENGINE_LOG_INFO("Gameplay ECS scene initialized: primitives=4, models=3");
}

void GameplayState::update(double dt) {
    m_elapsedSeconds += dt;
    const float delta = static_cast<float>(dt);

    if (m_playerEntity != ecs::kInvalidEntity) {
        if (auto* transform = m_world.getComponent<ecs::Transform>(m_playerEntity)) {
            float inputX = 0.0F;
            if (platform::Input::isKeyDown(platform::KeyCode::Left)) {
                inputX -= 1.0F;
            }
            if (platform::Input::isKeyDown(platform::KeyCode::Right)) {
                inputX += 1.0F;
            }

            transform->position.x += inputX * m_moveSpeed * delta;
            transform->position.x = std::clamp(transform->position.x, -1.65F, 1.65F);
            transform->position.y = -1.35F;

            const float settle = std::min(1.0F, delta * 8.0F);
            const float targetPitch = glm::radians(-12.0F);
            const float targetYaw = glm::radians(-20.0F) + inputX * 0.18F;
            const float targetRoll = -inputX * 0.28F;
            transform->rotationEulerRadians.x += (targetPitch - transform->rotationEulerRadians.x) * settle;
            transform->rotationEulerRadians.y += (targetYaw - transform->rotationEulerRadians.y) * settle;
            transform->rotationEulerRadians.z += (targetRoll - transform->rotationEulerRadians.z) * settle;
        }
    }
}

void GameplayState::render(renderer::Renderer& rendererInstance) {
    setupScene(rendererInstance);

    renderer::RenderAdapter renderAdapter{rendererInstance};
    const rhi::Extent2D extent = rendererInstance.frameExtent();
    const float aspectRatio =
        extent.height > 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : (16.0F / 9.0F);
    const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(40.0F), aspectRatio, 0.1F, 50.0F);
    const glm::mat4 view = glm::lookAtRH(
        glm::vec3(0.0F, 0.85F, -5.85F),
        glm::vec3(0.0F, -0.05F, 5.65F),
        glm::vec3(0.0F, 1.0F, 0.0F));
    m_renderSystem.render(m_world, renderAdapter, projection * view);
}

} // namespace engine::game
