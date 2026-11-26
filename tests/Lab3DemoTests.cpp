#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lua.h>

#include "engine/core/ProjectManager.h"
#include "engine/ecs/components.h"
#include "engine/ecs/world.h"
#include "engine/platform/Input.h"
#include "engine/scripting/ScriptingSystem.h"
#include "engine/scripting/react/ReactRuntime.h"

namespace {
using engine::ecs::EntityId;
using engine::scripting::ReactRuntime;
using engine::scripting::ScriptComponent;
using engine::scripting::ScriptingSystem;
using Json = nlohmann::json;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requireEnglishMetadata(const std::vector<engine::scripting::ScriptField>& fields, const Json& expected) {
    require(fields.size() == expected.size(), "Demo behavior did not expose every editable field");
    for (const auto& field : fields) {
        require(expected.contains(field.name) && expected.at(field.name).at(0) == field.label &&
            expected.at(field.name).at(1) == field.category, "Demo Inspector labels and categories must be English");
        for (const auto* text : {&field.label, &field.category, &field.description}) {
            require(!text->empty(), "Demo Inspector metadata must include labels, categories and help");
            for (const unsigned char value : *text)
                require(value >= 32 && value <= 126, "Demo Inspector metadata contains non-English or control bytes");
        }
    }
}

void verifyProjectOpen() {
    namespace fs = std::filesystem;
    struct RegistryFixture final {
        fs::path directory = fs::temp_directory_path() /
            ("tge-lab3-project-open-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        RegistryFixture() { fs::create_directories(directory); }
        ~RegistryFixture() { std::error_code ignored; fs::remove_all(directory, ignored); }
    } fixture;
    engine::core::ProjectManager manager(fixture.directory / "projects.json");
    const fs::path root = ENGINE_LAB3_DEMO_ROOT;
    for (const auto& path : {root, root / "Lab3Materials.tgeproj"}) {
        std::string error;
        const auto project = manager.open(path, error);
        if (!project) throw std::runtime_error("Demo ProjectManager::open failed: " + error);
        require(project->name == "ЛР 3 — Спектр", "Demo project display name is not localized");
        require(project->assetRoot == fs::canonical(root / "assets"), "Demo project asset root changed");
        require(project->scenePath == fs::canonical(root / "assets/scenes/materials.scene.json"),
                "Demo project startup scene changed");
    }
    std::cout << "LAB3_PROJECT_OPEN_PASS directory=1 manifest=1 isolated_registry=1\n";
}

void tick(ScriptingSystem& scripts, const double dt = 1.0 / 60.0) {
    scripts.update(dt);
    require(ReactRuntime::pump(scripts, dt), "Native demo React pump failed");
}

void click(ScriptingSystem& scripts, const std::string& text) {
    const std::string source = R"(
        local function find(node)
            if node.type == 'TextButton' and node.props and node.props.Text == )" + Json(text).dump() + R"( then return node end
            for _, child in node.children or {} do
                local found = find(child)
                if found then return found end
            end
        end
        local found
        for _, root in __turboUIRoots do found = find(root); if found then break end end
        assert(found, 'Rendered demo button was not found')
        table.insert(__turboUIEvents, {instance=found, name='Activated'})
    )";
    require(scripts.execute(source, "=Lab3ButtonQueue"), "Cannot queue actual retained UI button");
    tick(scripts);
    tick(scripts);
}

void requireText(ScriptingSystem& scripts, const std::string& text, const bool substring = false) {
    const std::string source = R"(
        local expected = )" + Json(text).dump() + R"(
        local substring = )" + std::string(substring ? "true" : "false") + R"(
        local function find(node)
            local value = node.props and node.props.Text
            if type(value) == 'string' and (value == expected or substring and string.find(value, expected, 1, true)) then return true end
            for _, child in node.children or {} do
                if find(child) then return true end
            end
            return false
        end
        local found = false
        for _, root in __turboUIRoots do if find(root) then found = true; break end end
        assert(found, 'Rendered localized demo text was not found')
    )";
    require(scripts.execute(source, "=Lab3LocalizedText"), "Localized/demo field text did not reach the actual UI tree");
}

