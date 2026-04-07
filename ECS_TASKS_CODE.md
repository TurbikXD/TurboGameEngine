# ECS: релевантный код по задачам 1-7

Ниже собран код из проекта, который закрывает все пункты задания:

1. базовые структуры `Entity`, `Component`, `System`;
2. `World/Registry` для управления сущностями и компонентами;
3. компоненты `Transform`, `Tag`, `MeshRenderer`;
4. система рендера через `RenderAdapter`;
5. интеграция ECS в игровой цикл;
6. отрисовка нескольких объектов с разными трансформациями;
7. иерархия объектов и преобразование локальных координат в мировые.

---

## 1) Entity + базовые ECS-структуры

### `engine/ecs/entity.h`

```cpp
#pragma once

#include <cstdint>

namespace engine::ecs {

using EntityId = std::uint32_t;

constexpr EntityId kInvalidEntity = 0;

} // namespace engine::ecs
```

### `engine/ecs/components.h`

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/ecs/entity.h"

namespace engine::resources {
class Mesh;
class ShaderProgram;
class Texture;
} // namespace engine::resources

namespace engine::ecs {

struct Transform final {
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::vec3 rotationEulerRadians{0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F, 1.0F, 1.0F};

    [[nodiscard]] glm::mat4 toMatrix() const {
        glm::mat4 matrix(1.0F);
        matrix = glm::translate(matrix, position);
        matrix = glm::rotate(matrix, rotationEulerRadians.x, glm::vec3(1.0F, 0.0F, 0.0F));
        matrix = glm::rotate(matrix, rotationEulerRadians.y, glm::vec3(0.0F, 1.0F, 0.0F));
        matrix = glm::rotate(matrix, rotationEulerRadians.z, glm::vec3(0.0F, 0.0F, 1.0F));
        matrix = glm::scale(matrix, scale);
        return matrix;
    }
};

struct Tag final {
    std::string value;
};

struct Hierarchy final {
    EntityId parent{kInvalidEntity};
};

enum class PrimitiveType : std::uint8_t {
    Triangle = 0
};

struct MeshRenderer final {
    PrimitiveType primitiveType{PrimitiveType::Triangle};
    glm::vec4 tint{1.0F, 1.0F, 1.0F, 1.0F};
    std::string meshId;
    std::string textureId;
    std::string shaderId;
    std::shared_ptr<resources::Mesh> mesh;
    std::shared_ptr<resources::Texture> texture;
    std::shared_ptr<resources::ShaderProgram> shader;
    bool visible{true};

    [[nodiscard]] bool hasResourceIds() const {
        return !meshId.empty() && !textureId.empty() && !shaderId.empty();
    }

    [[nodiscard]] bool hasGpuResources() const {
        return mesh != nullptr && texture != nullptr && shader != nullptr;
    }
};

} // namespace engine::ecs
```

### `engine/ecs/render_system.h` (System)

```cpp
#pragma once

#include <glm/glm.hpp>

namespace engine::renderer {
class RenderAdapter;
}

