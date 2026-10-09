# TurboGameEngine: как разобраться в проекте и коде

Путеводитель по базовой архитектуре на 02.10.2026. Дополнение 08.10.2026: [GPU upload queue — файлы, fence, корректность и защита](lab1/gpu-upload-queue.md). Ниже main-thread upload относится к fallback / TGE_GPU_UPLOAD=0; текущий D3D12/Vulkan transfer-путь записывает mesh/texture copies из jobs, main публикует готовые handles. Номера строк базового путеводителя могут сдвинуться. Исторические результаты — в [комплекте ЛР 1](../docs/lab1/README.md).

Ссылки на исходники относительные, подходят для GitHub и указывают на конкретные строки через `#L`. После изменения кода номера могут сдвинуться; имя функции остаётся ориентиром. Упрощённые примеры помечены и не являются дополнительными файлами проекта.

## Содержание

1. [Главная идея и карта папок](#1-главная-идея-и-карта-папок)
2. [Карта файлов: что где находится](#2-карта-файлов-что-где-находится)
3. [От main до закрытия приложения](#3-от-main-до-закрытия-приложения)
4. [Один кадр: update не равен render](#4-один-кадр-update-не-равен-render)
5. [Окно, ввод и состояния](#5-окно-ввод-и-состояния)
6. [ECS: как представлен объект](#6-ecs-как-представлен-объект)
7. [Координаты, иерархия и камера](#7-координаты-иерархия-и-камера)
8. [От объекта ECS до draw call](#8-от-объекта-ecs-до-draw-call)
9. [Job System: API и внутреннее устройство](#9-job-system-api-и-внутреннее-устройство)
10. [Ресурсы: от PNG и OBJ до GPU](#10-ресурсы-от-png-и-obj-до-gpu)
11. [Renderer, RHI и Diligent](#11-renderer-rhi-и-diligent)
12. [Как работает физика](#12-как-работает-физика)
13. [Редактор и большой GameplayState](#13-редактор-и-большой-gameplaystate)
14. [ЛР 1 и Tracy: что именно измеряется](#14-лр-1-и-tracy-что-именно-измеряется)
15. [Сборка, инструменты и тесты](#15-сборка-инструменты-и-тесты)
16. [Маршрут чтения, отладка и места для изменений](#16-маршрут-чтения-отладка-и-места-для-изменений)
17. [Термины и проверка понимания](#17-термины-и-проверка-понимания)

## 1. Главная идея и карта папок

Проект — небольшое C++20-приложение с собственными сценами, редактором, ECS, простой физикой, ресурсной системой и обёрткой рендера. Diligent выполняет низкоуровневую графическую работу; GLFW — окно и ввод; enkiTS — планирование CPU-задач. Это не один огромный класс, но `Application`, `GameplayState` и `Renderer` являются основными точками сборки остальных частей.

Разделяй четыре понятия:

- **Приложение** управляет запуском, временем и завершением: `Application`.
- **Сцена** хранит объекты и игровую/редакторскую логику: `GameplayState` + `World`.
- **Система** обрабатывает компоненты: PhysicsSystem, RenderSystem, DebugRenderSystem.
- **Сервис** используется разными системами: JobSystem, Renderer, ResourceManager.

```text
TurboGameEngine/
├─ app/                    точка входа
├─ engine/
│  ├─ core/                приложение, время, конфиг, лог, JobSystem, Tracy
│  ├─ platform/            окно GLFW, события ОС, состояние ввода
│  ├─ game/                состояния, сцена, редактор, управление камерой
│  ├─ ecs/                 объекты, компоненты, render/physics systems
│  ├─ renderer/            высокий уровень отрисовки
│  ├─ resources/           CPU-данные, кеш, загрузчики, очередь загрузки
│  ├─ rhi/                 интерфейсы графического слоя
│  ├─ rhi_diligent/         активная реализация этих интерфейсов
│  └─ rhi_opengl/vulkan/d3d12/ старые реализации, не текущий runtime
├─ assets/                 модели, текстуры, HLSL, scene/layout JSON
├─ third_party/            исходники Diligent и его примеры
├─ tests/                  четыре набора C++-проверок
├─ tools/                  генератор workload, запуск, замеры, анализ
├─ docs/                   объяснения и материалы защиты
└─ reports/                сохранённые результаты, трейсы, презентация
```

Папка — организация исходников, а **CMake target** — то, что компилятор действительно собирает. Они не совпадают один к одному: часть ECS входит в `engine_renderer`, часть — в `engine_game`. `Application.cpp`, несмотря на папку core, компилируется непосредственно в executable `app`.

Основной путь вызовов:

```text
main → Application → StateStack → GameplayState
                                 ├─ World → PhysicsSystem
                                 └─ World → RenderSystem → RenderAdapter
                                                          ↓
                                                       Renderer
                                                   resources + RHI
                                                          ↓
                                                    Diligent Engine
                                                          ↓
                                                        GPU/API
```

JobSystem — общий CPU-сервис сбоку от этого пути. Им пользуются подготовка матриц, независимые физические фазы и декодирование ресурсов. Он не заменяет главный цикл и не выполняет всю игру автоматически.

## 2. Карта файлов: что где находится

### Вход и core

| Файл | Содержимое и зачем открывать |
|---|---|
| [app/main.cpp](../app/main.cpp#L5) | Создать Application → init → run. Начало чтения проекта. |
| [Application.h](../engine/core/Application.h#L30), [Application.cpp](../engine/core/Application.cpp#L236) | Владельцы сервисов, startup/frame/shutdown, workspace, маршрутизация событий, runtime config reload. |
| [Config.h](../engine/core/Config.h#L9), [Config.cpp](../engine/core/Config.cpp#L13) | EngineConfig с default values и чтение `config.json`. |
| [LabOptions.h](../engine/core/LabOptions.h#L10) | Управляемые параметры ЛР из окружения `TGE_*`; фиксированные сцены и переключатели A/B. |
| [JobSystem.h](../engine/core/JobSystem.h#L15), [JobSystem.cpp](../engine/core/JobSystem.cpp#L30) | Публичный API задач и адаптация enkiTS: handles, ranges, зависимости, ожидание, исключения, counters. |
| [Profiling.h](../engine/core/Profiling.h#L3) | Макросы CPU-зон, кадров, messages и plots Tracy; без profiling они выключены. |
| [Time.h](../engine/core/Time.h), [Time.cpp](../engine/core/Time.cpp#L13) | FrameTimer на steady_clock; секундная агрегация avg/min/max для лога. |
| [Log.h](../engine/core/Log.h), [Log.cpp](../engine/core/Log.cpp) | `ENGINE_LOG_*`, консоль и вращающийся файл `logs/engine.log` через spdlog. |
| [Assert.h](../engine/core/Assert.h) | Макрос проверки программных условий; это не полноценная обработка пользовательских ошибок. |
| [EventBus.h](../engine/core/EventBus.h#L11) | Типизированные subscribe/publish. Сейчас связывает collision events с диагностикой сцены. |

### Platform и game

| Файл | Содержимое и зачем открывать |
|---|---|
| [Window.h](../engine/platform/Window.h#L26), [Window.cpp](../engine/platform/Window.cpp#L7) | Контракт окна и factory. Factory создаёт WindowGLFW. |
| [WindowGLFW.h](../engine/platform/WindowGLFW.h), [WindowGLFW.cpp](../engine/platform/WindowGLFW.cpp#L117) | Создание native window, callbacks клавиатуры/мыши/resize, pollEvents. |
| [Events.h](../engine/platform/Events.h#L50) | Enum типов события, клавиши/кнопки и struct Event с payload. |
| [Input.h](../engine/platform/Input.h#L11), [Input.cpp](../engine/platform/Input.cpp#L41) | Current/previous key states, pressed/down, mouse delta и UI capture. |
| [IGameState.h](../engine/game/IGameState.h#L13) | Интерфейсы IGameState и IGameStateUi; lifecycle/update/render contract. |
| [StateStack.h](../engine/game/StateStack.h), [StateStack.cpp](../engine/game/StateStack.cpp#L8) | Владение состояниями, отложенные push/pop/replace/clear, доступ к сервисам. |
| [MenuState.h](../engine/game/MenuState.h), [MenuState.cpp](../engine/game/MenuState.cpp#L25) | Меню; Enter заменяет его на GameplayState. |
| [PauseState.h](../engine/game/PauseState.h), [PauseState.cpp](../engine/game/PauseState.cpp#L18) | Верхнее состояние паузы; Escape снимает его. Не путать с Edit/Play редактора. |
| [GameplayState.h](../engine/game/GameplayState.h#L49), [GameplayState.cpp](../engine/game/GameplayState.cpp#L1101) | World, сцена, editor UI, snapshots, сохранение, picking, gizmo, lab demo. Читать по функциям, не линейно. |
| [FreeCameraController.h](../engine/game/FreeCameraController.h#L10), [FreeCameraController.cpp](../engine/game/FreeCameraController.cpp#L25) | Преобразует ввод в движение и поворот Transform камеры. |
| [ImGuizmoCompat.h](../engine/game/ImGuizmoCompat.h) | Совместимость внешнего ImGuizmo с используемой версией ImGui. |

### ECS

| Файл | Содержимое и зачем открывать |
|---|---|
| [entity.h](../engine/ecs/entity.h#L7) | EntityId — uint32; 0 означает invalid. |
| [world.h](../engine/ecs/world.h#L15), [world.cpp](../engine/ecs/world.cpp#L5) | Хранилище объектов и компонентов; templates forEach/add/get в header. |
| [components.h](../engine/ecs/components.h#L21) | Все текущие component data types. Здесь искать поле объекта. |
| [transform_utils.h](../engine/ecs/transform_utils.h), [transform_utils.cpp](../engine/ecs/transform_utils.cpp#L13) | Local→world matrix, transform point, scale, направления камеры. |
| [transform_batch.h](../engine/ecs/transform_batch.h), [transform_batch.cpp](../engine/ecs/transform_batch.cpp#L10) | Расчёт массива world-матриц serial/parallel; выбранная задача №2 ЛР. |
| [render_system.h](../engine/ecs/render_system.h), [render_system.cpp](../engine/ecs/render_system.cpp#L14) | Gather объектов → prepare matrices → submit draw calls. |
| [physics_system.h](../engine/ecs/physics_system.h), [physics_system.cpp](../engine/ecs/physics_system.cpp#L519) | Settings, integration, proxies, broadphase, solver, sleep, collision events. |
| [collision_utils.h](../engine/ecs/collision_utils.h), [collision_utils.cpp](../engine/ecs/collision_utils.cpp#L166) | World collider shapes, bounds и пересечения AABB/Sphere. |
| [debug_render_system.h](../engine/ecs/debug_render_system.h), [debug_render_system.cpp](../engine/ecs/debug_render_system.cpp#L76) | Визуальная диагностика collider bounds, не обычная отрисовка mesh. |

### Renderer и resources

| Файл | Содержимое и зачем открывать |
|---|---|
| [Renderer.h](../engine/renderer/Renderer.h#L50), [Renderer.cpp](../engine/renderer/Renderer.cpp#L330) | GPU/device, frame, draw, fallbacks, ImGui, загрузка и shader reload. |
| [RenderAdapter.h](../engine/renderer/RenderAdapter.h#L16) | Тонкая переадресация ECS-вызовов в Renderer; не отдельный renderer. |
| [Transform2D.h](../engine/renderer/Transform2D.h) | Простой 2D transform для primitive/demo path; не замена ECS Transform. |
| [resource_manager.h](../engine/resources/resource_manager.h#L15), [resource_manager.cpp](../engine/resources/resource_manager.cpp#L5) | Типизированные loaders, weak cache и fallbacks; cpp содержит clear. |
| [resource_state.h](../engine/resources/resource_state.h#L7) | Состояния загрузки. Reloading объявлен, но не основной используемый путь. |
| [mesh.h](../engine/resources/mesh.h#L16) | MeshVertex, MeshData и Mesh с CPU vectors + GPU buffers. |
| [texture.h](../engine/resources/texture.h#L14) | TextureData pixels/dimensions и Texture с GPU image. |
| [shader_program.h](../engine/resources/shader_program.h#L12) | Shader paths/modules/layout/pipeline и проверка готовности. |
| [loaders.h](../engine/resources/loaders.h#L10), [loaders.cpp](../engine/resources/loaders.cpp#L99) | CPU import Assimp, decode stb_image, parsing shader descriptor. |
| [async_load_queue.h](../engine/resources/async_load_queue.h#L30), [async_load_queue.cpp](../engine/resources/async_load_queue.cpp#L84) | Work→Finalize, Low jobs, backpressure, main-thread pump и shutdown join. |

### RHI, assets и служебные файлы

| Файл/папка | Содержимое |
|---|---|
| [rhi/Types.h](../engine/rhi/Types.h) | Общие enum/descriptor types: formats, buffers, pipeline, viewport, barriers. Descriptor — набор параметров создания, не созданный GPU object. |
| [rhi/Device.h](../engine/rhi/Device.h) | IDevice создаёт GPU wrappers; IQueue описывает submit. |
| [rhi/CommandBuffer.h](../engine/rhi/CommandBuffer.h) | Интерфейс begin/pass/bind/draw. Название не гарантирует native command-list implementation. |
| [rhi/Resources.h](../engine/rhi/Resources.h), [Pipeline.h](../engine/rhi/Pipeline.h) | Интерфейсы buffers/images/shader modules и pipelines/bind groups. |
| [rhi/Swapchain.h](../engine/rhi/Swapchain.h), [Sync.h](../engine/rhi/Sync.h) | Экранный буфер/present и контракты fence/semaphore. |
| [rhi/RHI.h](../engine/rhi/RHI.h), [RHI.cpp](../engine/rhi/RHI.cpp#L11) | Factory графического устройства; активный backend только Diligent. |
| [DiligentDevice.h](../engine/rhi_diligent/DiligentDevice.h#L32), [DiligentDevice.cpp](../engine/rhi_diligent/DiligentDevice.cpp#L458) | Конкретная адаптация RHI к Diligent и выбранному внутри него API. |
| [textured.shader.json](../assets/shaders_hlsl/textured.shader.json) | Связывает vertex/fragment HLSL paths. |
| [textured.vert.hlsl](../assets/shaders_hlsl/textured.vert.hlsl#L23), [textured.frag.hlsl](../assets/shaders_hlsl/textured.frag.hlsl#L19) | GPU vertex transform и pixel texture/tint/простое lighting. |
| [basic.vert.hlsl](../assets/shaders_hlsl/basic.vert.hlsl), [basic.frag.hlsl](../assets/shaders_hlsl/basic.frag.hlsl), [basic.hlsl](../assets/shaders_hlsl/basic.hlsl) | Простые shaders; последний также вход отдельного shader-compiler tool. |
| `assets/models`, `assets/textures` | Файлы данных, а не C++-логика. `__diligent_cube__` и похожие IDs обозначают процедурные ресурсы, не файлы. |
| [editor_scene.json](../assets/scenes/editor_scene.json), [editor_layout.json](../assets/editor_layout.json) | Сохранённая сцена и отдельно расположение/видимость UI-панелей. |
| `assets/shaders_gl`, `engine/rhi_opengl`, `engine/rhi_vulkan`, `engine/rhi_d3d12` | Исторический код/данные прежних backend paths. Не начинать изучение нынешнего runtime отсюда. |
| [CMakeLists.txt](../CMakeLists.txt#L277), [CMakePresets.json](../CMakePresets.json) | Какие cpp действительно компилируются, какие библиотеки линкуются, где build directories/configurations. |
| [tools/shader_compiler/CMakeLists.txt](../tools/shader_compiler/CMakeLists.txt) | Отдельный dxc build tool; не путать с runtime Diligent CreateShader. |

`.h` обычно описывает контракт и данные; `.cpp` — реализацию. Templates World/ResourceManager/parallelFor находятся в `.h`, потому что компилятор должен видеть их определения при создании конкретного `T`. Часть простых wrappers тоже реализована непосредственно в header.

## 3. От main до закрытия приложения

### Кто владеет чем

В [Application.h](../engine/core/Application.h#L66) лежат `m_jobs`, `m_window`, `m_renderer`, `m_stateStack`. Это главные долгоживущие владельцы.

```text
Application
├─ JobSystem                         общий scheduler CPU
├─ unique_ptr<Window>                окно
├─ Renderer                          графика + ресурсы + asset queue
└─ StateStack
   └─ unique_ptr<IGameState>...
      └─ GameplayState
         ├─ World                    компоненты сцены
         ├─ Physics/Render/Debug      обработчики данных World
         └─ EventBus                 collision notifications
```

StateStack передаёт jobs/window/options по указателям, но не становится владельцем этих сервисов. `unique_ptr` означает одного владельца; сырой указатель здесь — заимствованный доступ. При объявлении членов класса они создаются сверху вниз и уничтожаются снизу вверх. Поэтому JobSystem объявлен раньше потребителей.

Уже при создании `Application` конструируется `m_jobs` и запускаются workers; это происходит до `Application::init()` и настройки логгера.

### init

Читать [Application::init](../engine/core/Application.cpp#L236):

1. Включить Log; прочитать `config.json` относительно working directory. Если файла нет, используются defaults.
2. Прочитать `LabOptions`; передать один и тот же JobSystem StateStack и Renderer.
3. Включить выбранный async mode/upload budget. Для профилируемой сборки или lab scene зафиксировать 1280×720, VSync off, gameplay.
4. Создать окно, сбросить Input, подключить `onEvent` callback.
5. Renderer создаёт device, swapchain, fallback resources и ImGui.
6. Поставить initial state в стек; применить pending changes, чтобы вызвался его `onEnter()`.
7. Разрешить главный цикл.

Backend движка — Diligent. Поле `diligentDevice` в конфиге выбирает **API внутри Diligent**, например D3D12. Значения `ENGINE_BACKEND` и `diligentDevice` отвечают на разные вопросы.

### shutdown

Читать [Application::shutdown](../engine/core/Application.cpp#L406):

```text
States.onExit + clear
  → сброс tutorial resource references
  → Renderer.shutdown: stop/join asset work, уничтожить GPU objects
  → JobSystem.shutdown: drain оставшейся CPU-работы, остановить workers
  → уничтожить Window
  → выключить Log
```

Почему нельзя просто сначала удалить Renderer: в заданиях загрузки есть callbacks, захватывающие `[this]`. Пока такая работа жива, её владелец тоже должен быть жив. Поэтому очередь сначала перестаёт принимать задания, дожидается выданных CPU jobs и удаляет неисполненные GPU finalizers.

Деструктор Application также вызывает shutdown; повторный shutdown защищён guard. Но повторный `init()` **того же объекта** после полного shutdown не является поддержанным restart: scheduler не создаётся заново. Повторные прогоны лабораторной запускают новый процесс.

## 4. Один кадр: update не равен render

Главная функция проекта — [Application::run](../engine/core/Application.cpp#L312).

```text
Внешний цикл: один визуальный кадр
    │
    ├─ Input.beginFrame → Window.pollEvents → hot reload
    ├─ frameDt = timer.tickSeconds, clamp до 0.25 s
    ├─ accumulator += frameDt
    │    └─ пока accumulator >= 1/60 s:
    │         StateStack.update(1/60)
    │         applyPendingChanges
    │         accumulator -= 1/60
    ├─ Renderer.beginFrame       ← здесь main-thread asset pump
    ├─ beginImGuiFrame
    ├─ renderWorkspace          ← сцена или встроенный tutorial
    ├─ renderWorkspaceUi
    ├─ renderImGui → endFrame/Present
    └─ JobSystem metrics → Tracy FrameMark
```

**Update** меняет состояние мира: позиции, скорость, игровую логику. **Render** читает это состояние и отправляет команды рисунка. Смешивание этих фаз осложнило бы и многопоточность, и воспроизводимые замеры.

Шаг симуляции — 1/60 секунды. Но это не ограничение FPS:

- Кадр 10 ms: накопленного времени может не хватить, update будет 0.
- Кадр около 33 ms: обычно два update по 16.67 ms.
- Render вызывается один раз за внешний цикл независимо от количества update.
- Clamp 250 ms ограничивает длинный catch-up примерно 15 шагами, а не даёт бесконечно догонять секунды простоя.

Оставшийся accumulator сейчас не используется для интерполяции изображения. Поэтому fixed simulation и smooth interpolated rendering — не одно и то же; второе здесь отдельно не реализовано.

Для чтения ставь ориентиры: [события](../engine/core/Application.cpp#L350), [fixed update](../engine/core/Application.cpp#L362), [render](../engine/core/Application.cpp#L374), [FrameMark](../engine/core/Application.cpp#L399).

`TimeStatsAggregator` пишет avg/min/max примерно раз в секунду. Он получает уже clamped `frameDt`, поэтому его лог не заменяет Tracy для длинных stalls и percentile-анализа.

## 5. Окно, ввод и состояния

### Как клавиша доходит до сцены

```text
ОС → GLFW callback → platform::Event → Application::onEvent
                                      ├─ InputManager
                                      ├─ Renderer/ImGui
                                      └─ StateStack → верхний state
```

[WindowGLFW.cpp](../engine/platform/WindowGLFW.cpp#L173) переводит GLFW key codes в engine KeyCode. `glfwPollEvents()` вызывается на main; callbacks доставляются в том же потоке, не через JobSystem.

[Application::onEvent](../engine/core/Application.cpp#L807) сначала обновляет Input и ImGui; Quit завершает цикл, Resize меняет размер рендера. Если UI забрал событие, оно обычно не должно управлять сценой. Для RMB camera-look предусмотрен специальный override.

`isKeyDown` — клавиша удерживается. `wasKeyPressed` — current=true, previous=false, то есть переход нажатия в этом визуальном кадре. `beginFrame` сохраняет previous states и очищает deltas. Raw-методы обходят ImGui capture: камера использует их, когда viewport разрешил управление. Raw не означает «потокобезопасный».

Не путай два вида событий:

- `platform::Event` — окно/клавиатура/мышь.
- `core::EventBus` — сообщения внутри движка, например CollisionEnterEvent.

EventBus не содержит фоновой очереди. [publish](../engine/core/EventBus.h#L38) сразу вызывает подписчиков на текущем потоке. У него нет thread-safe subscribe/publish и unsubscribe token; очистку делает владелец.

### StateStack

`IGameState` задаёт onEnter/onExit/handleEvent/update/render. Дополнительный `IGameStateUi` в том же header задаёт UI render.

[StateStack](../engine/game/StateStack.cpp#L24) работает так:

| Операция | Какие states затрагивает |
|---|---|
| handleEvent, update | Только верхний |
| render, renderUi | Все снизу вверх |
| push/pop/replace/clear | Пока только добавляет запрос в pending list |
| applyPendingChanges | Реально изменяет стек, вызывает enter/exit |

Отложенное применение не позволяет удалить state прямо посреди его собственного метода. PauseState поверх GameplayState оставляет нижнюю сцену нарисованной, но не вызывает её update.

Важное уточнение: обычный Escape в текущем GameplayState не делает `push(PauseState)`. Он освобождает mouse-look или возвращает Play→Edit. PauseState существует как отдельный путь, включая initial state. Поэтому комментарий «Esc всегда открывает PauseState» для текущей версии неверен.

Application workspace — ещё один уровень: можно показывать Engine States, встроенные Diligent tutorials, ImGui demo или samples hub. Это не entities World и не игровые states; это выбор того, что Application рисует.

## 6. ECS: как представлен объект

ECS = Entity–Component–System:

- Entity: номер объекта.
- Component: данные, относящиеся к нему.
- System: код, который обрабатывает подходящие данные.

Нет обязательного `GameObject::update()` для каждого объекта. Например, камера — entity с Transform+Camera; рисуемый физический ящик — entity с Transform+MeshRenderer+Rigidbody+Collider.

Упрощённый пример использования существующего API:

```cpp
engine::ecs::World world;
const auto id = world.createEntity();

engine::ecs::Transform transform;
transform.position = glm::vec3(0.0F, 2.0F, 0.0F);
world.addComponent<engine::ecs::Transform>(id, transform);
world.addComponent<engine::ecs::Tag>(id, engine::ecs::Tag{"Box"});

world.forEach<engine::ecs::Transform, engine::ecs::Tag>(
    [](engine::ecs::EntityId entity, engine::ecs::Transform& t,
       engine::ecs::Tag& tag) {
        // Здесь доступны только объекты, имеющие оба компонента.
        // entity — ID, t и tag — ссылки на реальные данные World.
    });
```

Этот пример лишь создаёт данные: без MeshRenderer объект не нарисуется, без Rigidbody/Collider не получит нужного физического поведения.

### Что находится внутри World

В [world.h](../engine/ecs/world.h#L67) каждый тип имеет отдельный `unordered_map<EntityId,T>`. Ещё одна карта по `type_index` хранит эти typed storages. Это простая hash-map ECS, не archetype ECS и не плотно упакованное SoA.

`forEach<Transform,MeshRenderer>` начинает обход storage **первого** типа, проверяет alive и остальные компоненты, затем вызывает callback со ссылками. Он не выбирает автоматически самое маленькое хранилище. Порядок `unordered_map` не гарантирован.

Во время такого обхода или параллельной фазы нельзя произвольно удалять объекты/компоненты. World не защищён общим mutex. `const World&` запрещает изменения через эту ссылку, но не блокирует изменения другим потоком.

`createEntity` использует свободные IDs либо увеличивает список записей. `destroyEntity` удаляет компоненты, отмечает alive=false и кладёт ID в free list.

**Ограничение ID:** generation в записи увеличивается, но не упакован в публичный uint32 ID. Старый ID после повторного использования может обозначать новый объект. Это не безопасный generational handle. `addComponent` тоже не проверяет alive автоматически: валидность — обязанность вызывающего кода.

### Компоненты

| Component | Значение ключевых полей |
|---|---|
| Transform | position — локальный перенос; rotationEulerRadians — углы в радианах; scale — масштаб. |
| Tag | Читаемое имя; само по себе не меняет render/physics. |
| Hierarchy | parent ID; дети вычисляются поиском, отдельного списка детей нет. |
| Camera | verticalFovRadians, near/far clipping planes, active. |
| MeshRenderer | Строковые resource IDs, runtime shared_ptr, tint, UV scale, visible. |
| Rigidbody | velocity/angularVelocity, force/torque, mass/inverseMass, gravity, damping, friction, restitution, static/kinematic/sleep. |
| Collider | AABB или Sphere, offset, размеры, enabled; не рисунок mesh. |

Mesh определяет внешнюю форму для GPU. Collider определяет упрощённую форму столкновений. Они не обязаны совпадать: визуальная пирамида здесь может сталкиваться как AABB.

`applyForce/applyTorque` накапливают воздействие и будят Rigidbody. `inverseMass=1/mass` удобна solver; у static/kinematic она нулевая. Kinematic здесь не означает автоматически движущийся scripted body: integrator сам такое тело не перемещает.

## 7. Координаты, иерархия и камера

[Transform::toMatrix](../engine/ecs/components.h#L26) строит `T × Rx × Ry × Rz × S`. При применении к column vector операция справа выполняется первой: scale, вращения, перенос. В инспекторе удобно показывать градусы, но данные Transform хранят радианы.

Локальная матрица ребёнка — относительно родителя. [computeWorldMatrix](../engine/ecs/transform_utils.cpp#L13) поднимается по Hierarchy и домножает родительские матрицы слева:

```text
World(child) = World(parent) × Local(child)
```

Родитель X=10 и ребёнок local X=2 дают world X=12, если нет вращения/scale. Без Transform функция возвращает identity. Лимит 64 родителей и self-parent check предотвращают бесконечный обход, но не являются полной проверкой произвольного графа. Редактор отдельно валидирует нового родителя.

Путь вершины в шейдер:

```text
vertex local → model/world matrix → view matrix → projection → clip space
MVP = Projection × View × Model
```

Model — положение объекта. View — мир относительно камеры. Projection — перспектива: FOV, aspect ratio, near/far. Это три разных преобразования, не «матрица камеры плюс позиция».

[buildActiveCameraFrame](../engine/game/GameplayState.cpp#L3005) использует `lookAtRH` и `perspectiveRH_ZO`. ZO означает диапазон depth 0…1. Текущий camera path читает локальный Transform, не world matrix иерархии; parented camera пока не полностью поддержана. Если несколько Camera active, выбирается первая найденная, а unordered iteration не обещает конкретного победителя.

GameplayState включает mouse-look по RMB и меняет speed settings колесом. [FreeCameraController](../engine/game/FreeCameraController.cpp#L25) получает эти flags/settings и рассчитывает движение: WASD — горизонтально, QE — вертикально, Shift — ускорение; mouse delta — поворот. Нормализация movement предотвращает более быстрое диагональное движение; clamp pitch/delta защищает от переворота и резких скачков.

## 8. От объекта ECS до draw call

Главный вход — [RenderSystem::render](../engine/ecs/render_system.cpp#L14). Здесь три фазы, и они важнее отдельных строчек:

### 1. Gather — главный поток

Перебрать Transform+MeshRenderer, пропустить `visible=false`, запросить отсутствующие runtime pointers по resource IDs. Собрать переиспользуемые массивы `m_entities` и `m_renderers`.

`visible` — ручной флаг. Это **не frustum culling**: код не отбрасывает автоматически объект вне камеры.

### 2. Prepare — независимая CPU-работа

[prepareWorldMatrices](../engine/ecs/transform_batch.cpp#L10) получает стабильный список IDs и массив matrices той же длины. Каждый индекс независимо вычисляет `computeWorldMatrix(world,entities[index])`.

Условия:

```cpp
if (jobs != nullptr && entities.size() >= 1024U) {
    jobs->parallelFor(entities.size(), 256U, range);
} else {
    range(0U, entities.size());
}
```

256 — минимальная порция элементов, **не число потоков**. enkiTS разбивает task set на ranges; их количество не обязано быть ровно `count/256`.

Почему это безопасно:

- World и Transform/Hierarchy не меняются до окончания фазы.
- Каждый range пишет только свои `matrices[begin…end)`.
- Векторы заранее имеют нужный размер; workers не вызывают push_back/resize.
- `parallelFor` возвращается только после завершения: это барьер перед submit.

«Snapshot» здесь означает стабильный список IDs/указателей на фазу, а не копию всего World. Проверка размеров input/output защищает от неправильного batch.

### 3. Submit — главный поток

[Render Submit](../engine/ecs/render_system.cpp#L49) берёт matrix и MeshRenderer одного индекса, вызывает drawMesh либо primitive fallback. GPU-вызовы не перенесены на workers.

Это выбранная задача №2 ЛР: ускорение существующей CPU-подготовки сцены. Оно не уменьшает draw calls и не устраняет стоимость driver submission. Поэтому ускорение Prepare в 3 раза не означает ускорение всего кадра в 3 раза.

[Тест](../tests/TransformBatchTests.cpp#L12) сравнивает serial/parallel/reference для 4096 объектов с rotation, nonuniform scale и hierarchy, а также empty batch и ошибочные размеры.

## 9. Job System: API и внутреннее устройство

### Начинать с API, не с атомиков

[JobSystem.h](../engine/core/JobSystem.h#L15) отделяет движок от enkiTS через Pimpl: в header виден `Impl`, конкретная реализация находится в cpp. Один Application содержит один общий scheduler.

| Метод | Что делает |
|---|---|
| dispatch(fn,priority) | Ставит одну CPU-функцию, возвращает Handle. |
| dispatchAfter(handles,fn,priority) | Разрешает fn после предшественников. |
| isComplete(handle) | Только опрос; не выполняет работу. |
| wait(handle) | Дожидается, может помогать scheduler, затем перекидывает сохранённую ошибку. |
| parallelFor(count,grain,fn) | Выполняет ranges `[begin,end)` и ждёт все до возврата. |
| waitAll() | Дренирует все jobs; scheduler остаётся включённым. |
| shutdown() | Дренирует jobs и останавливает workers. |

Handle — ссылка на состояние конкретной работы, не thread ID и не ресурс GPU. `valid()` не означает completed. Пустой handle можно считать завершённым; чужой scheduler handle отклоняется.

Упрощённый пример. Захваченные векторы живут до `wait`, две первые задачи изменяют разные данные:

```cpp
using Jobs = engine::core::JobSystem;
Jobs jobs;
std::vector<int> left(1000), right(1000);
int result = 0;

auto a = jobs.dispatch([&] { std::fill(left.begin(), left.end(), 1); });
auto b = jobs.dispatch([&] { std::fill(right.begin(), right.end(), 2); });
auto c = jobs.dispatchAfter({a, b}, [&] { result = left[0] + right[0]; });
jobs.wait(c); // теперь result == 3 и данные можно использовать
```

```text
A: заполнить left  ─┐
                   ├─ C: объединить → wait(C) → использовать результат
B: заполнить right ─┘
```

Не захватывай локальную переменную по ссылке в fire-and-forget job, если функция вернётся до завершения. Lifetime задачи и lifetime её данных — разные вещи.

### Worker count и приоритеты

Default workers — до четырёх background threads, обычно `min(hardware_concurrency−1,4)`, минимум один. Явно переданный workerCount может быть другим. Главный поток тоже участвует в cooperative wait; это не «движок всегда использует ровно 4 потока», поскольку есть main, Tracy, driver и библиотечные threads.

High/Normal/Low — очереди приоритетов, не OS thread priorities. Frame-critical transforms — Normal; asset decode — Low. Уже запущенный Low job не прерывается появлением High: scheduler не preemptive.

`wait(Normal)` допускает помощь Normal/High, но не подхватывает unrelated Low decode. Это защищает кадр от выполнения тяжёлого IO во время ожидания матриц. Однако библиотечная постановка задачи при переполнении pipe может выполнять работу inline; asset backpressure снижает этот риск, а не даёт абсолютного обещания «dispatch никогда не исполняет job на caller».

### Как читать JobSystem.cpp

| Место | Смысл |
|---|---|
| [JobState](../engine/core/JobSystem.cpp#L30) | Native task set + callable, dependencies, counters, exception, mutex. |
| [ExecutionScope](../engine/core/JobSystem.cpp#L48) | Thread-local стек выполняющихся jobs; checks owner и self/ancestor waits. |
| [requireThread](../engine/core/JobSystem.cpp#L88) | Dispatch/wait разрешены creator либо own scheduler job, не произвольным std::thread. |
| [complete](../engine/core/JobSystem.cpp#L106) | И engine bodyDone, и native GetIsComplete должны быть true. |
| [submit](../engine/core/JobSystem.cpp#L115) | Создать state, проверить prerequisites, зарегистрировать continuations, поставить ready job. |
| [await](../engine/core/JobSystem.cpp#L182) | Сначала ждать predecessors, затем cooperative native wait. |
| [releasePrerequisite](../engine/core/JobSystem.cpp#L212) | Последнее условие запуска переводит job в scheduler. |
| [ExecuteRange](../engine/core/JobSystem.cpp#L216) | Выполнить range, сохранить исключение, завершить body и уведомить детей. |
| [stats](../engine/core/JobSystem.cpp#L301) | Диагностические atomic counters и Tracy plots. |

**Почему task state остаётся жив:** native enkiTS хранит указатель на задачу. Active registry удерживает shared_ptr даже при выброшенном пользовательском Handle. Продолжения хранят weak_ptr, чтобы не создать обратные циклы владения.

**Почему dependencies сложные:** предшественник может уже выполняться к моменту `dispatchAfter`. Wrapper регистрирует свои synchronized continuations. Mutexes предшественников берутся в одном порядке, чтобы избежать взаимной блокировки регистрации. Счётчик условий равен `N+1`: дополнительная единица — registration gate. Пока настройка связей не закончена, даже завершившийся последний predecessor не запустит недонастроенного ребёнка.

**Почему два completion flags:** native task, ещё не поставленный в очередь, может выглядеть idle. Поэтому одного GetIsComplete недостаточно. И наоборот: callback уже отметил bodyDone, но enkiTS ещё не вышел из него — удалять state рано. Полное завершение требует обоих признаков.

**Ошибки:** exception не вылетает за границу worker callback; первая сохраняется и перекидывается через `wait(handle)`. Ошибка predecessor не даёт вызвать пользовательскую функцию зависимого child. Ошибка одного range не отменяет автоматически остальные ranges. `waitAll/shutdown` не сообщают каждую ошибку fire-and-forget: это drain, не result API.

**Ограничения:** waitAll/shutdown — только idle owner thread. Проверяются ожидание себя и executing ancestor, но произвольные циклы графа не анализируются. Нельзя менять структуру World и трогать GPU из jobs. Это contract и архитектура фаз, не универсальный автоматический read/write scheduler.

Wrapper не полностью lock-free: enkiTS даёт work stealing/priority machinery, а engine использует mutex для dependencies/lifetime. Здесь нет собственной fiber system.

### Как понимать counters

submitted/completed — task sets, не число ECS objects. executedRanges — реально исполненные partitions. pending — submitted минус completed. queueDepth — task sets, у которых ещё не стартовал первый range, включая ожидающие dependencies. running включает nested callers, которые помогают scheduler во время ожидания. Snapshot приблизительно согласован: counters читаются отдельно, без глобального transactional lock.

## 10. Ресурсы: от PNG и OBJ до GPU

Это обязательная задача №1 ЛР: CPU load/decode на jobs, GPU finalize на main. Само существование PNG на диске ещё не означает наличие пригодной GPU texture.

### Четыре разных «указателя/ID»

| Сущность | Что означает |
|---|---|
| `textureId`, `meshId`, `shaderId` | Строка пути либо procedural ID; сохраняется в JSON. |
| `shared_ptr<Texture/Mesh/ShaderProgram>` | Владение runtime ресурсом. Он может быть ещё Loading. |
| `weak_ptr` в ResourceManager | Кеш не продлевает жизнь ресурса; можно вернуть существующий shared_ptr. |
| `unique_ptr<rhi::IImage/IBuffer>` | Единоличное владение GPU wrapper внутри ресурса. |

JobSystem::Handle и rhi::ResourceHandle — ещё две совершенно другие сущности. ResourceHandle у текущей Diligent-обёртки выводится из native object pointer; это не path и не кеш-index.

### Проследи одну texture

1. RenderSystem видит непустой `textureId`, но пустой pointer; вызывает RenderAdapter→Renderer.loadTexture.
2. [ResourceManager::load](../engine/resources/resource_manager.h#L61) ищет пару **тип+path**. При живом weak cache hit возвращает тот же ресурс, включая Loading.
3. При miss зарегистрированный loader [создаёт Texture](../engine/renderer/Renderer.cpp#L772), задаёт Loading и сразу возвращает shared_ptr.
4. AsyncLoadQueue получает Work. Worker вызывает [loadTextureDataRgba8](../engine/resources/loaders.cpp#L163): stb_image читает файл, декодирует четыре RGBA канала в vector.
5. Work возвращает Finalize lambda, захватив CPU data через move. GPU ещё не затронут.
6. Worker записывает callback/ошибку в Completion и публикует `ready.store(true,release)`.
7. [Renderer::beginFrame](../engine/renderer/Renderer.cpp#L1038) вызывает main-thread pump. Он читает ready с acquire и запускает Finalize.
8. [Finalize](../engine/renderer/Renderer.cpp#L783) переносит TextureData в тот же Texture, создаёт IImage и отмечает Loaded.
9. Следующий draw использует готовую texture. У ECS всё это время тот же shared_ptr: указатель не надо вручную подменять после загрузки.

```text
MAIN: запрос → Texture{Loading} → placeholder рисунок
                     │
WORKER:              └─ file read/decode → CPU pixels → ready.release
                                                            │
MAIN, beginFrame:       pump ← ready.acquire ←───────────────┘
                           → createImage → Texture{Loaded} → настоящий рисунок
```

Release/acquire — механизм видимости worker payload на main. Relaxed atomic `ResourceLoadState` сам по себе не публикует весь pixel vector. Worker не меняет GPU pointers напрямую.

### OBJ

[loadMeshData](../engine/resources/loaders.cpp#L99) читает модель через Assimp. Import triangulates, строит normals, объединяет vertices и pre-transforms nodes. MeshData содержит position/normal/uv и uint32 indices. При объединении aiMesh indices сдвигаются на baseVertex общего vertex vector.

После CPU-фазы [uploadMeshToGpu](../engine/renderer/Renderer.cpp#L741) создаёт vertex/index buffers на main. Только затем Mesh становится Loaded. Procedural IDs вроде `__diligent_cube__` идут отдельным путём, без чтения OBJ.

### Loading, Failed и fallback

[Renderer::drawMesh](../engine/renderer/Renderer.cpp#L1110) проверяет isReady:

- Mesh не готов → placeholder mesh.
- Texture не готова/отсутствует → белая 1×1 texture, tint всё ещё работает.
- Shader не готов → fallback shader.

`MeshRenderer::hasGpuResources()` проверяет только ненулевые shared_ptr, а не Loaded. Не используй его как доказательство успешного upload.

При ошибке выставляются Failed и lastError; рисунок продолжает использовать fallback. Автоматической новой попытки каждый кадр нет. Сброс pointer одного компонента не гарантирует новый decode: пока другой владелец держит тот же Failed-ресурс, weak cache вернёт его снова. Новая загрузка начнётся при cache miss (например, после освобождения всех владельцев), при новом path либо через отдельно реализованный механизм reload. Существующий shader hot reload — отдельный специализированный путь.

### AsyncLoadQueue: зачем pump и backpressure

[Контракт](../engine/resources/async_load_queue.h#L30): `Work = function<Finalize()>`, `Finalize = function<bool()>`. Work — CPU; Finalize и error callbacks — owner/main thread.

Default pump — одна попытка finalization за кадр, 2 ms. Время — **мягкий бюджет**: драйверный upload/compile нельзя прервать; проверка делается после него и предотвращает начало следующего. Поэтому один upload может занять >2 ms. `uploadedLastFrame` считает попытки, включая failed attempts.

Pump может взять любую ready запись: медленная первая загрузка не блокирует все следующие. Невыполненный backlog остаётся в main-owned waiting queue.

[dispatchPending](../engine/resources/async_load_queue.cpp#L119) ограничивает выданную работу + готовые staging entries значением `clamp(2×workers,1,16)`. При четырёх workers это восемь. Пока main не освободит completion, новая CPU load не выдаётся. Это backpressure, а не бесконечное декодирование всех файлов вперёд.

Лимит — **по количеству**, не байтам. Один большой asset всё ещё может занимать много памяти. Кроме того, текущие Mesh/Texture сохраняют CPU data после upload; общий RAM всех готовых ресурсов этим лимитом не ограничен.

Load/pump/stop — owner-thread operations. Mutex ResourceManager защищает maps, но не делает весь Renderer thread-safe. Cache loader вызывается вне lock, поэтому это не полноценная атомарная single-flight дедупликация двух одновременных miss. Обычный путь избегает этого, делая запросы на main.

[stop](../engine/resources/async_load_queue.cpp#L219) ждёт выданные CPU jobs, удаляет backlog/неисполненные finalizers и сообщает cancellation. Запущенный Assimp/stb read не прерывается посередине. Renderer вызывает stop до уничтожения GPU/device.

Sync mode — явный эксперимент: CPU и Finalize выполняются сразу на main, pump budget не действует. Это не описание старого loader: до общей JobSystem у него уже был свой async pool.

### Шейдеры и hot reload

Worker shader path сейчас только разбирает manifest/path descriptor. Реальное чтение HLSL, compile и pipeline creation идут в [main-thread Finalize](../engine/renderer/Renderer.cpp#L899) через `createShaderProgram` и Diligent CreateShader. Нельзя утверждать «вся компиляция shader вынесена в фон».

[pollHotReload](../engine/renderer/Renderer.cpp#L1334) проверяет timestamps с debounce. [reloadShaderInPlace](../engine/renderer/Renderer.cpp#L1316) сначала успешно создаёт новый pipeline и только потом заменяет GPU objects старого ShaderProgram. ECS shared_ptr остаётся тем же; при compile error старый рабочий shader сохраняется.

## 11. Renderer, RHI и Diligent

Renderer отвечает на вопрос «как нарисовать mesh с matrix/tint/texture». RHI — Render Hardware Interface — отвечает «как создать buffer/pipeline и выполнить draw». DiligentDevice переводит интерфейсные вызовы в реальные Diligent API.

### Что делает один draw

[Renderer::drawMesh](../engine/renderer/Renderer.cpp#L1110):

1. Выбрать готовые/fallback ресурсы.
2. Bind pipeline, vertex buffer, texture slot0.
3. Собрать constants: `viewProjection×model`, model, uvScale, tint.
4. При наличии indices вызвать drawIndexed, иначе draw.

[DiligentCommandBuffer](../engine/rhi_diligent/DiligentDevice.cpp#L574) перед draw обновляет dynamic uniform buffers через WRITE/DISCARD, привязывает texture SRV и CommitShaderResources, затем вызывает immediateContext Draw/DrawIndexed.

В интерфейсе это названо `pushConstants`, но текущая Diligent-реализация передаёт их через **uniform/constant buffers**, не универсальные native Vulkan push constants или DX root constants.

Pipeline описывает shader stages, vertex layout, triangle topology, color/depth formats, culling/depth, texture binding и sampler. Это «правила обработки draw», а не сам объект ECS.

В нынешнем адаптере vertex layout выводится из HLSL текста, а texture support частично определяется по именам shader paths с `texture/textured`. При добавлении shader учитывать эти соглашения, а не ожидать универсального shader reflection.

[beginFrame](../engine/renderer/Renderer.cpp#L1038) делает pump, начало command wrapper/pass и clear/viewport/scissor; [endFrame](../engine/renderer/Renderer.cpp#L1463) — submit и Present.

### Что действительно активно

`engine/rhi_diligent` — единственный active backend движка. Diligent может выбирать D3D12/D3D11/Vulkan/OpenGL/WebGPU при наличии соответствующей сборки. Windows auto предпочитает D3D12, если он доступен. Старые `rhi_*` не участвуют в данном app path, несмотря на наличие файлов.

### Что в RHI пока упрощено

Это особенно важно для честного понимания интерфейсов:

- [DiligentFence::wait](../engine/rhi_diligent/DiligentDevice.cpp#L195) меняет CPU bool, не ждёт завершения GPU.
- Semaphore — пустая wrapper; acquireNextImage возвращает 0.
- CommandBuffer использует immediateContext, не записывает отдельный native command list.
- [Queue::submit](../engine/rhi_diligent/DiligentDevice.cpp#L780) делает Flush и signal CPU fence; не реализует собственную полноценную очередь GPU synchronization.
- RHI barrier/bindBindGroup сейчас no-op; resource transitions выполняет Diligent через свои TRANSITION calls.

Настоящее backend управление graphics resources/Present находится в Diligent. Два FrameContext в Renderer сами по себе не доказывают native frames-in-flight/fence implementation. Это учебная адаптация immediate-context rendering, а не готовый render graph/parallel command recording.

В Tracy здесь CPU zones. Зона с названием `GPU Submit` измеряет CPU-вызов submit, а не время shader execution на GPU; GPU timing instrumentation отдельно не подключена.

## 12. Как работает физика

Главный вход — [PhysicsSystem::update](../engine/ecs/physics_system.cpp#L519). Application уже передал фиксированный dt; собственного accumulator внутри системы нет.

| Фаза | Что происходит | Поток |
|---|---|---|
| Integrate Bodies | Собрать Transform/Rigidbody pointers, обновить каждый body | Gather main; ranges jobs по 64 |
| Build Body Proxies | World collider + bounds для каждого body | Gather main; disjoint output ranges jobs по 64 |
| Broadphase | Найти потенциальные соседние пары в spatial grid | Main |
| Narrowphase + Solver | Проверить контакт и изменить участвующие тела | Main |
| Sleep Update | Обновить состояние покоя | Main |
| Collision Events | Сравнить прошлые и новые контакты | Main |

Порядок фаз и barriers не дают solver работать с недописанной интеграцией.

### Интеграция

[integrateBody](../engine/ecs/physics_system.cpp#L218) исправляет invalid values, пересчитывает inverseMass, пропускает static/kinematic и sleeping без новых сил. Смысл шага:

```text
accelerationTotal = acceleration + gravity + force × inverseMass
velocity += accelerationTotal × dt
position += velocity × dt
```

Добавлены damping и speed limits, угловая скорость через inverse inertia, обновление rotation; accumulated force/torque после шага сбрасываются. Сначала скорость, потом положение — semi-implicit Euler.

### Broadphase и narrowphase

[Broadphase](../engine/ecs/physics_system.cpp#L398) делит пространство на ячейки (default 2.6), помещает world bounds в пересекаемые ячейки, собирает пары, убирает дубликаты и пары двух неподвижных тел. Это уменьшает число дорогих точных проверок; при invalid cell size есть all-pairs fallback.

[collision_utils](../engine/ecs/collision_utils.cpp#L212) поддерживает AABB–AABB, Sphere–Sphere и AABB–Sphere. Контакт содержит normal/penetration/contact point.

Вращённый local box становится охватывающим **world AABB**, а не точным oriented box. Сфера при nonuniform scale получает максимальный scale как охватывающий radius. Это простые геометрические приближения, не mesh collision.

### Solver

[resolveCollision](../engine/ecs/physics_system.cpp#L310) исправляет penetration с учётом inverseMass, считает normal impulse/отскок и tangent impulse/трение, изменяет linear/angular velocities. Default — четыре прохода solver.

Почему не parallelFor по всем контактам: пары A–B и A–C одновременно записали бы один Rigidbody A. Независимость per-body integration не означает независимость pair solver. Для такого распараллеливания нужны дополнительное разбиение конфликтов/островов и другая схема; её сейчас нет.

Sleep уменьшает бессмысленную работу с почти неподвижными телами. Enter/Stay/Exit получаются сравнением карт контактов предыдущего и нынешнего шага; GameplayState подписывается и обновляет diagnostics.

Collider не имеет isTrigger. Эти события относятся к обычным физическим контактам с solver, а не готовой trigger-only системе.

Ограничения: простая собственная физика, не Jolt/Bullet/PhysX; нет CCD, capsule, точного OBB/mesh collider и полноценного manifold. Parent hierarchy динамических тел нельзя считать полноценно поддержанной: shape использует world matrix, а integrator/solver меняют Transform.position как обычную мировую позицию. Перемещение solver может создать новую broadphase пару, которая будет найдена только в следующем update.

## 13. Редактор и большой GameplayState

В GameplayState.cpp около пяти тысяч строк: сюда собраны сцена, editor, JSON, picking, layout, gizmo и lab UI. Для первого чтения **не нужно понимать весь файл подряд**.

### Основной каркас

- [Конструктор](../engine/game/GameplayState.cpp#L1101): передать общий jobs в RenderSystem/PhysicsSystem либо nullptr для serial ECS.
- [onEnter](../engine/game/GameplayState.cpp#L1112): editor layout, collision handlers, reset scene.
- [onExit](../engine/game/GameplayState.cpp#L1123): очистить World, history, handlers, input mode.
- [createDemoScene](../engine/game/GameplayState.cpp#L1353): camera, floor/walls, boxes, pyramids, sphere, striker.
- [update](../engine/game/GameplayState.cpp#L2955): lab trigger либо camera; в Play — pending spawns и PhysicsSystem.
- [render](../engine/game/GameplayState.cpp#L3187): camera matrices, RenderSystem, collider debug, selection outline.
- [renderUi](../engine/game/GameplayState.cpp#L5044): editor UI или lab UI.

Обычная стартовая сцена создаётся кодом. `assets/scenes/editor_scene.json` не подхватывается автоматически при onEnter: его загружает команда Load.

В profiling build без выбранной lab scene есть отдельная большая physics demo и auto Play. В `TGE_LAB_SCENE` используется иной фиксированный сценарий, а не эта прежняя нагрузка.

### Edit, Play и snapshots

[enterPlayMode](../engine/game/GameplayState.cpp#L1756) сохраняет edit scene snapshot и запускает simulation. [stopPlayMode](../engine/game/GameplayState.cpp#L1766) восстанавливает его. Объекты, упавшие/созданные в Play, после Stop возвращаются к edit-состоянию.

[captureSceneSnapshot](../engine/game/GameplayState.cpp#L1780) копирует известные компоненты в optional fields. [restoreSceneSnapshot](../engine/game/GameplayState.cpp#L1820) clear World → создать новые entities → oldID/newID map → восстановить components и remap parents/camera/selection.

Undo/Redo хранит целые scene snapshots, а не компактные inverse commands. History ограничена 64 entries; drag объединяется в одно редактирование. Snapshot MeshRenderer копирует shared_ptr: история может удерживать ресурсы живыми.

[saveSceneToDisk](../engine/game/GameplayState.cpp#L2020) и [loadSceneFromDisk](../engine/game/GameplayState.cpp#L2090) пишут/читают данные и resource IDs, не native GPU pointers. Некоторые transient physical fields не сохраняются. После Load runtime pointers пусты, Render Gather заново заказывает ресурсы.

Scene JSON и editor layout JSON — разные файлы. При `ENGINE_SOURCE_ROOT` save paths привязаны к исходному проекту, а не только к скопированным assets рядом с exe.

`m_pendingSpawnJobs` — deque обычных main-thread lambdas, **не jobs enkiTS**. [processPendingSpawns](../engine/game/GameplayState.cpp#L1743) исполняет ограниченное число на вызов update. Несколько fixed updates за один render frame могут выполнить несколько таких порций.

### Где искать UI и взаимодействие

| Возможность | Начало функции |
|---|---|
| Create / duplicate / delete entity | [2210](../engine/game/GameplayState.cpp#L2210) / [2296](../engine/game/GameplayState.cpp#L2296) / [2344](../engine/game/GameplayState.cpp#L2344) |
| Undo / Redo | [1991](../engine/game/GameplayState.cpp#L1991) / [2004](../engine/game/GameplayState.cpp#L2004) |
| Keyboard shortcuts | [2402](../engine/game/GameplayState.cpp#L2402) |
| Layout load / save | [2839](../engine/game/GameplayState.cpp#L2839) / [2900](../engine/game/GameplayState.cpp#L2900) |
| Menu bar / toolbar | [3658](../engine/game/GameplayState.cpp#L3658) / [3857](../engine/game/GameplayState.cpp#L3857) |
| Hierarchy / Content Browser | [3987](../engine/game/GameplayState.cpp#L3987) / [4067](../engine/game/GameplayState.cpp#L4067) |
| Component editors / Inspector | [4334](../engine/game/GameplayState.cpp#L4334) / [4576](../engine/game/GameplayState.cpp#L4576) |
| Stats / ImGuizmo | [4671](../engine/game/GameplayState.cpp#L4671) / [4784](../engine/game/GameplayState.cpp#L4784) |
| Viewport / entire editor UI | [4889](../engine/game/GameplayState.cpp#L4889) / [4990](../engine/game/GameplayState.cpp#L4990) |

Content Browser применяет asset, меняя ID и очищая runtime pointer; следующий Render Gather запускает load. Delete отсоединяет прямых детей, не удаляя их; local Transform при этом не пересчитывается для сохранения прежней мировой позиции.

Picking: [buildPickRay](../engine/game/GameplayState.cpp#L3042) переводит mouse position в NDC и через inverse viewProjection строит луч; [pickEntity](../engine/game/GameplayState.cpp#L3086) выбирает ближайшие bounds. Это не точный ray-triangle тест всех импортированных meshes.

ImGuizmo меняет world matrix; для child результат переводится обратно в local через `inverse(parentWorld)×world`. Затем матрица раскладывается на position/rotation/scale и операция попадает в history.

Viewport сейчас — прозрачная ImGui-панель поверх рисунка, не самостоятельный render-to-texture pipeline. DebugRenderSystem рисует collider bounds; изображение sphere debug собирается кольцами из segments и может давать много draw calls.

## 14. ЛР 1 и Tracy: что именно измеряется

Две основные реальные задачи:

1. PNG/OBJ: CPU load/decode через общий JobSystem; GPU finalize на main с bounded pump.
2. RenderSystem: world-matrix preparation на jobs с join до GPU submit.

Физическая integration/proxy preparation также использует этот scheduler, но её не нужно выдавать за выбранную итоговую задачу №2 или смешивать с отдельными ECS/Loading experiments.

### Где lab mode входит в код

[LabOptions.h](../engine/core/LabOptions.h#L41) читает отдельные environment variables, не `.env`.

| Переменная | Действие |
|---|---|
| TGE_LAB_SCENE=ecs/loading | Выбрать фиксированную lab scene. |
| TGE_JOBS=0/1 | Serial/parallel RenderSystem и physics; общий scheduler не уничтожается. |
| TGE_ASYNC_LOADING=0/1 | Sync/async asset load experiment. |
| TGE_ECS_ENTITIES | Число render objects, default 4096; bounds 1…16384. |
| TGE_BENCHMARK_ASSET_DIR | Каталог настоящих fixture PNG/OBJ. |
| TGE_LOAD_AT_SECONDS | Момент смены resource IDs, default 6 s. |
| TGE_DEMO_SECONDS | Автозавершение; default 0 — не ограничивать. |
| TGE_UPLOADS_PER_FRAME / TGE_UPLOAD_BUDGET_MS | Count/time limits pump, default 1 / 2 ms. |
| TGE_WAIT_FOR_TRACY=1 | Ждать подключение перед LAB_RUN_START, timeout 10 s. |
| TGE_MISSING_ASSET=1 | Сценарий ошибки загрузки. |

**TGE_JOBS=0 не означает «всё однопоточно».** Asset queue продолжает пользоваться общим scheduler. Это намеренно: ECS A/B не должен одновременно менять asset mechanism.

[createLabScene](../engine/game/GameplayState.cpp#L1510) делает ECS grid с hierarchy либо 16 loading objects. [requestLabAssets](../engine/game/GameplayState.cpp#L1556) на заданной секунде меняет IDs на 12 PNG + четыре OBJ. [renderLabUi](../engine/game/GameplayState.cpp#L1582) показывает queue/frame counters и отмечает завершение. Lab update не двигает камеру и не шагает физику; hot reload отключён для сравнимости.

### Зона — это время scope

`ENGINE_PROFILE_ZONE("Render Prepare Transforms")` открывает RAII zone, которая закрывается при выходе из C++ scope, а не после следующей строки. Вложенные зоны включены во время родительской. Поэтому нельзя складывать Main Frame + все его children: выйдет двойной счёт.

| Имя в Tracy | Где искать причину |
|---|---|
| Main Frame, Fixed Update, State Update Step, Frame Render | Application::run; Fixed Update охватывает фазу, State Update Step — один fixed step |
| Gameplay Update / Gameplay Render | GameplayState |
| Render Gather / Render Prepare Transforms / Render Submit | RenderSystem + transform_batch |
| Job Render Transforms / Job Execute / Job Wait | Worker work, wrapper и ожидание |
| Asset Decode Job / Asset Main Thread Tasks | AsyncLoadQueue CPU phase / pump |
| Physics Integrate Bodies / Broadphase / Narrowphase and Solver | Фазы PhysicsSystem |
| LAB_RUN_START / LAB_LOAD_START / LAB_LOAD_COMPLETE | Markers, не timed zones |

FrameMark стоит в конце внешнего кадра. Различай интервал frame marks и zone Main Frame: они близки по смыслу, но это разные измеряемые границы. Итоговый analysis script считает выбранные CPU-зоны.

### Как читать сравнение

Сначала одинаковая сцена/камера/input/настройки/бинарник, затем одинаковый range. Для подготовленного протокола: warmup 3 s; основной range `LAB_RUN_START +3…+14 s`; load на +6 s, отдельное loading window `LAB_LOAD_START −1…+5 s`.

Statistics/Find zone отвечают на разные вопросы: mean per call — средняя цена вызова; count — число вызовов; total — накопленная стоимость; median/p95/p99 кадра — типичное время и хвост задержек. Дорогой одиночный вызов не обязательно главная накопленная цена системы. В Find/Compare применять Limit range.

Сравнивать нужно длительность CPU-критического участка/кадра, а не сумму параллельных worker zones: они пересекаются по времени. GPU-bound нельзя доказать лишь названием GPU Submit CPU zone.

Before в loading — специально отключённый async на том же новом коде; исходный loader уже был async. `legacy-pump` отдельно снимает ограничения pump на новом scheduler. Это controlled ablation, не сравнение двух исторических git binaries.

Сохранённые [результаты](../docs/lab1/results.md) относятся к предыдущей серии N=3 на режим. Для понимания важен масштаб: world-matrix mean снизился примерно 2.790→0.925 ms, а median всего кадра 9.851→9.184 ms. Это нормально: ускорили часть кадра, не весь rendering. Loading устраняет длинный синхронный stall, но отдельные uploads всё ещё способны дать spikes. Здесь результаты не переснимались.

## 15. Сборка, инструменты и тесты

### Что собирает CMake

[CMakeLists](../CMakeLists.txt#L277):

| Target | Основное содержимое |
|---|---|
| engine_core | Log, Time, Config, JobSystem; enkiTS private dependency. |
| engine_platform | Window, WindowGLFW, Input; core + GLFW. |
| engine_rhi_diligent | DiligentDevice; native Diligent libs. |
| engine_rhi | RHI factory; active diligent implementation. |
| engine_renderer | Renderer, resources, RenderSystem, transform/batch/collision utilities. |
| engine_game | World, PhysicsSystem, DebugRenderSystem, states, camera, editor helpers. |
| app | main.cpp + Application.cpp, ссылки на перечисленные слои. |

`STATIC` — библиотека для линковки в exe, не отдельный процесс/поток. `PUBLIC` dependencies/includes видны потребителям; `PRIVATE` — деталь данного target. Header-only files могут не перечисляться как cpp source, но их код всё равно используется.

FetchContent подключает enkiTS, Tracy, GLFW, GLM, spdlog, JSON, Assimp, stb и ImGuizmo; Diligent живёт в third_party. GLM — математика; JSON — сериализация/config; Assimp — CPU model import; stb — image decode; ImGui — UI, ImGuizmo — transform handles.

Текущий Assimp build включает OBJ importer; наличие общей библиотеки не означает, что собраны все форматы импорта. Runtime-шейдеры загружаются и компилируются через HLSL/Diligent; отдельный dxc tool не делает автоматически все shaders заранее скомпилированными. Ray tracing path этим не подразумевается.

### Presets

| Preset | Directory | Build configuration |
|---|---|---|
| debug | C:/tge/dbg | Debug |
| release | C:/tge/rel | Release |
| profile | C:/tge/profile | RelWithDebInfo |
| profile-release | C:/tge/profile | Release + Tracy и symbols |

Visual Studio generator multi-config: выбранная configuration определяется build preset/`--config`, не одним CMAKE_BUILD_TYPE. `profile` и `profile-release` используют один build tree с разными configuration directories.

`ENGINE_ENABLE_TRACY=ON` подключает client v0.13.1, ON_DEMAND и ONLY_LOCALHOST, задаёт ENGINE_TRACY_PROFILE целям. В MSVC Release дополнительно включены debug symbols. Option включает instrumentation независимо от того, как называется выбранная build configuration; не считать его автоматически «только RelWithDebInfo».

Команды для самостоятельного повторения из корня проекта; этот walkthrough их не запускает:

```powershell
cmake --preset debug
cmake --build --preset debug --target app

cmake --preset profile-release
cmake --build --preset profile-release --target app engine_job_system_tests engine_async_load_tests engine_transform_batch_tests engine_physics_tests
ctest --test-dir C:/tge/profile -C Release --output-on-failure
```

Обычный debug exe — `C:/tge/dbg/Debug/app.exe`; профильный — `C:/tge/profile/Release/app.exe`. В CLion важно выбрать и exe, и working directory: config/log paths зависят от cwd. CMake копирует assets рядом с app после build; scene editor paths могут использовать source root.

### Инструменты ЛР

| Файл | Назначение |
|---|---|
| [Launch-Lab1Demo.ps1](../tools/lab1/Launch-Lab1Demo.ps1) | Запустить видимое loading/ecs demo, выбрать before/after, длительность/Tracy. |
| [New-BenchmarkAssets.ps1](../tools/lab1/New-BenchmarkAssets.ps1) + [generate-assets.mjs](../tools/lab1/generate-assets.mjs) | Генерация настоящих PNG/OBJ workload и manifest с hashes. |
| [Invoke-Lab1Measurements.ps1](../tools/lab1/Invoke-Lab1Measurements.ps1) | Повторные A/B runs, Tracy capture/export, metadata, вызов анализа. |
| [analyze-traces.mjs](../tools/lab1/analyze-traces.mjs) | CSV→range filtering→per-run metrics→медиана метрик запусков, Markdown/JSON/CSV. |
| [test-analysis.mjs](../tools/lab1/test-analysis.mjs) | Regression checks анализа, не test gameplay engine. |
| [Invoke-Lab1Stress.ps1](../tools/lab1/Invoke-Lab1Stress.ps1) | Missing assets, early live shutdown, long session и repeated starts. |
| [tools/lab1/README.md](../tools/lab1/README.md) | Полные параметры и методика, чтобы не угадывать команды/диапазоны. |

Node.js нужен scripts генерации/анализа, не самому C++ engine runtime. reports содержат результаты, а не исходники реализации.

### Тесты как небольшие примеры кода

| Тест | Что показывает |
|---|---|
| [JobSystemTests.cpp](../tests/JobSystemTests.cpp#L22) | ranges, dispatch/wait, disabled mode, exceptions, nested jobs, dependencies, foreign handles, thread restrictions, priority isolation, shutdown/stress. |
| [AsyncLoadQueueTests.cpp](../tests/AsyncLoadQueueTests.cpp#L30) | Main-thread finalize, count/time budget, errors/missing inputs, live stop/restart, sync baseline, long mixed outcomes. |
| [TransformBatchTests.cpp](../tests/TransformBatchTests.cpp#L12) | 4096 hierarchy matrices: serial/parallel/reference equivalence, bounds checks. |
| [PhysicsSystemTests.cpp](../tests/PhysicsSystemTests.cpp#L79) | Floor collision/events, inverseMass separation, angular crash, sphere rolling, serial/parallel equivalence. |

Прежний комплект содержит 4/4 CTest и 6/6 stress. Для этой документации сделана проверка исходников/ссылок, не новый прогон binaries. Тесты подтверждают конкретные сценарии, не математическое доказательство всей physics/thread safety.

## 16. Маршрут чтения, отладка и места для изменений

### Как читать, чтобы не потеряться

1. **Каркас:** main.cpp → поля Application.h → init/run/shutdown. После этого объясни, кто владеет окном/renderer/jobs.
2. **Вызов сцены:** StateStack update/render → GameplayState update/render. Пойми Edit/Play и fixed step.
3. **Данные:** entity.h → components.h → World create/add/forEach. Проследи один ящик, не весь editor.
4. **Картинка:** RenderSystem → transform_batch → computeWorldMatrix → Renderer.drawMesh → HLSL.
5. **Ресурс:** ResourceManager.load → requestTextureLoadFromDisk → AsyncLoadQueue submit/pump → createImage.
6. **Параллельность:** JobSystem.h и tests → callers → только затем submit/await/ExecuteRange в cpp.
7. **Физика/editor:** читай нужный функциональный кусок, пользуясь таблицей GameplayState.
8. **Замеры:** Profiling.h → lab scene → scripts → results, сверяя zone names с реальными scopes.

На каждом шаге отвечай на четыре вопроса: кто вызвал функцию, какие данные она читает/меняет, на каком потоке, когда заканчивается lifetime данных.

### Полезные breakpoints

| Цель | Где остановиться | Что посмотреть |
|---|---|---|
| Начало жизни | main, Application::init | backend, config, labOptions, window creation. |
| Порядок кадра | Application::run у accumulator/update | frameDt, accumulator, fixedDt; сколько update на render. |
| Появление объекта | GameplayState::spawnDynamicBody / World::createEntity | EntityId и добавленные компоненты. |
| Передача в render | RenderSystem::render / prepareWorldMatrices | entities.size, matrix[index], jobs pointer. |
| CPU load | loaders.cpp stbi/Assimp entry | Thread name, file path, data dimensions/counts. |
| GPU finalize | AsyncLoadQueue::pump / Renderer finalize lambda | Main thread, ready, attempts, GPU pointer/state. |
| Контакт | resolveCollision | inverseMass, normal, penetration, velocities до/после. |
| Завершение | AsyncLoadQueue::stop / Application::shutdown | Outstanding jobs, отсутствие обращения к уничтоженному renderer. |

Для пошаговой отладки удобнее Debug. Оптимизированная Release нужна для замеров: compiler может переставлять/inlining code, а остановка debugger полностью меняет timing. Не принимай trace под breakpoint за performance measurement.

### «Хочу изменить X» — куда идти

| Что изменить | Где и что проверить |
|---|---|
| Стартовое окно/VSync/цвет | Config defaults + config.json; profiling/lab overrides в Application::init. |
| Обычную сцену/добавить box | GameplayState::createDemoScene или spawn helpers. |
| Нагрузку ЛР | createLabScene, LabOptions, fixture generator; не менять её между A/B. |
| Поле объекта | components.h; при необходимости Inspector, snapshot и JSON serializer тоже. |
| Новый component | World templates уже универсальны, но gameplay/editor serialization нужно подключить явно. |
| Новый системный update | Вызвать из GameplayState::update в правильной фазе; компонент сам не вызовется. |
| Скорость/управление камерой | FreeCameraController Settings и viewport input routing. |
| Parent transform | Hierarchy + transform_utils; учитывать циклы и ограничения camera/physics. |
| Цвет/материал | MeshRenderer.tint/textureId, shader; при смене ID сбросить runtime pointer. |
| Shader effect | HLSL + manifest, Renderer constants и Diligent binding/layout conventions. |
| Новый asset format | loaders + CMake Assimp importers, затем queue/finalizer/error path tests. |
| Время upload на кадр | AsyncLoadQueue budget/LabOptions; время мягкое, не interrupt driver call. |
| Дополнительный CPU job | JobSystem API; определить read/write sets, lifetime, зависимости и точку wait. |
| Физические параметры | PhysicsSystem::Settings, Rigidbody fields, solver; проверять PhysicsSystemTests. |
| Панель/сохранение editor | GameplayState render*Panel, snapshot/JSON и layout отдельно. |
| Измерить новый bottleneck | Profiling scope вокруг реальной работы, одинаковая сцена и Limit range. |

Если объект не виден: alive? Transform+MeshRenderer? visible? камера? ID/path? ресурс Loading/Failed/isReady? fallback? scale/позиция? Не начинать сразу с D3D12 source.

Если «jobs ничего не ускоряют»: достаточен ли batch (render threshold 1024), tasks включены, есть ли независимая работа, не дороже ли Gather/Submit/IO, и где именно wait? Наличие нескольких worker lanes само по себе не равно ускорению критического пути.

## 17. Термины и проверка понимания

### Как читать C++-обозначения в этих файлах

- `engine::ecs::World` — тип World внутри namespaces engine и ecs; `::` уточняет область имени.
- `m_world`, `m_jobs` — префикс `m_` обозначает member, поле экземпляра класса; это соглашение проекта, не оператор C++.
- `World&` — ссылка на существующий World без копии; `World*` — указатель, который может быть nullptr. `const World&` запрещает изменение через эту ссылку, но не ставит межпоточный lock.
- `auto* transform = world.getComponent<Transform>(id)` — компилятор выводит тип указателя; nullptr означает отсутствие компонента. Прежде чем обращаться через `transform->position`, проверить указатель.
- `world.forEach<Transform,Rigidbody>(...)` — template выбирает типы компонентов на этапе компиляции. Компонентный набор здесь не разбирается из строк во время выполнения.
- `[&](size_t begin,size_t end) { ... }` — lambda; внешние переменные захвачены по ссылке. `[this]` захватывает указатель на текущий объект, не продлевая его жизнь. `[data=std::move(data)]` переносит данные в callable; это удобно для загрузочного payload.
- `std::move` позволяет переместить владение/содержимое, а не создаёт новый поток и не обязательно копирует bytes. После переноса не рассчитывать на прежнее содержимое source vector.
- `std::span<T>` не владеет массивом: его исходный vector должен оставаться живым, нужного размера и не перевыделять память до окончания работы.
- `virtual ... = 0` — абстрактный контракт интерфейса; `override` — реализация метода базового класса; `final` запрещает дальнейшее наследование/переопределение в указанном месте.
- `[[nodiscard]]` просит компилятор предупредить о проигнорированном результате. Это особенно полезно для success/handle, но само по себе не обрабатывает ошибку.
- `0.0F` — float literal, `256U` — unsigned integer literal. `size_t` используется для размеров/индексов; glm::vec3 — три float, glm::mat4 — матрица 4×4.
- `try/catch`, `exception_ptr`, `rethrow_exception` в JobSystem позволяют сохранить ошибку worker и получить её в вызывающем потоке через wait.
- `atomic.load/store` решают часть межпоточной синхронизации; обычный vector рядом с atomic не становится автоматически безопасным. В loading queue смысл acquire/release — публиковать завершённый payload, а relaxed counters нужны только для диагностики/состояния.

| Термин | Простое значение здесь |
|---|---|
| Ownership / lifetime | Кто обязан удалить объект и до какого момента он жив. |
| RAII | Создать ресурс в объекте, освободить в destructor; Tracy zone также закрывается по scope. |
| Pimpl | Скрыть implementation за Impl, не тянуть библиотечные детали в public headers. |
| Lambda capture | Данные, доступные задаче: копия/перемещение или ссылка с отдельным lifetime. |
| Span | Представление существующего непрерывного массива, без владения и копирования. |
| Range `[begin,end)` | begin входит, end не входит; соседние ranges не пересекаются. |
| Join/barrier | До следующей фазы дождаться предыдущей работы. |
| Cooperative wait | Ожидающий поток может помогать выполнить подходящие jobs. |
| Work stealing | Idle workers получают доступ к работе других очередей через scheduler. |
| Backpressure | Не выдавать бесконечно новые задания, если downstream GPU finalize не успевает. |
| Finalize/pump | Main-thread часть завершения ресурса / её ограниченная обработка в кадре. |
| Fallback | Рабочая замена отсутствующему/неготовому ресурсу. |
| Draw call | CPU-вызов отрисовки; не entity и не job. |
| Swapchain / Present | Экранные buffers / передача готового кадра на вывод. |
| Broadphase / narrowphase | Дешёвые возможные пары / более точная проверка контакта. |
| p95/p99 | Значение, ниже которого находятся примерно 95/99% наблюдений выбранного диапазона. |

Проверь себя без подсказок:

1. Как main доходит до PhysicsSystem и RenderSystem?
2. Почему 60 Hz update не гарантирует 60 FPS?
3. Какие компоненты нужны нарисованному физическому ящику?
4. Чем local matrix отличается от world и MVP?
5. Почему render transforms можно считать параллельно, а solver pairs — нельзя просто так?
6. Почему shared_ptr<Texture> может быть ненулевым до GPU upload?
7. Где PNG декодируется, где создаётся IImage, что публикует worker result?
8. Почему shutdown сначала закрывает asset queue, потом scheduler?
9. Что именно меняет TGE_JOBS=0, а что остаётся async?
10. Почему нельзя складывать длительности worker zones и назвать это временем кадра?
11. Где добавить сериализацию нового component и почему World сам её не сделает?
12. Чем legacy папка rhi_d3d12 отличается от D3D12 API внутри активного Diligent?

Краткая связная формулировка проекта: «Application владеет общими сервисами и запускает fixed updates/рендер. GameplayState хранит World и вызывает системы. RenderSystem собирает объекты, параллельно готовит мировые матрицы и на main отправляет draw через Renderer/RHI/Diligent. Ресурсы декодируются CPU jobs, а GPU objects создаёт bounded main-thread pump. Физика параллелит независимые per-body фазы, но solver последовательный. Tracy показывает реальные CPU scopes и позволяет сравнить одинаковые workloads до/после».
