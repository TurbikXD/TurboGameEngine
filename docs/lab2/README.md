# ЛР 2: Luau, интерфейс React и редактор скриптов

Встроен **Luau**, диалект Lua из Roblox. В движке используются настоящие
upstream-пакеты [Roblox/react-luau](https://github.com/Roblox/react-luau),
а `ReactTurbo` связывает их reconciler с ImGui. Roblox Studio и Roblox API
для запуска не требуются.

[Архитектура решения ЛР 2](architecture.md): карта файлов и функций,
жизненный цикл VM, bindings, prefab, hot reload, React UI и связь с редактором.

## Запуск демо

1. Соберите `app` обычным CMake preset. В селекторе Projects выберите
   **Engine Demo** либо создайте/откройте отдельный проект.
2. Выберите **Script → Lab 2: Collectathon + React UI**.
3. Выделенный `Collectathon Controller` содержит редактируемые параметры:
   `goal`, `duration`, `tokenCount`, `moveSpeed`, `pointsPerToken` и другие.
4. Нажмите **Play in Editor**, переключитесь кнопкой **Script** из кода в Viewport.
   Наведите курсор на Viewport: WASD/стрелки двигают синюю сферу; R начинает
   новую игру. Собирайте золотые кубы до окончания таймера.
5. Окно React показывает счёт, цель, оставшееся время, результат и кнопку
   локального счётчика на настоящем `React.useState`.
6. Правила можно менять в инспекторе во время Play. Для изменения кода откройте
   `scripts/Collectathon.luau` и нажмите **Save + Reload**.
   Во время Play перезагрузка переносит `self.state` и заменяет методы.
7. **Save Scene** сохраняет поля поведения в `assets/scenes/editor_scene.json`.
   **Load Scene** восстанавливает их. Stop возвращает сцену к снимку до Play.

Все игровые решения, включая движение, волны, подбор, таймер, счёт, победу
и рестарт, находятся в Luau. C++ предоставляет ECS, ввод, физику и рендер.

## Поведения и данные

Скрипт возвращает таблицу поведения. VM создаёт отдельные `self.fields` и
`self.state` для каждого экземпляра. В `self.entity` находится числовой
дескриптор EnTT с поколением, а не указатель C++. Уничтоженные дескрипторы
не могут случайно обратиться к новой сущности в той же ячейке.

```lua
local Behavior = {}
Behavior.fields = {
    speed = { type = "number", default = 3, min = 0, max = 30 },
    enabled = { type = "boolean", default = true },
    title = { type = "string", default = "Example" },
}

function Behavior:Init()
    self.state.elapsed = 0
end

function Behavior:Update(dt: number)
    self.state.elapsed += dt
end

function Behavior:OnEvent(event)
    if event.type == "CollisionEnter" then
        print(event.entityA, event.entityB)
    end
end

return Behavior
```

Инспектор объявленных полей строится из `fields`; изменения входят в историю
Undo/Redo, дублирование и сериализацию сцены. Поля редактируются в Edit и Play in Editor.
В Play вкладка **Watch** показывает текущее состояние выбранного экземпляра.
Модуль должен находиться внутри `assets`, например `scripts/Collectathon.luau`.
Имена с начальным `assets/` нормализуются.

Prefab — JSON с `components`: `transform`, `tag`, `meshRenderer`, `rigidbody`,
`collider`, `script`. Примеры находятся в `assets/prefabs`. В `script.fields`
сохраняются параметры баланса. `Engine.spawnPrefab("prefabs/collectible.json")`
создаёт экземпляр одной командой. Двойной щелчок по prefab в Content Browser
создаёт экземпляр в Edit; двойной щелчок по `.luau` открывает код.

## API скриптов

| Категория | API |
|---|---|
| Сущности | `Engine.isAlive(id)`, `Engine.getTag(id)`, `Engine.find(tag)` |
| Трансформ | `Engine.getPosition(id)`, `Engine.setPosition(id, vector)` |
| Поворот | `Engine.getRotation(id)`, `Engine.setRotation(id, vector)` — Euler в радианах |
| Физика | `Engine.getVelocity(id)`, `Engine.setVelocity(id, vector)`, `Engine.applyForce(id, vector)` |
| Спавн | `Engine.spawnPrefab(path, overrides?)`, `Engine.destroy(id)` |
| Запросы мира | `Engine.queryRadius(position, radius, tag?)` |
| Ввод | `Input.isKeyDown(key)`, `Input.wasKeyPressed(key)`, `Input.wasKeyReleased(key)`, `Input.mousePosition()` |
| Состояние HUD | `Engine.setStatus(name, value)`, `Engine.getStatus(name)` |
| UI | `require("React")`, `require("ReactTurbo")` |

Игровой ввод доступен, когда Viewport имеет фокус или находится под курсором,
а поле кода/чата не принимает текст. Движок не перехватывает Space/Delete
и команды сцены при вводе текста. Уничтожение из поведения ставится в очередь
на безопасную границу кадра. Физические задачи завершены до обработки событий.

## React Luau

```lua
local React = require("React")
local ReactTurbo = require("ReactTurbo")

local function Counter()
    local count, setCount = React.useState(0)
    return React.createElement("TextButton", {
        Text = "Clicks: " .. count,
        [React.Event.Activated] = function()
            setCount(function(previous) return previous + 1 end)
        end,
    })
end

local root = ReactTurbo.createRoot({ Name = "My UI" })
root:render(React.createElement(Counter))
-- root:unmount() освобождает дерево и вызывает очистку эффектов.
```

Адаптер поддерживает `ScreenGui`, `Window`, `Frame`, `TextLabel`, `TextButton`,
`Checkbox`, `SliderFloat`, `Separator`, `SameLine`, `UICorner`, `UIPadding`,
`UIListLayout`, `UIStroke`, callbacks и `React.Event`/`React.Change`. Hooks, контекст, refs,
эффекты, bindings и reconciliation выполняются настоящей библиотекой React Luau.

Основной пробел прежней интеграции был в native host: дерево React работало,
но размещение и оформление ограничивались последовательными ImGui-виджетами.
Теперь UI можно компоновать привычными для Roblox размерами, координатами и
декораторами. Значения создаются через `ReactTurbo`, без глобальных Roblox API:

```lua
local React = require("React")
local UI = require("ReactTurbo")
local e = React.createElement

local root = UI.createRoot({ Name = "Styled UI" })
root:render(e("ScreenGui", {}, {
    Panel = e("Frame", {
        Size = UI.UDim2.fromOffset(280, 160),
        Position = UI.UDim2.new(1, -16, 1, -16),
        AnchorPoint = UI.Vector2.new(1, 1),
        BackgroundColor3 = UI.Color3.fromRGB(24, 32, 49),
        BorderSizePixel = 0,
    }, {
        Corner = e("UICorner", { CornerRadius = UI.UDim.new(0, 12) }),
        Padding = e("UIPadding", {
            PaddingLeft = UI.UDim.new(0, 16), PaddingTop = UI.UDim.new(0, 16),
            PaddingRight = UI.UDim.new(0, 16), PaddingBottom = UI.UDim.new(0, 16),
        }),
        Layout = e("UIListLayout", {
            FillDirection = "Vertical", SortOrder = "LayoutOrder",
            Padding = UI.UDim.new(0, 8),
        }),
        Title = e("TextLabel", {
            Text = "React Luau", LayoutOrder = 1,
            Size = UI.UDim2.new(1, 0, 0, 28), TextSize = 22,
            TextColor3 = UI.Color3.fromRGB(115, 219, 245),
            BackgroundTransparency = 1,
        }),
        Action = e("TextButton", {
            Text = "Continue", LayoutOrder = 2,
            Size = UI.UDim2.new(1, 0, 0, 38),
            BackgroundColor3 = UI.Color3.fromRGB(54, 103, 219),
            [React.Event.Activated] = function() print("Continue") end,
        }),
    }),
}))
```

| Свойства | Поведение |
|---|---|
| `UI.UDim2.new(xs, xo, ys, yo)` | Scale относительно родителя плюс Offset в пикселях; также `fromScale(x,y)` и `fromOffset(x,y)` |
| `Size`, `Position`, `AnchorPoint` | Размер, позиция и точка привязки `UI.Vector2.new(x,y)`; в списке размещением управляет layout |
| `BackgroundColor3`, `BackgroundTransparency` | Цвет `UI.Color3.new(r,g,b)` / `fromRGB(r,g,b)` и прозрачность 0–1 |
| `BorderColor3`, `BorderSizePixel` | Цвет и толщина границы |
| `TextColor3`, `TextTransparency`, `TextSize` | Цвет, прозрачность и размер текста |
| `TextXAlignment`, `TextYAlignment`, `TextWrapped` | Строки `Left/Center/Right`, `Top/Center/Bottom`, перенос текста |
| `UICorner.CornerRadius` | `UI.UDim.new(scale, offset)` для радиуса |
| `UIPadding` | `PaddingLeft/Right/Top/Bottom` как `UI.UDim` |
| `UIListLayout` | `FillDirection=Vertical/Horizontal`, `Padding`, `SortOrder=LayoutOrder/Name`, выравнивание по обеим осям |
| `LayoutOrder` | Порядок элемента в списке |
| `ClipsDescendants` | Обрезка потомков по прямоугольнику родителя, включая области клика |
| `Disabled`, `Interactable`, `Active` | Блокировка взаимодействия; `Active` применяется к кнопкам |
| `AutoButtonColor` | Автоматическое изменение цвета кнопки при наведении/нажатии |

Направления и выравнивания принимают typed `Enum` и прежние строки.
Глобальные datatypes, UI `Instance`, refs/signals и bootstrap `ReactRoblox`
описаны в [руководстве по совместимости](../roblox-ui-compatibility.md).
`Players.LocalPlayer.PlayerGui` доступен как UI-контейнер; полный Roblox DataModel
и остальные платформенные services не эмулируются. `Width`/`Height`, `TextColor`
и прежние простые виджеты сохраняют совместимость. `ScreenGui` занимает область
корня: окно UI в редакторе либо игровой viewport в отдельном Play.
Дополнительно работают UIGradient с Color/NumberSequence, ImageLabel/ImageButton
(atlas, Fit/Crop/Tile/Slice), TextBox, ScrollingFrame, UIGridLayout, UIScale и
size/aspect constraints. [Примеры, asset aliases и границы](../roblox-ui-visuals.md).
UI-only [TweenService](../tween-service.md) добавляет TweenInfo, easing,
Play/Pause/Cancel/Completed, repeats/reverse/delay и анимацию градиентов.
В проекте UI Demo восьмая вкладка MOTION показывает переходы, hover и portal-окно.
Девятая вкладка Canvas / Layers показывает
[CanvasGroup](../canvas-groups.md): единый fade окна, общий оттенок, вложенные
слои, декораторы и блокировку ввода скрытого UI.
Это подмножество Roblox UI: CSS/flex, произвольные шрифты, полный DataModel,
Roblox CDN и platform services не эмулируются.
Корни используют синхронный `LegacyRoot` reconciler, в том числе alias `ReactRoblox`;
concurrent-режим Roblox не подключён. Полноэкранный ImGui-корень сохраняет прежний
захват мыши в пустой области; игровой ввод с клавиатуры продолжает работать.
Ориентиры API: [UDim2](https://create.roblox.com/docs/reference/engine/datatypes/UDim2),
[GuiObject](https://create.roblox.com/docs/reference/engine/classes/GuiObject),
[UIListLayout](https://create.roblox.com/docs/reference/engine/classes/UIListLayout).
Оформленный HUD с прогрессом и работающим hooks-счётчиком:
`assets/scripts/ui/demo_ui.luau` (запускается через Lab 2 Demo).
События UI исполняются в защищённом pump на следующем тике, после рисования.
Stop/смена сцены размонтирует корни и сбрасывает очередь событий.

## Редактор и reload

**Script Workspace** содержит вкладки документов, номера строк, подсветку,
Undo/Redo, поиск, Go to Line, комментарии, проверку синтаксиса, Save + Reload,
Format через StyLua, Output, Command, Watch, Call Stack, Script Activity и
Pause/Step Tick/Stop. Форматирование асинхронное, с проверкой AST; изменения
применяются только если документ за это время не изменился, и отменяются Undo.
`Check` проверяет компиляцию Luau, а не полную статическую типизацию.
`Step Tick` выполняет один кадр симуляции длительностью 1/60 секунды.
Полного построчного отладчика с VM breakpoints и просмотром локальных
переменных пока нет; стек ошибки отображается в Output.

Чтение и компиляция при Reload выполняются общей JobSystem; VM и ECS
изменяются только на главном потоке. Ошибка reload оставляет последний
рабочий код. Успешный reload сохраняет `self.state`, переносит поля и меняет
методы; Init повторно не выполняется. При следующем Play заново читаются файлы.
Модульные `require`-зависимости после reload перечитываются.

Ошибки содержат файл/строку. VM имеет лимит памяти 64 MiB и бюджет исполнения
callback 25 ms в Release и 100 ms в Debug по умолчанию; бесконечный цикл превращается в ошибку.
Стандартные `io`, `os`, загрузка нативных библиотек и доступ к произвольным
путям скриптам не предоставлены. Command выполняет код в текущей VM и может
менять мир; используйте его как инструмент редактора.

Сохранение кода заменяет файл через временный соседний файл. При внешней
правке файла Save требует сначала просмотреть актуальный код через Reload Disk;
несохранённые правки не перезаписывают файл незаметно.

## Assistant / Codex

Откройте **Script → Assistant**, нажмите **Connect**, затем **Sign in with
ChatGPT** при необходимости. Используется официальный локальный
[Codex app-server](https://developers.openai.com/codex/app-server/) через stdio.
Установленный Codex CLI владеет авторизацией; движок не читает `auth.json`
и не хранит API-ключи. При необходимости укажите абсолютный путь к нативному
`codex.exe`. Автоматическое обнаружение предпочитает свежий runtime установленного
Codex Desktop; если его нет, проверяет PATH и npm-установку. Версия runtime,
модель и reasoning отображаются в панели. Каталог `model/list` позволяет обнаружить
устаревший runtime, но окончательно доступ к модели подтверждает успешный запрос.

Чат получает выбранный пользователем текст запроса. Контекст открытого
скрипта передаётся только при включённой опции контекста. По умолчанию доступ
к проекту read-only. Для правок требуется включить режим изменений в панели;
запросы подтверждения команд и файлов показываются отдельными кнопками.
Есть потоковые ответы, новая беседа, отмена и просмотр предлагаемых diff.
В панели есть **Model**, **Reasoning effort** и обновление каталога моделей.
По умолчанию используются настройки Codex; выбранные вручную значения применяются
к следующему запросу внутри движка. Для каждой модели предлагаются только effort
из `model/list` установленного runtime. Глобальный `config.toml` не переписывается.
Возврат в Codex configuration восстанавливает известные модель и effort в той же
беседе. Если runtime не сообщил одно из исходных значений, создаётся новая беседа,
чтобы снять сохранённые на стороне сервера overrides. Во время запроса настройки заблокированы.

Отдельно выбираются sandbox-доступ к проекту и approvals: **untrusted**,
**on-request**, **never**, а также проверяющий **User** / **Codex auto-review**.
`never` запрещает запросы подтверждений; это не выдаёт доступ за пределами sandbox.
Управляемые ограничения сервера учитываются, отказы показываются в чате.
Ручные approvals остаются явными кнопками подтверждения/отказа; по умолчанию
включены read-only, untrusted и User.
Проверки интеграции не выполняли платных запросов, вход или изменение аккаунта.

## Сборка и проверка

Luau Compiler/VM статически связаны с движком. CMake скачивает закреплённый
commit Luau; исходники React Luau, зависимостей и лицензии лежат в
`third_party/react-luau`. Пакет копируется в `assets/scripts/react-luau` рядом
с приложением. Windows formatter StyLua поставляется в `tools/stylua.exe`.
Codex CLI — отдельная необязательная установка, не условие запуска игры.

```powershell
cmake --preset debug
cmake --build --preset debug --target app engine_scripting_tests engine_react_runtime_tests engine_codex_assistant_tests engine_gameplay_state_tests engine_script_formatter_tests engine_script_editor_tests
ctest --test-dir C:/tge/dbg -C Debug --output-on-failure -R "engine_(scripting|react_runtime|codex_assistant|gameplay_state|script_formatter|script_editor)_tests"
```

`engine_scripting_tests`: lifecycle, bindings, устаревшие handles, JSON prefab,
ошибки, hot reload, перенос состояния, ввод, ограничения VM и настоящая механика.
`engine_react_runtime_tests`: upstream hooks, reconciliation, эффекты,
bindings, события и восстановление после ошибки callback.
`engine_codex_assistant_tests`: handshake, протокол, approvals, стриминг,
ограничение буферов и сохранение настроек без запросов к модели.
`engine_gameplay_state_tests`: прежние ECS/редакторские сценарии и новая
интеграция скриптов с Play/Stop/историей и вводом.
`engine_script_formatter_tests`: настоящий StyLua, сохранение семантики,
идемпотентность, ошибки, ограничения и отмена.
`engine_script_editor_tests`: точное сохранение текста и Undo/Redo для
комментариев и форматирования.
