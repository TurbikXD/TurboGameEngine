local Behavior = {
    fields = {
        speed = { type = "number", default = 0.45, min = 0, max = 5, step = 0.05, label = "Rotation Speed", category = "Movement", description = "Radians per second. Zero pauses rotation and bobbing." },
        bobbing = { type = "boolean", default = true, label = "Bobbing", category = "Movement", description = "Enable vertical motion for this preview." },
        height = { type = "number", default = 0.16, min = 0, max = 1, step = 0.01, label = "Amplitude", category = "Movement", description = "Vertical displacement from the original position." },
        caption = { type = "string", default = "Образец материала", label = "Caption", category = "Presentation", description = "Text above the 3D gallery; up to 8 live captions." },
    },
}

function Behavior:PublishCaption()
    local captions = Engine.getStatus("lab3SampleCaptions") or {}
    local current, count = {}, 0
    if type(captions) == "table" then
        for key, value in captions do
            local id = tonumber(key)
            if id and id >= 0 and id <= 4294967295 and id % 1 == 0 and Engine.isAlive(id) and type(value) == "string" and count < 8 then
                current[key] = value
                count += 1
            end
        end
    end
    local key = tostring(self.entity)
    if current[key] ~= nil or count < 8 then
        current[key] = if type(self.fields.caption) == "string" then self.fields.caption else "Образец материала"
        self.state.caption = self.fields.caption
    end
    Engine.setStatus("lab3SampleCaptions", current)
end

function Behavior:Init()
    self.state.origin = Engine.getPosition(self.entity)
    self.state.angle = 0
    self:PublishCaption()
end

function Behavior:Update(dt)
    if self.state.caption ~= self.fields.caption then self:PublishCaption() end
    self.state.angle = (self.state.angle or 0) + dt * self.fields.speed
    local angle = self.state.angle
    Engine.setRotation(self.entity, vector.create(0.12, angle, 0))
    local offset = if self.fields.bobbing then math.sin(angle * 1.8) * self.fields.height else 0
    Engine.setPosition(self.entity, self.state.origin + vector.create(0, offset, 0))
end

function Behavior:Destroy()
    local captions = Engine.getStatus("lab3SampleCaptions") or {}
    if type(captions) == "table" then
        captions[tostring(self.entity)] = nil
        Engine.setStatus("lab3SampleCaptions", captions)
    end
end

return Behavior
