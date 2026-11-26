local React = require("React")
local Shared = require("Shared")
local Scheduler = require("Scheduler")
local Polyfill = require("LuauPolyfill")
local Instances = __turboRobloxUI
local Type = Shared.Type
local roots = {}
local pendingEvents = Instances.pendingEvents
local hostTypes = Instances.hostTypes
local mountedRoots = {}
local resetting = false
local wrappers = setmetatable({}, { __mode = "kv" })
local function rebuildRoots()
    table.clear(roots)
    local candidates = {}
    for container, root in pairs(mountedRoots) do if root.mounted and not container.disposed then candidates[container] = true end end
    local playerGui = Instances.playerGui
    if #playerGui.children > 0 and not playerGui.disposed then candidates[playerGui] = true end
    local tops = {}
    for target in pairs(candidates) do
        local ancestor, top = rawget(target, "parent"), target
        while ancestor do top = ancestor; ancestor = rawget(ancestor, "parent") end
        tops[top] = true
    end
    for top in pairs(tops) do
        local owner = mountedRoots[top]
        if owner and owner.legacy then table.insert(roots, top)
        elseif #top.children > 0 or (top.type ~= "Folder" and top.type ~= "PlayerGui") then
            local wrapper = wrappers[top]
            if not wrapper then wrapper = { type = "Folder", id = top.id, props = {}, children = { top } }; wrappers[top] = wrapper end
            wrapper.props.Name = top.props.Name
            table.insert(roots, wrapper)
        end
    end
    table.sort(roots, function(a, b) return a.id < b.id end)
end
Instances.setTreeObserver(rebuildRoots)

local disconnectBindings = Instances.disconnectBindings

local function applyProps(instance, props)
    disconnectBindings(instance)
    local previousAssigned = rawget(instance, "_assignedProps") or {}
    local previousValues = rawget(instance, "_reactPropsValues") or {}
    instance.events = {}
    local values = {}
    local assigned = { Name = true, Archivable = true, AbsolutePosition = true, AbsoluteSize = true, TextBounds = true, AbsoluteContentSize = true }
    for key, value in pairs(props) do
        if Type.of(key) == Type.HostEvent then
            assert(type(value) == "function", "React.Event callback must be a function")
            assert(Instances.isEvent(instance, key.name), "Unsupported " .. instance.type .. " event: " .. key.name)
            instance.events[key.name] = value
        elseif Type.of(key) == Type.HostChangeEvent then
            assert(type(value) == "function", "React.Change callback must be a function")
            assert(Instances.hasProperty(instance, key.name), "Unsupported " .. instance.type .. " property: " .. key.name)
            instance.events["Change." .. key.name] = value
        elseif type(key) == "string" and key ~= "children" then
            values[key] = value
            assigned[key] = true
        end
    end
    for key, value in pairs(values) do
        if type(value) == "table" and rawget(value, "$$typeof") == Shared.ReactSymbols.REACT_BINDING_TYPE then
            Instances.setProperty(instance, key, value:getValue())
            instance.bindings[key] = React.__subscribeToBinding(value, function(current) Instances.setProperty(instance, key, current) end)
            Instances.bindingAssigned(instance, key)
        elseif previousValues[key] ~= value then Instances.setProperty(instance, key, value) end
    end
    local defaultProps = rawget(instance, "_defaultProps") or {}
    for key in pairs(previousAssigned) do
        if not assigned[key] and not Instances.isReadonlyProperty(instance, key) then Instances.setProperty(instance, key, defaultProps[key]) end
    end
    rawset(instance, "_assignedProps", assigned)
    rawset(instance, "_reactPropsValues", values)
end

local function remove(parent, child)
    parent = rawget(parent, "_portalTarget") or parent
    if rawget(child, "parent") == parent then Instances.setParent(child, nil) end
end
local function append(parent, child)
    parent = rawget(parent, "_portalTarget") or parent
    if rawget(child, "parent") == parent then Instances.setParent(child, nil) end
    Instances.setParent(child, parent)
end
local function insertBefore(parent, child, before)
    parent = rawget(parent, "_portalTarget") or parent
    Instances.setParent(child, parent, before)
end
local function dispose(instance)
    instance:Destroy()
