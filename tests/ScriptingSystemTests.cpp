#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <lua.h>

#include "engine/core/JobSystem.h"
#include "engine/ecs/components.h"
#include "engine/ecs/render_interpolation.h"
#include "engine/ecs/world.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
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

void boundedScriptAndPrefabReads() {
    struct Fixture {
        std::filesystem::path root = std::filesystem::temp_directory_path() / ("tge-source-bounds-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } fixture;
    std::filesystem::create_directory(fixture.root);
    engine::core::JobSystem jobs(true, 2);
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, &jobs, fixture.root), "Bounded-source VM initialization failed");
    auto limits = scripts.limits(); limits.sourceBytes = 256; scripts.setLimits(limits);
    const std::string behavior = "return {Init=function(self) self.state.marker=7 end}";
    write(fixture.root / "live.luau", behavior);
    std::string exact = "return {}"; exact.resize(limits.sourceBytes, ' ');
    write(fixture.root / "exact.luau", exact);
    write(fixture.root / "large.luau", exact + ' ');
    require(scripts.loadBehavior("exact.luau") && !scripts.loadBehavior("large.luau"),
        "Initial module load failed its exact source-byte boundary");
    std::string prefab = R"({"name":"bounded","components":{"script":{"path":"live.luau"}}})";
    prefab.resize(limits.sourceBytes, ' ');
    write(fixture.root / "exact.json", prefab);
    write(fixture.root / "large.json", prefab + ' ');
    write(fixture.root / "trailing.json", R"({"name":"malformed"} {})");
    const auto entity = scripts.spawnPrefab("exact.json");
    require(world.isAlive(entity), "Prefab at the exact source limit failed to spawn");
    const auto count = world.aliveCount();
    const int stackTop = lua_gettop(scripts.state());
    require(scripts.spawnPrefab("large.json") == engine::ecs::kInvalidEntity &&
        scripts.spawnPrefab("trailing.json") == engine::ecs::kInvalidEntity && world.aliveCount() == count &&
        lua_gettop(scripts.state()) == stackTop, "Oversized or trailing-data prefab mutated ECS or leaked the VM stack");
    scripts.start();
    require(scripts.inspect(entity)["state"]["marker"] == 7, "Bounded-source fixture did not initialize");
    scripts.clearOutput();
    write(fixture.root / "live.luau", "return {Update=function(self) self.state.marker=99 end}" +
        std::string(limits.sourceBytes, ' '));
    require(scripts.requestReload("live.luau"), "Oversized reload was not scheduled");
    jobs.waitAll(); scripts.pollReload(); scripts.update(0.01);
    require(scripts.inspect(entity)["state"]["marker"] == 7 && lua_gettop(scripts.state()) == stackTop,
        "Oversized reload replaced last-good code/state or leaked its stack");
    bool diagnosed = false;
    for (const auto& message : scripts.output()) {
        diagnosed |= message.text.find("Live reload retained previous code:") != std::string::npos &&
            message.text.find("source size limit") != std::string::npos;
    }
    require(diagnosed, "Oversized reload omitted its last-good/source-byte diagnostic");
}

void fixedTickPointerAndCursorLock() {
    using namespace engine::platform;
    struct TestWindow final : Window {
        CursorMode mode{CursorMode::Normal};
        bool hasFocus{true};
        void pollEvents() override {}
        bool shouldClose() const override { return false; }
        void requestClose() override {}
        void* nativeHandle() const override { return nullptr; }
        int width() const override { return 1280; }
        int height() const override { return 720; }
        void setTitle(const std::string&) override {}
        void setVSync(bool) override {}
        void setCursorMode(CursorMode value) override { mode = value; }
        CursorMode cursorMode() const override { return mode; }
        bool focused() const override { return hasFocus; }
        bool hasOpenGLContext() const override { return false; }
        void setEventCallback(EventCallback) override {}
    } window;

    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr), "Pointer input VM initialization failed");
    scripts.setInputWindow(&window);
    require(scripts.execute("assert(not Input.setCursorLocked(true))"), "Edit mode acquired the game cursor");
    scripts.start();
    scripts.setPointerInputEnabled(false);
    require(scripts.execute("assert(not Input.setCursorLocked(true))"), "Editor UI acquired the game cursor");
    scripts.setPointerInputEnabled(true);
    window.hasFocus = false;
    require(scripts.execute("assert(not Input.setCursorLocked(true))"), "Unfocused game acquired the cursor");
    window.hasFocus = true;
    require(scripts.execute("assert(Input.setCursorLocked(true) and Input.isCursorLocked())") &&
        window.mode == CursorMode::Disabled, "Game could not acquire its cursor");

    InputManager::reset();
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 600, .mouseY = 350});
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 603, .mouseY = 348});
    InputManager::onEvent(Event{.type = EventType::MouseButtonPressed});
    InputManager::onEvent(Event{.type = EventType::MouseButtonReleased});
    InputManager::onEvent(Event{.type = EventType::KeyPressed, .key = KeyCode::R});
    InputManager::onEvent(Event{.type = EventType::KeyReleased, .key = KeyCode::R});
    InputManager::beginFrame();
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 608, .mouseY = 346});
    require(scripts.execute("assert(not Input.wasKeyPressed('R'))"), "Fixed-tick key edges leaked outside a tick");
    InputManager::beginFixedTick();
    require(scripts.execute(R"(
        local dx, dy = Input.mouseDelta()
        assert(dx == 8 and dy == -4)
        assert(Input.wasMouseButtonPressed('Left') and not Input.isMouseButtonDown('Left'))
        assert(not Input.wasMouseButtonPressed('Left\0Middle'))
        assert(Input.wasKeyPressed('R') and Input.wasKeyReleased('R') and not Input.isKeyDown('R'))
    )"), "Pointer movement or a quick click was lost between simulation ticks");
    InputManager::endFixedTick();
    InputManager::beginFixedTick();
    const bool tickInputCleared = scripts.execute(R"(
        local dx, dy = Input.mouseDelta()
        assert(dx == 0 and dy == 0 and not Input.wasMouseButtonPressed('Left'))
        assert(not Input.wasKeyPressed('R') and not Input.wasKeyReleased('R'))
    )");
    InputManager::endFixedTick();
    require(tickInputCleared, "A subsequent simulation tick replayed input");
    require(scripts.releaseCursorLock() && window.mode == CursorMode::Normal,
        "Host Escape could not release the game cursor");
    window.mode = CursorMode::Disabled;
    require(scripts.execute("assert(not Input.isCursorLocked() and not Input.setCursorLocked(true)); Input.setCursorLocked(false)") &&
        window.mode == CursorMode::Disabled, "The game released or claimed the editor camera's cursor");
    window.mode = CursorMode::Normal;
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 700, .mouseY = 400});
    require(scripts.execute("assert(Input.setCursorLocked(true))"), "Game could not reacquire its cursor");
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 2000, .mouseY = 1300});
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 2004, .mouseY = 1298});
    require(InputManager::mouseDeltaForTickRaw() == std::pair{4.0, -2.0} &&
        InputManager::mouseDeltaRaw() == std::pair{4.0, -2.0}, "Cursor reacquisition replayed a coordinate jump");
    InputManager::onEvent(Event{.type = EventType::FocusLost});
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 880, .mouseY = 90});
    InputManager::onEvent(Event{.type = EventType::FocusGained});
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 21, .mouseY = 9});
    InputManager::onEvent(Event{.type = EventType::MouseMoved, .mouseX = 19, .mouseY = 12});
    require(InputManager::mouseDeltaForTickRaw() == std::pair{-2.0, 3.0} &&
        InputManager::mouseDeltaRaw() == std::pair{-2.0, 3.0}, "Focus restoration replayed a coordinate jump");
    scripts.stop();
    require(window.mode == CursorMode::Normal, "Stop left the cursor locked");
    InputManager::reset();
}