namespace engine::ecs {

class World;

class RenderSystem final {
public:
    void render(World& world, renderer::RenderAdapter& renderer, const glm::mat4& viewProjectionMatrix);
};

} // namespace engine::ecs
```

---

## 2) World/Registry

### `engine/ecs/world.h`

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/ecs/entity.h"

namespace engine::ecs {

class World final {
public:
    World() = default;
    ~World() = default;

    EntityId createEntity();
    void destroyEntity(EntityId entity);
    void clear();

    [[nodiscard]] bool isAlive(EntityId entity) const;

    template <class T, class... Args>
    T& addComponent(EntityId entity, Args&&... args);

    template <class T>
    [[nodiscard]] bool hasComponent(EntityId entity) const;

    template <class T>
    T* getComponent(EntityId entity);

    template <class T>
    const T* getComponent(EntityId entity) const;

    template <class... Components, class Func>
    void forEach(Func&& func);

private:
    struct EntityRecord final {
        std::uint32_t generation{1};
        bool alive{false};
    };

    struct IComponentStorage {
        virtual ~IComponentStorage() = default;
        virtual void remove(EntityId entity) = 0;
        virtual void clear() = 0;
    };

    template <class T>
    struct ComponentStorage final : IComponentStorage {
        std::unordered_map<EntityId, T> components;

        void remove(EntityId entity) override {
            components.erase(entity);
        }

        void clear() override {
            components.clear();
        }
    };

    template <class T>
    ComponentStorage<T>* ensureStorage();

    template <class T>
    ComponentStorage<T>* getStorage();

    template <class T>
    const ComponentStorage<T>* getStorage() const;

    [[nodiscard]] bool isValidEntityIndex(EntityId entity) const;

    std::vector<EntityRecord> m_entities;
    std::vector<EntityId> m_freeList;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentStorage>> m_componentStores;
};

template <class T, class... Args>
T& World::addComponent(EntityId entity, Args&&... args) {
    auto* storage = ensureStorage<T>();
    auto [it, inserted] = storage->components.try_emplace(entity, std::forward<Args>(args)...);
    if (!inserted) {
        it->second = T(std::forward<Args>(args)...);
    }
    return it->second;
}

template <class T>
bool World::hasComponent(EntityId entity) const {
    if (!isAlive(entity)) {
        return false;
    }

    const auto* storage = getStorage<T>();
    if (storage == nullptr) {
        return false;
    }
    return storage->components.find(entity) != storage->components.end();
}

template <class T>
T* World::getComponent(EntityId entity) {
    auto* storage = getStorage<T>();
    if (storage == nullptr) {
        return nullptr;
    }

    const auto it = storage->components.find(entity);
    if (it == storage->components.end()) {
        return nullptr;
    }
    return &it->second;
}

template <class T>
const T* World::getComponent(EntityId entity) const {
    const auto* storage = getStorage<T>();
    if (storage == nullptr) {
        return nullptr;
    }

    const auto it = storage->components.find(entity);
    if (it == storage->components.end()) {
        return nullptr;
    }
    return &it->second;
}

template <class... Components, class Func>
void World::forEach(Func&& func) {
    static_assert(sizeof...(Components) > 0, "forEach requires at least one component type");

    using FirstComponent = std::tuple_element_t<0, std::tuple<Components...>>;
    auto* primaryStorage = getStorage<FirstComponent>();
    if (primaryStorage == nullptr) {
        return;
    }

    for (auto& [entity, ignored] : primaryStorage->components) {
        (void)ignored;
        if (!isAlive(entity)) {
            continue;
        }
        if (!(hasComponent<Components>(entity) && ...)) {
            continue;
        }
        func(entity, *getComponent<Components>(entity)...);
    }
}

template <class T>
World::ComponentStorage<T>* World::ensureStorage() {
    const std::type_index key(typeid(T));
    const auto it = m_componentStores.find(key);
    if (it != m_componentStores.end()) {
        return static_cast<ComponentStorage<T>*>(it->second.get());
    }

    auto storage = std::make_unique<ComponentStorage<T>>();
    auto* rawStorage = storage.get();
    m_componentStores.emplace(key, std::move(storage));
    return rawStorage;
}

template <class T>
World::ComponentStorage<T>* World::getStorage() {
    const std::type_index key(typeid(T));
    const auto it = m_componentStores.find(key);
    if (it == m_componentStores.end()) {
        return nullptr;
    }
    return static_cast<ComponentStorage<T>*>(it->second.get());
}

template <class T>
const World::ComponentStorage<T>* World::getStorage() const {
    const std::type_index key(typeid(T));
    const auto it = m_componentStores.find(key);
    if (it == m_componentStores.end()) {
        return nullptr;
    }
    return static_cast<const ComponentStorage<T>*>(it->second.get());
}

} // namespace engine::ecs
```

### `engine/ecs/world.cpp`