end
local function noOp() end
local context = {}
local config = {
    supportsMutation = true, supportsPersistence = false,
    supportsHydration = false, supportsTestSelectors = false,
    isPrimaryRenderer = true, warnsIfNotActing = false,
    getRootHostContext = function() return context end,
    getChildHostContext = function() return context end,
    getPublicInstance = function(instance) return instance end,
    prepareForCommit = noOp, resetAfterCommit = noOp,
    preparePortalMount = noOp, commitMount = noOp,
    beforeActiveInstanceBlur = noOp, afterActiveInstanceBlur = noOp,
    prepareScopeUpdate = noOp,
    shouldSetTextContent = function() return false end,
    createInstance = function(hostType, props, rootContainer, _, handle)
        assert(hostTypes[hostType], "Unsupported ReactTurbo host: " .. tostring(hostType))
        local instance = Instances.create(hostType)
        if rawget(rootContainer, "_robloxRoot") then Instances.applyDefaults(instance) end
        if handle and handle.key ~= nil and props.Name == nil then instance.props.Name = tostring(handle.key) end
        applyProps(instance, props)
        return instance
    end,
    createTextInstance = function(text, rootContainer)
        local instance = Instances.create("TextLabel")
        if rawget(rootContainer, "_robloxRoot") then Instances.applyDefaults(instance) end
        instance.props.Text = tostring(text)
        return instance
    end,
    appendInitialChild = append, appendChild = append,
    appendChildToContainer = append,
    insertBefore = insertBefore, insertInContainerBefore = insertBefore,
    removeChild = function(parent, child) remove(parent, child); dispose(child) end,
    removeChildFromContainer = function(parent, child) remove(parent, child); dispose(child) end,
    clearContainer = function(container)
        for _, child in ipairs(table.clone(container.children)) do dispose(child) end
        table.clear(container.children)
    end,
    finalizeInitialChildren = function() return false end,
    prepareUpdate = function() return true end,
    commitUpdate = function(instance, _, _, _, props) applyProps(instance, props) end,
    commitTextUpdate = function(instance, _, text) Instances.setProperty(instance, "Text", text) end,
    resetTextContent = function(instance) Instances.setProperty(instance, "Text", nil) end,
    hideInstance = function(instance) instance.hidden = true end,
    hideTextInstance = function(instance) instance.hidden = true end,
    unhideInstance = function(instance) instance.hidden = false end,
    unhideTextInstance = function(instance) instance.hidden = false end,
    scheduleTimeout = Polyfill.setTimeout, cancelTimeout = Polyfill.clearTimeout,
    noTimeout = -1, now = Scheduler.unstable_now,
    detachDeletedInstance = dispose,
}
for name, implementation in pairs(Shared.ReactFiberHostConfig.WithNoHydration) do config[name] = implementation end
for name, implementation in pairs(Shared.ReactFiberHostConfig.WithNoPersistence) do config[name] = implementation end
for name, implementation in pairs(Shared.ReactFiberHostConfig.WithNoTestSelectors) do config[name] = implementation end
local Reconciler = require("ReactReconciler")(config)
local Renderer = {}

Renderer.UDim = UDim
Renderer.UDim2 = UDim2
Renderer.Vector2 = Vector2
Renderer.Color3 = Color3
Renderer.Rect = Rect
Renderer.ColorSequence = ColorSequence
Renderer.ColorSequenceKeypoint = ColorSequenceKeypoint
Renderer.NumberSequence = NumberSequence
Renderer.NumberSequenceKeypoint = NumberSequenceKeypoint
Renderer.TweenInfo = TweenInfo
Renderer.Instance = Instance
Renderer.Enum = Enum

local nativeVersion = __turboNativeUIVersion or 1
local nativeFeatures = table.freeze({ version = nativeVersion, images = nativeVersion >= 2,
    gradient = nativeVersion >= 2, controls = nativeVersion >= 2, canvasGroups = nativeVersion >= 3 })
function Renderer.getNativeFeatures() return nativeFeatures end

function Renderer.createRoot(options)
    assert(not resetting, "Cannot create a React root during UI reset")
    options = options or {}
    local explicit = Instances.isInstance(options)
    local container = if explicit then options else Instances.create("Folder")
    assert(not container.disposed, "Cannot mount a root on a destroyed UI Instance")
    assert(mountedRoots[container] == nil, "A React root already owns this UI container")
    if not explicit then container.props.Name = options.Name or "Script UI" end
    rawset(container, "_robloxRoot", explicit)
    local fiber = Reconciler.createContainer(container, Reconciler.ReactRootTags.LegacyRoot, false, nil)
    local mounted = true
    local root = { container = container, mounted = true, legacy = not explicit }
    mountedRoots[container] = root
    rawset(container, "root", root)
    rebuildRoots()
    function root:render(element)
        assert(not resetting, "Cannot render a React root during UI reset")
        assert(mounted, "Cannot render an unmounted root")
        assert(not container.disposed, "Cannot render into a destroyed UI container")
        self.element = element
        Reconciler.flushSync(function() Reconciler.updateContainer(element, fiber, nil, nil) end)
    end
    function root:unmount()
        if not mounted then return end
        local ok, message = pcall(function()
            Reconciler.flushSync(function() Reconciler.updateContainer(nil, fiber, nil, nil) end)
            Reconciler.flushPassiveEffects()
        end)
        mounted = false
        root.mounted = false
        mountedRoots[container] = nil
        rawset(container, "root", nil)
        rawset(container, "_robloxRoot", nil)
        rebuildRoots()
        if not ok then error(message) end
    end
    return root
