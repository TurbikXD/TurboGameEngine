local entities = { [99] = { tag = "Preview", position = vector.create(0, 1, 6) } }
local statuses, nextId = {}, 100
local allowed = { ["prefabs/jade.json"] = true, ["prefabs/amber.json"] = true, ["prefabs/violet.json"] = true }
local failSpawn = false
local function checkpoint()
    if __lab3NativeSmokeStages then coroutine.yield() end
end
local function englishMetadata(definitions, expected)
    local count = 0
    for name, definition in definitions do
        local labels = expected[name]
        assert(labels and definition.label == labels[1] and definition.category == labels[2], "Expected English Inspector metadata")
        for _, key in { "label", "category", "description" } do
            local value = definition[key]
            assert(type(value) == "string" and #value > 0, "Expected complete Inspector metadata")
            for index = 1, #value do
                local byte = string.byte(value, index)
                assert(byte >= 32 and byte <= 126, "Inspector metadata must use English ASCII")
            end
        end
        count += 1
    end
    assert(count == 4, "Expected every editable field")
end
Engine = {
    spawnPrefab = function(path)
        assert(allowed[path], "Unexpected project prefab")
        if failSpawn == "throw" then error("Malformed prefab fixture") end
        if failSpawn then return nil end
        nextId += 1
        entities[nextId] = { tag = "Lab3Spawned", path = path, position = vector.create(0, 0, 0) }
        return nextId
    end,
    isAlive = function(id) return entities[id] ~= nil end,
    getPosition = function(id) return entities[id] and entities[id].position end,
    setPosition = function(id, value) assert(entities[id]); entities[id].position = value end,
    setRotation = function(id, value) assert(entities[id]); entities[id].rotation = value end,
    destroy = function(id) entities[id] = nil end,
    setStatus = function(key, value) statuses[key] = value end,
    getStatus = function(key) return statuses[key] end,
}

local UI = require("ReactTurbo")
local Behavior = require("scripts/MaterialLab.luau")
local fields = {}
for name, definition in Behavior.fields do fields[name] = definition.default end
assert(fields.title == "СПЕКТР / Лаборатория материалов", "Expected the Russian authored demo title")
englishMetadata(Behavior.fields, {
    title = { "Title", "Presentation" }, rounds = { "Order Length", "Gameplay" },
    spinSpeed = { "Rotation Speed", "Gallery" }, animate = { "Animate Samples", "Gallery" },
})
local owner = setmetatable({ entity = 1, state = {}, fields = fields }, { __index = Behavior })

local function visit(node, value)
    if node.props and node.props.Text == value then return node end
    for _, child in node.children or {} do
        local found = visit(child, value)
        if found then return found end
    end
    return nil
end

local function find(value)
    for _, root in UI.getRoots() do
        local found = visit(root, value)
        if found then return found end
    end
    error("Missing rendered UI: " .. value)
end

local function tick(dt)
    owner:Update(dt or 1 / 60)
    UI.pump(0)
end

local function click(value)
    local node = find(value)
    assert(node.type == "TextButton", "Expected rendered button")
    UI.dispatch(node, "Activated")
    tick()
end

owner:Init()
checkpoint()
tick()
assert(#UI.getRoots() == 1 and owner.state.goal == 6)
find(fields.title)
checkpoint()
click("Создать: Янтарь")
assert(owner.state.progress == 0 and owner.state.mistakes == 1 and #owner.state.spawned == 0)
failSpawn = true
click("Создать: Нефрит")
assert(owner.state.progress == 0, "Failed native spawn must not award progress")
failSpawn = "throw"
click("Создать: Нефрит")
assert(owner.state.progress == 0 and #owner.state.spawned == 0, "Thrown native spawn errors must leave the controller usable")
find("Не удалось создать prefab. Подробности — в Script Output.")
failSpawn = false
checkpoint()
for _, name in { "Нефрит", "Аметист", "Янтарь", "Нефрит", "Аметист", "Янтарь" } do click("Создать: " .. name) end
assert(owner.state.finished and #owner.state.spawned == 6 and statuses.lab3Materials.finished)
checkpoint()
local first = owner.state.spawned[1].id
assert(entities[first].path == "prefabs/jade.json")
local angle = entities[first].rotation.y
fields.spinSpeed = 2
tick(0.5)
assert(math.abs(entities[first].rotation.y - angle - 1) < 0.0001, "Changing rotation speed must not reinterpret elapsed time")
fields.animate = false
angle = entities[first].rotation.y
tick(0.5)
assert(entities[first].rotation.y == angle, "Inspector boolean must stop game-owned sample animation")
fields.animate = true
tick(0.25)
assert(math.abs(entities[first].rotation.y - angle - 0.5) < 0.0001, "Resuming animation must preserve its phase")
click("Создать: Нефрит")
assert(#owner.state.spawned == 6, "Completed orders must not grow without bound")
click("Только 3D-витрина")
assert(owner.state.compact)
click("Открыть лабораторию")
assert(not owner.state.compact)
checkpoint()
click("Защита Material Graph")
assert(owner.state.page == 2)
find("02 / Докинг графа")
click("Следующая тема >")
find("03 / Живой предпросмотр")
click("Следующая тема >")
find("04 / Черновик и Undo/Redo")
click("К игровому заказу")
assert(owner.state.page == 1 and owner.state.finished and #owner.state.spawned == 6)
checkpoint()
for page = 2, 6 do click("Следующая тема >"); assert(owner.state.page == page) end
click("Следующая тема >")
assert(owner.state.page == 6)
checkpoint()
for page = 5, 1, -1 do click("< Предыдущая тема"); assert(owner.state.page == page) end
checkpoint()
fields.rounds = 3
fields.title = "Изменено в Inspector"
click("Новый заказ")
assert(owner.state.goal == 3 and owner.state.progress == 0 and not entities[first])
find(fields.title)
for _, name in { "Нефрит", "Аметист", "Янтарь" } do click("Создать: " .. name) end
assert(owner.state.finished and #owner.state.spawned == 3)
checkpoint()

local Motion = require("scripts/SampleMotion.luau")
englishMetadata(Motion.fields, {
    speed = { "Rotation Speed", "Movement" }, bobbing = { "Bobbing", "Movement" },
    height = { "Amplitude", "Movement" }, caption = { "Caption", "Presentation" },
})
assert(Motion.fields.caption.default == "Образец материала", "English metadata must preserve the Russian authored caption")
local motionFields = {}
for name, definition in Motion.fields do motionFields[name] = definition.default end
local motion = setmetatable({ entity = 99, state = {}, fields = motionFields }, { __index = Motion })
motion:Init()
assert(statuses.lab3SampleCaptions["99"] == "Образец материала")
motion:Update(0.5)
assert(entities[99].rotation.y > 0 and entities[99].position.y > 1)
angle = entities[99].rotation.y
motionFields.speed = 2
motion:Update(0.5)
assert(math.abs(entities[99].rotation.y - angle - 1) < 0.0001, "Preview speed edits must preserve their phase")
motionFields.bobbing = false
motion:Update(0.5)
assert(entities[99].position.y == 1)
motionFields.caption = "Нефрит / подпись из Inspector"
motion:Update(0)
tick()
find("БАЗА + ЭКЗЕМПЛЯРЫ\nНефрит / подпись из Inspector\n\nЦвет, UV, смешивание и глубина")
motion:Destroy()
assert(next(statuses.lab3SampleCaptions) == nil, "Motion teardown must release its status entry")
checkpoint()
local extraMotions = {}
for index = 1, 10 do
    local id = 200 + index
    entities[id] = { tag = "Preview", position = vector.create(0, 1, 6) }
    local extra = setmetatable({ entity = id, state = {}, fields = motionFields }, { __index = Motion })
    extra:Init()
    table.insert(extraMotions, extra)
end
local function captionCount()
    local count = 0
    for _ in statuses.lab3SampleCaptions do count += 1 end
    return count
end
assert(captionCount() == 8, "Exported motion prefabs must not grow the caption status beyond its bound")
extraMotions[1]:Destroy()
extraMotions[10]:Update(0)
assert(captionCount() == 8 and statuses.lab3SampleCaptions["210"] ~= nil, "A freed caption slot must accept a waiting live preview")
for _, extra in extraMotions do extra:Destroy() end
assert(next(statuses.lab3SampleCaptions) == nil)
checkpoint()
owner:Destroy()
assert(#UI.getRoots() == 0 and entities[99] ~= nil, "Destroy must release UI and own samples only")
for _, entity in entities do assert(entity.tag ~= "Lab3Spawned") end
print("Lab3: real vendored React mount/events; order, wrong choice, failed spawn, fields, navigation, transforms and cleanup passed. Engine API was explicitly mocked; GPU not tested.")
