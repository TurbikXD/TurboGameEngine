-- Real React Luau hooks, rendered by the engine's ReactTurbo host.
local React = require("React")
local ReactTurbo = require("ReactTurbo")
local e = React.createElement

local function Counter()
    local count, setCount = React.useState(0)
    local visible, setVisible = React.useState(true)
    React.useEffect(function()
        print("Counter mounted")
        return function() print("Counter unmounted") end
    end, {})
    return e("Frame", {}, {
        Title = e("TextLabel", { Text = "Real React Luau in TurboGameEngine" }),
        Count = e("TextLabel", { Text = "Clicks: " .. count, Visible = visible }),
        Button = e("TextButton", {
            Text = "Increment", Width = 160,
            [React.Event.Activated] = function()
                setCount(function(previous) return previous + 1 end)
            end,
        }),
        ShowCount = e("Checkbox", {
            Text = "Show counter", Checked = visible,
            [React.Change.Checked] = function(_, checked) setVisible(checked) end,
        }),
    })
end

-- Executing this demo again replaces its previous root and runs effect cleanup.
if turboCounterRoot then turboCounterRoot:unmount() end
turboCounterRoot = ReactTurbo.createRoot({ Name = "Luau UI Demo" })
turboCounterRoot:render(e(Counter))
return turboCounterRoot