end

function Renderer.dispatch(instance, eventName, value, args)
    if instance.disposed then return end
    Reconciler.batchedUpdates(function() Instances.dispatch(instance, eventName, value, args) end)
end

function Renderer.pump(dt)
    local events = table.clone(pendingEvents)
    table.clear(pendingEvents)
    for index, event in ipairs(events) do
        local ok, message = pcall(Renderer.dispatch, event.instance, event.name, event.value, event.args)
        if not ok then
            for remaining = index + 1, #events do table.insert(pendingEvents, events[remaining]) end
            error(message)
        end
    end
    __turboReactPump(dt)
    if __turboTweenPump then __turboTweenPump(dt) end
    Reconciler.flushPassiveEffects()
end

Renderer.createPortal = function(element, container, key)
    assert(Instances.isInstance(container) and not container.disposed, "Portal target must be a live UI Instance")
    return Reconciler.createPortal(element, container, nil, key)
end
Renderer.flushSync = Reconciler.flushSync
Renderer.unstable_batchedUpdates = Reconciler.batchedUpdates
Renderer.ReactRoblox = {
    createRoot = Renderer.createRoot, createLegacyRoot = Renderer.createRoot,
    createPortal = Renderer.createPortal, flushSync = Renderer.flushSync,
    unstable_batchedUpdates = Renderer.unstable_batchedUpdates,
    Event = React.Event, Change = React.Change,
}
local portablePortals = setmetatable({}, { __mode = "kv" })
function Renderer.ReactRoblox.createPortal(element, container, key)
    assert(Instances.isInstance(container) and not container.disposed, "Portal target must be a live UI Instance")
    local wrapper = portablePortals[container]
    if not wrapper then
        wrapper = { _portalTarget = container, _robloxRoot = true, children = container.children }
        portablePortals[container] = wrapper
    end
    return Reconciler.createPortal(element, wrapper, nil, key)
end
Renderer.getRoots = function() return roots end
Renderer.hostTypes = hostTypes
Renderer.events = pendingEvents
function Renderer.refresh(replacements)
    local function refreshElement(element)
        if type(element) ~= "table" then return element end
        if React.isValidElement(element) then
            local refreshed = table.clone(element)
            refreshed.type = replacements[element.type] or element.type
            local props = table.clone(element.props)
            props.children = refreshElement(props.children)
            refreshed.props = props
            return refreshed
        end
        if rawget(element, "$$typeof") == Shared.ReactSymbols.REACT_PORTAL_TYPE then
            local refreshed = table.clone(element)
            refreshed.children = refreshElement(element.children)
            return refreshed
        end
        local children = {}
        for key, child in pairs(element) do children[key] = refreshElement(child) end
        return children
    end
    for _, root in pairs(table.clone(mountedRoots)) do
        if root.element then root:render(refreshElement(root.element)) end
    end
end
local function resetRenderer()
    local failure
    if __turboTweenReset then local ok, message = pcall(__turboTweenReset); if not ok then failure = message end end
    for _, root in pairs(table.clone(mountedRoots)) do local ok, message = pcall(root.unmount, root); if not ok then failure = failure or message end end
    local ok, message = pcall(Instances.reset)
    if not ok then failure = failure or message end
    if __turboTweenReset then local resetOk, resetMessage = pcall(__turboTweenReset); if not resetOk then failure = failure or resetMessage end end
    rebuildRoots()
    table.clear(pendingEvents)
    __turboReactResetTasks()
    if failure then error(failure) end
end
function Renderer.reset()
    assert(not resetting, "UI reset is already in progress")
    resetting = true
    local ok, message = pcall(resetRenderer)
    resetting = false
    if not ok then error(message, 0) end
end
return Renderer
