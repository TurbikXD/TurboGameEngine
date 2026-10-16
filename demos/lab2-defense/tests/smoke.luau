-- Headless application/React fixture. Engine here is a small test double:
-- these assertions do NOT validate native ECS, disk watching, fonts or GPU UI.
local entities, status, nextId = {}, {}, 100
Engine = {
    spawnPrefab = function(path)
        assert(path == "prefabs/marker.json", "Unexpected prefab path")
        nextId += 1
        entities[nextId] = { tag = "Lab2Marker", position = vector.create(0, 0, 5) }
        return nextId
    end,
    find = function(tag)
        local found = {}
        for id, item in entities do if item.tag == tag then table.insert(found, id) end end
        table.sort(found)
        return found
    end,
    isAlive = function(id) return entities[id] ~= nil end,
    getTag = function(id) return entities[id] and entities[id].tag end,
    getPosition = function(id) return entities[id] and entities[id].position end,
    setPosition = function(id, position) assert(entities[id]); entities[id].position = position end,
    setRotation = function(id, rotation) assert(entities[id]); entities[id].rotation = rotation end,
    destroy = function(id) entities[id] = nil end,
    setStatus = function(key, value) status[key] = value end,
    getStatus = function(key) return if key then status[key] else status end,
}
Input = { wasKeyPressed = function() return false end }

local React = require("React")
local UI = require("ReactTurbo")
assert(not pcall(require, "scripts/DefenseContent"), "Project loader must not invent extensionless aliases")
local Content = require("scripts/DefenseContent.luau")
local Behavior = require("scripts/Defense.luau")
local owner = setmetatable({ state = {}, fields = {}, entity = 1 }, { __index = Behavior })

local function findText(node, text, buttonOnly)
    if node.props and node.props.Text == text and (not buttonOnly or node.type == "TextButton") then return node end
    for _, child in node.children or {} do
        local found = findText(child, text, buttonOnly)
        if found then return found end
    end
    return nil
end

local function find(text, buttonOnly)
    for _, root in UI.getRoots() do
        local found = findText(root, text, buttonOnly)
        if found then return found end
    end
    error("Missing rendered UI text: " .. text)
end

local function tick()
    owner:Update(1 / 60)
    UI.pump(0)
end

local function click(text)
    local instance = find(text, true)
    assert(instance.type == "TextButton", "Expected an actual button")
    UI.dispatch(instance, "Activated")
    tick()
end

local function topic(id)
    for _, item in Content.topics do if item.id == id then return item end end
    error("Missing topic: " .. id)
end

local function go(id) click(topic(id).title) end

owner:Init()
tick()
assert(#Content.topics == 8 and #Content.quiz == 3)
assert(#UI.getRoots() == 1 and owner.state.initCount == 1)
find(Content.title)
click("Далее >")
assert(owner.state.page == 2 and owner.state.visited[2])
for _, question in Content.quiz do click(question.answers[question.correct]) end
assert(owner.state.badges.boundary, "Quiz did not complete through rendered event callbacks")

go("lifecycle")
click("self.state.counter + 1")
click("self.state.counter + 1")
assert(owner.state.counter == 2)
go("react")
click("React setState + 1")
find("Локальное состояние UI: 1")
click("Показать ссылки на исходники")
assert(owner.state.root, "Source toggle unmounted the application")

go("prefab")
for _ = 1, 3 do click("Создать настоящий prefab") end
click("Сдвинуть живые маркеры вверх")
assert(owner.state.badges.prefab and #Engine.find("Lab2Marker") == 3)
for _, id in Engine.find("Lab2Marker") do assert(math.abs(Engine.getPosition(id).y - 0.8) < 0.0001) end
local marker = setmetatable({ state = {}, entity = Engine.find("Lab2Marker")[1] }, { __index = require("scripts/Marker.luau") })
marker:Init()
marker:Update(0.5)
assert(entities[marker.entity].rotation.y > 0, "Marker Update did not call the rotation binding")
click("Показать 3D-сцену / вернуться кнопкой сверху")
assert(owner.state.preview)
click("< Вернуться к защите")
assert(not owner.state.preview and owner.state.page == 4)

go("reload")
click("Зафиксировать значение перед reload")
local rules = require("scripts/ReloadRules.luau")
rules.version = "B"
-- Explicit callback invocation, NOT a simulation of a native file watcher test.
owner:OnReload()
tick()
assert(owner.state.badges.reload and owner.state.counter == 2 and owner.state.initCount == 1)
assert(owner.state.reloadCount == 1 and status.lab2Defense.keys == 3)
owner.state.badges.reload = false
owner:ArmReload()
owner.state.counter += 1
rules.version = "A"
owner:OnReload()
assert(not owner.state.badges.reload, "Changed checkpoint was falsely accepted")
owner:ArmReload()
rules.version = "B"
owner:OnReload()
tick()
assert(owner.state.badges.reload)

for index, topic in Content.topics do
    click(topic.title)
    assert(owner.state.page == index)
end
assert(status.lab2Defense.page == 8 and status.lab2Defense.keys == 3)
click("Далее >")
assert(owner.state.page == 8)
click("Вернуться к началу")
click("< Назад")
assert(owner.state.page == 1)
owner:ClearMarkers()
assert(#Engine.find("Lab2Marker") == 0)
owner:Destroy()
assert(#UI.getRoots() == 0, "Defense root survived Destroy")
print("Lab2 Defense: real upstream React mount/events/hooks, navigation, quiz and callback logic passed. Native Engine API was a test double.")
