#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <lua.h>

#include "engine/core/JobSystem.h"
#include "engine/ecs/components.h"
#include "engine/ecs/world.h"
#include "engine/platform/Input.h"
#include "engine/scripting/react/ReactRuntime.h"
#include "engine/scripting/ScriptingSystem.h"

namespace {
using engine::scripting::ScriptComponent;
using engine::scripting::ScriptingSystem;
using Json = nlohmann::json;

void require(const bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary); file << text;
    if (!file) { throw std::runtime_error("Cannot write test fixture"); }
}
std::string behavior(const int factor) {
    return "local Behavior = {fields = {speed = {type='number',default=2,min=1,max=10}}}\n"
        "function Behavior:Init() self.state.count=0 end\n"
        "function Behavior:Update(dt) self.state.count+=1; Engine.setPosition(self.entity,vector.create(self.state.count*" +
        std::to_string(factor) + ",self.fields.speed,dt)) end\n"
        "function Behavior:OnEvent(event) self.state.event=event.type end\nreturn Behavior";
}

void runtimeAndReload() {
    const auto root = std::filesystem::temp_directory_path() / ("tge-scripting-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    write(root / "behavior.luau", behavior(1));
    write(root / "prefab.json", R"({"name":"Fixture","components":{"transform":{"position":[3,4,5]},"script":{"path":"assets/behavior.luau","fields":{"speed":7}}}})");
    write(root / "bad.json", R"({"components":{"script":{"enabled":true}}})");

    engine::core::JobSystem jobs(true, 2U);
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, &jobs, root), "Luau initialization failed");
    engine::platform::InputManager::reset();
    engine::platform::InputManager::beginFrame();
    engine::platform::Event keyEvent{};
    keyEvent.type = engine::platform::EventType::KeyPressed; keyEvent.key = engine::platform::KeyCode::W;
    engine::platform::InputManager::onEvent(keyEvent);
    engine::platform::InputManager::setCaptureState(true, true);
    require(scripts.execute("assert(Input.isKeyDown('W') and Input.wasKeyPressed('W'))"), "Focused viewport input was swallowed by ImGui capture");
    scripts.setInputEnabled(false);
    require(scripts.execute("assert(not Input.isKeyDown('W') and not Input.wasKeyPressed('W'))"), "Inactive gameplay input was not gated");
    scripts.setInputEnabled(true); engine::platform::InputManager::reset();
    const auto entity = scripts.spawnPrefab("prefab.json");
    require(world.isAlive(entity), "Prefab did not create an entity");
    require(world.getComponent<ScriptComponent>(entity)->fields["speed"] == 7.0, "Prefab script fields were not retained");
    require(world.getComponent<ScriptComponent>(entity)->path == "behavior.luau", "Behavior asset prefix was not normalized");
    const auto count = world.aliveCount();
    require(scripts.spawnPrefab("bad.json") == engine::ecs::kInvalidEntity && world.aliveCount() == count,
        "Malformed prefab created a partial entity");
    require(scripts.spawnPrefab("../outside.json") == engine::ecs::kInvalidEntity, "Asset path escape accepted");

    scripts.start(); scripts.update(0.1);
    require(world.getComponent<engine::ecs::Transform>(entity)->position.x == 1.0F, "Update binding failed");
    require(world.getComponent<engine::ecs::Transform>(entity)->position.y == 7.0F, "Inspector override did not reach behavior");
    scripts.queueEvent("CollisionEnter", Json{{"entityA", entity}}); scripts.dispatchEvents();
    require(scripts.inspect(entity)["state"]["event"] == "CollisionEnter", "Event callback failed");
    write(root / "behavior.luau", behavior(10));
    require(scripts.requestReload("behavior.luau"), "Async reload not submitted");
    jobs.waitAll(); scripts.pollReload(); scripts.update(0.1);
    require(world.getComponent<engine::ecs::Transform>(entity)->position.x == 20.0F, "Reload did not preserve runtime state");
    write(root / "behavior.luau", "local invalid =");
    require(scripts.requestReload("behavior.luau"), "Broken reload not submitted");
    jobs.waitAll(); scripts.pollReload(); scripts.update(0.1);
    require(world.getComponent<engine::ecs::Transform>(entity)->position.x == 30.0F, "Broken reload lost last-good behavior");
    require(!scripts.output().empty(), "Script diagnostics missing");
    write(root / "behavior.luau", behavior(20)); scripts.stop(); scripts.start(); scripts.update(0.1);
    require(world.getComponent<engine::ecs::Transform>(entity)->position.x == 20.0F, "Next Play did not read current disk code");

    const auto before = scripts.memoryUsed();
    for (int index = 0; index < 12; ++index) {
        require(scripts.requestReload("behavior.luau"), "Repeated reload failed"); jobs.waitAll(); scripts.pollReload();
    }
    require(scripts.memoryUsed() < before + 2U * 1024U * 1024U, "Repeated reload leaked excessive VM memory");
    require(!scripts.execute("while true do end", "loop.luau"), "Infinite loop escaped execution budget");
    require(scripts.execute("print('VM alive after timeout')"), "VM did not recover after timeout");
    require(!scripts.execute("local huge=string.rep('x',100000000)", "memory.luau"), "Memory budget was not enforced");
    require(scripts.memoryUsed() <= scripts.limits().memoryBytes, "VM exceeded memory budget");
    require(scripts.execute("print('VM alive after memory error')"), "VM did not recover after memory error");
    require(!scripts.execute("error(setmetatable({message='structured UI failure'}, {__tostring=function() error('must not run') end}))"), "Structured error was accepted");
    require(scripts.output().back().text.find("structured UI failure") != std::string::npos,
        "Structured React-style error lost its diagnostic message");
    require(scripts.execute("assert(true)"), "Structured error corrupted VM state");
    require(!scripts.analyze("local broken =", "syntax.luau"), "Syntax check accepted invalid source");
    require(scripts.analyze("local x: number = 2", "typed.luau"), "Luau typed syntax rejected");
    require(!scripts.execute("require('../escape.luau')"), "Require escaped asset root");
    scripts.registerModule("Scoped", "escapedGlobal = 1; error('expected scoped module error')");
    require(!scripts.execute("require('Scoped')"), "Invalid module unexpectedly loaded");
    require(scripts.execute("assert(escapedGlobal == nil)"), "Failed module polluted shared globals");

    scripts.registerModule("MalformedFields", R"(
        return {fields={
            valid={type='number',default=1},
            speed={type='number',default=1,min='oops'},
            badType={type=7,default=1},
            badDefault={type='number',default='oops'},
            badRange={type='number',default=1,min=5,max=0},
            badBoolean={type='boolean',default=3},
            missingDefault={type='string'},
            badInteger={type='integer',default=1.2},
        }}
    )");
    const auto safeSchema = scripts.fields("MalformedFields");
    require(safeSchema.size() == 1U && safeSchema.front().name == "valid", "Malformed field metadata reached the Inspector");
    bool fieldDiagnostic = false;
    for (const auto& message : scripts.output()) {
        if (message.kind == engine::scripting::ScriptMessage::Kind::Error && message.text.find("MalformedFields: fields.speed: min") != std::string::npos) {
            fieldDiagnostic = true;
        }
    }
    require(fieldDiagnostic, "Malformed field declaration lacks a useful Output diagnostic");
    const auto diagnosticCount = scripts.output().size();
    require(scripts.fields("MalformedFields").size() == 1U && scripts.output().size() == diagnosticCount,
        "Repeated Inspector reads repeated field errors or corrupted VM state");
    ScriptComponent malformedFields{"MalformedFields", true, Json::object()};
    scripts.populateDefaults(malformedFields);
    require(malformedFields.fields.contains("valid") && !malformedFields.fields.contains("speed"), "Malformed field defaults were applied");
    scripts.registerModule("MalformedFields", "return {fields={speed={type='number',default=1,min=0,max=100}}}");
    require(scripts.fields("MalformedFields").size() == 1U, "Field schema did not recover after corrected module registration");
    require(scripts.execute("print('VM alive after malformed fields')"), "Malformed fields corrupted the VM stack");

    scripts.registerModule("FieldAliases", "return {fields={count={type='integer',default=1,min=0,max=100},active={type='bool',default=false}}}");
    ScriptComponent savedFields{"FieldAliases", true, Json{{"count", 42}, {"active", true}}};
    scripts.populateDefaults(savedFields);
    require(savedFields.fields["count"] == 42.0 && savedFields.fields["active"] == true, "Saved integer/bool overrides were reset to defaults");
    const auto aliasSchema = scripts.fields("FieldAliases");
    for (const auto& field : aliasSchema) {
        if (field.name == "count") { require(field.type == "number" && field.integer, "Integer schema was not canonicalized"); }
        if (field.name == "active") { require(field.type == "boolean", "Bool schema was not canonicalized"); }
    }
    savedFields.fields["count"] = 42.6;
    scripts.populateDefaults(savedFields);
    require(savedFields.fields["count"] == 43.0, "Integer field accepted a fractional saved value");

    const std::string handle = std::to_string(entity);
    require(scripts.execute("Engine.destroy(" + handle + ")"), "Script destruction failed");
    const auto replacement = world.createEntity();
    require(replacement != entity && !world.isAlive(entity), "Entity generation was not invalidated");
    require(scripts.execute("assert(not Engine.isAlive(" + handle + ")); assert(Engine.getPosition(" + handle + ")==nil)"), "Stale Lua handle resolved to replacement");
    scripts.registerModule("Cleanup", R"(
        return {
            Init=function(self) self.state.ready=true end,
            Destroy=function(self)
                Engine.setStatus('cleanup',(Engine.getStatus('cleanup') or 0)+1)
                local ok=pcall(Engine.spawnPrefab,'prefab.json')
                assert(not ok,'Destroy must not mutate ECS')
            end,
        }
    )");
    const auto cleanup = world.createEntity();
    world.addComponent<ScriptComponent>(cleanup, ScriptComponent{"Cleanup", true, Json::object()});
    scripts.update(0.01);
    const auto beforeCleanup = world.aliveCount();
    scripts.forgetEntity(cleanup); scripts.forgetEntity(cleanup);
    require(scripts.status().value("cleanup", 0) == 1, "Forget cleanup did not run exactly once");
    world.removeComponent<ScriptComponent>(cleanup);
    const auto otherCleanup = world.createEntity();
    world.addComponent<ScriptComponent>(otherCleanup, ScriptComponent{"Cleanup", true, Json::object()});
    scripts.update(0.01); scripts.stop(); scripts.stop();
    require(scripts.status().value("cleanup", 0) == 2, "Stop cleanup did not run exactly once");
    require(world.aliveCount() == beforeCleanup + 1U, "Destroy cleanup mutated ECS");
    scripts.shutdown(); require(scripts.state() == nullptr && scripts.memoryUsed() == 0U, "Luau shutdown leaked allocations");
}

