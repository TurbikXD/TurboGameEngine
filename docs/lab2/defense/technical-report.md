# TurboGameEngine: технический отчёт к защите ЛР 2

Дата: 16.10.2026. Основание: ТЗ «Скриптинг», текущий код, тесты и зафиксированные локальные прогоны.

## 1. Результат и граница работы

В C++20 движок встроена Luau VM. Игровые решения выполняет скриптовая механика Collectathon. Префабы и поля связаны с ECS и Inspector; hot reload сохраняет состояние поведения. Настоящий React Luau работает через собственный retained host и native renderer. Подготовленная презентация тоже является Luau/React-проектом движка.

Документ охватывает весь текущий результат, включая незакоммиченные подсистемы. Это не полный Roblox runtime. Наличие реализации и факт выполнения теста разделены: матрица содержит исходники, сводка проверок - результаты запусков.

## 2. Архитектура и поток данных

Application создаёт платформу, jobs, renderer и state stack. GameplayState владеет миром и связывает с ним ScriptingSystem. Одна VM обслуживает экземпляры поведения мира. ScriptComponent сериализует путь модуля, enabled и fields; VM references остаются вне ECS.

На шаге симуляции движок вызывает поведение. Engine/Input позволяют безопасно работать с сущностями, Transform, физикой, prefab spawn, запросами и статусом. Сущности идентифицируются handle с поколением; destroy из callback откладывается до безопасной границы, spawnPrefab создаёт сущность сразу на потоке владельца. Lua-вызовы и ECS apply выполняются на owner thread.

При изменении файлов watcher ограниченно читает и сравнивает содержимое. Jobs читают/компилируют модули; поток владельца применяет результат и зависимые require. Успех сохраняет self.state и вызывает OnReload; неудача оставляет previous-good код. Destroy/Stop очищают экземпляры и UI, shutdown ждёт jobs и закрывает VM.

## 3. Сопоставление с обязательными требованиями

### 2.1.lifecycle: Язык как подсистема: init/shutdown, единая загрузка, lifetime

Статус исходников: Реализовано.

ScriptingSystem владеет Luau VM; GameplayState initialize/shutdown привязывает VM к миру и asset root. Экземпляры поведения получают fields/state.

Доказательства: engine/scripting/ScriptingSystem.cpp:739; engine/scripting/ScriptingSystem.cpp:752; engine/game/GameplayState.cpp:1197.

Пределы: Одна VM на мир. Luau behavior table вместо Python trampoline/C++ inheritance.

### 2.1.distribution: Поставка интерпретатора на пользовательской машине

Статус исходников: Реализовано.

Luau.VM/Compiler статически связаны. React, assets, Windows StyLua копируются рядом с app. Codex не нужен игре.

Доказательства: CMakeLists.txt:107; CMakeLists.txt:352; CMakeLists.txt:657.

Пределы: Первая configure загружает pinned dependencies; готовому app нужен комплект assets/DLL. Clean-machine запуск в этом аудите не выполнялся.

### 2.2.host_to_script: Движок → скрипт: Init/Update и реакция на события

Статус исходников: Реализовано.

Init/Update/Destroy/OnEvent вызываются защищённо. C++ collision EventBus ставит события в bounded queue, dispatch идёт после physics.

Доказательства: engine/scripting/ScriptingSystem.cpp:595; engine/scripting/ScriptingSystem.cpp:614; engine/scripting/ScriptingSystem.cpp:829; engine/game/GameplayState.cpp:1327.

Пределы: Broadcast OnEvent; произвольные subscribe/unsubscribe и приоритеты gameplay events отсутствуют.

### 2.2.script_to_host: Скрипт → движок: минимум4 категории API

Статус исходников: Реализовано.

Entity/transform; Input; prefab spawn/deferred destroy; find/queryRadius. Дополнительно velocity/force/status.