void runtimeAndReload() {
    const auto root = std::filesystem::temp_directory_path() / ("tge-scripting-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    write(root / "behavior.luau", behavior(1));
    write(root / "prefab.json", R"({"name":"Fixture","components":{"transform":{"position":[3,4,5]},"script":{"path":"assets/behavior.luau","fields":{"speed":7}}}})");
    write(root / "part.json", R"({"name":"Part","components":{"meshRenderer":{"meshId":"builtin:cube"},"camera":{"active":false}}})");
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
    keyEvent.key = engine::platform::KeyCode::X;
    engine::platform::InputManager::onEvent(keyEvent);
    keyEvent.key = engine::platform::KeyCode::Digit2;
    engine::platform::InputManager::onEvent(keyEvent);
    engine::platform::InputManager::setCaptureState(true, true);
    require(scripts.execute("assert(Input.isKeyDown('W') and Input.wasKeyPressed('W') and Input.wasKeyPressed('X') and Input.wasKeyPressed('2'))"),
        "Focused viewport input was swallowed by ImGui capture or missing a key binding");
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
    require(scripts.execute(R"(
        assert(Input.mouseSensitivity() == 1)
        assert(not pcall(Input.setMouseSensitivity, 0/0))
        assert(not pcall(Input.setMouseSensitivity, 4.05))
        assert(not Input.setMouseSensitivity(1.5))
        assert(Input.mouseSensitivity() == 1)
    )"), "Headless scripts accepted out-of-range or writable mouse preferences");
    require(scripts.execute(R"(
        local part = Engine.spawnPrefab('part.json', {components={
            transform={position=vector.create(2,4,6), scale={0.25,0.5,0.75}},
            meshRenderer={tint={0.125,0.25,0.5,0.75}},
        }})
        Engine.setStatus('part', part)
        assert(Engine.setCameraFov(part, 0.75))
        assert(not pcall(Engine.setCameraFov, part, 0))
    )"), "Luau prefab overrides failed to spawn");
    const auto part = scripts.status().at("part").get<engine::ecs::EntityId>();
    const auto* partTransform = world.getComponent<engine::ecs::Transform>(part);
    const auto* partMesh = world.getComponent<engine::ecs::MeshRenderer>(part);
    const auto* partCamera = world.getComponent<engine::ecs::Camera>(part);
    require(partTransform && partTransform->position == glm::vec3(2,4,6) && partTransform->scale == glm::vec3(0.25F,0.5F,0.75F) &&
        partMesh && partMesh->tint == glm::vec4(0.125F,0.25F,0.5F,0.75F), "Prefab numeric sequences changed the authored transform or tint");
    require(partCamera && partCamera->verticalFovRadians == 0.75F, "Camera FOV mutation accepted an invalid value or was not applied");
    require(scripts.execute(R"(
        local part = Engine.getStatus('part')
        assert(not Engine.snapRenderInterpolation(part))
        assert(Engine.setRenderInterpolation(part, true))
        Engine.setPosition(part, vector.create(4, 4, 6))
        assert(Engine.setCameraFov(part, 1.25))
        assert(Engine.setRenderInterpolation(part, true))
    )"), "Could not enable Luau render interpolation");
    const auto* history = world.getComponent<engine::ecs::RenderPoseHistory>(part);
    require(history && history->previous.position.x == 2.0F && history->previousFov == 0.75F,
        "Enabling interpolation repeatedly reset render pose history");
    require(scripts.execute("assert(Engine.snapRenderInterpolation(Engine.getStatus('part')))"),
        "Could not snap Luau render interpolation");
    require(history->previous.position.x == 4.0F && history->previousFov == 1.25F,
        "Snap failed to adopt the current pose and FOV");
    require(scripts.execute(R"(
        local part = Engine.getStatus('part')
        assert(Engine.setRenderInterpolation(part, false))
        assert(not Engine.snapRenderInterpolation(part))
        Engine.destroy(part)
    )"), "Could not disable Luau render interpolation and clean up its fixture");
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
    scripts.registerModule("InspectorMetadata", R"(
        return {fields={
            speed={type='number',default=2,min=0,max=20,step=0.25,label='Move speed',category='Movement',description='World units per second'},
            lives={type='integer',default=3,min=1,max=9},
            enabled={type='bool',default=true},
            title={type='string',default='Runner'},
            badStep={default=1,step=0}, badLabel={default=1,label=4}, badDefault={default=12,max=10}
        }}
    )");
    const auto inspectorSchema = scripts.fields("InspectorMetadata");
    require(inspectorSchema.size() == 4U, "Invalid Inspector metadata was accepted");
    const auto& speedMetadata = inspectorSchema.back();
    require(speedMetadata.name == "speed" && speedMetadata.label == "Move speed" && speedMetadata.step == 0.25 &&
        speedMetadata.category == "Movement" && speedMetadata.description == "World units per second", "Inspector metadata was lost or not grouped");
    ScriptComponent editedFields{"InspectorMetadata", true, Json::object()};
    require(scripts.setField(editedFields, "speed", 100.0) && editedFields.fields["speed"] == 20.0,
        "Inspector numeric edit escaped declared bounds");
    require(scripts.setField(editedFields, "lives", 4.7) && editedFields.fields["lives"] == 5.0,
        "Inspector integer edit was not normalized");
    require(scripts.setField(editedFields, "title", "Fast runner") && scripts.setField(editedFields, "enabled", false),
        "Inspector string/boolean edits failed");
    const auto validEdits = editedFields.fields;
    require(!scripts.setField(editedFields, "speed", "wrong") && !scripts.setField(editedFields, "enabled", 7) &&
        !scripts.setField(editedFields, "missing", true) && !scripts.setField(editedFields, "speed", 20.0) &&
        !scripts.setField(editedFields, "speed", std::numeric_limits<double>::quiet_NaN()) &&
        editedFields.fields == validEdits, "Invalid or unchanged Inspector edits changed authored fields");
    scripts.registerModule("FractionalIntegerBounds", R"(
        return {fields={
            valid={type='integer',default=3,min=2.2,max=3.8},
            emptyRange={type='integer',default=2.5,min=2.2,max=2.8}
        }}
    )");
    const auto fractionalIntegers = scripts.fields("FractionalIntegerBounds");
    require(fractionalIntegers.size() == 1U && fractionalIntegers[0].minimum == 3 && fractionalIntegers[0].maximum == 3,
        "Integer schema did not preserve the integral intersection of fractional bounds");
    ScriptComponent fractionalFields{"FractionalIntegerBounds", true, Json::object()};
    require(scripts.setField(fractionalFields, "valid", -10.0) && fractionalFields.fields["valid"] == 3.0,
        "Integer field normalization escaped fractional declared bounds");

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

void deferredCleanupDestroysOwnedEntities() {
    engine::ecs::World world;
    int nativeDeletes = 0;
    int deleteDepth = 0;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr, std::filesystem::temp_directory_path()), "Cleanup queue VM failed");
    const auto survivor = world.createEntity();
    world.addComponent<engine::ecs::Transform>(survivor);
    scripts.setDestroyCallback([&](const engine::ecs::EntityId entity) {
        require(++deleteDepth == 1, "Cleanup reentered native deletion");
        ++nativeDeletes;
        scripts.forgetEntity(entity);
        world.destroyEntity(entity);
        --deleteDepth;
    });
    scripts.registerModule("CleanupCascade", R"(
        return {
            fields = {target={type='integer',default=0}, survivor={type='integer',default=0}},
            Destroy = function(self)
                local counts = Engine.getStatus('cleanupCounts') or {}
                local key = tostring(self.entity)
                counts[key] = (counts[key] or 0) + 1
                Engine.setStatus('cleanupCounts',counts)
                assert(not pcall(Engine.spawnPrefab,'missing-cleanup-prefab.json'),'Cleanup spawned an entity')
                assert(not pcall(Engine.setPosition,self.fields.survivor,vector.create(9,9,9)),'Cleanup changed a pose')
                Engine.destroy(self.fields.target)
                Engine.destroy(self.entity)
            end,
        }
    )");
    const auto addPair = [&] {
        const auto first = world.createEntity();
        const auto second = world.createEntity();
        world.addComponent<ScriptComponent>(first, ScriptComponent{"CleanupCascade", true, {{"target", second}, {"survivor", survivor}}});
        world.addComponent<ScriptComponent>(second, ScriptComponent{"CleanupCascade", true, {{"target", first}, {"survivor", survivor}}});
        return std::pair{first, second};
    };
    const auto firstPair = addPair();
    scripts.start();
    scripts.forgetEntity(firstPair.first);
    scripts.forgetEntity(firstPair.first);
    require(world.aliveCount() == 3 && nativeDeletes == 0, "Forget performed structural deletion before a safe boundary");
    world.removeComponent<ScriptComponent>(firstPair.first);
    scripts.update(0.01);
    require(world.aliveCount() == 1 && world.isAlive(survivor) && nativeDeletes == 2,
            "Cascading cleanup leaked or removed unrelated entities");
    const auto secondPair = addPair();
    scripts.update(0.01);
    scripts.stop();
    scripts.stop();
    require(world.aliveCount() == 1 && nativeDeletes == 4, "Stop lost queued cleanup or repeated native deletion");
    for (const auto entity : {firstPair.first, firstPair.second, secondPair.first, secondPair.second})
        require(scripts.status().at("cleanupCounts").at(std::to_string(entity)) == 1, "Cleanup callback did not run exactly once");
    scripts.registerModule("CleanupPermissionsRestored", "assert(not pcall(Engine.destroy," + std::to_string(survivor) + ")); return {}");
    require(scripts.loadBehavior("CleanupPermissionsRestored") && world.isAlive(survivor), "Cleanup permission escaped its callback");
    require(world.getComponent<engine::ecs::Transform>(survivor)->position == glm::vec3(0), "Cleanup changed an unrelated pose");
    for (const auto& message : scripts.output())
        require(message.kind != engine::scripting::ScriptMessage::Kind::Error, "Deferred cleanup emitted a script error");
}

void fieldTypesAndBinaryStrings() {
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world,nullptr,std::filesystem::temp_directory_path()), "Binary string fixture VM failed");
    scripts.registerModule("RawFieldTypes", R"(
        return {fields={
            text={type='string',default='before'..string.char(0)..'after'},
            badDefault={type='string',default=function() error('must not run') end},
            badLabel={default=1,label=function() error('must not run') end},
            badCategory={default=1,category=coroutine.create(function() end)},
            badShort=function() error('must not run') end,
        }}
    )");
    const int stack=lua_gettop(scripts.state());
    const auto schema=scripts.fields("RawFieldTypes");
    require(schema.size()==1 && schema[0].name=="text", "Non-scalar raw field metadata was converted to a supported string");
    require(schema[0].defaultValue==std::string("before\0after",12), "Field schema truncated an embedded-NUL default");
    ScriptComponent component{"RawFieldTypes",true,Json::object()};
    scripts.populateDefaults(component);
    require(component.fields.at("text")==std::string("before\0after",12), "Default population truncated an embedded-NUL string");
    require(scripts.execute(R"(
        local zero=string.char(0)
        Engine.setStatus('binary','left'..zero..'right')
        Engine.setStatus('nested',{['key'..zero..'tail']='value'..zero..'tail'})
        local nested=Engine.getStatus('nested')
        assert(nested['key'..zero..'tail']=='value'..zero..'tail','JSON table keys/values did not round-trip')
    )"), "Binary Lua/JSON keys and values did not round-trip through Engine status");
    require(scripts.status().at("binary")==std::string("left\0right",10), "Engine status truncated a binary string value");
    require(scripts.status().at("nested").at(std::string("key\0tail",8))==std::string("value\0tail",10),
        "Engine status truncated a binary object key or value");
    require(lua_gettop(scripts.state())==stack && scripts.execute("assert(true)"), "Field validation or binary conversion corrupted the VM stack");
}

void largeFieldSchemaKeepsTypedDefaults() {
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr, std::filesystem::temp_directory_path()), "Large schema fixture VM failed");
    scripts.registerModule("LargeFieldSchema", R"(
        local fields={}
        for index=1,80 do
            fields['parameter'..index]={
                type='number',default=index,min=0,max=100,step=.25,
                label='Parameter '..index,description='Editable typed value',category='Tuning',
                unused={nested={ignored=true}},
            }
        end
        return {fields=fields}
    )");
    const int stack = lua_gettop(scripts.state());
    const auto schema = scripts.fields("LargeFieldSchema");
    require(schema.size() == 80U, "Detailed field schema was silently truncated by the generic JSON item budget");
    for (const auto& field : schema) {
        require(field.type == "number" && field.defaultValue.is_number() && field.minimum == 0 &&
                field.maximum == 100 && field.step == .25 && field.category == "Tuning",
            "Field metadata was replaced by a generic JSON budget placeholder");
    }
    ScriptComponent component{"LargeFieldSchema", true, Json{{"parameter1", 99}, {"unrelated", "retained"}}};
    scripts.populateDefaults(component);
    for (int index = 2; index <= 80; ++index) {
        require(component.fields.at("parameter" + std::to_string(index)) == index,
            "A large valid schema lost or changed a numeric default");
    }
    require(component.fields.at("parameter1") == 99 && component.fields.at("unrelated") == "retained" &&
            scripts.setField(component, "parameter80", 42.5) && component.fields.at("parameter80") == 42.5,
        "Large schema erased saved overrides or made later parameters uneditable");
    scripts.registerModule("TooManyFields", R"(
        local fields={}
        for index=1,129 do fields['parameter'..index]=index end
        return {fields=fields}
    )");
    require(scripts.fields("TooManyFields").empty(), "An oversized field schema was accepted partially");
    bool limitDiagnostic = false;
    for (const auto& message : scripts.output()) {
        if (message.kind == engine::scripting::ScriptMessage::Kind::Error &&
            message.text.find("at most 128 fields") != std::string::npos) limitDiagnostic = true;
    }
    require(limitDiagnostic, "Oversized field schema was rejected without an actionable diagnostic");
    require(lua_gettop(scripts.state()) == stack && scripts.execute("assert(true)"), "Large field schema leaked VM stack entries");
}

void eventsHonorCurrentBehaviorComponent() {
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr, std::filesystem::temp_directory_path()), "Event guard fixture VM failed");
    scripts.registerModule("EventGuardA", R"(
        return {
            OnEvent=function(self) Engine.setStatus('eventsA',(Engine.getStatus('eventsA') or 0)+1) end,
            Destroy=function(self) Engine.setStatus('destroyA',(Engine.getStatus('destroyA') or 0)+1) end,
        }
    )");
    scripts.registerModule("EventGuardB", R"(
        return {OnEvent=function(self) Engine.setStatus('eventsB',(Engine.getStatus('eventsB') or 0)+1) end}
    )");
    const auto entity = world.createEntity();
    world.addComponent<ScriptComponent>(entity, ScriptComponent{"EventGuardA", true, Json::object()});
    const auto event = [&] { scripts.queueEvent("Fixture", Json::object()); scripts.dispatchEvents(); };
    scripts.start();
    event();
    require(scripts.status().value("eventsA", 0) == 1, "Enabled behavior did not receive its initial event");
    world.getComponent<ScriptComponent>(entity)->enabled = false;
    event();
    require(scripts.status().value("eventsA", 0) == 1, "Disabled behavior received an event before update reconciliation");
    scripts.update(0);
    require(scripts.status().value("destroyA", 0) == 1 && scripts.inspect(entity).empty(), "Disabled behavior did not clean up once");
    world.getComponent<ScriptComponent>(entity)->enabled = true;
    scripts.update(0);
    event();
    require(scripts.status().value("eventsA", 0) == 2, "Re-enabled behavior did not resume events");
    world.removeComponent<ScriptComponent>(entity);
    event();
    require(scripts.status().value("eventsA", 0) == 2, "Removed behavior received an event before update reconciliation");
    scripts.update(0);
    require(scripts.status().value("destroyA", 0) == 2, "Removed behavior skipped or duplicated Destroy cleanup");
    world.addComponent<ScriptComponent>(entity, ScriptComponent{"EventGuardA", true, Json::object()});
    scripts.update(0);
    world.getComponent<ScriptComponent>(entity)->path = "EventGuardB";
    event();
    require(scripts.status().value("eventsA", 0) == 2 && scripts.status().value("eventsB", 0) == 0,
        "Changing a behavior module dispatched stale methods before update reconciliation");
    scripts.update(0);
    event();
    require(scripts.status().value("eventsB", 0) == 1 && scripts.status().value("destroyA", 0) == 3,
        "Replacement behavior did not initialize after cleaning up the old module once");
    require(lua_gettop(scripts.state()) == 0, "Behavior event guards left VM stack entries");
}