```cpp
#include "engine/ecs/world.h"

namespace engine::ecs {

EntityId World::createEntity() {
    if (!m_freeList.empty()) {
        const EntityId recycled = m_freeList.back();
        m_freeList.pop_back();
        if (isValidEntityIndex(recycled)) {
            auto& record = m_entities[recycled - 1];
            record.alive = true;
            return recycled;
        }
    }

    m_entities.push_back(EntityRecord{});
    m_entities.back().alive = true;
    return static_cast<EntityId>(m_entities.size());
}

void World::destroyEntity(EntityId entity) {
    if (!isAlive(entity)) {
        return;
    }

    auto& record = m_entities[entity - 1];
    record.alive = false;
    ++record.generation;
    m_freeList.push_back(entity);

    for (auto& [type, storage] : m_componentStores) {
        (void)type;
        storage->remove(entity);
    }
}

void World::clear() {
    for (auto& [type, storage] : m_componentStores) {
        (void)type;
        storage->clear();
    }
    m_entities.clear();
    m_freeList.clear();
}

bool World::isAlive(EntityId entity) const {
    if (!isValidEntityIndex(entity)) {
        return false;
    }
    return m_entities[entity - 1].alive;
}

bool World::isValidEntityIndex(EntityId entity) const {
    return entity > 0 && static_cast<std::size_t>(entity - 1) < m_entities.size();
}

} // namespace engine::ecs
```

---

## 3) Компоненты Transform / Tag / MeshRenderer

Компоненты уже включены в `engine/ecs/components.h` выше:
- `Transform` (позиция, поворот, масштаб, `toMatrix()`);
- `Tag` (имя объекта);
- `MeshRenderer` (тип примитива, tint/материал через `shaderId`, `textureId`, ссылки на GPU-ресурсы).

---

## 4) RenderSystem + RenderAdapter

### `engine/renderer/RenderAdapter.h`

```cpp
#pragma once

#include <memory>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "engine/ecs/components.h"
#include "engine/renderer/Renderer.h"

namespace engine::renderer {

class RenderAdapter final {
public:
    explicit RenderAdapter(Renderer& renderer) : m_renderer(&renderer) {}

    std::shared_ptr<resources::Mesh> loadMesh(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadMesh(id) : nullptr;
    }

    std::shared_ptr<resources::Texture> loadTexture(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadTexture(id) : nullptr;
    }

    std::shared_ptr<resources::ShaderProgram> loadShaderProgram(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadShaderProgram(id) : nullptr;
    }

    std::unique_ptr<rhi::IBuffer> createBuffer(const rhi::BufferDesc& desc, const void* initialData) {
        return m_renderer != nullptr ? m_renderer->createGpuBuffer(desc, initialData) : nullptr;
    }

    std::unique_ptr<rhi::IImage> createImage(const rhi::ImageDesc& desc, const void* initialData) {
        return m_renderer != nullptr ? m_renderer->createGpuImage(desc, initialData) : nullptr;
    }

    std::shared_ptr<resources::ShaderProgram> createShaderProgram(
        const std::string& key,
        const std::string& vertexPath,
        const std::string& fragmentPath) {
        return m_renderer != nullptr ? m_renderer->createShaderProgramFromFiles(key, vertexPath, fragmentPath) : nullptr;
    }

    void drawPrimitive(
        ecs::PrimitiveType primitiveType,
        const glm::mat4& transformMatrix,
        const glm::vec4& tint) {
        if (m_renderer != nullptr) {
            m_renderer->drawPrimitive(primitiveType, transformMatrix, tint);
        }
    }

    void drawMesh(
        const resources::Mesh& mesh,
        const resources::ShaderProgram& shader,
        const resources::Texture& texture,
        const glm::mat4& transformMatrix,
        const glm::vec4& tint) {
        if (m_renderer != nullptr) {
            m_renderer->drawMesh(mesh, shader, texture, transformMatrix, tint);
        }
    }

private:
    Renderer* m_renderer{nullptr};
};

} // namespace engine::renderer
```