Доказательства: engine/scripting/ScriptingSystem.cpp:499; engine/scripting/ScriptingSystem.cpp:164; engine/scripting/ScriptingSystem.cpp:459.

Пределы: Bindings ручные. queryRadius проверяет transform distance и optional tag; script raycast API нет.

### 2.2.ownership: Безопасное владение на границе

Статус исходников: Реализовано.

ECS ScriptComponent хранит только данные. VM refs принадлежат runtime; versioned EnTT numeric handle проверяется World::isAlive. Destroy deferred.

Доказательства: engine/scripting/ScriptComponent.h:9; engine/scripting/ScriptingSystem.cpp:137; engine/scripting/ScriptingSystem.cpp:409; engine/scripting/ScriptingSystem.cpp:658.

Пределы: Номер entity — дескриптор, не указатель/владелец. Object-reference field type/picker отсутствует.

### 2.3.reload: Reload без перезапуска; минимум вне Play

Статус исходников: Реализовано.

Watcher stable changes + explicit reload. JobSystem читает/компилирует, owner thread применяет. Ошибка сохраняет last-good; следующий Play перечитывает disk.

Доказательства: engine/scripting/ScriptingSystem.cpp:868; engine/scripting/ScriptingSystem.cpp:909; engine/scripting/ScriptingSystem.cpp:923; engine/scripting/ScriptingSystem.cpp:771.

Пределы: При jobs=null чтение/compile синхронно. VM/ECS изменения всегда owner thread.

### 2.4.mechanic: Одна настоящая механика полностью в скрипте

Статус исходников: Реализовано.

Collectathon: движение, волны, pickup, таймер, счёт, победа/поражение и restart; C++ предоставляет сервисы и demo scene setup.

Доказательства: assets/scripts/Collectathon.luau:17; assets/scripts/Collectathon.luau:29; assets/scripts/Collectathon.luau:50; assets/scripts/Collectathon.luau:90.

Пределы: Локальная механика. Pickup — queryRadius, CollisionEnter отдельно наблюдается в state.

### 2.5.prefab: JSON prefab с компонентами и script.fields; spawn одной командой

Статус исходников: Реализовано.

Engine.spawnPrefab разбирает JSON/overrides до изменения ECS; Collectathon порождает collectible/player из шаблонов.

Доказательства: engine/scripting/ScriptingSystem.cpp:668; engine/scripting/ScriptingSystem.cpp:681; assets/prefabs/collectathon_controller.json:9; assets/scripts/Collectathon.luau:20.

Пределы: Конкретный список компонентов, не generic reflection. Inspector сохраняет экземпляр в сцену; Save Prefab UI не подтверждён.

### 2.6.inspector: Выбор объекта, typed поля, сохранение/применение

Статус исходников: Реализовано.

Explorer selection, Luau Behavior schema widgets, scene Undo/Redo. Scene JSON сохраняет script.path/enabled/fields; данные применяются следующим Play.

Доказательства: engine/game/GameplayState.cpp:4714; engine/scripting/ScriptingSystem.cpp:521; engine/scripting/ScriptingSystem.cpp:576; engine/game/GameplayState.cpp:2258.

Пределы: Scalar number/integer/bool/string; min/max/default. Нет object-reference/enum schema. Rendered checkbox fixture проверяет Mesh Visible; script fields покрыты отдельными integration cases.

## 4. Полный обзор результата рабочего дерева

Ниже - все 26 группы реализации и материалов из аудита. Файл git-snapshot.json содержит полный пофайловый перечень; vendored и служебные файлы не выдаются за самостоятельные пользовательские функции.

### Luau runtime/bindings

Новая untracked подсистема: VM lifecycle, behavior instances, API, require graph, events, quotas.

Исходники: engine/scripting/ScriptingSystem.cpp:739; engine/scripting/ScriptingSystem.cpp:499; engine/scripting/ScriptingSystem.cpp:845.

