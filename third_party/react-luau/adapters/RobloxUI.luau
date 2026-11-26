local originalTypeof = typeof
local knownTypes = setmetatable({}, { __mode = "k" })
local function tagged(value, name, meta)
    knownTypes[value] = name
    return table.freeze(setmetatable(value, meta))
end
local function finite(value, fallback)
    value = if value == nil then fallback else value
    assert(type(value) == "number" and value == value and math.abs(value) < math.huge, "UI value must be a finite number")
    return value
end
local function valueMeta(name, fields)
    return {
        __typeName = name,
        __tostring = function(value)
            local parts = {}
            for _, field in ipairs(fields) do table.insert(parts, tostring(value[field])) end
            return table.concat(parts, ", ")
        end,
        __eq = function(a, b)
            for _, field in ipairs(fields) do if a[field] ~= b[field] then return false end end
            return true
        end,
    }
end
local udimMeta = valueMeta("UDim", { "Scale", "Offset" })
UDim = { new = function(scale, offset) return tagged({ Scale = finite(scale, 0), Offset = finite(offset, 0) }, "UDim", udimMeta) end }
udimMeta.__add = function(a, b) return UDim.new(a.Scale + b.Scale, a.Offset + b.Offset) end
udimMeta.__sub = function(a, b) return UDim.new(a.Scale - b.Scale, a.Offset - b.Offset) end
local udim2Meta = valueMeta("UDim2", { "X", "Y" })
UDim2 = {
    new = function(xs, xo, ys, yo)
        if knownTypes[xs] == "UDim" and knownTypes[xo] == "UDim" then return tagged({ X = xs, Y = xo }, "UDim2", udim2Meta) end
        return tagged({ X = UDim.new(xs, xo), Y = UDim.new(ys, yo) }, "UDim2", udim2Meta)
    end,
    fromScale = function(x, y) return UDim2.new(x, 0, y, 0) end,
    fromOffset = function(x, y) return UDim2.new(0, x, 0, y) end,
}
udim2Meta.__add = function(a, b) return UDim2.new(a.X + b.X, a.Y + b.Y) end
udim2Meta.__sub = function(a, b) return UDim2.new(a.X - b.X, a.Y - b.Y) end
udim2Meta.__index = { Lerp = function(a, b, alpha)
    return UDim2.new(a.X.Scale + (b.X.Scale - a.X.Scale) * alpha, a.X.Offset + (b.X.Offset - a.X.Offset) * alpha,
        a.Y.Scale + (b.Y.Scale - a.Y.Scale) * alpha, a.Y.Offset + (b.Y.Offset - a.Y.Offset) * alpha)
end }
local vectorMeta = valueMeta("Vector2", { "X", "Y" })
Vector2 = { new = function(x, y) return tagged({ X = finite(x, 0), Y = finite(y, 0) }, "Vector2", vectorMeta) end }
vectorMeta.__add = function(a, b) return Vector2.new(a.X + b.X, a.Y + b.Y) end
vectorMeta.__sub = function(a, b) return Vector2.new(a.X - b.X, a.Y - b.Y) end
vectorMeta.__unm = function(a) return Vector2.new(-a.X, -a.Y) end
vectorMeta.__mul = function(a, b)
    if type(a) == "number" then return Vector2.new(a * b.X, a * b.Y) end
    if type(b) == "number" then return Vector2.new(a.X * b, a.Y * b) end
    return Vector2.new(a.X * b.X, a.Y * b.Y)
end
vectorMeta.__div = function(a, b)
    if type(b) == "number" then return Vector2.new(a.X / b, a.Y / b) end
    return Vector2.new(a.X / b.X, a.Y / b.Y)
end
vectorMeta.__index = function(value, key)
    if key == "Magnitude" then return math.sqrt(value.X * value.X + value.Y * value.Y) end
    if key == "Unit" then local length = math.sqrt(value.X * value.X + value.Y * value.Y); return if length == 0 then Vector2.zero else Vector2.new(value.X / length, value.Y / length) end
    if key == "Lerp" then return function(a, b, alpha) return a + (b - a) * alpha end end
    if key == "Dot" then return function(a, b) return a.X * b.X + a.Y * b.Y end end
end
Vector2.zero = Vector2.new(0, 0)
Vector2.one = Vector2.new(1, 1)
Vector2.xAxis = Vector2.new(1, 0)
Vector2.yAxis = Vector2.new(0, 1)
__turboVector2Metatable = table.freeze(vectorMeta)
local colorMeta = valueMeta("Color3", { "R", "G", "B" })
Color3 = {
    new = function(r, g, b)
        return tagged({ R = math.clamp(finite(r, 0), 0, 1), G = math.clamp(finite(g, 0), 0, 1), B = math.clamp(finite(b, 0), 0, 1) }, "Color3", colorMeta)
    end,
    fromRGB = function(r, g, b) return Color3.new(finite(r, 0) / 255, finite(g, 0) / 255, finite(b, 0) / 255) end,
    fromHSV = function(h, s, v)
        h = finite(h, 0) % 1; s = math.clamp(finite(s, 0), 0, 1); v = math.clamp(finite(v, 0), 0, 1)
        local sector = h * 6; local fraction = sector % 1; local p = v * (1 - s); local q = v * (1 - s * fraction); local t = v * (1 - s * (1 - fraction))
        local colors = { { v, t, p }, { q, v, p }, { p, v, t }, { p, q, v }, { t, p, v }, { v, p, q } }
        return Color3.new(table.unpack(colors[math.floor(sector) + 1]))
    end,
}
colorMeta.__index = { Lerp = function(a, b, alpha) return Color3.new(a.R + (b.R - a.R) * alpha, a.G + (b.G - a.G) * alpha, a.B + (b.B - a.B) * alpha) end }
table.freeze(udimMeta); table.freeze(udim2Meta); table.freeze(colorMeta)
UDim = table.freeze(UDim); UDim2 = table.freeze(UDim2); Vector2 = table.freeze(Vector2); Color3 = table.freeze(Color3)

