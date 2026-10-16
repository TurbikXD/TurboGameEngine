-- Black-box compatibility checks; all time comes from the owner-thread UI pump.
-- Roblox's contract is documented in TweenService, TweenBase and TweenInfo.
-- No native drawing, wall-clock sleeps or private tween state is used here.
do
    local function runTweenServiceTests()
        local React = require("React")
        local UI = require("ReactTurbo")
        local ReactRoblox = require("ReactRoblox")
        local TweenService = game:GetService("TweenService")
        local linear = Enum.EasingStyle.Linear
        local out = Enum.EasingDirection.Out
        local states = Enum.PlaybackState

        local function near(actual, expected, message)
            assert(type(actual) == "number" and actual == actual and math.abs(actual - expected) < 0.00001,
                (message or "Tween value differs") .. ": got " .. tostring(actual) .. ", expected " .. tostring(expected))
        end

        -- Completed/property signals are deliberately deferred to a protected
        -- pump; a zero-time drain must not consume animation time.
        local function drain()
            UI.pump(0)
            UI.pump(0)
        end

        local function info(time, repeats, reverses, delay)
            return TweenInfo.new(time, linear, out, repeats or 0, reverses or false, delay or 0)
        end

        local function frame()
            local result = Instance.new("Frame")
            result.BackgroundTransparency = 0
            result.Position = UDim2.fromOffset(0, 0)
            result.Size = UDim2.fromOffset(0, 0)
            return result
        end

        do
            local defaults = TweenInfo.new()
            assert(typeof(defaults) == "TweenInfo" and defaults.Time == 1 and defaults.DelayTime == 0,
                "TweenInfo defaults/type differ from Roblox")
            assert(defaults.EasingStyle == Enum.EasingStyle.Quad and defaults.EasingDirection == out
                and defaults.RepeatCount == 0 and not defaults.Reverses, "TweenInfo easing/repetition defaults differ")
            assert(not pcall(function() defaults.Time = 5 end), "TweenInfo was mutable")
            assert(not pcall(TweenInfo.new, -1) and not pcall(TweenInfo.new, 0 / 0) and not pcall(TweenInfo.new, math.huge),
                "Invalid tween duration was accepted")
            assert(not pcall(TweenInfo.new, 1, Enum.SortOrder.Name) and not pcall(TweenInfo.new, 1, linear, Enum.FillDirection.Vertical),
                "Unrelated easing enum types were accepted")
            assert(not pcall(TweenInfo.new, 1, linear, out, 0.5) and not pcall(TweenInfo.new, 1, linear, out, 0, false, -1),
                "Fractional repetition or negative delay was accepted")
            assert(Enum.EasingStyle.Linear.Value == 0 and Enum.EasingStyle.Quad.Value == 3 and Enum.EasingStyle.Cubic.Value == 10,
                "EasingStyle numeric identities differ from Roblox")
            assert(Enum.EasingDirection.In.Value == 0 and out.Value == 1 and Enum.EasingDirection.InOut.Value == 2,
                "EasingDirection numeric identities differ from Roblox")
            assert(states.Begin.Value == 0 and states.Delayed.Value == 1 and states.Playing.Value == 2
                and states.Paused.Value == 3 and states.Completed.Value == 4 and states.Cancelled.Value == 5,
                "PlaybackState numeric identities differ from Roblox")
            assert(TweenService == game:GetService("TweenService"), "TweenService is not stable within a Play generation")
        end

        do
            near(TweenService:GetValue(-100, linear, out), 0, "GetValue must clamp its input")
            near(TweenService:GetValue(100, linear, out), 1, "GetValue must clamp its input")
            near(TweenService:GetValue(0.25, linear, out), 0.25)
            near(TweenService:GetValue(0.25, Enum.EasingStyle.Quad, Enum.EasingDirection.In), 0.0625)
            near(TweenService:GetValue(0.25, Enum.EasingStyle.Quad, out), 0.4375)
            near(TweenService:GetValue(0.25, Enum.EasingStyle.Quad, Enum.EasingDirection.InOut), 0.125)
            near(TweenService:GetValue(0.25, Enum.EasingStyle.Cubic, Enum.EasingDirection.In), 0.015625)
            near(TweenService:GetValue(0.5, Enum.EasingStyle.Quart, Enum.EasingDirection.In), 0.0625)
            near(TweenService:GetValue(0.5, Enum.EasingStyle.Quint, Enum.EasingDirection.In), 0.03125)
            near(TweenService:GetValue(0.5, Enum.EasingStyle.Sine, Enum.EasingDirection.In), 1 - math.sqrt(0.5))
            near(TweenService:GetValue(0.5, Enum.EasingStyle.Circular, Enum.EasingDirection.In), 1 - math.sqrt(0.75))
            near(TweenService:GetValue(0.5, Enum.EasingStyle.Exponential, Enum.EasingDirection.In), 0.03125)
            near(TweenService:GetValue(0.5, Enum.EasingStyle.Bounce, out), 0.765625)
            assert(TweenService:GetValue(0.5, Enum.EasingStyle.Back, out) > 1, "Back easing lost its overshoot")
            assert(TweenService:GetValue(0.5, Enum.EasingStyle.Elastic, out) > 1, "Elastic easing lost its overshoot")
            for _, style in ipairs(Enum.EasingStyle:GetEnumItems()) do
                for _, direction in ipairs(Enum.EasingDirection:GetEnumItems()) do
                    near(TweenService:GetValue(0, style, direction), 0, "Easing must start exactly at zero")
                    near(TweenService:GetValue(1, style, direction), 1, "Easing must finish exactly at one")
                end
                for _, alpha in ipairs({ 0.125, 0.375, 0.625, 0.875 }) do
                    near(TweenService:GetValue(alpha, style, out),
                        1 - TweenService:GetValue(1 - alpha, style, Enum.EasingDirection.In), "In/Out easing symmetry failed")
                    -- Back/Elastic use dedicated InOut overshoot/period
                    -- constants, so their half curve need not equal In(2t)/2.
                    near(TweenService:GetValue(alpha, style, Enum.EasingDirection.InOut),
                        1 - TweenService:GetValue(1 - alpha, style, Enum.EasingDirection.InOut), "InOut easing symmetry failed")
                end
            end
            assert(not pcall(TweenService.GetValue, TweenService, 0 / 0, linear, out)
                and not pcall(TweenService.GetValue, TweenService, 0.5, Enum.SortOrder.Name, out),
                "GetValue accepted invalid values/enums")
        end

        do
            local target = frame()
            target.BackgroundColor3 = Color3.new(0, 0.25, 0.5)
            local corner = Instance.new("UICorner", target)
            corner.CornerRadius = UDim.new(0, 0)
            local gradient = Instance.new("UIGradient", target)
            gradient.Offset = Vector2.new(-1, 0)
            local specification = info(2)
            local goals = {
                BackgroundTransparency = 1, BackgroundColor3 = Color3.new(1, 0.75, 0.25),
                Size = UDim2.new(1, 100, 0.5, 40), Position = UDim2.new(0.5, 20, 1, 80), AnchorPoint = Vector2.one,
            }
            local tween = TweenService:Create(target, specification, goals)
            assert(typeof(tween) == "Instance" and tween.ClassName == "Tween" and tween:IsA("Tween")
                and tween:IsA("TweenBase") and tween:IsA("Instance"), "Tween is not a portable typed object")
            assert(tween.Instance == target and tween.TweenInfo == specification and tween.PlaybackState == states.Begin,
                "Tween immutable configuration/initial state differs")
            assert(typeof(tween.Completed) == "RBXScriptSignal", "Tween Completed is not a typed signal")
            assert(not pcall(function() tween.PlaybackState = states.Playing end)
                and not pcall(function() tween.Instance = corner end)
                and not pcall(function() tween.TweenInfo = info(5) end), "Read-only Tween properties were mutable")
            near(target.BackgroundTransparency, 0, "Create wrote to its target")
            goals.BackgroundTransparency = 0.75 -- Create must snapshot the goal dictionary.
            target.BackgroundTransparency = 0.2 -- Start values are captured by Play, not Create.
            local completions, lastState, changes = 0, nil, 0
            local connection = tween.Completed:Connect(function(state) completions += 1; lastState = state end)
            local propertyConnection = target:GetPropertyChangedSignal("BackgroundTransparency"):Connect(function() changes += 1 end)
            local cornerTween = TweenService:Create(corner, specification, { CornerRadius = UDim.new(0.5, 20) })
            local gradientTween = TweenService:Create(gradient, specification, { Rotation = 180, Offset = Vector2.new(1, 0.5) })
            tween:Play()
            cornerTween:Play()
            gradientTween:Play()
            assert(tween.PlaybackState == states.Playing and completions == 0, "Play did not begin without calling user code")
            UI.pump(1)
            near(target.BackgroundTransparency, 0.6, "Play did not capture current start/copied goals")
            near(target.Size.X.Scale, 0.5); near(target.Size.X.Offset, 50)
            near(target.Size.Y.Scale, 0.25); near(target.Size.Y.Offset, 20)
            near(target.Position.X.Scale, 0.25); near(target.Position.Y.Offset, 40)
            near(target.AnchorPoint.X, 0.5); near(target.AnchorPoint.Y, 0.5)
            near(target.BackgroundColor3.R, 0.5); near(target.BackgroundColor3.G, 0.5); near(target.BackgroundColor3.B, 0.375)
            near(corner.CornerRadius.Scale, 0.25); near(corner.CornerRadius.Offset, 10)
            near(gradient.Rotation, 90); near(gradient.Offset.X, 0); near(gradient.Offset.Y, 0.25)
            tween:Play() -- An already active tween must not restart its timer.
            UI.pump(1)
            drain()
            near(target.BackgroundTransparency, 1)
            assert(target.Size == UDim2.new(1, 100, 0.5, 40) and target.AnchorPoint == Vector2.one,
                "Typed endpoints were not assigned exactly")
            assert(tween.PlaybackState == states.Completed and completions == 1 and lastState == states.Completed and changes > 0,
                "Tween completion/property signals failed")
            UI.pump(3)
            assert(completions == 1, "Completed was emitted more than once")
            tween:Destroy(); cornerTween:Destroy(); gradientTween:Destroy()
            assert(not connection.Connected, "Destroy retained Completed listeners")
            propertyConnection:Disconnect()
            target:Destroy()
        end

        do
            local target = frame()
            local tween = TweenService:Create(target, info(1), { BackgroundTransparency = 1 })
            local completions = {}
            tween.Completed:Connect(function(state) table.insert(completions, state) end)
            tween:Play()
            UI.pump(0.25)
            near(target.BackgroundTransparency, 0.25)
            tween:Pause()
            assert(tween.PlaybackState == states.Paused, "Pause did not preserve a playing tween")
            UI.pump(4)
            near(target.BackgroundTransparency, 0.25, "Paused tween kept advancing")
            assert(#completions == 0, "Pause fired Completed")
            tween:Play()
            UI.pump(0.25)
            near(target.BackgroundTransparency, 0.5, "Play after Pause did not resume elapsed progress")
            tween:Cancel()
            near(target.BackgroundTransparency, 0.5, "Cancel rewound the target property")
            assert(tween.PlaybackState == states.Cancelled and #completions == 0, "Cancel invoked user listeners inline")
            drain()
            assert(#completions == 1 and completions[1] == states.Cancelled, "Cancel did not report Cancelled completion")
            tween:Play()
            UI.pump(0.5)
            near(target.BackgroundTransparency, 0.75, "Cancelled replay did not restart with its full duration/current start")
            UI.pump(0.5)
            drain()
            assert(#completions == 2 and completions[2] == states.Completed, "Cancelled tween could not be replayed")
            target.BackgroundTransparency = 0
            tween:Play()
            UI.pump(0.5)
            near(target.BackgroundTransparency, 0.5, "Completed replay did not capture current start")
            tween:Destroy(); target:Destroy()
        end

        do
            local target = frame()
            local tween = TweenService:Create(target, info(1, 0, false, 0.5), { BackgroundTransparency = 1 })
            tween:Play()
            assert(tween.PlaybackState == states.Delayed, "Initial delay has the wrong playback state")
            tween:Pause()
            assert(tween.PlaybackState == states.Delayed, "Pause must not pause a delayed Roblox tween")
            UI.pump(0.25)
            near(target.BackgroundTransparency, 0)
            tween:Play() -- Repeated Play while delayed is also a no-op.
            UI.pump(0.5)
            near(target.BackgroundTransparency, 0.25, "Delay consumed animation time or repeated Play restarted the delay")
            assert(tween.PlaybackState == states.Playing, "Delay did not transition to Playing")
            UI.pump(0.75)
            drain()
            assert(tween.PlaybackState == states.Completed, "Delayed tween did not finish at delay + duration")
            tween:Destroy(); target:Destroy()
        end

        do
            local target = frame()
            local completed = 0
            local tween = TweenService:Create(target, info(1, 1, true), { BackgroundTransparency = 1 })
            tween.Completed:Connect(function() completed += 1 end)
            tween:Play()
            UI.pump(0.5); near(target.BackgroundTransparency, 0.5)
            UI.pump(1); near(target.BackgroundTransparency, 0.5, "Reverse leg did not return towards its start")
            UI.pump(0.75); near(target.BackgroundTransparency, 0.25, "Repeat count did not include a complete reverse cycle")
            UI.pump(1); near(target.BackgroundTransparency, 0.75)
            assert(tween.PlaybackState == states.Playing and completed == 0, "Repeat completed before its last reverse leg")
            UI.pump(0.75)
            drain()
            near(target.BackgroundTransparency, 0)
            assert(tween.PlaybackState == states.Completed and completed == 1, "Reversing tween must finish at its original value once")
            tween:Destroy(); target:Destroy()

            target = frame()
            tween = TweenService:Create(target, info(0.5, 1, false, 0.25), { BackgroundTransparency = 1 })
            tween:Play()
            UI.pump(0.5); near(target.BackgroundTransparency, 0.5)
            UI.pump(0.375) -- First cycle ended at .75; repeat is waiting until 1.0.
            assert(tween.PlaybackState == states.Delayed, "Repeat cycles did not apply DelayTime")
            UI.pump(0.375); near(target.BackgroundTransparency, 0.5)
            UI.pump(0.25)
            drain()
            near(target.BackgroundTransparency, 1)
            assert(tween.PlaybackState == states.Completed, "Finite repeated tween did not complete")
            tween:Destroy(); target:Destroy()
        end

        do
            local target = frame()
            local old = TweenService:Create(target, info(1), { Size = UDim2.fromOffset(100, 100), BackgroundTransparency = 1 })
            local cancellations = 0
            old.Completed:Connect(function(state) assert(state == states.Cancelled); cancellations += 1 end)
            old:Play()
            UI.pump(0.25)
            local replacement = TweenService:Create(target, info(1), { BackgroundTransparency = 0.5 })
            assert(old.PlaybackState == states.Playing, "Create cancelled an existing tween before Play")
            replacement:Play()
            assert(old.PlaybackState == states.Cancelled, "Conflicting Play did not cancel the entire older tween")
            local disjoint = TweenService:Create(target, info(1), { Position = UDim2.fromOffset(100, 20) })
            disjoint:Play()
            UI.pump(0.5)
            drain()
            near(target.Size.X.Offset, 25, "A cancelled tween kept writing its other properties")
            near(target.BackgroundTransparency, 0.375)
            near(target.Position.X.Offset, 50)
            assert(cancellations == 1 and replacement.PlaybackState == states.Playing and disjoint.PlaybackState == states.Playing,
                "Disjoint tweens cancelled one another or a conflict emitted duplicate completion")
            old:Destroy(); replacement:Destroy(); disjoint:Destroy(); target:Destroy()
        end

        do
            local target = frame()
            local active = TweenService:Create(target, info(1), { BackgroundTransparency = 1 })
            active:Play()
            UI.pump(0.25)
            local invalidGoals = {
                { Size = Vector2.one }, { AbsoluteSize = Vector2.one }, { Name = "not tweenable" }, { Visible = false },
                { Size = UDim2.fromOffset(100, 100), Text = "wrong class" }, { BackgroundTransparency = math.huge },
                { [1] = 20 }, { Position = UDim2.fromOffset(10, 20), BackgroundTransparency = "bad" },
            }
            for _, goals in ipairs(invalidGoals) do
                assert(not pcall(TweenService.Create, TweenService, target, info(1), goals), "Invalid/read-only goals were accepted")
                near(target.Size.X.Offset, 0, "Rejected Create partially wrote valid properties")
                near(target.Position.X.Offset, 0, "Rejected Create partially wrote valid properties")
                near(target.BackgroundTransparency, 0.25, "Rejected Create wrote to or cancelled an existing animation")
                assert(active.PlaybackState == states.Playing, "Invalid Create cancelled the active tween")
            end
            assert(not pcall(TweenService.Create, TweenService, {}, info(1), { Size = UDim2.fromOffset(1, 1) })
                and not pcall(TweenService.Create, TweenService, target, {}, { BackgroundTransparency = 1 }),
                "Untyped target/TweenInfo was accepted")
            local gradient = Instance.new("UIGradient", target)
            assert(not pcall(TweenService.Create, TweenService, gradient, info(1), { Color = ColorSequence.new(Color3.new(1, 0, 0)) })
                and not pcall(TweenService.Create, TweenService, gradient, info(1), { Transparency = NumberSequence.new(1) }),
                "Unsupported gradient sequence interpolation silently pretended to work")
            UI.pump(0.25)
            near(target.BackgroundTransparency, 0.5, "Invalid goals damaged subsequent pump state")
            active:Destroy(); target:Destroy()
        end

        do
            local target = frame()
            local tween = TweenService:Create(target, info(0), { BackgroundTransparency = 1 })
            local completed = 0
            tween.Completed:Once(function(state) assert(state == states.Completed); completed += 1 end)
            tween:Play()
            UI.pump(0)
            drain()
            near(target.BackgroundTransparency, 1, "Zero duration did not reach the exact endpoint")
            assert(completed == 1 and tween.PlaybackState == states.Completed, "Zero-duration tween failed to complete once")
            tween:Play()
            UI.pump(0)
            drain()
            assert(completed == 1, "Completed:Once retained its connection")
            tween:Destroy()
            assert(not pcall(TweenService.Create, TweenService, target, info(0, -1), { BackgroundTransparency = 0 }),
                "Infinite zero-duration/zero-delay tween must fail explicitly instead of spinning")
            target.BackgroundTransparency = 0
            tween = TweenService:Create(target, info(0.25, -1, true), { BackgroundTransparency = 1 })
            tween:Play()
            UI.pump(100000000.125)
            near(target.BackgroundTransparency, 0.5, "Infinite tween could not skip a large number of cycles in bounded work")
            assert(tween.PlaybackState == states.Playing, "Infinite tween completed")
            tween:Destroy(); target:Destroy()
        end

        do
            local target = frame()
            local first = TweenService:Create(target, info(0.5), { BackgroundTransparency = 0.5 })
            local second, firstCompletions, secondCompletions = nil, 0, 0
            first.Completed:Connect(function(state)
                assert(state == states.Completed)
                firstCompletions += 1
                first:Destroy()
                second = TweenService:Create(target, info(1), { BackgroundTransparency = 1 })
                second.Completed:Connect(function(nextState) assert(nextState == states.Completed); secondCompletions += 1 end)
                second:Play()
            end)
            UI.pump(0) -- No completion before Play.
            first:Play()
            UI.pump(0.5)
            drain()
            assert(firstCompletions == 1 and second and second.PlaybackState == states.Playing, "Completion could not safely destroy/start tweens")
            near(target.BackgroundTransparency, 0.5, "A tween created by Completed inherited the previous frame's delta")
            UI.pump(0.5)
            near(target.BackgroundTransparency, 0.75)
            UI.pump(0.5)
            drain()
            assert(secondCompletions == 1, "Chained tween did not complete")
            second:Destroy(); target:Destroy()
        end

        do
            local target = frame()
            local completed = 0
            local tween = TweenService:Create(target, info(1, -1, true), { BackgroundTransparency = 1 })
            local connection = tween.Completed:Connect(function() completed += 1 end)
            tween:Play()
            UI.pump(0.25)
            target:Destroy()
            drain()
            UI.pump(10)
            assert(tween.PlaybackState == states.Cancelled and completed == 0 and not connection.Connected,
                "Target destruction retained a running animation or a stale callback")
            assert(not pcall(TweenService.Create, TweenService, target, info(1), { BackgroundTransparency = 1 }),
                "Destroyed UI target accepted a new tween")
            tween:Destroy()

            target = frame()
            tween = TweenService:Create(target, info(1), { BackgroundTransparency = 1 })
            connection = tween.Completed:Connect(function() completed += 1 end)
            tween:Play()
            tween:Cancel() -- Queued completion has not entered a protected pump.
            tween:Destroy()
            drain()
            assert(completed == 0 and not connection.Connected, "Destroy retained a pending completion callback")
            target:Destroy()
        end

        do
            local firstTarget, secondTarget = frame(), frame()
            local first = TweenService:Create(firstTarget, info(0.5), { BackgroundTransparency = 1 })
            local second = TweenService:Create(secondTarget, info(0.5), { BackgroundTransparency = 1 })
            local successfulCallbacks, observedFailure = 0, false
            first.Completed:Connect(function() error("Intentional Tween Completed failure") end)
            second.Completed:Connect(function() successfulCallbacks += 1 end)
            first:Play(); second:Play()
            for _, dt in ipairs({ 0.5, 0, 0, 0 }) do
                local ok = pcall(UI.pump, dt)
                if not ok then observedFailure = true end
            end
            assert(observedFailure and successfulCallbacks == 1,
                "Completed failure escaped its protected pump or discarded another tween's queued completion")
            near(firstTarget.BackgroundTransparency, 1)
            near(secondTarget.BackgroundTransparency, 1)
            first:Destroy(); second:Destroy(); firstTarget:Destroy(); secondTarget:Destroy()
        end

        do
            local reference = React.createRef()
            local root = ReactRoblox.createRoot(Instance.new("Folder"))
            local function render(color, position)
                root:render(React.createElement("Frame", {
                    ref = reference, BackgroundColor3 = color, Position = position, Size = UDim2.fromOffset(10, 20),
                }))
            end
            render(Color3.new(0, 0, 0), UDim2.fromOffset(0, 0))
            local target = reference.current
            local tween = TweenService:Create(target, info(1), { Position = UDim2.fromOffset(100, 0) })
            tween:Play()
            UI.pump(0.5)
            near(target.Position.X.Offset, 50)
            -- Value-equal new datatype objects are unchanged authored values.
            render(Color3.new(1, 0, 0), UDim2.fromOffset(0, 0))
            near(target.Position.X.Offset, 50, "Unrelated React commit rewound an imperative tween")
            UI.pump(0.25)
            near(target.Position.X.Offset, 75)
            render(Color3.new(1, 0, 0), UDim2.fromOffset(200, 0))
            near(target.Position.X.Offset, 200, "An intentionally changed authored prop was ignored")
            tween:Destroy()
            root:unmount()
        end

        do
            -- Bindings are continuous React-owned property writers. Rejecting
            -- tween ownership is explicit; the two systems must not fight each frame.
            local binding, updateBinding = React.createBinding(0.2)
            local positionBinding, updatePosition = React.createBinding(UDim2.fromOffset(10, 0))
            local reference = React.createRef()
            local root = ReactRoblox.createRoot(Instance.new("Folder"))
            local function render(position)
                root:render(React.createElement("Frame", { ref = reference, BackgroundTransparency = binding, Position = position }))
            end
            render(UDim2.fromOffset(0, 0))
            local target = reference.current
            assert(not pcall(TweenService.Create, TweenService, target, info(1), { BackgroundTransparency = 0.8 }),
                "Create silently competed with a React binding")
            updateBinding(0.1)
            near(target.BackgroundTransparency, 0.1, "Rejected tween damaged its React binding")
            local beforeBinding = TweenService:Create(target, info(1), { Position = UDim2.fromOffset(100, 0) })
            render(positionBinding)
            assert(not pcall(beforeBinding.Play, beforeBinding), "Play did not revalidate a binding added after Create")
            beforeBinding:Destroy()
            render(UDim2.fromOffset(0, 0))
            local tween = TweenService:Create(target, info(1), { Position = UDim2.fromOffset(100, 0) })
            tween:Play()
            UI.pump(0.25)
            near(target.Position.X.Offset, 25)
            render(positionBinding)
            assert(tween.PlaybackState == states.Cancelled, "Attaching a binding did not cancel an active conflicting tween")
            near(target.Position.X.Offset, 10)
            updatePosition(UDim2.fromOffset(20, 0))
            UI.pump(0.25)
            near(target.Position.X.Offset, 20, "Cancelled tween kept overwriting its new binding")
            drain()
            tween:Destroy()
            tween = TweenService:Create(target, info(1, -1, true), { Size = UDim2.fromOffset(100, 100) })
            tween:Play()
            local staleCallbacks = 0
            local connection = tween.Completed:Connect(function() staleCallbacks += 1 end)
            root:unmount()
            UI.pump(10)
            drain()
            assert(reference.current == nil and staleCallbacks == 0 and not connection.Connected,
                "React unmount retained a tween target/ref/Completed listener")
            tween:Destroy()
        end

        do
            local target = frame()
            local staleCallbacks = 0
            local infinite = TweenService:Create(target, info(1, -1, true), { BackgroundTransparency = 1 })
            local infiniteConnection = infinite.Completed:Connect(function() staleCallbacks += 1 end)
            infinite:Play()
            local queuedTarget = frame()
            local queued = TweenService:Create(queuedTarget, info(1), { BackgroundTransparency = 1 })
            local queuedConnection = queued.Completed:Connect(function() staleCallbacks += 1 end)
            queued:Play()
            queued:Cancel()
            UI.reset()
            UI.pump(100)
            drain()
            assert(#UI.getRoots() == 0 and staleCallbacks == 0 and not infiniteConnection.Connected and not queuedConnection.Connected,
                "Stop/reset retained infinite tweens, pending completions or listeners")
            local fresh = frame()
            local nextTween = game:GetService("TweenService"):Create(fresh, info(1), { BackgroundTransparency = 1 })
            nextTween:Play()
            UI.pump(0.5)
            near(fresh.BackgroundTransparency, 0.5, "New Play generation inherited old tween progress/owners")
            UI.reset()
        end

        do
            -- Destroying handlers run inside reset and can create detached UI.
            -- Registry iteration order must not let that generation's animation
            -- survive Stop. Several fixed cases cover different table slots;
            -- animation time and the expected result are deterministic.
            local callbacks = 0
            for _ = 1, 16 do
                local createdTarget, createdTween, connection
                local owner = frame()
                owner.Destroying:Connect(function()
                    createdTarget = frame()
                    createdTween = TweenService:Create(createdTarget, info(1, -1, true), { BackgroundTransparency = 1 })
                    connection = createdTween.Completed:Connect(function() callbacks += 1 end)
                    createdTween:Play()
                end)
                UI.reset()
                assert(createdTween and createdTween.PlaybackState == states.Cancelled and not connection.Connected,
                    "A tween created during Destroying escaped Stop/reset")
                assert(not pcall(createdTween.Play, createdTween), "A cleanup-created tween could replay in the next generation")
                UI.pump(0.25)
                near(createdTarget.BackgroundTransparency, 0, "An animation created by cleanup kept advancing after Stop")
                assert(callbacks == 0, "Cleanup-created tween delivered a stale Completed callback")
            end
            UI.reset()
        end

        do
            -- A bounded batch exercises the published capacity without an
            -- unbounded stress loop or a wall-clock-dependent assertion.
            local capacity = 512
            local tweens, callbacks, connection = {}, 0, nil
            local specification = info(1, -1, true)
            for index = 1, capacity do
                local tween = TweenService:Create(frame(), specification, { BackgroundTransparency = 1 })
                tweens[index] = tween
                tween:Play()
                if index == 1 then connection = tween.Completed:Connect(function() callbacks += 1 end) end
            end
            local rejected = TweenService:Create(frame(), specification, { BackgroundTransparency = 1 })
            assert(not pcall(rejected.Play, rejected), "Tween active capacity was not enforced")
            assert(rejected.PlaybackState == states.Begin, "Rejected Play partially acquired animation ownership")
            UI.pump(0.25)
            near(tweens[1].Instance.BackgroundTransparency, 0.25)
            near(tweens[capacity].Instance.BackgroundTransparency, 0.25)
            assert(tweens[1].PlaybackState == states.Playing and tweens[capacity].PlaybackState == states.Playing,
                "Capacity rejection disrupted existing animations")
            UI.reset()
            drain()
            assert(callbacks == 0 and not connection.Connected, "Capacity reset retained completion listeners/events")
            local nextTween = TweenService:Create(frame(), info(1), { BackgroundTransparency = 1 })
            nextTween:Play()
            UI.pump(0.5)
            near(nextTween.Instance.BackgroundTransparency, 0.5, "Stop/reset did not release tween capacity")
            UI.reset()
        end
        print("TweenServiceTests passed: easing, typed interpolation, playback, conflicts, gradients, React and cleanup")
    end
    runTweenServiceTests()
end
