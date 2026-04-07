#include "engine/game/GameplayState.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "engine/core/Log.h"
#include "engine/ecs/components.h"
#include "engine/ecs/transform_utils.h"
#include "engine/game/PauseState.h"
#include "engine/game/StateStack.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/renderer/RenderAdapter.h"
#include "engine/renderer/Renderer.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace {

constexpr const char* kCubeMeshId = "__diligent_cube__";
constexpr const char* kPyramidMeshId = "__procedural_pyramid__";
constexpr const char* kSphereMeshId = "__diligent_sphere__";
constexpr const char* kShaderId = "assets/shaders_hlsl/textured.shader.json";
constexpr const char* kLogoTextureId = "assets/textures/DGLogo.png";
constexpr const char* kArenaFloorTextureId = "assets/textures/arena_floor.ppm";
constexpr const char* kArenaWallTextureId = "assets/textures/arena_wall.ppm";

const glm::vec3 kCameraStartPosition{0.0F, 7.0F, -18.5F};
const glm::vec3 kCameraStartRotation{glm::radians(-12.0F), 0.0F, 0.0F};

const glm::vec3 kFloorPosition{0.0F, -2.5F, 12.0F};
const glm::vec3 kFloorScale{28.0F, 1.0F, 22.0F};

const glm::vec3 kStrikerStartPosition{-11.4F, -1.0F, 12.0F};
const glm::vec3 kStrikerScale{1.85F, 1.85F, 1.85F};
const glm::vec3 kStrikerLaunchVelocity{22.0F, 0.0F, 0.0F};
const glm::vec3 kShowcaseSphereStartPosition{-10.8F, -1.3F, 18.2F};
const glm::vec3 kShowcaseSphereLaunchVelocity{7.5F, 0.0F, -2.1F};
constexpr std::size_t kSpawnBudgetPerFrame = 6U;

glm::vec4 paletteColor(const std::size_t index) {
    static constexpr std::array<glm::vec4, 8> kPalette{
        glm::vec4(0.82F, 0.42F, 0.32F, 1.0F),
        glm::vec4(0.88F, 0.63F, 0.24F, 1.0F),
        glm::vec4(0.55F, 0.68F, 0.32F, 1.0F),
        glm::vec4(0.23F, 0.63F, 0.61F, 1.0F),
        glm::vec4(0.28F, 0.52F, 0.79F, 1.0F),
        glm::vec4(0.55F, 0.44F, 0.74F, 1.0F),
        glm::vec4(0.80F, 0.46F, 0.62F, 1.0F),
        glm::vec4(0.77F, 0.72F, 0.58F, 1.0F)};
    return kPalette[index % kPalette.size()];
}

glm::vec4 pyramidAccentColor(const std::size_t index) {
    static constexpr std::array<glm::vec4, 5> kPalette{
        glm::vec4(0.90F, 0.50F, 0.24F, 1.0F),
        glm::vec4(0.27F, 0.68F, 0.56F, 1.0F),
        glm::vec4(0.29F, 0.52F, 0.88F, 1.0F),
        glm::vec4(0.80F, 0.34F, 0.54F, 1.0F),
        glm::vec4(0.76F, 0.64F, 0.22F, 1.0F)};
    return kPalette[index % kPalette.size()];
}

} // namespace

