local React = require("React")
local UI = require("ReactTurbo")
local e = React.createElement
local C = Color3.fromRGB

local colors = {
    panel = C(17, 25, 39), card = C(28, 40, 59), ink = C(239, 245, 252),
    muted = C(166, 184, 205), mint = C(69, 218, 183), amber = C(250, 184, 71),
    violet = C(173, 134, 248), line = C(48, 67, 91), red = C(255, 130, 130),
}
local samples = {
    { name = "Нефрит", path = "prefabs/jade.json", color = colors.mint },
    { name = "Янтарь", path = "prefabs/amber.json", color = colors.amber },
    { name = "Аметист", path = "prefabs/violet.json", color = colors.violet },
}
local pages = {
    {
        title = "01 / Собери спектр",
        body = "Выберите материал, указанный в заказе. Каждый верный ответ создаёт настоящий ECS-объект из prefab. Соберите последовательность, затем осмотрите 3D-витрину.",
    },
    {
        title = "02 / Докинг графа",
        body = "В редакторе остановите Play. Выберите LayeredMaterialPreview в Explorer, затем Material → Open Material Graph в Properties. Перетащите разделитель между графом и Viewport. Нажмите Float, затем Dock beside Viewport.",
        check = "Оставьте граф закреплённым. При узкой рабочей области он открывается под Viewport. Колесо меняет масштаб, средняя кнопка сдвигает холст.",
    },
    {
        title = "03 / Живой предпросмотр",
        body = "В Layered Tile соедините выход Scalar со входом Alpha узла Lerp. В свойствах Scalar задайте 0.10, затем 0.85 двумя отдельными правками. Для точного ввода используйте Ctrl+клик по Value и Enter.",
        check = "LayeredMaterialPreview меняется сразу. Соседняя сфера LayeredMaterialReference показывает сохранённый вид того же материала. Оставьте правки в черновике.",
    },
    {
        title = "04 / Черновик и Undo/Redo",
        body = "Нажмите Undo в графе.\nТекущее значение 0.10, в Redo осталось 0.85.\nЗакройте редактор с открытым закреплённым графом и снова откройте тот же проект. Отложите Save Material до конца показа.",
        check = "Граф, выбранная сфера, холст и черновик восстановятся. После открытия Redo даст 0.85, Undo вернёт 0.10. Для следующего показа верните сохранённый вид через Discard / Reload в копии проекта.",
    },
    {
        title = "05 / Материалы и кэш",
        body = "Нефрит, янтарь и аметист переопределяют base.json. Цепочка base → stage → pedestal показывает три уровня наследования. В Properties можно создать свой инстанс. Renderer переиспользует PSO для одинаковых шейдера, blend и depth при разных цветах.",
    },
    {
        title = "06 / Свойства и проверка",
        body = "В Edit у MaterialLabController можно изменить длину заказа, скорость и заголовок. У JadePreview доступны движение и подпись. Save Scene сохраняет поля, а Save New Prefab экспортирует их в шаблон. Результаты тестов и замеры есть в docs/lab3/verification.md.",
    },
}

local Behavior = {
    fields = {
        title = { type = "string", default = "СПЕКТР / Лаборатория материалов", label = "Title", category = "Presentation", description = "Title of the game panel." },
        rounds = { type = "integer", default = 6, min = 3, max = 12, step = 1, label = "Order Length", category = "Gameplay", description = "Samples per order. Applies on the next Play or when starting a new order." },
        spinSpeed = { type = "number", default = 0.7, min = 0, max = 4, step = 0.05, label = "Rotation Speed", category = "Gallery", description = "Radians per second for player-created samples." },
        animate = { type = "boolean", default = true, label = "Animate Samples", category = "Gallery", description = "Rotate and bob player-created samples." },
    },
}

local function text(value, order, height, size, color)
    return e("TextLabel", {
        Text = value, LayoutOrder = order, Size = UDim2.new(1, 0, 0, height),
        BackgroundTransparency = 1, TextColor3 = color or colors.ink,
        TextSize = size or 17, TextWrapped = true, AutomaticSize = "Y", TextXAlignment = "Left", TextYAlignment = "Top",
    })
end

