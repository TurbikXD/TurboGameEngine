local Collectible = {}
Collectible.fields = {
    bobSpeed = { type = "number", default = 2.5, min = 0, max = 10, step = 0.05, label = "Bobbing Speed", category = "Motion", description = "Live bobbing phase speed in radians per second; speed changes preserve the current phase." },
    bobHeight = { type = "number", default = 0.12, min = 0, max = 0.5, step = 0.01, label = "Bobbing Height", category = "Motion", description = "Live vertical amplitude around the token's spawn position." },
    bobbing = { type = "boolean", default = true, label = "Bobbing Enabled", category = "Motion", description = "Live toggle; disabled tokens return to their spawn height." },
    spinSpeed = { type = "number", default = 0, min = -10, max = 10, step = 0.1, label = "Rotation Speed", category = "Motion", description = "Live Y-axis rotation in radians per second; zero stops rotation and preserves its current angle." },
}
function Collectible:Init()
    self.state.origin = Engine.getPosition(self.entity)
    self.state.age = 0
    self.state.bobPhase = 0
    self.state.spinPhase = 0
    self.state.baseRotation = Engine.getRotation(self.entity)
end
function Collectible:Update(dt: number)
    self.state.age += dt
    self.state.bobPhase = (self.state.bobPhase or ((self.state.age - dt) * self.fields.bobSpeed)) + dt * self.fields.bobSpeed
    self.state.spinPhase = (self.state.spinPhase or 0) + dt * self.fields.spinSpeed
    local origin = self.state.origin
    Engine.setPosition(self.entity, origin + vector.create(0,
        if self.fields.bobbing then math.sin(self.state.bobPhase) * self.fields.bobHeight else 0, 0))
    if self.fields.spinSpeed ~= 0 or self.state.spinPhase ~= 0 then
        self.state.baseRotation = self.state.baseRotation or Engine.getRotation(self.entity)
        Engine.setRotation(self.entity, self.state.baseRotation + vector.create(0, self.state.spinPhase, 0))
    end
end
return Collectible
