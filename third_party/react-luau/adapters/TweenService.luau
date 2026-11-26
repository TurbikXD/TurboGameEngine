local UI = __turboRobloxUI
local State = Enum.PlaybackState
local records = setmetatable({}, { __mode = "kv" })
local owners = setmetatable({}, { __mode = "k" })
local byTarget = setmetatable({}, { __mode = "k" })
local active, pending = {}, {}
local ownedCount, nextId, generation = 0, 0, 0
local MAX_TWEENS, MAX_GOALS, MAX_EVENTS = 512, 64, 2048
local continuous = { number = true, UDim = true, UDim2 = true, Vector2 = true, Color3 = true }

local function finite(value, fallback)
    if value == nil then value = fallback end
    assert(type(value) == "number" and value == value and math.abs(value) < math.huge, "Tween value must be a finite number")
    return value
end
local function enum(value, enumType, fallback)
    if value == nil then value = fallback end
    assert(typeof(value) == "EnumItem" and value.EnumType == enumType, "Tween expects " .. tostring(enumType))
    return value
end
local infoMeta = table.freeze({ __tostring = function(value)
    return "TweenInfo(" .. value.Time .. ", " .. tostring(value.EasingStyle) .. ", " .. tostring(value.EasingDirection) .. ")"
end })
TweenInfo = table.freeze({ new = function(time, style, direction, repeatCount, reverses, delayTime)
    time = finite(time, 1); delayTime = finite(delayTime, 0); repeatCount = finite(repeatCount, 0)
    assert(time >= 0 and delayTime >= 0, "Tween time and delay must be nonnegative")
    assert(repeatCount >= -1 and repeatCount % 1 == 0, "Tween RepeatCount must be an integer at least -1")
    if reverses == nil then reverses = false end
    assert(type(reverses) == "boolean", "Tween Reverses must be a boolean")
    return UI.tagValue({ Time = time, EasingStyle = enum(style, Enum.EasingStyle, Enum.EasingStyle.Quad),
        EasingDirection = enum(direction, Enum.EasingDirection, Enum.EasingDirection.Out), RepeatCount = repeatCount,
        Reverses = reverses, DelayTime = delayTime }, "TweenInfo", infoMeta)
end })

local function bounceOut(value)
    local n, d = 7.5625, 2.75
    if value < 1 / d then return n * value * value end
    if value < 2 / d then value -= 1.5 / d; return n * value * value + 0.75 end
    if value < 2.5 / d then value -= 2.25 / d; return n * value * value + 0.9375 end
    value -= 2.625 / d; return n * value * value + 0.984375
end
local function easeIn(value, style)
    if style == "Linear" then return value end
    if style == "Sine" then return 1 - math.cos(value * math.pi / 2) end
    if style == "Quad" then return value * value end
    if style == "Cubic" then return value ^ 3 end
    if style == "Quart" then return value ^ 4 end
    if style == "Quint" then return value ^ 5 end
    if style == "Circular" then return 1 - math.sqrt(math.max(0, 1 - value * value)) end
    if style == "Exponential" then return 2 ^ (10 * value - 10) end
    if style == "Back" then return value * value * (2.70158 * value - 1.70158) end
    if style == "Bounce" then return 1 - bounceOut(1 - value) end
    if style == "Elastic" then return -(2 ^ (10 * value - 10)) * math.sin((10 * value - 10.75) * (2 * math.pi / 3)) end
    error("Unsupported tween easing style")
end
local function easing(alpha, style, direction)
    alpha = math.clamp(finite(alpha), 0, 1)
    if alpha == 0 or alpha == 1 then return alpha end
    local name, way = style.Name, direction.Name
    if way == "In" then return easeIn(alpha, name) end
    if way == "Out" then return 1 - easeIn(1 - alpha, name) end
    if name == "Back" then
        local c = 1.70158 * 1.525
        if alpha < 0.5 then local value = alpha * 2; return value * value * ((c + 1) * value - c) / 2 end
        local value = alpha * 2 - 2; return (value * value * ((c + 1) * value + c) + 2) / 2
    end
    if name == "Elastic" then
        local period = 2 * math.pi / 4.5
        if alpha < 0.5 then return -(2 ^ (20 * alpha - 10)) * math.sin((20 * alpha - 11.125) * period) / 2 end
        return (2 ^ (-20 * alpha + 10)) * math.sin((20 * alpha - 11.125) * period) / 2 + 1
    end
    return if alpha < 0.5 then easeIn(alpha * 2, name) / 2 else 1 - easeIn(2 - alpha * 2, name) / 2
end
local function interpolate(first, last, alpha, kind)
    if alpha == 0 then return first end
    if alpha == 1 then return last end
    if kind == "number" then return first * (1 - alpha) + last * alpha end
    if kind == "UDim" then return UDim.new(first.Scale * (1 - alpha) + last.Scale * alpha, first.Offset * (1 - alpha) + last.Offset * alpha) end
    if kind == "UDim2" then return UDim2.new(interpolate(first.X, last.X, alpha, "UDim"), interpolate(first.Y, last.Y, alpha, "UDim")) end
    if kind == "Vector2" then return Vector2.new(first.X * (1 - alpha) + last.X * alpha, first.Y * (1 - alpha) + last.Y * alpha) end
    return Color3.new(first.R * (1 - alpha) + last.R * alpha, first.G * (1 - alpha) + last.G * alpha, first.B * (1 - alpha) + last.B * alpha)