local rectMeta = valueMeta("Rect", { "Min", "Max" })
rectMeta.__index = function(value, key)
    if key == "Width" then return value.Max.X - value.Min.X end
    if key == "Height" then return value.Max.Y - value.Min.Y end
end
Rect = table.freeze({ new = function(minX, minY, maxX, maxY)
    local minimum, maximum
    if type(minX) == "table" and type(minY) == "table" and getmetatable(minX) == vectorMeta and getmetatable(minY) == vectorMeta then minimum = minX; maximum = minY
    else minimum = Vector2.new(minX, minY); maximum = Vector2.new(maxX, maxY) end
    assert(minimum.X <= maximum.X and minimum.Y <= maximum.Y, "Rect minimum must not exceed maximum")
    return tagged({ Min = minimum, Max = maximum }, "Rect", rectMeta)
end })
table.freeze(rectMeta)
local colorKeypointMeta = table.freeze(valueMeta("ColorSequenceKeypoint", { "Time", "Value" }))
local numberKeypointMeta = table.freeze(valueMeta("NumberSequenceKeypoint", { "Time", "Value", "Envelope" }))
local function keyTime(value)
    assert(type(value) == "number", "Sequence keypoint time must be a number")
    value = finite(value, 0)
    assert(value >= 0 and value <= 1, "Sequence keypoint time must be between 0 and 1")
    return value
end
ColorSequenceKeypoint = table.freeze({ new = function(time, value)
    assert(knownTypes[value] == "Color3", "ColorSequenceKeypoint requires a Color3")
    return tagged({ Time = keyTime(time), Value = value }, "ColorSequenceKeypoint", colorKeypointMeta)
end })
NumberSequenceKeypoint = table.freeze({ new = function(time, value, envelope)
    assert(type(value) == "number", "NumberSequenceKeypoint value must be a number")
    value = finite(value, 0); envelope = finite(envelope, 0)
    assert(envelope >= 0, "NumberSequenceKeypoint envelope must be nonnegative")
    return tagged({ Time = keyTime(time), Value = value, Envelope = envelope }, "NumberSequenceKeypoint", numberKeypointMeta)
end })
local function sequenceMeta(name)
    return table.freeze({
        __tostring = function() return name end,
        __eq = function(a, b)
            if #a.Keypoints ~= #b.Keypoints then return false end
            for index, keypoint in ipairs(a.Keypoints) do if keypoint ~= b.Keypoints[index] then return false end end
            return true
        end,
    })
end
local colorSequenceMeta = sequenceMeta("ColorSequence")
local numberSequenceMeta = sequenceMeta("NumberSequence")
local function sequence(keys, name, meta)
    assert(type(keys) == "table", name .. " requires keypoints")
    local count = #keys
    assert(count >= 2 and count <= 20, name .. " requires 2 to 20 keypoints")
    local result, previous = {}, -1
    for index = 1, count do
        local keypoint = rawget(keys, index)
        assert(knownTypes[keypoint] == name .. "Keypoint", name .. " requires typed keypoints")
        assert(keypoint.Time > previous, name .. " keypoint times must be strictly increasing")
        previous = keypoint.Time
        result[index] = keypoint
    end
    for index in pairs(keys) do assert(type(index) == "number" and index >= 1 and index <= count and index % 1 == 0, "Sequence keypoints must be a dense array") end
    assert(result[1].Time == 0 and result[count].Time == 1, name .. " keypoints must include time 0 and time 1")
    return tagged({ Keypoints = table.freeze(result) }, name, meta)
end
ColorSequence = table.freeze({ new = function(first, last)
    if knownTypes[first] == "Color3" then
        return sequence({ ColorSequenceKeypoint.new(0, first), ColorSequenceKeypoint.new(1, if last == nil then first else last) }, "ColorSequence", colorSequenceMeta)
    end
    assert(last == nil, "ColorSequence keypoint-array overload takes one argument")
    return sequence(first, "ColorSequence", colorSequenceMeta)
end })
NumberSequence = table.freeze({ new = function(first, last)
    if type(first) == "number" then
        return sequence({ NumberSequenceKeypoint.new(0, first), NumberSequenceKeypoint.new(1, if last == nil then first else last) }, "NumberSequence", numberSequenceMeta)
    end
    assert(last == nil, "NumberSequence keypoint-array overload takes one argument")
    return sequence(first, "NumberSequence", numberSequenceMeta)
end })

