# Архитектура TurboGameEngine

Этот обзор описывает базовую архитектуру до внедрения общей Job System. Его разделы об asset workers, параллельности ECS/physics и составе тестов не отражают текущую реализацию. Актуальный подробный разбор файлов, потока кадра и многопоточности: [путеводитель по коду](project-code-guide.md). Детали ЛР 1: [архитектура Job System](lab1/architecture.md).

## Модули и цели сборки

```mermaid
flowchart TD
    app["app<br/>main.cpp + Application.cpp"]
    core["engine_core<br/>Log · Time · Config · EventBus"]
    platform["engine_platform<br/>GLFW window · events · input"]
    game["engine_game<br/>StateStack · states · editor<br/>World · PhysicsSystem"]
    renderer["engine_renderer<br/>Renderer · resources/loaders<br/>RenderSystem · transform utilities"]
    rhi["engine_rhi<br/>интерфейсы и фабрика RHI"]
    diligent_impl["engine_rhi_diligent<br/>единственная активная реализация"]
    diligent["Diligent Engine<br/>D3D11 · D3D12 · Vulkan · GL · WebGPU"]
    assets["assets<br/>models · textures · shaders · scenes"]
    tests["engine_physics_tests"]
    legacy["rhi_opengl / rhi_vulkan / rhi_d3d12<br/>legacy-код, не подключён к runtime"]

    app --> core
    app --> platform
    app --> renderer
    app --> game
    game --> core
    game --> platform
    game --> renderer
    renderer --> core
    renderer --> rhi
    rhi --> diligent_impl
    diligent_impl --> diligent
    platform --> core
    assets -. "POST_BUILD copy" .-> app
    tests --> game
    tests --> renderer
    legacy -. "не собирается" .-> rhi

    classDef inactive fill:#eeeeee,stroke:#888888,stroke-dasharray:5 5,color:#555555
    class legacy inactive
```

Папки и CMake-цели совпадают не полностью: `engine/resources` и часть `engine/ecs` собираются в `engine_renderer`; `World`, физика и debug rendering — в `engine_game`; `engine/core/Application.cpp` входит непосредственно в executable `app`. Проверено по `CMakeLists.txt:235-387` и `engine/rhi/RHI.cpp:11-30`.

## Инициализация и главный цикл

```mermaid
sequenceDiagram
    autonumber
    participant Main as main
    participant App as Application
    participant Window as WindowGLFW
    participant Renderer
    participant RHI as RHI / Diligent
    participant Stack as StateStack
    participant State as IGameState

    Main->>App: construct(Diligent)
    Main->>App: init()
    App->>App: Log + Config
    App->>Window: create + event callback
    App->>Renderer: init(window, backend, vsync)
    Renderer->>RHI: device + swapchain
    Renderer->>Renderer: asset workers, frame contexts,
    Note right of Renderer: fallbacks, loaders, ImGui
    App->>Stack: push initial state
    Stack->>State: onEnter()
    App-->>Main: initialized

    Main->>App: run()
    loop visual frame
        App->>Window: begin input + poll events
        Window-->>App: onEvent(event), 0..N
        App->>Renderer: config/shader hot reload
        loop accumulator >= 1/60 s
            App->>Stack: update(1/60)
            Stack->>State: update top state
        end
        App->>Renderer: beginFrame + beginImGuiFrame
        App->>Stack: render all states + UI
        App->>Renderer: renderImGui + endFrame
        Renderer->>RHI: Flush + Present
    end

    App->>Stack: clear / onExit
    App->>Renderer: shutdown
    App->>Window: destroy
```

Точка входа — `app/main.cpp:5-15`. Главный цикл находится в `engine/core/Application.cpp:290-347`. `StateStack` обновляет верхнее состояние, а рисует стек снизу вверх (`engine/game/StateStack.cpp:24-47`).

## Игровые состояния и ECS

```mermaid
classDiagram
    direction LR

    class IGameState {
        <<interface>>
        +onEnter()
        +onExit()
        +handleEvent(event)
        +update(dt)
        +render(renderer)
    }
    class StateStack {
        -states
        -pendingChanges
        +push()
        +pop()
        +replace()
        +update()
        +render()
    }
    class GameplayState {
        -World world
        -PhysicsSystem physicsSystem
        -RenderSystem renderSystem
        -DebugRenderSystem debugRenderSystem
        -EventBus eventBus
    }
    class MenuState
    class PauseState

    IGameState <|-- MenuState
    IGameState <|-- PauseState
    IGameState <|-- GameplayState
    StateStack "1" *-- "0..*" IGameState

    class EntityId {
        <<uint32_t alias>>
    }
    class World {
        -entityRecords
        -freeList
        -componentStores
        +createEntity()
        +destroyEntity()
        +addComponent()
        +getComponent()
        +forEach()
    }
    class ComponentStorage~T~ {
        -unordered_map components
    }
    class Transform {
        +position
        +rotation
        +scale
        +toMatrix()
    }
    class Rigidbody {
        +mass
        +velocity
        +applyForce()
        +wakeUp()
    }
    class Collider
    class Camera
    class Tag
    class Hierarchy
    class MeshRenderer {
        +meshId / textureId / shaderId
        +mesh / texture / shader
        +tint / uvScale
    }
    class PhysicsSystem
    class RenderSystem
    class Renderer

    GameplayState *-- World
    GameplayState *-- PhysicsSystem
    GameplayState *-- RenderSystem
    World --> EntityId
    World *-- ComponentStorage~T~
    ComponentStorage~T~ ..> Transform
    ComponentStorage~T~ ..> Rigidbody
    ComponentStorage~T~ ..> Collider
    ComponentStorage~T~ ..> Camera
    ComponentStorage~T~ ..> Tag
    ComponentStorage~T~ ..> Hierarchy
    ComponentStorage~T~ ..> MeshRenderer
    PhysicsSystem ..> World
    RenderSystem ..> World
    RenderSystem --> Renderer
```

