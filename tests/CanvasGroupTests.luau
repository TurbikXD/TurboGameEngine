do
    local function runCanvasGroupTests()
        local React = require("React")
        local UI = require("ReactTurbo")
        local ReactRoblox = require("ReactRoblox")
        local TweenService = game:GetService("TweenService")
        local e = React.createElement
        local function near(actual, expected, message)
            assert(math.abs(actual - expected) < 0.00001, (message or "CanvasGroup value differs") .. ": " .. tostring(actual))
        end
        UI.reset()
        local features = UI.getNativeFeatures()
        assert(type(features.canvasGroups) == "boolean" and features.canvasGroups == (features.version >= 3), "CanvasGroup feature guard ignored the running native version")

        local group = Instance.new("CanvasGroup")
        assert(typeof(group) == "Instance" and group.ClassName == "CanvasGroup" and group:IsA("GuiObject") and not group:IsA("Frame"), "CanvasGroup must inherit directly from GuiObject as in Roblox")
        assert(group.GroupTransparency == 0 and group.GroupColor3 == Color3.new(1, 1, 1) and group.ClipsDescendants == true and group.Interactable == true, "CanvasGroup defaults differ")
        assert(not pcall(function() group.ClipsDescendants = false end), "CanvasGroup accepted disabled descendant clipping")
        assert(not pcall(function() group.GroupTransparency = 0 / 0 end) and not pcall(function() group.GroupTransparency = math.huge end), "CanvasGroup accepted non-finite transparency")
        assert(not pcall(function() group.GroupColor3 = { R = 1, G = 1, B = 1 } end) and not pcall(function() group.Interactable = 1 end), "CanvasGroup accepted untyped color/input values")
        local ordinaryFrame = Instance.new("Frame")
        assert(not pcall(function() ordinaryFrame.GroupTransparency = 0.5 end), "CanvasGroup properties leaked into ordinary Frame")
        group.GroupTransparency = -2; assert(group.GroupTransparency == 0, "Negative group transparency was not clamped")
        group.GroupTransparency = 2; assert(group.GroupTransparency == 1, "Group transparency above one was not clamped")
        group.GroupTransparency = 0
        local changed, colorChanges = {}, 0
        local transparencyConnection = group:GetPropertyChangedSignal("GroupTransparency"):Connect(function() table.insert(changed, group.GroupTransparency) end)
        local colorConnection = group:GetPropertyChangedSignal("GroupColor3"):Connect(function() colorChanges += 1 end)
        group.GroupTransparency = 0.25; group.GroupColor3 = Color3.fromRGB(80, 150, 220)
        assert(#changed == 0 and colorChanges == 0, "Group callbacks escaped the deferred protected pump")
        UI.pump(0)
        assert(#changed == 1 and changed[1] == 0.25 and colorChanges == 1, "CanvasGroup property signals failed")
        local child = Instance.new("TextButton", group)
        child.Name = "ClonedButton"; child.BackgroundTransparency = 0.3
        local clone = group:Clone()
        assert(clone.ClassName == "CanvasGroup" and clone.GroupTransparency == 0.25 and clone.GroupColor3 == group.GroupColor3 and clone.ClipsDescendants, "CanvasGroup clone lost its group properties")
        assert(clone.ClonedButton ~= child and clone.ClonedButton.Parent == clone and clone.ClonedButton.BackgroundTransparency == 0.3, "CanvasGroup clone did not copy its subtree independently")
        clone:Destroy(); group:Destroy(); ordinaryFrame:Destroy()
        assert(not transparencyConnection.Connected and not colorConnection.Connected, "Group destruction retained property subscriptions")

        local binding, updateTransparency = React.createBinding(0.2)
        local colorBinding, updateColor = React.createBinding(Color3.new(1, 1, 1))
        local bindingContainer = Instance.new("Folder")
        local bindingRoot = ReactRoblox.createRoot(bindingContainer)
        local boundRef = React.createRef()
        local bindingChanges = 0
        bindingRoot:render(e("CanvasGroup", {
            ref = boundRef, GroupTransparency = binding, GroupColor3 = colorBinding,
            [React.Change.GroupTransparency] = function(_, value) assert(value == 0.2 or value == 0.6); bindingChanges += 1 end,
        }))
        local bound = boundRef.current
        updateTransparency(0.6); updateColor(Color3.fromRGB(100, 130, 160))
        UI.pump(0)
        assert(bound.GroupTransparency == 0.6 and bound.GroupColor3 == Color3.fromRGB(100, 130, 160) and bound.props.ClipsDescendants == true and bindingChanges > 0, "CanvasGroup React bindings/events/default clipping failed")
        assert(not pcall(TweenService.Create, TweenService, bound, TweenInfo.new(1), { GroupTransparency = 1 }), "TweenService stole a bound group property")
        bindingRoot:unmount(); bindingContainer:Destroy()
        updateTransparency(0.8)
        assert(boundRef.current == nil and bound.GroupTransparency == 0.6, "Group unmount retained a binding/ref")

        local outerRef, innerRef, buttonRef = React.createRef(), React.createRef(), React.createRef()
        local targetContainer = Instance.new("Folder")
        local root = ReactRoblox.createRoot(targetContainer)
        local clicks, leaves, focusLost = 0, 0, 0
        local function tree(caption)
            return e("CanvasGroup", { ref = outerRef, GroupTransparency = 0, Size = UDim2.fromOffset(320, 200) }, {
                Inner = e("CanvasGroup", { ref = innerRef, GroupColor3 = Color3.new(1, 1, 1) }, {
                    Action = e("TextButton", { ref = buttonRef, Text = caption, BackgroundTransparency = 0.3,
                        [React.Event.Activated] = function() clicks += 1 end,
                        [React.Event.MouseLeave] = function() leaves += 1 end,
                    }),
                    Input = e("TextBox", { [React.Event.FocusLost] = function() focusLost += 1 end }),
                }),
            })
        end
        root:render(tree("First"))
        local outer, inner, button = outerRef.current, innerRef.current, buttonRef.current
        assert(inner.Parent == outer and button.Parent == inner and inner.ClipsDescendants, "Nested CanvasGroup parenting/defaults failed")
        local propsReads = 0
        local propsMetatable = { __index = function() propsReads += 1; error("Input gate executed a props metamethod") end }
        setmetatable(button.props, propsMetatable); setmetatable(inner.props, propsMetatable); setmetatable(outer.props, propsMetatable)
        table.insert(UI.events, { instance = button, name = "Activated" })
        rawset(inner, "_canvasReady", false)
        UI.pump(0)
        assert(clicks == 0, "Queued activation escaped an ancestor whose GPU canvas was unavailable")
        rawset(inner, "_canvasReady", true)
        table.insert(UI.events, { instance = button, name = "Activated" }); UI.pump(0)
        assert(clicks == 1, "Visible CanvasGroup blocked its queued button activation")
        assert(propsReads == 0, "Input gate invoked a props __index metamethod")
        setmetatable(button.props, nil); setmetatable(inner.props, nil); setmetatable(outer.props, nil)
        local buttonProps = button.props
        rawset(button, "props", false); UI.dispatch(button, "Activated"); rawset(button, "props", buttonProps)
        rawset(button, "parent", button); UI.dispatch(button, "Activated"); rawset(button, "parent", inner)
        assert(clicks == 1, "Malformed props or cyclic ancestry escaped fail-closed input gating")
        table.insert(UI.events, { instance = button, name = "Activated" })
        outer.GroupTransparency = 1
        UI.pump(0)
        assert(clicks == 1, "Queued activation escaped a fully transparent ancestor group")
        UI.dispatch(button, "MouseLeave"); UI.dispatch(inner.Input, "FocusLost", false)
        assert(leaves == 1 and focusLost == 1, "Hidden group blocked input cleanup notifications")
        outer.GroupTransparency = 0.5
        assert(button.BackgroundTransparency == 0.3 and inner.GroupTransparency == 0, "Facade faked group opacity by rewriting descendant alpha")
        UI.dispatch(button, "Activated"); assert(clicks == 2, "Partially visible group was treated as fully hidden")
        outer.Interactable = false
        UI.dispatch(button, "Activated"); assert(clicks == 2, "Ancestor Interactable=false did not gate the group subtree")
        outer.Interactable = true; inner.GroupTransparency = 1
        UI.dispatch(button, "Activated"); assert(clicks == 2, "Nested fully transparent group failed input gating")
        inner.GroupTransparency = 0
        UI.dispatch(button, "Activated"); assert(clicks == 3, "Input did not recover after nested group reopened")

        outer.GroupTransparency = 0
        local tween = TweenService:Create(outer, TweenInfo.new(1, Enum.EasingStyle.Linear, Enum.EasingDirection.Out), { GroupTransparency = 1, GroupColor3 = Color3.new(1, 0, 0) })
        local completed = 0
        tween.Completed:Connect(function(state) assert(state == Enum.PlaybackState.Completed); completed += 1 end)
        tween:Play(); UI.pump(0.25)
        near(outer.GroupTransparency, 0.25); near(outer.GroupColor3.G, 0.75)
        root:render(tree("Unrelated update"))
        assert(outerRef.current == outer and buttonRef.current == button and outer.GroupTransparency == 0.25, "Unchanged React group props rewound the fade tween")
        UI.pump(0.75)
        assert(outer.GroupTransparency == 1 and outer.GroupColor3 == Color3.new(1, 0, 0) and completed == 1, "Group transparency/color TweenService interpolation failed")
        UI.dispatch(button, "Activated"); assert(clicks == 3, "Completed fade retained clickable descendants")

        outer.GroupTransparency = 0
        local loop = TweenService:Create(inner, TweenInfo.new(1, Enum.EasingStyle.Linear, Enum.EasingDirection.Out, -1, true), { GroupTransparency = 0.8 })
        local staleCallbacks = 0
        local staleConnection = loop.Completed:Connect(function() staleCallbacks += 1 end)
        loop:Play(); UI.pump(0.2)
        root:unmount(); targetContainer:Destroy()
        UI.pump(1)
        assert(outerRef.current == nil and innerRef.current == nil and buttonRef.current == nil and not staleConnection.Connected and staleCallbacks == 0, "CanvasGroup unmount leaked refs/tweens/completion callbacks")
        assert(#UI.getRoots() == 0, "CanvasGroup root cleanup retained a mounted native tree")
        UI.reset()
        print("CanvasGroupTests passed: schema, bindings, nested input, tweens and cleanup; native composition tested separately")
    end
    runCanvasGroupTests()
end