local definitions = {
    FillDirection = { Horizontal = 0, Vertical = 1 },
    HorizontalAlignment = { Center = 0, Left = 1, Right = 2 },
    VerticalAlignment = { Center = 0, Top = 1, Bottom = 2 },
    TextXAlignment = { Left = 0, Right = 1, Center = 2 },
    TextYAlignment = { Top = 0, Center = 1, Bottom = 2 },
    SortOrder = { Name = 0, Custom = 1, LayoutOrder = 2 },
    AutomaticSize = { None = 0, X = 1, Y = 2, XY = 3 },
    ZIndexBehavior = { Global = 0, Sibling = 1 },
    BorderMode = { Outline = 0, Middle = 1, Inset = 2 },
    ApplyStrokeMode = { Contextual = 0, Border = 1 },
    LineJoinMode = { Round = 0, Bevel = 1, Miter = 2 },
    DominantAxis = { Width = 0, Height = 1 },
    AspectType = { FitWithinMaxSize = 0, ScaleWithParentSize = 1 },
    TextTruncate = { None = 0, AtEnd = 1, SplitWord = 2 },
    ScaleType = { Stretch = 0, Slice = 1, Tile = 2, Fit = 3, Crop = 4 },
    ScrollingDirection = { X = 1, Y = 2, XY = 4 },
    StartCorner = { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 },
    EasingStyle = { Linear = 0, Sine = 1, Back = 2, Quad = 3, Quart = 4, Quint = 5, Bounce = 6, Elastic = 7, Exponential = 8, Circular = 9, Cubic = 10 },
    EasingDirection = { In = 0, Out = 1, InOut = 2 },
    PlaybackState = { Begin = 0, Delayed = 1, Playing = 2, Paused = 3, Completed = 4, Cancelled = 5 },
    Font = { Legacy = 0, Arial = 1, ArialBold = 2, SourceSans = 3, SourceSansBold = 4, SourceSansLight = 5, SourceSansItalic = 6,
        Code = 10, SourceSansSemibold = 16, Gotham = 17, GothamMedium = 18, GothamBold = 19, GothamBlack = 20,
        Roboto = 39, RobotoMono = 41, BuilderSans = 46, BuilderSansMedium = 47, BuilderSansBold = 48, BuilderSansExtraBold = 49 },
}
local enumTypes = {}
for enumName, entries in pairs(definitions) do
    local enumType, items = {}, {}
    local enumMeta = { __tostring = function() return "Enum." .. enumName end }
    for name, value in pairs(entries) do
        local item = tagged({ Name = name, Value = value, EnumType = enumType }, "EnumItem", table.freeze({
            __tostring = function() return "Enum." .. enumName .. "." .. name end,
        }))
        enumType[name] = item
        table.insert(items, item)
    end
    table.sort(items, function(a, b) return a.Value < b.Value end)
    enumType.GetEnumItems = function() return table.clone(items) end
    enumType.FromName = function(_, name) local value = rawget(enumType, name); return if knownTypes[value] == "EnumItem" then value else nil end
    enumType.FromValue = function(_, value) for _, item in ipairs(items) do if item.Value == value then return item end end end
    enumMeta.__index = function(_, name) error("Unsupported Enum." .. enumName .. " item: " .. tostring(name), 2) end
    enumTypes[enumName] = tagged(enumType, "Enum", table.freeze(enumMeta))
end
enumTypes.GetEnums = function()
    local result = {}
    for _, value in pairs(enumTypes) do if knownTypes[value] == "Enum" then table.insert(result, value) end end
    return result
end
Enum = tagged(enumTypes, "Enums", table.freeze({ __index = function(_, name) error("Unsupported UI Enum: " .. tostring(name), 2) end }))
typeof = function(value)
    if type(value) == "table" then
        if getmetatable(value) == vectorMeta then return "Vector2" end
        local recognized = knownTypes[value]
        if recognized then return recognized end
    end
    return originalTypeof(value)
end

local hosts = {
    Window = true, ScreenGui = true, Frame = true, CanvasGroup = true, TextLabel = true, TextButton = true,
    Checkbox = true, SliderFloat = true, Separator = true, SameLine = true,
    UICorner = true, UIPadding = true, UIListLayout = true, UIStroke = true,
    Folder = true, PlayerGui = true, UIGradient = true, ImageLabel = true, ImageButton = true,
    UIScale = true, UISizeConstraint = true, UIAspectRatioConstraint = true, UIGridLayout = true, ScrollingFrame = true, TextBox = true,
}
local bases = {
    Window = "Frame", Frame = "GuiObject", CanvasGroup = "GuiObject", TextLabel = "GuiObject", TextButton = "GuiButton", GuiButton = "GuiObject",
    Checkbox = "GuiObject", SliderFloat = "GuiObject", Separator = "GuiObject", SameLine = "GuiObject",
    ImageLabel = "GuiObject", ImageButton = "GuiButton", TextBox = "GuiObject", ScrollingFrame = "GuiObject",
    GuiObject = "GuiBase2d", ScreenGui = "LayerCollector", LayerCollector = "GuiBase2d", GuiBase2d = "Instance",
    UICorner = "UIComponent", UIPadding = "UIComponent", UIListLayout = "UIGridStyleLayout", UIGridStyleLayout = "UIComponent",
    UIGradient = "UIComponent", UIScale = "UIComponent", UIGridLayout = "UIGridStyleLayout",
    UIStroke = "UIComponent", UISizeConstraint = "UIConstraint", UIAspectRatioConstraint = "UIConstraint", UIConstraint = "UIComponent", UIComponent = "Instance",
}
local events = { Changed = true, ChildAdded = true, ChildRemoved = true, AncestryChanged = true, Destroying = true }
local buttonEvents = { Activated = true, MouseButton1Click = true, MouseButton1Down = true, MouseButton1Up = true }
local guiEvents = { MouseEnter = true, MouseLeave = true }
local textBoxEvents = { Focused = true, FocusLost = true }
local common = { Name = "string", Archivable = "boolean", Parent = "Instance" }
local gui = {
    Size = "UDim2", Position = "UDim2", AnchorPoint = "Vector2", Visible = "boolean", BackgroundColor3 = "Color3",
    BackgroundTransparency = "number", BorderColor3 = "Color3", BorderSizePixel = "number", BorderMode = "Enum.BorderMode",
    LayoutOrder = "number", ZIndex = "number", ClipsDescendants = "boolean", Active = "boolean", Selectable = "boolean",
    AutomaticSize = "Enum.AutomaticSize", AbsoluteSize = "readonly", AbsolutePosition = "readonly",
    Width = "number", Height = "number", Disabled = "boolean", Interactable = "boolean", TextColor = "table", TextColor3 = "Color3",
}
local text = { Text = "string", TextColor3 = "Color3", TextColor = "table", TextTransparency = "number", TextSize = "number",
    TextWrapped = "boolean", TextScaled = "boolean", TextXAlignment = "Enum.TextXAlignment", TextYAlignment = "Enum.TextYAlignment",
    Font = "Enum.Font", RichText = "boolean", TextTruncate = "Enum.TextTruncate", TextBounds = "readonly" }
