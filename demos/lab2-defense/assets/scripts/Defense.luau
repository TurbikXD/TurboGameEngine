local React = require("React")
local UI = require("ReactTurbo")
local Content = require("scripts/DefenseContent.luau")
local Rules = require("scripts/ReloadRules.luau")
local e = React.createElement
local C = Color3.fromRGB
local palette = {
    background = C(13, 18, 31), panel = C(22, 30, 47), card = C(29, 40, 60),
    ink = C(237, 243, 250), muted = C(164, 181, 204), accent = C(82, 214, 196),
    blue = C(66, 105, 221), line = C(52, 69, 94), warning = C(255, 202, 115),
}
local Behavior = { fields = {} }

local function label(text, order, height, size, color)
    return e("TextLabel", {
        Name = "Text", Text = text, LayoutOrder = order,
        Size = UDim2.new(1, 0, 0, height), BackgroundTransparency = 1,
        TextSize = size or 18, TextColor3 = color or palette.ink,
        TextWrapped = true, TextXAlignment = "Left", TextYAlignment = "Top",
    })
end

local function button(text, callback, order, color)
    return e("TextButton", {
        Text = text, LayoutOrder = order, Size = UDim2.new(1, 0, 0, 42),
        BackgroundColor3 = color or palette.blue, BorderSizePixel = 0,
        TextSize = 17, TextColor3 = palette.ink, TextWrapped = true,
        [React.Event.Activated] = callback,
    }, { Corner = e("UICorner", { CornerRadius = UDim.new(0, 7) }) })
end

local function stack(children, height, order, background)
    children.Padding = e("UIPadding", {
        PaddingLeft = UDim.new(0, 16), PaddingRight = UDim.new(0, 16),
        PaddingTop = UDim.new(0, 16), PaddingBottom = UDim.new(0, 16),
    })
    children.Layout = e("UIListLayout", { Padding = UDim.new(0, 10), SortOrder = "LayoutOrder" })
    children.Corner = e("UICorner", { CornerRadius = UDim.new(0, 10) })
    return e("Frame", {
        LayoutOrder = order, Size = UDim2.new(1, 0, 0, height),
        BackgroundColor3 = background or palette.card, BorderSizePixel = 0,
    }, children)
end

local function badgeCount(state)
    local count = 0
    for _, key in { "boundary", "prefab", "reload" } do
        if state.badges[key] then count += 1 end
    end
    return count
end

local function visitCount(state)
    local count = 0
    for _, visited in state.visited do if visited then count += 1 end end
    return count
end