void unicodeBehaviorAndPrefabPaths() {
    struct Fixture {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
            ("tge-unicode-scripts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } fixture;
    const auto assetRoot = fixture.root / std::filesystem::u8path("Параметры");
    std::filesystem::create_directories(assetRoot / "scripts");
    std::filesystem::create_directories(assetRoot / "prefabs");
    const std::string module = "scripts/поведение.luau";
    const std::string dependency = "scripts/настройки.luau";
    const std::string prefab = "prefabs/персонаж.json";
    write(assetRoot / std::filesystem::u8path(dependency), "return {factor=1}");
    write(assetRoot / std::filesystem::u8path(module), R"(
        local rules=require('scripts/настройки.luau')
        return {
            fields={speed={type='number',default=3,min=0,max=10}},
            Init=function(self) self.state.initialized=(self.state.initialized or 0)+1 end,
            Update=function(self) Engine.setStatus('unicodeValue',rules.factor*self.fields.speed) end,
            OnReload=function(self) self.state.reloads=(self.state.reloads or 0)+1 end,
        }
    )");
    write(assetRoot / std::filesystem::u8path(prefab), Json{{"name", "Unicode"}, {"components", {
        {"script", {{"path", "assets/" + module}, {"fields", {{"speed", 7}}}}}
    }}}.dump());
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr, assetRoot), "Unicode asset root VM failed");
    const auto schema = scripts.fields("assets/" + module);
    require(schema.size() == 1U && schema.front().name == "speed", "Unicode behavior path did not expose its schema");
    const auto entity = scripts.spawnPrefab(prefab);
    require(world.isAlive(entity) && world.getComponent<ScriptComponent>(entity)->path == module &&
            world.getComponent<ScriptComponent>(entity)->fields.at("speed") == 7,
        "Unicode prefab/script path was not loaded or normalized as UTF-8");
    scripts.start();
    scripts.update(0);
    require(scripts.status().value("unicodeValue", 0) == 7, "Unicode behavior or required dependency did not run");
    write(assetRoot / std::filesystem::u8path(dependency), "return {factor=2}");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && scripts.status().value("unicodeValue", 0) != 14) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        scripts.update(0);
    }
    const auto instance = scripts.inspect(entity);
    require(scripts.status().value("unicodeValue", 0) == 14 && instance.at("state").value("initialized", 0) == 1 &&
            instance.at("state").value("reloads", 0) >= 1 && instance.at("fields").at("speed") == 7,
        "Automatic Unicode dependency reload lost module identity, state or authored fields");
    require(instance.at("script") == module && lua_gettop(scripts.state()) == 0,
        "Unicode module identity was converted to the system code page or leaked stack entries");
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