local image = { Image = "string", ImageColor3 = "Color3", ImageTransparency = "number", ImageRectOffset = "Vector2", ImageRectSize = "Vector2",
    ScaleType = "Enum.ScaleType", SliceCenter = "Rect", SliceScale = "number", TileSize = "UDim2", IsLoaded = "readonly" }
local classProps = {
    CanvasGroup = { GroupTransparency = "number", GroupColor3 = "Color3" },
    ScreenGui = { Enabled = "boolean", IgnoreGuiInset = "boolean", ResetOnSpawn = "boolean", DisplayOrder = "number", ZIndexBehavior = "Enum.ZIndexBehavior" },
    UICorner = { CornerRadius = "UDim" }, UIPadding = { PaddingLeft = "UDim", PaddingRight = "UDim", PaddingTop = "UDim", PaddingBottom = "UDim" },
    UIListLayout = { Padding = "UDim", FillDirection = "Enum.FillDirection", HorizontalAlignment = "Enum.HorizontalAlignment", VerticalAlignment = "Enum.VerticalAlignment", SortOrder = "Enum.SortOrder", AbsoluteContentSize = "readonly" },
    UIStroke = { Color = "Color3", Transparency = "number", Thickness = "number", Enabled = "boolean", ApplyStrokeMode = "Enum.ApplyStrokeMode", LineJoinMode = "Enum.LineJoinMode" },
    UIGradient = { Color = "ColorSequence", Transparency = "NumberSequence", Rotation = "number", Offset = "Vector2", Enabled = "boolean" },
    UIScale = { Scale = "number" },
    UISizeConstraint = { MinSize = "Vector2", MaxSize = "Vector2" },
    UIAspectRatioConstraint = { AspectRatio = "number", AspectType = "Enum.AspectType", DominantAxis = "Enum.DominantAxis" },
    UIGridLayout = { CellPadding = "UDim2", CellSize = "UDim2", FillDirectionMaxCells = "number", StartCorner = "Enum.StartCorner",
        FillDirection = "Enum.FillDirection", HorizontalAlignment = "Enum.HorizontalAlignment", VerticalAlignment = "Enum.VerticalAlignment", SortOrder = "Enum.SortOrder",
        AbsoluteContentSize = "readonly", AbsoluteCellCount = "readonly", AbsoluteCellSize = "readonly" },
    ScrollingFrame = { CanvasSize = "UDim2", CanvasPosition = "Vector2", AutomaticCanvasSize = "Enum.AutomaticSize", ScrollingDirection = "Enum.ScrollingDirection",
        ScrollBarThickness = "number", ScrollBarImageColor3 = "Color3", ScrollBarImageTransparency = "number", ScrollingEnabled = "boolean", AbsoluteCanvasSize = "readonly", AbsoluteWindowSize = "readonly" },
    TextBox = { TextEditable = "boolean", ClearTextOnFocus = "boolean", MultiLine = "boolean", PlaceholderText = "string", PlaceholderColor3 = "Color3" },
    Checkbox = { Checked = "boolean", Text = "string", Disabled = "boolean", Width = "number", Height = "number" },
    SliderFloat = { Value = "number", Min = "number", Max = "number", Text = "string", Disabled = "boolean", Width = "number", Height = "number" },
}
local function isA(instance, name)
    local class = instance.type
    repeat if class == name then return true end; class = bases[class] until class == nil
    return name == "Instance"
end
local function propertyType(instance, property)
    return common[property] or (classProps[instance.type] and classProps[instance.type][property])
        or (isA(instance, "GuiObject") and gui[property])
        or ((instance.type == "TextLabel" or instance.type == "TextButton" or instance.type == "TextBox") and text[property])
        or ((instance.type == "ImageLabel" or instance.type == "ImageButton") and image[property])
        or (instance.type == "ImageButton" and (property == "HoverImage" or property == "PressedImage") and "string")
        or (isA(instance, "GuiButton") and property == "AutoButtonColor" and "boolean")
