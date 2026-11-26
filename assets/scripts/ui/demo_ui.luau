local React = require("React")
local ReactTurbo = require("ReactTurbo")
local e = React.createElement
local UDim = ReactTurbo.UDim
local UDim2 = ReactTurbo.UDim2
local Color3 = ReactTurbo.Color3
local Behavior = {
    fields = {
        title = { type = "string", default = "Luau Collectathon", label = "HUD Title", category = "Content", description = "Live heading used when Use Game Title is disabled, or the controller has not published a title." },
        useGameTitle = { type = "boolean", default = false, label = "Use Game Title", category = "Content", description = "Live toggle to use the controller's Game Title status instead of HUD Title." },
        showControls = { type = "boolean", default = true, label = "Show Controls", category = "Display", description = "Live visibility of the movement and restart hint; Allow Restart changes the hint text." },
        showCounter = { type = "boolean", default = true, label = "Show Click Counter", category = "Display", description = "Live visibility of the React counter button; hiding it preserves its hook state." },
        showProgress = { type = "boolean", default = true, label = "Show Progress", category = "Display", description = "Live visibility of the score progress bar." },
        showTimer = { type = "boolean", default = true, label = "Show Timer", category = "Display", description = "Live visibility of the round countdown label." },
        showResult = { type = "boolean", default = true, label = "Show Result", category = "Display", description = "Live visibility of the session result label." },
        width = { type = "integer", default = 340, min = 280, max = 640, step = 10, label = "Card Width", category = "Appearance", description = "Live HUD card width in pixels; does not change the scene viewport." },
        opacity = { type = "number", default = 0.94, min = 0.15, max = 1, step = 0.01, label = "Card Opacity", category = "Appearance", description = "Live card background opacity; text and buttons keep their own authored opacity." },
    },
}

local function HUD(props)
    local clicks, setClicks = React.useState(0)
    local status = props.status
    local fields = props.fields
    local score = status.score or 0
    local goal = math.max(1, status.goal or 10)
    local cardHeight = 320
        - (if fields.showProgress then 0 else 17)
        - (if fields.showTimer then 0 else 33)
        - (if fields.showResult then 0 else 33)
        - (if fields.showCounter then 0 else 47)
        - (if fields.showControls then 0 else 29)
    local label = function(text, order, height, color, size)
        return e("TextLabel", {
            Text = text, LayoutOrder = order,
            Size = UDim2.new(1, 0, 0, height), BackgroundTransparency = 1,
            TextColor3 = color or Color3.fromRGB(211, 220, 238),
            TextSize = size or 16, TextXAlignment = "Left",
        })
    end
    return e("ScreenGui", {}, {
        Card = e("Frame", {
            Size = UDim2.fromOffset(fields.width, cardHeight), Position = UDim2.fromOffset(12, 12),
            BackgroundColor3 = Color3.fromRGB(19, 25, 39), BackgroundTransparency = 1 - fields.opacity,
            BorderSizePixel = 1, BorderColor3 = Color3.fromRGB(61, 79, 109),
        }, {
            Corner = e("UICorner", { CornerRadius = UDim.new(0, 14) }),
            Padding = e("UIPadding", {
                PaddingLeft = UDim.new(0, 16), PaddingRight = UDim.new(0, 16),
                PaddingTop = UDim.new(0, 16), PaddingBottom = UDim.new(0, 16),
            }),
            Layout = e("UIListLayout", { Padding = UDim.new(0, 9), SortOrder = "LayoutOrder" }),
            Badge = label("TURBO / LUAU + REACT", 1, 18, Color3.fromRGB(100, 211, 242), 13),
            Title = label(if fields.useGameTitle then status.title or fields.title else fields.title, 2, 32, Color3.fromRGB(245, 249, 255), 23),
            Score = label("COLLECTED   " .. score .. " / " .. goal, 3, 26, nil, 19),
            Progress = if fields.showProgress then e("Frame", {
                LayoutOrder = 4, Size = UDim2.new(1, 0, 0, 8),
                BackgroundColor3 = Color3.fromRGB(43, 54, 76), BorderSizePixel = 0,
            }, {
                Corner = e("UICorner", { CornerRadius = UDim.new(0.5, 0) }),
                Fill = e("Frame", {
                    Size = UDim2.fromScale(math.clamp(score / goal, 0, 1), 1),
                    BackgroundColor3 = Color3.fromRGB(79, 211, 178), BorderSizePixel = 0,
                }, { Corner = e("UICorner", { CornerRadius = UDim.new(0.5, 0) }) }),
            }) else nil,
            Time = if fields.showTimer then label(string.format("TIME LEFT   %.1f s", status.remaining or 0), 5, 24) else nil,
            Result = if fields.showResult then label("SESSION   " .. (status.result or "Starting"), 6, 24) else nil,
            Button = if fields.showCounter then e("TextButton", {
                LayoutOrder = 7, Text = "React.useState clicks: " .. clicks,
                Size = UDim2.new(1, 0, 0, 38), TextSize = 16,
                BackgroundColor3 = Color3.fromRGB(54, 103, 219),
                TextColor3 = Color3.fromRGB(255, 255, 255), BorderSizePixel = 0,
                [React.Event.Activated] = function()
                    setClicks(function(previous) return previous + 1 end)
                end,
            }, { Corner = e("UICorner", { CornerRadius = UDim.new(0, 8) }) }) else nil,
            Controls = if fields.showControls then label(
                if status.allowRestart == false then "WASD / arrows: move" else "WASD / arrows: move    R: restart",
                8, 20, Color3.fromRGB(144, 161, 190), 13) else nil,
        }),
    })
end

function Behavior:Init()
    if self.state.root then self.state.root:unmount() end
    self.state.root = ReactTurbo.createRoot({ Name = self.fields.title })
end

function Behavior:Update(_dt)
    if not self.state.root then self:Init() end
    self.state.root:render(e(HUD, { fields = self.fields, status = Engine.getStatus() }))
end

function Behavior:Destroy()
    if self.state.root then self.state.root:unmount(); self.state.root = nil end
end

return Behavior