class FixtureNetwork final : public engine::scripting::ScriptNetwork {
public:
    std::string_view role() const override { return currentRole; }
    bool connected() const override { return true; }
    uint32_t localPeer() const override { return currentRole == "client" ? 17U : 0U; }
    bool isHost(uint32_t peer) const override { return currentRole == "server" && peer == 17; }
    bool usesSteam() const override { return steam; }
    std::vector<uint32_t> peers() const override { return {17U, 32U}; }
    Json stats() const override { return {{"rttMs", 12}}; }

    bool bind(engine::ecs::EntityId entity, uint32_t key) override {
        if (key != 42U) return false;
        bound = entity;
        return true;
    }
    bool unbind(uint32_t key) override {
        if (key != 42U || !bound) return false;
        bound = 0;
        return true;
    }
    bool registerPrefab(uint32_t key, std::string_view path) override {
        prefab = path;
        return key == 7U;
    }
    engine::ecs::EntityId entity(uint32_t key) const override { return key == 42U ? bound : 0U; }
    bool setState(uint32_t key, uint16_t value, uint8_t flags) override {
        if (key != 42U || !bound) return false;
        replica.value = value;
        replica.flags = flags;
        return true;
    }
    std::optional<engine::scripting::ScriptReplicaState> state(uint32_t key) const override {
        return entity(key) ? std::optional{replica} : std::nullopt;
    }
    std::optional<engine::scripting::ScriptReplicaState> rawState(uint32_t key) const override {
        return currentRole == "client" ? state(key) : std::nullopt;
    }
    std::optional<engine::scripting::ScriptPeerMotion> localMotion() const override {
        return currentRole == "client"
            ? std::optional{engine::scripting::ScriptPeerMotion{motion.x, motion.y, motion.z, motion.jumpBuffer,
                motion.coyote, 120, 51}}
            : std::nullopt;
    }
    uint64_t nextInputSequence() const override { return currentRole == "client" ? 55 : 0; }
    bool setPeerMotion(uint32_t peer, engine::scripting::ScriptPeerMotion value) override {
        if (peer != 17) return false;
        motion = value;
        return true;
    }