end
local defaults = {
    Visible = true, Enabled = true, Archivable = true, Active = false, Selectable = false, Disabled = false,
    BackgroundTransparency = 0, BorderSizePixel = 0, LayoutOrder = 0, ZIndex = 1, Text = "", TextTransparency = 0,
    TextSize = 14, TextWrapped = false, TextScaled = false, ClipsDescendants = false, RichText = false, AutoButtonColor = true,
    AutomaticSize = Enum.AutomaticSize.None, TextXAlignment = Enum.TextXAlignment.Center, TextYAlignment = Enum.TextYAlignment.Center,
    Font = Enum.Font.SourceSans, FillDirection = Enum.FillDirection.Vertical, HorizontalAlignment = Enum.HorizontalAlignment.Left,
    VerticalAlignment = Enum.VerticalAlignment.Top, SortOrder = Enum.SortOrder.Name, IgnoreGuiInset = false, ResetOnSpawn = true,
    DisplayOrder = 0, ZIndexBehavior = Enum.ZIndexBehavior.Sibling, Thickness = 1, Transparency = 0, AspectRatio = 1,
    AspectType = Enum.AspectType.FitWithinMaxSize, DominantAxis = Enum.DominantAxis.Width,
    Size = UDim2.fromOffset(100, 100), Position = UDim2.fromOffset(0, 0), AnchorPoint = Vector2.zero,
    AbsoluteSize = Vector2.zero, AbsolutePosition = Vector2.zero, TextBounds = Vector2.zero, AbsoluteContentSize = Vector2.zero,
    BackgroundColor3 = Color3.new(1, 1, 1), BorderColor3 = Color3.new(0, 0, 0), TextColor3 = Color3.new(1, 1, 1), Color = Color3.new(1, 1, 1),
    CornerRadius = UDim.new(0, 8), Padding = UDim.new(0, 0), PaddingLeft = UDim.new(0, 0), PaddingRight = UDim.new(0, 0), PaddingTop = UDim.new(0, 0), PaddingBottom = UDim.new(0, 0),
    MinSize = Vector2.zero, MaxSize = Vector2.new(1000000, 1000000),
    Image = "", ImageColor3 = Color3.new(1, 1, 1), ImageTransparency = 0, ImageRectOffset = Vector2.zero, ImageRectSize = Vector2.zero,
    ScaleType = Enum.ScaleType.Stretch, SliceCenter = Rect.new(), SliceScale = 1, TileSize = UDim2.fromScale(1, 1),
    Scale = 1, Rotation = 0, Offset = Vector2.zero,
    CellPadding = UDim2.fromOffset(5, 5), CellSize = UDim2.fromOffset(100, 100), FillDirectionMaxCells = 0, StartCorner = Enum.StartCorner.TopLeft,
    AbsoluteCellCount = Vector2.zero, AbsoluteCellSize = Vector2.zero,
    CanvasSize = UDim2.fromOffset(0, 0), CanvasPosition = Vector2.zero, AutomaticCanvasSize = Enum.AutomaticSize.None,
    ScrollingDirection = Enum.ScrollingDirection.XY, ScrollBarThickness = 12, ScrollingEnabled = true,
    ScrollBarImageColor3 = Color3.new(0, 0, 0),
    ScrollBarImageTransparency = 0,
    AbsoluteCanvasSize = Vector2.zero, AbsoluteWindowSize = Vector2.zero,
    TextEditable = true, ClearTextOnFocus = true, MultiLine = false, PlaceholderText = "", PlaceholderColor3 = Color3.fromRGB(178, 178, 178),
    IsLoaded = false, HoverImage = "", PressedImage = "",
    GroupTransparency = 0, GroupColor3 = Color3.new(1, 1, 1),
}
local classDefaults = {
    UIGradient = { Color = ColorSequence.new(Color3.new(1, 1, 1)), Transparency = NumberSequence.new(0) },
    UIGridLayout = { FillDirection = Enum.FillDirection.Horizontal },
    ScrollingFrame = { ClipsDescendants = true },
    CanvasGroup = { ClipsDescendants = true },
}
local function defaultValue(instance, property)
    local overrides = classDefaults[instance.type]
    if overrides and overrides[property] ~= nil then return overrides[property] end
    return defaults[property]
end
local facade = { hostTypes = hosts, pendingEvents = {} }
facade.isEvent = function(instance, name) return events[name] == true or (isA(instance, "GuiObject") and guiEvents[name] == true)
    or (isA(instance, "GuiButton") and buttonEvents[name] == true) or (instance.type == "TextBox" and textBoxEvents[name] == true) end
facade.hasProperty = function(instance, name) return propertyType(instance, name) ~= nil end
facade.isReadonlyProperty = function(instance, name) return propertyType(instance, name) == "readonly" end
local instances = setmetatable({}, { __mode = "k" })
local resetCreated
local destroyObservers = {}
local treeObserver = function() end
local nextId = 0
local instanceMeta, methods = {}, {}
function methods:CaptureFocus()
    assert(self.type == "TextBox" and not self.disposed, "CaptureFocus expects a live TextBox")
    rawset(self, "_focusRequested", true); rawset(self, "_releaseFocusRequested", nil)
end
function methods:ReleaseFocus(enterPressed)
    assert(self.type == "TextBox" and not self.disposed, "ReleaseFocus expects a live TextBox")
    assert(enterPressed == nil or type(enterPressed) == "boolean", "ReleaseFocus expects an optional boolean")
    rawset(self, "_focusRequested", nil); rawset(self, "_releaseFocusRequested", enterPressed == true)
end
function methods:IsFocused()
    assert(self.type == "TextBox" and not self.disposed, "IsFocused expects a live TextBox")
    return rawget(self, "_focused") == true
