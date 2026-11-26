local React = require("React")
local UI = require("ReactTurbo")
local e = React.createElement
local ReactRoblox = require("ReactRoblox")
assert(typeof(Enum) == "Enums" and typeof(Enum.FillDirection) == "Enum" and typeof(Enum.FillDirection.Horizontal) == "EnumItem", "Enum types were not recognized")
assert(Enum.TextXAlignment.Center.Name == "Center" and Enum.TextXAlignment.Center.Value == 2, "Roblox Enum identities differ")
assert(Enum.SortOrder.LayoutOrder.Value == 2 and Enum.FillDirection.Vertical.EnumType == Enum.FillDirection, "Enum metadata is incomplete")
assert(tostring(Enum.FillDirection.Vertical) == "Enum.FillDirection.Vertical", "Enum tostring is not portable")
local enumItems = Enum.AutomaticSize:GetEnumItems()
assert(#enumItems == 4 and enumItems[1] == Enum.AutomaticSize.None and Enum.AutomaticSize:FromValue(3) == Enum.AutomaticSize.XY, "Enum catalog lookup failed")
table.clear(enumItems)
assert(#Enum.AutomaticSize:GetEnumItems() == 4, "Enum item listing exposed mutable catalog")
assert(not pcall(function() Enum.FillDirection.Vertical.Value = 999 end) and not pcall(function() return Enum.MadeUp end), "Unsupported/mutable Enum was accepted")
assert(not pcall(Instance.new, "Part") and not pcall(function() game:GetService("Workspace") end), "UI facade must not pretend to implement non-UI Roblox APIs")
assert(typeof(UDim.new(0, 1)) == "UDim" and typeof(UDim2.fromOffset(1, 2)) == "UDim2", "Global dimensions are not typed")
assert(typeof(Vector2.zero) == "Vector2" and typeof(Color3.fromRGB(12, 34, 56)) == "Color3", "Global vector/color types failed")
assert((UDim2.fromScale(1, 1) + UDim2.fromOffset(5, 6)).X.Offset == 5 and (Vector2.new(3, 4)).Magnitude == 5, "Portable datatype arithmetic failed")
assert(Color3.new(0, 0, 0):Lerp(Color3.new(1, 1, 1), 0.5).G == 0.5 and Color3.fromHSV(0, 1, 1).R == 1, "Color helpers failed")
local red, blue = Color3.new(1, 0, 0), Color3.new(0, 0, 1)
local gradientColors = ColorSequence.new(red, blue)
assert(typeof(gradientColors) == "ColorSequence" and typeof(gradientColors.Keypoints[1]) == "ColorSequenceKeypoint" and gradientColors.Keypoints[2].Value == blue, "ColorSequence constructors/types failed")
assert(gradientColors == ColorSequence.new({ ColorSequenceKeypoint.new(0, red), ColorSequenceKeypoint.new(1, blue) }), "Color sequence value equality failed")
local gradientAlpha = NumberSequence.new({ NumberSequenceKeypoint.new(0, 0), NumberSequenceKeypoint.new(0.5, 0.2, 0.1), NumberSequenceKeypoint.new(1, 1) })
assert(typeof(gradientAlpha) == "NumberSequence" and typeof(gradientAlpha.Keypoints[1]) == "NumberSequenceKeypoint" and gradientAlpha.Keypoints[2].Envelope == 0.1, "NumberSequence constructors/types failed")
assert(not pcall(function() gradientColors.Keypoints[1].Time = 0.1 end) and not pcall(function() gradientColors.Keypoints[1] = gradientColors.Keypoints[2] end), "Sequence/keypoint arrays must be immutable")
local sourceKeypoints = { ColorSequenceKeypoint.new(0, red), ColorSequenceKeypoint.new(1, blue) }
local copiedSequence = ColorSequence.new(sourceKeypoints)
sourceKeypoints[1] = ColorSequenceKeypoint.new(0, blue)
assert(copiedSequence.Keypoints[1].Value == red, "Sequence retained its caller's mutable array")
assert(not pcall(ColorSequenceKeypoint.new, 0 / 0, red) and not pcall(NumberSequenceKeypoint.new, 0, math.huge), "Non-finite sequence keypoints were accepted")
assert(not pcall(ColorSequenceKeypoint.new, 2, red) and not pcall(NumberSequenceKeypoint.new, 0, 0, -1), "Invalid sequence time/envelope was accepted")
assert(not pcall(ColorSequence.new, { { Time = 0, Value = red }, { Time = 1, Value = blue } }), "Untyped sequence keypoints were accepted")
assert(not pcall(NumberSequence.new, { NumberSequenceKeypoint.new(0.1, 0), NumberSequenceKeypoint.new(1, 1) }), "Sequence without endpoint zero was accepted")
assert(not pcall(NumberSequence.new, { NumberSequenceKeypoint.new(0, 0), NumberSequenceKeypoint.new(0, 1), NumberSequenceKeypoint.new(1, 1) }), "Repeated/out-of-order sequence times were accepted")
local tooManyKeypoints = {}
for index = 0, 20 do table.insert(tooManyKeypoints, NumberSequenceKeypoint.new(index / 20, index / 20)) end
assert(not pcall(NumberSequence.new, tooManyKeypoints), "Sequence keypoint budget was not enforced")
local slice = Rect.new(Vector2.new(4, 5), Vector2.new(20, 30))
assert(typeof(slice) == "Rect" and slice.Width == 16 and slice.Height == 25 and Rect.new(4, 5, 20, 30) == slice, "Rect constructors/type/equality failed")
assert(not pcall(Rect.new, 8, 5, 4, 10) and not pcall(function() slice.Min = Vector2.zero end), "Malformed/mutable Rect was accepted")
assert(Enum.ScaleType.Crop.Value == 4 and Enum.ScrollingDirection.XY.Value == 4 and Enum.StartCorner.BottomRight.Value == 3, "New UI enum numeric identities differ from Roblox")

local extendedRoot = ReactRoblox.createRoot(Instance.new("Folder"))
local imageRef, gradientRef, textBoxRef, scrollRef = React.createRef(), React.createRef(), React.createRef(), React.createRef()
local colorBinding, updateColors = React.createBinding(gradientColors)
local hoverPosition, downPosition, focusLost, changedText = nil, nil, nil, nil
local function extendedElements(text)
    return e("Frame", { Size = UDim2.fromOffset(400, 300) }, {
        Gradient = e("UIGradient", { ref = gradientRef, Color = colorBinding, Transparency = gradientAlpha, Rotation = 35, Offset = Vector2.new(0.1, -0.2) }),
        Scale = e("UIScale", { Scale = 1.25 }),
        Bounds = e("UISizeConstraint", { MinSize = Vector2.new(100, 80), MaxSize = Vector2.new(800, 600) }),
        Aspect = e("UIAspectRatioConstraint", { AspectRatio = 4 / 3, DominantAxis = Enum.DominantAxis.Width, AspectType = Enum.AspectType.FitWithinMaxSize }),
        Image = e("ImageButton", {
            ref = imageRef, Image = "textures/local.png", HoverImage = "textures/hover.png", PressedImage = "textures/down.png",
            Size = UDim2.fromOffset(120, 80), ImageColor3 = red, ImageTransparency = 0.25, ImageRectOffset = Vector2.new(1, 2), ImageRectSize = Vector2.new(32, 32),
            ScaleType = Enum.ScaleType.Slice, SliceCenter = slice, SliceScale = 0.5, TileSize = UDim2.fromOffset(32, 32),
            [React.Event.MouseEnter] = function(_, x, y) hoverPosition = Vector2.new(x, y) end,
            [React.Event.MouseButton1Down] = function(_, x, y) downPosition = Vector2.new(x, y) end,
        }),
        Input = e("TextBox", {
            ref = textBoxRef, Text = text, TextEditable = true, ClearTextOnFocus = false, MultiLine = true, PlaceholderText = "Write", PlaceholderColor3 = blue,
            [React.Event.FocusLost] = function(_, submitted) focusLost = submitted end,
            [React.Change.Text] = function(_, value) changedText = value end,
        }),
        Scroll = e("ScrollingFrame", { ref = scrollRef, Size = UDim2.fromOffset(200, 120), CanvasSize = UDim2.fromOffset(300, 500), CanvasPosition = Vector2.new(10, 20),
            AutomaticCanvasSize = Enum.AutomaticSize.Y, ScrollingDirection = Enum.ScrollingDirection.XY, ScrollBarThickness = 6, ScrollingEnabled = true }, {
            Grid = e("UIGridLayout", { CellSize = UDim2.fromOffset(50, 30), CellPadding = UDim2.fromOffset(6, 8), FillDirection = Enum.FillDirection.Horizontal,
                FillDirectionMaxCells = 3, StartCorner = Enum.StartCorner.TopLeft, SortOrder = Enum.SortOrder.LayoutOrder }),
            Icon = e("ImageLabel", { Image = "textures/local.png", ScaleType = Enum.ScaleType.Fit }),
        }),
    })
end
extendedRoot:render(extendedElements("First"))
local imageInstance, gradientInstance = imageRef.current, gradientRef.current
assert(not textBoxRef.current:IsFocused(), "Fresh TextBox must not report native focus")
textBoxRef.current:CaptureFocus()
assert(rawget(textBoxRef.current, "_focusRequested") == true, "TextBox capture request did not reach the native protocol")
textBoxRef.current:ReleaseFocus(false)
assert(rawget(textBoxRef.current, "_focusRequested") == nil and rawget(textBoxRef.current, "_releaseFocusRequested") == false,
    "ReleaseFocus(false) was lost as a missing request")
assert(not pcall(function() imageInstance:CaptureFocus() end), "Non-TextBox accepted a focus request")
assert(imageInstance:IsA("GuiButton") and textBoxRef.current:IsA("GuiObject") and scrollRef.current.Grid:IsA("UIGridStyleLayout"), "Expanded UI host inheritance failed")
assert(gradientInstance.Color == gradientColors and gradientInstance.Transparency == gradientAlpha and imageInstance.SliceCenter == slice, "Typed gradient/image properties failed")
local imageClicks = 0
local imageConnection = imageInstance.MouseButton1Down:Connect(function(x, y) assert(x == 12 and y == 34); imageClicks += 1 end)
UI.dispatch(imageInstance, "MouseEnter", nil, table.pack(4, 9))
UI.dispatch(imageInstance, "MouseButton1Down", nil, table.pack(12, 34))
UI.dispatch(textBoxRef.current, "FocusLost", nil, table.pack(true))
assert(hoverPosition == Vector2.new(4, 9) and downPosition == Vector2.new(12, 34) and imageClicks == 1 and focusLost == true, "Pointer/focus events lost their argument list")
assert(not pcall(function() gradientInstance.Color = {} end) and not pcall(function() imageInstance.ScaleType = Enum.SortOrder.Name end), "Malformed gradient/image properties were accepted")
assert(not pcall(function() imageInstance.ImageRectSize = Vector2.new(-1, 20) end) and not pcall(function() imageInstance.IsLoaded = true end), "Invalid/read-only image properties were accepted")
assert(not pcall(function() extendedRoot.container.children[1].Scale.Scale = -1 end) and not pcall(function() scrollRef.current.Grid.FillDirectionMaxCells = 1.5 end), "Invalid scale/grid properties were accepted")
updateColors(ColorSequence.new(blue))
assert(gradientInstance.Color.Keypoints[1].Value == blue, "Gradient binding update failed")
imageInstance.props.IsLoaded = true
extendedRoot:render(extendedElements("Second"))
UI.pump(0)
assert(imageRef.current == imageInstance and imageInstance.IsLoaded and changedText == "Second", "Host updates lost identity/readonly native state/Text change events")
extendedRoot:unmount()
assert(imageRef.current == nil and gradientRef.current == nil and textBoxRef.current == nil and not imageConnection.Connected, "Expanded UI unmount retained refs/signals")
updateColors(gradientColors)
assert(gradientInstance.Color.Keypoints[1].Value == blue, "Unmount retained a gradient binding")

local uncontrolledRoot = ReactRoblox.createRoot(Instance.new("Folder"))
local uncontrolledInput, uncontrolledScroll = React.createRef(), React.createRef()
local function uncontrolledElements(tag, controlled)
    return e("Frame", {}, {
        Input = e("TextBox", { ref = uncontrolledInput, PlaceholderText = tag, Text = controlled and "controlled" or nil }),
        Scroll = e("ScrollingFrame", { ref = uncontrolledScroll, ScrollBarThickness = tag == "one" and 6 or 7,
            CanvasPosition = controlled and Vector2.zero or nil }),
    })
end
uncontrolledRoot:render(uncontrolledElements("one", false))
uncontrolledScroll.current.ScrollBarImageColor3 = red
uncontrolledScroll.current.ScrollBarImageTransparency = 0.25
assert(uncontrolledScroll.current.ScrollBarImageColor3 == red and uncontrolledScroll.current.ScrollBarImageTransparency == 0.25,
    "Scrollbar styling properties were rejected")
uncontrolledInput.current.props.Text = "typed natively"
uncontrolledScroll.current.props.CanvasPosition = Vector2.new(0, 50)
uncontrolledRoot:render(uncontrolledElements("two", false))
assert(uncontrolledInput.current.Text == "typed natively" and uncontrolledScroll.current.CanvasPosition.Y == 50,
    "Unrelated React commit erased uncontrolled text/scroll state")
uncontrolledRoot:render(uncontrolledElements("one", true))
assert(uncontrolledInput.current.Text == "controlled" and uncontrolledScroll.current.CanvasPosition.Y == 0, "Controlled props did not override native state")
uncontrolledRoot:render(uncontrolledElements("two", false))
assert(uncontrolledInput.current.Text == "" and uncontrolledScroll.current.CanvasPosition == Vector2.zero,
    "Removing formerly controlled props did not restore their defaults")
uncontrolledRoot:unmount()

local folder = Instance.new("Folder")
folder.Name = "Compatibility"
local screen = Instance.new("ScreenGui", folder)
screen.Name = "Screen"
local manual = Instance.new("Frame", screen)
manual.Name = "Manual"
assert(typeof(manual) == "Instance" and manual.ClassName == "Frame" and manual:IsA("GuiObject") and manual:IsA("Instance") and not manual:IsA("TextButton"), "Instance ancestry is wrong")
assert(manual.Parent == screen and folder.Screen == screen and folder:FindFirstChild("Manual", true) == manual, "Parenting or child lookup failed")
assert(folder:FindFirstChildWhichIsA("GuiObject", true) == manual and #folder:GetDescendants() == 2 and manual:GetFullName() == "Compatibility.Screen.Manual", "Instance tree helpers failed")
assert(manual:IsDescendantOf(folder) and folder:IsAncestorOf(manual) and manual:FindFirstAncestor("Screen") == screen, "Ancestor helpers failed")
assert(not pcall(function() folder.Parent = manual end) and folder.Parent == nil, "Parent cycle was accepted")
assert(not pcall(function() manual.AbsoluteSize = Vector2.one end) and not pcall(function() manual.Text = "wrong class" end), "Unsupported or read-only properties were accepted")
assert(not pcall(function() manual.AutomaticSize = Enum.FillDirection.Vertical end), "Wrong enum type was accepted")
assert(not pcall(function() manual.AutomaticSize = "GetEnumItems" end) and Enum.AutomaticSize:FromName("GetEnumItems") == nil, "Enum method names were accepted as items")
local changedProperties, sizeChanges, nativeEvents = {}, 0, 0
local changedConnection = manual.Changed:Connect(function(property) table.insert(changedProperties, property) end)
local sizeConnection = manual:GetPropertyChangedSignal("Size"):Connect(function() sizeChanges += 1 end)
assert(typeof(sizeConnection) == "RBXScriptConnection" and typeof(manual.Changed) == "RBXScriptSignal", "Signals/connections are not typed")
manual.Size = UDim2.fromOffset(240, 120)
assert(sizeChanges == 0, "Property callbacks must be deferred outside drawing/commit")
UI.pump(0)
assert(sizeChanges == 1 and changedProperties[1] == "Size", "Property signals were not pumped")
sizeConnection:Disconnect()
manual.Size = UDim2.fromOffset(260, 120)
UI.pump(0)
assert(sizeChanges == 1 and not sizeConnection.Connected, "Disconnected property signal fired")
local buttonRef = React.createRef()
local compatRoot = ReactRoblox.createRoot(manual)
assert(not pcall(ReactRoblox.createRoot, manual), "Duplicate root ownership was accepted")
compatRoot:render(e("TextButton", {
    ref = buttonRef, Name = "PortableButton", Size = UDim2.fromScale(1, 1),
    Text = "Portable", TextXAlignment = Enum.TextXAlignment.Center,
    [React.Event.Activated] = function(instance) assert(typeof(instance) == "Instance"); nativeEvents += 1 end,
}))
local portableButton = buttonRef.current
assert(portableButton == manual:FindFirstChild("PortableButton") and portableButton.Parent == manual, "React refs are not UI Instances")
assert(portableButton.props.TextColor3 == Color3.new(0, 0, 0) and portableButton.props.BackgroundColor3 == Color3.new(1, 1, 1), "Explicit Instance roots did not apply portable visual defaults")
local imperativeClicks = 0
local clickConnection = portableButton.Activated:Connect(function() imperativeClicks += 1 end)
UI.dispatch(portableButton, "Activated")
assert(imperativeClicks == 1 and nativeEvents == 1, "Imperative and React events did not share a host")
local portalContainer = Instance.new("Folder")
local portalRoot = ReactRoblox.createRoot(portalContainer)
portalRoot:render(ReactRoblox.createPortal(e("TextLabel", { Name = "PortalLabel", Text = "Portal" }), manual))
assert(manual:FindFirstChild("PortalLabel") ~= nil, "ReactRoblox portal did not mount into an Instance")
assert(manual.PortalLabel.props.TextColor3 == Color3.new(0, 0, 0), "Portable portal lost Roblox default text contrast")
local portalIdentity = manual.PortalLabel
portalRoot:render(ReactRoblox.createPortal(e("TextLabel", { Name = "PortalLabel", Text = "Portal V2" }), manual))
assert(manual.PortalLabel == portalIdentity and portalIdentity.Text == "Portal V2", "Portal re-render lost host identity")
UI.refresh({})
assert(manual.PortalLabel == portalIdentity, "Hot refresh traversed/changed a portal target instead of its element")
local beforeReparentRoots = #UI.getRoots()
folder.Parent = game:GetService("Players").LocalPlayer:WaitForChild("PlayerGui")
assert(#UI.getRoots() == beforeReparentRoots and folder.Parent:IsA("PlayerGui"), "Reparenting duplicated visual roots")
local clone = manual:Clone()
assert(clone.Parent == nil and clone ~= manual and clone.Size == manual.Size and #clone:GetChildren() == 2 and clone.children[1] ~= manual.children[1], "Clone did not copy properties/subtree independently")
manual.Archivable = false
assert(manual:Clone() == nil, "Archivable=false clone contract failed")
clone:Destroy()
portalRoot:unmount()
assert(manual:FindFirstChild("PortalLabel") == nil, "Portal unmount retained children")
compatRoot:unmount()
assert(buttonRef.current == nil and portableButton.Parent == nil and not clickConnection.Connected, "Unmount did not clear refs/signals")
UI.dispatch(portableButton, "Activated")
assert(nativeEvents == 1 and imperativeClicks == 1, "Disposed ref retained callbacks")
assert(manual.Parent == screen and not manual.disposed, "Root unmount must retain its caller-owned container")
changedConnection:Disconnect()
folder:Destroy(); portalContainer:Destroy()
assert(#UI.getRoots() == 0, "Manual tree cleanup retained a visual root")
local reentrantParent = Instance.new("Folder")
local reentrantChild = Instance.new("Frame", reentrantParent)
local destroyingCalls = 0
reentrantChild.Destroying:Connect(function()
    destroyingCalls += 1
    reentrantChild:Destroy()
    reentrantParent:Destroy()
end)
reentrantChild:Destroy()
assert(destroyingCalls == 1 and reentrantChild.disposed and reentrantParent.disposed and reentrantChild.Parent == nil, "Reentrant Destroying did not dispose its tree exactly once")
local genericRoot = UI.createRoot()
local genericRef = React.createRef()
local genericChanged, genericAdded, genericRemoved, genericDestroyed, genericAncestry = false, 0, 0, 0, 0
genericRoot:render(e("Frame", {
    ref = genericRef,
    [React.Event.Changed] = function(_, property) if property == "Size" then genericChanged = true end end,
    [React.Event.ChildAdded] = function(_, child) assert(child:IsA("TextLabel")); genericAdded += 1 end,
    [React.Event.ChildRemoved] = function(_, child) assert(child:IsA("TextLabel")); genericRemoved += 1 end,
    [React.Event.Destroying] = function() genericDestroyed += 1 end,
    [React.Event.AncestryChanged] = function(_, child) assert(child == genericRef.current); genericAncestry += 1 end,
}))
genericRef.current.Size = UDim2.fromOffset(90, 60)
local genericChild = Instance.new("TextLabel", genericRef.current)
genericChild:Destroy()
UI.pump(0)
assert(genericChanged and genericAdded == 1 and genericRemoved == 1 and genericAncestry == 1, "React.Event generic Instance events did not dispatch")
genericRoot:unmount()
assert(genericDestroyed == 1, "React.Event.Destroying did not dispatch")
local dim = UI.UDim2.new(UI.UDim.new(0.5, 12), UI.UDim.new(1, -20))
assert(dim.X.Scale == 0.5 and dim.X.Offset == 12 and dim.Y.Offset == -20, "UDim2 dimension overload failed")
assert(UI.UDim2.fromScale(0.5, 1).Y.Scale == 1 and UI.UDim2.fromOffset(20, 30).X.Offset == 20, "UDim2 helpers failed")
assert(UI.Vector2.new(0.5, 1).Y == 1 and UI.Color3.fromRGB(255, 128, 0).G == 128 / 255, "UI vector/color helpers failed")
assert(not pcall(function() dim.X.Offset = 40 end), "Layout values must be immutable")
assert(not pcall(UI.UDim.new, 0 / 0, 0) and not pcall(UI.Color3.new, math.huge, 0, 0), "Invalid layout values must fail at construction")
local geometry, setGeometry = React.createBinding(UI.UDim2.fromScale(0.5, 1))
local styledRoot = UI.createRoot()
styledRoot:render(e("Frame", { Size = geometry, BackgroundColor3 = UI.Color3.fromRGB(12, 34, 56) }, {
    e("UICorner", { CornerRadius = UI.UDim.new(0, 12) }),
    e("UIPadding", { PaddingLeft = UI.UDim.new(0, 8) }),
    e("UIListLayout", { Padding = UI.UDim.new(0, 6), SortOrder = "LayoutOrder" }),
    e("TextLabel", { LayoutOrder = 1, Text = "Styled", TextSize = 24 }),
}))
local styledFrame = styledRoot.container.children[1]
assert(#styledFrame.children == 4 and styledFrame.props.Size.X.Scale == 0.5, "Native modifier hosts failed to mount")
setGeometry(UI.UDim2.fromOffset(320, 240))
assert(styledFrame.props.Size.X.Offset == 320, "Layout binding did not update without a component render")
styledRoot:unmount()
setGeometry(UI.UDim2.fromOffset(640, 480))
assert(styledFrame.props.Size.X.Offset == 320, "Unmount retained a layout binding subscription")
local cleanups, effects, layouts = 0, 0, 0
local context = React.createContext("default")
local captured

local function Counter()
    local count, setCount = React.useState(0)
    local extra, dispatch = React.useReducer(function(state, action) return state + action end, 10)
    local label = React.useContext(context)
    local ref = React.useRef({ renders = 0 })
    ref.current.renders += 1
    local value = React.useMemo(function() return count + extra end, { count, extra })
    local click = React.useCallback(function()
        setCount(function(previous) return previous + 1 end)
        dispatch(2)
    end, {})
    React.useEffect(function()
        effects += 1
        return function() cleanups += 1 end
    end, {})
    React.useLayoutEffect(function() layouts += 1 end, { count })
    captured = ref.current
    return e("TextButton", { Text = label .. ":" .. value, [React.Event.Activated] = click })
end

local root = UI.createRoot({ Name = "Contract Test" })
root:render(e(context.Provider, { value = "value" }, { Counter = e(Counter) }))
UI.pump(0)
assert(#root.container.children == 1, "Root should contain one host button")
local button = root.container.children[1]
assert(button.props.Text == "value:10", "useContext/useMemo initial render failed")
assert(effects == 1 and layouts == 1, "Effects did not run")
UI.dispatch(button, "Activated")
UI.pump(0)
assert(root.container.children[1] == button, "Reconciliation must reuse matching host node")
assert(button.props.Text == "value:13", "useState/useReducer event update failed")
assert(captured.renders == 2 and effects == 1 and layouts == 2, "Hooks lost state or dependency semantics")

local binding, updateBinding = React.createBinding("first")
local bindingRoot = UI.createRoot({ Name = "Binding Test" })
local bindingChanges = {}
bindingRoot:render(e("TextLabel", { Text = binding, [React.Change.Text] = function(_, value) table.insert(bindingChanges, value) end }))
local text = bindingRoot.container.children[1]
assert(text.props.Text == "first", "Initial binding failed")
updateBinding("second")
assert(text.props.Text == "second", "Binding subscription did not update host")
UI.pump(0)
assert(bindingChanges[#bindingChanges] == "second", "Binding updates did not reach React.Change")
bindingRoot:unmount()
updateBinding("third")
assert(text.props.Text == "second", "Unmounted bindings must disconnect")

local keyedRoot = UI.createRoot({ Name = "Key Test" })
local function items(reversed)
    return e("Frame", {}, if reversed then { e("TextLabel", { key = "b", Text = "B" }), e("TextLabel", { key = "a", Text = "A" }) }
        else { e("TextLabel", { key = "a", Text = "A" }), e("TextLabel", { key = "b", Text = "B" }) })
end
keyedRoot:render(items(false))
local frame = keyedRoot.container.children[1]
local first, second = frame.children[1], frame.children[2]
keyedRoot:render(items(true))
assert(frame.children[1] == second and frame.children[2] == first, "Keyed host reorder lost identity")

local checked = false
local changeRoot = UI.createRoot()
changeRoot:render(e("Checkbox", { Checked = checked, LayoutOrder = 1, TextColor = { 1, 1, 1 }, [React.Change.Checked] = function(_, value) checked = value end }))
UI.dispatch(changeRoot.container.children[1], "Change.Checked", true)
assert(checked, "React.Change event mapping failed")
changeRoot:render(e("SliderFloat", { Value = 0.5, Min = 0, Max = 1, LayoutOrder = 2, Size = UDim2.fromOffset(120, 20), TextColor3 = Color3.new(1, 1, 1) }))
assert(changeRoot.container.children[1].LayoutOrder == 2, "Legacy widgets lost common UI layout/style properties")

local fired = 0
local delayed = task.delay(1, function() fired += 1 end)
task.cancel(delayed)
UI.pump(2)
assert(fired == 0, "Cancelled task ran")
task.defer(function() fired += 1; task.defer(function() fired += 1 end) end)
UI.pump(0)
assert(fired == 1, "Deferred recursion must yield until the next frame")
UI.pump(0)
assert(fired == 2, "Deferred callback was lost")

task.defer(function() error("Intentional task failure") end)
task.defer(function() fired += 1 end)
assert(not pcall(UI.pump, 0), "Task failure must reach protected caller")
UI.pump(0)
assert(fired == 3, "A failing task discarded later queued work")
local heartbeat = game:GetService("RunService").Heartbeat
heartbeat:Connect(function() error("Intentional Heartbeat failure") end)
task.defer(function() fired += 1 end)
assert(not pcall(UI.pump, 0), "Heartbeat failure must reach protected caller")
UI.pump(0)
assert(fired == 4, "A failing Heartbeat listener stranded queued work")

local eventRoot = UI.createRoot()
local nextEvent = false
eventRoot:render(e("Frame", {}, {
    e("TextButton", { key = "bad", [React.Event.Activated] = function() error("Intentional event failure") end }),
    e("TextButton", { key = "good", [React.Event.Activated] = function() nextEvent = true end }),
}))
local eventFrame = eventRoot.container.children[1]
table.insert(UI.events, { instance = eventFrame.children[1], name = "Activated" })
table.insert(UI.events, { instance = eventFrame.children[2], name = "Activated" })
assert(not pcall(UI.pump, 0), "Event failure must reach protected caller")
UI.pump(0)
assert(nextEvent, "A failing UI event discarded later queued events")

local staleCallbacks = 0
task.delay(0, function() staleCallbacks += 1 end)
heartbeat:Connect(function() staleCallbacks += 1 end)

UI.reset()
assert(#UI.getRoots() == 0, "Reset did not unmount all roots")
assert(cleanups == 1, "Effect cleanup must run exactly once")
UI.dispatch(button, "Activated")
assert(button.props.Text == "value:13", "Disposed node must ignore pending UI events")
UI.pump(0)
assert(staleCallbacks == 0, "Reset retained callbacks from the previous Play generation")
local afterResetEffects = 0
local newRoot = UI.createRoot()
newRoot:render(e(function()
    React.useEffect(function() afterResetEffects += 1 end, {})
    return e("TextLabel", { Text = "After reset" })
end))
UI.pump(0)
assert(afterResetEffects == 1, "Reset stranded upstream Scheduler work")
UI.reset()
local resetFrame = Instance.new("Frame", game:GetService("Players").LocalPlayer.PlayerGui)
local failedDestroy = resetFrame.Destroying:Connect(function() error("Intentional destroy failure") end)
local resetButton = Instance.new("TextButton", resetFrame)
local leakedConnection = resetButton.Activated:Connect(function() error("Stale manual event") end)
assert(not pcall(UI.reset), "Destroy callback failures must reach the protected reset caller")
assert(#UI.getRoots() == 0 and #UI.events == 0 and not failedDestroy.Connected and not leakedConnection.Connected, "A failing Destroy callback stranded roots/signals")
assert(not game:GetService("Players").LocalPlayer.PlayerGui.disposed, "Reset did not recreate the local UI mount")
do
    local function resetCreationTests()
        for _=1,16 do
            local createdDuringReset,grandchild
            local resetOwner=Instance.new("Frame")
            resetOwner.Destroying:Connect(function()
                createdDuringReset=Instance.new("Frame")
                createdDuringReset.Destroying:Connect(function() grandchild=Instance.new("Frame") end)
            end)
            UI.reset()
            assert(createdDuringReset and createdDuringReset.disposed,"An Instance created by Destroying survived reset")
            assert(grandchild and grandchild.disposed,"Nested cleanup-created Instances survived reset")
        end
        local recursiveOwner=Instance.new("Frame")
        recursiveOwner.Destroying:Connect(function() UI.reset() end)
        assert(not pcall(UI.reset),"A recursive UI reset silently corrupted the reset generation")
        assert(recursiveOwner.disposed and not game:GetService("Players").LocalPlayer.PlayerGui.disposed,"Recursive reset stranded UI cleanup")
        UI.reset()
        local fresh=Instance.new("Frame")
        assert(not fresh.disposed,"Reset guard was not released after recursive cleanup failure")
        UI.reset()
    end
    resetCreationTests()
end
do
    local function resetRootGuards()
        local owner = UI.createRoot()
        local cleanups = 0
        local creationRejected, renderRejected, recursionRejected = false, false, false
        owner:render(e(function()
            React.useEffect(function()
                return function()
                    cleanups += 1
                    local ok, message = pcall(UI.createRoot)
                    creationRejected = not ok and string.find(tostring(message), "during UI reset", 1, true) ~= nil
                    ok, message = pcall(function() owner:render(e("Frame")) end)
                    renderRejected = not ok and string.find(tostring(message), "during UI reset", 1, true) ~= nil
                    ok, message = pcall(UI.reset)
                    recursionRejected = not ok and string.find(tostring(message), "already in progress", 1, true) ~= nil
                end
            end, {})
            return e("TextLabel", { Text = "Reset owner" })
        end))
        UI.pump(0)
        UI.reset()
        assert(cleanups == 1 and creationRejected and renderRejected and recursionRejected,
            "Effect cleanup escaped or reentered the React reset generation")
        assert(not owner.mounted and #UI.getRoots() == 0 and #UI.events == 0,
            "Reset retained a mounted fiber or pending events")
        local manual = Instance.new("Frame")
        local manualRejected = false
        manual.Destroying:Connect(function()
            local ok, message = pcall(UI.createRoot)
            manualRejected = not ok and string.find(tostring(message), "during UI reset", 1, true) ~= nil
        end)
        UI.reset()
        assert(manualRejected and manual.disposed, "Destroying callback created a root during reset")
        local fresh = UI.createRoot()
        fresh:render(e("TextLabel", { Text = "Recovered after reset" }))
        UI.pump(0)
        assert(#UI.getRoots() == 1 and fresh.container.children[1].Text == "Recovered after reset",
            "Reset guard remained active after cleanup")
        UI.reset()
    end
    resetRootGuards()
end
print("ReactRuntimeTests passed: upstream hooks, events, keys, bindings, effects and cleanup")
