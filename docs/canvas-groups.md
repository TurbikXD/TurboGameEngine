# CanvasGroup: анимация целого окна

`CanvasGroup` объединяет фон, текст, картинки и дочерние элементы в один
отрисованный слой. `GroupTransparency` меняет прозрачность результата, а
`GroupColor3` — его общий оттенок. Перекрывающиеся элементы сначала сводятся
вместе: при fade они не начинают просвечивать друг через друга.

```lua
local React = require("React")
local TweenService = game:GetService("TweenService")
local e = React.createElement

local function Window(props)
    local ref = React.useRef(nil)
    React.useEffect(function()
        local tween = TweenService:Create(ref.current,
            TweenInfo.new(0.35, Enum.EasingStyle.Quint, Enum.EasingDirection.Out),
            { GroupTransparency = props.open and 0 or 1 })
        tween:Play()
        return function() tween:Destroy() end
    end, { props.open })
    return e("CanvasGroup", {
        ref = ref,
        Size = UDim2.fromOffset(420, 240),
        GroupTransparency = 1,
        GroupColor3 = Color3.new(1, 1, 1),
        Interactable = props.open,
        BackgroundColor3 = Color3.fromRGB(24, 32, 49),
    }, {
        Corner = e("UICorner", { CornerRadius = UDim.new(0, 18) }),
        Title = e("TextLabel", {
            Text = "Всё окно анимируется одним tween",
            BackgroundTransparency = 1,
            Size = UDim2.new(1, 0, 0, 60),
        }),
    })
end
```

Default `GroupTransparency = 0`, `GroupColor3 = white`. Конечная прозрачность
ограничивается диапазоном 0–1, включая overshoot Back/Elastic. Цвет — typed
`Color3`. Оба свойства работают с refs, signals, React bindings, Clone и
TweenService. Binding и tween не должны одновременно владеть одним свойством.

Не меняйте `Visible` на false до завершения fade: невидимое дерево не рисуется.
Повторный tween на то же свойство заменяет предыдущий и начинает с текущего
значения; этим можно плавно развернуть появление/закрытие посередине перехода.
При `Completed` проверяйте аргумент на `Enum.PlaybackState.Completed`, потому
что Cancel тоже доставляет событие. Уничтожайте tween/подписку в cleanup.

## Вложенность, декораторы и ввод

Группы можно вкладывать. Каждая сначала сводит своё содержимое, затем внешний
слой применяет свой оттенок и прозрачность. Исходные свойства дочерних элементов
остаются прежними. `UIGradient` и `UICorner`, непосредственно вложенные в
CanvasGroup, применяются ко всему сведённому изображению; градиент можно
анимировать по `Rotation`/`Offset`. Вложенные декораторы действуют на свои слои.

Как в Roblox, CanvasGroup наследуется прямо от `GuiObject`, а не от `Frame`.
`ClipsDescendants` всегда true; попытка отключить clipping отклоняется.
Содержимое за пределами группы обрезается. В движке сортировка по ZIndex
сохраняется внутри слоя; группа занимает своё место среди соседей.

Полностью прозрачная группа автоматически блокирует ввод потомков. Также
`Interactable = false` на предке блокирует дочерние кнопки, текстовый ввод и
прокрутку. Очередь событий повторно проверяет предков перед dispatch: клик,
накопленный перед скрытием окна, не проходит после скрытия. Cleanup-события
MouseLeave/FocusLost и property signals продолжают доставляться.

Автоблокировка прозрачного слоя и наследование Interactable — **правило этого
движка**. Roblox отделяет прозрачность от интерактивности. Для блокировки на
всём протяжении fade задайте `Interactable = false` при начале закрытия;
для показа включите его в нужный момент.

## UI Demo

Откройте **UI Demo → Play → 09 Canvas / Layers**. В демо есть цельное окно
экспедиции с PNG, текстом, stroke и градиентом; шахматный фон показывает fade
слоя. Доступны скрытие/показ, пауза/продолжение и разворот перехода, смена
GroupColor3, отдельный вложенный слой награды и Interactable. Внешние счётчики
помогают проверить, что скрытое или заблокированное окно не принимает клики.
Предыдущие восемь вкладок сохранены.

Перезапустите обновлённый Release после изменения native кода. Проверка
`require("ReactTurbo").getNativeFeatures().canvasGroups` подтверждает поддержку
текущего бинарника (native version 3); наличие новых Luau-файлов недостаточно.

## Рендеринг и ограничения

`ReactUI` захватывает ImGui geometry группы и отдаёт её через CanvasResolver.
`Renderer` / `UiCanvasCompositor` на GPU рисует дочерние слои перед родительскими
в прозрачный render target, затем переводит premultiplied RGB в straight RGBA
для обычного ImGui image shader. Оттенок, alpha, градиент и скругление группы
применяются один раз при выводе результата. GPU→CPU readback в игровом пути нет.

Render targets переиспользуются; старые размеры и draw data удерживаются до
завершения frame fence. Ограничения: до 32 слоёв за кадр, глубина групп до 8,
2048 pixels на сторону и 64 MiB для пары GPU targets всех сохранённых слоёв.
Захват геометрии также ограничен. При превышении бюджета слой пропускается
с диагностикой; прозрачность не подменяется отдельным alpha каждого ребёнка.
Для больших окон и мобильных размеров предпочтительны стабильные размеры.

Корневое ImGui-окно сохраняет прежние правила захвата мыши, в том числе при
нескольких UI roots. Visible=false убирает содержимое из traversal; чтобы
убрать сам корень и его захват ввода, размонтируйте root после Completed.

Контракты: `tests/CanvasGroupTests.luau`, `engine_react_runtime_tests`,
native ImGui checks в `engine_gameplay_state_tests` и аппаратный
`engine_canvas_gpu_tests` с pixel readback. Последний запускается явно,
как `engine_gpu_upload_tests`, и не входит в headless CTest.

```powershell
cmake --build C:/tge/clion-vs2022b-rel --config Debug --target engine_canvas_gpu_tests
& C:/tge/clion-vs2022b-rel/Debug/engine_canvas_gpu_tests.exe d3d12
& C:/tge/clion-vs2022b-rel/Debug/engine_canvas_gpu_tests.exe d3d11
```

GPU fixture проверяет реальные overlapping pixels, tint, полупрозрачные края,
вложенность, resize/fences, два слоя 2048² до 64 MiB и восстановление бюджета.
Некорректные захваты отклоняются до выделения targets.

Ориентиры, проверены 2026-10-15:
[Roblox CanvasGroup](https://create.roblox.com/docs/reference/engine/classes/CanvasGroup),
[GuiObject.Interactable](https://create.roblox.com/docs/reference/engine/classes/GuiObject#Interactable).
