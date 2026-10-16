local Collectible = {}
Collectible.fields = {
    bobSpeed = { type = "number", default = 2.5, min = 0, max = 10 },
    bobHeight = { type = "number", default = 0.12, min = 0, max = 0.5 },
}
function Collectible:Init()
    self.state.origin = Engine.getPosition(self.entity)
    self.state.age = 0
end
function Collectible:Update(dt: number)
    self.state.age += dt
    local origin = self.state.origin
    Engine.setPosition(self.entity, origin + vector.create(0,
        math.sin(self.state.age * self.fields.bobSpeed) * self.fields.bobHeight, 0))
end
return Collectible
