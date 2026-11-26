local React = require("React")
local UI = require("ReactTurbo")
local e = React.createElement
local rgb = Color3.fromRGB

local palette = {
    panel = rgb(11, 21, 33),
    surface = rgb(22, 39, 56),
    edge = rgb(45, 72, 89),
    ink = rgb(234, 243, 250),
    muted = rgb(153, 177, 196),
    cyan = rgb(100, 233, 231),
    amber = rgb(255, 184, 106),
    lilac = rgb(197, 171, 255),
    alarm = rgb(110, 54, 47),
}

local channels = {
    { name = "Питание", color = palette.amber },
    { name = "Синхронизация", color = palette.cyan },
    { name = "Телеметрия", color = palette.lilac },
}

local probes = {
    { id = "divide-by-zero", name = "Integer divide by zero", code = "0xC0000094", revision = 1 },
    { id = "access-violation", name = "Access violation", code = "0xC0000005", revision = 1 },
    { id = "assertion-failure", name = "Assertion failure", code = "0xC0000420", revision = 2 },
}

local slides = {
    {
        section = "СБОРКА И ВЫПУСК",
        title = "Из коммита в два ZIP",
        body = "GitHub Actions собирает Windows x64 с символами. Тег выпускает игровой ZIP и архив PDB из той же сборки.",
        detail = "В обоих архивах build.json хранит buildId и SHA. buildId связывает запуск с релизом, SHA указывает на исходный код.",
    },
    {
        section = "АВАРИЙНЫЙ ОТЧЁТ",
        title = "Что осталось после сбоя",
        body = "Передайте папку отчёта целиком. В ней crash.dmp, engine.log и report.json с buildId, reportId и кодом исключения.",
        detail = "Калибровка и запуск пробы отмечаются в журнале. Повторный сбой создаёт отдельный reportId.",
    },
    {
        section = "СИМВОЛИЗАЦИЯ",
        title = "Строка, на которой упали",
        body = "Найдите релиз по buildId и возьмите его app.exe и PDB. Откройте дамп, выберите контекст аварийного потока и посмотрите стек.",
        detail = "Покажите код исключения, функцию, файл и строку. Затем загрузите исходники по SHA и воспроизведите ту же пробу.",
    },
    {
        section = "ЖИВАЯ ПРОВЕРКА",
        title = "Три запуска",
        body = "1. Новый релиз: включите каналы и выйдите по Esc.\n\n2. Ранний релиз: включите каналы, вызовите сбой и разберите свежий дамп.\n\n3. Повторите пробу раннего релиза и сравните стеки и reportId.",
        detail = "У штатного выхода нет отчёта. У двух аварий совпадает место сбоя, а reportId различаются.",
    },
}

local Behavior = {}

local function corner()
    return e("UICorner", { CornerRadius = UDim.new(0, 6) })
end

local function text(value, order, size, color, height)
    return e("TextLabel", {
        Text = value,
        LayoutOrder = order,
        Size = UDim2.new(1, 0, 0, height or 0),
        AutomaticSize = "Y",
        BackgroundTransparency = 1,
        TextSize = size,
        TextColor3 = color,
        TextWrapped = true,
        TextXAlignment = "Left",
        TextYAlignment = "Top",
    })
end

local function button(value, callback, order, background, ink, height, position, width)
    return e("TextButton", {
        Text = value,
        Position = position,
        Size = width or UDim2.new(1, 0, 0, height),
        LayoutOrder = order,
        BackgroundColor3 = background,
        BorderSizePixel = 0,
        TextSize = 17,
        TextColor3 = ink,
        TextWrapped = true,
        Disabled = callback == nil,
        [React.Event.Activated] = callback,
    }, { Corner = corner() })
end

local function pair(order, first, second)
    local half = UDim2.new(0.5, -4, 0, 38)
    return e("Frame", { LayoutOrder = order, Size = UDim2.new(1, 0, 0, 38), BackgroundTransparency = 1 }, {
        First = button(first.label, first.click, 0, first.background, first.ink, 38, UDim2.fromOffset(0, 0), half),
        Second = button(second.label, second.click, 0, second.background, second.ink, 38, UDim2.new(0.5, 4, 0, 0), half),
    })
end

local function panelTabs(state)
    local function tab(label, slide)
        local selected = (state.slide ~= 0) == slide
        return {
            label = label,
            click = function()
                state.slide = if slide then math.max(1, state.slide) else 0
                state.armed = nil
                state.dirty = true
            end,
            background = if selected then palette.cyan else palette.surface,
            ink = if selected then palette.panel else palette.ink,
        }
    end
    return pair(5, tab("Станция", false), tab("Как устроен релиз", true))
end