    bool setInput(float x, float z) override {
        axes = {x, z};
        return true;
    }
    std::optional<std::array<float, 2>> input(uint32_t peer) const override {
        return peer == 17U ? std::optional{axes} : std::nullopt;
    }
    bool setPeerName(uint32_t peer, std::string_view name) override {
        if (peer != 17) return false;
        nickname = name;
        return true;
    }
    std::string_view peerName(uint32_t peer) const override {
        return peer == 17 ? std::string_view(nickname) : std::string_view{};
    }
    std::string peerAvatar(uint32_t peer) const override {
        return steam && peer == 17 ? "steamavatar://76561197960265729" : std::string{};
    }
    uint32_t request(uint32_t key, uint8_t action, int32_t argument, std::string_view text) override {
        requestText = text;
        return key == 0x7fffffff && action == 255 && argument == INT32_MIN ? UINT32_MAX : 0U;
    }

    std::string_view currentRole{"server"};
    engine::ecs::EntityId bound{};
    engine::scripting::ScriptReplicaState replica{1, 2, 3, 0.5F, 0, 0, -0.25F, 0.75F};
    engine::scripting::ScriptPeerMotion motion{};
    std::array<float, 2> axes{};
    std::string prefab;
    std::string nickname;
    std::string requestText;
    bool steam{};
};

