# ECS на EnTT и JobSystem

`engine_ecs` использует EnTT **v3.15.0** с фиксированной версией зависимости. `World` остаётся API движка, но хранит сущности и компоненты в `entt::registry`, а выборки выполняет через EnTT views. Собственных `unordered_map` для component storage больше нет. Публичная зависимость и определение `ENTT_NO_ETO` распространяются через CMake target `engine_ecs`: пустые marker components сохраняют обычный API `T&`/`T*`.

## Сущности и компоненты

`EntityId` остаётся `std::uint32_t`; `0` — `kInvalidEntity`. Значение равно полному integral handle EnTT плюс один, включая version, а не только индекс сущности. После удаления и повторного использования слота новый handle отличается от старого. `isAlive`, `hasComponent` и `getComponent` учитывают поколение; stale handle не обращается к компонентам новой сущности. `addComponent` сохраняет семантику добавления или замены компонента.

`World::clear()` очищает существующий registry, сохраняя поколения. Старые handles не становятся валидными сразу после восстановления сцены. Handles принадлежат конкретному `World`; они не являются глобальными IDs. Version имеет конечное число значений и может обернуться после многократного использования слота, как в стандартном EnTT. Не следует сохранять runtime handles как постоянные идентификаторы объектов.

EnTT может переместить другой компонент при удалении через swap-and-pop. Поэтому указатель или reference из `getComponent` нельзя сохранять через структурные изменения. Порядок обхода views не является порядком создания сущностей.

## Фазы и потоки

Owner — поток, создавший `World`. Только он выполняет `createEntity`, `destroyEntity`, `clear`, `addComponent`, `removeComponent`, изменяемый `forEach` и подготовку `forEachParallel`.

`stablePhase()` — const RAII guard, создаваемый на owner-потоке. Пока guard существует, структурные операции отклоняются: это защищает views и указатели компонентов от инвалидирования. Guard не блокирует изменения самих значений компонентов и не синхронизирует такие изменения между потоками. Изменяемые серийные `forEach` и `forEachEntity` автоматически защищают свой обход: callback может менять полученные компоненты, но не структуру `World`. Структурные команды нужно собрать и применить после выхода из обхода/фазы.

Const lookup, const `forEach` и const `forEachEntity` не создают отсутствующие storage. Const traversal на owner-потоке автоматически защищает обход; на workers он не изменяет счётчик фаз, поэтому owner-поток должен удерживать `stablePhase()` до завершения их работы. Читаемые данные не должны изменяться конкурентно. `stablePhase()` и `forEachParallel` нельзя вызывать из worker callback, в том числе как вложенные операции.

`World::forEachParallel<Components...>(jobs, grain, callback)` на owner-потоке собирает соответствующие сущности и указатели компонентов через view. Затем `JobSystem::parallelFor` распределяет элементы по диапазонам и ожидает завершения всех callbacks перед возвратом. Если `jobs == nullptr`, выполняется серийный путь. Const overload передаёт const references. GPU APIs и структурные операции внутри callbacks запрещены.

Каждый callback может писать только в компоненты собственной сущности, которыми владеет его элемент. Общие объекты, компоненты родителей в hierarchy, захваченные переменные и выходные контейнеры требуют отдельной защиты или непересекающихся диапазонов. Guard обеспечивает стабильность storage, а не отсутствие гонок данных.

```cpp
engine::ecs::World world;
engine::core::JobSystem jobs(true, 4U);

const auto entity = world.createEntity();
world.addComponent<engine::ecs::Transform>(entity);
world.addComponent<engine::ecs::Rigidbody>(entity);

const float dt = 1.0F / 60.0F;
world.forEachParallel<engine::ecs::Transform, engine::ecs::Rigidbody>(
    &jobs, 64U,
    [dt](engine::ecs::EntityId, engine::ecs::Transform& transform,
         engine::ecs::Rigidbody& body) {
        transform.position += body.velocity * dt;
    }); // Все jobs завершены; структурные изменения снова разрешены.

world.destroyEntity(entity);
```

## Системы и восстановление сцен

Physics integration использует `forEachParallel`. Подготовка physics proxies и solver сохраняют stable phase до последнего использования component pointers; collision events публикуются после выхода из фазы. Интеграция завершается до чтения transform/hierarchy при подготовке proxies. В render guard охватывает gather, подготовку матриц и submit. Отдельный transform batch также защищён и синхронно ожидает jobs. Прежний `JobSystem` и его scheduler продолжают работать без изменения модели выполнения.

Gameplay snapshots и JSON сохраняют числовые source IDs и переназначают их при восстановлении: сначала создаются сущности, затем hierarchy и служебные ссылки получают новые handles. Старые сцены с числовыми IDs и нулевым sentinel остаются читаемыми. Очереди runtime deletes очищаются при восстановлении; stale request не должен удалять новую сущность, занявшую прежний слот.

## Проверка

Локальный `clion-debug` использует `C:/tge/clion-vs2022b-dbg` и конфигурацию Debug. В Visual Studio multi-config конфигурацию выбирает build preset/`-C Debug`, а не `CMAKE_BUILD_TYPE`.

```powershell
cmake --preset clion-debug
cmake --build --preset clion-debug --target app engine_world_tests engine_job_system_tests engine_physics_tests engine_transform_batch_tests engine_gameplay_state_tests engine_async_load_tests engine_texture_loader_tests
ctest --test-dir C:/tge/clion-vs2022b-dbg -C Debug --output-on-failure
```

ECS tests проверяют lifecycle и stale handles, очистку, component API и пустые markers, views, структурные ограничения и parallel iteration. Physics/transform tests сравнивают серийный и параллельный результат; gameplay tests проверяют удаление, повторное использование слотов, Stop и Edit Undo/Redo.

Контракт EnTT: [entity-component system, pointer stability и multithreading](https://github.com/skypjack/entt/wiki/Entity-Component-System). Wiki описывает текущую upstream-версию; реализация движка закреплена на v3.15.0.