local function drawSlide(children, state)
    local slide = slides[state.slide]
    children.Section = text(slide.section, 10, 14, palette.cyan)
    children.Title = text(slide.title, 11, 28, palette.ink)
    children.Body = text(slide.body, 12, 19, palette.ink)
    children.Detail = text(slide.detail, 13, 17, palette.muted)
    local function direction(label, step)
        local nextSlide = state.slide + step
        return {
            label = label,
            click = if slides[nextSlide] then function()
                state.slide = nextSlide
                state.dirty = true
            end else nil,
            background = palette.surface,
            ink = if slides[nextSlide] then palette.ink else palette.muted,
        }
    end
    children.Navigation = pair(14, direction("Назад", -1), direction("Далее", 1))
    children.Page = text(string.format("%d / %d", state.slide, #slides), 15, 15, palette.muted)
end

local function drawStation(children, owner, state)
    local count = state.count
    local segments = {}
    for index, channel in channels do
        segments[index] = e("Frame", {
            Position = UDim2.fromScale((index - 1) / #channels, 0),
            Size = UDim2.new(1 / #channels, -3, 1, 0),
            BackgroundColor3 = if index <= count then channel.color else palette.edge,
            BorderSizePixel = 0,
        })
    end
    children.Progress = text(string.format("КАЛИБРОВКА   %d / %d", count, #channels), 10, 16, palette.cyan)
    children.ProgressBar = e("Frame", {
        LayoutOrder = 11,
        Size = UDim2.new(1, 0, 0, 5),
        BackgroundColor3 = palette.surface,
        BorderSizePixel = 0,
    }, segments)
    for index, channel in channels do
        local done = index <= count
        local nextChannel = index == count + 1 and state.ready
        local name = string.format("%d   %s", index, channel.name)
        children["Channel" .. index] = button(if done then name .. "   ·   Готово" else name,
            if done or not state.ready then nil else function() owner:Calibrate(index) end,
            11 + index, if nextChannel then channel.color else palette.surface,
            if nextChannel then palette.panel elseif done then channel.color else palette.muted, 37)
    end
    children.Feedback = text(state.feedback, 15, 17, if count == #channels then palette.cyan else palette.muted, 26)
    if count < #channels then
        children.Hint = text("Мышь или клавиши 1–3. Esc закрывает игру.", 16, 15, palette.muted)
        return
    end
    children.Probes = text("АВАРИЙНЫЕ ПРОБЫ", 17, 15, palette.amber)
    if state.revision == 0 then
        children.Hint = text("Запустите игровой пакет «Маяк», чтобы включить нативные пробы.", 18, 17, palette.muted)
        return
    end
    for index, probe in probes do
        if probe.revision <= state.revision then
            children["Probe" .. index] = button(probe.name .. "  ·  " .. probe.code, function()
                state.armed = probe
                state.dirty = true
            end, 18 + index, palette.surface, if state.armed == probe then palette.amber else palette.ink, 36)
        end
    end
    if state.armed then
        local probe = state.armed
        children.Warning = text("Проба завершит игру и сохранит отчёт в папку краш-репортов.", 22, 16, palette.amber)
        children.Confirm = pair(23, {
            label = "Отмена",
            click = function()
                state.armed = nil
                state.dirty = true
            end,
            background = palette.surface,
            ink = palette.ink,
        }, {
            label = "Вызвать сбой",
            click = function() owner:RequestProbe(probe.id) end,
            background = palette.alarm,
            ink = palette.ink,
        })
    end
end

local function BeaconPanel(props)
    local owner = props.owner
    local state = owner.state
    local children = {
        Padding = e("UIPadding", {
            PaddingTop = UDim.new(0, 18), PaddingBottom = UDim.new(0, 18),
            PaddingLeft = UDim.new(0, 18), PaddingRight = UDim.new(0, 18),
        }),
        Order = e("UIListLayout", { Padding = UDim.new(0, 8), SortOrder = "LayoutOrder" }),
        Kicker = text("TURBOGAMEENGINE / ЛАБОРАТОРНАЯ 05", 1, 14, palette.cyan),
        Hero = text("МАЯК", 2, 49, palette.ink),
        Descriptor = text("Станция аварийной диагностики", 3, 19, palette.ink),
        Build = text(if state.revision == 0 then "Режим редактора" else
            string.format("Релиз %d   ·   buildId %s", state.revision, state.buildId), 4, 15, palette.muted),
        Tabs = panelTabs(state),
        Corner = corner(),
        Stroke = e("UIStroke", { Color = palette.edge, Thickness = 1 }),
        Width = e("UISizeConstraint", { MinSize = Vector2.new(284, 0), MaxSize = Vector2.new(440, 4000) }),
    }
    if state.slide == 0 then drawStation(children, owner, state) else drawSlide(children, state) end
    return e("ScreenGui", { Name = "BeaconHud" }, {
        Panel = e("ScrollingFrame", {
            Position = UDim2.fromOffset(16, 16),
            Size = UDim2.new(0.38, 0, 1, -32),
            BackgroundColor3 = palette.panel,
            BackgroundTransparency = 0.025,
            BorderSizePixel = 0,
            ScrollBarThickness = 5,
            ScrollBarImageColor3 = palette.cyan,
            AutomaticCanvasSize = "Y",
            CanvasSize = UDim2.fromOffset(0, 0),
            ScrollingDirection = "Y",
        }, children),
    })
end

local function findPart(tag)
    local id = Engine.find(tag)[1]
    if not id then return nil end
    local position = Engine.getPosition(id)
    if not position then return nil end
    return { id = id, origin = position, alignment = 0 }
end

function Behavior:Publish()
    local state = self.state
    state.dirty = true
    Engine.setStatus("lab5", {
        ready = state.ready,
        channels = state.count,
        lastAction = state.lastAction,
        crashRequest = state.crashRequest,
    })
end

function Behavior:Init()
    local state = self.state
    state.root = UI.createRoot({ Name = "BeaconStation" })
    state.count, state.time, state.slide, state.revision = 0, 0, 0, 0
    state.lastAction = "station-online"
    state.core = findPart("BeaconCore")
    state.orbs = {}
    state.ready = state.core ~= nil
    for index = 1, #channels do
        local orb = findPart("BeaconChannel" .. index)
        state.orbs[index] = orb
        state.ready = state.ready and orb ~= nil
    end
    state.feedback = if state.ready then "Подключите питание, синхронизацию и телеметрию." else "В сцене отсутствует ядро или приёмник маяка."
    self:Publish()
end

function Behavior:Calibrate(index)
    local state = self.state
    if not state.ready or state.crashRequest or type(index) ~= "number" or not channels[index] or index <= state.count then return end
    if index ~= state.count + 1 then
        state.feedback = "Следующий канал — " .. channels[state.count + 1].name .. "."
        state.dirty = true
        return
    end
    state.count = index
    state.lastAction = "calibrate-channel-" .. index
    state.feedback = if index == #channels then "Маяк синхронизирован. Станция готова к аварийной пробе."
        else "Сигнал принят. Следующий канал — " .. channels[index + 1].name .. "."
    self:Publish()
end

function Behavior:RequestProbe(id)
    local state = self.state
    if not state.ready or state.count ~= #channels or state.crashRequest then return end
    for _, probe in probes do
        if id == probe.id and state.revision >= probe.revision then
            state.lastAction = "probe-" .. id
            state.crashRequest = id
            self:Publish()
            return
        end
    end
end

function Behavior:OnEvent(event)
    if event.type ~= "Lab5Control" then return end
    local state = self.state
    if event.command == "configure" and type(event.buildId) == "string" and (event.demoRevision == 1 or event.demoRevision == 2) then
        state.revision = event.demoRevision
        state.buildId = string.sub(event.buildId, 1, 72)
        state.dirty = true
    elseif event.command == "calibrate" then
        self:Calibrate(event.channel)
    elseif event.command == "crash" then
        self:RequestProbe(event.scenario)
    end
end

function Behavior:Update(dt)
    local state = self.state
    if state.ready then
        if state.slide == 0 then
            for index = 1, #channels do
                if Input.wasKeyPressed(tostring(index)) then self:Calibrate(index) end
            end
        end
        state.time += dt
        local wave = state.time * (0.55 + 0.05 * state.count)
        local core = state.core
        local origin = core.origin + vector.create(0, math.sin(wave * 3) * 0.08, 0)
        Engine.setPosition(core.id, origin)
        Engine.setRotation(core.id, vector.create(0.38 + math.sin(wave) * 0.2, 0.42 + wave, 0.78))
        for index, orb in state.orbs do
            if index <= state.count then
                orb.alignment = math.min(orb.alignment + dt * 1.5, 1)
                local phase = wave + (index - 1) * math.pi * 2 / #channels
                local aim = origin + vector.create(1.42 * math.cos(phase), 1.42 * math.sin(phase), -0.72)
                local blend = orb.alignment * orb.alignment * (3 - 2 * orb.alignment)
                Engine.setPosition(orb.id, orb.origin + (aim - orb.origin) * blend)
            end
        end
    end
    if state.dirty then
        state.dirty = false
        state.root:render(e(BeaconPanel, { owner = self }))
    end
end

function Behavior:OnReload()
    self.state.armed = nil
    self.state.dirty = true
end

function Behavior:Destroy()
    if self.state.root then
        self.state.root:unmount()
        self.state.root = nil
    end
end

return Behavior
