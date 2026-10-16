-- Engine host services required by unmodified React Luau and its dependencies.
-- These services perform no network or filesystem access.
local queue = {}
local heartbeatListeners = {}
local clock = 0
local generation = 0
local heartbeat = {}
if warn == nil then warn = print end

function heartbeat:Connect(callback)
    local connection = { Connected = true }
    function connection:Disconnect()
        self.Connected = false
        heartbeatListeners[self] = nil
    end
    heartbeatListeners[connection] = callback
    return connection
end

local function schedule(internal, seconds, callback, ...)
    assert(type(callback) == "function" or type(callback) == "thread", "task callback must be a function or coroutine")
    local item = { due = clock + math.max(0, seconds), callback = callback, args = table.pack(...), internal = internal, generation = generation }
    table.insert(queue, item)
    return item
end

task = {
    delay = function(seconds, callback, ...) return schedule(false, seconds, callback, ...) end,
    defer = function(callback, ...)
        return schedule(false, 0, callback, ...)
    end,
    spawn = function(callback, ...)
        return schedule(false, 0, callback, ...)
    end,
    cancel = function(item)
        if type(item) == "table" then item.cancelled = true end
    end,
}
-- Injected only into upstream Scheduler modules' private environments. Host
-- scheduler callbacks must survive a Play reset so its internal pending flags
-- can settle; callbacks created by game scripts belong to one Play generation.
__turboSchedulerTask = {
    delay = function(seconds, callback, ...) return schedule(true, seconds, callback, ...) end,
    defer = function(callback, ...) return schedule(true, 0, callback, ...) end,
    spawn = function(callback, ...) return schedule(true, 0, callback, ...) end,
    cancel = task.cancel,
}

__turboReactResetTasks = function()
    generation += 1
    local preserved = {}
    for _, item in ipairs(queue) do
        if item.internal and not item.cancelled then table.insert(preserved, item) end
    end
    queue = preserved
    for connection in pairs(heartbeatListeners) do connection.Connected = false end
    table.clear(heartbeatListeners)
end

local function quote(value)
    return '"' .. string.gsub(value, '[%z\1-\31\\"]', function(char)
        local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
        return escapes[char] or string.format('\\u%04x', string.byte(char))
    end) .. '"'
end

local httpService = {}
function httpService:JSONEncode(value)
    -- Upstream inspect() asks HttpService only to quote strings.
    assert(type(value) == "string", "Engine compatibility JSONEncode supports strings")
    return quote(value)
end
function httpService:UrlEncode(value)
    return (string.gsub(value, '[^%w%-_%.~]', function(char)
        return string.format('%%%02X', string.byte(char))
    end))
end
local services = { RunService = { Heartbeat = heartbeat }, HttpService = httpService }
game = {
    GetService = function(_, name)
        assert(services[name] ~= nil, "Unsupported engine compatibility service: " .. name)
        return services[name]
    end,
    DefineFastInt = function(_, _, defaultValue) return defaultValue end,
}

__turboReactPump = function(dt)
    clock += math.max(0, dt)
    for connection, callback in pairs(table.clone(heartbeatListeners)) do
        if connection.Connected then
            local ok, message = pcall(callback, dt)
            if not ok then connection:Disconnect(); error(message) end
        end
    end
    -- Run a snapshot: work scheduled during a callback runs on the next frame.
    -- This preserves scheduler yielding and prevents zero-delay recursive loops.
    local pending = queue
    queue = {}
    for index, item in ipairs(pending) do
        if not item.cancelled and (item.internal or item.generation == generation) then
            if item.due <= clock then
                local ok, message = pcall(function()
                    if type(item.callback) == "thread" then
                        local resumed, result = coroutine.resume(item.callback, table.unpack(item.args, 1, item.args.n))
                        if not resumed then error(result) end
                    else
                        item.callback(table.unpack(item.args, 1, item.args.n))
                    end
                end)
                if not ok then
                    -- A failed user callback must not discard remaining work,
                    -- including the React scheduler's next host invocation.
                    for remaining = index + 1, #pending do table.insert(queue, pending[remaining]) end
                    error(message)
                end
            else
                table.insert(queue, item)
            end
        end
    end
end

-- SafeFlags is an internal Roblox dependency. The engine supplies its documented
-- getter shape with explicit defaults, disabling Roblox-only experimental flags.
__turboSafeFlags = {
    createGetFFlag = function(name, defaultValue)
        local value = if name == "ReactIsolatedGlobalsEnabled" then true else (defaultValue == true)
        return function() return value end
    end,
    createGetFInt = function(_, defaultValue)
        return function() return defaultValue or 0 end
    end,
    createGetFString = function(_, defaultValue)
        return function() return defaultValue or "" end
    end,
}