local function button(value, callback, order, color, height)
    return e("TextButton", {
        Text = value, LayoutOrder = order, Size = UDim2.new(1, 0, 0, height or 38),
        BackgroundColor3 = color or colors.line, BorderSizePixel = 0,
        TextColor3 = colors.ink, TextSize = 16, TextWrapped = true,
        [React.Event.Activated] = callback,
    }, { Corner = e("UICorner", { CornerRadius = UDim.new(0, 6) }) })
end

function Behavior:ClearSamples()
    for _, sample in self.state.spawned do
        if Engine.isAlive(sample.id) then Engine.destroy(sample.id) end
    end
    self.state.spawned = {}
end

function Behavior:NewOrder()
    self:ClearSamples()
    self.state.goal = math.clamp(math.floor(self.fields.rounds), 3, 12)
    self.state.progress = 0
    self.state.mistakes = 0
    self.state.feedback = "Заказ принят. Выберите первый материал."
    self.state.finished = false
end

function Behavior:Choose(index)
    if self.state.finished then
        self.state.feedback = "Заказ уже выполнен. Нажмите «Новый заказ»."
        return
    end
    local expected = (self.state.progress * 2) % #samples + 1
    if index ~= expected then
        self.state.mistakes += 1
        self.state.feedback = "Этот оттенок не подходит. Нужен " .. samples[expected].name .. "."
        return
    end
    local definition = samples[index]
    local ok, id = pcall(Engine.spawnPrefab, definition.path)
    if not ok or not id or not Engine.isAlive(id) then
        self.state.feedback = "Не удалось создать prefab. Подробности — в Script Output."
        if not ok then print("Ошибка создания образца: " .. tostring(id)) end
        return
    end
    local slot = self.state.progress
    local position = vector.create((slot % 6 - 2.5) * 1.35, -0.2, 2.1 + math.floor(slot / 6) * 2.3)
    Engine.setPosition(id, position)
    table.insert(self.state.spawned, { id = id, origin = position, phase = slot * 0.5 })
    self.state.progress += 1
    self.state.finished = self.state.progress >= self.state.goal
    self.state.feedback = if self.state.finished
        then "Спектр собран! Все образцы — настоящие ECS-объекты."
        else "Создан образец: " .. definition.name .. ". Продолжайте заказ."
end

local function galleryCaption()
    local entries = {}
    local captions = Engine.getStatus("lab3SampleCaptions")
    if type(captions) == "table" then
        for id, caption in captions do
            if type(caption) == "string" and #entries < 8 then
                table.insert(entries, { id = tonumber(id) or 0, caption = caption })
            end
        end
    end
    table.sort(entries, function(a, b) return a.id < b.id end)
    local lines = {}
    for _, entry in entries do table.insert(lines, entry.caption) end
    if #lines == 0 then
        lines = { "Нефрит / непрозрачность", "Янтарь / прозрачность", "Аметист / свечение" }
    end
    return "БАЗА + ЭКЗЕМПЛЯРЫ\n" .. table.concat(lines, "\n") .. "\n\nЦвет, UV, смешивание и глубина", 96 + #lines * 30
end