Ограничения: Не полный Roblox runtime. Deferred destruction repair verified in recorded bounded scenarios; no universal absence-of-leaks claim.

### Collectathon/prefabs/HUD

Новые Luau/JSON assets: gameplay decisions, balance fields, player/tokens, React HUD.

Исходники: assets/scripts/Collectathon.luau:50; assets/prefabs/collectathon_controller.json:9.

Ограничения: Демо локальной игровой механики.

### LiveFileWatcher/reload

Новая shared-core служба + integration: stable content jobs, module consumers invalidation, last-good/state attach.

Исходники: engine/core/LiveFileWatcher.cpp:44; engine/scripting/ScriptingSystem.cpp:909.

Ограничения: jobs=null — синхронный fallback.

### Upstream React Luau vendor

Настоящие pinned Roblox/react-luau + polyfill/symbol/promise; hashes/licenses/vendor script.

Исходники: third_party/react-luau/upstream.json:3; tools/vendor_react_luau.py:6.

Ограничения: Upstream hooks/reconciler не добавляют Roblox platform services.

### ReactTurbo/ReactRoblox UI host

Новые UI adapters/native host: refs/bindings/signals, enums/datatypes, roots/portals, native geometry и deferred pump.

Исходники: engine/scripting/react/ReactRuntime.cpp:39; engine/scripting/react/ReactUI.cpp:1198; third_party/react-luau/adapters/ReactTurbo.luau:181.

Ограничения: Синхронный LegacyRoot. Instance facade UI-only; неизвестные non-UI services fail явно.

### UI layout/images/controls

UIGradient, image atlas/Fit/Crop/Tile/Slice, TextBox, scroll/grid/scale/constraints; bounded renderer cache и frame pinning.

Исходники: engine/scripting/react/ReactUI.cpp:1198; engine/renderer/Renderer.cpp:1447.

Ограничения: Локальные images/aliases; HTTP/CDN отсутствуют.

### UI TweenService

Новый adapter: один deterministic pump, TweenInfo/easing/repeat/reverse/delay/cancel/completed, binding ownership.

Исходники: third_party/react-luau/adapters/TweenService.luau:1.

Ограничения: UI-only, не scene/physics tween.

### CanvasGroup GPU composition

Новый GPU subsystem: capture subtree, nested child-first flatten, straight-alpha resolve, fence lifetime.

Исходники: engine/renderer/UiCanvasCompositor.cpp:161; engine/renderer/UiCanvasCompositor.cpp:198.

Ограничения: 32 layers/frame,2048 side,64 MiB targets. GPU harness не запускался этим аудитом.

### Script Workspace/TextEditor

Новый editor: documents, linear Luau tokenizer, Find/Goto/comment, document history, Check, tools tabs, disk conflicts.

Исходники: engine/editor/ScriptWorkspace.cpp:369; engine/editor/ScriptWorkspace.cpp:138.

Ограничения: Нет LSP и VM debugger.

### Асинхронный StyLua

Настоящий formatter subprocess с --verify, revision check/cancel, Undo apply.

Исходники: engine/editor/ScriptFormatter.cpp:150; engine/editor/ScriptFormatter.cpp:228.

Ограничения: Отдельный Windows runtime копируется CMake.

### Codex app-server client

Новый optional subsystem: stdio JSON-RPC, catalog pagination, settings, streaming, UTF-8 bounds, explicit approvals.

Исходники: engine/editor/CodexAssistant.cpp:422; engine/editor/CodexAssistant.cpp:442; engine/editor/CodexAssistant.cpp:332.

Ограничения: Protocol/transport tests не доказывают login/paid inference или право на модель.

### Projects launcher/manifest/recents

Новые ProjectManager/Launcher + Application integration; contained paths, .tgeproj, asset namespace isolation.

Исходники: engine/core/ProjectManager.cpp:204; engine/core/ProjectManager.cpp:241; engine/core/Application.cpp:348.