namespace engine::game {

GameplayState::GameplayState(StateStack& stack) : IGameState(stack) {}

void GameplayState::onEnter() {
    ENGINE_LOG_INFO("Entered GameplayState (hold RMB + WASDQE for UE-style camera, Space starts/launches demo)");
    m_eventBus.clear();
    setCameraLookActive(false);
    bindCollisionEventHandlers();
    resetDemoScene();
}

void GameplayState::onExit() {
    ENGINE_LOG_INFO("Exited GameplayState");
    setCameraLookActive(false);
    m_pendingSpawnJobs.clear();
    m_eventBus.clear();
    m_physicsSystem.clear();
    m_world.clear();
    m_sceneInitialized = false;
    m_simulationRunning = false;
    m_cameraEntity = ecs::kInvalidEntity;
    m_strikerEntity = ecs::kInvalidEntity;
    m_showcaseSphereEntity = ecs::kInvalidEntity;
    m_showcaseSphereLaunched = false;
}

void GameplayState::handleEvent(const platform::Event& event) {
    if (event.type == platform::EventType::MouseButtonPressed && event.mouseButton == platform::MouseButton::Right) {
        setCameraLookActive(true);
        return;
    }

    if (event.type == platform::EventType::MouseButtonReleased && event.mouseButton == platform::MouseButton::Right) {
        setCameraLookActive(false);
        return;
    }

    if (event.type != platform::EventType::KeyPressed || event.repeat) {
        return;
    }

    if (event.key == platform::KeyCode::Escape) {
        setCameraLookActive(false);
        stack().push(std::make_unique<PauseState>(stack()));
        return;
    }

    if (event.key == platform::KeyCode::Space) {
        if (!m_simulationRunning) {
            startSimulation();
        } else {
            launchStriker();
        }
    }
}

void GameplayState::bindCollisionEventHandlers() {
    m_eventBus.subscribe<ecs::CollisionEnterEvent>([this](const ecs::CollisionEnterEvent& event) {
        ++m_collisionEnterCount;
        m_lastCollisionMessage = "Enter: " + std::to_string(event.contact.entityA) + " <-> " +
                                 std::to_string(event.contact.entityB) + " depth=" +
                                 std::to_string(event.contact.penetrationDepth);
        ENGINE_LOG_INFO(
            "Collision enter: {} <-> {} depth={:.3f}",
            event.contact.entityA,
            event.contact.entityB,
            event.contact.penetrationDepth);
    });
    m_eventBus.subscribe<ecs::CollisionStayEvent>(
        [this](const ecs::CollisionStayEvent&) { ++m_collisionStayCount; });
    m_eventBus.subscribe<ecs::CollisionExitEvent>([this](const ecs::CollisionExitEvent& event) {
        ++m_collisionExitCount;
        m_lastCollisionMessage =
            "Exit: " + std::to_string(event.contact.entityA) + " <-> " + std::to_string(event.contact.entityB);
    });
}

ecs::EntityId GameplayState::spawnStaticBody(
    const std::string& tag,
    const glm::vec3& position,
    const glm::vec3& scale,
    const glm::vec4& tint,
    const std::string& textureId,
    const glm::vec2& uvScale) {
    const ecs::EntityId entity = m_world.createEntity();

    ecs::Transform transform{};
    transform.position = position;
    transform.scale = scale;

    ecs::MeshRenderer meshRenderer{};
    meshRenderer.meshId = kCubeMeshId;
    meshRenderer.shaderId = kShaderId;
    meshRenderer.textureId = textureId;
    meshRenderer.tint = tint;
    meshRenderer.uvScale = uvScale;

    ecs::Rigidbody rigidbody{};
    rigidbody.isStatic = true;
    rigidbody.useGravity = false;
    rigidbody.friction = 0.92F;
    rigidbody.recalculateMassProperties();

    ecs::Collider collider{};
    collider.type = ecs::ColliderType::Aabb;
    collider.aabb.halfExtents = glm::vec3(0.5F);

    m_world.addComponent<ecs::Transform>(entity, transform);
    m_world.addComponent<ecs::MeshRenderer>(entity, meshRenderer);
    m_world.addComponent<ecs::Rigidbody>(entity, rigidbody);
    m_world.addComponent<ecs::Collider>(entity, collider);
    m_world.addComponent<ecs::Tag>(entity, ecs::Tag{tag});
    return entity;
}

ecs::EntityId GameplayState::spawnDynamicBody(
    const std::string& tag,
    const glm::vec3& position,
    const glm::vec3& scale,
    const glm::vec4& tint,
    const float mass,
    const bool pyramidMesh,
    const std::string& textureId,
    const float restitution,
    const float friction,
    const glm::vec2& uvScale) {
    const ecs::EntityId entity = m_world.createEntity();

    ecs::Transform transform{};
    transform.position = position;
    transform.scale = scale;

    ecs::MeshRenderer meshRenderer{};
    meshRenderer.meshId = pyramidMesh ? kPyramidMeshId : kCubeMeshId;
    meshRenderer.shaderId = kShaderId;
    meshRenderer.textureId = textureId;
    meshRenderer.tint = tint;
    meshRenderer.uvScale = uvScale;

    ecs::Rigidbody rigidbody{};
    rigidbody.mass = mass;
    rigidbody.linearDamping = pyramidMesh ? 0.18F : 0.14F;
    rigidbody.angularDamping = pyramidMesh ? 0.78F : 0.62F;
    rigidbody.restitution = restitution;
    rigidbody.friction = friction;
    rigidbody.recalculateMassProperties();

    ecs::Collider collider{};
    collider.type = ecs::ColliderType::Aabb;
    collider.aabb.halfExtents = glm::vec3(0.5F);

    m_world.addComponent<ecs::Transform>(entity, transform);
    m_world.addComponent<ecs::MeshRenderer>(entity, meshRenderer);
    m_world.addComponent<ecs::Rigidbody>(entity, rigidbody);
    m_world.addComponent<ecs::Collider>(entity, collider);
    m_world.addComponent<ecs::Tag>(entity, ecs::Tag{tag});
    return entity;
}

ecs::EntityId GameplayState::spawnDynamicSphere(
    const std::string& tag,
    const glm::vec3& position,
    const float diameter,
    const glm::vec4& tint,
    const float mass,
    const float restitution,
    const float friction) {
    const ecs::EntityId entity = m_world.createEntity();

    ecs::Transform transform{};
    transform.position = position;
    transform.scale = glm::vec3(diameter);

    ecs::MeshRenderer meshRenderer{};
    meshRenderer.meshId = kSphereMeshId;
    meshRenderer.shaderId = kShaderId;
    meshRenderer.tint = tint;

    ecs::Rigidbody rigidbody{};
    rigidbody.mass = mass;
    rigidbody.linearDamping = 0.03F;
    rigidbody.angularDamping = 0.08F;
    rigidbody.restitution = restitution;
    rigidbody.friction = friction;
    rigidbody.recalculateMassProperties();

    ecs::Collider collider{};
    collider.type = ecs::ColliderType::Sphere;
    collider.sphere.radius = 0.5F;

    m_world.addComponent<ecs::Transform>(entity, transform);
    m_world.addComponent<ecs::MeshRenderer>(entity, meshRenderer);
    m_world.addComponent<ecs::Rigidbody>(entity, rigidbody);
    m_world.addComponent<ecs::Collider>(entity, collider);
    m_world.addComponent<ecs::Tag>(entity, ecs::Tag{tag});
    return entity;
}

void GameplayState::createDemoScene() {
    if (m_sceneInitialized) {
        return;
    }

    m_cameraEntity = m_world.createEntity();
    ecs::Transform cameraTransform{};
    cameraTransform.position = kCameraStartPosition;
    cameraTransform.rotationEulerRadians = kCameraStartRotation;
    ecs::Camera camera{};
    camera.verticalFovRadians = glm::radians(52.0F);
    camera.nearPlane = 0.1F;
    camera.farPlane = 220.0F;
    m_world.addComponent<ecs::Transform>(m_cameraEntity, cameraTransform);
    m_world.addComponent<ecs::Camera>(m_cameraEntity, camera);
    m_world.addComponent<ecs::Tag>(m_cameraEntity, ecs::Tag{"free_camera"});

    spawnStaticBody(
        "floor",
        kFloorPosition,
        kFloorScale,
        glm::vec4(0.96F, 0.98F, 1.0F, 1.0F),
        kArenaFloorTextureId,
        glm::vec2(8.0F, 6.0F));
    spawnStaticBody(
        "wall_left",
        glm::vec3(-14.3F, -1.1F, 12.0F),
        glm::vec3(0.7F, 4.0F, 22.0F),
        glm::vec4(0.60F, 0.64F, 0.72F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(6.0F, 2.0F));
    spawnStaticBody(
        "wall_right",
        glm::vec3(14.3F, -1.1F, 12.0F),
        glm::vec3(0.7F, 4.0F, 22.0F),
        glm::vec4(0.60F, 0.64F, 0.72F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(6.0F, 2.0F));
    spawnStaticBody(
        "wall_back",
        glm::vec3(0.0F, -1.1F, 22.2F),
        glm::vec3(28.0F, 4.0F, 0.7F),
        glm::vec4(0.58F, 0.62F, 0.70F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(7.0F, 2.0F));
    spawnStaticBody(
        "wall_front",
        glm::vec3(0.0F, -1.1F, 1.8F),
        glm::vec3(28.0F, 4.0F, 0.7F),
        glm::vec4(0.58F, 0.62F, 0.70F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(7.0F, 2.0F));
    spawnStaticBody(
        "pillar_left",
        glm::vec3(-6.0F, 0.4F, 12.0F),
        glm::vec3(1.6F, 5.2F, 1.6F),
        glm::vec4(0.68F, 0.72F, 0.79F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(2.0F, 5.0F));
    spawnStaticBody(
        "pillar_right",
        glm::vec3(6.0F, 0.4F, 12.0F),
        glm::vec3(1.6F, 5.2F, 1.6F),
        glm::vec4(0.68F, 0.72F, 0.79F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(2.0F, 5.0F));

    for (int layer = 0; layer < 4; ++layer) {
        const float y = -1.35F + static_cast<float>(layer) * 1.22F;
        spawnDynamicBody(
            "gate_left_" + std::to_string(layer),
            glm::vec3(-2.9F, y, 12.0F),
            glm::vec3(1.15F, 1.15F, 1.15F),
            paletteColor(static_cast<std::size_t>(layer)),
            1.1F);
        spawnDynamicBody(
            "gate_right_" + std::to_string(layer),
            glm::vec3(2.9F, y, 12.0F),
            glm::vec3(1.15F, 1.15F, 1.15F),
            paletteColor(static_cast<std::size_t>(layer + 4)),
            1.1F);
    }

    spawnDynamicBody("gate_bridge_left", glm::vec3(-1.2F, 2.45F, 12.0F), glm::vec3(2.2F, 0.55F, 1.2F), paletteColor(6), 1.35F);
    spawnDynamicBody("gate_bridge_right", glm::vec3(1.2F, 2.45F, 12.0F), glm::vec3(2.2F, 0.55F, 1.2F), paletteColor(7), 1.35F);
    spawnDynamicBody(
        "gate_cap",
        glm::vec3(0.0F, 3.45F, 12.0F),
        glm::vec3(1.35F, 1.35F, 1.35F),
        glm::vec4(0.98F, 0.98F, 0.98F, 1.0F),
        0.95F,
        true,
        kLogoTextureId,
        0.16F,
        0.62F,
        glm::vec2(1.0F, 1.0F));

    for (int i = 0; i < 8; ++i) {
        const float z = 6.0F + static_cast<float>(i) * 1.45F;
        spawnDynamicBody(
            "domino_" + std::to_string(i),
            glm::vec3(-8.4F, -0.95F, z),
            glm::vec3(0.38F, 1.95F, 0.95F),
            paletteColor(static_cast<std::size_t>(i)),
            0.5F,
            false,
            "",
            0.05F,
            0.9F);
    }

    for (int row = 0; row < 3; ++row) {
        const int columns = 3 - row;
        for (int column = 0; column < columns; ++column) {
            const float x = -0.95F * static_cast<float>(columns - 1) + static_cast<float>(column) * 1.9F;
            const float y = -1.35F + static_cast<float>(row) * 1.02F;
            spawnDynamicBody(
                "rear_stack_" + std::to_string(row) + "_" + std::to_string(column),
                glm::vec3(x, y, 17.0F),
                glm::vec3(1.0F, 1.0F, 1.0F),
                paletteColor(static_cast<std::size_t>(row * 3 + column)),
                0.82F + static_cast<float>(row) * 0.08F);
        }
    }

    spawnDynamicBody(
        "front_pyramid_left",
        glm::vec3(-2.4F, -1.25F, 8.0F),
        glm::vec3(1.2F, 1.2F, 1.2F),
        pyramidAccentColor(0),
        0.78F,
        true,
        "",
        0.1F,
        0.58F,
        glm::vec2(1.5F, 1.5F));
    spawnDynamicBody(
        "front_pyramid_center",
        glm::vec3(0.0F, -1.25F, 8.8F),
        glm::vec3(1.05F, 1.05F, 1.05F),
        pyramidAccentColor(1),
        0.74F,
        true,
        "",
        0.1F,
        0.58F,
        glm::vec2(1.4F, 1.4F));
    spawnDynamicBody(
        "front_pyramid_right",
        glm::vec3(2.4F, -1.25F, 8.0F),
        glm::vec3(1.2F, 1.2F, 1.2F),
        pyramidAccentColor(2),
        0.78F,
        true,
        "",
        0.1F,
        0.58F,
        glm::vec2(1.5F, 1.5F));

    spawnDynamicBody("impact_box_left", glm::vec3(-4.5F, -1.35F, 11.0F), glm::vec3(1.0F, 1.0F, 1.0F), paletteColor(2), 0.9F);
    spawnDynamicBody("impact_box_right", glm::vec3(4.5F, -1.35F, 13.0F), glm::vec3(1.0F, 1.0F, 1.0F), paletteColor(5), 0.9F);
    m_showcaseSphereEntity = spawnDynamicSphere(
        "showcase_sphere",
        kShowcaseSphereStartPosition,
        1.35F,
        glm::vec4(0.92F, 0.38F, 0.16F, 1.0F),
        2.4F,
        0.03F,
        0.95F);

    m_strikerEntity = spawnDynamicBody(
        "striker",
        kStrikerStartPosition,
        kStrikerScale,
        glm::vec4(1.0F, 1.0F, 1.0F, 1.0F),
        8.0F,
        false,
        kLogoTextureId,
        0.08F,
        0.42F,
        glm::vec2(1.0F, 1.0F));

    m_sceneInitialized = true;
    m_lastCollisionMessage = "Scene ready. Start physics from UI or press Space.";
    ENGINE_LOG_INFO("Gameplay crash showcase scene initialized in paused state");
}

void GameplayState::resetDemoScene() {
    m_world.clear();
    m_physicsSystem.clear();
    m_cameraEntity = ecs::kInvalidEntity;
    m_strikerEntity = ecs::kInvalidEntity;
    m_sceneInitialized = false;
    m_simulationRunning = false;
    m_pendingSpawnJobs.clear();
    m_spawnSequence = 0;
    m_collisionEnterCount = 0;
    m_collisionStayCount = 0;
    m_collisionExitCount = 0;
    m_lastCollisionMessage = "Scene reset.";
    m_showcaseSphereLaunched = false;
    setCameraLookActive(false);
    createDemoScene();
}

void GameplayState::startSimulation(const bool relaunchStriker) {
    if (!m_sceneInitialized) {
        createDemoScene();
    }

    m_simulationRunning = true;
    m_lastCollisionMessage = "Simulation running.";

    if (!m_showcaseSphereLaunched) {
        if (auto* sphereBody = m_world.getComponent<ecs::Rigidbody>(m_showcaseSphereEntity)) {
            sphereBody->velocity = kShowcaseSphereLaunchVelocity;
            sphereBody->angularVelocity = glm::vec3(0.0F);
            sphereBody->acceleration = glm::vec3(0.0F);
            sphereBody->accumulatedForce = glm::vec3(0.0F);
            sphereBody->accumulatedTorque = glm::vec3(0.0F);
            sphereBody->wakeUp();
            m_showcaseSphereLaunched = true;
        }
    }

    if (relaunchStriker) {
        launchStriker();
    }
}

void GameplayState::pauseSimulation() {
    m_simulationRunning = false;
    m_lastCollisionMessage = "Simulation paused.";
}

void GameplayState::launchStriker() {
    if (!m_sceneInitialized) {
        createDemoScene();
    }

    auto* strikerTransform = m_world.getComponent<ecs::Transform>(m_strikerEntity);
    auto* strikerBody = m_world.getComponent<ecs::Rigidbody>(m_strikerEntity);
    if (strikerTransform == nullptr || strikerBody == nullptr) {
        return;
    }

    strikerTransform->position = kStrikerStartPosition;
    strikerBody->velocity = kStrikerLaunchVelocity;
    strikerBody->acceleration = glm::vec3(0.0F);
    strikerBody->accumulatedForce = glm::vec3(0.0F);
    strikerBody->angularVelocity = glm::vec3(0.0F);
    strikerBody->accumulatedTorque = glm::vec3(0.0F);
    strikerBody->useGravity = true;
    strikerBody->wakeUp();
    m_simulationRunning = true;
    m_lastCollisionMessage = "Striker launched into the structure.";
}

void GameplayState::spawnDropWave() {
    if (!m_sceneInitialized) {
        createDemoScene();
    }

    const std::uint32_t waveIndex = m_spawnSequence++;
    const float baseHeight = 7.2F + static_cast<float>(waveIndex % 3U) * 0.8F;
    const float zBase = 7.0F + static_cast<float>(waveIndex % 3U) * 1.1F;

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 6; ++column) {
            const int objectIndex = row * 6 + column;
            const float x = -5.5F + static_cast<float>(column) * 2.2F;
            const float y = baseHeight + static_cast<float>(row) * 1.25F + static_cast<float>(column % 2) * 0.25F;
            const float z = zBase + static_cast<float>(row) * 3.15F;
            const glm::vec3 scale(0.85F + 0.12F * static_cast<float>((objectIndex + row) % 3), 0.9F, 0.9F);
            const bool pyramidMesh = ((waveIndex + static_cast<std::uint32_t>(objectIndex)) % 4U) == 0U;
            const std::string tag = "wave_" + std::to_string(waveIndex) + "_" + std::to_string(objectIndex);
            const glm::vec3 position(x, y, z);
            const glm::vec4 tint =
                pyramidMesh ? pyramidAccentColor(static_cast<std::size_t>(waveIndex + static_cast<std::uint32_t>(objectIndex)))
                            : paletteColor(static_cast<std::size_t>(waveIndex + static_cast<std::uint32_t>(objectIndex)));
            const float mass = 0.7F + static_cast<float>(objectIndex % 5) * 0.12F;
            const bool sphereMesh = ((waveIndex + static_cast<std::uint32_t>(objectIndex)) % 7U) == 3U;

            m_pendingSpawnJobs.push_back([=, this]() {
                if (sphereMesh) {
                    spawnDynamicSphere(tag, position, scale.x * 1.2F, tint, mass, 0.03F, 0.9F);
                    return;
                }

                spawnDynamicBody(
                    tag,
                    position,
                    scale,
                    tint,
                    mass,
                    pyramidMesh,
                    "",
                    0.08F,
                    0.72F,
                    pyramidMesh ? glm::vec2(1.35F, 1.35F) : glm::vec2(1.0F, 1.0F));
            });
        }
    }

    m_simulationRunning = true;
    m_lastCollisionMessage = "Cascade queued above the arena.";
}

void GameplayState::processPendingSpawns() {
    std::size_t spawnedThisFrame = 0;
    while (!m_pendingSpawnJobs.empty() && spawnedThisFrame < kSpawnBudgetPerFrame) {
        std::function<void()> spawnJob = std::move(m_pendingSpawnJobs.front());
        m_pendingSpawnJobs.pop_front();
        if (spawnJob) {
            spawnJob();
        }
        ++spawnedThisFrame;
    }
}

void GameplayState::setCameraLookActive(const bool active) {
    if (m_cameraLookActive == active) {
        return;
    }

    m_cameraLookActive = active;
    if (active) {
        m_skipNextCameraLookDelta = true;
    } else {
        m_skipNextCameraLookDelta = false;
    }

    if (platform::Window* window = stack().window()) {
        window->setCursorMode(active ? platform::CursorMode::Disabled : platform::CursorMode::Normal);
    }
}

void GameplayState::update(double dt) {
    if (!m_sceneInitialized) {
        createDemoScene();
    }

    if (platform::InputManager::wasKeyPressed(platform::KeyCode::F1)) {
        m_showColliderDebug = !m_showColliderDebug;
    }

    const auto [scrollX, scrollY] = platform::InputManager::scrollDelta();
    (void)scrollX;
    if (scrollY != 0.0) {
        auto cameraSettings = m_cameraController.settings();
        cameraSettings.moveSpeed =
            std::clamp(cameraSettings.moveSpeed + static_cast<float>(scrollY), 1.0F, 30.0F);
        m_cameraController.setSettings(cameraSettings);
    }

    const bool rightMouseDown = platform::InputManager::isMouseButtonDownRaw(platform::MouseButton::Right);
    if (rightMouseDown != m_cameraLookActive) {
        setCameraLookActive(rightMouseDown);
    }

    if (auto* cameraTransform = m_world.getComponent<ecs::Transform>(m_cameraEntity)) {
        const bool allowMouseLook = m_cameraLookActive && !m_skipNextCameraLookDelta;
        m_cameraController.update(*cameraTransform, dt, m_cameraLookActive, allowMouseLook);
        m_skipNextCameraLookDelta = false;
    }

    processPendingSpawns();

    if (m_simulationRunning) {
        m_physicsSystem.update(m_world, dt, m_eventBus);
    }
}

glm::mat4 GameplayState::buildActiveCameraViewProjection(const renderer::Renderer& renderer) {
    const rhi::Extent2D extent = renderer.frameExtent();
    const float aspectRatio =
        extent.height > 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : (16.0F / 9.0F);

    glm::mat4 viewProjection(1.0F);
    bool foundCamera = false;
    m_world.forEach<ecs::Transform, ecs::Camera>([&](ecs::EntityId entity, ecs::Transform& transform, ecs::Camera& camera) {
        (void)entity;
        if (foundCamera || !camera.active) {
            return;
        }

        const glm::vec3 forward = ecs::forwardFromEuler(transform.rotationEulerRadians);
        const glm::vec3 up = ecs::upFromEuler(transform.rotationEulerRadians);
        const glm::mat4 projection =
            glm::perspectiveRH_ZO(camera.verticalFovRadians, aspectRatio, camera.nearPlane, camera.farPlane);
        const glm::mat4 view = glm::lookAtRH(transform.position, transform.position + forward, up);
        viewProjection = projection * view;
        foundCamera = true;
    });

    if (!foundCamera) {
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(52.0F), aspectRatio, 0.1F, 220.0F);
        const glm::mat4 view = glm::lookAtRH(
            kCameraStartPosition,
            glm::vec3(0.0F, 1.5F, 12.0F),
            glm::vec3(0.0F, 1.0F, 0.0F));
        return projection * view;
    }

    return viewProjection;
}

void GameplayState::render(renderer::Renderer& rendererInstance) {
    const glm::mat4 viewProjection = buildActiveCameraViewProjection(rendererInstance);
    renderer::RenderAdapter renderAdapter{rendererInstance};
    m_renderSystem.render(m_world, renderAdapter, viewProjection);
    if (m_showColliderDebug) {
        m_debugRenderSystem.render(m_world, renderAdapter, viewProjection);
    }
}

void GameplayState::renderUi(renderer::Renderer& rendererInstance) {
    if (!rendererInstance.isImGuiEnabled()) {
        return;
    }

    auto cameraSettings = m_cameraController.settings();
    auto physicsSettings = m_physicsSystem.settings();
    int solverIterations = static_cast<int>(physicsSettings.solverIterations);

    ImGui::SetNextWindowPos(ImVec2(24.0F, 340.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(390.0F, 0.0F), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Gameplay Debug")) {
        ImGui::TextUnformatted("Interactive crash sandbox");
        ImGui::TextUnformatted("Hold RMB + WASDQE for UE-style fly camera");
        ImGui::TextUnformatted("Mouse wheel changes speed, Shift boosts movement, Space launches striker");
        ImGui::TextUnformatted("F1 toggles collider debug. Scene stays posed until physics starts.");
        ImGui::Separator();

        if (!m_simulationRunning) {
            if (ImGui::Button("Start Physics")) {
                startSimulation();
            }
        } else if (ImGui::Button("Pause Physics")) {
            pauseSimulation();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Scene")) {
            resetDemoScene();
        }
        if (ImGui::Button("Launch Striker")) {
            launchStriker();
        }
        ImGui::SameLine();
        if (ImGui::Button("Drop Wave")) {
            spawnDropWave();
        }

        ImGui::Text("Simulation: %s", m_simulationRunning ? "Running" : "Paused");
        ImGui::Text("Bodies / sleeping / broadphase pairs: %zu / %zu / %zu",
            m_physicsSystem.bodyCount(),
            m_physicsSystem.sleepingBodyCount(),
            m_physicsSystem.broadphasePairCount());
        ImGui::Checkbox("Draw collider bounds", &m_showColliderDebug);
        if (ImGui::SliderFloat("Move speed", &cameraSettings.moveSpeed, 1.0F, 30.0F, "%.1f")) {
            m_cameraController.setSettings(cameraSettings);
        }
        if (ImGui::SliderFloat("Mouse sensitivity", &cameraSettings.mouseSensitivity, 0.0005F, 0.01F, "%.4f")) {
            m_cameraController.setSettings(cameraSettings);
        }
        if (ImGui::SliderFloat("Sprint multiplier", &cameraSettings.sprintMultiplier, 1.0F, 6.0F, "%.1f")) {
            m_cameraController.setSettings(cameraSettings);
        }

        if (ImGui::DragFloat3("Gravity", &physicsSettings.gravity.x, 0.05F, -40.0F, 40.0F, "%.2f")) {
            m_physicsSystem.setSettings(physicsSettings);
        }
        if (ImGui::SliderInt("Solver iterations", &solverIterations, 1, 8)) {
            physicsSettings.solverIterations = static_cast<std::size_t>(solverIterations);
            m_physicsSystem.setSettings(physicsSettings);
        }
        if (ImGui::SliderFloat("Broadphase cell", &physicsSettings.broadphaseCellSize, 1.2F, 6.0F, "%.1f")) {
            m_physicsSystem.setSettings(physicsSettings);
        }

        ImGui::Separator();
        ImGui::Text("Active contacts: %zu", m_physicsSystem.activeCollisionCount());
        ImGui::Text("Collision enter/stay/exit: %u / %u / %u", m_collisionEnterCount, m_collisionStayCount, m_collisionExitCount);
        ImGui::TextWrapped("%s", m_lastCollisionMessage.c_str());
    }
    ImGui::End();
}

} // namespace engine::game