end
local function signal()
    local listeners = {}
    local result = {}
    function result:Connect(callback)
        assert(type(callback) == "function", "Signal callback must be a function")
        local connection = { Connected = true }
        knownTypes[connection] = "RBXScriptConnection"
        function connection:Disconnect() self.Connected = false; listeners[self] = nil end
        listeners[connection] = callback
        return connection
    end
    function result:Once(callback)
        local connection
        connection = self:Connect(function(...) connection:Disconnect(); callback(...) end)
        return connection
    end
    function result:Wait() error("Signal:Wait is not supported by the UI facade; use Connect", 2) end
    function result._fire(...)
        local failed
        for connection, callback in pairs(table.clone(listeners)) do
            if connection.Connected then local ok, message = pcall(callback, ...); if not ok then failed = failed or message end end
        end
        if failed then error(failed) end
    end
    function result._clear() for connection in pairs(listeners) do connection:Disconnect() end end
    knownTypes[result] = "RBXScriptSignal"
    return result
end
facade.tagValue = tagged
facade.registerType = function(value, name) knownTypes[value] = name; return value end
facade.createSignal = signal
facade.bindingAssigned = function(instance, property) if facade.onBindingAssigned then facade.onBindingAssigned(instance, property) end end
facade.addDestroyObserver = function(callback)
    assert(type(callback) == "function", "Destroy observer must be a function")
    table.insert(destroyObservers, callback)
end
local function getSignal(instance, name)
    if not instance._signals[name] then instance._signals[name] = signal() end
    return instance._signals[name]
end
local function enqueue(instance, name, value, args)
    if #facade.pendingEvents < 16384 then table.insert(facade.pendingEvents, { instance = instance, name = name, value = value, args = args }) end
end
local function notify(instance, property, value)
    if instance._signals.Changed or instance.events.Changed or instance._signals["Change." .. property] or instance.events["Change." .. property] then enqueue(instance, "Change." .. property, value) end
end
local function detach(instance)
    local parent = instance.parent
    if parent then
        local index = table.find(parent.children, instance)
        if index then table.remove(parent.children, index) end
        if parent._signals.ChildRemoved or parent.events.ChildRemoved then enqueue(parent, "_Signal.ChildRemoved", nil, table.pack(instance)) end
    end
    rawset(instance, "parent", nil)
end
local function setParent(instance, parent, before)
    assert(not instance.disposed, "Cannot parent a destroyed UI Instance")
    assert(parent == nil or instances[parent], "Parent must be a UI Instance or nil")
    if parent then
        assert(not parent.disposed, "Cannot parent to a destroyed UI Instance")
        local ancestor = parent
        while ancestor do assert(ancestor ~= instance, "UI parenting cycle is not allowed"); ancestor = ancestor.parent end
    end
    if parent == instance.parent and before == nil then return end
    detach(instance)
    rawset(instance, "parent", parent)
    if parent then
        if before then local index = table.find(parent.children, before); assert(index, "React host sibling is missing"); table.insert(parent.children, index, instance)
        else table.insert(parent.children, instance) end
        if parent._signals.ChildAdded or parent.events.ChildAdded then enqueue(parent, "_Signal.ChildAdded", nil, table.pack(instance)) end
    end
    notify(instance, "Parent", parent)
    if instance._signals.AncestryChanged or instance.events.AncestryChanged then enqueue(instance, "_Signal.AncestryChanged", nil, table.pack(instance, parent)) end
    treeObserver()
