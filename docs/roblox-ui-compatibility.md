# Перенос React Luau UI из Roblox

Движок предоставляет совместимый UI-слой поверх собственного native renderer.
`Instance`, `Enum`, `UDim`, `UDim2`, `Vector2`, `Color3`, `Rect`,
`ColorSequence`/`NumberSequence` и их keypoints доступны глобально;
`require("ReactRoblox")` предоставляет знакомый bootstrap для корней и portals.
Hooks, reconciliation, bindings и эффекты по-прежнему выполняет настоящий React Luau.

В готовом компоненте обычно достаточно заменить расположение пакетов:

```lua
local React = require("React")
local ReactRoblox = require("ReactRoblox")
local e = React.createElement

local function App()
    local launches, setLaunches = React.useState(0)
    local ref = React.useRef(nil)
    React.useEffect(function()
        local button = ref.current
        assert(typeof(button) == "Instance" and button:IsA("GuiButton"))
        local connection = button:GetPropertyChangedSignal("AbsoluteSize"):Connect(function()
            print(button.AbsoluteSize)
        end)
        return function() connection:Disconnect() end
    end, {})
    return e("ScreenGui", { ResetOnSpawn = false }, {
        Launch = e("TextButton", {
            ref = ref,
            Size = UDim2.fromOffset(260, 52),
            Position = UDim2.fromScale(0.5, 0.5),
            AnchorPoint = Vector2.new(0.5, 0.5),
            BackgroundColor3 = Color3.fromRGB(62, 202, 183),
            Text = "Launches: " .. launches,
            TextXAlignment = Enum.TextXAlignment.Center,
            [React.Event.Activated] = function()
                setLaunches(function(previous) return previous + 1 end)
            end,
        }, {
            Corner = e("UICorner", { CornerRadius = UDim.new(0, 12) }),
        }),
    })
end

local container = Instance.new("Folder")
local root = ReactRoblox.createRoot(container)
root:render(ReactRoblox.createPortal(e(App), game:GetService("Players").LocalPlayer.PlayerGui))
-- При уничтожении поведения: root:unmount(); container:Destroy()
```

## Поддерживаемый контракт

| Область | Что работает |
|---|---|
| UI Instances | `ScreenGui`, `Frame`, `TextLabel`, `TextButton`, `ImageLabel`, `ImageButton`, `TextBox`, `ScrollingFrame`, `UICorner`, `UIPadding`, `UIListLayout`, `UIGridLayout`, `UIStroke`, `UIGradient`, `UIScale`, `UISizeConstraint`, `UIAspectRatioConstraint`, служебные `Folder`/`PlayerGui` |
| Прежние hosts | `Window`, `Checkbox`, `SliderFloat`, `Separator`, `SameLine`; `ReactTurbo.createRoot({Name=...})`, `Width`/`Height`, RGB `TextColor` и строковые выравнивания сохранены |
| Properties и refs | `ref.current` — UI Instance; `ClassName`, `Name`, `Parent`, `IsA`, обычное чтение/изменение поддерживаемых свойств |
| Дерево | `GetChildren`, `GetDescendants`, `FindFirstChild`, `FindFirstChildOfClass`, `FindFirstChildWhichIsA`, `FindFirstAncestor`, `GetFullName`, `IsDescendantOf`, `IsAncestorOf` |
| Lifecycle | `Destroy`, `ClearAllChildren`, `Clone`, `Archivable`; parenting проверяет циклы; unmount очищает refs, bindings и подписки |
| События | `Activated`, `MouseButton1Click`, `Changed`, `GetPropertyChangedSignal`, `ChildAdded`, `ChildRemoved`, `AncestryChanged`, `Destroying`; `Connect`, `Once`, `Disconnect`, `Connected` |
| Enum | Неизменяемые элементы с `Name`, `Value`, `EnumType`, `tostring`; `GetEnumItems`, `FromName`, `FromValue`, `Enum:GetEnums()` |
| Размеры и оформление | Scale/Offset, AnchorPoint, padding, list/grid layout, constraints и UIScale; `AutomaticSize.None/X/Y/XY`, UIStroke, UIGradient, изображения с atlas/Fit/Crop/Tile/Slice |
| Геометрия refs | `AbsolutePosition`, `AbsoluteSize`, `TextBounds`, `UIListLayout.AbsoluteContentSize` как read-only `Vector2` |
| Bootstrap | `ReactRoblox.createRoot(instance)`, `createLegacyRoot`, `createPortal`, `flushSync`, `unstable_batchedUpdates`, `Event`, `Change` |

Доступны UI-enums `FillDirection`, `HorizontalAlignment`, `VerticalAlignment`,
`TextXAlignment`, `TextYAlignment`, `SortOrder`, `AutomaticSize`, `ZIndexBehavior`,
`BorderMode`, `ApplyStrokeMode`, `LineJoinMode`, `DominantAxis`, `AspectType`,
`ScaleType`, `ScrollingDirection`, `StartCorner`, `TextTruncate` и подмножество `Font`. Перечисления для неподдерживаемых функций
не означают, что эти функции реализованы: точные ограничения перечислены ниже.

`typeof` различает UI Instances, Enum/EnumItem, UDim/UDim2, Vector2/Color3,
signals и connections. Значения неизменяемы; работают операции над размерностями,
Vector2, `Lerp` и создание цветов через `Color3.fromHSV`. Старые `ReactTurbo.UDim2` и другие
namespaced-конструкторы ссылаются на те же реализации.