void gameplayDemoAndReact() {
    engine::platform::InputManager::reset();
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr, ENGINE_TEST_ASSET_ROOT), "Demo runtime initialization failed");
    const auto controller = scripts.spawnPrefab("prefabs/collectathon_controller.json");
    require(world.isAlive(controller), "Collectathon controller missing");
    scripts.start(); scripts.update(0.016);
    if (world.aliveCount() != 14U || scripts.status().value("goal", 0) != 10) {
        std::cerr << "Demo entities: " << world.aliveCount() << '\n';
        for (const auto& message : scripts.output()) { std::cerr << message.text << '\n'; }
        std::cerr << scripts.inspect(controller).dump() << '\n';
    }
    require(world.aliveCount() == 14U, "Lua mechanic did not spawn player and prefab wave");
    require(scripts.status().value("goal", 0) == 10, "Lua mechanic status missing");
    require(scripts.execute("local player=Engine.find('ScriptPlayer')[1]; local token=Engine.find('ScriptToken')[1]; Engine.setPosition(player,Engine.getPosition(token))"), "Cannot position demo player");
    scripts.update(0.016);
    require(scripts.status().value("score", 0) >= 1, "Script pickup rule did not award score");
    require(engine::scripting::ReactRuntime::install(scripts), "Actual React Luau renderer bootstrap failed");
    const bool rendered = scripts.execute(R"(
        local React=require('React')
        local ReactTurbo=require('ReactTurbo')
        local function Counter()
            local value, setValue=React.useState(3)
            return React.createElement('TextButton', {Name='CounterTest', Text=tostring(value), [React.Event.Activated]=function() setValue(value+1) end})
        end
        local root=ReactTurbo.createRoot()
        root:render(React.createElement(Counter))
        local button=root.container.children[1]
        ReactTurbo.dispatch(button, 'Activated')
        assert(button.props.Text=='4', 'A real UI event did not update React state')
    )", "react-test.luau");
    if (!rendered) { for (const auto& message : scripts.output()) { std::cerr << message.text << '\n'; } }
    require(rendered, "Actual React hooks or retained renderer failed");
    engine::scripting::ReactRuntime::pump(scripts, 0.016);
    require(scripts.execute("assert(#__turboUIRoots >= 1)"), "React did not commit a retained UI root");
    scripts.stop(); scripts.shutdown();
}