void observeDemoDestroy(ScriptingSystem& scripts) {
    require(scripts.execute(R"(
        __lab3DestroyCount = __lab3DestroyCount or 0
        local behavior = require('scripts/MaterialLab.luau')
        if __lab3ObservedBehavior ~= behavior then
            local destroy = behavior.Destroy
            behavior.Destroy = function(self)
                __lab3DestroyCount += 1
                destroy(self)
            end
            __lab3ObservedBehavior = behavior
        end
    )", "=Lab3DestroyCounter"), "Cannot observe actual demo Destroy callbacks");
}

std::vector<EntityId> spawned(engine::ecs::World& world) {
    std::vector<EntityId> result;
    world.forEach<engine::ecs::Tag>([&](const EntityId id, const auto& tag) {
        if (tag.value == "Lab3Spawned") result.push_back(id);
    });
    return result;
}

void verifyPrefabFailure() {
    namespace fs = std::filesystem;
    struct AssetFixture final {
        fs::path directory = fs::temp_directory_path() /
            ("tge-lab3-prefab-failure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        AssetFixture() {
            fs::create_directories(directory / "scripts");
            fs::create_directories(directory / "prefabs");
            fs::copy_file(fs::path(ENGINE_LAB3_DEMO_ROOT) / "assets/scripts/MaterialLab.luau",
                          directory / "scripts/MaterialLab.luau");
        }
        ~AssetFixture() { std::error_code ignored; fs::remove_all(directory, ignored); }
    } fixture;
    engine::ecs::World world;
    ScriptingSystem scripts;
    require(scripts.initialize(world, nullptr, fixture.directory), "Cannot initialize broken-prefab fixture");
    require(ReactRuntime::install(scripts), "Cannot install React in broken-prefab fixture");
    const auto controller = world.createEntity();
    ScriptComponent component;
    component.path = "scripts/MaterialLab.luau";
    scripts.populateDefaults(component);
    world.addComponent<ScriptComponent>(controller, component);
    scripts.start();
    tick(scripts);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            std::ofstream output(fixture.directory / "prefabs/jade.json", std::ios::binary);
            output << "{invalid prefab";
        }
        click(scripts, "Создать: Нефрит");
        require(!scripts.inspect(controller).value("failed", true), "Broken prefab disabled the playable controller");
        require(world.aliveCount() == 1 && scripts.status().at("lab3Materials").value("progress", -1) == 0,
                "Broken prefab leaked an entity or advanced the order");
    }
    click(scripts, "Следующая тема >");
    require(scripts.status().at("lab3Materials").value("page", 0) == 2,
            "Demo UI stopped responding after prefab failure");
    scripts.stop();
    require(scripts.execute("assert(#__turboUIRoots == 0 and #__turboUIEvents == 0)", "=Lab3FailureCleanup"),
            "Broken-prefab demo retained UI resources on Stop");
    std::cout << "LAB3_PREFAB_FAILURE_PASS missing=1 malformed=1 no_entity_leak=1 responsive_ui=1\n";
}