end
local function queue(tween, record, kind, state)
    assert(#pending < MAX_EVENTS, "Tween event queue limit exceeded")
    table.insert(pending, { tween = tween, record = record, kind = kind, state = state, generation = generation })
end
local function changeState(tween, record, state)
    if record.state == state then return end
    record.state = state
    if record.playbackSignal or record.changedSignal then queue(tween, record, "state", state) end
end
local function release(tween, record)
    active[tween] = nil
    if not record.owned then return end
    record.owned = false; ownedCount -= 1
    local properties = owners[record.target]
    if properties then
        for property in pairs(record.goals) do if properties[property] == tween then properties[property] = nil end end
        if next(properties) == nil then owners[record.target] = nil end
    end
end
local function cancel(tween, record)
    if record.destroyed or (record.state ~= State.Playing and record.state ~= State.Delayed and record.state ~= State.Paused) then return end
    release(tween, record)
    record.phase = 0; record.remaining = record.info.RepeatCount + 1
    changeState(tween, record, State.Cancelled)
    queue(tween, record, "complete", State.Cancelled)
end
local function dispose(tween, record)
    if record.destroyed then return end
    release(tween, record); record.destroyed = true
    record.state = State.Cancelled
    record.completed._clear()
    if record.playbackSignal then record.playbackSignal._clear() end
    if record.changedSignal then record.changedSignal._clear() end
end
local function alive(tween)
    local record = records[tween]
    assert(record and not record.destroyed, "Tween has been destroyed or reset")
    assert(not record.target.disposed and not rawget(record.target, "_destroying"), "Tween target has been destroyed")
    return record
end
local function goalValue(target, property, value)
    assert(type(property) == "string", "Tween goal keys must be property names")
    UI.validateProperty(target, property, value)
    local kind = typeof(value)
    assert(continuous[kind], "Unsupported tween goal type: " .. tostring(kind))
    assert(property ~= "FillDirectionMaxCells", "FillDirectionMaxCells is discrete and cannot be tweened")
    assert(target.bindings[property] == nil, "Cannot tween React-bound property: " .. property)
    local current = target[property]
    assert(typeof(current) == kind, "Tween start and goal types differ for " .. property)
    if kind == "number" then finite(current); finite(value) end
    return current, kind
end
local methods = {}
function methods:Play()
    local record = alive(self)
    if record.state == State.Playing or record.state == State.Delayed then return end
    local starts, kinds = {}, {}
    for property, value in pairs(record.goals) do starts[property], kinds[property] = goalValue(record.target, property, value) end
    local conflicts, properties = {}, owners[record.target]
    if properties then for property in pairs(record.goals) do local previous = properties[property]; if previous and previous ~= self then conflicts[previous] = true end end end
    for previous in pairs(conflicts) do cancel(previous, records[previous]) end
    if not record.owned then
        assert(ownedCount < MAX_TWEENS, "Tween active/paused limit exceeded")
        ownedCount += 1; record.owned = true
    end
    properties = owners[record.target] or {}; owners[record.target] = properties
    for property in pairs(record.goals) do properties[property] = self end
    if record.state ~= State.Paused then
        record.starts, record.kinds = starts, kinds; record.phase = 0; record.remaining = record.info.RepeatCount + 1
    end
    active[self] = record
    changeState(self, record, if record.info.DelayTime > 0 and record.phase < record.delay then State.Delayed else State.Playing)
end
function methods:Pause()
    local record = alive(self)
    if record.state == State.Playing then active[self] = nil; changeState(self, record, State.Paused) end
end
function methods:Cancel()
    local record = records[self]
    if record and not record.destroyed then cancel(self, record) end
end
function methods:Destroy()
    local record = records[self]
    if record then dispose(self, record) end
end
function methods:IsA(className) return className == "Tween" or className == "TweenBase" or className == "Instance" end
function methods:GetPropertyChangedSignal(property)
    local record = alive(self)
    assert(property == "PlaybackState", "Tween only changes PlaybackState")
    if not record.playbackSignal then record.playbackSignal = UI.createSignal() end
    return record.playbackSignal
end
local function tweenMeta(record)
    return table.freeze({
    __index = function(tween, name)
        if methods[name] then return methods[name] end
        if name == "ClassName" or name == "Name" then return "Tween" end
        if name == "Instance" then return record.target end
        if name == "TweenInfo" then return record.info end
        if name == "PlaybackState" then return record.state end
        if name == "Completed" then return record.completed end
        if name == "Changed" then if not record.changedSignal then record.changedSignal = UI.createSignal() end; return record.changedSignal end
        error("Unsupported Tween member: " .. tostring(name), 2)
    end,
    __tostring = function() return "Tween" end,
    })
end
local service = {}
function service:Create(target, info, goals)
    assert(UI.isInstance(target) and not target.disposed and not rawget(target, "_destroying"), "TweenService expects a live UI Instance")
    assert(typeof(info) == "TweenInfo", "TweenService expects TweenInfo")
    assert(type(goals) == "table", "Tween goals must be a table")
    assert(not (info.Time == 0 and info.DelayTime == 0 and info.RepeatCount == -1), "An infinitely repeating zero-duration tween needs a positive delay")
    local copy, count = {}, 0
    for property, value in pairs(goals) do
        count += 1; assert(count <= MAX_GOALS, "Tween property limit exceeded")
        goalValue(target, property, value); copy[property] = value
    end
    assert(count > 0, "Tween needs at least one goal property")
    nextId += 1
    local scale = math.max(info.Time, info.DelayTime, 1)
    local record = { target = target, info = info, goals = table.freeze(copy), starts = {}, kinds = {},
        state = State.Begin, destroyed = false, owned = false, completed = UI.createSignal(), id = nextId,
        phase = 0, remaining = info.RepeatCount + 1, scale = scale, time = info.Time / scale, delay = info.DelayTime / scale }
    record.period = record.delay + record.time * (if info.Reverses then 2 else 1)
    local tween = UI.registerType(setmetatable({}, tweenMeta(record)), "Instance")
    records[tween] = record
    local targetTweens = byTarget[target]
    if not targetTweens then targetTweens = setmetatable({}, { __mode = "kv" }); byTarget[target] = targetTweens end
    targetTweens[tween] = record
    return table.freeze(tween)
end
function service:GetValue(alpha, style, direction)
    return easing(alpha, enum(style, Enum.EasingStyle), enum(direction, Enum.EasingDirection))
end
service = table.freeze(service)
local previousGetService = game.GetService
game.GetService = function(self, name) if name == "TweenService" then return service end; return previousGetService(self, name) end

local function step(tween, record, dt)
    if record.target.disposed then dispose(tween, record); return end
    for property in pairs(record.goals) do if record.target.bindings[property] then cancel(tween, record); return end end
    local total = record.phase + dt / record.scale
    local finished = record.period == 0
    if record.period > 0 then
        local skipped = math.floor(total / record.period)
        if record.info.RepeatCount >= 0 and skipped >= record.remaining then finished = true
        else
            record.phase = math.fmod(total, record.period)
            if record.info.RepeatCount >= 0 then record.remaining -= skipped end
        end
    end
    local alpha
    if finished then alpha = if record.info.Reverses then 0 else 1
    elseif record.phase < record.delay then changeState(tween, record, State.Delayed); alpha = 0
    else
        changeState(tween, record, State.Playing)
        local elapsed = record.phase - record.delay
        if record.time == 0 then alpha = if record.info.Reverses then 0 else 1
        elseif elapsed <= record.time then alpha = elapsed / record.time
        else alpha = 1 - (elapsed - record.time) / record.time end
        alpha = easing(alpha, record.info.EasingStyle, record.info.EasingDirection)
    end
    for property, value in pairs(record.goals) do
        if record.destroyed or active[tween] ~= record then return end
        UI.setProperty(record.target, property, interpolate(record.starts[property], value, alpha, record.kinds[property]))
    end
    if finished then release(tween, record); changeState(tween, record, State.Completed); queue(tween, record, "complete", State.Completed) end
end
__turboTweenPump = function(dt)
    dt = finite(dt, 0); assert(dt >= 0, "Tween frame delta must be nonnegative")
    local snapshot = {}
    for tween, record in pairs(active) do table.insert(snapshot, { tween = tween, record = record }) end
    table.sort(snapshot, function(a, b) return a.record.id < b.record.id end)
    for _, entry in ipairs(snapshot) do
        if active[entry.tween] == entry.record and not entry.record.destroyed then
            local ok, message = pcall(step, entry.tween, entry.record, dt)
            if not ok then cancel(entry.tween, entry.record); error(message) end
        end
    end
    local events = pending; pending = {}
    for index, event in ipairs(events) do
        if event.generation == generation and not event.record.destroyed then
            local ok, message = pcall(function()
                if event.kind == "complete" then event.record.completed._fire(event.state)
                else
                    if event.record.playbackSignal then event.record.playbackSignal._fire() end
                    if event.record.changedSignal then event.record.changedSignal._fire("PlaybackState") end
                end
            end)
            if not ok then
                for remaining = index + 1, #events do if #pending < MAX_EVENTS then table.insert(pending, events[remaining]) end end
                error(message)
            end
        end
    end
end
__turboTweenReset = function()
    generation += 1
    for tween, record in pairs(records) do dispose(tween, record) end
    table.clear(active); table.clear(pending); table.clear(owners); table.clear(byTarget); ownedCount = 0
end
UI.addDestroyObserver(function(target)
    local targetTweens = byTarget[target]
    if targetTweens then for tween, record in pairs(targetTweens) do dispose(tween, record) end; byTarget[target] = nil end
end)
UI.onBindingAssigned = function(target, property)
    local properties = owners[target]
    local tween = properties and properties[property]
    if tween then cancel(tween, records[tween]) end
end
