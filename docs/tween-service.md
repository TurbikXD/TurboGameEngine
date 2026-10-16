# TweenService для UI

`game:GetService("TweenService")` анимирует свойства UI Instances в обоих режимах
Play. Используются обычные Roblox API `TweenInfo`, `Enum.EasingStyle`,
`Enum.EasingDirection` и `Enum.PlaybackState`. Изменения скриптов работают через
существующий live reload; после обновления самого движка нужен один перезапуск.

```lua
local TweenService = game:GetService("TweenService")
local tween = TweenService:Create(window, TweenInfo.new(
    0.4, Enum.EasingStyle.Back, Enum.EasingDirection.Out
), {
    Position = UDim2.fromScale(0.5, 0.5),
    Size = UDim2.fromOffset(420, 260),
    BackgroundTransparency = 0,
})
local completed = tween.Completed:Connect(function(playbackState)
    if playbackState == Enum.PlaybackState.Completed then
        print("Window opened")
    end
end)
tween:Play()
-- Cleanup: completed:Disconnect(); tween:Destroy()
```

## Параметры и управление

`TweenInfo.new(time, style, direction, repeatCount, reverses, delayTime)` создаёт
неизменяемое значение. Defaults: `1, Quad, Out, 0, false, 0`.
`RepeatCount` задаёт дополнительные циклы; `-1` повторяет бесконечно.
`Reverses=true` возвращает объект к исходным значениям после прямого хода.
`DelayTime` применяется перед каждым циклом. Значения времени конечные и >= 0.

Поддерживаются все 11 стилей: Linear, Sine, Quad, Cubic, Quart, Quint, Circular,
Exponential, Back, Bounce и Elastic; направления In, Out и InOut.
`TweenService:GetValue(alpha, style, direction)` позволяет строить собственные
кривые. Входной alpha ограничивается диапазоном 0–1; Back/Elastic могут дать
overshoot на выходе.

| API | Поведение |
| --- | --- |
| `tween:Play()` | Запускает с текущих значений свойств; продолжает после Pause. Повторный вызов при Playing/Delayed ничего не меняет. |
| `tween:Pause()` | Сохраняет прогресс Playing. В состоянии Delayed ничего не делает. |
| `tween:Cancel()` | Сбрасывает прогресс, оставляет текущие свойства; следующий Play начинает полный цикл заново. |
| `tween:Destroy()` | Убирает анимацию и подписки без Completed; безопасен при cleanup после Stop. |
| `tween.PlaybackState` | Begin, Delayed, Playing, Paused, Completed или Cancelled. |
| `tween.Completed:Connect(fn)` | Получает Completed либо Cancelled. Pause не вызывает Completed. |
| `tween:GetPropertyChangedSignal("PlaybackState")` | Уведомляет об изменении состояния. Также есть `Changed`. |

Если два tween претендуют на одно свойство, новый Play отменяет старый tween
целиком. Анимации разных свойств одного объекта работают одновременно.
После завершения можно повторно вызвать Play. Goals копируются при Create;
начала свойств снимаются при Play. События доставляются защищённым pump.

## React и градиенты

Создавайте tween через ref в эффекте и уничтожайте его в cleanup:

```lua
local React = require("React")
local TweenService = game:GetService("TweenService")

local function AnimatedGradient()
    local gradient = React.useRef(nil)
    React.useEffect(function()
        local tween = TweenService:Create(gradient.current,
            TweenInfo.new(3, Enum.EasingStyle.Sine,
                Enum.EasingDirection.InOut, -1, true), {
                Rotation = 90,
                Offset = Vector2.new(0.4, 0),
            })
        tween:Play()
        return function() tween:Destroy() end
    end, {})
    return React.createElement("Frame", {
        Size = UDim2.fromOffset(360, 180),
    }, {
        Gradient = React.createElement("UIGradient", {
            ref = gradient,
            Color = ColorSequence.new(Color3.fromRGB(50, 220, 230),
                Color3.fromRGB(130, 65, 240)),
            Rotation = 0,
            Offset = Vector2.zero,
        }),
    })
end
```

Повторный React render с неизменёнными authored props не сбрасывает текущую
анимацию. Изменённые props по-прежнему применяются. У одного свойства должен
быть один источник анимации: Create/Play отклоняет свойство с React binding,
а назначение binding во время tween отменяет его. Для hover можно запускать
короткие tween цвета, `UIScale.Scale`, размеров и прозрачности из MouseEnter /
MouseLeave; новый Play заменяет прежний переход.

`UIGradient.Rotation` и `.Offset` анимируются. `ColorSequence` и
`NumberSequence` можно заменять обычными props, но напрямую интерполировать
через TweenService нельзя. Поддерживаемые типы целей: **number, UDim, UDim2,
Vector2, Color3**, только для валидных записываемых свойств UI. Не поддержаны
3D/CFrame, bool/Enum/Rect goals, SmoothDamp и ожидающий `Completed:Wait()`.
Используйте Connect/Once. Целочисленное `FillDirectionMaxCells` не tweenable.

## UI Demo и проверка

Откройте **UI Demo → Play → 08 MOTION**. Вкладка показывает переходы между
двумя сценами, полёт корабля с выбором easing/direction/duration, Play/Pause/
Cancel, repeat/reverse, hover картинки-кнопки, движущийся градиент и появление /
закрытие окна через portal. Остальные семь вкладок сохранены.
Для fade целого окна одним свойством используйте
[CanvasGroup.GroupTransparency](canvas-groups.md); девятая вкладка показывает
такое окно и вложенные слои с общим оттенком.

Все анимации обновляет один pump, без отдельной Heartbeat-подписки на tween и
без React render на каждом кадре. Destroy целевого Instance, unmount и Stop
освобождают владельцев и подписки; reset отбрасывает старые события. Ограничения:
512 активных/приостановленных tween, 64 goal properties и 2048 pending events.
Большое dt обрабатывается расчётом фазы, без цикла по пропущенным повторениям.
Бесконечный tween с нулевым Time и DelayTime отклоняется.

Реализация: `third_party/react-luau/adapters/TweenService.luau`, подключение
после `RobloxUI.luau` в `ReactRuntime.cpp`, pump/reset в `ReactTurbo.luau`.
Upstream React не изменён. Контракты: `tests/TweenServiceTests.luau` через
`tools/test_react_luau.py` и `engine_react_runtime_tests`; native геометрия и
градиенты — `engine_gameplay_state_tests`.

Ориентиры API, проверены 2026-10-15:
[TweenService](https://create.roblox.com/docs/reference/engine/classes/TweenService),
[TweenBase](https://create.roblox.com/docs/reference/engine/classes/TweenBase),
[TweenInfo](https://create.roblox.com/docs/reference/engine/datatypes/TweenInfo).
Это UI-подмножество движка; перечисленные ограничения остаются явными.