void networkBindingsAndProtectedRpc() {
    engine::ecs::World world;
    FixtureNetwork network;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr), "Network fixture VM failed");
    require(scripts.execute(R"(
        assert(Network.role() == "offline" and not Network.connected() and not Network.isHost())
        assert(Network.localPeer() == nil and #Network.peers() == 0 and next(Network.stats()) == nil)
        assert(Network.peerAvatar(17) == nil)
        assert(Network.rawState(42) == nil and Network.localMotion() == nil and Network.nextInputSequence() == nil)
        assert(Network.entity(42) == nil and Network.state(42) == nil and Network.input(17) == nil)
        assert(not Network.setInput(0, 0) and Network.request(42, 1, 0) == nil)
    )"), "Offline Network contract failed");
    scripts.setNetwork(&network);
    scripts.registerModule("network_fixture", R"(
        return {
            Init = function(self)
                assert(Network.role() == "server" and Network.connected())
                assert(not Network.isHost() and Network.isHost(17) and not Network.isHost(18))
                assert(not pcall(Network.isHost, 0))
                assert(Network.bind(self.entity, 42) and Network.entity(42) == self.entity)
                assert(Network.setPeerMotion(17, 1.25, -2.5, 0.5, 0.125, 0.0625))
            end,
            Destroy = function(self)
                assert(Network.unbind(42))
            end,
            Update = function(self)
                self.state.ticked = true
            end,
            OnNetworkRpc = function(self, peer, action, argument, text)
                assert(peer == 17)
                if action == 2 then return false end
                if action == 3 then error("RPC fixture failure") end
                if action == 4 then return true, "7" end
                if action == 5 then return true, 65536 end
                if action == 6 then while true do end end
                assert(action == 1 and argument == -2147483648)
                assert(type(text) == "string")
                assert(Network.setPeerName(peer, text))
                assert(Network.setState(42, 65535, 255))
                return true, 65535
            end,
            OnEvent = function(self, event)
                if event.type == "networkRpcResult" then
                    self.state.response = event.request
                end
            end,
        }
    )");
    const auto entity = world.createEntity();
    auto& component = world.addComponent<ScriptComponent>(entity, ScriptComponent{"network_fixture", true, Json::object()});
    scripts.start();
    require(network.bound == entity, "Network.bind did not register the running behavior");
    const int stackTop = lua_gettop(scripts.state());
    const auto accepted = scripts.invokeNetworkRpc(entity, 17, 1, INT32_MIN, "  Delta \xC3\xA4  ");
    require(accepted.handled && accepted.accepted && accepted.value == 65535, "RPC callback lost its accepted reply");
    require(network.nickname == "Delta \xC3\xA4", "RPC text did not reach server nickname registration intact");
    auto limits = scripts.limits();
    limits.callbackMilliseconds = 5.0;
    scripts.setLimits(limits);
    for (const auto action : std::array<uint8_t, 5>{2, 3, 4, 5, 6}) {
        const auto reply = scripts.invokeNetworkRpc(entity, 17, action, 0);
        require(reply.handled && !reply.accepted && reply.value == 0, "Invalid RPC callback did not fail closed");
    }
    scripts.setLimits(ScriptingSystem::Limits{});
    require(lua_gettop(scripts.state()) == stackTop, "RPC callback leaked the Luau stack");
    scripts.update(1.0 / 60.0);
    require(scripts.inspect(entity)["state"]["ticked"] == true, "RPC failure disabled the behavior");
    component.enabled = false;
    require(!scripts.invokeNetworkRpc(entity, 17, 1, 0).handled, "RPC called a disabled behavior");
    component.enabled = true;
    require(!scripts.invokeNetworkRpc(engine::ecs::kInvalidEntity, 17, 1, 0).handled, "RPC called a stale behavior");

    network.currentRole = "client";
    network.steam = true;
    using Input = engine::platform::InputManager;
    using Event = engine::platform::Event;
    using EventType = engine::platform::EventType;
    using KeyCode = engine::platform::KeyCode;
    Input::reset();
    scripts.setInputEnabled(true);
    Input::onEvent(Event{.type = EventType::KeyPressed, .key = KeyCode::X});
    Input::onEvent(Event{.type = EventType::KeyReleased, .key = KeyCode::X});
    Input::beginFrame();
    require(scripts.execute("assert(Input.wasKeyPressed('X') and Input.wasKeyReleased('X') and not Input.isKeyDown('X'))"),
        "Network Luau lost a key tap between simulation ticks");
    Input::clearTickKeyEdges();
    require(scripts.execute("assert(not Input.wasKeyPressed('X') and not Input.wasKeyReleased('X'))"),
        "Network Luau repeated input from an earlier tick");
    Input::reset();
    require(scripts.execute(R"(
        assert(Network.role() == "client" and Network.localPeer() == 17 and not Network.isHost())
        assert(Network.peers()[1] == 17 and Network.peers()[2] == 32 and Network.stats().rttMs == 12)
        local state = Network.state(42)
        assert(state.x == 1 and state.y == 2 and state.z == 3 and state.yaw == 0.5)
        assert(state.pitch == -0.25 and state.roll == 0.75 and state.value == 65535 and state.flags == 255)
        assert(Network.rawState(42).yaw == 0.5 and Network.rawState(99) == nil)
        local motion = Network.localMotion()
        assert(motion.x == 1.25 and motion.y == -2.5 and motion.z == 0.5)
        assert(motion.jumpBuffer == 0.125 and motion.coyote == 0.0625 and motion.tick == 120 and motion.inputAck == 51)
        assert(Network.nextInputSequence() == 55 and not Network.setPeerMotion(17, 0, 0, 0, 0, 0))
        assert(not pcall(Network.setPeerMotion, 17, math.huge, 0, 0, 0, 0))
        assert(Network.registerPrefab(7, "prefabs/pilot.json"))
        assert(Network.setInput(0.25, -1))
        local x, z = Network.input(17)
        assert(x == 0.25 and z == -1 and Network.input(18) == nil)
        assert(Network.request(2147483647, 255, -2147483648) == 4294967295)
        assert(Network.request(2147483647, 255, -2147483648, "Join \u{00e4}") == 4294967295)
        assert(Network.usesSteam() and Network.peerName(17) == "Delta \u{00e4}" and Network.peerName(32) == nil)
        assert(Network.peerAvatar(17) == "steamavatar://76561197960265729" and Network.peerAvatar(32) == nil)
        assert(not pcall(Network.peerAvatar, 0))
        assert(not Network.setPeerName(17, "Other"))
        assert(Network.normalizePeerName("  \u{0414}  ") == "\u{0414}")
        assert(Network.normalizePeerName(" ") == "" and Network.normalizePeerName("a\0b") == nil)
        assert(Network.normalizePeerName(string.rep("a", 129)) == nil)
        assert(not pcall(Network.request, 42, 1, 0, string.rep("a", 129)))
        assert(not pcall(Network.request, 0, 1, 0))
        assert(not pcall(Network.request, 2147483648, 1, 0))
        assert(not pcall(Network.request, "42", 1, 0))
        assert(not pcall(Network.request, 42, 256, 0))
        assert(not pcall(Network.request, 42, 1, 0.5))
        assert(not pcall(Network.setInput, 0/0, 0))
        assert(not pcall(Network.setInput, 1.01, 0))
        assert(not pcall(Network.registerPrefab, 7, "path\0trailing"))
        assert(not pcall(Network.registerPrefab, 7, string.rep("a", 1025)))
    )"), "Luau Network round trip or argument checks failed");
    require(network.prefab == "prefabs/pilot.json", "Invalid prefab path reached the host");
    require(network.requestText == "Join \xC3\xA4", "Client RPC lost text or accepted oversized text");
    require(!scripts.invokeNetworkRpc(entity, 17, 1, 0).handled, "RPC ran in a client VM");
    scripts.queueEvent("networkRpcResult", {{"request", UINT32_MAX}, {"status", "Accepted"}, {"value", 65535}});
    scripts.dispatchEvents();
    require(scripts.inspect(entity)["state"]["response"] == UINT32_MAX, "RPC result event was lost");
    scripts.stop();
    require(network.bound == 0, "Destroy did not release the replica binding");
}

class FixtureSteam final : public engine::scripting::ScriptSteam {
public:
    bool available() const override { return true; }
    Json localUser() const override { return {{"id", "76561197960265729"}, {"name", "Host"}}; }
    Json friends(unsigned offset, unsigned limit) override {
        if (offset != 2 || limit != 1) return Json::array();
        return Json::array({{{"id", "76561197960265731"}, {"name", "Friend"}, {"online", true}}});
    }
    Json session() const override { return {{"id", "109775243846574081"}}; }
    Json sessions(unsigned offset, unsigned limit) const override {
        return offset == 0 && limit == 32 ? Json::array({session()}) : Json::array();
    }
    std::string avatar(std::uint64_t id) override { return id == 76561197960265731ULL ? "steam-avatar://test" : ""; }

    std::uint32_t submit(engine::scripting::ScriptSteamRequest request, std::string& error) override {
        if (request.action == engine::scripting::ScriptSteamAction::LeaveSession) {
            error = "No active Steam lobby";
            return 0;
        }
        submitted.push_back(std::move(request));
        return static_cast<std::uint32_t>(submitted.size());
    }

    std::vector<engine::scripting::ScriptSteamRequest> submitted;
};

void steamBindingsAndDeferredEvents() {
    using engine::scripting::ScriptSteamAction;
    engine::ecs::World world;
    FixtureSteam steam;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr), "Steam fixture VM failed");
    require(scripts.execute(R"(
        assert(not Steam.available())
        assert(Steam.localUser() == nil and Steam.session() == nil and Steam.avatar("1") == nil)
        assert(#Steam.friends() == 0 and #Steam.sessions() == 0)
        local request, error = Steam.createSession({})
        assert(request == nil and type(error) == "string")
    )"), "Steam SDK-disabled contract failed");

    scripts.setSteam(&steam);
    require(scripts.execute("local request, error = Steam.openInviteOverlay(); assert(not request and type(error) == 'string')"),
        "Steam requests must fail safely outside Play");
    require(steam.submitted.empty(), "Steam request reached host in Edit mode");

    scripts.registerModule("steam_fixture", R"(
        return {
            Init = function(self)
                self.state.request = Steam.createSession(setmetatable({
                    name = "Test Lobby", visibility = "public", maxPlayers = 12, joinable = false,
                    properties = { ["game-mode"] = "race", map_name = "arena" },
                }, {__index = function() error("options invoked a metatable") end}))
            end,
            OnEvent = function(self, event)
                if event.type == "steamSession" then
                    self.state.resultRequest = event.request
                    self.state.resultLobby = event.lobbyId
                end
            end,
        }
    )");
    const auto entity = world.createEntity();
    world.addComponent<ScriptComponent>(entity, ScriptComponent{"steam_fixture", true, Json::object()});
    scripts.start();
    require(scripts.inspect(entity)["state"]["request"] == 1, "Steam lobby action did not return its request ID");
    const int stackTop = lua_gettop(scripts.state());
    require(scripts.execute(R"(
        assert(Steam.available() and Steam.localUser().id == "76561197960265729")
        assert(Steam.friends(2, 1)[1].id == "76561197960265731")
        assert(Steam.session().id == "109775243846574081" and Steam.sessions()[1].id == Steam.session().id)
        assert(Steam.avatar("76561197960265731") == "steam-avatar://test" and Steam.avatar("1") == nil)
        assert(Steam.findSessions(10) == 2)
        assert(Steam.joinSession("18446744073709551615") == 3)
        assert(Steam.inviteFriend("76561197960265731") == 4)
        assert(Steam.openProfile("76561197960265731") == 5)
        assert(Steam.openInviteOverlay() == 6)
        assert(Steam.setRichPresence("status", "Playing") == 7)
        assert(Steam.updateSession({ name = "Updated" }) == 8)
        local rejected, reason = Steam.leaveSession()
        assert(rejected == nil and reason == "No active Steam lobby")
        assert(not pcall(Steam.joinSession, 76561197960265731))
        assert(not pcall(Steam.joinSession, "18446744073709551616"))
        assert(not pcall(Steam.inviteFriend, "0001"))
        assert(not pcall(Steam.inviteFriend, "1\0x"))
        assert(not pcall(Steam.inviteFriend, "+1"))
        assert(not pcall(Steam.friends, -1, 1) and not pcall(Steam.friends, 0, 33))
        assert(not pcall(Steam.findSessions, 0) and not pcall(Steam.sessions, 1.5, 1))
        assert(not pcall(Steam.createSession, { maxPlayers = 1 }))
        assert(not pcall(Steam.createSession, { visibility = "hidden" }))
        assert(not pcall(Steam.updateSession, { joinable = 1 }))
        assert(not pcall(Steam.updateSession, { name = string.rep("n", 65) }))
        assert(not pcall(Steam.createSession, { properties = { ["invalid key"] = "x" } }))
        assert(not pcall(Steam.createSession, { properties = { mode = string.rep("a", 129) } }))
        local count = {}
        for i = 1, 9 do count["key_" .. i] = "value" end
        assert(not pcall(Steam.createSession, { properties = count }))
        assert(not pcall(Steam.setRichPresence, string.rep("k", 64), ""))
        assert(not pcall(Steam.setRichPresence, "status", string.rep("v", 256)))
    )"), "Steam Luau queries, argument bounds or request IDs failed");
    require(steam.submitted.size() == 8, "Malformed Steam actions reached the service");
    const auto& create = steam.submitted[0];
    require(create.action == ScriptSteamAction::CreateSession && create.options.name == "Test Lobby" &&
        create.options.visibility == "public" && create.options.maxPlayers == 12 && !create.options.joinable &&
        create.options.properties.at("game-mode") == "race", "Steam options did not reach the service");
    require(steam.submitted[1].action == ScriptSteamAction::FindSessions && steam.submitted[1].limit == 10 &&
        steam.submitted[2].id == UINT64_MAX && steam.submitted[3].id == 76561197960265731ULL &&
        steam.submitted[4].action == ScriptSteamAction::OpenProfile &&
        steam.submitted[5].action == ScriptSteamAction::OpenInviteOverlay &&
        steam.submitted[6].key == "status" && steam.submitted[6].value == "Playing" &&
        steam.submitted[7].options.visibility == "friends" && steam.submitted[7].options.maxPlayers == 8,
        "Steam action payloads lost precision or default values");

    scripts.queueEvent("steamSession", {{"request", 1}, {"lobbyId", "109775243846574081"}});
    scripts.dispatchEvents();
    require(scripts.inspect(entity)["state"]["resultRequest"] == 1 &&
        scripts.inspect(entity)["state"]["resultLobby"] == "109775243846574081", "Steam result event was not delivered");
    require(lua_gettop(scripts.state()) == stackTop, "Steam binding leaked the Luau stack");
    scripts.stop();
}
}

int main() {
    try { boundedScriptAndPrefabReads(); fixedTickPointerAndCursorLock(); runtimeAndReload(); deferredCleanupDestroysOwnedEntities(); fieldTypesAndBinaryStrings(); largeFieldSchemaKeepsTypedDefaults(); eventsHonorCurrentBehaviorComponent(); unicodeBehaviorAndPrefabPaths(); gameplayDemoAndReact(); updateDestructionKeepsPrefabRespawnsBounded(); automaticDependencyReload(); hostileBehaviorTables(); networkBindingsAndProtectedRpc(); steamBindingsAndDeferredEvents(); }
    catch (const std::exception& error) { std::cerr << "Scripting test failure: " << error.what() << '\n'; return 1; }
    std::cout << "Luau: lifecycle, typed field schemas, Unicode paths, current-component events, prefab fields, reload, budgets, gameplay and React passed\n";
    return 0;
}