void updateDestructionKeepsPrefabRespawnsBounded() {
    engine::platform::InputManager::reset();
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr, ENGINE_TEST_ASSET_ROOT), "Respawn runtime initialization failed");
    require(engine::scripting::ReactRuntime::install(scripts), "Respawn React bootstrap failed");
    scripts.registerModule("RespawnDriver", R"(
        local B = {}
        function B:Spawn()
            Engine.spawnPrefab('prefabs/collectathon_controller.json')
            Engine.spawnPrefab('prefabs/react_ui.json')
        end
        function B:Init()
            self.state.tick = 0
            self:Spawn()
        end
        function B:Update(_dt)
            self.state.tick += 1
            if self.state.tick % 3 ~= 0 then return end
            Engine.setStatus('destroyedHud', Engine.find('ReactLuauHUD')[1])
            for _, tag in {'ScriptToken', 'ScriptPlayer', 'Collectathon Controller', 'ReactLuauHUD'} do
                for _, id in Engine.find(tag) do Engine.destroy(id) end
            end
            self:Spawn()
        end
        return B
    )");
    // The driver runs before its scripted victims. Destruction from Update must
    // survive their later behavior cleanup, including the real React HUD root.
    const auto driver = world.createEntity();
    world.addComponent<ScriptComponent>(driver, ScriptComponent{"RespawnDriver", true, Json::object()});
    scripts.start();
    for (int frame = 1; frame <= 36; ++frame) {
        scripts.update(0.01);
        require(engine::scripting::ReactRuntime::pump(scripts, 0.01), "Respawn React pump failed");
        const bool resetFrame = frame % 3 == 0;
        require(world.aliveCount() == (resetFrame ? 3U : 16U),
            "Destroy from Update left scripted prefab entities alive or duplicated their Init spawns");
        require(scripts.execute(resetFrame ? "assert(#__turboUIRoots == 0)" : "assert(#__turboUIRoots == 1)"),
            "Destroyed HUD roots accumulated or the replacement HUD did not mount");
        if (resetFrame) {
            const auto oldHud = scripts.status().at("destroyedHud").get<engine::ecs::EntityId>();
            require(!world.isAlive(oldHud), "Forgotten HUD behavior cancelled its pending ECS destruction");
        }
        require(!scripts.inspect(driver).value("failed", true), "Respawn driver failed silently");
    }
    for (const auto& message : scripts.output()) {
        require(message.kind != engine::scripting::ScriptMessage::Kind::Error, "Prefab/React respawn produced a script error");
    }
    scripts.stop();
    require(engine::scripting::ReactRuntime::reset(scripts), "Respawn React reset failed");
    require(scripts.execute("assert(#__turboUIRoots == 0 and #__turboUIEvents == 0)"), "Respawn cleanup retained UI work");
    scripts.shutdown();
    require(scripts.memoryUsed() == 0U, "Respawn shutdown retained VM allocations");
}