Ограничения: Пользовательские projects/recents — user data.

### Оба Play режима

Изменённый GameplayState + новый PlaySession: pre-Play snapshot/history restore; child app с temporary scene/manifest и source/snapshot distinction.

Исходники: engine/game/GameplayState.cpp:2265; engine/core/PlaySession.cpp:74; engine/core/PlaySession.cpp:46.

Ограничения: Scene/history snapshot не VM checkpoint. Настоящий standalone D3D12 smoke не запускался этим аудитом.

### Live authored scene deltas

Watcher + sourceId remap + validation/apply сохраняют продолжающуюся simulation и runtime-spawned entities.

Исходники: engine/game/GameplayState.cpp:2371; engine/game/GameplayState.cpp:2385.

Ограничения: JSON parse и ECS apply — owner thread, не полный async scene loader.

### Inspector/history/input integration

Script fields serialization, lazy history edits, component editing, Play-safe history и scene shortcuts capture.

Исходники: engine/game/GameplayState.cpp:2137; engine/game/GameplayState.cpp:2258; engine/game/GameplayCommands.cpp:202.

Ограничения: Runtime Undo реконструирует ECS и script instances; VM instruction state не сохраняется.

### Command registry/Studio-style native UI

Новые command registry/5tabs, split choices, whole-group overflow, snap prefs, flat dock panels, shared Assistant rail, fonts/icons provenance.

Исходники: engine/editor/EditorCommands.cpp:111; engine/editor/EditorRibbon.cpp:324; engine/game/GameplayState.cpp:685.

Ограничения: Реальные TGE handlers, не сервисы Roblox. Local reference fonts ignored/не portable dependency.

### AtomicFile и сохранения

Новая core служба: exclusive temp sibling, flush/replace, sidecar/hardlink safety; scripts/scenes/layout.

Исходники: engine/core/AtomicFile.cpp:43; engine/editor/ScriptWorkspace.cpp:146.

Ограничения: Локальная FS/OS; disk conflicts требуют Reload Disk.

### Mesh bounds/frustum culling

Изменённые loaders/mesh/render prep + новый conservative AABB clip-frustum helper.

Исходники: engine/ecs/frustum.h:6; engine/ecs/render_system.cpp:57; engine/resources/mesh.h:36.

Ограничения: Новых FPS измерений нет; не occlusion culling.

### Texture safety/UI image GPU lifetime

Изменены decode preflight/limits, cache reservations, borrowed SRV pinning и RHI Canvas support.

Исходники: engine/resources/loaders.cpp:303; engine/renderer/Renderer.cpp:1421; engine/renderer/Renderer.cpp:1498.

Ограничения: Базовая upload queue уже в commit c8e3967; весь pipeline не новая ЛР2.

### Frame catch-up/native diagnostics

Ограничены4 interactive fixed steps; crash-last с категорией/module offset; native input additions.

Исходники: engine/core/Application.cpp:444; app/main.cpp:31; engine/platform/WindowGLFW.cpp:18.

Ограничения: Не recovery native faults/доказательство отсутствия крашей.

### Локальные REA/Ghidra tools

Pinned REA6.5.0 setup/registration + ignored research evidence; отдельно от runtime/CMake.

Исходники: tools/rea/package.json:6; tools/rea/README.md:4.

Ограничения: tools/rea/local и node_modules ignored; исследования не попадут в clone без отдельной передачи.

### Гайды, provenance, regression harnesses

Новые docs ЛР2/projects/UI и tests; README является claim, исполнение доказывают отдельные logs.

Исходники: docs/lab2/README.md:268; CMakeLists.txt:576.

Ограничения: Даты/названия docs могут отставать; старые результаты ЛР1 не ЛР2.

### Интерактивная защита как отдельный Luau/React project

8 тем, quiz, настоящий ECS prefab/Transform и dependency OnReload с counter/Init checkpoint;3 ключа в native GUI.

