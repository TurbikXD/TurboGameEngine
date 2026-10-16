local Marker = {}

function Marker:Init()
    self.state.elapsed = 0
end

function Marker:Update(dt)
    self.state.elapsed += dt
    Engine.setRotation(self.entity, vector.create(0.15, self.state.elapsed * 0.65, 0))
end

return Marker