void hostileBehaviorTables() {
    struct Fixture {
        std::filesystem::path root = std::filesystem::temp_directory_path() / ("tge-host-boundary-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } fixture;
    std::filesystem::create_directory(fixture.root);
    write(fixture.root / "frozen.luau", "return {Init=function(self) table.freeze(self) end, Update=function(self) end}");
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr, fixture.root), "Frozen behavior VM failed");
    const auto frozen = world.createEntity();
    world.addComponent<ScriptComponent>(frozen, ScriptComponent{"frozen.luau", true, Json::object()});
    scripts.start();
    world.getComponent<ScriptComponent>(frozen)->fields["changed"] = 1;
    scripts.update(0.01);
    require(scripts.inspect(frozen).value("failed", false), "Frozen self did not produce a contained behavior error");
    require(scripts.execute("Engine.setStatus('aliveAfterFrozenSelf',true)"), "Frozen self escaped the host VM boundary");
    require(lua_gettop(scripts.state()) == 0, "Frozen self left the VM stack unbalanced");
    write(fixture.root / "frozen.luau", "return {Update=function(self) end}");
    require(scripts.requestReload("frozen.luau"), "Frozen behavior reload was not queued");
    scripts.pollReload();
    require(scripts.inspect(frozen).value("failed", false), "Reload silently unfroze a script-owned table");
    require(scripts.execute("assert(Engine.getStatus('aliveAfterFrozenSelf'))"), "Frozen reload escaped the host VM boundary");
    require(lua_gettop(scripts.state()) == 0, "Frozen reload left the VM stack unbalanced");
    scripts.stop();
    world.removeComponent<ScriptComponent>(frozen);
    scripts.registerModule("HostileFields", R"(
        return {Init=function(self)
            self.fields=nil
            setmetatable(self,{__newindex=function() error('unexpected user metamethod in host write') end})
        end}
    )");
    const auto guarded = world.createEntity();
    world.addComponent<ScriptComponent>(guarded, ScriptComponent{"HostileFields", true, Json::object()});
    scripts.start();
    world.getComponent<ScriptComponent>(guarded)->fields["changed"] = 2;
    scripts.update(0.01);
    require(!scripts.inspect(guarded).value("failed", true), "Host field synchronization ran a user __newindex");
    require(scripts.inspect(guarded)["fields"]["changed"] == 2, "Host field synchronization failed");
    require(lua_gettop(scripts.state()) == 0, "Host fields left the VM stack unbalanced");
}