Исходники: demos/lab2-defense/assets/scripts/Defense.luau:342; demos/lab2-defense/README.md:79.

Ограничения: Headless Engine — test double, OnReload fixture вызывается напрямую; это отдельно от native GUI evidence. Не Roblox platform runtime.

### Отдельный Collectathon project для живой защиты

Самостоятельные .tgeproj/scene/scripts/prefabs/shaders/textures без изменения пользовательского проекта.

Исходники: demos/lab2-collectathon/README.md:1.

Ограничения: Movement/pickup не повторялись в финальном GUI smoke.

### Воспроизводимая stability verification

Isolated Windows process counters/fixture runner, Linux ASan/UBSan/LSan headless build+sameVM lifecycle, summaries with raw negative/positive evidence.

Исходники: tools/run_lab2_stability.py:32; tools/verify_lab2_memory/MemoryLifecycle.cpp:143.

Ограничения: Инструменты проверки не runtime engine features; GPU/driver не покрыты Linux sanitizers.

### Презентация/доклад/отчёт и генераторы

13 редактируемых PPTX слайдов, PDF слайдов и технический PDF, speaker/source notes и общие content/evidence данные.

Исходники: docs/lab2/defense/README.md:5.

Ограничения: Публичные пути финальны; parent перегенерирует существующий report PDF после Git counts, поэтому content hashes этих mutable output здесь не фиксируются.

## 5. Качество и дополнительные пункты ТЗ

- 3.stability: обнаруженный дефект отложенного уничтожения исправлен. Повторный Windows CTest 19/19 PASS; Linux ASan/LSan/UBSan 11/11 с 240 lifecycle-циклами; Windows native 300 секунд без ошибок Luau. Подробности и пределы - раздел 6 и memory-report.md. Все сценарии движка/GPU этим не покрыты.
- 4.jobs: Реализовано. Read/compile dispatch Low, join до owner-thread apply; watcher jobs + stable contents debounce. Используется существующий pool ЛР1, не второй пул.
- 4.play_reload_L2: Реализовано. Сохраняется self.state; заменяется metatable методов, fields синхронизируются; OnReload без повторного Init, require consumers invalidated. Автор мигрирует изменённую state schema. React hooks identity переносится не универсально; VM checkpoint отсутствует.
- 4.fields_L1: Реализовано. Defaults, bounds, integer semantics, scalar widgets, сохранение scene fields. Произвольные nested значения/enum combo schema отсутствуют.
- 4.fields_L2: Не реализовано. Есть безопасные runtime handles, но ScriptField schema и Inspector не имеют entity-reference типа/picker. Numeric field/self.state handle не равны сериализуемому editor object-reference field.
- 4.events: Реализовано с ограничениями. CollisionEnter/Stay/Exit → queue → Behavior:OnEvent; UI signals — отдельный deferred pump. Нет script subscribe/unsubscribe, приоритетов и post-in-future общего gameplay bus.
- 4.bindgen: Не реализовано. Engine/Input API регистрируется вручную. Нет генерации из C++ headers/annotations и IDE stubs.
- 4.ide: Реализовано с ограничениями. Documents, Luau highlighting, Find/GoToLine/comment, Undo/Redo, Check и AST-verified async StyLua. Check — compile/syntax, не full typechecker/LSP/autocomplete.
- 4.repl: Реализовано. Command выполняет source в текущей VM; State — inspect(selected), Errors — последний traceback; Activity memory/reload/status. Command может менять мир. State JSON не является save game.
- 4.profiling: Реализовано. Callback и reload read/compile содержат ENGINE_PROFILE_ZONE. Новых ЛР2 .tracy/performance измерений этим аудитом не проводилось.
- 4.debugger: Не реализовано. Step Tick — один simulation tick; Errors — последний traceback. Нет breakpoints/step-over/locals/VM debug adapter.
- 4.sandbox: Реализовано с ограничениями. Luau sandbox, contained asset paths, VM64 MiB, source2 MiB, callback25 ms Release/100 ms Debug; protected host boundaries. Одна VM мира, не отдельный state на mod/entity. Не OS security boundary.
- 4.scene_runtime: Частично. Watcher читает файлы в jobs; live scene authored deltas применяются к работающему миру. Есть project scene load. JSON parse/validation и ECS apply на owner thread. Нет полного script API async level switching.
- 4.save_game: Не реализовано. Scene сохраняет authored components и script.fields; self.state/VM refs не сериализуются. Scene Save/Load, pre-Play snapshot и State view не являются save game/VM checkpoint.
- 4.world_queries: Реализовано с ограничениями. Engine.find(tag) и queryRadius(position,radius,tag?) используются для pickup. Нет script raycast/sweep/physics shape query; editor picking ray не API Luau.