Это гибридная архитектура: наследование используется для крупных состояний приложения, а объекты сцены собираются из компонентов. Компоненты в основном хранят состояние, но не являются строго data-only: у `Transform`, `Rigidbody` и `MeshRenderer` есть вспомогательные методы (`engine/ecs/components.h:21-143`). Автоматического ECS-scheduler нет — порядок систем задаёт `GameplayState`.

## Асинхронные ресурсы, рендеринг и потоки

```mermaid
sequenceDiagram
    participant Caller as RenderSystem / editor
    participant RM as ResourceManager
    participant Worker as Asset workers (1-4)
    participant Main as Main-thread queue
    participant RHI as Diligent / GPU

    Caller->>RM: load(type, path)
    alt weak cache hit
        RM-->>Caller: existing shared_ptr
    else cache miss
        RM-->>Caller: shared_ptr, state = Loading
        RM->>Worker: enqueue CPU task
        Worker->>Worker: Assimp / stb_image / JSON parse
        Worker->>Main: enqueue completion
        Main->>RHI: buffers / image / shaders / PSO
        RHI-->>Main: GPU resources
        Main-->>Caller: same object, Loaded or Failed
    end
    Caller->>RHI: drawMesh
    Note over Caller,RHI: Пока ресурс не готов, используются fallback mesh, texture и shader.
```

CPU-чтение и декодирование выполняются асинхронно. GPU upload и shader compilation выполняются на главном потоке при обработке completion-очереди (`engine/renderer/Renderer.cpp:398-489,753-975,1089-1095`). Очереди защищены `mutex` и `condition_variable`; ECS, физика, UI и render submission не распараллелены.

`ResourceManager` хранит loaders, fallback-ресурсы и cache на `weak_ptr`. Реальное время жизни задают `shared_ptr` у клиентов, в частности у `MeshRenderer`; сами `Mesh`, `Texture` и `ShaderProgram` владеют RHI-объектами.

За кадр `RenderSystem` обходит сущности с `Transform + MeshRenderer`, вычисляет world matrix и вызывает `Renderer::drawMesh`. Перед каждым draw обновляются dynamic uniform buffers с MVP, model, UV scale, tint и фиксированным освещением. Render queue, batching, sorting и render graph отсутствуют.

## Материалы и шейдеры

Отдельного типа `Material` нет. Его упрощённую роль выполняют поля `shader + texture + tint + uvScale` в `MeshRenderer`. `ShaderProgram` содержит vertex/fragment modules, pipeline layout и PSO. Shader descriptor — JSON с путями vertex и fragment shader; hot reload отслеживает timestamps и заменяет PSO на главном потоке.

Активный путь использует HLSL через Diligent. Поддерживается одна texture binding. Вызов RHI `pushConstants()` в Diligent-реализации фактически обновляет dynamic uniform buffers через `MAP_FLAG_DISCARD` (`engine/rhi_diligent/DiligentDevice.cpp:327-395`).

## На что ориентировались

Diligent Engine — не только источник вдохновения, а реальный графический backend проекта. `reports/5.tex:139-153` сравнивает возможности редактора с Unity, Unreal Engine и Roblox Studio; подтверждения, что ECS или `StateStack` копируют архитектуру конкретного движка, в репозитории нет.

## Что стоило бы переделать

1. Разделить `GameplayState.cpp` на runtime сцены, editor layer, serialization, undo/redo и picking; разделить `Renderer.cpp` на frame renderer, asset manager, shader manager и ImGui layer.
2. Выделить настоящие CMake-модули `engine_ecs`, `engine_physics`, `engine_resources`, `engine_editor`.
3. Сделать entity handle парой `{index, generation}`: generation увеличивается, но сейчас не входит в `EntityId` и не проверяется.
4. Либо использовать Diligent напрямую, либо устранить Diligent-зависимости из `Renderer` и реализовать полноценный backend-neutral RHI.
5. Ввести `Material` / `MaterialInstance` и shader metadata/reflection вместо определения vertex layout по тексту HLSL и текстурности по имени файла.
6. Коалесцировать одинаковые запросы ресурсов, ограничить GPU-upload budget на кадр и добавить тесты resource concurrency и RHI lifecycle.

Важно: два `FrameContext` не означают настоящие два frames-in-flight. В текущем Diligent-адаптере semaphore пуст, а fence не ждёт GPU (`engine/rhi_diligent/DiligentDevice.cpp:189-209,780-796`).