void verifyScriptSmoke() {
    engine::ecs::World world;
    ScriptingSystem scripts;
    const auto root = std::filesystem::path(ENGINE_LAB3_DEMO_ROOT);
    require(scripts.initialize(world, nullptr, root / "assets"), "Cannot initialize demo script smoke VM");
    require(ReactRuntime::install(scripts), "Cannot install actual React for demo script smoke");
    const int originalStack = lua_gettop(scripts.state());
    std::ifstream source(root / "tests/smoke.luau", std::ios::binary);
    require(static_cast<bool>(source), "Missing demo script smoke fixture");
    const std::string text{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
    const std::string staged = "__lab3NativeSmokeStages=true; __lab3NativeSmokeDone=false\n"
        "__lab3NativeSmokeRunner=coroutine.wrap(function()\n" + text +
        "\n__lab3NativeSmokeDone=true\nend)\n"
        "function __lab3NativeSmokeAdvance() if not __lab3NativeSmokeDone then __lab3NativeSmokeRunner() end end";
    if (!scripts.execute(staged, "=Lab3ScriptSmoke")) {
        for (const auto& message : scripts.output()) std::cerr << message.text << '\n';
        throw std::runtime_error("Cannot prepare staged demo smoke on the real Luau VM and React runtime");
    }
    for (int stage = 0; stage < 16; ++stage) {
        if (scripts.callGlobal("__lab3NativeSmokeAdvance")) continue;
        for (const auto& message : scripts.output()) std::cerr << message.text << '\n';
        throw std::runtime_error("Demo script smoke failed at stage " + std::to_string(stage + 1));
    }
    require(scripts.execute("assert(__lab3NativeSmokeDone, 'Demo smoke exceeded its bounded stage count'); "
        "__lab3NativeSmokeRunner=nil; __lab3NativeSmokeAdvance=nil; __lab3NativeSmokeStages=nil", "=Lab3SmokeDone"),
        "Demo smoke did not finish every staged assertion");
    require(world.aliveCount() == 0, "Explicit smoke Engine test double leaked native world mutations");
    require(ReactRuntime::reset(scripts), "Demo script smoke React reset failed");
    require(scripts.execute("assert(#__turboUIRoots == 0 and #__turboUIEvents == 0)", "=Lab3SmokeCleanup"),
            "Demo script smoke retained roots/events");
    require(lua_gettop(scripts.state()) == originalStack, "Demo script smoke leaked VM stack entries");
    for (const auto& message : scripts.output())
        require(message.kind != engine::scripting::ScriptMessage::Kind::Error, "Demo script smoke emitted a script error");
    std::cout << "LAB3_SCRIPT_SMOKE_PASS actual_luau=1 actual_react=1 explicit_engine_test_double=1 phase_continuity=1 caption_bound=8"
                 " max_stages=16 watchdog_unchanged=1 GPU_NOT_TESTED\n";
}
}