## 6. Проверки, память и стабильность

Актуальная машиночитаемая сводка: verification.json. Подробности и сырые логи: memory-report.md, memory-linux/, memory-windows-*/ и evidence/.

Базовый Windows CTest: 19/19 PASS, 35.96 секунды. Это результат до исправления дефекта из нагрузочного прогона; повторные проверки отражены в итоговой сводке.

Фактический CLion Profile app на Windows/MSVC вернул: «Perf profiler is not available for selected toolchain». Это ограничение выбранного инструментария. Снимок сохранён как evidence/clion-profiler-unavailable.jpg. CPU profile не равен проверке утечек.

Первый 300-секундный Release/D3D12 прогон повторял создание/уничтожение Collectathon и React HUD и перезагрузку скрипта. Выявлено исчерпание 64 MiB Luau. Исходный exit=0 не означает успех: runtime содержал ошибки. Эти отрицательные данные сохранены, а не заменены новым успешным логом.

Счётчики Private Bytes/Working Set/handles показывают поведение процесса, но не называют утечки. ASan/UBSan/LSan проверяют инструментированные CPU-пути. LSan при lua_close не обнаружит как leak всё, что ещё было достижимо до закрытия VM; для этого нужен same-VM lifecycle stress. GPU/драйвер и все пользовательские сценарии этим не покрыты.

Повторный Windows CTest после исправления: 19/19 PASS за 35.12 с. Новый regression использует настоящие prefab и React HUD, проверяет число сущностей/UI roots и невалидность старого handle. Оптимизированный app пересобран; SHA-256 указан в verification.json.
Windows после fix: 303.1 с, exit=0; ошибок Luau 0, применённых reload batches 30. Диапазон Luau по секундным samples [3.14, 7.65] MiB; диапазон сущностей [5, 18]. После прогрева Private Bytes: 628.54-630.03 MiB; линейный тренд 0.219 MiB/min. Это наблюдения сценария, не универсальный тест отсутствия утечек.
Linux ASan/LSan/UBSan: 11/11 успешных процессов: scripting 5 раз, React 5 раз и same-VM lifecycle с двумя фазами по 120 циклов. Диагностик sanitizer нет, suppressions нет. В normal-GC фазе live world/root counts ограничены; перед принудительным GC 6,814,692 bytes, после 3,527,060 bytes при baseline 3,476,060 bytes. После Stop/reset roots=0, events=0; после shutdown VM=0 в обеих фазах.
Для инструментированной React test-copy общий batch watchdog увеличен до 1000 ms из-за sanitizer overhead; оригинальный production guard не менялся, отдельный тест 5 ms сохранён. Byte-identical assets скопированы на Linux FS, чтобы WSL NTFS I/O не расходовало watchdog. Исходные неуспешные попытки с неизменённым timing сохранены в memory-linux/ и local-assets/.
Native-презентация прошла квиз, реальный prefab/Transform и disk reload с counter=1, Init=1, OnReload=1; скриншоты в evidence/. Первоначальная ошибка require без .luau исправлена, headless loader теперь проверяет точные имена. Файлы правил возвращены к A после проверки.
CLion Profile app был действительно запущен, но capture недоступен для текущего Windows/MSVC toolchain. Это точный блокер запрошенного CLion-профиля; альтернативные прогоны не называются результатом CLion. Настройки IDE и системные пакеты не менялись.