function Behavior:GoTo(index)
    self.state.page = math.clamp(index, 1, #Content.topics)
    self.state.visited[self.state.page] = true
    self.state.preview = false
end

function Behavior:Answer(index)
    local question = Content.quiz[self.state.quizIndex]
    if not question then return end
    if index == question.correct then
        self.state.quizIndex += 1
        self.state.feedback = "Верно. Решение принято по границе ответственности."
        if self.state.quizIndex > #Content.quiz then
            self.state.badges.boundary = true
            self.state.feedback = "Ключ архитектуры получен: все ответы верны."
        end
    else
        self.state.feedback = "Попробуйте ещё раз: C++ предоставляет механизмы, Luau задаёт правила."
    end
end

function Behavior:SpawnMarker()
    local markers = Engine.find("Lab2Marker")
    if #markers >= 3 then
        self.state.spawnMessage = "Уже есть 3 живых маркера. Теперь сдвиньте их."
        return
    end
    local id = Engine.spawnPrefab("prefabs/marker.json")
    Engine.setPosition(id, vector.create((#markers - 1) * 2.0, 0, 5))
    self.state.spawnMessage = "Создан ECS-дескриптор " .. tostring(id) .. ". Источник: prefabs/marker.json."
end

function Behavior:MoveMarkers()
    local markers = Engine.find("Lab2Marker")
    local moved = 0
    for _, id in markers do
        if Engine.isAlive(id) then
            local before = Engine.getPosition(id)
            Engine.setPosition(id, before + vector.create(0, 0.8, 0))
            local after = Engine.getPosition(id)
            if after and after.y > before.y then moved += 1 end
        end
    end
    self.state.spawnMessage = "Engine.getPosition подтвердил перемещение: " .. moved .. " сущн."
    if moved >= 3 then self.state.badges.prefab = true end
end

function Behavior:ClearMarkers()
    for _, id in Engine.find("Lab2Marker") do Engine.destroy(id) end
    self.state.spawnMessage = "Маркеры этой демонстрации отправлены на безопасное удаление."
end

function Behavior:ArmReload()
    if self.state.counter == 0 then
        self.state.reloadMessage = "Сначала увеличьте counter хотя бы один раз."
        return
    end
    self.state.checkpoint = self.state.counter
    self.state.checkpointVersion = Rules.version
    self.state.awaitingReload = true
    self.state.reloadMessage = "Зафиксировано " .. self.state.counter .. ". Измените version в ReloadRules.luau и Save без Stop."
end

local function quizCard(owner)
    local state = owner.state
    local question = Content.quiz[state.quizIndex]
    local children = {
        Title = label("ЗАДАНИЕ 1 / ключ архитектуры", 1, 22, 14, palette.accent),
    }
    if question then
        children.Question = label(string.format("%d / %d. %s", state.quizIndex, #Content.quiz, question.prompt), 2, 54, 19)
        for index, answer in question.answers do
            local answerIndex = index
            children["Answer" .. index] = button(answer, function() owner:Answer(answerIndex) end, 2 + index)
        end
    else
        children.Done = label("Архитектура: C++ ядро + Luau поведение + React UI.", 2, 60, 23, palette.accent)
        children.Replay = button("Ответить ещё раз", function()
            state.quizIndex = 1
            state.feedback = ""
        end, 3)
    end
    children.Feedback = label(state.feedback, 10, 48, 16, palette.warning)
    return stack(children, if question then 340 else 230, 20)
end

local function stateCard(owner, reload)
    local state = owner.state
    local children = {
        Title = label(if reload then "ЗАДАНИЕ 3 / ключ непрерывности" else "ЖИВОЕ СОСТОЯНИЕ ПОВЕДЕНИЯ", 1, 22, 14, palette.accent),
        Values = label(string.format("counter = %d    Init = %d    OnReload = %d\nПравила: %s · %s",
            state.counter, state.initCount, state.reloadCount, tostring(Rules.version), Rules.message), 2, 56, 19),
        Increment = button("self.state.counter + 1", function() state.counter += 1 end, 3),
    }
    if reload then
        children.Arm = button("Зафиксировать значение перед reload", function() owner:ArmReload() end, 4)
        children.Result = label(state.reloadMessage, 5, 74, 17, if state.badges.reload then palette.accent else palette.warning)
        children.Instructions = label("Script → Open → scripts/ReloadRules.luau → сменить A/B → Save. Затем вернитесь во Viewport.\nНе нажимайте Stop и не меняйте counter после фиксации.", 6, 74, 16, palette.muted)
    else
        children.Ticks = label(string.format("Реальные Update: %d · время симуляции: %.1f с", state.ticks, state.elapsed), 4, 30, 16, palette.muted)
    end
    return stack(children, if reload then 418 else 230, 20)
end

local function prefabCard(owner)
    local state = owner.state
    local markers = Engine.find("Lab2Marker")
    local rows = {}
    for _, id in markers do
        local position = Engine.getPosition(id)
        if Engine.isAlive(id) and position then
            table.insert(rows, string.format("id=%s  tag=%s  position=(%.1f, %.1f, %.1f)", tostring(id), Engine.getTag(id), position.x, position.y, position.z))
        end
    end
    return stack({
        Title = label("ЗАДАНИЕ 2 / ключ ECS", 1, 22, 14, palette.accent),
        Count = label("Живых экземпляров Lab2Marker: " .. #markers .. " / 3", 2, 30, 21),
        Spawn = button("Создать настоящий prefab", function() owner:SpawnMarker() end, 3),
        Move = button("Сдвинуть живые маркеры вверх", function() owner:MoveMarkers() end, 4),
        Preview = button("Показать 3D-сцену / вернуться кнопкой сверху", function() state.preview = true end, 5, palette.line),
        Readback = label(if #rows == 0 then "Пока нет экземпляров. Значения появятся после spawn." else table.concat(rows, "\n"), 6, 88, 15, palette.muted),
        Feedback = label(state.spawnMessage, 7, 52, 16, if state.badges.prefab then palette.accent else palette.warning),
        Clear = button("Очистить только Lab2Marker", function() owner:ClearMarkers() end, 8, palette.line),
    }, 490, 20)
end

local function DefenseUI(props)
    local owner = props.owner
    local state = owner.state
    local reactClicks, setReactClicks = React.useState(0)
    local showSources, setShowSources = React.useState(false)
    local topic = Content.topics[state.page]
    if state.preview then
        return e("ScreenGui", { Name = "Lab2Defense" }, {
            Return = e("Frame", {
                Position = UDim2.fromOffset(18, 18), Size = UDim2.fromOffset(390, 104),
                BackgroundColor3 = palette.panel, BorderSizePixel = 0,
            }, {
                Label = e("TextLabel", { Text = "Живая сцена: spawn + transform + Luau Update", Position = UDim2.fromOffset(12, 10), Size = UDim2.fromOffset(366, 28),
                    BackgroundTransparency = 1, TextColor3 = palette.ink, TextSize = 16, TextXAlignment = "Left" }),
                Button = e("TextButton", { Text = "< Вернуться к защите", Position = UDim2.fromOffset(12, 48), Size = UDim2.fromOffset(366, 42),
                    TextSize = 18, TextColor3 = palette.ink, BackgroundColor3 = palette.blue, BorderSizePixel = 0,
                    [React.Event.Activated] = function() state.preview = false end }),
            }),
        })
    end
    local sidebar = {
        Padding = e("UIPadding", { PaddingLeft = UDim.new(0, 14), PaddingRight = UDim.new(0, 14), PaddingTop = UDim.new(0, 18), PaddingBottom = UDim.new(0, 14) }),
        Layout = e("UIListLayout", { Padding = UDim.new(0, 8), SortOrder = "LayoutOrder" }),
        Title = label("КАРТА ЛАБОРАТОРИИ", 1, 24, 13, palette.accent),
    }
    for index, item in Content.topics do
        local page = index
        sidebar["Topic" .. index] = button(item.title, function() owner:GoTo(page) end, 1 + index,
            if page == state.page then palette.blue else palette.card)
    end
    sidebar.Hint = label("Кнопки доступны в любом порядке.\nКлавиши: Left / Right.\nПрогресс живёт до Stop.", 20, 86, 15, palette.muted)
    local content = {
        Padding = e("UIPadding", { PaddingLeft = UDim.new(0, 22), PaddingRight = UDim.new(0, 22), PaddingTop = UDim.new(0, 20), PaddingBottom = UDim.new(0, 20) }),
        Layout = e("UIListLayout", { Padding = UDim.new(0, 12), SortOrder = "LayoutOrder" }),
        Title = label(topic.title, 1, 40, 27),
        Summary = label(topic.summary, 2, 94, 20, palette.muted),
    }
    for index, text in topic.bullets do
        content["Bullet" .. index] = stack({ Text = label(string.format("%02d   %s", index, text), 1, 64, 17) }, 96, 2 + index, palette.panel)
    end
    if topic.station == "quiz" or topic.id == "boundary" then content.Task = quizCard(owner)
    elseif topic.station == "spawn" or topic.id == "prefab" then content.Task = prefabCard(owner)
    elseif topic.id == "lifecycle" then content.Task = stateCard(owner, false)
    elseif topic.station == "reload" or topic.id == "reload" then content.Task = stateCard(owner, true)
    elseif topic.id == "react" then
        content.Task = stack({
            Title = label("РЕАЛЬНЫЙ React.useState", 1, 22, 14, palette.accent),
            Counter = label("Локальное состояние UI: " .. reactClicks, 2, 40, 25),
            Button = button("React setState + 1", function() setReactClicks(function(previous) return previous + 1 end) end, 3),
            Note = label("Это hooks upstream React Luau. При remount компонента значение может сброситься.", 4, 54, 16, palette.muted),
        }, 220, 20)
    elseif topic.id == "verification" then
        content.Task = stack({
            Title = label("РЕЗУЛЬТАТ ЭТОЙ СЕССИИ", 1, 24, 14, palette.accent),
            Score = label(string.format("Ключей: %d / 3 · посещено тем: %d / %d", badgeCount(state), visitCount(state), #Content.topics), 2, 48, 25),
            Details = label((if state.badges.boundary then "[+]" else "[ ]") .. " Архитектура    " .. (if state.badges.prefab then "[+]" else "[ ]") .. " ECS    " .. (if state.badges.reload then "[+]" else "[ ]") .. " Reload", 3, 58, 19, palette.accent),
            Honest = label("Это результат заданий, а не отчёт автоматических тестов. Для защиты покажите исходники, Output и реальные действия.", 4, 64, 17, palette.muted),
            Back = button("Вернуться к началу", function() owner:GoTo(1) end, 5),
        }, 320, 20)
        local implemented = {}
        for _, item in Content.topics do
            if item.id ~= "mission" and item.id ~= "verification" then
                table.insert(implemented, item.title)
            end
        end
        content.Implemented = stack({
            Title = label("СДЕЛАНО / темы реализации", 1, 24, 14, palette.accent),
            List = label(table.concat(implemented, "\n"), 2, #implemented * 28, 18, palette.muted),
        }, 68 + #implemented * 28, 21)
        if Content.appendix and Content.appendix.limits then
            content.Limits = stack({
                Title = label("ГРАНИЦЫ УТВЕРЖДЕНИЙ", 1, 24, 14, palette.warning),
                List = label(table.concat(Content.appendix.limits, "\n\n"), 2, 220, 17, palette.muted),
            }, 290, 22)
        end
    end
    content.SourcesButton = button(if showSources then "Скрыть ссылки на исходники" else "Показать ссылки на исходники", function() setShowSources(not showSources) end, 30, palette.line)
    if showSources then
        local references = table.clone(topic.sources)
        local lines = #references
        if topic.notes and #topic.notes > 0 then
            table.insert(references, "\nЗаметки для защиты:")
            lines += 2
            for _, note in topic.notes do
                table.insert(references, note)
                lines += math.ceil(#note / 85) + 1
            end
        end
        if topic.id == "verification" and Content.appendix then
            for _, path in Content.appendix.sourceFiles or {} do table.insert(references, path); lines += 1 end
        end
        content.Sources = label(table.concat(references, "\n"), 31, math.max(110, lines * 22), 15, palette.muted)
    end
    return e("ScreenGui", { Name = "Lab2Defense" }, {
        Background = e("Frame", { Size = UDim2.fromScale(1, 1), BackgroundColor3 = palette.background, BorderSizePixel = 0 }, {
            Header = e("TextLabel", { Text = Content.title, Position = UDim2.fromOffset(24, 18), Size = UDim2.new(1, -380, 0, 42),
                BackgroundTransparency = 1, TextColor3 = palette.ink, TextSize = 32, TextXAlignment = "Left" }),
            Subtitle = e("TextLabel", { Text = Content.subtitle, Position = UDim2.fromOffset(26, 66), Size = UDim2.new(1, -330, 0, 24),
                BackgroundTransparency = 1, TextColor3 = palette.muted, TextSize = 16, TextXAlignment = "Left" }),
            Score = e("TextLabel", { Text = string.format("КЛЮЧИ  %d / 3\nТЕМЫ  %d / %d", badgeCount(state), visitCount(state), #Content.topics),
                Position = UDim2.new(1, -230, 0, 24), Size = UDim2.fromOffset(210, 60), BackgroundTransparency = 1,
                TextColor3 = palette.accent, TextSize = 18, TextXAlignment = "Right" }),
            Sidebar = e("ScrollingFrame", { Position = UDim2.fromOffset(16, 108), Size = UDim2.new(0, 230, 1, -124),
                BackgroundColor3 = palette.panel, BorderSizePixel = 0, ScrollBarThickness = 5,
                CanvasSize = UDim2.fromOffset(0, 0), AutomaticCanvasSize = "Y", ScrollingDirection = "Y" }, sidebar),
            Content = e("ScrollingFrame", { key = "Page" .. state.page, Position = UDim2.fromOffset(262, 108), Size = UDim2.new(1, -278, 1, -186),
                BackgroundColor3 = palette.card, BorderSizePixel = 0, ScrollBarThickness = 6,
                CanvasSize = UDim2.fromOffset(0, 0), AutomaticCanvasSize = "Y", ScrollingDirection = "Y" }, content),
            Previous = e("TextButton", { Text = "< Назад", Position = UDim2.new(0, 262, 1, -62), Size = UDim2.fromOffset(140, 42),
                BackgroundColor3 = palette.line, BorderSizePixel = 0, TextColor3 = palette.ink, TextSize = 18,
                [React.Event.Activated] = function() owner:GoTo(state.page - 1) end }),
            Page = e("TextLabel", { Text = string.format("%02d / %02d · %s", state.page, #Content.topics,
                    if state.badges.boundary and state.badges.prefab and state.badges.reload then "Все ключи собраны" else "Продолжайте исследование"),
                Position = UDim2.new(0, 418, 1, -62), Size = UDim2.new(1, -590, 0, 42),
                BackgroundTransparency = 1, TextColor3 = palette.muted, TextSize = 16 }),
            Next = e("TextButton", { Text = "Далее >", Position = UDim2.new(1, -156, 1, -62), Size = UDim2.fromOffset(140, 42),
                BackgroundColor3 = palette.blue, BorderSizePixel = 0, TextColor3 = palette.ink, TextSize = 18,
                [React.Event.Activated] = function() owner:GoTo(state.page + 1) end }),
        }),
    })
end

function Behavior:Init()
    if self.state.root then self.state.root:unmount() end
    self.state.page = 1
    self.state.visited = { [1] = true }
    self.state.badges = { boundary = false, prefab = false, reload = false }
    self.state.quizIndex = 1
    self.state.feedback = ""
    self.state.spawnMessage = "Создайте 3 маркера, затем проверьте запись и чтение transform."
    self.state.reloadMessage = "Ожидается контрольное значение и реальное сохранение файла."
    self.state.counter = 0
    self.state.initCount = (self.state.initCount or 0) + 1
    self.state.reloadCount = 0
    self.state.ticks = 0
    self.state.elapsed = 0
    self.state.preview = false
    self.state.root = UI.createRoot({ Name = "Lab2DefenseRoot" })
    print("ЛР2: Лаборатория скриптов. Откройте Viewport; Left/Right переключают темы.")
end

function Behavior:Update(dt)
    if not self.state.root then return end
    self.state.ticks += 1
    self.state.elapsed += dt
    if not self.state.preview then
        if Input.wasKeyPressed("Right") then self:GoTo(self.state.page + 1) end
        if Input.wasKeyPressed("Left") then self:GoTo(self.state.page - 1) end
    end
    Engine.setStatus("lab2Defense", {
        page = self.state.page, keys = badgeCount(self.state), counter = self.state.counter,
        initCount = self.state.initCount, reloadCount = self.state.reloadCount,
        version = Rules.version, markers = #Engine.find("Lab2Marker"),
    })
    self.state.root:render(e(DefenseUI, { owner = self }))
end

function Behavior:OnReload()
    self.state.reloadCount = (self.state.reloadCount or 0) + 1
    if self.state.awaitingReload and Rules.version ~= self.state.checkpointVersion then
        self.state.awaitingReload = false
        if self.state.counter == self.state.checkpoint and self.state.initCount == 1 then
            self.state.badges.reload = true
            self.state.reloadMessage = "Реальный OnReload: версия " .. tostring(Rules.version) .. ", counter=" .. self.state.counter .. ", Init=1. Ключ непрерывности получен."
        else
            self.state.reloadMessage = "OnReload произошёл, но контрольное значение изменилось. Зафиксируйте его и повторите A/B."
        end
    elseif not self.state.awaitingReload then
        self.state.reloadMessage = "Реальный OnReload вызван. Для задания сначала зафиксируйте counter, затем измените version A/B."
    end
    print("ЛР2 OnReload: version=" .. tostring(Rules.version) .. ", counter=" .. tostring(self.state.counter) .. ", Init=" .. tostring(self.state.initCount))
end

function Behavior:Destroy()
    if self.state.root then self.state.root:unmount(); self.state.root = nil end
end

return Behavior