local function LabUI(props)
    local owner, state = props.owner, props.owner.state
    local nextSample = samples[(state.progress * 2) % #samples + 1]
    local caption, captionHeight = galleryCaption()
    local children = {
        Padding = e("UIPadding", { PaddingLeft = UDim.new(0, 16), PaddingRight = UDim.new(0, 16), PaddingTop = UDim.new(0, 16), PaddingBottom = UDim.new(0, 16) }),
        Layout = e("UIListLayout", { Padding = UDim.new(0, 9), SortOrder = "LayoutOrder" }),
        Kicker = text("TURBOGAMEENGINE   /   ЛР 3 · МАТЕРИАЛЫ", 1, 22, 13, colors.mint),
        Title = text(owner.fields.title, 2, 68, 25),
        Progress = text(string.format("ЗАКАЗ  %02d / %02d     ОШИБКИ  %d", state.progress, state.goal, state.mistakes), 3, 26, 17, colors.amber),
        Gallery = button(if state.compact then "Открыть лабораторию" else "Только 3D-витрина", function() state.compact = not state.compact end, 4),
    }
    if not state.compact then
        local page = pages[state.page]
        children.Defense = if state.page == 1
            then button("Защита Material Graph", function() state.page = 2 end, 5, colors.card)
            else button("К игровому заказу", function() state.page = 1 end, 5, colors.card)
        children.Topic = text(page.title, 6, 26, 20)
        children.Description = text(page.body, 7, 0, 17, colors.muted)
        if page.check then children.Check = text(page.check, 8, 0, 16, colors.mint) end
        children.Previous = button("< Предыдущая тема", function() state.page = math.max(1, state.page - 1) end, 9, colors.card, 30)
        children.Next = button("Следующая тема >", function() state.page = math.min(#pages, state.page + 1) end, 10, colors.card, 30)
        if state.page == 1 then
            children.Target = text(if state.finished then "ЗАКАЗ ВЫПОЛНЕН" else "СЕЙЧАС НУЖЕН: " .. nextSample.name, 11, 34, 21, if state.finished then colors.mint else nextSample.color)
            for index, definition in samples do
                local choice = index
                children["Choice" .. index] = button("Создать: " .. definition.name, function() owner:Choose(choice) end, 11 + index, colors.card)
            end
            children.Feedback = text(state.feedback, 15, 65, 16, if state.finished then colors.mint else colors.amber)
            children.Reset = button("Новый заказ", function() owner:NewOrder() end, 16)
            children.Fields = text(string.format("Параметры: заказ=%d | скорость=%.2f\nАнимация: %s | создано образцов: %d\nStop восстанавливает исходную сцену.", owner.fields.rounds, owner.fields.spinSpeed, if owner.fields.animate then "вкл." else "выкл.", #state.spawned), 17, 76, 14, colors.muted)
        end
    end
    return e("ScreenGui", { Name = "MaterialLab" }, {
        Sidebar = e("ScrollingFrame", {
            Position = UDim2.fromOffset(18, 18), Size = if state.compact then UDim2.fromOffset(370, 232) else UDim2.new(0, 370, 1, -36),
            BackgroundColor3 = colors.panel, BorderSizePixel = 0, BackgroundTransparency = 0.035,
            ScrollBarThickness = 4, AutomaticCanvasSize = "Y", CanvasSize = UDim2.fromOffset(0, 0), ScrollingDirection = "Y",
        }, children),
        Caption = e("TextLabel", {
            Text = caption,
            Position = UDim2.new(1, -330, 0, 24), Size = UDim2.fromOffset(306, captionHeight),
            BackgroundTransparency = 1, TextColor3 = colors.ink, TextSize = 18,
            TextXAlignment = "Right", TextYAlignment = "Top", TextWrapped = true,
        }),
    })
end

function Behavior:Init()
    if self.state.root then self.state.root:unmount() end
    self.state.spawned = {}
    self.state.page = 1
    self.state.compact = false
    self.state.angle = 0
    self.state.root = UI.createRoot({ Name = "MaterialLabRoot" })
    self:NewOrder()
    print("ЛР 3: защита Material Graph и игровой заказ доступны в HUD «Спектр».")
end

function Behavior:Update(dt)
    if not self.state.root then return end
    if self.fields.animate then
        self.state.angle = (self.state.angle or 0) + dt * self.fields.spinSpeed
        for _, sample in self.state.spawned do
            if Engine.isAlive(sample.id) then
                local angle = self.state.angle + sample.phase
                Engine.setRotation(sample.id, vector.create(0.15, angle, 0))
                Engine.setPosition(sample.id, sample.origin + vector.create(0, math.sin(angle) * 0.09, 0))
            end
        end
    end
    Engine.setStatus("lab3Materials", {
        progress = self.state.progress, goal = self.state.goal, mistakes = self.state.mistakes,
        finished = self.state.finished, page = self.state.page, spawned = #self.state.spawned,
        spinSpeed = self.fields.spinSpeed, animate = self.fields.animate,
    })
    self.state.root:render(e(LabUI, { owner = self }))
end

function Behavior:Destroy()
    if self.state.root then self.state.root:unmount(); self.state.root = nil end
    self:ClearSamples()
end

return Behavior
