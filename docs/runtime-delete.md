# Удаление объектов в Play/runtime

Delete доступен через клавишу, Inspector, Scene Hierarchy, контекстное меню и Edit → Delete Selected. На паузе он тоже работает. В текстовом поле клавиша Delete продолжает редактировать текст, не удаляет сущность.

В Play запрос попадает в `m_pendingRuntimeDeletes`. В начале следующего `GameplayState::update` главный поток применяет удаления **до** новых spawn/physics jobs. Physics и подготовка матриц используют синхронный `parallelFor`: он ждёт завершения перед возвратом, поэтому прошлые ECS jobs уже закончены. Очередь не предназначена для вызовов из workers.

У объекта удаляются компоненты и связанные cached collision pairs, отправляется CollisionExit. Дочерние сущности остаются, но отсоединяются от родителя; очищаются selection/hover, camera/striker/showcase и состояние gizmo. Повторные запросы объединяются. Это удаление сущности, не немедленное освобождение всех кешированных ресурсов Mesh/Texture.

Runtime-изменения временные: Stop восстанавливает pre-Play snapshot. Если Stop, Reset или восстановление сцены произошли до обработки запроса, очередь очищается: старые ID не могут удалить новые/восстановленные сущности. Edit Delete по-прежнему выполняется сразу и поддерживает Undo/Redo; runtime Delete не меняет эту историю.

Код: [GameplayState.cpp](../engine/game/GameplayState.cpp), [physics_system.cpp](../engine/ecs/physics_system.cpp). Headless regression: [GameplayStateTests.cpp](../tests/GameplayStateTests.cpp) — Delete/text input/repeat, deferred boundary, pause, references/hierarchy, Stop до и после обработки, Edit Undo/Redo, реальная parallel physics и повторное использование ID. Это проверка конкретных сценариев, не формальное доказательство отсутствия всех гонок.

```powershell
cmake --build --preset profile-release --target app engine_gameplay_state_tests engine_physics_tests
ctest --test-dir C:/tge/profile -C Release --output-on-failure
```

## Диагностика низкого FPS с Tracy, 09.10.2026

До исправления `ENGINE_TRACY_PROFILE` не только размечал зоны:

- `GameplayState::createDemoScene` добавляет 32×40 = **1280 динамических тел** к обычной сцене: получается 1301 сущность, как на скриншоте.
- `GameplayState::onEnter` автоматически включает Play.
- `Application::init` задаёт 1280×720, VSync off и начальное состояние gameplay. Последующее изменение размера окна возможно.

Последний журнал показанного запуска находится в `C:/tge/clion-vs2022b-rel/Debug/logs/engine.log`. В cache этого build tree Debug flags — `/Od /Ob0 /RTC1`, Release — `/O2 /Ob2 /DNDEBUG`. Название preset `clion-release` при Visual Studio multi-config само по себе не выбирает Release для сборки/запуска.

При большом dt цикл Application выполняет несколько фиксированных шагов 1/60 с перед render, до 15 при ограниченном dt=0,25 с. Это усиливает просадку тяжёлой Debug-физики. Кроме того, ImGui получает ограниченный dt, поэтому показанные FPS/frame time могут скрывать ещё более длинный реальный кадр. Стоимость собственно Tracy и доли физики/рендера отдельно в этой диагностике не измерены.

## Исправление: Tracy не меняет сцену

Включение/подключение Tracy теперь не добавляет объекты, не включает Play и не меняет параметры окна, VSync или initialState из config.json. При обычном входе в Gameplay создаётся исходная сцена из **21 сущности в Edit**, с Tracy или без него. Для симуляции нажать Play.

Прежняя тяжёлая сцена сохранена как явная опция окружения запуска:

```text
TGE_STRESS_SCENE=1
```

Только этот флаг добавляет 1280 тел, включает Play при входе и выбирает прежние стресс-настройки 1280×720/VSync off/gameplay. Отсутствие флага, 0 или любое значение кроме 1 выключает её. Переменные читаются при создании Application: для изменения нужен перезапуск.

`TGE_LAB_SCENE=ecs/loading` имеет приоритет: lab-сцены и их режим/параметры не меняются даже при заданном stress-флаге. Это проверяется GameplayStateTests на сборках с включённым Tracy, вместе с normal=21/Edit и stress=1301/Play. CMakeUserPresets.json не редактировался, исторические трейсы не перезаписаны. Debug-оптимизации и fixed-step catch-up этим исправлением не менялись; для замеров по-прежнему нужна Release-сборка.