end
function facade.validateProperty(instance, property, value)
    assert(not instance.disposed, "Cannot edit a destroyed UI Instance")
    local expected = propertyType(instance, property)
    assert(expected, "Unsupported " .. instance.type .. " property: " .. tostring(property))
    assert(expected ~= "readonly", property .. " is read-only")
    if instance.type == "CanvasGroup" and property == "ClipsDescendants" then assert(value ~= false, "CanvasGroup always clips its descendants") end
    assert(not ((property == "RichText" or property == "TextScaled") and value == true), property .. " rendering is not supported; use plain text and TextSize")
    if property == "TextTruncate" and value ~= nil then assert(value == Enum.TextTruncate.None or value == "None", "TextTruncate rendering is not supported") end
    if property == "LineJoinMode" and value ~= nil then assert(value == Enum.LineJoinMode.Round or value == "Round", "Only UIStroke Round joins are supported") end
    if property == "Parent" then assert(value == nil or instances[value], "Parent must be a UI Instance or nil"); return end
    if value ~= nil then
        if string.sub(expected, 1, 5) == "Enum." then
            local enumType = Enum[string.sub(expected, 6)]
            assert((knownTypes[value] == "EnumItem" and value.EnumType == enumType) or (type(value) == "string" and knownTypes[rawget(enumType, value)] == "EnumItem"), property .. " expects " .. expected)
        elseif expected == "number" then finite(value, 0)
        elseif expected == "table" then assert(type(value) == "table", property .. " expects a table")
        else assert(typeof(value) == expected, property .. " expects " .. expected) end
        if property == "Scale" or property == "SliceScale" or property == "ScrollBarThickness" then assert(value >= 0, property .. " must be nonnegative") end
        if property == "AspectRatio" then assert(value > 0, "AspectRatio must be positive") end
        if property == "FillDirectionMaxCells" then assert(value >= 0 and value % 1 == 0 and value <= 4096, "FillDirectionMaxCells must be an integer from 0 to 4096") end
        if property == "MinSize" or property == "MaxSize" or property == "ImageRectOffset" or property == "ImageRectSize" or property == "CanvasPosition" then
            assert(value.X >= 0 and value.Y >= 0, property .. " components must be nonnegative")
        end
        if property == "Image" or property == "HoverImage" or property == "PressedImage" then assert(#value <= 4096, property .. " asset reference is too long") end
        if property == "Text" or property == "PlaceholderText" then assert(#value <= 32768, property .. " is too long for a UI text field") end
    end
end
function facade.setProperty(instance, property, value)
    facade.validateProperty(instance, property, value)
    if property == "Parent" then setParent(instance, value); return end
    local previous = instance.props[property]
    if instance.type == "CanvasGroup" and property == "GroupTransparency" and value ~= nil then value = math.clamp(value, 0, 1) end
    if instance.type == "CanvasGroup" and property == "ClipsDescendants" then value = true end
    if property == "Name" and value == nil then value = instance.type end
    instance.props[property] = value
    if previous ~= value then notify(instance, property, value) end
end
function methods:IsA(className) return isA(self, className) end
function methods:GetChildren() return table.clone(self.children) end
function methods:GetDescendants()
    local result = {}
    local function visit(parent) for _, child in ipairs(parent.children) do table.insert(result, child); visit(child) end end
    visit(self); return result
end
function methods:FindFirstChild(name, recursive)
    for _, child in ipairs(self.children) do if child.Name == name then return child end end
    if recursive then for _, child in ipairs(self.children) do local found = child:FindFirstChild(name, true); if found then return found end end end
end
function methods:FindFirstChildOfClass(className) for _, child in ipairs(self.children) do if child.type == className then return child end end end
function methods:FindFirstChildWhichIsA(className, recursive)
    for _, child in ipairs(self.children) do if isA(child, className) then return child end end
    if recursive then for _, child in ipairs(self.children) do local found = child:FindFirstChildWhichIsA(className, true); if found then return found end end end
end
function methods:WaitForChild(name)
    local child = self:FindFirstChild(name)
    assert(child, "UI WaitForChild supports existing children only: " .. tostring(name)); return child
end
function methods:FindFirstAncestor(name) local parent = self.parent; while parent do if parent.Name == name then return parent end; parent = parent.parent end end
function methods:IsDescendantOf(ancestor) local parent = self.parent; while parent do if parent == ancestor then return true end; parent = parent.parent end; return false end
function methods:IsAncestorOf(descendant) return instances[descendant] == true and descendant:IsDescendantOf(self) end
function methods:GetFullName() local names = { self.Name }; local parent = self.parent; while parent do table.insert(names, 1, parent.Name); parent = parent.parent end; return table.concat(names, ".") end
function methods:GetPropertyChangedSignal(property)
    assert(propertyType(self, property), "Unsupported " .. self.type .. " property: " .. tostring(property)); return getSignal(self, "Change." .. property)
end
function facade.disconnectBindings(instance) for _, disconnect in pairs(instance.bindings) do disconnect() end; table.clear(instance.bindings) end
function methods:Destroy()
    if self.disposed or rawget(self, "_destroying") then return end
    rawset(self, "_destroying", true)
    local failure
    for _, observer in ipairs(destroyObservers) do local ok, message = pcall(observer, self); if not ok then failure = failure or message end end
    if self._signals.Destroying then local ok, message = pcall(self._signals.Destroying._fire); if not ok then failure = message end end
    if self.events.Destroying then local ok, message = pcall(self.events.Destroying, self); if not ok then failure = failure or message end end
    detach(self)
    self.disposed = true
    facade.disconnectBindings(self)
    table.clear(self.events)
    for _, child in ipairs(table.clone(self.children)) do local ok, message = pcall(methods.Destroy, child); if not ok then failure = failure or message end end
    table.clear(self.children)
    for _, current in pairs(self._signals) do current._clear() end
    treeObserver()
    if failure then error(failure) end
end
function methods:ClearAllChildren() for _, child in ipairs(table.clone(self.children)) do child:Destroy() end end
function methods:Clone()
    assert(not self.disposed, "Cannot clone a destroyed UI Instance")
    if not self.Archivable then return nil end
    local clone = facade.create(self.type)
    clone.props = table.clone(self.props)
    for _, child in ipairs(self.children) do local copy = child:Clone(); if copy then setParent(copy, clone) end end
    return clone
end
instanceMeta.__index = function(instance, key)
    if key == "parent" then return nil end
    if key == "ClassName" then return instance.type end
    if key == "Parent" then return instance.parent end
    if methods[key] then return methods[key] end
    if facade.isEvent(instance, key) then return getSignal(instance, key) end
    if propertyType(instance, key) then
        local value = instance.props[key]
        if value == nil and key == "Active" and isA(instance, "GuiButton") then return true end
        if value == nil and key == "Interactable" then return true end
        return if value == nil then defaultValue(instance, key) else value
    end
    local child = methods.FindFirstChild(instance, key)
    if child then return child end
    error("Unsupported " .. instance.type .. " member: " .. tostring(key), 2)
end
instanceMeta.__newindex = function(instance, key, value)
    if key == "ClassName" then error("ClassName is read-only", 2) end
    facade.setProperty(instance, key, value)
end
instanceMeta.__tostring = function(instance) return instance.props.Name end
function facade.create(className)
    assert(hosts[className], "Unsupported Roblox UI class: " .. tostring(className))
    assert(not resetCreated or #resetCreated < 16384, "UI reset creation limit exceeded")
    nextId += 1
    local instance = { type = className, id = nextId, children = {}, props = { Name = className, Archivable = true }, events = {}, bindings = {}, hidden = false, disposed = false, _signals = {} }
    if className == "CanvasGroup" then instance.props.ClipsDescendants = true end
    instances[instance] = true; knownTypes[instance] = "Instance"
    if resetCreated then table.insert(resetCreated, instance) end
    return setmetatable(instance, instanceMeta)
end
local defaultTemplates = {}
function facade.applyDefaults(instance)
    local className = instance.type
    local template = defaultTemplates[className]
    if not template then
        template = {}
        for property in pairs(defaults) do
            local expected = propertyType(instance, property)
            if expected and expected ~= "readonly" then template[property] = defaultValue(instance, property) end
        end
        if isA(instance, "GuiObject") then template.Active = isA(instance, "GuiButton"); template.Interactable = true end
        if className == "TextLabel" or className == "TextButton" or className == "TextBox" then template.TextColor3 = Color3.new(0, 0, 0) end
        template = table.freeze(template)
        defaultTemplates[className] = template
    end
    for property, value in pairs(template) do instance.props[property] = value end
    rawset(instance, "_defaultProps", template)
    return instance
end
local gatedInput = { Activated = true, MouseButton1Click = true, MouseButton1Down = true, MouseButton1Up = true, MouseEnter = true, Focused = true }
local function canDispatchInput(instance)
    local current = instance
    local depth = 0
    while current do
        if type(current) ~= "table" or depth > 64 then return false end
        depth += 1
        local props = rawget(current, "props")
        if type(props) ~= "table" then return false end
        if rawget(current, "disposed") or rawget(current, "hidden") or rawget(props, "Visible") == false or rawget(props, "Disabled") == true or rawget(props, "Interactable") == false then return false end
        local className = rawget(current, "type")
        if className == "ScreenGui" and rawget(props, "Enabled") == false then return false end
        if className == "CanvasGroup" then
            if rawget(current, "_canvasReady") == false then return false end
            local transparency = rawget(props, "GroupTransparency")
            if type(transparency) == "number" and transparency >= 1 then return false end
        end
        current = rawget(current, "parent")
    end
    return true
end
function facade.dispatch(instance, name, value, args)
    if instance.disposed then return end
    if gatedInput[name] and not canDispatchInput(instance) then return end
    if string.sub(name, 1, 7) == "Change." then
        local property = string.sub(name, 8)
        local changed = instance._signals[name]
        if changed then changed._fire() end
        if instance._signals.Changed then instance._signals.Changed._fire(property) end
        if instance.events.Changed then instance.events.Changed(instance, property) end
    elseif string.sub(name, 1, 8) == "_Signal." then
        local eventName = string.sub(name, 9)
        local current = instance._signals[eventName]
        if current then current._fire(table.unpack(args or {}, 1, if args then args.n else 0)) end
        if instance.events[eventName] then instance.events[eventName](instance, table.unpack(args or {}, 1, if args then args.n else 0)) end
        return
    else
        local current = instance._signals[name]
        if current then
            if args then current._fire(table.unpack(args, 1, args.n or #args)) else current._fire(value) end
        end
    end
    local callback = instance.events[name]
    if callback then
        if args and string.sub(name, 1, 7) ~= "Change." then callback(instance, table.unpack(args, 1, args.n or #args)) else callback(instance, value) end
    end
end
facade.setParent = setParent
facade.isInstance = function(value) return instances[value] == true end
facade.setTreeObserver = function(observer) treeObserver = observer end
Instance = table.freeze({ new = function(className, parent)
    local instance = facade.applyDefaults(facade.create(className))
    if parent ~= nil then setParent(instance, parent) end
    return instance
end })
facade.playerGui = facade.create("PlayerGui")
facade.playerGui.props.Name = "PlayerGui"
local players = { LocalPlayer = { Name = "LocalPlayer", PlayerGui = facade.playerGui } }
function players.LocalPlayer:FindFirstChild(name) return if name == "PlayerGui" then facade.playerGui else nil end
function players.LocalPlayer:WaitForChild(name) assert(name == "PlayerGui", "Only LocalPlayer.PlayerGui is available"); return facade.playerGui end
facade.reset = function()
    assert(not resetCreated, "UI reset is already in progress")
    local failure
    local original = {}
    for instance in pairs(instances) do table.insert(original, instance) end
    local function disposeAll()
        for _, instance in ipairs(original) do
            if not instance.disposed then local ok, message = pcall(methods.Destroy, instance); if not ok then failure = failure or message end end
        end
        local index = 1
        while index <= #resetCreated do
            local instance = resetCreated[index]
            if not instance.disposed then local ok, message = pcall(methods.Destroy, instance); if not ok then failure = failure or message end end
            index += 1
        end
    end
    resetCreated = {}
    local ok, message = pcall(disposeAll)
    resetCreated = nil
    if not ok then failure = failure or message end
    table.clear(facade.pendingEvents)
    facade.playerGui = facade.create("PlayerGui")
    players.LocalPlayer.PlayerGui = facade.playerGui
    if failure then error(failure) end
end
local previousGetService = game.GetService
game.GetService = function(self, name) if name == "Players" then return players end; return previousGetService(self, name) end
__turboRobloxUI = facade
