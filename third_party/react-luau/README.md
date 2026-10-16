# React Luau in TurboGameEngine

`packages/` contains **unmodified** production source from the MIT repositories
listed in `upstream.json`. React Luau is 17.3.11, pinned to
`5f31aba406f25b4fc28a95df73c792c6b690af38`. `upstream.json` records each original
file's SHA-256. Regenerate with `python tools/vendor_react_luau.py`; a normal build
does not download React packages. Licenses are retained in `licenses/`.

`adapters/` belongs to TurboGameEngine. It supplies a ModuleScript-style require
hierarchy, SafeFlags default getters, a frame-pumped task queue, Heartbeat and
string quoting/URL escaping used by upstream diagnostics. It does not emulate
the Roblox engine. `ReactTurbo` is our host renderer using the real upstream
ReactReconciler; ReactRoblox itself requires Roblox Instances and is not loaded.

Supported hosts: Window, ScreenGui, Frame, TextLabel, TextButton, Checkbox,
SliderFloat, Separator, SameLine, UICorner, UIPadding, UIListLayout and UIStroke;
Folder/PlayerGui provide structural UI containers.
`ReactTurbo.UDim`, `.UDim2`, `.Vector2` and `.Color3` construct values for
Roblox-style Size/Position/AnchorPoint, colors, padding and list layouts.
The same constructors are global for portable Roblox components. The custom
`adapters/RobloxUI.luau` provides UI Instance refs/properties/signals and typed
Enum values; `require("ReactRoblox")` exposes synchronous roots and portals.
Legacy string alignment/direction values remain supported. Styling includes
backgrounds/borders/corners and text size/color/alignment/wrapping.
See `docs/lab2/README.md` for the supported subset and limitations.
See `docs/roblox-ui-compatibility.md` for portability and lifecycle contracts.
See `docs/roblox-ui-visuals.md` for gradients, local images, grid/scroll/text
input, constraints and the native capability guard. Sequence/keypoint/Rect
datatypes are custom facade values; upstream React sources stay unchanged.
Legacy props include Text, Name, Width, Height,
Visible, Disabled, Checked, Value, Min, Max and TextColor (an RGB table).
`[React.Event.Activated]` and `[React.Event.MouseButton1Click]` receive the host
instance; `Changed` and `[React.Change.Value]` / `[React.Change.Checked]` also
receive the updated value. Core hooks, keyed reconciliation, refs, context and
effect cleanup are provided by upstream React, with bindings handled by our host.
Stopping Play cancels game-owned timers, deferred tasks and Heartbeat listeners.
The custom `adapters/TweenService.luau` runs after RobloxUI and adds UI-only
TweenService/TweenInfo, easing, lifecycle and property ownership. ReactTurbo
pumps/reset tweens and preserves imperatively animated values when authored
React props have not changed. See `docs/tween-service.md` for supported types,
binding restrictions, bounds and the MOTION demo. Upstream React stays intact.
CanvasGroup adds GroupTransparency/GroupColor3 and forced clipping to the UI
facade; native version 3 resolves the whole subtree as a flattened GPU layer.
See `docs/canvas-groups.md` for modifiers, nested layers, input policy and bounds.
The upstream Scheduler keeps its separate host queue so new roots remain usable.

```lua
local React = require("React")
local ReactTurbo = require("ReactTurbo")
local function App()
    local count, setCount = React.useState(0)
    return React.createElement("TextButton", {
        Text = "Count: " .. count,
        [React.Event.Activated] = function()
            setCount(function(previous) return previous + 1 end)
        end,
    })
end
local root = ReactTurbo.createRoot({ Name = "My UI" })
root:render(React.createElement(App))
-- root:unmount() runs effect cleanup and releases bindings.
```