### `engine/ecs/render_system.cpp`

```cpp
#include "engine/ecs/render_system.h"

#include <glm/mat4x4.hpp>

#include "engine/ecs/components.h"
#include "engine/ecs/world.h"
#include "engine/renderer/RenderAdapter.h"

namespace engine::ecs {

namespace {

glm::mat4 computeWorldMatrix(World& world, const EntityId entity) {
    auto* transform = world.getComponent<Transform>(entity);
    if (transform == nullptr) {
        return glm::mat4(1.0F);
    }

    glm::mat4 worldMatrix = transform->toMatrix();
    EntityId parent = kInvalidEntity;
    if (const auto* hierarchy = world.getComponent<Hierarchy>(entity)) {
        parent = hierarchy->parent;
    }

    int safetyCounter = 0;
    while (parent != kInvalidEntity && safetyCounter < 64) {
        if (const auto* parentTransform = world.getComponent<Transform>(parent)) {
            worldMatrix = parentTransform->toMatrix() * worldMatrix;
        }

        const auto* parentHierarchy = world.getComponent<Hierarchy>(parent);
        if (parentHierarchy == nullptr || parentHierarchy->parent == parent) {
            break;
        }
        parent = parentHierarchy->parent;
        ++safetyCounter;
    }

    return worldMatrix;
}

} // namespace

void RenderSystem::render(
    World& world,
    renderer::RenderAdapter& renderer,
    const glm::mat4& viewProjectionMatrix) {
    world.forEach<Transform, MeshRenderer>([&](EntityId entity, Transform& transform, MeshRenderer& meshRenderer) {
        (void)transform;
        if (!meshRenderer.visible) {
            return;
        }

        if (meshRenderer.mesh == nullptr && !meshRenderer.meshId.empty()) {
            meshRenderer.mesh = renderer.loadMesh(meshRenderer.meshId);
        }
        if (meshRenderer.texture == nullptr && !meshRenderer.textureId.empty()) {
            meshRenderer.texture = renderer.loadTexture(meshRenderer.textureId);
        }
        if (meshRenderer.shader == nullptr && !meshRenderer.shaderId.empty()) {
            meshRenderer.shader = renderer.loadShaderProgram(meshRenderer.shaderId);
        }

        const glm::mat4 modelMatrix = computeWorldMatrix(world, entity);
        if (meshRenderer.hasGpuResources()) {
            renderer.drawMesh(
                *meshRenderer.mesh,
                *meshRenderer.shader,
                *meshRenderer.texture,
                viewProjectionMatrix * modelMatrix,
                meshRenderer.tint);
            return;
        }

        renderer.drawPrimitive(meshRenderer.primitiveType, modelMatrix, meshRenderer.tint);
    });
}

} // namespace engine::ecs
```

---

## 5) Интеграция ECS в игровой цикл

### `engine/game/StateStack.cpp` (вызов `update/render` активного состояния)