## 7. Git: что ещё не закоммичено и не отправлено

Снимок: 2026-10-16T12:54:35.549088+00:00. Ветка main; HEAD 76c2386e44c4a82cbb16f17ea9f4d20492845353. Upstream origin/main; ahead=0, behind=0 по локальным tracking refs. Fetch не выполнялся; состояние remote-сервера не проверялось.

Изменённых путей: 496; tracked без staged: 26; untracked: 470; staged: 0. По этому локальному сравнению неотправленных коммитов нет. Несохранённые в Git изменения существуют независимо от ahead=0.

Полный перечень - git-snapshot.json / files. Он включает изменения, новые файлы и предсуществующее удаление reports/5.pdf. Удаление не выполнялось этой работой. Игнорируемые REA-исследования/reference fonts/личные настройки перечислены отдельно; временные зависимости и копии fixtures не являются новой функциональностью.

Существующая закоммиченная основа: 76c2386 Migrate ECS to EnTT and optimize Inspector undo snapshots; c8e3967 Add GPU upload queue, fix resource uploads, and benchmark 4 vs24 workers; 9782dea Add shared job system, Tracy measurements, and Lab1 defense package. Она используется, но не приписывается целиком новой ЛР 2.

## 8. Поставка и демонстрация

Luau VM/Compiler статически связаны. В поставку входят app и необходимые DLL, встроенные assets, React-пакеты и используемые инструменты редактора. Studio/Python/npm/Codex не нужны для запуска игры. Проверка на чистой пользовательской машине не выполнялась.

Открыть demos/lab2-defense/Lab2Defense.tgeproj через Projects, затем Play. Игра содержит восемь тем и три задания: квиз, реальные prefab/Transform и проверку настоящего OnReload. Кнопка смены темы не считается reload. Подробные действия - README демонстрации.

Сценарий доклада, 180-секундной демонстрации Collectathon и ответы - defense-notes.md. Резервные слайды имеют speaker notes с источниками.

## 9. Явно не заявляется

- Нет полного Roblox runtime и полного отладчика VM с breakpoints.
- Runtime Undo/Redo пересоздаёт мир, это не checkpoint VM.
- State preservation относится к поведению; изменение идентичности React-компонента может сбросить hooks.
- Ограниченный прогон не доказывает отсутствие утечек во всех сценариях, драйвере или GPU.
- Нет object-reference/enum field schema, bind generator, полного gameplay subscribe/unsubscribe bus, script raycast/sweep или savegame всех VM состояний.
- Check проверяет компиляцию/синтаксис, а не заменяет полноценный typechecker/LSP.
- Смена модели в Codex-каталоге не доказывает успешный платный inference; в проверках использованы протокол/локальные fixtures.

## 10. Источники и воспроизводимость

implementation-evidence.json хранит точные file:line/anchor/excerpt; номера строк относятся к моменту аудита. git-snapshot.json хранит состояние файлов на момент снимка. verification.json связывает итог с конкретными логами и бинарниками.

Документация JetBrains: https://www.jetbrains.com/help/clion/cpu-profiler.html и https://www.jetbrains.com/help/clion/memory-profiling-with-valgrind.html (проверены 16.10.2026). CPU profiler документирован для Linux/macOS; Memcheck на Windows интегрируется через WSL. Онлайн-справка может быть новее установленного CLion 2026.1; локальная GUI-ошибка является самостоятельным доказательством текущей конфигурации.

Авторские материалы и проверки не выполняют commit/push, не меняют глобальную IDE-конфигурацию и не переписывают пользовательские проекты.
