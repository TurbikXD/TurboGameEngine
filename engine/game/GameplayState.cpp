#include "engine/game/GameplayState.h"

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
    ENGINE_LOG_INFO("Entered GameplayState (Esc = pause/resume)");
    m_world.clear();
    m_sceneInitialized = false;
    m_elapsedSeconds = 0.0;
    m_animatedEntity = ecs::kInvalidEntity;
}

void GameplayState::onExit() {
    ENGINE_LOG_INFO("Exited GameplayState");
    m_world.clear();
    m_sceneInitialized = false;
    m_animatedEntity = ecs::kInvalidEntity;
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
        glm::vec3(-1.2F, 0.9F, 0.0F),
        glm::vec3(0.0F, 0.9F, 0.0F),
        glm::vec3(1.2F, 0.9F, 0.0F),
        glm::vec3(0.0F, -0.1F, 0.0F),
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
        transform.scale = glm::vec3(0.5F, 0.5F, 0.5F);
        if (i == 3) {
            transform.scale = glm::vec3(0.7F, 0.7F, 0.7F);
            m_animatedEntity = entity;
        }

        ecs::MeshRenderer rendererComponent{};
        rendererComponent.primitiveType = ecs::PrimitiveType::Triangle;
        rendererComponent.tint = primitiveTints[i];

        m_world.addComponent<ecs::Transform>(entity, transform);
        m_world.addComponent<ecs::MeshRenderer>(entity, rendererComponent);
        m_world.addComponent<ecs::Tag>(entity, ecs::Tag{std::string("primitive_") + std::to_string(i)});
    }

    renderer::RenderAdapter renderAdapter{rendererInstance};

    constexpr std::array<const char*, 3> kModelMeshIds{
        "assets/models/plane.obj",
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
        glm::vec3(-1.0F, -0.9F, 0.0F),
        glm::vec3(1.0F, 0.0F, 0.0F),
        glm::vec3(2.0F, 0.0F, 0.0F),
    };
    const glm::vec4 modelTint[] = {
        glm::vec4(1.0F, 1.0F, 1.0F, 1.0F),
        glm::vec4(0.85F, 0.95F, 1.0F, 1.0F),
        glm::vec4(1.0F, 0.9F, 0.9F, 1.0F),
    };

    ecs::EntityId rootModelEntity = ecs::kInvalidEntity;
    for (int i = 0; i < 3; ++i) {
        const ecs::EntityId entity = m_world.createEntity();

        ecs::Transform transform{};
        transform.position = modelPositions[i];
        transform.scale = glm::vec3(0.9F, 0.9F, 0.9F);

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
        if (i == 0) {
            rootModelEntity = entity;
        } else {
            ecs::Hierarchy hierarchy{};
            hierarchy.parent = rootModelEntity;
            m_world.addComponent<ecs::Hierarchy>(entity, hierarchy);
        }
        m_world.addComponent<ecs::Tag>(entity, ecs::Tag{std::string("model_") + std::to_string(i)});
    }

    m_sceneInitialized = true;
    ENGINE_LOG_INFO("Gameplay ECS scene initialized: primitives=4, models=3");
}

void GameplayState::update(double dt) {
    m_elapsedSeconds += dt;
    const float delta = static_cast<float>(dt);

    if (m_animatedEntity != ecs::kInvalidEntity) {
        if (auto* transform = m_world.getComponent<ecs::Transform>(m_animatedEntity)) {
            if (platform::Input::isKeyDown(platform::KeyCode::Left)) {
                transform->position.x -= m_moveSpeed * delta;
            }
            if (platform::Input::isKeyDown(platform::KeyCode::Right)) {
                transform->position.x += m_moveSpeed * delta;
            }
            if (platform::Input::isKeyDown(platform::KeyCode::Up)) {
                transform->position.y += m_moveSpeed * delta;
            }
            if (platform::Input::isKeyDown(platform::KeyCode::Down)) {
                transform->position.y -= m_moveSpeed * delta;
            }

            transform->rotationEulerRadians.z += m_rotationSpeed * delta;
            transform->position.y = 0.2F + glm::sin(static_cast<float>(m_elapsedSeconds * 2.2)) * 0.35F;
        }
    }

    m_world.forEach<ecs::Transform, ecs::Tag>([&](ecs::EntityId entity, ecs::Transform& transform, ecs::Tag& tag) {
        (void)entity;
        if (tag.value.rfind("model_", 0) == 0) {
            transform.rotationEulerRadians.z -= 0.25F * delta;
        }
    });
}

void GameplayState::render(renderer::Renderer& rendererInstance) {
    setupScene(rendererInstance);

    renderer::RenderAdapter renderAdapter{rendererInstance};
    const glm::mat4 projection = glm::ortho(-2.0F, 2.0F, -1.6F, 1.6F, -5.0F, 5.0F);
    const glm::mat4 view = glm::mat4(1.0F);
    m_renderSystem.render(m_world, renderAdapter, projection * view);
}

} // namespace engine::game