```cpp
#include "engine/game/StateStack.h"

#include "engine/core/Log.h"
#include "engine/renderer/Renderer.h"

namespace engine::game {

void StateStack::push(std::unique_ptr<IGameState> state) {
    m_pending.push_back(PendingChange{Action::Push, std::move(state)});
}

void StateStack::pop() {
    m_pending.push_back(PendingChange{Action::Pop, nullptr});
}

void StateStack::replace(std::unique_ptr<IGameState> state) {
    m_pending.push_back(PendingChange{Action::Replace, std::move(state)});
}

void StateStack::clear() {
    m_pending.push_back(PendingChange{Action::Clear, nullptr});
}

void StateStack::handleEvent(const platform::Event& event) {
    if (!m_stack.empty()) {
        m_stack.back()->handleEvent(event);
    }
}

void StateStack::update(double dt) {
    if (!m_stack.empty()) {
        m_stack.back()->update(dt);
    }
}

void StateStack::render(renderer::Renderer& renderer) {
    for (const auto& state : m_stack) {
        state->render(renderer);
    }
}

void StateStack::applyPendingChanges() {
    for (auto& pending : m_pending) {
        switch (pending.action) {
            case Action::Push:
                if (pending.state) {
                    ENGINE_LOG_INFO("StateStack push");
                    pending.state->onEnter();
                    m_stack.push_back(std::move(pending.state));
                }
                break;
            case Action::Pop:
                if (!m_stack.empty()) {
                    ENGINE_LOG_INFO("StateStack pop");
                    m_stack.back()->onExit();
                    m_stack.pop_back();
                }
                break;
            case Action::Replace:
                if (!m_stack.empty()) {
                    ENGINE_LOG_INFO("StateStack replace old");
                    m_stack.back()->onExit();
                    m_stack.pop_back();
                }
                if (pending.state) {
                    ENGINE_LOG_INFO("StateStack replace new");
                    pending.state->onEnter();
                    m_stack.push_back(std::move(pending.state));
                }
                break;
            case Action::Clear:
                while (!m_stack.empty()) {
                    m_stack.back()->onExit();
                    m_stack.pop_back();
                }
                break;
        }
    }
    m_pending.clear();
}

bool StateStack::empty() const {
    return m_stack.empty();
}

} // namespace engine::game
```

### Фрагменты `engine/core/Application.cpp` (игровой цикл -> `StateStack`)

```cpp
int Application::run() {
    if (!m_initialized) {
        return 1;
    }

    FrameTimer timer;
    timer.reset();

    constexpr double fixedDt = 1.0 / 60.0;
    double accumulator = 0.0;
    TimeStatsAggregator stats(1.0);

    while (m_running && m_window && !m_window->shouldClose()) {
        m_window->pollEvents();
        pollRuntimeHotReload();

        double frameDt = timer.tickSeconds();
        frameDt = std::min(frameDt, 0.25);
        m_workspaceSampleTime += frameDt;

        if (m_workspaceMode == WorkspaceMode::EngineStates) {
            accumulator += frameDt;
            while (accumulator >= fixedDt) {
                m_stateStack.update(fixedDt);
                m_stateStack.applyPendingChanges();
                accumulator -= fixedDt;
            }
        }

        const glm::vec4 clearColor{
            m_config.clearColor[0], m_config.clearColor[1], m_config.clearColor[2], m_config.clearColor[3]};
        m_renderer.beginFrame(clearColor);
        m_renderer.beginImGuiFrame(static_cast<float>(frameDt));
        renderWorkspace(frameDt);
        renderWorkspaceUi(frameDt);
        m_renderer.renderImGui();
        m_renderer.endFrame();

        // ...
    }

    shutdown();
    return 0;
}
```

```cpp
void Application::renderWorkspace(const double frameDtSeconds) {
    switch (m_workspaceMode) {
        case WorkspaceMode::EngineStates:
            m_stateStack.render(m_renderer);
            break;
        // ...
    }
}
```

---

## 6) Несколько объектов с разными трансформациями

### `engine/game/GameplayState.h`

```cpp
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
```

### `engine/game/GameplayState.cpp`

```cpp
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
```

---

## 7) Иерархия и локальные -> мировые трансформации

Ключевая логика в `computeWorldMatrix()` (см. `engine/ecs/render_system.cpp` выше):
- берётся локальная матрица текущей сущности;
- затем цепочка родителей из `Hierarchy.parent`;
- на каждом шаге родительская матрица умножается слева: `parent * child`;
- результат — итоговая `world`-матрица для рендера.

Связь parent-child задаётся при построении сцены в `GameplayState::setupScene()`:

```cpp
if (i == 0) {
    rootModelEntity = entity;
} else {
    ecs::Hierarchy hierarchy{};
    hierarchy.parent = rootModelEntity;
    m_world.addComponent<ecs::Hierarchy>(entity, hierarchy);
}
```