У корней `ReactRoblox` и portals применяются Roblox-подобные значения по умолчанию,
включая контрастный чёрный текст на белом фоне. Старые корни ReactTurbo сохраняют
привычные значения ImGui и последовательный layout.

## Границы совместимости

- Это UI-объекты движка, а не полный Roblox DataModel. Неизвестные классы,
  свойства, enum-items и services завершаются понятной ошибкой.
- Нет CSS/flex, сетевых Roblox services, arbitrary ModuleScript hierarchy.
  Для пакетов проекта меняйте путь `require`; не ожидайте `ReplicatedStorage`.
- UI-only [TweenService](tween-service.md) поддерживает TweenInfo, easing,
  playback/signals и пять непрерывных типов целей, включая Rotation/Offset
  градиентов. 3D goals, SmoothDamp и ожидающий Signal:Wait не реализованы.
- [CanvasGroup](canvas-groups.md) сводит дочерние элементы в единый слой:
  GroupTransparency/GroupColor3, вложенные группы и общие UICorner/UIGradient.
  Interactable=false у предка и полностью скрытый CanvasGroup блокируют ввод
  поддерева по правилу движка; это расширение семантики Roblox.
- Изображения загружаются из локальных assets; Roblox IDs переназначаются через
  `assets/ui_images.json`. Загрузка с Roblox CDN и HTTP не эмулируется.
  [Градиенты, изображения и расширенные controls](roblox-ui-visuals.md) описаны отдельно.
- `Font` принимается как подсказка и использует локальный шрифт ImGui.
  `RichText=true`, `TextScaled=true`, truncation кроме `None` и `LineJoinMode`
  кроме `Round` отклоняются. UIStroke всегда рисует границу объекта, включая
  `ApplyStrokeMode.Contextual`; контур отдельных букв не реализован.
- `BorderMode`, `IgnoreGuiInset`, `ResetOnSpawn` и `ZIndexBehavior` сохраняются
  для переносимости, но не воспроизводят полную платформенную семантику Roblox.
  Границы используют native outline; в движке нет inset/topbar или player respawn.
- `ZIndex`/`DisplayOrder` сортируют соседей для рисования; глобальный порядок
  перекрывающихся interactive UI не полностью повторяет Roblox. Модальные
  компоненты должны явно блокировать нижние кнопки.
- Корни синхронные (`LegacyRoot`), в том числе через имя `ReactRoblox.createRoot`.
  Concurrent scheduling и Roblox platform/input services не эмулируются.
- `WaitForChild` возвращает уже существующего ребёнка; ожидание ещё не созданного
  объекта и `Signal:Wait` не поддержаны. Используйте `FindFirstChild`/`Connect`.
- События изменения свойств отрисовки проходят через защищённый pump на следующем
  тике. При большом дереве публикация геометрии может занять несколько кадров;
  ориентируйтесь на property signal, а не на немедленную геометрию после mount.
- Изменённый тип компонента при hot reload может перемонтироваться со сбросом
  hooks. Не храните ссылки на уничтоженные UI Instances между Play-сеансами.

## Проверка и устройство

В проекте **UI Demo** шестая вкладка **Roblox** открывает **ORBIT / Mission
Control**: анимированную орбитальную карту, телеметрию, выбор экипажа, улучшения
и запуск миссии через portal. Анимации используют bindings и подписку с cleanup;
игровые действия — reducer, а тема — context. Кнопка **SOURCE** открывает
дополнительный workshop с переносом исходных компонентов и Instance/ref probe.
Остальные пять вкладок демо сохранены.

В ORBIT встроены `LoadingDots` и `ProgressBar` из
[StudioComponents](https://github.com/sircfenner/StudioComponents), commit
`ceb9d451ec5f53adec6e539d4ea4a6a986882fd6`. Оригиналы, MIT license и список
изменений находятся в `assets/scripts/ui/studio_components/`; исходный tutorial
workshop — в `assets/scripts/ui/roblox_tutorial/`. Это перенос выбранных
компонентов с собственной игровой композицией, а не поддержка всей библиотеки.
После обновления C++/bootstrap перезапустите Release движка, откройте UI Demo
и нажмите Play. Дальнейшие изменения скриптов применяются через live reload.

`third_party/react-luau/adapters/RobloxUI.luau` реализует datatypes, Enum,
Instances и signals. `ReactTurbo.luau` связывает их с upstream reconciler;
`ReactRuntime.cpp` устанавливает globals и alias `ReactRoblox`. Native renderer
`ReactUI.cpp` читает raw-поля, вычисляет геометрию и ставит события в очередь;
пользовательские callbacks из C++ traversal не вызываются.

В компиляторе скриптов отключена только builtin/import-специализация `typeof`,
которая иначе обходила бы совместимую реализацию для host-backed tables.
Остальные оптимизации Luau сохраняются. Native обход и AutoSize ограничены
по глубине/числу узлов; неизменная геометрия повторно не аллоцируется.

Проверки: `engine_react_runtime_tests` — Instances/Enum/refs/portals/cleanup;
`engine_gameplay_state_tests` — настоящие ImGui geometry, AutoSize, сигналы,
мутации и клики; `tools/test_react_luau.py` — standalone Luau contract.

Ориентиры: [Roblox Instance](https://create.roblox.com/docs/reference/engine/classes/Instance),
[EnumItem](https://create.roblox.com/docs/reference/engine/datatypes/EnumItem),
[AutomaticSize](https://create.roblox.com/docs/reference/engine/enums/AutomaticSize),
[ReactRoblox API](https://github.com/Roblox/react-luau/blob/main/docs/api-reference/react-roblox.md).
