-- All game rules, session state, movement, collection and victory live here.
-- Change fields or this code, then Reload or Stop / Play; no C++ build is needed.
local Behavior = {}

Behavior.fields = {
    duration = { type = "number", default = 45, min = 5, max = 300 },
    goal = { type = "number", default = 10, min = 1, max = 1000 },
    tokenCount = { type = "number", default = 12, min = 1, max = 100 },
    moveSpeed = { type = "number", default = 6, min = 0.1, max = 30 },
    pickupRadius = { type = "number", default = 1, min = 0.2, max = 5 },
    spawnRadius = { type = "number", default = 4, min = 1, max = 10 },
    pointsPerToken = { type = "number", default = 1, min = 1, max = 100 },
    title = { type = "string", default = "Collect the golden cubes" },
    allowRestart = { type = "boolean", default = true },
}

function Behavior:SpawnWave()
    for index = 1, math.floor(self.fields.tokenCount) do
        local angle = 2 * math.pi * index / self.fields.tokenCount
        local token = Engine.spawnPrefab("prefabs/collectible.json")
        Engine.setPosition(token, vector.create(
            math.cos(angle) * self.fields.spawnRadius,
            -1.2,
            8 + math.sin(angle) * self.fields.spawnRadius))
    end
    self.state.wave += 1
end

function Behavior:Restart()
    for _, token in Engine.find("ScriptToken") do
        Engine.destroy(token)
    end
    if self.state.player and Engine.isAlive(self.state.player) then
        Engine.destroy(self.state.player)
    end
    self.state.player = Engine.spawnPrefab("prefabs/collectathon_player.json")
    self.state.score = 0
    self.state.remaining = self.fields.duration
    self.state.wave = 0
    self.state.result = "Playing"
    self:SpawnWave()
    print(self.fields.title .. " — WASD / arrows to move, R to restart")
end

function Behavior:Init()
    self.state.collisionEvents = 0
    self:Restart()
end

function Behavior:Update(dt: number)
    if self.fields.allowRestart and Input.wasKeyPressed("R") then
        self:Restart()
    end
    if self.state.result == "Playing" and Engine.isAlive(self.state.player) then
        self.state.remaining = math.max(0, self.state.remaining - dt)
        local x = (if Input.isKeyDown("D") or Input.isKeyDown("Right") then 1 else 0)
            - (if Input.isKeyDown("A") or Input.isKeyDown("Left") then 1 else 0)
        local z = (if Input.isKeyDown("W") or Input.isKeyDown("Up") then 1 else 0)
            - (if Input.isKeyDown("S") or Input.isKeyDown("Down") then 1 else 0)
        local direction = vector.create(x, 0, z)
        if vector.magnitude(direction) > 0 then
            local position = Engine.getPosition(self.state.player)
                + vector.normalize(direction) * self.fields.moveSpeed * dt
            Engine.setPosition(self.state.player,
                vector.create(math.clamp(position.x, -11, 11), -1.2, math.clamp(position.z, 1, 18)))
        end
        local position = Engine.getPosition(self.state.player)
        for _, token in Engine.queryRadius(position, self.fields.pickupRadius, "ScriptToken") do
            Engine.destroy(token)
            self.state.score += self.fields.pointsPerToken
        end
        if self.state.score >= self.fields.goal then
            self.state.result = "Victory"
            print("Victory! Score: " .. self.state.score)
        elseif self.state.remaining <= 0 then
            self.state.result = "Time expired"
            print("Time expired. Press R to restart.")
        elseif #Engine.find("ScriptToken") == 0 then
            self:SpawnWave()
        end
    end
    Engine.setStatus("title", self.fields.title)
    Engine.setStatus("score", self.state.score)
    Engine.setStatus("goal", self.fields.goal)
    Engine.setStatus("remaining", self.state.remaining)
    Engine.setStatus("result", self.state.result)
    Engine.setStatus("wave", self.state.wave)
end

function Behavior:OnEvent(event)
    if event.type == "CollisionEnter" then
        self.state.collisionEvents += 1
    end
end

return Behavior