int main() {
    engine::ecs::World world;
    std::vector<EntityId> destroyed;
    ScriptingSystem scripts;
    try {
        verifyProjectOpen();
        engine::platform::InputManager::reset();
        const auto assets = std::filesystem::path(ENGINE_LAB3_DEMO_ROOT) / "assets";
        require(scripts.initialize(world, nullptr, assets), "Cannot initialize real demo asset root");
        require(ReactRuntime::install(scripts), "Cannot install real upstream React");
        const int originalStack = lua_gettop(scripts.state());

        const auto controller = world.createEntity();
        world.addComponent<engine::ecs::Tag>(controller, engine::ecs::Tag{"MaterialLabController"});
        ScriptComponent component;
        component.path = "scripts/MaterialLab.luau";
        scripts.populateDefaults(component);
        require(component.fields.value("rounds", 0) == 6 && component.fields.value("animate", false),
                "Demo behavior declarations did not become native field defaults");
        require(component.fields.value("title", "") == "СПЕКТР / Лаборатория материалов",
                "Demo behavior title did not use its Russian authoring default");
        const auto declarations = scripts.fields(component.path);
        requireEnglishMetadata(declarations, {{"title", {"Title", "Presentation"}}, {"rounds", {"Order Length", "Gameplay"}},
            {"spinSpeed", {"Rotation Speed", "Gallery"}}, {"animate", {"Animate Samples", "Gallery"}}});
        require(scripts.setField(component, "rounds", 100) && component.fields.at("rounds") == 12,
                "Demo order maximum was not enforced");
        require(scripts.setField(component, "rounds", 3), "Cannot set native authoring override");
        world.addComponent<ScriptComponent>(controller, component);

        const auto preview = world.createEntity();
        engine::ecs::Transform previewTransform;
        previewTransform.position = {0, 1, 6};
        world.addComponent<engine::ecs::Transform>(preview, previewTransform);
        ScriptComponent motion;
        motion.path = "scripts/SampleMotion.luau";
        scripts.populateDefaults(motion);
        requireEnglishMetadata(scripts.fields(motion.path), {{"speed", {"Rotation Speed", "Movement"}},
            {"bobbing", {"Bobbing", "Movement"}}, {"height", {"Amplitude", "Movement"}}, {"caption", {"Caption", "Presentation"}}});
        require(motion.fields.value("caption", "") == "Образец материала", "English metadata changed the Russian caption value");
        world.addComponent<ScriptComponent>(preview, motion);

        scripts.start();
        tick(scripts);
        require(!scripts.inspect(controller).value("failed", true), "Native demo controller failed during Init/Update");
        require(scripts.execute("assert(#__turboUIRoots == 1)", "=Lab3RootMounted"), "Native demo root did not mount");
        requireText(scripts, "СПЕКТР / Лаборатория материалов");
        require(scripts.status().at("lab3Materials").value("goal", 0) == 3, "Authoring field override did not reach next Play");
        require(world.getComponent<engine::ecs::Transform>(preview)->rotationEulerRadians.y > 0,
                "Real SampleMotion callback did not mutate native Transform");

        auto* authoredFields = world.getComponent<ScriptComponent>(controller);
        require(scripts.setField(*authoredFields, "title", "Редактируемый спектр"), "Cannot edit demo title field");
        tick(scripts);
        requireText(scripts, "Редактируемый спектр");
        auto* previewFields = world.getComponent<ScriptComponent>(preview);
        require(scripts.setField(*previewFields, "caption", "Подпись из параметров Luau"), "Cannot edit sample caption field");
        tick(scripts);
        tick(scripts);
        requireText(scripts, "Подпись из параметров Luau", true);

        click(scripts, "Создать: Янтарь");
        require(spawned(world).empty() && scripts.status().at("lab3Materials").value("mistakes", 0) == 1,
                "Wrong material choice incorrectly spawned/awarded progress");
        click(scripts, "Создать: Нефрит");
        click(scripts, "Создать: Аметист");
        click(scripts, "Создать: Янтарь");
        const auto samples = spawned(world);
        require(samples.size() == 3 && world.aliveCount() == 5, "UI callbacks did not spawn exactly three native ECS entities");
        require(scripts.status().at("lab3Materials").value("finished", false), "Three real prefab spawns did not finish the order");
        click(scripts, "Создать: Нефрит");
        require(spawned(world).size() == 3, "Completed order accepted an extra sample");
        std::vector<std::string> materials;
        for (const auto id : samples) {
            const auto* mesh = world.getComponent<engine::ecs::MeshRenderer>(id);
            const auto* transform = world.getComponent<engine::ecs::Transform>(id);
            require(mesh && transform, "Real prefab lost mesh/transform components");
            require(mesh->meshId == "__diligent_sphere__", "Demo prefab did not retain built-in mesh reference");
            materials.push_back(mesh->materialId);
        }
        for (const auto* expected : {"materials/jade-gallery.json", "materials/amber.json", "materials/violet.json"}) {
            bool found = false;
            for (const auto& material : materials) if (material == expected) found = true;
            require(found, "Native prefab loader lost a materialId assignment");
        }

        const auto first = samples.front();
        const float initialRotation = world.getComponent<engine::ecs::Transform>(first)->rotationEulerRadians.y;
        auto* liveFields = world.getComponent<ScriptComponent>(controller);
        require(scripts.setField(*liveFields, "spinSpeed", 2.0), "Cannot update native number field");
        tick(scripts, 0.4);
        require(world.getComponent<engine::ecs::Transform>(first)->rotationEulerRadians.y > initialRotation,
                "Edited number field did not drive live native sample transform");
        liveFields = world.getComponent<ScriptComponent>(controller);
        require(scripts.setField(*liveFields, "animate", false), "Cannot update native boolean field");
        const float stoppedRotation = world.getComponent<engine::ecs::Transform>(first)->rotationEulerRadians.y;
        tick(scripts, 0.4);
        require(world.getComponent<engine::ecs::Transform>(first)->rotationEulerRadians.y == stoppedRotation,
                "Edited boolean did not stop native sample animation");

        click(scripts, "Только 3D-витрина");
        click(scripts, "Открыть лабораторию");
        for (int page = 2; page <= 6; ++page) {
            click(scripts, "Следующая тема >");
            require(scripts.status().at("lab3Materials").value("page", 0) == page, "Native topic navigation skipped a page");
        }
        for (int page = 5; page >= 1; --page) click(scripts, "< Предыдущая тема");
        liveFields = world.getComponent<ScriptComponent>(controller);
        require(scripts.setField(*liveFields, "rounds", 4), "Cannot edit next order length");
        click(scripts, "Новый заказ");
        require(scripts.status().at("lab3Materials").value("goal", 0) == 4,
                "New order did not apply the edited rounds field");
        require(spawned(world).empty() && world.aliveCount() == 2, "New order did not safely destroy only its own native prefab entities");
        require(world.isAlive(preview) && world.isAlive(controller), "Demo cleanup destroyed an unrelated entity");

        scripts.stop();
        require(scripts.execute("assert(#__turboUIRoots == 0 and #__turboUIEvents == 0)", "=Lab3NativeCleanup"),
                "Native Stop retained roots/events");
        const auto captions = scripts.status().value("lab3SampleCaptions", Json::object());
        require(captions.empty(), "Native Stop retained a destroyed behavior's sample caption");
        scripts.start();
        tick(scripts);
        requireText(scripts, "Редактируемый спектр");
        require(scripts.status().at("lab3Materials").value("goal", 0) == 4,
                "Restarted demo did not apply serialized component fields");
        observeDemoDestroy(scripts);
        scripts.setDestroyCallback([&](const EntityId id) {
            destroyed.push_back(id);
            world.destroyEntity(id);
        });
        click(scripts, "Создать: Нефрит");
        click(scripts, "Создать: Аметист");
        click(scripts, "Создать: Янтарь");
        require(spawned(world).size() == 3 && world.aliveCount() == 5,
                "Live Stop cleanup fixture did not create three owned native samples");
        scripts.stop();
        require(spawned(world).empty() && destroyed.size() == 3 && world.aliveCount() == 2,
                "Stop failed to destroy exactly the demo-owned live prefab entities");
        require(world.isAlive(controller) && world.isAlive(preview), "Stop cleanup destroyed an unrelated authored entity");
        require(scripts.execute("assert(#__turboUIRoots == 0 and #__turboUIEvents == 0)", "=Lab3RepeatedCleanup"),
                "Repeated Play/Stop retained demo roots/events");
        scripts.stop();
        require(scripts.execute("assert(__lab3DestroyCount == 1)", "=Lab3StopDestroyOnce"),
                "Stop invoked the live controller Destroy callback more than once");

        destroyed.clear();
        scripts.start();
        tick(scripts);
        observeDemoDestroy(scripts);
        click(scripts, "Создать: Нефрит");
        require(spawned(world).size() == 1, "Live controller deletion fixture did not create its owned sample");
        require(scripts.execute("local matches=Engine.find('MaterialLabController'); assert(#matches==1); Engine.destroy(matches[1])", "=Lab3DeleteLiveController"),
                "Cannot delete the live demo controller through the real Engine binding");
        tick(scripts);
        require(!world.isAlive(controller) && world.isAlive(preview) && world.aliveCount() == 1
                && spawned(world).empty() && destroyed.size() == 2,
                "Deleting the live demo controller left orphan samples or removed an unrelated preview");
        require(scripts.execute("assert(#__turboUIRoots == 0 and #__turboUIEvents == 0)", "=Lab3ControllerDeleteUiCleanup"),
                "Live controller deletion did not release its UI roots/events");
        require(scripts.execute("assert(__lab3DestroyCount == 2)", "=Lab3ControllerDeleteDestroyOnce"),
                "Live controller deletion did not run its Destroy callback exactly once");
        scripts.stop();
        require(lua_gettop(scripts.state()) == originalStack, "Native demo callbacks leaked VM stack entries");
        for (const auto& message : scripts.output())
            require(message.kind != engine::scripting::ScriptMessage::Kind::Error, "Native demo emitted a script error");
        verifyPrefabFailure();
        verifyScriptSmoke();
        std::cout << "LAB3_NATIVE_DEMO_PASS real_world=1 real_prefab=1 material_ids=3 real_react_events=1 russian_ui=1 field_sync=1 caption_sync=1 repeated_cleanup=1 live_stop_cleanup=1 live_controller_delete=1 GPU_NOT_TESTED\n";
        return 0;
    } catch (const std::exception& error) {
        for (const auto& message : scripts.output()) std::cerr << message.text << '\n';
        std::cerr << "Lab3 demo failure: " << error.what() << '\n';
        return 1;
    }
}