void automaticDependencyReload() {
    struct Fixture {
        std::filesystem::path root = std::filesystem::temp_directory_path() / ("tge-live-scripts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } fixture;
    std::filesystem::create_directory(fixture.root);
    write(fixture.root / "rules.luau", "return {factor=2}");
    write(fixture.root / "component.luau", "local R=require('React'); return function() return R.createElement('TextLabel',{Text='old UI'}) end");
    write(fixture.root / "game.luau", R"(
        local rules=require('rules.luau')
        local Component=require('component.luau')
        local function PrivateUI() return require('React').createElement('TextLabel',{Text='private old'}) end
        return {
            Init=function(self)
                self.state.ticks=0
                Engine.setStatus('init',(Engine.getStatus('init') or 0)+1)
                local root=require('ReactTurbo').createRoot()
                self.state.root=root
                root:render(require('React').createElement(Component))
                self.state.privateRoot=require('ReactTurbo').createRoot()
                self.state.privateRoot:render(require('React').createElement(PrivateUI))
            end,
            Update=function(self) self.state.ticks+=1; Engine.setStatus('factor',rules.factor) end,
            OnReload=function(self) self.state.reloads=(self.state.reloads or 0)+1 end,
        }
    )");
    engine::core::JobSystem jobs(true, 2);
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, &jobs, fixture.root), "Live fixture VM failed");
    require(engine::scripting::ReactRuntime::install(scripts), "Live fixture React failed");
    const auto entity = world.createEntity();
    world.addComponent<ScriptComponent>(entity, ScriptComponent{"game.luau", true, Json::object()});
    world.addComponent<engine::ecs::Transform>(entity);
    scripts.start(); scripts.update(0.01);
    const auto waitFor = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            scripts.pollReload(); jobs.waitAll(); scripts.pollReload();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!predicate()) { for (const auto& message : scripts.output()) std::cerr << message.text << '\n'; }
        require(predicate(), "Automatic reload timed out");
    };
    write(fixture.root / "rules.luau", "return {factor=9}");
    // pollReload runs even while simulation is paused: no tick/Init should execute.
    waitFor([&] { return scripts.inspect(entity)["state"].value("reloads", 0) >= 1; });
    require(scripts.inspect(entity)["state"]["ticks"] == 1, "Reload advanced a paused game");
    scripts.update(0.01);
    require(scripts.status().at("factor") == 9 && scripts.status().at("init") == 1, "Dependency reload restarted Init or retained a stale require");
    const auto reloaded = scripts.inspect(entity)["state"].value("reloads", 0);
    write(fixture.root / "component.luau", "local R=require('React'); return function() return R.createElement('TextLabel',{Text='live UI'}) end");
    waitFor([&] { return scripts.inspect(entity)["state"].value("reloads", 0) > reloaded; });
    require(scripts.execute("assert(__turboUIRoots[1].children[1].props.Text=='live UI')"), "Init-only React root did not refresh");
    std::ifstream source(fixture.root / "game.luau");
    std::string changed(std::istreambuf_iterator<char>(source), {}); source.close();
    changed.replace(changed.find("private old"), std::string("private old").size(), "private live");
    const auto privateReload = scripts.inspect(entity)["state"].value("reloads", 0);
    write(fixture.root / "game.luau", changed);
    waitFor([&] { return scripts.inspect(entity)["state"].value("reloads", 0) > privateReload; });
    require(scripts.execute("assert(__turboUIRoots[2].children[1].props.Text=='private live')"), "Module-local Init-only React component did not refresh");
    const int stack = lua_gettop(scripts.state());
    for (int i=0; i<100; ++i) require(engine::scripting::ReactRuntime::pump(scripts, 0.016), "Cached React pump failed");
    require(lua_gettop(scripts.state()) == stack, "Cached React pump leaked stack");
    const auto outputCount = scripts.output().size();
    write(fixture.root / "rules.luau", "return {factor=");
    waitFor([&] { return scripts.output().size() > outputCount; });
    scripts.update(0.01);
    require(scripts.status().at("factor") == 9, "Syntax error discarded last-good dependency graph");
    const auto errors = scripts.output().size();
    write(fixture.root / "rules.luau", "error('broken evaluation')");
    waitFor([&] { return scripts.output().size() > errors; });
    scripts.update(0.01);
    require(scripts.status().at("factor") == 9, "Evaluation error discarded live behavior");
    const auto recovered = scripts.inspect(entity)["state"].value("reloads", 0);
    write(fixture.root / "rules.luau", "return {factor=4}");
    waitFor([&] { return scripts.inspect(entity)["state"].value("reloads", 0) > recovered; });
    scripts.update(0.01);
    require(scripts.status().at("factor") == 4 && scripts.status().at("init") == 1, "Live recovery lost game state");
    require(scripts.execute("local e=" + std::to_string(entity) + "; Engine.setRotation(e,vector.create(1,2,3)); assert(Engine.getRotation(e)==vector.create(1,2,3))" ) == true,
            "Rotation binding unavailable");
}
} // namespace

int main() {
    try { runtimeAndReload(); gameplayDemoAndReact(); updateDestructionKeepsPrefabRespawnsBounded(); automaticDependencyReload(); hostileBehaviorTables(); }
    catch (const std::exception& error) { std::cerr << "Scripting test failure: " << error.what() << '\n'; return 1; }
    std::cout << "Luau: lifecycle, prefab fields, bindings, async reload, last-good code, budgets, stale handles, gameplay and React passed\n";
    return 0;
}
