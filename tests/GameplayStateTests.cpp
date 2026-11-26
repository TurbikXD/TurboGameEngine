#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <entt/entity/entity.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <lua.h>
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/misc/cpp/imgui_stdlib.h"

#include "engine/core/JobSystem.h"
#include "engine/core/LabOptions.h"
#include "engine/core/NetworkTestControl.h"
#include "engine/core/NetworkTestCsv.h"
#include "engine/core/PlaySession.h"
#include "engine/ecs/components.h"
#include "engine/game/GameplayState.h"
#include "engine/game/SceneNetwork.h"
#include "engine/game/StateStack.h"
#include "engine/editor/EditorTheme.h"
#include "engine/editor/MaterialInspector.h"
#include "engine/network/NetworkSession.h"
#include "engine/platform/Input.h"
#include "engine/renderer/Renderer.h"
#include "engine/resources/material.h"
#include "engine/scripting/react/ReactUI.h"
#include "engine/scripting/react/ReactRuntime.h"
#include <thread>

namespace engine::game {

struct GameplayStateTestAccess {
    static void require(const bool condition, const char* message) {
        if (!condition) { throw std::runtime_error(message); }
    }

    static void liveScenesPreservePlay(core::JobSystem& jobs) {
        using Json = nlohmann::json;
        struct Fixture {
            std::filesystem::path root = std::filesystem::temp_directory_path() / ("tge-live-scene-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
        } fixture;
        std::filesystem::create_directory(fixture.root);
        core::ProjectManager projects(fixture.root / "recent.json");
        std::string error;
        auto project = projects.createBlank(fixture.root, "Live", error);
        require(project.has_value(), "Live project fixture failed");
        const auto write = [](const std::filesystem::path& path, const std::string& content) {
            std::ofstream file(path, std::ios::binary); file << content; require(static_cast<bool>(file), "Live fixture write failed");
        };
        write(project->assetRoot / "scripts/Counter.luau", "return {Init=function(self) self.state.count=12 end, Update=function(self) self.state.count+=1 end}");
        const Json initial = {{"version",1}, {"cameraEntity",100}, {"entities", Json::array({
            {{"sourceId",100}, {"tag",{{"value","Camera"}}}, {"transform",{{"position",{0,2,5}}}}, {"camera",Json::object()}},
            {{"sourceId",200}, {"tag",{{"value","Counter"}}}, {"script",{{"path","scripts/Counter.luau"},{"fields",{{"speed",1}}}}}},
            {{"sourceId",300}, {"tag",{{"value","Body"}}}, {"transform",{{"position",{1,2,3}}}}, {"rigidbody",{{"mass",1}}}},
            {{"sourceId",400}, {"tag",{{"value","Remove"}}}, {"transform",Json::object()}}
        })}};
        for (const bool standalone : {false, true}) {
            write(project->assetRoot / "scripts/Counter.luau", "return {Init=function(self) self.state.count=12 end, Update=function(self) self.state.count+=1 end}");
            write(project->scenePath, initial.dump());
            core::Project launch = *project;
            if (standalone) {
                launch.liveScenePath = project->scenePath;
                launch.scenePath = fixture.root / "session.json";
                auto copy = initial;
                copy["cameraEntity"] = 1000;
                copy["liveSourceMap"] = Json::object();
                for (auto& entity : copy["entities"]) {
                    const auto source = entity["sourceId"].get<unsigned>();
                    entity["sourceId"] = source * 10;
                    copy["liveSourceMap"][std::to_string(source)] = source * 10;
                }
                write(launch.scenePath, copy.dump());
            }
            StateStack stack; stack.setServices(&jobs, nullptr); stack.setProject(&launch); stack.setStandalone(standalone);
            GameplayState state(stack); state.onEnter();
            if (!standalone) state.enterPlayMode();
            state.m_simulationRunning = false;
            const auto counter = tagged(state,"Counter"), body = tagged(state,"Body"), removed = tagged(state,"Remove");
            require(counter != ecs::kInvalidEntity && body != ecs::kInvalidEntity, "Live scene did not load");
            state.m_world.getComponent<ecs::Transform>(body)->position.x = 42;
            auto changed = initial;
            changed["entities"][1]["script"]["fields"]["speed"] = 9;
            changed["entities"][2]["rigidbody"]["mass"] = 7;
            changed["entities"].erase(3);
            changed["entities"].push_back({{"sourceId",500},{"tag",{{"value","LiveAdded"}}},{"transform",{{"position",{5,6,7}}}},{"hierarchy",{{"parent",300}}}});
            write(project->scenePath, changed.dump());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (tagged(state,"LiveAdded") == ecs::kInvalidEntity && std::chrono::steady_clock::now() < deadline) {
                state.update(1.0/60.0); jobs.waitAll(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto added = tagged(state,"LiveAdded");
            require(added != ecs::kInvalidEntity && !state.m_world.isAlive(removed), "Live scene create/delete failed");
            require(state.m_world.getComponent<ecs::Hierarchy>(added)->parent == body, "Live hierarchy source ID was not remapped");
            require(state.m_world.getComponent<ecs::Transform>(body)->position.x == 42, "Live scene reset simulated position");
            require(state.m_world.getComponent<ecs::Rigidbody>(body)->mass == 7, "Live scene did not apply changed component");
            require(state.m_world.getComponent<scripting::ScriptComponent>(counter)->fields["speed"] == 9, "Live script fields unchanged");
            require(state.m_scripts.inspect(counter)["state"]["count"] == 12 && state.m_scripts.playing() && !state.m_simulationRunning, "Live scene restarted or unpaused game");
            require(state.m_scriptWorkspace.openScript("scripts/Counter.luau"), "Live document failed to open");
            write(project->assetRoot / "scripts/Counter.luau", "-- external live code\nreturn {Init=function(self) self.state.count=99 end, Update=function(self) self.state.count+=1 end}");
            const auto documentDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (state.m_scriptWorkspace.assistantContext().find("external live code") == std::string::npos && std::chrono::steady_clock::now() < documentDeadline) {
                state.update(0.016); jobs.waitAll(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(state.m_scriptWorkspace.assistantContext().find("external live code") != std::string::npos, "Editor document did not show AI changes live");
            require(state.m_scripts.inspect(counter)["state"]["count"] == 12, "Live document update reran Init");
            write(project->scenePath, "{invalid");
            for (int i=0;i<45;++i) { state.update(0.016); jobs.waitAll(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            require(state.m_world.isAlive(added), "Invalid live JSON discarded the scene");
            state.stopPlayMode();
            require(tagged(state,"LiveAdded") == ecs::kInvalidEntity && tagged(state,"Remove") != ecs::kInvalidEntity, "Stop did not restore the pre-Play scene");
            require(state.m_world.getComponent<ecs::Transform>(tagged(state,"Body"))->position.x == 1, "Stop kept transient live positions");
            stack.setStandalone(true); state.onExit();
        }
    }

    static void authoredSceneIdsSurviveHistoryAndLiveSave(core::JobSystem& jobs) {
        using Json = nlohmann::json;
        namespace fs = std::filesystem;
        struct Fixture {
            fs::path root = fs::temp_directory_path() / ("tge-stable-scene-ids-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
        } fixture;
        core::ProjectManager projects(fixture.root / "registry.json");
        std::string error;
        const auto project = projects.createBlank(fixture.root, "Stable", error);
        require(project.has_value(), "Stable scene project fixture failed");
        const auto write = [](const fs::path& path, const std::string& text) { std::ofstream out(path, std::ios::binary); out << text; require(static_cast<bool>(out), "Stable scene fixture write failed"); };
        write(project->assetRoot / "scripts/Counter.luau", "return {fields={rate=1},Init=function(self) self.state.count=12 end,Update=function(self) self.state.count+=1 end}");
        const Json initial = {{"version",1},{"cameraEntity",100},{"selectedEntity",200},{"entities",Json::array({
            {{"sourceId",100},{"tag",{{"value","Camera"}}},{"transform",{{"position",{0,2,5}}}},{"camera",Json::object()}},
            {{"sourceId",200},{"tag",{{"value","Counter"}}},{"transform",Json::object()},{"script",{{"path","scripts/Counter.luau"},{"fields",{{"rate",1}}}}}},
            {{"sourceId",300},{"tag",{{"value","Child"}}},{"transform",{{"position",{1,2,3}}}},{"hierarchy",{{"parent",200}}}}
        })}};
        write(project->scenePath, initial.dump());
        StateStack editorStack; editorStack.setServices(&jobs,nullptr); editorStack.setProject(&*project);
        GameplayState editor(editorStack); editor.onEnter();
        editor.enterPlayMode(); editor.stopPlayMode();
        const auto sessionPath = fixture.root / "session.json";
        write(sessionPath, editor.serializeScene(editor.captureSceneSnapshot(), true));
        core::Project launch = *project; launch.scenePath = sessionPath; launch.liveScenePath = project->scenePath;
        StateStack childStack; childStack.setServices(&jobs,nullptr); childStack.setProject(&launch); childStack.setStandalone(true);
        GameplayState child(childStack); child.onEnter(); child.m_simulationRunning = false;
        const auto runningCounter = tagged(child,"Counter"), runningChild = tagged(child,"Child");
        child.m_world.getComponent<ecs::Transform>(runningCounter)->position.x = 42;
        child.m_scripts.update(0.01);
        const auto count = child.m_scripts.inspect(runningCounter).at("state").at("count");
        editor.recordSceneHistory("Edit counter");
        editor.m_world.getComponent<scripting::ScriptComponent>(tagged(editor,"Counter"))->fields["rate"] = 3;
        editor.undoSceneEdit(); editor.redoSceneEdit();
        require(editor.saveSceneToDisk(), "Stable scene save failed");
        Json saved; { std::ifstream file(project->scenePath); file >> saved; }
        const auto byTag = [](const Json& scene, const std::string& tag) {
            for (const auto& entity : scene.at("entities")) if (entity.at("tag").at("value")==tag) return entity;
            throw std::runtime_error("Stable scene tag missing");
        };
        require(byTag(saved,"Camera").at("sourceId")==100 && byTag(saved,"Counter").at("sourceId")==200 && byTag(saved,"Child").at("sourceId")==300,
            "Saving after Stop/Undo renumbered authored scene IDs");
        require(saved.at("cameraEntity")==100 && saved.at("selectedEntity")==200 && byTag(saved,"Child").at("hierarchy").at("parent")==200,
            "Stable scene references did not use authored IDs");
        child.applyLiveScene(saved);
        require(tagged(child,"Counter")==runningCounter && tagged(child,"Child")==runningChild,
            "Authoring save replaced running entities");
        require(child.m_scripts.inspect(runningCounter).at("state").at("count")==count &&
            child.m_world.getComponent<ecs::Transform>(runningCounter)->position.x==42 &&
            child.m_world.getComponent<scripting::ScriptComponent>(runningCounter)->fields.at("rate")==3,
            "Authoring save lost runtime state or did not apply the field delta");
        const auto added = editor.createEditorEntity(GameplayState::EditorEntityKind::Empty);
        editor.m_world.getComponent<ecs::Tag>(added)->value = "New";
        editor.m_fileEntities.erase(200);
        editor.m_fileEntities.emplace(added, tagged(editor,"Counter"));
        const auto collision = Json::parse(editor.serializeScene(editor.captureSceneSnapshot()));
        std::unordered_set<ecs::EntityId> ids;
        for (const auto& entity : collision.at("entities")) require(ids.insert(entity.at("sourceId").get<ecs::EntityId>()).second, "A new entity reused an authored scene ID");
        require(byTag(collision,"Counter").at("sourceId")==added && byTag(collision,"New").at("sourceId")!=added,
            "New entity did not avoid an authored source ID collision");
        child.onExit(); editor.onExit();
    }

    static void projectsHaveIndependentScenesAndScripts(core::JobSystem& jobs) {
        struct Fixture final {
            std::filesystem::path root;
            ~Fixture() {
                if (!root.empty() && root.parent_path() == std::filesystem::temp_directory_path() &&
                    root.filename().string().starts_with("tge-gameplay-projects-")) {
                    std::error_code error;
                    std::filesystem::remove_all(root, error);
                }
            }
        } fixture;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() / ("tge-gameplay-projects-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) { fixture.root = candidate; break; }
        }
        require(!fixture.root.empty(), "Cannot create project integration fixture");
        core::ProjectManager projects(fixture.root / "recent.json");
        std::string error;
        auto first = projects.createBlank(fixture.root / "projects", "Alpha", error);
        auto second = projects.createBlank(fixture.root / "projects", "Beta", error);
        require(first.has_value() && second.has_value(), "Cannot create independent Blank projects");
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        const auto edit = [&](const core::Project& project, const char* name, const float x, const float panelWidth) {
            stack.setProject(&project);
            GameplayState state(stack);
            state.onEnter();
            require(state.m_world.aliveCount() == 1U && state.m_world.hasComponent<ecs::Camera>(state.m_cameraEntity),
                    "Blank project must contain only an internal editor camera");
            require(state.m_undoStack.empty() && state.editorAssetsRoot() == project.assetRoot &&
                    state.editorScenePath() == project.scenePath && state.editorLayoutPath() == project.layoutPath,
                    "Project paths or initial scene history are not isolated");
            require(state.m_scriptWorkspace.assistantContext().empty(), "New project retained old script tabs");
            const auto entity = state.createEditorEntity(GameplayState::EditorEntityKind::Empty);
            state.m_world.getComponent<ecs::Tag>(entity)->value = name;
            state.m_world.getComponent<ecs::Transform>(entity)->position.x = x;
            {
                std::ofstream script(project.assetRoot / "scripts/SameName.luau");
                script << "return {Init=function(self) self.state.origin='" << name << "' end}";
            }
            scripting::ScriptComponent component;
            component.path = "scripts/SameName.luau";
            state.m_world.addComponent<scripting::ScriptComponent>(entity, component);
            require(state.m_scriptWorkspace.openScript(component.path), "Project-local script did not open");
            state.enterPlayMode();
            state.update(1.0 / 60.0);
            require(state.m_scripts.inspect(tagged(state, name))["state"]["origin"] == name,
                    "Another project's behavior module was reused");
            state.stopPlayMode();
            require(state.saveSceneToDisk(), "Could not save project-local scene");
            state.m_editorLeftWidth = panelWidth;
            state.onExit();
        };
        edit(*first, "AlphaEntity", 12.0F, 330.0F);
        edit(*second, "BetaEntity", -7.0F, 370.0F);
        stack.setProject(&*first);
        GameplayState reopened(stack);
        reopened.onEnter();
        require(reopened.m_world.aliveCount() == 2U && tagged(reopened, "BetaEntity") == ecs::kInvalidEntity,
                "Reopening Alpha loaded Beta's scene");
        require(reopened.m_world.getComponent<ecs::Transform>(tagged(reopened, "AlphaEntity"))->position.x == 12.0F &&
                reopened.m_editorLeftWidth == 330.0F, "Scene or editor preferences did not persist per project");
        reopened.enterPlayMode();
        reopened.update(1.0 / 60.0);
        require(reopened.m_scripts.inspect(tagged(reopened, "AlphaEntity"))["state"]["origin"] == "AlphaEntity",
                "Reopened project did not reload its own scripts");
        reopened.stopPlayMode();
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        const auto secondScene = read(second->scenePath);
        const auto secondLayout = read(second->layoutPath);
        std::filesystem::remove(first->scenePath);
        std::filesystem::create_hard_link(second->scenePath, first->scenePath);
        require(reopened.saveSceneToDisk() && read(second->scenePath) == secondScene,
                "Saving a hardlinked scene overwrote another project");
        std::filesystem::remove(first->layoutPath);
        std::filesystem::create_hard_link(second->layoutPath, first->layoutPath);
        reopened.m_editorLeftWidth = 410.0F;
        reopened.saveEditorLayout();
        require(read(second->layoutPath) == secondLayout,
                "Saving hardlinked editor preferences overwrote another project");
        reopened.onExit();
        stack.setProject(nullptr);
    }

    static void scriptSaveDoesNotFollowExistingSidecars() {
        struct Fixture final {
            std::filesystem::path root;
            ~Fixture() {
                std::error_code error;
                if (root.empty()) { return; }
                for (const auto* relative : {"assets/scripts/Save.luau.tge-save", "assets/scripts/Link.luau.tge-save",
                        "assets/scripts/Save.luau", "assets/scripts/Link.luau", "outside-assets.txt",
                        "assets/scripts", "assets"}) { std::filesystem::remove(root / relative, error); }
                std::filesystem::remove(root, error);
            }
        } fixture;
        const auto tempRoot = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = tempRoot / ("tge-script-save-test-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) { fixture.root = candidate; break; }
        }
        require(!fixture.root.empty(), "Cannot create script save regression fixture");
        std::filesystem::create_directories(fixture.root / "assets/scripts");
        const auto write = [](const std::filesystem::path& path, const std::string& text) {
            std::ofstream file(path, std::ios::binary);
            file << text;
            if (!file) { throw std::runtime_error("Cannot write script save fixture"); }
        };
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        const std::string original = "return { Update = function(self, dt) end }\n";
        const std::string protectedText = "External file must stay unchanged.\n";
        const auto guard = fixture.root / "outside-assets.txt";
        const auto script = fixture.root / "assets/scripts/Save.luau";
        const auto sidecar = fixture.root / "assets/scripts/Save.luau.tge-save";
        write(guard, protectedText);
        write(script, original);
        std::filesystem::create_hard_link(guard, sidecar);

        scripting::ScriptingSystem scripts;
        editor::ScriptWorkspace workspace;
        workspace.initialize(fixture.root / "assets", scripts, {});
        require(workspace.openScript("scripts/Save.luau"), "Could not open script save fixture");
        require(workspace.saveActive(), "Safe save should succeed with an existing hardlinked legacy sidecar");
        require(read(guard) == protectedText && read(sidecar) == protectedText && std::filesystem::equivalent(guard, sidecar),
                "Script Save overwrote or consumed an existing hardlinked sidecar");
        require(read(script) == original, "Atomic Save did not preserve the document content");
        require(workspace.saveActive(), "Repeated atomic Save should succeed");

        const auto linkedScript = fixture.root / "assets/scripts/Link.luau";
        const auto linkedSidecar = fixture.root / "assets/scripts/Link.luau.tge-save";
        write(linkedScript, original);
        std::error_code symlinkError;
        std::filesystem::create_symlink(guard, linkedSidecar, symlinkError);
        if (!symlinkError) {
            require(workspace.openScript("assets/scripts/Link.luau") && workspace.saveActive(), "Safe save with a symlinked legacy sidecar failed");
            require(read(guard) == protectedText && std::filesystem::is_symlink(linkedSidecar),
                    "Script Save overwrote or consumed an existing symbolic sidecar");
        }
        for (const auto& entry : std::filesystem::directory_iterator(fixture.root / "assets/scripts")) {
            require(entry.path().filename().string().find(".tge-save-") == std::string::npos,
                    "Atomic Save left its exclusively created temporary file behind");
        }
    }

    static ecs::EntityId addBody(GameplayState& state, const char* name, const glm::vec3 position) {
        const auto entity = state.m_world.createEntity();
        ecs::Transform transform;
        transform.position = position;
        state.m_world.addComponent<ecs::Transform>(entity, transform);
        state.m_world.addComponent<ecs::Tag>(entity, ecs::Tag{name});
        ecs::Rigidbody body;
        body.useGravity = false;
        state.m_world.addComponent<ecs::Rigidbody>(entity, body);
        state.m_world.addComponent<ecs::Collider>(entity);
        return entity;
    }

    static ecs::EntityId tagged(const GameplayState& state, const char* name) {
        ecs::EntityId found = ecs::kInvalidEntity;
        state.m_world.forEach<ecs::Tag>([&](const auto entity, const auto& tag) {
            if (tag.value == name) { found = entity; }
        });
        return found;
    }

    static void sceneSelectionIsIndependentOfTracy(core::JobSystem& jobs) {
        core::LabOptions options;
        StateStack stack;
        stack.setServices(&jobs, &options);
        {
            GameplayState state(stack);
            state.onEnter();
            require(state.m_world.aliveCount() == 21U, "Tracy/default launch added stress bodies");
            require(state.m_editorMode == GameplayState::EditorMode::Edit && !state.m_simulationRunning,
                    "Default launch automatically entered Play");
        }
        options.stressScene = true;
        {
            GameplayState state(stack);
            state.onEnter();
            require(state.m_world.aliveCount() == 1301U, "Explicit stress scene is missing its 1280 bodies");
            require(state.m_editorMode == GameplayState::EditorMode::Play && state.m_simulationRunning,
                    "Explicit stress scene did not start simulation");
        }
        options.scene = "ecs";
        options.entityCount = 64U;
        {
            GameplayState state(stack);
            state.onEnter();
            const auto firstCount = state.m_world.aliveCount();
            require(state.m_labEntities.size() == 64U && state.m_editorMode == GameplayState::EditorMode::Edit,
                    "Stress option changed the existing ECS lab workload/mode");
            options.stressScene = false;
            state.resetDemoScene();
            require(state.m_world.aliveCount() == firstCount, "Stress option changed lab entity count");
        }
    }

    static void runtimeDeleteAndRestore(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        const auto parent = addBody(state, "parent", {2.0F, 0.0F, 0.0F});
        const auto child = addBody(state, "child", {6.0F, 0.0F, 0.0F});
        state.m_world.addComponent<ecs::Hierarchy>(child, ecs::Hierarchy{parent});
        state.m_world.addComponent<ecs::Camera>(parent);
        state.m_selectedEntity = state.m_hoveredEntity = parent;
        state.m_cameraEntity = state.m_strikerEntity = state.m_showcaseSphereEntity = parent;
        state.enterPlayMode();
        state.m_simulationRunning = false;
        state.m_gizmoWasUsing = state.m_gizmoChanged = true;

        platform::Event key;
        key.type = platform::EventType::KeyPressed;
        key.key = platform::KeyCode::Delete;
        ImGui::GetIO().WantTextInput = true;
        state.handleEvent(key);
        require(state.m_pendingRuntimeDeletes.empty(), "Delete in text input queued an entity deletion");
        ImGui::GetIO().WantTextInput = false;
        key.repeat = true;
        state.handleEvent(key);
        require(state.m_pendingRuntimeDeletes.empty(), "Repeated key queued a deletion");
        key.repeat = false;
        state.handleEvent(key);
        state.deleteEntity(parent);
        require(state.m_pendingRuntimeDeletes.size() == 1, "Duplicate requests were not coalesced");
        require(state.m_world.isAlive(parent), "UI deleted entity before the safe update boundary");
        state.update(1.0 / 60.0);
        require(!state.m_world.isAlive(parent), "Runtime deletion was not applied");
        require(state.m_world.getComponent<ecs::Rigidbody>(parent) == nullptr, "Components survived deletion");
        require(state.m_world.isAlive(child), "Deleting a parent deleted its child");
        require(state.m_world.getComponent<ecs::Hierarchy>(child)->parent == ecs::kInvalidEntity,
                "Child kept a dangling parent ID");
        require(state.m_hoveredEntity == ecs::kInvalidEntity && state.m_selectedEntity != parent &&
                state.m_cameraEntity == ecs::kInvalidEntity && state.m_strikerEntity == ecs::kInvalidEntity &&
                state.m_showcaseSphereEntity == ecs::kInvalidEntity, "Entity references survived deletion");
        require(!state.m_gizmoWasUsing && !state.m_gizmoChanged, "Deleted selection left an active gizmo");
        require(state.m_undoStack.size() == 1U && state.m_editUndoStack.empty(), "Runtime deletion must have its own Play history");
        state.deleteEntity(parent);
        state.deleteEntity(ecs::kInvalidEntity);
        state.stopPlayMode();
        const auto restoredParent = tagged(state, "parent");
        const auto restoredChild = tagged(state, "child");
        require(state.m_world.isAlive(restoredParent) && state.m_world.isAlive(restoredChild), "Stop lost snapshot entities");
        require(state.m_world.getComponent<ecs::Hierarchy>(restoredChild)->parent == restoredParent,
                "Stop did not restore parent relationship");
        require(state.m_world.getComponent<ecs::Transform>(restoredParent)->position.x == 2.0F,
                "Stop did not restore the pre-Play transform");

        state.enterPlayMode();
        state.deleteEntity(restoredParent);
        state.stopPlayMode();
        state.update(1.0 / 60.0);
        require(state.m_world.isAlive(tagged(state, "parent")), "Pending Play delete targeted the restored Edit scene");
        state.deleteEntity(tagged(state, "parent"));
        require(!state.m_world.isAlive(tagged(state, "parent")) && state.hasUndo(), "Edit Delete/Undo regressed");
        state.undoSceneEdit();
        require(state.m_world.isAlive(tagged(state, "parent")), "Undo did not restore an Edit deletion");
        state.redoSceneEdit();
        require(!state.m_world.isAlive(tagged(state, "parent")), "Redo did not reapply an Edit deletion");
    }

    static void liveDeletionCleansPausedPhysics(core::JobSystem& jobs) {
        using Json = nlohmann::json;
        StateStack stack; stack.setServices(&jobs, nullptr);
        GameplayState state(stack); state.m_sceneInitialized = true;
        require(state.m_scripts.initialize(state.m_world, &jobs, std::filesystem::temp_directory_path()),
            "Live-delete fixture VM initialization failed");
        state.m_scripts.registerModule("LiveDeleteCleanup", "return {Destroy=function(self) Engine.setStatus('cleanup',(Engine.getStatus('cleanup') or 0)+1) end}");
        state.bindCollisionEventHandlers();
        const auto parent = addBody(state, "Live parent", {0, 0, 0});
        addBody(state, "Live neighbor", {0.4F, 0, 0});
        state.m_world.addComponent<ecs::Camera>(parent);
        state.m_world.addComponent<scripting::ScriptComponent>(parent, scripting::ScriptComponent{"LiveDeleteCleanup"});
        const auto child = state.m_world.createEntity();
        state.m_world.addComponent<ecs::Tag>(child, ecs::Tag{"Live child"});
        state.m_world.addComponent<ecs::Transform>(child);
        state.m_world.addComponent<ecs::Hierarchy>(child, ecs::Hierarchy{parent});
        state.m_selectedEntity = state.m_hoveredEntity = parent;
        state.m_cameraEntity = state.m_strikerEntity = state.m_showcaseSphereEntity = parent;
        const auto snapshot = state.captureSceneSnapshot();
        state.m_fileEntities = snapshot.fileEntities;
        state.m_diskScene = Json::parse(state.serializeScene(snapshot));
        auto next = state.m_diskScene;
        auto& entities = next["entities"];
        std::erase_if(entities.get_ref<Json::array_t&>(), [&](const Json& entity) { return entity["sourceId"] == parent; });
        for (auto& entity : entities) if (entity["sourceId"] == child) entity["transform"]["position"] = {5, 0, 0};
        std::size_t exits = 0;
        state.m_eventBus.subscribe<ecs::CollisionExitEvent>([&](const auto&) { ++exits; });
        state.enterPlayMode(); state.update(1.0 / 60);
        require(state.m_physicsSystem.activeCollisionCount() == 1, "Live-delete fixture did not generate its contact");
        state.m_simulationRunning = false;
        state.applyLiveScene(next);
        require(!state.m_world.isAlive(parent) && state.m_physicsSystem.activeCollisionCount() == 0 && exits == 1,
            "Live deletion retained a paused contact or omitted its exit event");
        require(state.m_scripts.status().value("cleanup", 0) == 1 &&
            state.m_world.getComponent<ecs::Hierarchy>(child)->parent == ecs::kInvalidEntity &&
            state.m_world.getComponent<ecs::Transform>(child)->position.x == 5,
            "Live deletion skipped behavior cleanup or restored a dangling parent from the child edit");
        require(state.m_selectedEntity != parent && state.m_hoveredEntity != parent && state.m_cameraEntity != parent &&
            state.m_strikerEntity != parent && state.m_showcaseSphereEntity != parent && state.m_undoStack.size() == 1,
            "Live deletion retained editor references or split its batch history");
        const auto replacement = addBody(state, "Live replacement", {30, 0, 0});
        require(replacement != parent && !state.m_world.isAlive(parent), "Live deletion reused a stale handle");
        state.update(0);
        require(state.m_world.isAlive(replacement) && exits == 1, "Paused update replayed a stale contact/deletion");
        state.stopPlayMode();
        require(state.m_world.isAlive(tagged(state, "Live parent")) &&
            state.m_world.getComponent<ecs::Hierarchy>(tagged(state, "Live child"))->parent == tagged(state, "Live parent"),
            "Stop lost the pre-live-delete authoring hierarchy");
    }

    static void collisionEventsRejectWorkerDispatch(core::JobSystem& jobs) {
        StateStack stack; stack.setServices(&jobs, nullptr);
        GameplayState state(stack); state.m_sceneInitialized = true;
        require(state.m_scripts.initialize(state.m_world, &jobs, std::filesystem::temp_directory_path()),
            "Worker event fixture VM initialization failed");
        state.bindCollisionEventHandlers();
        bool publishRejected = false, subscribeRejected = false, clearRejected = false;
        std::atomic_bool done{false};
        const auto handle = jobs.dispatch([&] {
            try { state.m_eventBus.publish(ecs::CollisionEnterEvent{}); } catch (const std::logic_error&) { publishRejected = true; }
            try { state.m_eventBus.subscribe<ecs::CollisionEnterEvent>([](const auto&) {}); } catch (const std::logic_error&) { subscribeRejected = true; }
            try { state.m_eventBus.clear(); } catch (const std::logic_error&) { clearRejected = true; }
            done.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done.load(std::memory_order_acquire)) {
            require(std::chrono::steady_clock::now() < deadline, "Worker collision dispatch fixture timed out");
            std::this_thread::yield();
        }
        jobs.wait(handle);
        require(publishRejected && subscribeRejected && clearRejected && state.m_collisionEnterCount == 0,
            "Worker EventBus access reached owner-only collision handlers or mutated the registry");
        state.m_eventBus.publish(ecs::CollisionEnterEvent{});
        require(state.m_collisionEnterCount == 1, "Rejected worker dispatch removed owner collision handlers");
    }

    static void parallelPhysicsAndRecycledIds(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        auto settings = state.m_physicsSystem.settings();
        settings.gravity = glm::vec3(0.0F);
        state.m_physicsSystem.setSettings(settings);
        const auto first = addBody(state, "first", {0.0F, 0.0F, 0.0F});
        addBody(state, "second", {0.4F, 0.0F, 0.0F});
        for (int i = 0; i < 256; ++i) {
            addBody(state, "remote", {20.0F + static_cast<float>(i) * 3.0F, 0.0F, 0.0F});
        }
        state.enterPlayMode();
        std::size_t exits = 0;
        state.m_eventBus.subscribe<ecs::CollisionExitEvent>([&](const auto&) { ++exits; });
        state.update(1.0 / 60.0);
        require(state.m_physicsSystem.activeCollisionCount() != 0, "Fixture generated no contacts");
        state.m_simulationRunning = false;
        state.deleteEntity(first);
        state.update(1.0 / 60.0);
        require(state.m_physicsSystem.activeCollisionCount() == 0 && exits == 1,
                "Runtime deletion did not remove contacts/emit one exit while paused");
        const auto replacement = addBody(state, "replacement", {-40.0F, 0.0F, 0.0F});
        require(replacement != first &&
                entt::to_entity(static_cast<entt::entity>(replacement - 1U)) ==
                entt::to_entity(static_cast<entt::entity>(first - 1U)),
                "Fixture did not reuse an entity slot with a distinct generation");
        state.deleteEntity(first);
        require(state.m_pendingRuntimeDeletes.empty(), "Stale handle queued a replacement deletion");
        state.m_simulationRunning = true;
        for (int i = 0; i < 6; ++i) { state.update(1.0 / 60.0); }
        require(state.m_world.isAlive(replacement) && exits == 1, "Stale deletion/contact affected recycled ID");
        state.deleteEntity(replacement);
        state.resetDemoScene();
        state.update(0.0);
        require(state.m_pendingRuntimeDeletes.empty(), "Reset retained a pending runtime deletion");
    }
    static void inspectorHistory(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        state.m_selectedEntity = addBody(state, "edited", {1.0F, 2.0F, 3.0F});
        const auto other = addBody(state, "other", {8.0F, 0.0F, 0.0F});
        state.m_world.addComponent<ecs::Hierarchy>(other, ecs::Hierarchy{state.m_selectedEntity});

        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800.0F, 600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        ImVec2 namePoint{}, massLeft{}, massRight{}, gravityPoint{};
        const ImVec2 away(600.0F, 500.0F);
        const auto frame = [&](const ImVec2 mouse, const bool down, const char* input = nullptr) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            if (input != nullptr) { io.AddInputCharactersUTF8(input); }
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(10.0F, 10.0F));
            ImGui::SetNextWindowSize(ImVec2(480.0F, 360.0F));
            ImGui::Begin("Inspector History Test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const auto entity = state.m_selectedEntity;
            auto* tag = state.m_world.getComponent<ecs::Tag>(entity);
            auto* body = state.m_world.getComponent<ecs::Rigidbody>(entity);

            auto before = state.captureEntitySnapshot(entity);
            ImGui::SetNextItemWidth(300.0F);
            bool changed = ImGui::InputText("Name", &tag->value);
            state.trackEditedItem("Rename Entity", changed, before);
            const auto nameMin = ImGui::GetItemRectMin();
            const auto nameMax = ImGui::GetItemRectMax();
            namePoint = ImVec2(nameMin.x + 30.0F, (nameMin.y + nameMax.y) * 0.5F);

            before = state.captureEntitySnapshot(entity);
            ImGui::SetNextItemWidth(300.0F);
            changed = ImGui::SliderFloat("Mass", &body->mass, 0.1F, 10.0F);
            state.trackEditedItem("Edit Mass", changed, before);
            if (changed) { body->recalculateMassProperties(); body->wakeUp(); }
            const auto massMin = ImGui::GetItemRectMin();
            const auto massMax = ImGui::GetItemRectMax();
            massLeft = ImVec2(massMin.x + 105.0F, (massMin.y + massMax.y) * 0.5F);
            massRight = ImVec2(massMin.x + 210.0F, (massMin.y + massMax.y) * 0.5F);

            before = state.captureEntitySnapshot(entity);
            changed = ImGui::Checkbox("Use Gravity", &body->useGravity);
            state.trackEditedItem("Edit Gravity", changed, before);
            const auto gravityMin = ImGui::GetItemRectMin();
            const auto gravityMax = ImGui::GetItemRectMax();
            gravityPoint = ImVec2(gravityMin.x + 8.0F, (gravityMin.y + gravityMax.y) * 0.5F);
            ImGui::End();
            ImGui::Render();
        };

        for (int i = 0; i < 30; ++i) { frame(away, false); }
        require(state.m_undoStack.empty() && !state.m_pendingSceneEdit.has_value(),
                "Idle Inspector fields created history or an active edit");
        frame(massRight, true);
        require(state.m_pendingSceneEdit.has_value() && state.m_pendingSceneEdit->changed && state.m_undoStack.empty(),
                "Slider activation did not begin a single pending edit");
        const auto* dragSnapshot = state.m_pendingSceneEdit->before.entities.data();
        for (int i = 0; i < 5; ++i) { frame(massLeft, true); }
        require(state.m_pendingSceneEdit->before.entities.data() == dragSnapshot && state.m_undoStack.empty(),
                "Holding a slider replaced its initial snapshot or split its history");
        const float editedMass = state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->mass;
        require(editedMass != 1.0F, "Slider fixture did not change mass");
        frame(massLeft, false);
        require(state.m_undoStack.size() == 1U && !state.m_pendingSceneEdit.has_value(),
                "Slider release did not commit exactly one history entry");
        state.undoSceneEdit();
        require(state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->mass == 1.0F &&
                state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->inverseMass == 1.0F &&
                state.m_world.getComponent<ecs::Hierarchy>(tagged(state, "other"))->parent == state.m_selectedEntity,
                "Lazy Undo lost pre-widget values, mass properties or another entity's hierarchy");
        state.redoSceneEdit();
        require(state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->mass == editedMass &&
                state.m_world.getComponent<ecs::Transform>(tagged(state, "other"))->position.x == 8.0F,
                "Redo did not restore the final drag or changed another entity");

        frame(namePoint, true);
        frame(namePoint, false);
        require(state.m_pendingSceneEdit.has_value() && !state.m_pendingSceneEdit->changed,
                "Focusing text without editing did not preserve its initial value");
        frame(away, true);
        frame(away, false);
        require(state.m_undoStack.size() == 1U && !state.m_pendingSceneEdit.has_value(),
                "Focus without edits left history or a stale pending edit");

        frame(namePoint, true);
        frame(namePoint, false);
        frame(namePoint, false, "Z");
        const auto renamed = state.m_world.getComponent<ecs::Tag>(state.m_selectedEntity)->value;
        require(renamed != "edited", "InputText fixture did not rename the entity");
        frame(gravityPoint, true);
        frame(gravityPoint, false);
        require(state.m_undoStack.size() == 3U && !state.m_pendingSceneEdit.has_value() &&
                state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->useGravity,
                "Rename and checkbox were not independent history entries");
        state.undoSceneEdit();
        require(state.m_world.getComponent<ecs::Tag>(state.m_selectedEntity)->value == renamed &&
                !state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->useGravity,
                "Undoing a checkbox also undid the previous field");
        state.undoSceneEdit();
        require(state.m_world.getComponent<ecs::Tag>(state.m_selectedEntity)->value == "edited" &&
                state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->mass == editedMass,
                "Undoing text lost its pre-edit string or changed the previous drag");
        state.redoSceneEdit();
        state.redoSceneEdit();
        require(state.m_world.getComponent<ecs::Tag>(state.m_selectedEntity)->value == renamed &&
                state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->useGravity,
                "Redo lost the text or discrete field value");

        const auto historyCount = state.m_undoStack.size();
        state.enterPlayMode();
        frame(gravityPoint, true);
        frame(gravityPoint, false);
        require(state.m_editUndoStack.size() == historyCount && state.m_undoStack.size() == 1U && !state.m_pendingSceneEdit.has_value(),
                "Play Inspector edits must use isolated runtime history");
        state.stopPlayMode();
        require(state.m_undoStack.size() == historyCount && state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->useGravity,
                "Stop must restore editor properties and history");
    }

    static void editablePlayRestoresSceneAndHistory(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        state.createEditorCamera();
        const auto body = addBody(state, "Editable", {2.0F, 0.0F, 0.0F});
        state.m_world.getComponent<ecs::Rigidbody>(body)->useGravity = false;
        state.m_selectedEntity = body;
        require(state.m_scripts.initialize(state.m_world, &jobs, state.editorAssetsRoot()), "Live-edit VM failed to initialize");
        scripting::ReactRuntime::install(state.m_scripts);
        state.m_scripts.registerModule("LiveA", R"(
            return {fields={speed={type='number',default=1}},
                Init=function(self) self.state.kind='A' end,
                Update=function(self) self.state.speed=self.fields.speed end}
        )");
        state.m_scripts.registerModule("LiveB", "return {Init=function(self) self.state.kind='B' end}");
        scripting::ScriptComponent script;
        script.path = "LiveA"; script.fields = {{"speed", 1}};
        state.m_world.addComponent<scripting::ScriptComponent>(body, script);
        state.recordSceneHistory("Before Play");
        const auto editorHistory = state.m_undoStack.size();
        const auto count = state.m_world.aliveCount();
        state.enterPlayMode();
        require(state.canEditScene() && state.m_undoStack.empty(), "Play must allow live editing with independent history");
        state.recordSceneHistory("Live Properties");
        state.m_world.getComponent<ecs::Rigidbody>(body)->mass = 4.0F;
        state.m_world.getComponent<ecs::Transform>(body)->position.x = 17.0F;
        state.m_world.getComponent<scripting::ScriptComponent>(body)->fields["speed"] = 7;
        state.update(1.0 / 60.0);
        require(state.m_scripts.inspect(body)["state"]["speed"] == 7, "Edited Luau fields did not reach the active instance");
        state.m_world.getComponent<scripting::ScriptComponent>(body)->path = "LiveB";
        state.update(1.0 / 60.0);
        require(state.m_scripts.inspect(body)["state"]["kind"] == "B", "Changing the behavior module did not replace its instance");
        const auto added = state.createEditorEntity(GameplayState::EditorEntityKind::PhysicsCube);
        require(added != ecs::kInvalidEntity && state.m_world.aliveCount() == count + 1U, "Play could not create an entity");
        state.undoSceneEdit();
        require(state.m_world.aliveCount() == count && state.m_editorMode == GameplayState::EditorMode::Play &&
                state.m_simulationRunning && state.m_scripts.playing(), "Play Undo must keep simulation and VM running");
        state.pauseSimulation();
        state.redoSceneEdit();
        require(state.m_world.aliveCount() == count + 1U && !state.m_simulationRunning && state.m_scripts.playing(),
                "Play Redo must preserve paused simulation");
        require(!state.saveSceneToDisk(), "Temporary Play edits must not overwrite the editor scene on disk");
        state.stopPlayMode();
        const auto restored = tagged(state, "Editable");
        require(state.m_world.aliveCount() == count && state.m_world.getComponent<ecs::Transform>(restored)->position.x == 2.0F &&
                state.m_world.getComponent<ecs::Rigidbody>(restored)->mass != 4.0F &&
                state.m_world.getComponent<scripting::ScriptComponent>(restored)->path == "LiveA" &&
                state.m_world.getComponent<scripting::ScriptComponent>(restored)->fields["speed"] == 1,
                "Stop lost the pre-Play properties or components");
        require(state.m_undoStack.size() == editorHistory && !state.m_scripts.playing(), "Stop lost editor history or left VM running");
    }

    static void standaloneUsesUnsavedSnapshot(core::JobSystem& jobs) {
        namespace fs = std::filesystem;
        const auto root = fs::temp_directory_path() / ("tge-play-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(root), "Could not create exclusive Play fixture");
        struct Cleanup { fs::path root; ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); } } cleanup{root};
        core::ProjectManager projects(root / "recent.json");
        std::string error;
        const auto project = projects.createBlank(root, "Игра", error);
        require(project.has_value(), "Could not create isolated project for Play");
        StateStack stack;
        stack.setServices(&jobs, nullptr); stack.setProject(&*project);
        GameplayState state(stack);
        state.onEnter();
        const auto added = state.createEditorEntity(GameplayState::EditorEntityKind::Empty);
        state.m_world.getComponent<ecs::Tag>(added)->value = "Unsaved";
        std::string captured;
        stack.setPlayWindowCallbacks([&](const std::string& scene, const fs::path& assets, std::string&) {
            captured = scene;
            require(assets == project->assetRoot, "Standalone launch lost the project asset root");
            return true;
        }, {}, {});
        state.enterPlayMode();
        state.m_world.getComponent<ecs::Tag>(added)->value = "LiveOnly";
        state.launchStandalonePlay();
        require(!captured.empty() && captured.find("LiveOnly") != std::string::npos, "Standalone did not export the current unsaved Play scene");
        require(state.m_editorMode == GameplayState::EditorMode::Edit && tagged(state, "Unsaved") != ecs::kInvalidEntity,
                "Switching to standalone did not restore the editor scene");
        std::ifstream original(project->scenePath);
        nlohmann::json saved; original >> saved;
        require(saved["entities"].empty(), "Launching game overwrote the saved project scene");
        const auto session = root / "session";
        fs::create_directory(session);
        { std::ofstream scene(session / "scene.json"); scene << captured; }
        const auto utf8 = [](const fs::path& path) {
            const auto bytes = path.generic_u8string(); return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        };
        const nlohmann::json descriptor = {{"version", 1}, {"name", project->name}, {"root", utf8(project->root)},
            {"assets", utf8(project->assetRoot)}, {"width", 800}, {"height", 600}, {"vsync", true}, {"device", "d3d12"}};
        { std::ofstream manifest(session / "play.json"); manifest << descriptor.dump(); }
        const auto launch = core::PlaySession::load(session / "play.json", error);
        require(launch && launch->project.assetRoot == project->assetRoot && launch->project.scenePath == session / "scene.json",
                "Unicode Play manifest did not route scene/assets correctly");
        StateStack childStack;
        childStack.setServices(&jobs, nullptr); childStack.setProject(&launch->project); childStack.setStandalone(true);
        GameplayState child(childStack);
        child.onEnter();
        require(child.m_editorMode == GameplayState::EditorMode::Play && child.m_scripts.playing() && !child.canEditScene() &&
                tagged(child, "LiveOnly") != ecs::kInvalidEntity, "Standalone did not load snapshot and auto-start Play");
        require(child.m_scripts.execute(R"(
            local React=require('React')
            local ReactTurbo=require('ReactTurbo')
            local root=ReactTurbo.createRoot({Name='StandaloneHUD'})
            root:render(React.createElement('TextLabel', {Text='Game UI'}))
        )"), "Standalone could not mount game UI");
        scripting::ReactRuntime::pump(child.m_scripts, 1.0 / 60.0);
        renderer::Renderer uiRenderer;
        ImGui::NewFrame();
        child.renderStandaloneUi(uiRenderer);
        ImGui::Render();
        for (const auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active) require(std::string_view(window->Name).find("Inspector") == std::string_view::npos &&
                std::string_view(window->Name).find("Hierarchy") == std::string_view::npos &&
                std::string_view(window->Name).find("Toolbar") == std::string_view::npos, "Standalone exposed an editor panel");
        }
        bool hud = false;
        for (const auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && std::string_view(window->Name) == "##ReactGameUI") {
                require((window->Flags & ImGuiWindowFlags_NoTitleBar) != 0, "Game HUD must not show editor window chrome");
                hud = true;
            }
        }
        require(hud, "Standalone hid the game's own React UI");
        child.onExit(); state.onExit();

        const auto onlineDirectory = session / "online";
        fs::create_directory(onlineDirectory);
        const auto onlineManifest = onlineDirectory / "play.json";
        auto online = descriptor;
        online["liveScene"] = utf8(project->scenePath);
        online["reactPackage"] = utf8(root / "react");
        online["onlineNetwork"] = {{"role", "server"}, {"address", 0}, {"port", 27020}, {"host", ""},
            {"steam", false}, {"steamId", 0}, {"capacity", 16}, {"compatibility", 0x8000000000000001ULL}};
        const auto loadOnline = [&] {
            { std::ofstream manifest(onlineManifest); manifest << online.dump(); }
            return core::PlaySession::load(onlineManifest, error);
        };
        const auto server = loadOnline();
        require(server && server->networkRole == core::SceneNetworkRole::Server && server->networkEndpoint.port == 27020 &&
            server->networkEndpoint.address == 0 && server->networkMaxPeers == 16 &&
            server->networkCompatibility == 0x8000000000000001ULL && server->project.scenePath == session / "scene.json" &&
            server->project.liveScenePath.empty() && server->networkStopPath == session / "stop" &&
            server->networkReadyPath == session / "ready" && server->reactPackage == root / "react" &&
            server->networkControlDirectory.empty(), "Online host launch lost its isolated scene or bind settings");

        online["onlineNetwork"]["role"] = "client";
        online["onlineNetwork"]["host"] = "example.invalid:27021";
        online["onlineNetwork"]["port"] = 0;
        const auto client = loadOnline();
        require(client && client->networkRole == core::SceneNetworkRole::Client &&
            client->networkHost == "example.invalid:27021" && client->networkStopPath.empty() &&
            client->networkControlDirectory.empty(), "Online join launch did not preserve the host for child DNS resolution");
        online["onlineNetwork"]["compatibility"] = 0;
        require(!loadOnline() && !error.empty(), "Online launch accepted a missing scene identity");
    }

    static void standaloneProcessSmoke(const std::filesystem::path& executable, core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.onEnter();
        state.createScriptDemo();
        const auto scene = state.serializeScene(state.captureSceneSnapshot());
        core::PlayLaunch launch;
        launch.project.name = "Standalone smoke";
        launch.project.assetRoot = std::filesystem::absolute(state.editorAssetsRoot());
        launch.project.root = launch.project.assetRoot.parent_path();
        launch.width = 960; launch.height = 640; launch.device = "d3d12";
        core::PlaySession session;
        std::string error;
        require(session.start(executable, scene, launch, error), error.c_str());
        require(session.running(), "Game process did not start");
        bool ready = false;
        for (int attempt = 0; attempt < 200 && session.running(); ++attempt) {
            std::ifstream log(session.directory() / "logs/engine.log");
            const std::string output((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            if (output.find("Scene ready: mode=Play") != std::string::npos) { ready = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(ready, "Game window did not load the snapshot and start simulation");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        require(session.running(), "Standalone game crashed after initialization");
        const auto directory = session.directory();
        session.stop();
        require(!session.running() && !std::filesystem::exists(directory), "Stop left a process or temporary scene behind");
        require(scene == state.serializeScene(state.captureSceneSnapshot()), "Standalone process changed the editor's world");
        stack.setStandalone(true);
        state.onExit();
    }

    static void requireNoScriptErrors(const GameplayState& state) {
        for (const auto& message : state.m_scripts.output()) {
            if (message.kind == scripting::ScriptMessage::Kind::Error) {
                throw std::runtime_error("Unexpected script error: " + message.text);
            }
        }
    }

    static void styledReactLayoutAndInput() {
        ecs::World world;
        scripting::ScriptingSystem scripts;
        require(scripts.initialize(world, nullptr, "assets") && scripting::ReactRuntime::install(scripts), "Styled UI runtime initialization failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement
            local D=UI.UDim; local D2=UI.UDim2; local C=UI.Color3
            __nativeClicks=0; __nativeClipClicks=0
            __nativeFirst=React.createRef(); __nativeSecond=React.createRef()
            local function Panel()
                local count,setCount=React.useState(0)
                return e('ScreenGui',{}, {
                    Card=e('Frame',{Size=D2.new(.5,20,0,200),Position=D2.fromScale(.5,.5),AnchorPoint=UI.Vector2.new(.5,.5),BackgroundColor3=C.fromRGB(210,20,30)}, {
                        Corner=e('UICorner',{CornerRadius=D.new(0,12)}),
                        Padding=e('UIPadding',{PaddingLeft=D.new(0,10),PaddingRight=D.new(0,10),PaddingTop=D.new(0,10),PaddingBottom=D.new(0,10)}),
                        Layout=e('UIListLayout',{Padding=D.new(0,10),HorizontalAlignment='Center'}),
                        Second=e('TextButton',{ref=__nativeSecond,LayoutOrder=2,Text='Second',Size=D2.new(1,0,0,36),BackgroundColor3=C.fromRGB(31,90,180),AutoButtonColor=false,[React.Event.Activated]=function() error('Disabled button activated') end}),
                        First=e('TextButton',{ref=__nativeFirst,LayoutOrder=1,Text='First '..count,Size=D2.new(1,0,0,36),BackgroundColor3=C.fromRGB(20,180,80),AutoButtonColor=false,[React.Event.Activated]=function() __nativeClicks+=1; setCount(count+1) end}),
                    }),
                    Clip=e('Frame',{Size=D2.fromOffset(80,40),Position=D2.fromOffset(20,20),ClipsDescendants=true,BackgroundTransparency=1},{
                        e('TextButton',{Size=D2.fromOffset(80,30),Position=D2.fromOffset(60,10),Text='',BackgroundColor3=C.fromRGB(180,20,180),AutoButtonColor=false,[React.Event.Activated]=function() __nativeClipClicks+=1 end}),
                    }),
                    Wrapped=e('TextLabel',{Size=D2.fromOffset(110,100),Position=D2.fromOffset(20,100),Text='wrapped words onto several lines',TextSize=24,TextWrapped=true,TextYAlignment='Top',TextColor3=C.fromRGB(20,220,230),TextTransparency=.5,BackgroundTransparency=1}),
                })
            end
            __nativeRoot=UI.createRoot({Name='StyledNativeTest'}); __nativeRoot:render(e(Panel))
            setmetatable(__nativeFirst.current.props,{__index=function() error('Native props metamethod') end})
        )", "=StyledNativeFixture"), "Styled UI fixture failed");
        auto& io = ImGui::GetIO();
        const int stackTop = lua_gettop(scripts.state());
        const auto frame = [&](const ImVec2 mouse, const bool down, const float width = 800.0F) {
            io.DisplaySize = ImVec2(width, 600.0F);
            io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame(); scripting::ReactUI::draw(scripts, true); ImGui::Render();
            require(lua_gettop(scripts.state()) == stackTop, "Styled native drawing leaked Lua stack entries");
        };
        struct Bounds { float left=1000000, top=1000000, right=-1000000, bottom=-1000000; int vertices=0; };
        const auto bounds = [](const ImU32 color) {
            Bounds result;
            for (const auto* window : ImGui::GetCurrentContext()->Windows) {
                if (!window->Active || std::string_view(window->RootWindow->Name) != "##ReactGameUI") continue;
                for (const auto& vertex : window->DrawList->VtxBuffer) if (vertex.col == color) {
                    result.left=std::min(result.left,vertex.pos.x); result.top=std::min(result.top,vertex.pos.y);
                    result.right=std::max(result.right,vertex.pos.x); result.bottom=std::max(result.bottom,vertex.pos.y); ++result.vertices;
                }
            }
            return result;
        };
        const auto near = [](const float a, const float b) { return std::abs(a-b)<2.0F; };
        frame(ImVec2(700,500), false); frame(ImVec2(700,500), false);
        const auto card = bounds(IM_COL32(210,20,30,255));
        const auto first = bounds(IM_COL32(20,180,80,255));
        const auto second = bounds(IM_COL32(31,90,180,255));
        require(card.vertices>8 && near(card.left,190) && near(card.top,200) && near(card.right,610) && near(card.bottom,400),
                "UDim2 scale/offset, AnchorPoint or UICorner did not produce the centered rounded card");
        require(first.vertices>0 && near(first.left,200) && near(first.top,210) && near(first.right,600) && near(first.bottom,246) &&
                near(second.top,256) && near(second.bottom,292), "Padding, list spacing, scale Size or LayoutOrder produced wrong item geometry");
        const auto wrapped = bounds(IM_COL32(20,220,230,128));
        require(wrapped.vertices>0 && wrapped.bottom-wrapped.top>30 && wrapped.right<=130.1F && wrapped.bottom<=200.1F,
                "TextSize, TextWrapped, text transparency or label clipping failed");
        frame(ImVec2(400,228), false); frame(ImVec2(400,228), true); frame(ImVec2(400,228), false);
        require(scripting::ReactRuntime::pump(scripts,0), "Styled click React pump failed");
        require(scripts.execute("assert(__nativeClicks==1 and __nativeFirst.current.props.Text=='First 1')", "=StyledNativeClick"),
                "A positioned styled button did not update actual React hook state exactly once");
        require(scripts.execute("__nativeSecond.current.props.Disabled=true", "=StyledNativeDisable"), "Could not disable styled button");
        frame(ImVec2(400,274), false); frame(ImVec2(400,274), true); frame(ImVec2(400,274), false);
        require(scripting::ReactRuntime::pump(scripts,0), "A disabled styled button still ran its callback");
        frame(ImVec2(130,40), false); frame(ImVec2(130,40), true); frame(ImVec2(130,40), false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__nativeClipClicks==0)", "=StyledClipOutside"),
                "ClipsDescendants allowed input outside its clipped parent");
        frame(ImVec2(90,40), false); frame(ImVec2(90,40), true); frame(ImVec2(90,40), false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__nativeClipClicks==1)", "=StyledClipInside"),
                "ClipsDescendants prevented a visible button from receiving input");
        frame(ImVec2(900,500), false, 1000.0F);
        const auto resized = bounds(IM_COL32(210,20,30,255));
        require(near(resized.left,240) && near(resized.right,760), "Scale layout did not respond to a resized game viewport");
        for (const auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && !window->ParentWindow && std::string_view(window->Name) == "##ReactGameUI")
                require(!window->ScrollbarX && !window->ScrollbarY && window->Scroll.y == 0.0F &&
                        (window->Flags & ImGuiWindowFlags_NoScrollWithMouse) != 0, "A fullscreen HUD developed scrollbars or scroll-wheel movement");
        }
        require(scripting::ReactRuntime::reset(scripts), "Styled UI reset failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement
            __legacyClicks=0
            local root=UI.createRoot({Name='StyledNativeTestLegacy'})
            root:render(e('ScreenGui',{}, {e('Frame',{Width=240,Height=140},{
                e('TextLabel',{Text='Legacy label',TextColor={199/255,201/255,15/255}}),
                e('TextButton',{Text='Legacy button',TextColor={17/255,219/255,111/255},[React.Event.Activated]=function() __legacyClicks+=1 end}),
            })}))
        )", "=LegacyNativeFixture"), "Legacy flow fixture failed");
        frame(ImVec2(700,500),false); frame(ImVec2(700,500),false);
        const auto legacyLabel=bounds(IM_COL32(199,201,15,255));
        const auto legacyButton=bounds(IM_COL32(17,219,111,255));
        require(legacyLabel.vertices>0 && legacyButton.vertices>0 && legacyButton.top>legacyLabel.bottom,
                "ScreenGui broke sequential legacy Frame Width/Height widgets");
        const ImVec2 legacyMouse{(legacyButton.left+legacyButton.right)*0.5F,(legacyButton.top+legacyButton.bottom)*0.5F};
        frame(legacyMouse,false); frame(legacyMouse,true); frame(legacyMouse,false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__legacyClicks==1)", "=LegacyNativeClick"),
                "A legacy button inside ScreenGui stopped receiving input");
        require(scripting::ReactRuntime::reset(scripts), "Legacy UI reset failed");
    }

    static void embeddedReactViewportLayoutAndInput() {
        ecs::World world;
        scripting::ScriptingSystem scripts;
        require(scripts.initialize(world, nullptr, "assets") && scripting::ReactRuntime::install(scripts), "Embedded UI initialization failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement
            __embeddedClicks=0; __embeddedClipClicks=0; __embeddedScreen=React.createRef()
            __embeddedRoot=UI.createRoot({Name='EmbeddedRoot'})
            __embeddedRoot:render(e('ScreenGui',{ref=__embeddedScreen},{
                First=e('TextButton',{Text='Embedded',Position=UDim2.fromOffset(20,20),Size=UDim2.new(.5,0,0,30),
                    BackgroundColor3=Color3.fromRGB(20,180,80),AutoButtonColor=false,
                    [React.Event.Activated]=function() __embeddedClicks+=1 end}),
                Clipped=e('TextButton',{Text='',Position=UDim2.fromOffset(-20,80),Size=UDim2.fromOffset(60,30),
                    [React.Event.Activated]=function() __embeddedClipClicks+=1 end}),
                Scroll=e('ScrollingFrame',{Position=UDim2.fromOffset(250,100),Size=UDim2.fromOffset(80,80),
                    CanvasSize=UDim2.fromOffset(80,200),ScrollBarThickness=4}),
                Group=e('CanvasGroup',{Position=UDim2.fromOffset(340,130),Size=UDim2.fromOffset(50,60),
                    BackgroundColor3=Color3.fromRGB(210,40,90),GroupTransparency=.5}),
            }))
            __embeddedOther=UI.createRoot({Name='EmbeddedSecondRoot'})
            __embeddedOther:render(e('ScreenGui',{}, {e('TextLabel',{Text='Second root',
                Position=UDim2.new(1,-90,0,0),Size=UDim2.fromOffset(90,20),BackgroundTransparency=1})}))
        )", "=EmbeddedViewportFixture"), "Embedded UI fixture failed");
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = {1000, 700}; io.DeltaTime = 1.0F / 60;
        ImGui::ClearActiveID();
        scripting::ReactUI::Viewport region{200,160,400,240};
        const int stackTop = lua_gettop(scripts.state());
        unsigned capturedGroups=0;
        const scripting::ReactUI::CanvasResolver canvas=[&](const void*, const ImDrawList& list, float x, float y, float width, float height) {
            require(x==region.x+340 && y==region.y+130 && width==50 && height==60 && list.VtxBuffer.Size>0,
                "Embedded CanvasGroup used window rather than viewport coordinates");
            for(const auto& vertex:list.VtxBuffer)
                require(vertex.pos.x>=x && vertex.pos.x<=x+width && vertex.pos.y>=y && vertex.pos.y<=y+height,
                    "Embedded CanvasGroup captured unrelated editor/HUD geometry");
            ++capturedGroups;
            return scripting::ReactUI::Image{reinterpret_cast<void*>(1),width,height};
        };
        const auto frame = [&](const ImVec2 mouse, const bool down) {
            io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({150,80}); ImGui::SetNextWindowSize({700,500});
            ImGui::Begin("EmbeddedViewportHost", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("Editor header");
            auto* window = ImGui::GetCurrentWindow();
            const auto cursor = window->DC.CursorPos, maximum = window->DC.CursorMaxPos;
            const auto clip = window->ClipRect;
            const int ids = window->IDStack.Size, styles = ImGui::GetCurrentContext()->StyleVarStack.Size;
            const bool captured = scripting::ReactUI::drawInViewport(scripts, region, {}, canvas);
            require(window->IDStack.Size == ids && ImGui::GetCurrentContext()->StyleVarStack.Size == styles &&
                window->DC.CursorPos.x == cursor.x && window->DC.CursorPos.y == cursor.y &&
                window->DC.CursorMaxPos.x == maximum.x && window->DC.CursorMaxPos.y == maximum.y &&
                window->ClipRect.Min.x == clip.Min.x && window->ClipRect.Max.y == clip.Max.y,
                "Embedded HUD leaked host layout, clip or ID/style state");
            ImGui::TextUnformatted("Editor footer");
            ImGui::End(); ImGui::Render();
            require(lua_gettop(scripts.state()) == stackTop, "Embedded drawing leaked the Lua stack");
            for (const auto* other : ImGui::GetCurrentContext()->Windows)
                require(!other->Active || std::string_view(other->Name).find("##ReactRoot") == std::string_view::npos,
                    "Embedded HUD created a separate root window");
            return captured;
        };
        frame({900,600},false); frame({900,600},false);
        require(capturedGroups>=2, "Embedded CanvasGroup was not composited by the supplied resolver");
        require(scripts.execute("assert(__embeddedScreen.current.props.AbsolutePosition.X==200 and __embeddedScreen.current.props.AbsolutePosition.Y==160 and __embeddedScreen.current.props.AbsoluteSize.X==400 and __embeddedScreen.current.props.AbsoluteSize.Y==240)", "=EmbeddedGeometry"),
            "Embedded ScreenGui did not use the viewport origin and size");
        require(frame({240,195},false), "Embedded button did not capture pointer input");
        frame({240,195},true); frame({240,195},false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__embeddedClicks==1)", "=EmbeddedClick"),
            "A second root intercepted the embedded button or click was dispatched more than once");
        frame({190,250},false); frame({190,250},true); frame({190,250},false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__embeddedClipClicks==0)", "=EmbeddedOutside"),
            "Embedded UI accepted input outside the viewport clip");
        frame({210,250},false); frame({210,250},true); frame({210,250},false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__embeddedClipClicks==1)", "=EmbeddedInside"),
            "Viewport clipping prevented a visible button portion from receiving input");
        require(frame({480,285},false), "Scrolling HUD surface did not capture input");
        frame({528,270},false); frame({528,270},true);
        require(frame({700,290},true), "Dragging a HUD scrollbar outside the viewport released input capture");
        require(!frame({700,290},false), "Released HUD scrollbar retained input capture outside the viewport");
        region.width=520; region.height=300;
        frame({900,600},false);
        require(scripts.execute("assert(__embeddedScreen.current.props.AbsoluteSize.X==520 and __embeddedScreen.current.props.AbsoluteSize.Y==300)", "=EmbeddedResize"),
            "Embedded UI did not respond to viewport resize");
        region.width=0;
        require(!frame({240,195},false), "Zero-sized viewport consumed input");
        require(scripting::ReactRuntime::reset(scripts), "Embedded HUD reset failed");
    }

    static void editorHudUsesViewport(core::JobSystem& jobs) {
        namespace fs = std::filesystem;
        struct Fixture {
            fs::path root=fs::temp_directory_path() / ("tge-editor-hud-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() {std::error_code ignored; fs::remove_all(root,ignored);}
        } fixture;
        core::ProjectManager projects(fixture.root / "registry.json");
        std::string error;
        const auto project=projects.createBlank(fixture.root,"Embedded",error);
        require(project.has_value(), "Editor HUD project fixture failed");
        StateStack stack; stack.setServices(&jobs,nullptr); stack.setProject(&*project);
        GameplayState state(stack); state.onEnter();
        state.m_showAssistant=state.m_showHierarchyPanel=state.m_showProjectPanel=state.m_showInspectorPanel=state.m_showStatsPanel=false;
        state.m_showViewportPanel=true; state.m_showScriptWorkspace=false;
        state.enterPlayMode();
        require(state.m_scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement
            __editorHudClicks=0; __editorHudScreen=React.createRef()
            UI.createRoot({Name='EditorEmbeddedFixture'}):render(e('ScreenGui',{ref=__editorHudScreen},{
                e('Frame',{Size=UDim2.fromScale(1,1),BackgroundColor3=Color3.fromRGB(13,47,91),BorderSizePixel=0}),
                e('TextButton',{Text='HUD action',Position=UDim2.fromOffset(20,20),Size=UDim2.fromOffset(100,40),
                    [React.Event.Activated]=function() __editorHudClicks+=1 end})
            }))
        )", "=EditorHudFixture"), "Editor HUD setup failed");
        renderer::Renderer renderer;
        auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={1280,720}; io.DeltaTime=1.0F/60;
        ImGui::ClearActiveID(); platform::InputManager::reset();
        const auto frame=[&](const ImVec2 mouse,const bool down) {
            io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left,down);
            ImGui::NewFrame(); state.renderEditorUi(renderer); ImGui::Render();
            for(const auto* window:ImGui::GetCurrentContext()->Windows)
                require(!window->Active || std::string_view(window->Name).find("##ReactRoot")==std::string_view::npos,
                    "Editor Play created a detached HUD window");
        };
        frame({-1000,-1000},false); frame({-1000,-1000},false);
        require(state.m_scripts.execute("assert(__editorHudScreen.current.props.AbsoluteSize.X>320)", "=EditorHudSize"),
            "Editor did not render its HUD into the viewport");
        const auto selected=state.m_world.createEntity();
        const auto camera=state.buildActiveCameraFrame(renderer, 1.0F);
        const glm::vec3 position=glm::inverse(camera.view)*glm::vec4(0,0,-5,1);
        state.m_world.addComponent<ecs::Transform>(selected).position=position;
        state.m_selectedEntity=selected; state.m_selectionToolActive=false;
        frame({-1000,-1000},false);
        const auto* viewport=ImGui::FindWindowByName("Viewport");
        require(viewport!=nullptr, "Editor viewport window missing");
        unsigned int precedingVertex=0;
        bool hudCoversGizmo=false;
        for(const auto& command:viewport->DrawList->CmdBuffer) {
            for(unsigned int i=0;i<command.ElemCount;++i) {
                const unsigned int vertex=command.VtxOffset+viewport->DrawList->IdxBuffer[command.IdxOffset+i];
                if(viewport->DrawList->VtxBuffer[vertex].col==IM_COL32(13,47,91,255)) {
                    hudCoversGizmo=precedingVertex>vertex;
                    break;
                }
                precedingVertex=std::max(precedingVertex,vertex);
            }
            if(hudCoversGizmo) break;
        }
        require(hudCoversGizmo, "Editor gizmo was drawn over the HUD");
        const ImVec2 point{state.m_viewportPosition.x+50,state.m_viewportPosition.y+40};
        frame(point,false); frame(point,true);
        require(state.m_viewportUiCaptured && state.m_hoveredEntity==ecs::kInvalidEntity,
            "HUD mouse input did not suppress scene picking/hover");
        platform::Event rightClick; rightClick.type=platform::EventType::MouseButtonPressed; rightClick.mouseButton=platform::MouseButton::Right;
        state.handleEvent(rightClick);
        require(!state.m_cameraLookActive, "HUD right click activated the editor camera");
        frame(point,false);
        require(scripting::ReactRuntime::pump(state.m_scripts,0) && state.m_scripts.execute("assert(__editorHudClicks==1)", "=EditorHudClick"),
            "Editor HUD event was not delivered exactly once");
        frame({state.m_viewportPosition.x+20,state.m_viewportPosition.y-10},false);
        require(!state.m_viewportHovered && !state.m_viewportUiCaptured, "Viewport header was treated as game input");
        state.m_showViewportPanel=false;
        frame(point,false);
        require(!state.m_viewportHovered && !state.m_viewportFocused && !state.m_viewportUiCaptured,
            "Hidden viewport retained gameplay focus/input");
        state.m_showViewportPanel=true; state.m_showScriptWorkspace=true;
        frame(point,false);
        require(!state.m_viewportHovered && !state.m_viewportUiCaptured, "Script workspace retained viewport HUD input");
        state.m_showScriptWorkspace=false;
        state.stopPlayMode(); state.onExit();
    }

    static void robloxInstanceLayoutAndSignals() {
        ecs::World world;
        scripting::ScriptingSystem scripts;
        require(scripts.initialize(world, nullptr, "assets") && scripting::ReactRuntime::install(scripts), "Roblox native UI runtime initialization failed");
        require(scripts.execute(R"(
            local React=require('React'); local ReactRoblox=require('ReactRoblox'); local e=React.createElement
            __compatHud=ReactRoblox.createRoot(Instance.new('ScreenGui'))
            __compatHud:render(e('TextLabel',{Text='HUD',Position=UDim2.fromOffset(350,20),Size=UDim2.fromOffset(100,32)}))
            __compatScreen=Instance.new('ScreenGui'); __compatScreen.Name='RobloxNativeTest'
            __compatFrame=React.createRef(); __compatFirst=React.createRef(); __compatList=React.createRef(); __compatText=React.createRef()
            __compatSizeChanges=0; __compatSignalChanges=0; __compatClicks=0
            __compatRoot=ReactRoblox.createRoot(__compatScreen)
            __compatRoot:render(e(React.Fragment,nil,{
                Card=e('Frame',{ref=__compatFrame,AutomaticSize=Enum.AutomaticSize.XY,Size=UDim2.fromOffset(10,10),Position=UDim2.fromOffset(40,50),BackgroundColor3=Color3.fromRGB(21,39,61),
                    [React.Change.AbsoluteSize]=function() __compatSizeChanges+=1 end}, {
                    Padding=e('UIPadding',{PaddingLeft=UDim.new(0,10),PaddingRight=UDim.new(0,10),PaddingTop=UDim.new(0,10),PaddingBottom=UDim.new(0,10)}),
                    Layout=e('UIListLayout',{ref=__compatList,FillDirection=Enum.FillDirection.Horizontal,SortOrder=Enum.SortOrder.LayoutOrder,Padding=UDim.new(0,6),VerticalAlignment=Enum.VerticalAlignment.Center}),
                    Stroke=e('UIStroke',{Color=Color3.fromRGB(231,121,17),Thickness=4}),
                    First=e('TextButton',{ref=__compatFirst,LayoutOrder=1,Text='Press',Size=UDim2.fromOffset(80,32),TextXAlignment=Enum.TextXAlignment.Center,TextYAlignment=Enum.TextYAlignment.Center,BackgroundColor3=Color3.fromRGB(18,151,97),AutoButtonColor=false,
                        [React.Event.Activated]=function() __compatClicks+=1 end}),
                    Second=e('TextLabel',{LayoutOrder=2,Text='Label',Size=UDim2.fromOffset(50,32),BackgroundTransparency=1}),
                }),
                Text=e('TextLabel',{ref=__compatText,Text='Automatic text wraps across multiple lines',TextSize=20,TextWrapped=true,AutomaticSize=Enum.AutomaticSize.Y,Size=UDim2.fromOffset(60,0),Position=UDim2.fromOffset(250,50),TextYAlignment=Enum.TextYAlignment.Top,BackgroundTransparency=1}),
            }))
            __compatFrame.current:GetPropertyChangedSignal('AbsoluteSize'):Connect(function() __compatSignalChanges+=1 end)
            __compatEmptyRoot=require('ReactTurbo').createRoot({Name='EmptyPortalOwner'})
            setmetatable(__compatFrame.current.props,{__index=function() error('Native Instance props metamethod') end})
        )", "=RobloxNativeFixture"), "Roblox Instance/Enum UI fixture failed");
        auto& io = ImGui::GetIO();
        const int stackTop = lua_gettop(scripts.state());
        const auto frame = [&](const ImVec2 mouse, const bool down, const bool gameOnly = true) {
            io.DisplaySize=ImVec2(800,600); io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left,down);
            ImGui::NewFrame(); scripting::ReactUI::draw(scripts,gameOnly); ImGui::Render();
            require(lua_gettop(scripts.state())==stackTop, "Roblox native UI leaked the Lua stack");
        };
        frame(ImVec2(700,500),false); frame(ImVec2(700,500),false);
        require(scripts.execute(R"(
            local card=__compatFrame.current; local button=__compatFirst.current; local content=__compatList.current.AbsoluteContentSize
            assert(typeof(card)=='Instance', 'Card type: '..typeof(card))
            assert(typeof(card.AbsoluteSize)=='Vector2', 'AbsoluteSize type: '..typeof(card.AbsoluteSize))
            assert(typeof(button.TextBounds)=='Vector2', 'TextBounds type: '..typeof(button.TextBounds))
            assert(card.AbsolutePosition.X==40 and card.AbsolutePosition.Y==50)
            assert(card.AbsoluteSize.X==156 and card.AbsoluteSize.Y==52)
            assert(button.AbsolutePosition.X==50 and button.AbsolutePosition.Y==60 and button.AbsoluteSize.X==80)
            assert(content.X==136 and content.Y==32 and __compatText.current.AbsoluteSize.Y>60)
            assert(__compatSignalChanges==0 and __compatSizeChanges==0, 'Draw invoked an unprotected property callback')
        )", "=RobloxNativeGeometry"), "AutomaticSize, typed geometry, Enum layout or deferred property signals failed");
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__compatSignalChanges==1 and __compatSizeChanges==1)", "=RobloxNativeSignal"),
            "A native AbsoluteSize change did not dispatch the Instance property signal exactly once");
        bool stroke = false;
        for (const auto* window : ImGui::GetCurrentContext()->Windows) {
            if (!window->Active) continue;
            require(std::string_view(window->Name).find("EmptyPortalOwner")==std::string_view::npos,
                "An empty portal owner created a fullscreen window that would intercept another root's input");
            for (const auto& vertex : window->DrawList->VtxBuffer) if (vertex.col==IM_COL32(231,121,17,255)) stroke=true;
        }
        require(stroke, "A Roblox UIStroke did not draw its border");
        require(scripts.execute("__compatFirst.current.Size=UDim2.fromOffset(100,42); __compatFirst.current.Text='Changed'", "=RobloxNativeMutation"),
            "Instance property mutation failed");
        frame(ImVec2(700,500),false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__compatFrame.current.AbsoluteSize.X==176 and __compatFrame.current.AbsoluteSize.Y==62 and __compatSignalChanges==2 and __compatSizeChanges==2)", "=RobloxNativeResize"),
            "Changing an Instance property did not update native layout or notify its sized parent");
        frame(ImVec2(90,80),false); frame(ImVec2(90,80),true); frame(ImVec2(90,80),false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__compatClicks==1)", "=RobloxNativeClick"),
            "The standalone HUD intercepted a button in another React root");
        require(scripts.execute("__compatScreen.Enabled=false", "=RobloxNativeDisable"), "ScreenGui.Enabled mutation failed");
        frame(ImVec2(90,80),false); frame(ImVec2(90,80),true); frame(ImVec2(90,80),false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__compatClicks==1)", "=RobloxNativeDisabledClick"),
            "A disabled ScreenGui still dispatched a button event");
        require(scripts.execute("__compatScreen.Enabled=true; table.freeze(__compatFirst.current.props)", "=RobloxNativeFrozenProps"), "Could not create frozen native props fixture");
        frame(ImVec2(700,500),false,false);
        require(scripting::ReactRuntime::reset(scripts), "Roblox Instance UI reset failed");
    }

    static void tweenedNativePresentation() {
        ecs::World world;
        scripting::ScriptingSystem scripts;
        require(scripts.initialize(world, nullptr, "assets") && scripting::ReactRuntime::install(scripts), "Native tween runtime failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement
            __nativeTweenCard=React.createRef(); __nativeTweenGradient=React.createRef()
            __nativeTweenRoot=UI.createRoot({Name='NativeTweenPresentation'})
            __nativeTweenRoot:render(e('ScreenGui',{}, {
                Card=e('Frame',{ref=__nativeTweenCard,Position=UDim2.fromOffset(40,60),Size=UDim2.fromOffset(80,40),
                    BackgroundColor3=Color3.new(1,1,1)}, {
                    Gradient=e('UIGradient',{ref=__nativeTweenGradient,Color=ColorSequence.new(Color3.new(1,0,0),Color3.new(0,0,1))})
                })
            }))
            local service=game:GetService('TweenService')
            __nativeTweenMove=service:Create(__nativeTweenCard.current,TweenInfo.new(1,Enum.EasingStyle.Linear),{
                Position=UDim2.fromOffset(240,160),Size=UDim2.fromOffset(160,80),BackgroundTransparency=.5})
            __nativeTweenSweep=service:Create(__nativeTweenGradient.current,TweenInfo.new(1,Enum.EasingStyle.Linear),{
                Rotation=90,Offset=Vector2.new(.5,0)})
            __nativeTweenMove:Play(); __nativeTweenSweep:Play()
        )", "=NativeTweenSetup"), "Native tween setup failed");
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(800,600); io.DeltaTime=1.0F/60.0F;
        io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);
        const auto frame = [&] {
            ImGui::NewFrame(); scripting::ReactUI::draw(scripts,true); ImGui::Render();
        };
        require(scripting::ReactRuntime::pump(scripts,.5), "Native tween midpoint pump failed");
        frame();
        require(scripts.execute(R"(
            assert(__nativeTweenCard.current.AbsolutePosition==Vector2.new(140,110))
            assert(__nativeTweenCard.current.AbsoluteSize==Vector2.new(120,60))
            assert(__nativeTweenCard.current.BackgroundTransparency==.25)
            assert(__nativeTweenGradient.current.Rotation==45 and __nativeTweenGradient.current.Offset==Vector2.new(.25,0))
        )", "=NativeTweenMidpoint"), "Tween properties did not reach native geometry/gradient drawing");
        require(scripting::ReactRuntime::pump(scripts,.5), "Native tween final pump failed");
        frame();
        require(scripts.execute("assert(__nativeTweenCard.current.AbsolutePosition==Vector2.new(240,160) and __nativeTweenMove.PlaybackState==Enum.PlaybackState.Completed)",
                               "=NativeTweenFinal"), "Native tween did not finish at its goal");
        require(scripting::ReactRuntime::reset(scripts), "Native tween reset failed");
        frame();
        require(scripts.execute("assert(#__turboUIRoots==0 and __nativeTweenCard.current==nil)", "=NativeTweenStop"), "Tween Stop retained a UI root/ref");
    }

    static void advancedReactVisualsAndControls() {
        ecs::World world; scripting::ScriptingSystem scripts;
        require(scripts.initialize(world,nullptr,"assets") && scripting::ReactRuntime::install(scripts),"Advanced UI runtime failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement
            __advancedImage=React.createRef(); __advancedGradient=React.createRef(); __advancedScale=React.createRef(); __advancedChild=React.createRef()
            __advancedConstraint=React.createRef(); __advancedGrid=React.createRef(); __advancedCell=React.createRef(); __advancedScroll=React.createRef(); __advancedText=React.createRef()
            __advancedClicks=0; __advancedScrollClicks=0; __advancedChanges=0; __advancedEnter=0; __advancedLeave=0; __advancedDown=0; __advancedUp=0; __advancedFocus=false
            local function App()
                local text,setText=React.useState('')
                return e('ScreenGui',{}, {
                    Gradient=e('Frame',{Size=UDim2.fromOffset(160,80),Position=UDim2.fromOffset(20,20),BackgroundColor3=Color3.new(1,1,1)}, {
                        e('UICorner',{CornerRadius=UDim.new(0,16)}),
                        e('UIGradient',{ref=__advancedGradient,Color=ColorSequence.new({ColorSequenceKeypoint.new(0,Color3.new(1,0,0)),ColorSequenceKeypoint.new(.5,Color3.new(0,1,0)),ColorSequenceKeypoint.new(1,Color3.new(0,0,1))}),
                            Transparency=NumberSequence.new({NumberSequenceKeypoint.new(0,0),NumberSequenceKeypoint.new(.5,.5),NumberSequenceKeypoint.new(1,0)})}),
                    }),
                    Fit=e('ImageLabel',{Image='atlas',ScaleType=Enum.ScaleType.Fit,Size=UDim2.fromOffset(120,80),Position=UDim2.fromOffset(200,20),BackgroundTransparency=1,ImageRectOffset=Vector2.new(32,16),ImageRectSize=Vector2.new(64,32)}),
                    Crop=e('ImageLabel',{Image='atlas',ScaleType=Enum.ScaleType.Crop,Size=UDim2.fromOffset(80,60),Position=UDim2.fromOffset(340,20),BackgroundTransparency=1,ImageRectOffset=Vector2.new(32,16),ImageRectSize=Vector2.new(64,32)}),
                    Tile=e('ImageLabel',{Image='atlas',ScaleType=Enum.ScaleType.Tile,TileSize=UDim2.fromOffset(32,32),Size=UDim2.fromOffset(96,64),Position=UDim2.fromOffset(440,20),BackgroundTransparency=1}),
                    Slice=e('ImageLabel',{Image='atlas',ScaleType=Enum.ScaleType.Slice,SliceCenter=Rect.new(16,16,112,48),Size=UDim2.fromOffset(160,80),Position=UDim2.fromOffset(560,20),BackgroundTransparency=1}),
                    Scale=e('Frame',{ref=__advancedScale,Size=UDim2.fromOffset(100,80),Position=UDim2.fromOffset(20,140),BackgroundTransparency=1},{
                        e('UIScale',{Scale=.75}), e('TextLabel',{ref=__advancedChild,Text='Scaled',Size=UDim2.fromOffset(40,20),Position=UDim2.fromOffset(10,10),BackgroundTransparency=1}),
                    }),
                    Constraint=e('Frame',{ref=__advancedConstraint,Size=UDim2.fromOffset(300,200),Position=UDim2.fromOffset(140,140),BackgroundTransparency=1},{
                        e('UISizeConstraint',{MinSize=Vector2.new(40,10),MaxSize=Vector2.new(120,100)}),e('UIAspectRatioConstraint',{AspectRatio=2}),
                    }),
                    ImageButton=e('ImageButton',{ref=__advancedImage,Image='base',HoverImage='hover',PressedImage='press',Size=UDim2.fromOffset(80,48),Position=UDim2.fromOffset(20,240),BackgroundTransparency=1,
                        [React.Event.Activated]=function() __advancedClicks+=1 end,
                        [React.Event.MouseEnter]=function(_,x,y) assert(type(x)=='number' and type(y)=='number'); __advancedEnter+=1 end,
                        [React.Event.MouseLeave]=function() __advancedLeave+=1 end,
                        [React.Event.MouseButton1Down]=function() __advancedDown+=1 end,[React.Event.MouseButton1Up]=function() __advancedUp+=1 end}),
                    Missing=e('ImageLabel',{Image='missing',Size=UDim2.fromOffset(60,40),Position=UDim2.fromOffset(120,240)}),
                    Grid=e('Frame',{Size=UDim2.fromOffset(220,140),Position=UDim2.fromOffset(20,400),BackgroundTransparency=1},{
                        e('UIGridLayout',{ref=__advancedGrid,CellSize=UDim2.fromOffset(60,30),CellPadding=UDim2.fromOffset(10,5),FillDirectionMaxCells=2,SortOrder=Enum.SortOrder.LayoutOrder}),
                        e('TextLabel',{Text='one',LayoutOrder=1}),e('TextLabel',{Text='two',LayoutOrder=2}),e('TextLabel',{ref=__advancedCell,Text='three',LayoutOrder=3}),e('Folder',{},{}),
                    }),
                    Scroll=e('ScrollingFrame',{ref=__advancedScroll,Size=UDim2.fromOffset(200,100),Position=UDim2.fromOffset(280,300),CanvasSize=UDim2.fromOffset(0,0),CanvasPosition=Vector2.new(0,40),AutomaticCanvasSize=Enum.AutomaticSize.Y,ScrollingDirection=Enum.ScrollingDirection.Y,ScrollBarThickness=10},{
                        e('UIListLayout',{Padding=UDim.new(0,4),SortOrder=Enum.SortOrder.LayoutOrder}),
                        e('TextButton',{LayoutOrder=1,Size=UDim2.new(1,0,0,36),Text='one'}),
                        e('TextButton',{LayoutOrder=2,Size=UDim2.new(1,0,0,36),Text='two',[React.Event.Activated]=function() __advancedScrollClicks+=1 end}),
                        e('TextButton',{LayoutOrder=3,Size=UDim2.new(1,0,0,36),Text='three'}),e('TextButton',{LayoutOrder=4,Size=UDim2.new(1,0,0,36),Text='four'}),
                        e('TextButton',{LayoutOrder=5,Size=UDim2.new(1,0,0,36),Text='five'}),e('TextButton',{LayoutOrder=6,Size=UDim2.new(1,0,0,36),Text='six'}),
                    }),
                    Text=e('TextBox',{ref=__advancedText,Text=text,ClearTextOnFocus=false,PlaceholderText='Type here',Size=UDim2.fromOffset(180,40),Position=UDim2.fromOffset(520,240),
                        [React.Change.Text]=function(instance) __advancedChanges+=1; setText(instance.Text) end,
                        [React.Event.FocusLost]=function(_,enterPressed) __advancedFocus=enterPressed end}),
                })
            end
            __advancedRoot=UI.createRoot({Name='AdvancedNativeTest'}); __advancedRoot:render(e(App))
        )","=AdvancedNativeFixture"),"Advanced UI fixture failed");
        const auto resolver=[](std::string_view name)->scripting::ReactUI::Image {
            if (name=="missing") return {};
            const uintptr_t id=name=="hover" ? 0x3456 : name=="press" ? 0x4567 : name=="base" ? 0x2345 : 0x1234;
            return {reinterpret_cast<void*>(id),128,64};
        };
        auto& io=ImGui::GetIO();
        const int top=lua_gettop(scripts.state());
        const auto frame=[&](ImVec2 mouse,bool down) {
            io.DisplaySize=ImVec2(800,600); io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left,down);
            ImGui::NewFrame(); scripting::ReactUI::draw(scripts,true,resolver); ImGui::Render();
            require(lua_gettop(scripts.state())==top,"Advanced UI leaked the Lua stack");
        };
        const auto textureSeen=[](uintptr_t texture) {
            for (const auto* window:ImGui::GetCurrentContext()->Windows) if (window->Active)
                for (const auto& command:window->DrawList->CmdBuffer)
                    if (command.ElemCount && command.TexRef._TexData==nullptr && command.TexRef._TexID==static_cast<ImTextureID>(texture)) return true;
            return false;
        };
        frame({760,560},false); frame({760,560},false);
        require(scripts.execute(R"(
            assert(__advancedScale.current.AbsoluteSize==Vector2.new(75,60) and __advancedChild.current.AbsoluteSize==Vector2.new(30,15))
            assert(__advancedChild.current.AbsolutePosition==Vector2.new(27.5,147.5))
            assert(__advancedConstraint.current.AbsoluteSize==Vector2.new(120,60))
            assert(__advancedGrid.current.AbsoluteContentSize==Vector2.new(130,65) and __advancedGrid.current.AbsoluteCellCount==Vector2.new(2,2))
            assert(__advancedCell.current.AbsolutePosition==Vector2.new(20,435))
            assert(__advancedScroll.current.AbsoluteCanvasSize.Y==236 and __advancedScroll.current.AbsoluteWindowSize.X==190)
            assert(__advancedImage.current.IsLoaded)
        )","=AdvancedNativeGeometry"),"UIScale, constraints, grid or automatic canvas geometry failed");
        bool middle=false,fit=false,crop=false,tile=false,slice=false;
        for (const auto* window:ImGui::GetCurrentContext()->Windows) if (window->Active && std::string_view(window->RootWindow->Name)=="##ReactGameUI") {
            for (const auto& vertex:window->DrawList->VtxBuffer) {
                const auto color=ImGui::ColorConvertU32ToFloat4(vertex.col);
                if (vertex.pos.x>=20 && vertex.pos.x<36 && vertex.pos.y>=20 && vertex.pos.y<36)
                    require((vertex.pos.x-36)*(vertex.pos.x-36)+(vertex.pos.y-36)*(vertex.pos.y-36)<=256.1F,"Gradient mesh escaped its rounded corner clip");
                if (vertex.pos.x>=20 && vertex.pos.x<=180 && vertex.pos.y>=20 && vertex.pos.y<=100 && color.y>.95F && color.x<.05F && color.z<.05F && color.w<.6F) middle=true;
                if (std::abs(vertex.pos.x-200)<.01F && std::abs(vertex.pos.y-30)<.01F && std::abs(vertex.uv.x-.25F)<.01F && std::abs(vertex.uv.y-.25F)<.01F) fit=true;
                if (std::abs(vertex.pos.x-340)<.01F && std::abs(vertex.pos.y-20)<.01F && std::abs(vertex.uv.x-1.0F/3.0F)<.01F) crop=true;
                if (std::abs(vertex.pos.x-472)<.01F && std::abs(vertex.pos.y-52)<.01F && vertex.uv.x<.01F) tile=true;
                if (std::abs(vertex.pos.x-576)<.01F && std::abs(vertex.pos.y-36)<.01F && std::abs(vertex.uv.x-.125F)<.01F) slice=true;
            }
        }
        require(middle && fit && crop && tile && slice && textureSeen(0x1234),"Gradient middle keypoint/alpha or image fit/crop/tile/slice atlas geometry failed");
        frame({60,260},false); require(textureSeen(0x3456),"HoverImage did not use its resolved texture");
        frame({60,260},true); require(textureSeen(0x4567),"PressedImage did not use its resolved texture"); frame({60,260},false); frame({760,560},false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__advancedClicks==1 and __advancedEnter==1 and __advancedLeave==1 and __advancedDown==1 and __advancedUp==1)","=AdvancedNativeMouse"),"ImageButton mouse events failed");
        frame({320,320},false); frame({320,320},true); frame({320,320},false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__advancedScrollClicks==1)","=AdvancedNativeScrollClick"),"Scrolled content did not receive its clipped click");
        io.AddMouseWheelEvent(0,-1); frame({320,320},false);
        require(scripts.execute("assert(__advancedScroll.current.CanvasPosition.Y>40)","=AdvancedNativeWheel"),"ScrollingFrame ignored the mouse wheel");
        frame({570,260},false); frame({570,260},true); frame({570,260},false); io.AddInputCharactersUTF8("Luau text"); frame({570,260},false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__advancedText.current.Text=='Luau text' and __advancedChanges>0)","=AdvancedNativeText"),"TextBox did not preserve controlled text edits");
        io.AddKeyEvent(ImGuiKey_Enter,true); frame({570,260},false); io.AddKeyEvent(ImGuiKey_Enter,false); frame({570,260},false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__advancedFocus==true)","=AdvancedNativeFocus"),"TextBox FocusLost did not report Enter");
        require(scripts.execute("__advancedText.current:CaptureFocus()","=AdvancedNativeCapture"),"TextBox CaptureFocus request failed");
        frame({760,560},false); frame({760,560},false);
        require(scripts.execute("assert(__advancedText.current:IsFocused()); __advancedText.current:ReleaseFocus(false)","=AdvancedNativeRelease"),"Programmatic TextBox focus was not applied");
        frame({760,560},false);
        require(scripts.execute("assert(not __advancedText.current:IsFocused())","=AdvancedNativeReleased"),"TextBox ReleaseFocus did not blur its own input");
        require(scripting::ReactRuntime::reset(scripts),"Advanced UI cleanup failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement; local children={}
            for index=1,80 do children[index]=e('Frame',{Size=UDim2.fromOffset(200,100),BackgroundColor3=Color3.new(1,1,1)}, {
                e('UICorner',{CornerRadius=UDim.new(0,16)}),e('UIGradient',{Color=ColorSequence.new(Color3.new(1,0,0),Color3.new(0,0,1))}),
            }) end
            __meshRoot=UI.createRoot({Name='BoundedGradientTest'}); __meshRoot:render(e('ScreenGui',{},children))
        )","=AdvancedNativeMeshBudget"),"Could not mount bounded gradient stress fixture");
        frame({760,560},false);
        for (const auto* window:ImGui::GetCurrentContext()->Windows) if (window->Active)
            require(window->DrawList->VtxBuffer.Size<65536,"Gradient tessellation overflowed a 16-bit ImGui draw list");
        require(scripting::ReactRuntime::reset(scripts),"Gradient stress cleanup failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement
            __largeText=string.rep(string.rep('x',800)..'\n',40); __largeTextRef=React.createRef()
            __largeTextRoot=UI.createRoot({Name='BoundedTextBoxTest'})
            __largeTextRoot:render(e('TextBox',{ref=__largeTextRef,Text=__largeText,TextEditable=false,MultiLine=true,TextSize=6,Size=UDim2.fromOffset(8000,6000)}, {e('UIScale',{Scale=.1})}))
        )","=AdvancedNativeTextBudget"),"Could not mount the large editable-buffer fixture");
        frame({760,560},false);
        require(scripts.execute("assert(__largeTextRef.current.Text==__largeText)","=AdvancedNativeTextPreserved"),"Headless text rendering truncated the editable buffer");
        for (const auto* window:ImGui::GetCurrentContext()->Windows) if (window->Active)
            require(window->DrawList->VtxBuffer.Size<65536,"TextBox overflowed a 16-bit ImGui draw list");
        require(scripting::ReactRuntime::reset(scripts),"TextBox stress cleanup failed");
    }

    static void canvasGroupsFlattenAndGateInput() {
        ecs::World world; scripting::ScriptingSystem scripts;
        require(scripts.initialize(world,nullptr,"assets") && scripting::ReactRuntime::install(scripts),"Canvas UI runtime failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo'); local e=React.createElement
            __canvasRef=React.createRef(); __nestedCanvasRef=React.createRef(); __canvasButton=React.createRef(); __canvasText=React.createRef(); __canvasScroll=React.createRef()
            __canvasClicks=0; __canvasEnters=0; __canvasLost=0
            __canvasRoot=UI.createRoot({Name='CanvasNativeTest'})
            __canvasRoot:render(e('CanvasGroup',{ref=__canvasRef,Size=UDim2.fromOffset(180,160),Position=UDim2.fromOffset(20,20),BackgroundTransparency=1,GroupTransparency=.5,GroupColor3=Color3.new(.8,1,1)}, {
                Red=e('Frame',{Size=UDim2.fromOffset(100,80),BackgroundColor3=Color3.new(1,0,0),BorderSizePixel=0,ZIndex=1}),
                Blue=e('Frame',{Size=UDim2.fromOffset(100,80),Position=UDim2.fromOffset(50,0),BackgroundColor3=Color3.new(0,0,1),BorderSizePixel=0,ZIndex=2}),
                Nested=e('CanvasGroup',{ref=__nestedCanvasRef,Position=UDim2.fromOffset(100,100),Size=UDim2.fromOffset(40,40),BackgroundColor3=Color3.new(0,1,0),BorderSizePixel=0,GroupTransparency=.25,ZIndex=3}),
                Outside=e('Frame',{Position=UDim2.fromOffset(160,20),Size=UDim2.fromOffset(100,20),BackgroundColor3=Color3.new(1,0,1),BorderSizePixel=0}),
                Button=e('TextButton',{ref=__canvasButton,Position=UDim2.fromOffset(0,90),Size=UDim2.fromOffset(80,30),Text='',BackgroundColor3=Color3.new(1,1,0),AutoButtonColor=false,BorderSizePixel=0,
                    [React.Event.Activated]=function() __canvasClicks+=1 end,[React.Event.MouseEnter]=function() __canvasEnters+=1 end}),
                Text=e('TextBox',{ref=__canvasText,Position=UDim2.fromOffset(0,130),Size=UDim2.fromOffset(80,25),Text='',MultiLine=true,ClearTextOnFocus=false,[React.Event.FocusLost]=function() __canvasLost+=1 end}),
                Scroll=e('ScrollingFrame',{ref=__canvasScroll,Position=UDim2.fromOffset(100,80),Size=UDim2.fromOffset(70,15),CanvasSize=UDim2.fromOffset(70,120),ScrollingDirection=Enum.ScrollingDirection.Y,ScrollBarThickness=3,BackgroundTransparency=1}),
            }))
        )","=CanvasNativeFixture"),"Canvas native fixture failed");
        struct Captured {std::shared_ptr<ImDrawList> list; ImVec2 origin,size;};
        std::unordered_map<uintptr_t,Captured> textures;
        std::unordered_map<const void*,uintptr_t> ids;
        std::vector<const void*> order;
        bool fail=false;
        const scripting::ReactUI::CanvasResolver resolver=[&](const void* key,const ImDrawList& list,float x,float y,float width,float height) {
            order.push_back(key);
            for (const auto& command:list.CmdBuffer) {
                require(!command.UserCallback,"Canvas capture retained an ImGui callback marker");
                require(command.IdxOffset+command.ElemCount<=static_cast<unsigned int>(list.IdxBuffer.Size),"Canvas capture has invalid command indices");
                for (unsigned i=0;i<command.ElemCount;++i) require(list.IdxBuffer[command.IdxOffset+i]+command.VtxOffset<static_cast<unsigned int>(list.VtxBuffer.Size),"Canvas capture has invalid vertex offsets");
            }
            if (fail) return scripting::ReactUI::Image{};
            const auto [found,inserted]=ids.try_emplace(key,0x9000+ids.size());
            textures[found->second]={std::shared_ptr<ImDrawList>(list.CloneOutput(),[](ImDrawList* value){IM_DELETE(value);}),{x,y},{width,height}};
            return scripting::ReactUI::Image{reinterpret_cast<void*>(found->second),width,height};
        };
        std::function<ImVec4(const ImDrawList&,ImVec2,int)> sample;
        sample=[&](const ImDrawList& list,ImVec2 point,int depth) {
            ImVec4 accumulated;
            require(depth<10,"Canvas texture dependency cycle");
            for (const auto& command:list.CmdBuffer) {
                if (point.x<command.ClipRect.x || point.x>=command.ClipRect.z || point.y<command.ClipRect.y || point.y>=command.ClipRect.w) continue;
                for (unsigned i=0;i+2<command.ElemCount;i+=3) {
                    const auto& a=list.VtxBuffer[list.IdxBuffer[command.IdxOffset+i]+command.VtxOffset];
                    const auto& b=list.VtxBuffer[list.IdxBuffer[command.IdxOffset+i+1]+command.VtxOffset];
                    const auto& c=list.VtxBuffer[list.IdxBuffer[command.IdxOffset+i+2]+command.VtxOffset];
                    const float area=(b.pos.y-c.pos.y)*(a.pos.x-c.pos.x)+(c.pos.x-b.pos.x)*(a.pos.y-c.pos.y);
                    if (std::abs(area)<.0001F) continue;
                    const float wa=((b.pos.y-c.pos.y)*(point.x-c.pos.x)+(c.pos.x-b.pos.x)*(point.y-c.pos.y))/area;
                    const float wb=((c.pos.y-a.pos.y)*(point.x-c.pos.x)+(a.pos.x-c.pos.x)*(point.y-c.pos.y))/area,wc=1-wa-wb;
                    if (wa<0 || wb<0 || wc<0) continue;
                    const auto ca=ImGui::ColorConvertU32ToFloat4(a.col),cb=ImGui::ColorConvertU32ToFloat4(b.col),cc=ImGui::ColorConvertU32ToFloat4(c.col);
                    ImVec4 color{ca.x*wa+cb.x*wb+cc.x*wc,ca.y*wa+cb.y*wb+cc.y*wc,ca.z*wa+cb.z*wb+cc.z*wc,ca.w*wa+cb.w*wb+cc.w*wc};
                    if (command.TexRef._TexData==nullptr) if (const auto texture=textures.find(static_cast<uintptr_t>(command.TexRef._TexID));texture!=textures.end()) {
                        const auto& image=texture->second;
                        const auto pixel=sample(*image.list,{image.origin.x+(a.uv.x*wa+b.uv.x*wb+c.uv.x*wc)*image.size.x,image.origin.y+(a.uv.y*wa+b.uv.y*wb+c.uv.y*wc)*image.size.y},depth+1);
                        color={color.x*pixel.x,color.y*pixel.y,color.z*pixel.z,color.w*pixel.w};
                    }
                    accumulated={color.x*color.w+accumulated.x*(1-color.w),color.y*color.w+accumulated.y*(1-color.w),color.z*color.w+accumulated.z*(1-color.w),color.w+accumulated.w*(1-color.w)};
                }
            }
            if (accumulated.w>0) {accumulated.x/=accumulated.w; accumulated.y/=accumulated.w; accumulated.z/=accumulated.w;}
            return accumulated;
        };
        auto& io=ImGui::GetIO(); const int top=lua_gettop(scripts.state());
        const auto frame=[&](ImVec2 mouse=ImVec2(350,250),bool down=false,bool available=true) {
            io.DisplaySize={400,300}; io.AddMousePosEvent(mouse.x,mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left,down); order.clear();
            ImGui::NewFrame(); scripting::ReactUI::draw(scripts,true,{},available ? resolver : scripting::ReactUI::CanvasResolver{}); ImGui::Render();
            require(lua_gettop(scripts.state())==top,"Canvas drawing leaked the Lua stack");
        };
        const auto output=[&](ImVec2 point) {
            for (const auto* window:ImGui::GetCurrentContext()->Windows) if (window->Active && std::string_view(window->Name)=="##ReactGameUI")
                return sample(*window->DrawList,point,0);
            return ImVec4{};
        };
        frame(); frame();
        require(order.size()==2,"Nested CanvasGroup did not submit both flattened surfaces");
        require(scripts.execute("assert(__canvasRef.current.AbsoluteSize==Vector2.new(180,160)); assert(__nestedCanvasRef.current.AbsolutePosition==Vector2.new(120,120))","=CanvasNativeGeometry"),"CanvasGroup geometry failed");
        const auto red=output({30.37F,30.63F}),overlap=output({90.37F,30.63F}),nested=output({130.37F,130.63F}),outside=output({220.37F,50.63F});
        require(std::abs(red.x-.8F)<.02F && red.y<.01F && std::abs(red.w-.5F)<.01F,"CanvasGroup color/opacity did not apply once to its flattened texture");
        require(overlap.x<.01F && overlap.z>.99F && std::abs(overlap.w-.5F)<.01F,"Overlapping opaque children were faded individually or leaked into the main draw list");
        require(nested.y>.99F && std::abs(nested.w-.375F)<.015F && outside.w==0,"Nested CanvasGroup composition order or mandatory clipping failed");
        require(scripts.execute(R"(
            __canvasCorner=Instance.new('UICorner'); __canvasCorner.CornerRadius=UDim.new(0,20); __canvasCorner.Parent=__canvasRef.current
            __canvasGradient=Instance.new('UIGradient'); __canvasGradient.Color=ColorSequence.new(Color3.new(1,0,0),Color3.new(0,0,1)); __canvasGradient.Transparency=NumberSequence.new(.25); __canvasGradient.Parent=__canvasRef.current
        )","=CanvasWholeModifiers"),"Canvas modifiers could not be attached"); frame();
        require(output({21.37F,21.63F}).w==0 && std::abs(output({90.37F,30.63F}).w-.375F)<.015F,"UICorner/UIGradient were not applied to the whole flattened group");
        require(scripts.execute("__canvasCorner:Destroy(); __canvasGradient:Destroy()","=CanvasRemoveModifiers"),"Canvas modifier cleanup failed"); frame();
        frame({50,125}); frame({50,125},true); frame({50,125});
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__canvasClicks==1 and __canvasEnters==1)","=CanvasNativeClick"),"Visible CanvasGroup did not receive input");
        require(scripts.execute("__canvasText.current:CaptureFocus()","=CanvasFocus"),"Canvas TextBox focus request failed"); frame(); frame();
        require(scripts.execute("assert(__canvasText.current:IsFocused()); __canvasRef.current.Interactable=false","=CanvasDisable"),"Canvas TextBox did not focus"); frame(); frame({140,110}); io.AddMouseWheelEvent(0,-1); frame({140,110}); frame({50,125},true); frame({50,125});
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(not __canvasText.current:IsFocused() and __canvasClicks==1 and __canvasLost==1)","=CanvasDisabledInput"),"Ancestor Interactable did not clear focus and block child input");
        require(std::abs(output({90.37F,30.63F}).w-.5F)<.01F,"Interaction gating changed the group's visual opacity");
        require(scripts.execute("assert(__canvasScroll.current.CanvasPosition.Y==0); __canvasRef.current.Interactable=true","=CanvasEnable"),"Disabled CanvasGroup accepted a scroll wheel"); frame({140,110}); io.AddMouseWheelEvent(0,-1); frame({140,110});
        require(scripts.execute("assert(__canvasScroll.current.CanvasPosition.Y>0); __canvasPreviousScroll=__canvasScroll.current.CanvasPosition.Y","=CanvasEnabledScroll"),"Enabled CanvasGroup ignored the scroll wheel");
        frame({50,125}); frame({50,125},true); frame({50,125});
        require(scripts.execute("__canvasRef.current.GroupTransparency=1","=CanvasHideBeforePump") && scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__canvasClicks==1)","=CanvasHiddenQueue"),"A queued click escaped a newly hidden CanvasGroup"); frame({140,110}); io.AddMouseWheelEvent(0,-1); frame({140,110}); frame({50,125},true); frame({50,125});
        require(scripts.execute("assert(__canvasScroll.current.CanvasPosition.Y==__canvasPreviousScroll)","=CanvasHiddenScroll"),"A fully transparent group accepted a scroll wheel");
        require(output({90.37F,30.63F}).w==0,"Fully transparent CanvasGroup retained visible child geometry");
        require(scripts.execute("__canvasRef.current.GroupTransparency=.5","=CanvasShow"),"Canvas show failed"); frame(); frame();
        fail=true; frame({50,125},true); frame({50,125});
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__canvasClicks==1)","=CanvasFailedQueue"),"Failed CanvasGroup allocation allowed a queued activation");
        frame({50,125},true,false); frame({50,125},false,false);
        require(scripting::ReactRuntime::pump(scripts,0) && scripts.execute("assert(__canvasClicks==1)","=CanvasMissingResolver"),"An unavailable CanvasGroup accepted input");
        require(scripting::ReactRuntime::reset(scripts),"Canvas native cleanup failed");
    }

    static void importedScriptDemoHasEditableBehaviors(core::JobSystem& jobs) {
        namespace fs = std::filesystem;
        struct Fixture {
            fs::path root = fs::temp_directory_path() / ("tge-imported-properties-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
        } fixture;
        core::ProjectManager projects(fixture.root / "registry.json");
        std::string error;
        const auto project = projects.createBlank(fixture.root, "EditableGame", error);
        require(project.has_value(), "Could not create import fixture project");
        StateStack stack; stack.setServices(&jobs, nullptr); stack.setProject(&*project);
        GameplayState state(stack); state.onEnter(); state.createScriptDemo();
        require(fs::is_regular_file(project->assetRoot / "scripts/Collectible.luau"), "Imported demo is missing its collectible behavior");
        auto controller = tagged(state, "Collectathon Controller");
        auto* script = state.m_world.getComponent<scripting::ScriptComponent>(controller);
        require(script != nullptr && state.m_scripts.setField(*script, "countdownRate", 0) &&
            state.m_scripts.setField(*script, "tokenCount", 3) && state.m_scripts.setField(*script, "pointsPerToken", 7),
            "Imported game did not expose its gameplay properties");
        const auto before = state.captureSceneSnapshot();
        const auto authored = script->fields;
        state.enterPlayMode(); state.update(1.0/60.0);
        require(state.m_scripts.execute("assert(#Engine.find('ScriptToken')==3)", "=ImportedTokens"), "Imported token count was ignored");
        const auto remaining = state.m_scripts.status().at("remaining");
        state.update(1.0/60.0);
        require(state.m_scripts.status().at("remaining") == remaining, "Imported countdownRate did not freeze the clock");
        require(state.m_scripts.execute("local p=Engine.find('ScriptPlayer')[1]; local t=Engine.find('ScriptToken')[1]; Engine.setPosition(p,Engine.getPosition(t))", "=ImportedPickup"),
            "Could not arrange imported pickup");
        state.update(1.0/60.0);
        require(state.m_scripts.status().value("score", 0)==7, "Imported pointsPerToken was ignored");
        state.stopPlayMode();
        controller = tagged(state, "Collectathon Controller");
        require(state.m_world.getComponent<scripting::ScriptComponent>(controller)->fields==authored,
            "Imported Play changed authored fields");
        const auto componentsByTag = [&](const GameplayState::SceneSnapshot& snapshot) {
            auto components = nlohmann::json::object();
            const auto serialized = nlohmann::json::parse(state.serializeScene(snapshot));
            for (auto entity : serialized.at("entities")) {
                const auto tag = entity.at("tag").at("value").get<std::string>();
                entity.erase("sourceId");
                components[tag] = std::move(entity);
            }
            return components;
        };
        require(componentsByTag(state.captureSceneSnapshot())==componentsByTag(before), "Imported Stop did not restore its authored components");
        require(state.m_selectedEntity==controller && state.m_world.aliveCount()==before.entities.size(), "Imported Stop lost selection or retained runtime objects");
        requireNoScriptErrors(state); state.onExit();
    }

    static void scriptDemoLifecycleAndSceneHistory(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.onEnter();
        const auto initialCount = state.m_world.aliveCount();
        state.createScriptDemo();
        require(state.m_world.aliveCount() == initialCount + 2U, "Script demo did not add controller and React HUD");
        state.createScriptDemo();
        require(state.m_world.aliveCount() == initialCount + 2U, "Repeated script demo request added duplicate sessions");
        ImGui::GetIO().DisplaySize = ImVec2(1280.0F, 720.0F);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10.0F, 10.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 650.0F));
        state.m_scriptWorkspace.render(&state.m_showScriptWorkspace, state.m_selectedEntity, false, false);
        ImGui::Render();
        auto controller = tagged(state, "Collectathon Controller");
        auto ui = tagged(state, "ReactLuauHUD");
        require(state.m_world.isAlive(controller) && state.m_world.isAlive(ui), "Script demo prefabs are missing");
        auto* component = state.m_world.getComponent<scripting::ScriptComponent>(controller);
        require(component != nullptr && component->path == "scripts/Collectathon.luau", "Controller behavior path is missing");
        component->fields["duration"] = 20.0;
        component->fields["tokenCount"] = 6;
        component->fields["pointsPerToken"] = 3;
        component->fields["goal"] = 1000;
        component->fields["title"] = "Preserved script fields";
        const auto expectedFields = component->fields;
        const auto snapshot = state.captureEntitySnapshot(controller);
        require(snapshot.script.has_value() && snapshot.script->fields == expectedFields, "Entity snapshot lost script fields");
        const auto copied = state.duplicateEntity(controller);
        require(state.m_world.getComponent<scripting::ScriptComponent>(copied)->fields == expectedFields, "Duplicate lost script fields");
        state.undoSceneEdit();
        require(tagged(state, "Collectathon Controller_copy") == ecs::kInvalidEntity, "Undo did not remove duplicated behavior");
        state.redoSceneEdit();
        const auto restoredCopy = tagged(state, "Collectathon Controller_copy");
        require(state.m_world.getComponent<scripting::ScriptComponent>(restoredCopy)->fields == expectedFields, "Redo lost duplicated script fields");
        state.undoSceneEdit();
        controller = tagged(state, "Collectathon Controller");
        state.enterPlayMode();
        state.update(1.0 / 60.0);
        require(state.m_scripts.playing(), "Play did not start real scripting runtime");
        require(state.m_scripts.execute("assert(#Engine.find('ScriptPlayer') == 1 and #Engine.find('ScriptToken') == 6)", "=GameplayDemoSpawn"),
                "Behavior Init did not spawn player and configured token wave");
        require(state.m_scripts.status().value("goal", 0) == 1000 && state.m_scripts.status().value("result", std::string{}) == "Playing",
                "Behavior Update did not publish gameplay status");
        require(state.m_scripts.execute("function __findDemoButton(node) if node.type=='TextButton' then return node end; for _,child in ipairs(node.children or {}) do local found=__findDemoButton(child); if found then return found end end end; assert(#__turboUIRoots == 1); assert(__turboUIRoots[1].children[1].type=='ScreenGui'); local button=__findDemoButton(__turboUIRoots[1]); assert(button.props.Text=='React.useState clicks: 0'); table.insert(__turboUIEvents,{instance=button,name='Activated'})", "=GameplayReactSetup"),
                "Play did not mount actual React UI behavior");
        const int originalStack = lua_gettop(state.m_scripts.state());
        require(state.m_scripts.execute("setmetatable(__turboUIRoots[1].children[1].props,{__index=function() error('Native UI must not execute metatables') end})", "=GameplayHostMetatable"),
                "Could not set native host metatable regression fixture");
        ImGui::NewFrame();
        scripting::ReactUI::draw(state.m_scripts);
        ImGui::Render();
        require(lua_gettop(state.m_scripts.state()) == originalStack, "ImGui React traversal leaked VM stack entries");
        state.update(1.0 / 60.0);
        require(state.m_scripts.execute("assert(__findDemoButton(__turboUIRoots[1]).props.Text=='React.useState clicks: 1')", "=GameplayReactEvent"),
                "Queued UI button did not update upstream React state");

        const auto beforeEvents = state.m_scripts.inspect(controller).at("state").value("collisionEvents", 0);
        ecs::CollisionEnterEvent collision;
        collision.contact.entityA = controller;
        collision.contact.entityB = tagged(state, "ScriptPlayer");
        state.m_eventBus.publish(collision);
        require(state.m_scripts.inspect(controller).at("state").value("collisionEvents", 0) == beforeEvents,
                "Collision handler executed script immediately instead of queueing");
        state.m_scripts.dispatchEvents();
        require(state.m_scripts.inspect(controller).at("state").value("collisionEvents", 0) == beforeEvents + 1,
                "Queued collision event did not reach Behavior.OnEvent");
        requireNoScriptErrors(state);

        require(state.m_scripts.execute("table.insert(__turboUIEvents,{instance=__findDemoButton(__turboUIRoots[1]),name='Activated'})", "=GameplayPendingUIEvent"),
                "Could not create pending UI callback fixture");
        state.stopPlayMode();
        require(!state.m_scripts.playing() && state.m_editorMode == GameplayState::EditorMode::Edit, "Stop did not end scripted Play");
        controller = tagged(state, "Collectathon Controller");
        require(state.m_world.getComponent<scripting::ScriptComponent>(controller)->fields == expectedFields, "Stop lost pre-Play script fields");
        require(tagged(state, "ScriptPlayer") == ecs::kInvalidEntity && tagged(state, "ScriptToken") == ecs::kInvalidEntity,
                "Stop retained runtime prefab entities");
        require(state.m_scripts.execute("assert(#__turboUIRoots == 0 and #__turboUIEvents == 0)", "=GameplayStopUI"),
                "Stop retained UI roots or pending stale callbacks");
        state.enterPlayMode();
        state.update(1.0 / 60.0);
        require(state.m_scripts.execute("assert(#__turboUIRoots == 1); assert(__findDemoButton(__turboUIRoots[1]).props.Text=='React.useState clicks: 0')", "=GameplayRestartUI"),
                "Restart retained previous UI hook state or dispatched stale callbacks");
        state.m_simulationRunning = false;
        state.deleteEntity(tagged(state, "ReactLuauHUD"));
        state.update(0.0);
        require(state.m_scripts.execute("assert(#__turboUIRoots == 0)", "=GameplayUIDestroy"),
                "Destroying UI behavior entity did not release its retained React root");
        state.stopPlayMode();
        requireNoScriptErrors(state);
    }

    static void networkTestUsesUnsavedReplicaScene(core::JobSystem& jobs) {
        using Command = editor::EditorCommandId;
        using Json = nlohmann::json;
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        bool running = false;
        std::string launchedScene;
        std::filesystem::path launchedAssets;
        net::NetworkTestOptions launchedOptions;
        unsigned starts = 0;
        unsigned stops = 0;
        stack.setNetworkPlayCallbacks([&](const std::string& scene, const std::filesystem::path& assets,
            const net::NetworkTestOptions& options, std::string&) {
            launchedScene = scene;
            launchedAssets = assets;
            launchedOptions = options;
            ++starts;
            return running = true;
        }, [&] { ++stops; running = false; }, [&] { return running; }, [] { return std::string_view{}; });

        GameplayState state(stack);
        state.m_sceneInitialized = true;
        state.createEditorCamera();
        auto body = addBody(state, "Network Body", {2.0F, 3.0F, 0.0F});
        state.m_world.addComponent<net::NetworkReplica>(body, net::NetworkReplica{91});
        state.m_selectedEntity = body;
        const auto replica = [&](ecs::EntityId entity) { return state.m_world.getComponent<net::NetworkReplica>(entity); };
        const auto findBody = [](Json& scene) -> Json& {
            for (auto& entity : scene.at("entities")) {
                if (entity.value("tag", Json::object()).value("value", "") == "Network Body") return entity;
            }
            throw std::runtime_error("Network scene snapshot is missing the selected entity");
        };

        auto snapshot = state.captureSceneSnapshot();
        auto authored = Json::parse(state.serializeScene(snapshot));
        require(findBody(authored).at("networkReplica").at("key") == 91, "Scene serialization lost the replica key");
        state.restoreSceneSnapshot(state.parseScene(authored));
        body = tagged(state, "Network Body");
        require(replica(body) && replica(body)->key == 91, "Restoring the scene lost the replica key");
        const auto copy = state.duplicateEntity(body);
        require(replica(copy) && replica(copy)->key != 91, "Duplicate did not allocate an independent replica key");
        const auto copyKey = replica(copy)->key;
        state.undoSceneEdit();
        require(tagged(state, "Network Body_copy") == ecs::kInvalidEntity, "Undo retained the duplicated replica");
        state.redoSceneEdit();
        const auto restoredCopy = tagged(state, "Network Body_copy");
        require(replica(restoredCopy) && replica(restoredCopy)->key == copyKey, "Redo changed the copied replica key");
        state.undoSceneEdit();
        body = tagged(state, "Network Body");

        auto invalid = authored;
        findBody(invalid)["networkReplica"]["key"] = -1;
        bool rejectedKey = false;
        try { (void)state.parseScene(invalid); }
        catch (const std::runtime_error&) { rejectedKey = true; }
        require(rejectedKey, "Scene parser accepted an out-of-range replica key");
        state.m_diskScene = authored;
        auto changed = authored;
        findBody(changed)["networkReplica"]["key"] = 92;
        state.applyLiveScene(changed);
        require(replica(body) && replica(body)->key == 92, "Live scene merge discarded the replica key edit");

        auto duplicate = addBody(state, "Invalid Replica", {0.0F, 0.0F, 0.0F});
        state.m_world.addComponent<net::NetworkReplica>(duplicate, net::NetworkReplica{92});
        require(state.m_editorCommands.execute(Command::StartNetworkTest) && starts == 0 &&
            state.m_editorStatusMessage.find("unique") != std::string::npos,
            "Network Test accepted ambiguous authored keys");
        state.m_world.destroyEntity(duplicate);
        state.m_world.getComponent<ecs::Transform>(body)->position.x = 19.0F;
        state.m_editorMode = GameplayState::EditorMode::Play;
        require(!state.m_editorCommands.state(Command::StartNetworkTest).enabled, "Network Test captured the simulated Play world");
        state.m_editorMode = GameplayState::EditorMode::Edit;
        state.m_ribbonState.networkTest = {3, 120, 25};
        require(!state.m_editorCommands.state(Command::StopNetworkTest).enabled, "Network Stop was enabled before launch");
        require(state.m_editorCommands.execute(Command::StartNetworkTest) && starts == 1 && running,
            "Network Test did not dispatch the launch callback");
        auto launch = Json::parse(launchedScene);
        require(findBody(launch).at("networkReplica").at("key") == 92 &&
            findBody(launch).at("transform").at("position").at(0) == 19.0 &&
            launchedAssets == state.editorAssetsRoot(), "Network Test did not send the unsaved scene and project assets");
        require(launchedOptions.clientCount == 3 && launchedOptions.latencyMs == 120 &&
            launchedOptions.lossPercent == 25, "Network Test discarded the selected launch options");
        require(!state.m_editorCommands.execute(Command::StartNetworkTest), "Network Test allowed a second group while running");
        require(state.m_editorCommands.execute(Command::StopNetworkTest) && stops == 1 && !running,
            "Network Stop did not dispatch the group stop callback");
        state.m_ribbonState.networkTest.clientCount = net::MaxNetworkTestClients;
        for (std::size_t i = 0; i < net::MaxReplicas - net::MaxNetworkTestClients; ++i) {
            const auto entity = addBody(state, "Capacity Replica", {0.0F, 0.0F, 0.0F});
            state.m_world.addComponent<net::NetworkReplica>(entity, net::NetworkReplica{100U + static_cast<std::uint32_t>(i)});
        }
        require(state.m_editorCommands.execute(Command::StartNetworkTest) && starts == 1 &&
            state.m_editorStatusMessage.find("room for") != std::string::npos,
            "Network Test exceeded the replica budget for the selected client count");
    }

    static void networkTestCsvPreservesSamples() {
        struct Fixture {
            std::filesystem::path directory;
            ~Fixture() {
                if (directory.empty()) return;
                std::error_code ignored;
                std::filesystem::remove(directory / "run.csv", ignored);
                std::filesystem::remove(directory, ignored);
            }
        } fixture;
        const auto directory = std::filesystem::temp_directory_path() / ("tge-network-test-csv-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory), "Could not create Network Test CSV fixture");
        fixture.directory = directory;
        const auto path = directory / "run.csv";
        const auto write = [&](std::string_view rows) {
            std::ofstream file(path, std::ios::binary);
            file << "\xef\xbb\xbf" << core::NetworkTestCsvHeader << "\r\n" << rows;
            require(static_cast<bool>(file), "Could not write Network Test CSV fixture");
        };
        write("2,4000,1,1,0,0,42,0,0,0,2048,16,0,0\n"
              "1,4250,1,2,100,20,,22.5,,0,,,,\r\n"
              "1,4000,1,1,0,0,0,0,0,0,0,0,1024,16\n");
        std::string error;
        const auto run = core::readNetworkTestCsv(path, error);
        require(run && error.empty() && run->clients.size() == 2, "Could not import both CSV clients");
        const auto& first = run->clients.at(1).points();
        const auto& second = run->clients.at(2).points();
        require(first.size() == 2 && first.front().elapsedMs == 4000 && first.back().elapsedMs == 4250 &&
            first.back().breakBefore && first.back().profile.revision == 2 && first.back().profile.conditions.latencyMs == 100,
            "CSV did not preserve elapsed time, sample order, profile or segment break");
        require(first.front().metrics.rttMs == 0U && first.front().metrics.upload && first.front().metrics.upload->bytesPerSecond == 0.0 &&
            !first.back().metrics.rttMs && !first.back().metrics.upload && first.back().metrics.uplinkLossPercent == 22.5 &&
            !first.back().metrics.downlinkLossPercent && second.size() == 1 && second.front().metrics.rttMs == 42U,
            "CSV did not preserve per-client measurements, missing values and zero values");
        write("1,4000,1,1,0,0,42,0,nan,0,0,0,0,0\n");
        require(!core::readNetworkTestCsv(path, error) && error.find("Line 2: Invalid packet loss") != std::string::npos,
            "CSV accepted nonfinite loss or omitted the invalid row diagnostic");
    }

    static void networkTestSummaryCsvPreservesComparison() {
        namespace fs = std::filesystem;
        struct Fixture {
            fs::path root;
            std::vector<fs::path> files;
            ~Fixture() {
                std::error_code ignored;
                for (const auto& file : files) fs::remove(file, ignored);
                if (!root.empty()) fs::remove(root, ignored);
            }
        } fixture;
        const auto root = fs::temp_directory_path() / ("tge-network-summary-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(root), "Could not create summary CSV fixture");
        fixture.root = root;
        const auto runA = root / fs::path(u8"Run A, \u0434\u0430 & one.csv");
        const auto runB = root / "=Run B.csv";
        const auto alias = root / "source-alias.csv";
        const auto output = root / "summary.csv";
        fixture.files = {runA, runB, alias, output};
        std::ofstream(runA) << "Original A";
        std::ofstream(runB) << "Original B";
        std::ofstream(output) << "Replace me";
        fs::create_hard_link(runA, alias);
        const auto read = [](const fs::path& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        };

        core::NetworkTestSummaryCsv summary{2, 1'250, 31'250, "Last \"30\" <seconds>", runA, runB, {}, {}};
        summary.runA.meanRttMs = 0.0;
        summary.runB.meanRttMs = 12.5;
        summary.runA.meanUplinkLossPercent = 12.5;
        summary.runB.meanUplinkLossPercent = 10.25;
        summary.runA.meanUploadBytesPerSecond = 3'072.0;
        summary.runB.meanUploadBytesPerSecond = 1'024.0;
        summary.runB.meanDownloadPacketsPerSecond = 0.0;

        std::string error;
        require(core::writeNetworkTestSummaryCsv(output, summary, error) && error.empty(), "Summary CSV save failed");
        const auto csv = read(output);
        require(csv.starts_with("client_index,range,range_start_ms,range_end_ms,run_a_file,run_b_file,metric,unit,"
            "run_a,run_b,delta_b_minus_a,delta_unit\r\n") && std::ranges::count(csv, '\n') == 10,
            "Summary CSV lost its schema or metric rows");
        const std::string row = "2,\"Last \"\"30\"\" <seconds>\",1250,31250,\"Run A, \xd0\xb4\xd0\xb0 & one.csv\",\"'=Run B.csv\",";
        const auto hasRow = [&](const char* values) { return csv.find(row + values) != std::string::npos; };
        require(hasRow("mean_rtt,ms,0,12.5,12.5,ms\r\n") && hasRow("p95_rtt,ms,,,,ms\r\n") &&
            hasRow("mean_uplink_loss,%,12.5,10.25,-2.25,pp\r\n") &&
            hasRow("mean_upload_kib_per_second,KiB/s,3,1,-2,KiB/s\r\n") &&
            hasRow("mean_download_packets_per_second,pkt/s,,0,,pkt/s\r\n"),
            "Summary CSV lost selection metadata, a unit, a delta, a missing value or an explicit zero");
        require(!core::writeNetworkTestSummaryCsv(runB, summary, error) && !error.empty() && read(runB) == "Original B" &&
            !core::writeNetworkTestSummaryCsv(alias, summary, error) && !error.empty() && read(runA) == "Original A" &&
            read(alias) == "Original A", "Summary CSV could overwrite an input or its alias");
        require(!core::writeNetworkTestSummaryCsv(root / "missing" / "summary.csv", summary, error) && !error.empty() &&
            read(output) == csv, "Summary CSV did not report an unwritable path");

        const auto reportPng = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v2.PNG");
        const auto reportCsv = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v2.csv");
        const auto sourcePng = root / "=Run B.png";
        fixture.files.insert(fixture.files.end(), {reportPng, reportCsv, sourcePng});
        const std::string png("\x89PNG\r\n\x1a\n\0sample", 15);
        require(!core::writeNetworkTestComparisonReport(reportPng, "invalid image", summary, error) && !error.empty() &&
            !fs::exists(reportPng) && !fs::exists(reportCsv), "Invalid image left report files behind");
        require(core::writeNetworkTestComparisonReport(reportPng, png, summary, error) && error.empty() &&
            read(reportPng) == png && read(reportCsv) == csv, "Report pair changed its stem, image bytes or summary CSV");
        require(!core::writeNetworkTestComparisonReport(sourcePng, png, summary, error) && !error.empty() &&
            !fs::exists(sourcePng) && read(runB) == "Original B", "Report pair could overwrite an input CSV");

        const auto batchBase = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v3.PNG");
        const auto client2Png = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v3-client-2.png");
        const auto client2Csv = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v3-client-2.csv");
        const auto client7Png = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v3-client-7.png");
        const auto client7Csv = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v3-client-7.csv");
        const auto batchSummary = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v3-summary.csv");
        auto batchBaseCsv = batchBase;
        batchBaseCsv.replace_extension(".csv");
        fixture.files.insert(fixture.files.end(), {batchBase, batchBaseCsv, client2Png, client2Csv, client7Png, client7Csv, batchSummary});
        auto client7 = summary;
        client7.clientIndex = 7;
        client7.runA = {};
        std::array batch{core::NetworkTestComparisonReport{client7, png}, core::NetworkTestComparisonReport{summary, png}};
        require(core::writeNetworkTestComparisonReports(batchBase, batch, error) && error.empty() &&
            read(client2Png) == png && read(client7Png) == png && read(client2Csv) == csv &&
            !fs::exists(batchBase) && !fs::exists(batchBaseCsv), "Batch report did not produce both suffixed pairs");
        const auto client7Row = "7" + row.substr(1);
        const auto csv7 = read(client7Csv);
        require(csv7.find(client7Row + "mean_rtt,ms,,12.5,,ms\r\n") != std::string::npos,
            "Batch report lost the common interval or a missing client value");
        require(read(batchSummary) == csv + csv7.substr(csv7.find('\n') + 1),
            "Batch summary did not preserve both clients' rows in client order under one header");

        const auto collisionBase = root / "collision.png";
        const auto occupiedCsv = root / "collision-client-7.csv";
        const auto collision2Png = root / "collision-client-2.png";
        const auto collision2Csv = root / "collision-client-2.csv";
        const auto collision7Png = root / "collision-client-7.png";
        const auto collisionSummary = root / "collision-summary.csv";
        fixture.files.insert(fixture.files.end(), {occupiedCsv, collision2Png, collision2Csv, collision7Png, collisionSummary});
        std::ofstream(occupiedCsv) << "Existing CSV";
        batch[0].pngBytes = png;
        batch[1].pngBytes = png;
        require(!core::writeNetworkTestComparisonReports(collisionBase, batch, error) && !error.empty() &&
            read(occupiedCsv) == "Existing CSV" && !fs::exists(collision2Png) && !fs::exists(collision2Csv) &&
            !fs::exists(collision7Png) && !fs::exists(collisionSummary),
            "Batch report collision left new files or changed an existing file");

        const auto aggregateCollisionBase = root / "aggregate-collision.png";
        const auto aggregateCollisionSummary = root / "aggregate-collision-summary.csv";
        const std::array aggregateCollisionPairs{
            root / "aggregate-collision-client-2.png", root / "aggregate-collision-client-2.csv",
            root / "aggregate-collision-client-7.png", root / "aggregate-collision-client-7.csv"};
        fixture.files.push_back(aggregateCollisionSummary);
        fixture.files.insert(fixture.files.end(), aggregateCollisionPairs.begin(), aggregateCollisionPairs.end());
        std::ofstream(aggregateCollisionSummary) << "Existing summary";
        batch[0].pngBytes = png;
        batch[1].pngBytes = png;
        const auto noClientPairs = [&] {
            return std::ranges::none_of(aggregateCollisionPairs, [](const auto& path) { return fs::exists(path); });
        };
        require(!core::writeNetworkTestComparisonReports(aggregateCollisionBase, batch, error) && !error.empty() &&
            read(aggregateCollisionSummary) == "Existing summary" && noClientPairs(),
            "Batch summary collision left new files or changed the existing summary");

        const auto sourceSummaryBase = root / "source.png";
        const auto sourceSummaryAlias = root / "source-summary.csv";
        const auto sourceClientPng = root / "source-client-2.png";
        fixture.files.insert(fixture.files.end(), {sourceSummaryAlias, sourceClientPng, root / "source-client-2.csv"});
        fs::create_hard_link(runA, sourceSummaryAlias);
        std::array sourceBatch{core::NetworkTestComparisonReport{summary, png}};
        require(!core::writeNetworkTestComparisonReports(sourceSummaryBase, sourceBatch, error) &&
            error.find("source CSVs") != std::string::npos && read(sourceSummaryAlias) == "Original A" &&
            !fs::exists(sourceClientPng), "Batch summary could overwrite an input CSV alias");

        const auto archive = root / fs::path(u8"\u043e\u0442\u0447\u0451\u0442.v4.ZIP");
        const auto archiveStem = archive.stem();
        const auto sibling = [&](std::string_view suffix) {
            auto member = root / archiveStem;
            member += suffix;
            return member;
        };
        const auto archiveSummary = sibling("-summary.csv");
        const auto archive2Png = sibling("-client-2.png");
        const auto archive2Csv = sibling("-client-2.csv");
        const auto archive7Png = sibling("-client-7.png");
        const auto archive7Csv = sibling("-client-7.csv");
        const auto looseHtml = root / "index.html";
        const auto archiveAlias = root / "source.zip";
        fixture.files.insert(fixture.files.end(), {archive, archiveSummary, archive2Png, archive2Csv, archive7Png, archive7Csv, looseHtml, archiveAlias});
        std::ofstream(archiveSummary) << "Existing loose summary";
        const auto png7 = png + '\x7f';
        batch[0].pngBytes = png7;
        batch[1].pngBytes = png;
        batch[0].chartSamples[1] = {{31'250, {.downlinkLossPercent = 0.0,
            .download = net::NetworkTestTrafficRate{512.0, 8.0}}, true}};
        batch[1].chartSamples[0] = {{1'250, {.rttMs = 0, .uplinkLossPercent = 0.0,
            .upload = net::NetworkTestTrafficRate{1'536.0, 12.0}}, false},
            {2'500, {.rttMs = 25, .probeLossPercent = 12.5,
                .download = net::NetworkTestTrafficRate{0.0, 0.0}}, true}};
        batch[1].chartSamples[1] = {{1'500, {.rttMs = 50, .downlinkLossPercent = 25.0,
            .upload = net::NetworkTestTrafficRate{3'072.0, 24.0}}, false}};
        require(core::writeNetworkTestComparisonArchive(archive, batch, error) && error.empty(), "Could not write report ZIP");
        require(read(archiveSummary) == "Existing loose summary" && !fs::exists(archive2Png) && !fs::exists(archive2Csv) &&
            !fs::exists(archive7Png) && !fs::exists(archive7Csv) && !fs::exists(looseHtml),
            "Report ZIP wrote loose files or changed an existing one");
        const auto archiveBytes = read(archive);
        const std::array expectedMembers{
            std::pair{archive7Png.filename().u8string(), png7}, std::pair{archive7Csv.filename().u8string(), csv7},
            std::pair{archive2Png.filename().u8string(), png}, std::pair{archive2Csv.filename().u8string(), csv},
            std::pair{archiveSummary.filename().u8string(), read(batchSummary)}};
        const auto littleEndian = [&](std::size_t position, std::size_t count) {
            require(position <= archiveBytes.size() && count <= archiveBytes.size() - position, "Truncated report ZIP header");
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < count; ++i)
                value |= static_cast<std::uint32_t>(static_cast<unsigned char>(archiveBytes[position + i])) << (i * 8);
            return value;
        };
        const auto memberCount = expectedMembers.size() + 1;
        std::string_view html;
        std::array<bool, expectedMembers.size()> memberSeen{};
        std::size_t cursor = 0;
        for (std::size_t member = 0; member < memberCount; ++member) {
            const auto size = littleEndian(cursor + 18, 4);
            const auto nameSize = littleEndian(cursor + 26, 2);
            const auto extra = littleEndian(cursor + 28, 2);
            require(littleEndian(cursor, 4) == 0x04034b50U && (littleEndian(cursor + 6, 2) & 0x800U) != 0 &&
                littleEndian(cursor + 8, 2) == 0 && littleEndian(cursor + 22, 4) == size,
                "Report ZIP member header lost its format or length");
            cursor += 30;
            require(cursor <= archiveBytes.size() && nameSize + extra <= archiveBytes.size() - cursor &&
                size <= archiveBytes.size() - cursor - nameSize - extra, "Truncated report ZIP member");
            const auto name = std::string_view(archiveBytes).substr(cursor, nameSize);
            cursor += nameSize + extra;
            const auto contents = std::string_view(archiveBytes).substr(cursor, size);
            cursor += size;
            if (name == "index.html") {
                require(html.empty() && !contents.empty(), "Report ZIP has an empty or duplicate HTML member");
                html = contents;
                continue;
            }
            const auto expected = std::ranges::find_if(expectedMembers, [&](const auto& entry) {
                return name == std::string_view(reinterpret_cast<const char*>(entry.first.data()), entry.first.size());
            });
            require(expected != expectedMembers.end(), "Report ZIP member lost its flat UTF-8 name");
            const auto index = static_cast<std::size_t>(expected - expectedMembers.begin());
            require(!memberSeen[index] && contents == expected->second, "Report ZIP duplicated or changed report bytes");
            memberSeen[index] = true;
        }
        require(archiveBytes.size() >= 22 && littleEndian(cursor, 4) == 0x02014b50U &&
            littleEndian(archiveBytes.size() - 22, 4) == 0x06054b50U &&
            littleEndian(archiveBytes.size() - 12, 2) == memberCount, "Report ZIP directory is missing or incomplete");
        require(html.find("Summary") != std::string_view::npos && html.find("<table") != std::string_view::npos &&
            html.find("Run A") != std::string_view::npos && html.find("Run B") != std::string_view::npos &&
            html.find("B - A") != std::string_view::npos && html.find("Client 2") != std::string_view::npos &&
            html.find("Client 7") != std::string_view::npos && html.find("&mdash;") != std::string_view::npos &&
            html.find("+12.50") != std::string_view::npos && html.find("-2.25 pp") != std::string_view::npos,
            "HTML report lost its comparison summary, clients or missing values");
        require(html.find("Last &quot;30&quot; &lt;seconds&gt;") != std::string_view::npos &&
            html.find("Run A, \xd0\xb4\xd0\xb0 &amp; one.csv") != std::string_view::npos &&
            html.find("<seconds>") == std::string_view::npos, "HTML report did not escape its input metadata");
        require(html.find("data:image/png;base64,iVBORw0KGgoAc2FtcGxl\"") != std::string_view::npos &&
            html.find("data:image/png;base64,iVBORw0KGgoAc2FtcGxlfw==\"") != std::string_view::npos,
            "HTML report did not embed both client chart images");
        constexpr std::array chartControls{
            "interactive-charts", "chart-client", "chart-zoom-in", "chart-zoom-out", "chart-reset", "chart-window",
            "chart-status", "chart-rtt", "chart-loss", "chart-traffic", "chart-tooltip", "static-charts",
            "visible-summary", "visible-summary-heading", "visible-summary-window", "visible-summary-rows",
            "visible-summary-download", "visible-summary-download-status"};
        for (const auto* control : chartControls) {
            require(html.find(std::string("id=\"") + control + '"') != std::string_view::npos,
                "HTML report is missing an interactive control, summary, CSV download or PNG fallback");
        }
        require(html.find("id=\"report-run-a\">Run A, \xd0\xb4\xd0\xb0 &amp; one.csv<") != std::string_view::npos &&
            html.find("id=\"report-run-b\">=Run B.csv<") != std::string_view::npos,
            "HTML report did not expose the escaped source filenames for the CSV download");
        const auto staticSummary = html.find("id=\"summary\"");
        const auto staticSummaryEnd = html.find("</section>", staticSummary);
        require(staticSummary != std::string_view::npos && staticSummaryEnd != std::string_view::npos,
            "HTML report did not retain the full interval summary");
        const auto staticRows = html.substr(staticSummary, staticSummaryEnd - staticSummary);
        require(staticRows.find("Client 2") != std::string_view::npos && staticRows.find("Client 7") != std::string_view::npos &&
            staticRows.find("+12.50") != std::string_view::npos && staticRows.find("-2.25 pp") != std::string_view::npos,
            "Static summary lost the all-client exported values");
        require(html.find("<script src=") == std::string_view::npos && html.find("<script>") != std::string_view::npos,
            "HTML report did not embed its interactive chart script");
        const auto dataId = html.find("id=\"chart-data\"");
        require(dataId != std::string_view::npos, "HTML report did not embed chart data");
        const auto dataStart = html.find('>', dataId);
        require(dataStart != std::string_view::npos, "HTML report chart data has no opening tag");
        const auto dataEnd = html.find("</script>", dataStart + 1);
        require(dataEnd != std::string_view::npos, "HTML report chart data has no closing tag");
        const auto chartData = nlohmann::json::parse(html.substr(dataStart + 1, dataEnd - dataStart - 1));
        const auto expectedChartData = nlohmann::json::parse(R"JSON({
            "start":"1250", "end":"31250", "clients":[
                {"id":2,"runs":[
                    [["1250",0,0,0,null,null,1.5,null,12,null],["2500",1,25,null,null,12.5,null,0,null,0]],
                    [["1500",0,50,null,25,null,3,null,24,null]]]},
                {"id":7,"runs":[[],[["31250",1,null,null,0,null,null,0.5,null,8]]]}
            ]})JSON");
        require(chartData == expectedChartData, "HTML report changed selected chart data, units, gaps or client order");

        batch[0].pngBytes = png;
        batch[1].pngBytes = png;
        require(!core::writeNetworkTestComparisonArchive(archive, batch, error) && !error.empty() &&
            read(archive) == archiveBytes, "Report ZIP collision replaced an existing archive");
        fs::create_hard_link(runA, archiveAlias);
        require(!core::writeNetworkTestComparisonArchive(archiveAlias, sourceBatch, error) &&
            error.find("source CSVs") != std::string::npos && read(runA) == "Original A",
            "Report ZIP could overwrite an input CSV alias");
    }

    static void networkTestChangesOneLiveClient() {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;
        struct Fixture {
            std::filesystem::path directory = std::filesystem::temp_directory_path() /
                ("tge-live-network-" + std::to_string(Clock::now().time_since_epoch().count()));
            ~Fixture() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
        } fixture;
        require(std::filesystem::create_directory(fixture.directory), "Could not create the live Network Test fixture");
        std::array<net::NetworkTestProfile, 2> profiles{};
        std::string error;
        require(core::writeNetworkTestProfiles(fixture.directory, profiles, error), "Could not publish Network Test profiles");

        std::array<ecs::World, 3> worlds;
        std::array<scripting::ScriptingSystem, 3> scripts;
        const auto destroy = [](ecs::EntityId) {};
        SceneNetwork server(worlds[0], scripts[0], destroy);
        SceneNetwork first(worlds[1], scripts[1], destroy);
        SceneNetwork second(worlds[2], scripts[2], destroy);
        core::PlayLaunch launch;
        launch.networkControlDirectory = fixture.directory;
        launch.networkRole = core::SceneNetworkRole::Server;
        require(server.start(launch, error), "Could not start live Network Test server");
        launch.networkRole = core::SceneNetworkRole::Client;
        launch.networkEndpoint.port = server.localPort();
        launch.networkClientIndex = 2;
        require(second.start(launch, error), "Could not start Network Test Client 2");

        const auto until = [&](const auto& condition, const char* message) {
            const auto deadline = Clock::now() + 9s;
            while (!condition() && Clock::now() < deadline) {
                first.beginTick();
                second.beginTick();
                server.beginTick();
                server.endTick();
                std::this_thread::sleep_for(2ms);
            }
            require(condition(), message);
        };
        const auto applied = [&](unsigned index, uint32_t revision) {
            const auto client = core::readNetworkTestClientStatus(fixture.directory, index);
            const auto serverProfiles = core::readNetworkTestAppliedProfiles(fixture.directory, 2);
            return client && client->state == net::NetworkTestLinkState::Connected && client->nonce &&
                client->revision == revision && serverProfiles &&
                (*serverProfiles)[index - 1] == net::NetworkTestAppliedProfile{client->nonce, revision};
        };

        until([&] { return applied(2, 1); }, "Client 2 did not receive its initial profile");
        const auto secondPeer = second.localPeer();
        launch.networkClientIndex = 1;
        require(first.start(launch, error), "Could not start Network Test Client 1");
        until([&] { return applied(1, 1); }, "Client 1 did not receive its initial profile");
        const auto firstPeer = first.localPeer();
        require(secondPeer && firstPeer > secondPeer, "Fixture did not connect clients out of window order");
        std::uint64_t firstNonce{};
        until([&] {
            const auto client = core::readNetworkTestClientTelemetry(fixture.directory, 1);
            const auto server = core::readNetworkTestServerTelemetry(fixture.directory, 2);
            if (!client || client->revision != 1 || !client->rttMs || !client->txBytes || !client->rxBytes ||
                !server || !(*server)[0] || (*server)[0]->nonce != client->nonce || !(*server)[0]->outbound.packets) {
                return false;
            }
            firstNonce = client->nonce;
            return client->outbound.dropped == 0 && (*server)[0]->outbound.dropped == 0;
        }, "Initial telemetry did not report RTT and traffic for the same connection");

        profiles[0] = {{40, 0}, 2};
        require(core::writeNetworkTestProfiles(fixture.directory, profiles, error), "Could not change live latency");
        until([&] { return applied(1, 2); }, "Live latency was not applied in both directions");
        require(first.stats().at("latencyMs") == 40 && second.stats().at("latencyMs") == 0 &&
            first.localPeer() == firstPeer && second.localPeer() == secondPeer && applied(2, 1),
            "Live latency affected the wrong client or restarted a connection");

        profiles[0] = {{40, 100}, 3};
        require(core::writeNetworkTestProfiles(fixture.directory, profiles, error), "Could not change live loss");
        until([&] { return applied(1, 3); }, "Live loss was not applied in both directions");
        until([&] {
            const auto firstSample = core::readNetworkTestClientTelemetry(fixture.directory, 1);
            const auto secondSample = core::readNetworkTestClientTelemetry(fixture.directory, 2);
            const auto serverSamples = core::readNetworkTestServerTelemetry(fixture.directory, 2);
            return firstSample && firstSample->nonce == firstNonce && firstSample->revision == 3 &&
                firstSample->outbound.dropped > 0 && secondSample && secondSample->revision == 1 &&
                secondSample->outbound.dropped == 0 && serverSamples && (*serverSamples)[0] && (*serverSamples)[1] &&
                (*serverSamples)[0]->nonce == firstNonce && (*serverSamples)[0]->revision == 3 &&
                (*serverSamples)[0]->outbound.dropped > 0 && (*serverSamples)[1]->outbound.dropped == 0;
        }, "Measured drops did not stay isolated to the impaired client");
        until([&] {
            const auto status = core::readNetworkTestClientStatus(fixture.directory, 1);
            return status && status->state == net::NetworkTestLinkState::TimedOut;
        }, "Client 1 did not time out under total loss");
        require(!first.connected() && second.connected() && second.localPeer() == secondPeer &&
            second.stats().at("lossPercent") == 0, "Total loss interrupted the healthy client");

        profiles[0] = {{0, 0}, 4};
        require(core::writeNetworkTestProfiles(fixture.directory, profiles, error), "Could not restore live conditions");
        until([&] { return applied(1, 4); }, "Client 1 did not recover without a restart");
        until([&] {
            const auto sample = core::readNetworkTestClientTelemetry(fixture.directory, 1);
            return sample && sample->nonce != firstNonce && sample->revision == 4 && sample->rttMs;
        }, "Telemetry did not switch to the recovered connection");
        require(first.localPeer() != firstPeer && second.localPeer() == secondPeer &&
            first.stats().at("lossPercent") == 0 && applied(2, 1), "Recovery changed the unaffected peer or profile");
    }

    static void ribbonCommandsPreserveHistoryAndPlay(core::JobSystem& jobs) {
        using Command = editor::EditorCommandId;
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        state.createEditorCamera();
        auto body = addBody(state, "Command Body", {2.0F, 3.0F, 0.0F});
        state.m_selectedEntity = body;
        require(state.m_scripts.initialize(state.m_world, &jobs, state.editorAssetsRoot()), "Command VM initialization failed");
        scripting::ReactRuntime::install(state.m_scripts);
        state.m_scripts.registerModule("CommandCounter", "return {Init=function(self) self.state.ticks=0 end, Update=function(self) self.state.ticks+=1 end}");
        scripting::ScriptComponent script;
        script.path = "CommandCounter";
        state.m_world.addComponent<scripting::ScriptComponent>(body, script);
        const auto initialCount = state.m_world.aliveCount();
        for (const auto& definition : editor::EditorCommandRegistry::definitions()) (void)state.m_editorCommands.state(definition.id);
        require(state.m_world.aliveCount() == initialCount && state.m_undoStack.empty(), "Querying command state mutated the scene");
        require(state.m_editorCommands.execute(Command::Duplicate), "Duplicate command failed");
        require(state.m_world.aliveCount() == initialCount + 1 && state.m_undoStack.size() == 1, "Duplicate did not create exactly one history entry");
        require(state.m_editorCommands.execute(Command::Undo) && state.m_world.aliveCount() == initialCount, "Command Undo did not restore entity count");
        require(state.m_editorCommands.execute(Command::Redo) && state.m_world.aliveCount() == initialCount + 1, "Command Redo did not restore duplicate");
        require(state.m_editorCommands.execute(Command::Undo), "Could not remove duplicate before Play");
        body = tagged(state, "Command Body");
        state.m_selectedEntity = body;

        state.m_undoStack.clear(); state.m_redoStack.clear();
        state.beginSceneEdit("Pending Inspector", state.captureSceneSnapshot(), body);
        state.m_world.getComponent<ecs::Transform>(body)->position.x = 12.0F;
        state.m_pendingSceneEdit->changed = true;
        require(state.m_editorCommands.state(Command::Undo).enabled, "Pending inspector edit disabled Undo");
        require(state.m_editorCommands.execute(Command::Undo), "Pending inspector Undo failed");
        body = tagged(state, "Command Body");
        require(state.m_world.getComponent<ecs::Transform>(body)->position.x == 2.0F, "Undo skipped the pending inspector edit");
        require(state.m_editorCommands.execute(Command::Redo), "Pending inspector edit was not redoable");
        require(state.m_editorCommands.execute(Command::Undo), "Pending inspector restoration failed");
        body = tagged(state, "Command Body"); state.m_selectedEntity = body;
        state.m_undoStack.clear(); state.m_redoStack.clear();
        require(state.m_editorCommands.execute(Command::ToggleGravity), "Gravity command failed");
        require(state.m_undoStack.size() == 1, "Property command lost history");
        require(state.m_editorCommands.execute(Command::ToggleGravity), "Could not disable gravity again");
        const auto editorHistory = state.m_undoStack.size();
        require(state.m_editorCommands.execute(Command::Play), "Command Play failed");
        require(!state.m_editorCommands.state(Command::SaveScene).enabled && !state.m_editorCommands.execute(Command::SaveScene), "Registry allowed saving the temporary Play scene");
        require(state.m_editorCommands.execute(Command::ToggleStatic), "Runtime property edit failed");
        require(state.m_editorCommands.execute(Command::Undo) && state.m_simulationRunning, "Runtime command Undo stopped simulation");
        require(state.m_editorCommands.execute(Command::Pause), "Pause command failed");
        require(state.m_editorCommands.execute(Command::Redo) && !state.m_simulationRunning, "Runtime command Redo resumed paused simulation");
        body = tagged(state, "Command Body");
        const auto ticks = state.m_scripts.inspect(body).at("state").at("ticks").get<int>();
        require(state.m_editorCommands.execute(Command::StepTick), "Step Tick was disabled while paused");
        require(!state.m_editorCommands.execute(Command::StepTick), "A second command queued the same pending tick twice");
        state.update(0.0);
        require(state.m_scripts.inspect(body).at("state").at("ticks").get<int>() == ticks + 1 && !state.m_simulationRunning, "Step Tick did not advance exactly one paused VM tick");
        require(state.m_editorCommands.execute(Command::Stop), "Stop command failed");
        body = tagged(state, "Command Body");
        require(state.m_editorMode == GameplayState::EditorMode::Edit && !state.m_scripts.playing() &&
            !state.m_world.getComponent<ecs::Rigidbody>(body)->isStatic && state.m_undoStack.size() == editorHistory,
            "Command Stop did not restore the pre-Play scene/history");
        const auto stale = state.m_world.createEntity();
        state.m_world.destroyEntity(stale);
        state.m_selectedEntity = stale;
        require(!state.m_editorCommands.state(Command::Delete).enabled && !state.m_editorCommands.execute(Command::ToggleStatic), "Registry accepted a stale entity handle");
    }

    static void ribbonDockedGeometry(core::JobSystem& jobs) {
        StateStack stack; stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        state.m_showHierarchyPanel = true;
        state.m_showInspectorPanel = true;
        state.m_hierarchyDockSlot = EditorDockSlot::LeftTop;
        state.m_inspectorDockSlot = EditorDockSlot::RightTop;
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1280.0F, 720.0F};
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        for (const bool compact : {false, true}) {
            state.m_ribbonState.compact = compact;
            ImGui::NewFrame();
            state.renderMainMenuBar();
            state.renderToolbar();
            state.renderHierarchyPanel();
            state.renderInspectorPanel();
            ImGui::Render();
            const auto* ribbon = ImGui::FindWindowByName("Toolbar");
            const auto* hierarchy = ImGui::FindWindowByName("Scene Hierarchy");
            const auto* inspector = ImGui::FindWindowByName("Inspector");
            require(ribbon && hierarchy && inspector, "Docked ribbon/panels did not render");
            require(ribbon->Pos.y + ribbon->Size.y <= hierarchy->Pos.y &&
                ribbon->Pos.y + ribbon->Size.y <= inspector->Pos.y, "Ribbon overlapped the docked scene panels");
            require(state.m_world.aliveCount() == 0 && state.m_undoStack.empty(), "Rendering the ribbon mutated the scene");
        }
    }

    static void referenceEditorLayoutGeometry(core::JobSystem& jobs) {
        struct Fixture {
            std::filesystem::path root = std::filesystem::temp_directory_path() / ("tge-reference-layout-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
        } fixture;
        std::filesystem::create_directory(fixture.root);
        core::ProjectManager projects(fixture.root / "recent.json");
        std::string error;
        auto project = projects.createBlank(fixture.root, "ReferenceLayout", error);
        require(project.has_value(), "Reference layout project fixture failed");
        StateStack stack; stack.setServices(&jobs, nullptr); stack.setProject(&*project);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        state.resetEditorLayout();
        require(state.m_showToolbarPanel && state.m_showViewportPanel && state.m_showStatusPanel && state.m_showAssistant &&
            !state.m_showHierarchyPanel && !state.m_showProjectPanel && !state.m_showInspectorPanel && !state.m_showStatsPanel,
            "Reset Layout did not restore the reference viewport/Assistant workspace");
        require(state.m_hierarchyDockSlot == EditorDockSlot::RightTop &&
            state.m_inspectorDockSlot == EditorDockSlot::RightBottom && state.m_statsDockSlot == EditorDockSlot::LeftTop &&
            state.m_projectDockSlot == EditorDockSlot::LeftBottom, "Reset Layout did not restore the reference panel slots");
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1280.0F, 720.0F};
        io.DeltaTime = 1.0F / 60.0F;
        io.MousePos = {-1000.0F, -1000.0F};
        io.MouseDown[0] = io.MouseDown[1] = false;
        platform::InputManager::reset();
        ImGui::ClearActiveID();
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        renderer::Renderer renderer;
        struct Rect { ImVec2 pos, size; };
        struct Geometry { Rect viewport, assistant, ribbon, hierarchy, inspector; };
        const auto rect = [](const char* name) {
            const auto* window = ImGui::FindWindowByName(name);
            require(window && window->Active, "Expected editor layout window did not render");
            return Rect{window->Pos, window->Size};
        };
        const auto draw = [&]() {
            state.m_editorLayoutDirty = false;
            ImGui::NewFrame();
            state.renderEditorUi(renderer);
            ImGui::Render();
            Geometry result{rect("Viewport"), {}, {}, {}, {}};
            if (state.m_showAssistant) result.assistant = rect("Assistant");
            if (state.m_showToolbarPanel) result.ribbon = rect("Toolbar");
            if (state.m_showHierarchyPanel) result.hierarchy = rect("Scene Hierarchy");
            if (state.m_showInspectorPanel) result.inspector = rect("Inspector");
            return result;
        };
        const auto near = [](const float a, const float b) { return std::abs(a - b) < 1.1F; };
        const auto right = [](const Rect& rectangle) { return rectangle.pos.x + rectangle.size.x; };
        const auto bottom = [](const Rect& rectangle) { return rectangle.pos.y + rectangle.size.y; };
        const auto reference = draw();
        const auto origin = ImGui::GetMainViewport()->Pos;
        const auto extent = ImGui::GetMainViewport()->Size;
        require(near(reference.viewport.pos.x, origin.x), "Hidden left sidebars left an empty viewport column");
        require(near(right(reference.viewport), reference.assistant.pos.x) && near(right(reference.assistant), origin.x + extent.x),
            "Reference viewport did not reserve a flush right Assistant dock");
        require(near(reference.viewport.pos.y, reference.assistant.pos.y) && near(bottom(reference.viewport), bottom(reference.assistant)),
            "Assistant and viewport did not share the dock's vertical bounds");
        require(bottom(reference.ribbon) <= reference.viewport.pos.y + 1.1F && bottom(reference.ribbon) <= reference.assistant.pos.y + 1.1F,
            "Reference ribbon overlapped the viewport or Assistant");

        state.m_showHierarchyPanel = state.m_showInspectorPanel = true;
        const auto columns = draw();
        require(near(columns.viewport.pos.x, origin.x) && near(columns.viewport.size.x, reference.viewport.size.x) &&
            near(right(columns.viewport), columns.hierarchy.pos.x) && near(columns.hierarchy.pos.x, columns.inspector.pos.x) &&
            near(columns.inspector.pos.x, columns.assistant.pos.x) && near(right(columns.assistant), origin.x + extent.x),
            "Explorer, Properties and Assistant did not share one flush right rail");
        require(near(columns.hierarchy.pos.y, columns.viewport.pos.y) && near(bottom(columns.hierarchy), columns.inspector.pos.y) &&
            near(bottom(columns.inspector), columns.assistant.pos.y) && near(bottom(columns.assistant), bottom(columns.viewport)),
            "Right rail panels did not stack Explorer above Properties above Assistant");
        state.m_showHierarchyPanel = false;
        const auto noExplorer = draw();
        require(near(noExplorer.viewport.size.x, columns.viewport.size.x) && near(noExplorer.inspector.pos.y, columns.viewport.pos.y) &&
            near(bottom(noExplorer.inspector), noExplorer.assistant.pos.y), "Properties did not reclaim hidden Explorer's vertical space");
        state.m_showInspectorPanel = false;
        const auto noColumns = draw();
        require(near(noColumns.viewport.size.x, reference.viewport.size.x) &&
            near(noColumns.assistant.pos.y, noColumns.viewport.pos.y) && near(bottom(noColumns.assistant), bottom(noColumns.viewport)),
            "Assistant did not reclaim the rail after the last scene panel was hidden");

        state.m_hierarchyDockSlot = EditorDockSlot::LeftTop; state.m_showHierarchyPanel = true;
        const auto savedLeft = draw();
        require(near(savedLeft.hierarchy.pos.x, origin.x) && near(right(savedLeft.hierarchy), savedLeft.viewport.pos.x) &&
            near(savedLeft.viewport.size.x + savedLeft.hierarchy.size.x, reference.viewport.size.x),
            "A saved left-docked Explorer no longer reserves its own sidebar");
        state.m_showHierarchyPanel = false;
        const auto noLeft = draw();
        require(near(noLeft.viewport.pos.x, origin.x) && near(noLeft.viewport.size.x, reference.viewport.size.x),
            "Hiding a saved left panel did not reclaim its viewport width");

        state.m_showHierarchyPanel = state.m_showInspectorPanel = true;
        state.m_hierarchyDockSlot = state.m_inspectorDockSlot = EditorDockSlot::Floating;
        require(!state.hasDockedPanel(EditorDockSlot::LeftTop) && !state.hasDockedPanel(EditorDockSlot::RightTop) &&
            !state.hasDockedPanel(EditorDockSlot::RightBottom),
            "Floating panels still reserve docked sidebar columns");
        const auto floating = draw();
        require(near(floating.viewport.pos.x, reference.viewport.pos.x) && near(floating.viewport.size.x, reference.viewport.size.x),
            "Floating scene panels narrowed the docked viewport");
        state.m_showHierarchyPanel = state.m_showInspectorPanel = false;
        state.m_showAssistant = false;
        const auto noAssistant = draw();
        require(near(noAssistant.viewport.pos.x, origin.x) && near(noAssistant.viewport.size.x, extent.x),
            "Hiding Assistant left a reserved right column");
        state.m_showToolbarPanel = false;
        const auto noRibbon = draw();
        const auto* mainMenu = ImGui::FindWindowByName("##MainMenuBar");
        if (!mainMenu || !mainMenu->Active || !near(noRibbon.viewport.pos.y, origin.y + mainMenu->Size.y)) {
            throw std::runtime_error("Hidden ribbon left unused space above the actual native menu bar: viewportY=" +
                std::to_string(noRibbon.viewport.pos.y) + " originY=" + std::to_string(origin.y) +
                " menuHeight=" + std::to_string(mainMenu ? mainMenu->Size.y : -1.0F) +
                " globalFrameHeight=" + std::to_string(ImGui::GetFrameHeight()));
        }
        state.m_showStatusPanel = false;
        const auto noBars = draw();
        require(near(bottom(noBars.viewport), origin.y + extent.y), "Hidden status bar left unused space below the viewport");
        require(state.m_world.aliveCount() == 0 && state.m_undoStack.empty(), "Layout rendering mutated the scene");
    }

    static void renderedPanelsPreserveWidgetHistory(core::JobSystem& jobs) {
        namespace fs = std::filesystem;
        struct Fixture {
            fs::path root = fs::temp_directory_path() / ("tge-rendered-panels-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() {
                if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("tge-rendered-panels-")) {
                    std::error_code ignored; fs::remove_all(root, ignored);
                }
            }
        } fixture;
        require(fs::create_directory(fixture.root), "Cannot create rendered-panel fixture");
        core::ProjectManager projects(fixture.root / "recent.json");
        std::string error;
        auto project = projects.createBlank(fixture.root, "RenderedPanels", error);
        require(project.has_value(), "Cannot create isolated rendered-panel project");
        const auto read = [](const fs::path& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        };
        const auto diskScene = read(project->scenePath);

        struct Context {
            ImGuiContext* previous{ImGui::GetCurrentContext()};
            ImGuiContext* owned{ImGui::CreateContext()};
            Context() { ImGui::SetCurrentContext(owned); }
            ~Context() { ImGui::DestroyContext(owned); ImGui::SetCurrentContext(previous); }
        } gui;
        editor::applyEditorTheme();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = {1280.0F, 900.0F}; io.DeltaTime = 1.0F / 60.0F;
        require(editor::configureEditorFonts(io, fs::path(__FILE__).parent_path().parent_path() / "assets"),
            "Cannot load native panel test fonts");
        unsigned char* pixels{}; int atlasWidth{}, atlasHeight{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
        require(pixels && atlasWidth > 0 && atlasHeight > 0, "Cannot build native panel font atlas");
        io.Fonts->SetTexID(static_cast<ImTextureID>(0xFFFF));

        StateStack stack; stack.setServices(&jobs, nullptr); stack.setProject(&*project);
        GameplayState state(stack); state.m_sceneInitialized = true;
        state.m_showHierarchyPanel = state.m_showInspectorPanel = true;
        state.m_showToolbarPanel = state.m_showStatusPanel = state.m_showAssistant = false;
        state.m_showStatsPanel = state.m_showProjectPanel = state.m_showViewportPanel = false;
        state.m_hierarchyDockSlot = EditorDockSlot::LeftTop; state.m_inspectorDockSlot = EditorDockSlot::RightTop;
        state.m_editorLeftWidth = 300.0F; state.m_editorRightWidth = 400.0F;
        state.m_editorLayoutLocked = true; state.m_resetEditorLayout = true;
        const auto mesh = state.m_world.createEntity();
        state.m_world.addComponent<ecs::Tag>(mesh, ecs::Tag{"PanelMesh"});
        state.m_world.addComponent<ecs::Transform>(mesh);
        state.m_world.addComponent<ecs::MeshRenderer>(mesh);
        const auto other = state.m_world.createEntity();
        state.m_world.addComponent<ecs::Tag>(other, ecs::Tag{"OtherMesh"});
        ecs::Transform otherTransform; otherTransform.position.x = 7.0F;
        state.m_world.addComponent<ecs::Transform>(other, otherTransform);
        ecs::MeshRenderer otherMesh; otherMesh.visible = false;
        state.m_world.addComponent<ecs::MeshRenderer>(other, otherMesh);
        state.m_selectedEntity = mesh;

        const auto near = [](const float a, const float b) { return std::abs(a - b) <= 0.6F; };
        const auto child = [](ImGuiWindow* parent, const char* label) -> ImGuiWindow* {
            const ImGuiID id = ImHashStr(label, 0, parent->ID);
            for (auto* window : ImGui::GetCurrentContext()->Windows) {
                if (window->Active && window->ParentWindow == parent && window->ChildId == id) { return window; }
            }
            return nullptr;
        };
        const auto validDrawData = [] {
            const auto* data = ImGui::GetDrawData();
            require(data && data->Valid && data->TotalVtxCount > 0, "Rendered panels produced no draw data");
            for (const auto* list : data->CmdLists) {
                for (const auto& vertex : list->VtxBuffer) {
                    require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) &&
                        std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y), "Nonfinite native-panel draw vertex");
                }
                for (const auto& command : list->CmdBuffer) {
                    const auto& clip = command.ClipRect;
                    require(std::isfinite(clip.x) && std::isfinite(clip.y) && std::isfinite(clip.z) && std::isfinite(clip.w) &&
                        clip.x <= clip.z && clip.y <= clip.w, "Invalid native-panel clip rectangle");
                    require(static_cast<std::size_t>(command.IdxOffset) + command.ElemCount <=
                        static_cast<std::size_t>(list->IdxBuffer.Size), "Native-panel command exceeds index buffer");
                    for (unsigned i = command.IdxOffset; i < command.IdxOffset + command.ElemCount; ++i) {
                        require(static_cast<std::size_t>(list->IdxBuffer[i]) + command.VtxOffset <
                            static_cast<std::size_t>(list->VtxBuffer.Size), "Native-panel index exceeds vertex buffer");
                    }
                }
            }
        };
        ImGuiID visibleId{}; ImVec2 visiblePoint{};
        const ImVec2 away{-1000.0F, -1000.0F};
        const auto frame = [&](const ImVec2 mouse, const bool down) {
            io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            auto& context = *ImGui::GetCurrentContext();
            const int styles = context.StyleVarStack.Size, colors = context.ColorStack.Size;
            const int fonts = context.FontStack.Size, windows = context.CurrentWindowStack.Size;
            const int tables = context.TablesTempDataStacked;
            const auto* activeTable = context.CurrentTable;
            const auto* host = context.CurrentWindow; const int ids = host->IDStack.Size;
            state.renderHierarchyPanel(); state.renderInspectorPanel();
            require(context.StyleVarStack.Size == styles && context.ColorStack.Size == colors &&
                context.FontStack.Size == fonts && context.CurrentWindowStack.Size == windows &&
                context.TablesTempDataStacked == tables && context.CurrentTable == activeTable &&
                context.CurrentWindow == host && host->IDStack.Size == ids,
                "Native panel rendering leaked an ImGui window, table, font, style or ID scope");
            auto* hierarchy = ImGui::FindWindowByName("Scene Hierarchy");
            auto* inspector = ImGui::FindWindowByName("Inspector");
            require(hierarchy && hierarchy->Active && inspector && inspector->Active, "Native panels did not render");
            auto* hierarchyBody = child(hierarchy, "##PanelBody");
            auto* inspectorBody = child(inspector, "##PanelBody");
            require(hierarchyBody && inspectorBody, "Native panels lost their body child scopes");
            require(near(hierarchyBody->Pos.y - hierarchy->DC.CursorStartPos.y, editor::kPanelTitleHeight) &&
                near(inspectorBody->Pos.y - inspector->DC.CursorStartPos.y, editor::kPanelTitleHeight),
                "Flat panel title bars do not match the measured reference height");
            auto* hierarchyList = child(hierarchyBody, "HierarchyList");
            const float expectedRowsHeight = static_cast<float>(state.m_world.aliveCount()) * editor::kPanelRowHeight;
            if (!hierarchyList || !near(hierarchyList->DC.CursorMaxPos.y - hierarchyList->DC.CursorStartPos.y,
                expectedRowsHeight)) {
                throw std::runtime_error("Explorer rows do not use the reference pitch: frame=" + std::to_string(context.FrameCount) +
                    " startY=" + std::to_string(hierarchyList ? hierarchyList->DC.CursorStartPos.y : -1.0F) +
                    " maxY=" + std::to_string(hierarchyList ? hierarchyList->DC.CursorMaxPos.y : -1.0F) +
                    " expected=" + std::to_string(expectedRowsHeight) +
                    " entities=" + std::to_string(state.m_world.aliveCount()) +
                    " previousLine=" + std::to_string(hierarchyList ? hierarchyList->DC.PrevLineSize.y : -1.0F) +
                    " listHeight=" + std::to_string(hierarchyList ? hierarchyList->Size.y : -1.0F) +
                    " skip=" + std::to_string(hierarchyList ? hierarchyList->SkipItems : false) +
                    " explorerFont=" + std::to_string(editor::kExplorerFontSize));
            }

            const ImGuiID meshTableId = ImHashStr("##Properties", 0, ImHashStr("Mesh Renderer", 0, inspectorBody->ID));
            auto* meshTable = context.Tables.GetByKey(meshTableId);
            require(meshTable && meshTable->OuterWindow == inspectorBody && meshTable->ColumnsCount == 2,
                "Rendered Mesh Renderer property table is missing");
            int propertyTables = 0;
            for (int i = 0; i < context.Tables.GetMapSize(); ++i) {
                const auto* table = context.Tables.TryGetMapData(i);
                if (!table || table->OuterWindow != inspectorBody || table->LastFrameActive != context.FrameCount) { continue; }
                ++propertyTables;
                require(near(table->RowPosY2 - table->RowPosY1, editor::kPropertyRowHeight) && table->CurrentRow >= 0 &&
                    near(table->OuterRect.GetHeight(), static_cast<float>(table->CurrentRow + 1) * editor::kPropertyRowHeight),
                    "Inspector properties no longer fit reference-height table rows");
            }
            require(propertyTables == 3, "Expected Data, Transform and Mesh Renderer property sections did not render");
            visibleId = ImHashStr("##Value", 0, ImHashStr("Visible", 0, meshTableId));
            visiblePoint = ImVec2(meshTable->Columns[1].WorkMinX + 8.0F,
                meshTable->OuterRect.Min.y + editor::kPropertyRowHeight * .5F);
            ImGui::Render(); validDrawData();
        };
        for (int i = 0; i < 3; ++i) { frame(away, false); }
        require(state.m_undoStack.empty() && !state.m_pendingSceneEdit && state.m_world.aliveCount() == 2,
            "Passive rendered panels changed the scene or history");
        frame(visiblePoint, false);
        require(ImGui::GetCurrentContext()->HoveredId == visibleId, "Mesh Visible locator did not hit the real rendered checkbox");
        frame(visiblePoint, true);
        require(state.m_world.getComponent<ecs::MeshRenderer>(state.m_selectedEntity)->visible && state.m_undoStack.empty() &&
            state.m_pendingSceneEdit && !state.m_pendingSceneEdit->changed,
            "Checkbox press lost actual-widget activation or edited the scene before release");
        frame(visiblePoint, false);
        require(!state.m_world.getComponent<ecs::MeshRenderer>(state.m_selectedEntity)->visible &&
            state.m_undoStack.size() == 1 && !state.m_pendingSceneEdit &&
            state.m_undoStack.back().label == "Edit Mesh Visibility", "Rendered checkbox did not commit exactly one visibility transaction");
        frame(away, false);
        require(state.m_undoStack.size() == 1 && !state.m_pendingSceneEdit, "Idle rendering repeated or retained a completed checkbox edit");
        state.undoSceneEdit();
        const auto restored = tagged(state, "PanelMesh");
        require(state.m_world.getComponent<ecs::MeshRenderer>(restored)->visible && state.m_redoStack.size() == 1 &&
            !state.m_world.getComponent<ecs::MeshRenderer>(tagged(state, "OtherMesh"))->visible &&
            state.m_world.getComponent<ecs::Transform>(tagged(state, "OtherMesh"))->position.x == 7.0F,
            "Undo did not restore visible state independently of the other entity");
        frame(away, false);
        state.redoSceneEdit();
        require(!state.m_world.getComponent<ecs::MeshRenderer>(tagged(state, "PanelMesh"))->visible &&
            state.m_undoStack.size() == 1 && state.m_redoStack.empty(), "Redo did not reapply the final visibility value");
        frame(away, false);
        require(read(project->scenePath) == diskScene, "Rendering property panels unexpectedly saved the project scene");
    }

    static void renderedScriptPropertiesUseRealWidgets(core::JobSystem& jobs) {
        struct Context {
            ImGuiContext* previous{ImGui::GetCurrentContext()};
            ImGuiContext* owned{ImGui::CreateContext()};
            Context() { ImGui::SetCurrentContext(owned); }
            ~Context() { ImGui::DestroyContext(owned); ImGui::SetCurrentContext(previous); }
        } gui;
        editor::applyEditorTheme();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = {900, 900}; io.DeltaTime = 1.0F / 60.0F;
        io.Fonts->AddFontDefault();
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        StateStack stack; stack.setServices(&jobs, nullptr);
        GameplayState state(stack); state.m_sceneInitialized = true;
        require(state.m_scripts.initialize(state.m_world, &jobs, std::filesystem::temp_directory_path()), "Widget test VM initialization failed");
        state.m_scripts.registerModule("WidgetFields", "return {fields={active={type='bool',default=true,label='Active'},speed={type='number',default=2,min=0,max=20,step=0.25,label='Speed'}}}");
        const auto entity = state.m_world.createEntity();
        state.m_world.addComponent<ecs::Tag>(entity, ecs::Tag{"Widget Object"});
        state.m_world.addComponent<scripting::ScriptComponent>(entity, scripting::ScriptComponent{"WidgetFields", true, {{"active",true},{"speed",2}}});
        state.m_showInspectorPanel = false; state.m_inspectorFilter = "old unrelated filter";
        state.inspectEntity(entity);
        require(state.m_showInspectorPanel && state.m_selectedEntity == entity && state.m_inspectorFilter.empty(),
            "Selecting a scripted object did not reveal its editable properties");
        ImVec2 booleanPoint{}, speedPoint{}, resetPoint{};
        ImGuiID booleanId{}, speedId{};
        const ImVec2 away{-1000, -1000};
        const auto frame = [&](const ImVec2 mouse, const bool down) {
            io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({700, 820});
            ImGui::Begin("Script Properties Widgets", nullptr, ImGuiWindowFlags_NoSavedSettings);
            auto& context = *ImGui::GetCurrentContext();
            const int ids = context.CurrentWindow->IDStack.Size, styles = context.StyleVarStack.Size;
            const int colors = context.ColorStack.Size, tables = context.TablesTempDataStacked;
            const ImGuiID tableId = ImHashStr("##Properties", 0, ImHashStr("Luau Behavior", 0, context.CurrentWindow->ID));
            state.renderScriptInspector(state.m_selectedEntity);
            require(context.CurrentWindow->IDStack.Size == ids && context.StyleVarStack.Size == styles &&
                context.ColorStack.Size == colors && context.TablesTempDataStacked == tables,
                "Script property widgets leaked an ImGui ID, style, color or table scope");
            const auto* table = context.Tables.GetByKey(tableId);
            require(table && table->ColumnsCount == 2, "Script field property table was not rendered");
            const auto fieldSeed = [&](const char* name) {
                const auto key = ImHashData(name, std::strlen(name));
                return ImHashStr(name, 0, ImHashData(&key, sizeof(key), tableId));
            };
            booleanId = ImHashStr("##Value", 0, fieldSeed("active"));
            speedId = ImHashStr("##Value", 0, fieldSeed("speed"));
            booleanPoint = {table->Columns[1].WorkMinX + 8, table->OuterRect.Min.y + editor::kPropertyRowHeight * 5.5F};
            speedPoint = {table->Columns[1].WorkMinX + 45, table->OuterRect.Min.y + editor::kPropertyRowHeight * 6.5F};
            resetPoint = {table->Columns[1].WorkMaxX - 8, speedPoint.y};
            ImGui::End(); ImGui::Render();
            require(ImGui::GetDrawData()->Valid && ImGui::GetDrawData()->TotalVtxCount > 0, "Script fields produced no native draw data");
        };
        for (int i = 0; i < 3; ++i) frame(away, false);
        require(state.m_undoStack.empty() && !state.m_pendingSceneEdit, "Passive script Inspector changed authored data or history");
        frame(booleanPoint, false);
        require(ImGui::GetCurrentContext()->HoveredId == booleanId, "Script boolean locator missed the actual checkbox");
        frame(booleanPoint, true); frame(booleanPoint, false);
        require(state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity)->fields["active"] == false &&
            state.m_undoStack.size() == 1 && !state.m_pendingSceneEdit, "Actual script checkbox did not commit one field transaction");
        state.undoSceneEdit();
        require(state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity)->fields["active"] == true, "Undo did not restore the script checkbox value");
        frame(speedPoint, false);
        require(ImGui::GetCurrentContext()->HoveredId == speedId, "Script number locator missed the actual drag widget");
        frame(speedPoint, true);
        const ImVec2 dragged{speedPoint.x + 40, speedPoint.y};
        frame(dragged, true); frame(dragged, true);
        require(ImGui::GetCurrentContext()->ActiveId == speedId, "Override marker changed the active numeric widget ID during a drag");
        frame(dragged, false);
        const auto edited = state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity)->fields["speed"].get<double>();
        require(edited != 2.0 && edited <= 20.0 && state.m_undoStack.size() == 1 && !state.m_pendingSceneEdit,
            "Actual script number drag failed bounds or history coalescing");
        frame(resetPoint, false); frame(resetPoint, true); frame(resetPoint, false);
        require(state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity)->fields["speed"] == 2.0 &&
            state.m_undoStack.size() == 2, "Actual script Reset button did not restore the module default");
        state.undoSceneEdit();
        require(state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity)->fields["speed"] == edited,
            "Undo did not restore the field override after Reset");
        const auto undoCount = state.m_undoStack.size(), redoCount = state.m_redoStack.size();
        const auto unchanged = state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity)->fields;
        const ImVec2 defaultsPoint{booleanPoint.x + 30, booleanPoint.y - editor::kPropertyRowHeight};
        frame(defaultsPoint, false);
        require(ImGui::GetCurrentContext()->HoveredId != 0, "Defaults locator missed the actual Fill Missing button");
        frame(defaultsPoint, true); frame(defaultsPoint, false);
        require(state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity)->fields == unchanged &&
            state.m_undoStack.size() == undoCount && state.m_redoStack.size() == redoCount && redoCount > 0,
            "Fill Missing with valid overrides created a no-op Undo entry or erased Redo");
    }

    static void scriptFieldWidgetsKeepBinaryKeysDistinct(core::JobSystem& jobs) {
        StateStack stack; stack.setServices(&jobs, nullptr);
        GameplayState state(stack); state.m_sceneInitialized = true;
        require(state.m_scripts.initialize(state.m_world, &jobs, std::filesystem::temp_directory_path()),
            "Binary-key Inspector VM initialization failed");
        state.m_scripts.registerModule("BinaryWidgetKeys", R"(
            return {fields={
                ['first###shared']={type='boolean',default=false,label='First suffix key'},
                ['other###shared']={type='boolean',default=false,label='Other suffix key'},
                ['binary\0first']={type='boolean',default=false,label='First binary key'},
                ['binary\0other']={type='boolean',default=false,label='Other binary key'},
            }}
        )");
        const auto entity = state.m_world.createEntity();
        state.m_selectedEntity = entity;
        state.m_world.addComponent<ecs::Tag>(entity, ecs::Tag{"Binary keys"});
        state.m_world.addComponent<scripting::ScriptComponent>(entity, scripting::ScriptComponent{"BinaryWidgetKeys"});
        state.m_scripts.populateDefaults(*state.m_world.getComponent<scripting::ScriptComponent>(entity));
        const auto fields = state.m_scripts.fields("BinaryWidgetKeys");
        require(fields.size() == 4, "Binary-key fixture lost schema entries");
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {900, 1000}; io.DeltaTime = 1.0F / 60.0F;
        std::vector<ImVec2> points(fields.size());
        const ImVec2 away{-1000, -1000};
        const auto frame = [&](const ImVec2 mouse, const bool down) {
            io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({700, 900});
            ImGui::Begin("Binary Script Widgets", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const auto windowId = ImGui::GetCurrentWindow()->ID;
            state.renderScriptInspector(state.m_selectedEntity);
            const auto tableId = ImHashStr("##Properties", 0, ImHashStr("Luau Behavior", 0, windowId));
            const auto* table = ImGui::GetCurrentContext()->Tables.GetByKey(tableId);
            require(table != nullptr, "Binary-key Inspector table missing");
            for (std::size_t index = 0; index < points.size(); ++index) {
                points[index] = {table->Columns[1].WorkMinX + 8,
                    table->OuterRect.Min.y + editor::kPropertyRowHeight * (5.5F + static_cast<float>(index))};
            }
            ImGui::End(); ImGui::Render();
        };
        for (int index = 0; index < 3; ++index) frame(away, false);
        std::unordered_set<ImGuiID> identifiers;
        for (const auto point : points) {
            frame(point, false);
            const auto id = ImGui::GetCurrentContext()->HoveredId;
            require(id != 0 && identifiers.insert(id).second, "Distinct script keys share an actual Inspector widget ID");
        }
        for (std::size_t selected = 0; selected < fields.size(); ++selected) {
            frame(points[selected], true); frame(points[selected], false);
            const auto* script = state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity);
            for (std::size_t index = 0; index < fields.size(); ++index) {
                require(script->fields.at(fields[index].name) == (index == selected), "Inspector edited a different binary/suffix key");
            }
            state.undoSceneEdit();
        }
        state.redoSceneEdit();
        const auto snapshot = state.parseScene(nlohmann::json::parse(state.serializeScene(state.captureSceneSnapshot())));
        state.restoreSceneSnapshot(snapshot);
        const auto* restored = state.m_world.getComponent<scripting::ScriptComponent>(state.m_selectedEntity);
        require(restored && restored->fields.at(fields.back().name) == true, "Scene JSON roundtrip lost the independently edited key");
    }

    static void nativeTextAndRootNamesPreserveBinaryStrings() {
        ecs::World world; scripting::ScriptingSystem scripts;
        require(scripts.initialize(world, nullptr, "assets") && scripting::ReactRuntime::install(scripts),
            "Binary native UI runtime initialization failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo')
            __binaryText=React.createRef(); __binaryChanges=0
            __binaryTextRoot=UI.createRoot({Name='BinaryTextHost'})
            __binaryTextRoot:render(React.createElement('ScreenGui',{}, {
                React.createElement('TextBox',{ref=__binaryText,Text='A'..string.char(0)..'B',ClearTextOnFocus=false,
                    Position=UDim2.fromOffset(20,20),Size=UDim2.fromOffset(220,35),
                    [React.Change.Text]=function() __binaryChanges+=1 end}),
            }))
        )", "=BinaryNativeText"), "Binary TextBox fixture failed");
        require(scripting::ReactRuntime::pump(scripts, 0) && scripts.execute("__binaryChanges=0"),
            "Binary TextBox initial signals failed");
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {800, 600}; io.DeltaTime = 1.0F / 60.0F;
        ImGui::ClearActiveID();
        const int stackTop = lua_gettop(scripts.state());
        const auto frame = [&](const bool gameOnly = true) {
            io.AddMousePosEvent(700, 500); io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            ImGui::NewFrame(); scripting::ReactUI::draw(scripts, gameOnly); ImGui::Render();
            require(lua_gettop(scripts.state()) == stackTop, "Binary native UI leaked Lua stack entries");
            require(scripting::ReactRuntime::pump(scripts, 0), "Binary native UI event dispatch failed");
        };
        frame(); frame();
        require(scripts.execute("assert(__binaryText.current.Text=='A'..string.char(0)..'B' and __binaryChanges==0)"),
            "Passive native drawing truncated TextBox text or emitted a spurious change");
        require(scripts.execute("__binaryText.current:CaptureFocus()"), "Binary TextBox focus request failed");
        frame(); frame();
        require(scripts.execute("assert(__binaryText.current:IsFocused() and __binaryText.current.Text=='A'..string.char(0)..'B' and __binaryChanges==0)"),
            "Focusing a non-clearing TextBox silently changed its text");
        io.AddInputCharactersUTF8("Z"); frame(); frame();
        require(scripts.execute("local text=__binaryText.current.Text; assert(string.find(text,'Z',1,true) and not string.find(text,string.char(0),1,true) and __binaryChanges==1); __binaryText.current:ReleaseFocus(false)"),
            "Explicit native text editing lost input or emitted duplicate changes");
        frame();
        require(scripts.execute("__binaryText.current.Text=string.char(0)..'hidden'; __binaryText.current.ClearTextOnFocus=true"),
            "Binary clear-on-focus setup failed");
        require(scripting::ReactRuntime::pump(scripts, 0) && scripts.execute("__binaryChanges=0; __binaryText.current:CaptureFocus()"),
            "Binary clear-on-focus request failed");
        frame(); frame();
        require(scripts.execute("assert(__binaryText.current.Text=='' and __binaryChanges==1)"),
            "ClearTextOnFocus did not clear a leading-NUL string exactly once");
        require(scripting::ReactRuntime::reset(scripts), "Binary native text cleanup failed");
        require(scripts.execute(R"(
            local React=require('React'); local UI=require('ReactTurbo')
            __binaryFirst=UI.createRoot({Name='BinaryRoot'..string.char(0)..'first'})
            __binaryOther=UI.createRoot({Name='BinaryRoot'..string.char(0)..'other'})
            __binaryFirst:render(React.createElement('TextLabel',{Text='first'}))
            __binaryOther:render(React.createElement('TextLabel',{Text='other'}))
        )", "=BinaryNativeRootNames"), "Binary root name fixture failed");
        frame(false); frame(false);
        std::unordered_set<ImGuiID> windows;
        for (const auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && std::string_view(window->Name).find("BinaryRoot ") != std::string_view::npos &&
                std::string_view(window->Name).find("##ReactRoot") != std::string_view::npos) windows.insert(window->ID);
        }
        require(windows.size() == 2, "Binary root names collided in the native window host");
        require(scripts.execute("assert(__binaryFirst.container.Name=='BinaryRoot'..string.char(0)..'first' and __binaryOther.container.Name=='BinaryRoot'..string.char(0)..'other'); __binaryFirst.container.Name=string.char(0)..'leading'"),
            "Native title sanitization changed authored root names");
        frame(false);
        require(scripting::ReactRuntime::reset(scripts) && lua_gettop(scripts.state()) == stackTop,
            "Binary root cleanup failed or leaked stack entries");
    }

    static void renderedMaterialEditorCreatesSavesAndRejectsConflicts() {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;
        struct Fixture {
            fs::path root = fs::temp_directory_path() / ("tge-material-widgets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
        } fixture;
        fs::create_directory(fixture.root);
        core::ProjectManager projects(fixture.root / "recent.json");
        std::string error;
        const auto project = projects.createBlank(fixture.root, "MaterialWidgets", error);
        require(project.has_value(), "Material editor test project creation failed");
        fs::create_directories(project->assetRoot / "shaders_hlsl");
        const auto write = [](const fs::path& path, const std::string& contents) {
            std::ofstream file(path, std::ios::binary); file << contents;
            require(static_cast<bool>(file), "Material editor fixture write failed");
        };
        const auto read = [](const fs::path& path) {
            std::ifstream file(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        const auto shaderDescriptor = R"({"vertex":"textured.vert.hlsl","fragment":"textured.frag.hlsl"})";
        write(project->assetRoot / "shaders_hlsl/textured.shader.json", shaderDescriptor);
        write(project->assetRoot / "shaders_hlsl/alternate.shader.json", shaderDescriptor);
        StateStack stack;
        stack.setProject(&*project);
        GameplayState state(stack);
        state.onEnter();
        const auto entity = state.createEditorEntity(GameplayState::EditorEntityKind::RenderCube);
        state.m_world.getComponent<ecs::Tag>(entity)->value = "MaterialWidgetMesh";
        state.m_selectedEntity = entity;
        state.m_undoStack.clear(); state.m_redoStack.clear(); state.m_pendingSceneEdit.reset();
        renderer::Renderer renderer;
        renderer.setProjectAssetRoot(project->assetRoot);
        auto& library = renderer.materialLibrary();
        state.restoreMaterialGraphLayout(library);
        const auto mesh = [&]() -> ecs::MeshRenderer& {
            const auto current = tagged(state, "MaterialWidgetMesh");
            auto* component = state.m_world.getComponent<ecs::MeshRenderer>(current);
            require(component != nullptr, "Material editor scene mesh disappeared");
            return *component;
        };
        struct Context {
            ImGuiContext* previous{ImGui::GetCurrentContext()};
            ImGuiContext* owned{ImGui::CreateContext()};
            Context() { ImGui::SetCurrentContext(owned); }
            ~Context() { ImGui::DestroyContext(owned); ImGui::SetCurrentContext(previous); }
        } gui;
        editor::applyEditorTheme();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.DisplaySize = {1000, 1000}; io.DeltaTime = 1.0F / 60.0F;
        io.Fonts->AddFontDefault();
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        const ImVec2 away{-1000, -1000};
        float panelWidth = 500;
        ImGuiID probeId{};
        ImGuiWindow* probeWindow{};
        std::shared_ptr<resources::Material> graphPreview;
        const auto frame = [&](const ImVec2 mouse, const bool down, const bool rightDown = false) {
            io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, rightDown);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({panelWidth, 940});
            ImGui::Begin("Material Editor Widgets", nullptr, ImGuiWindowFlags_NoSavedSettings);
            auto& context = *ImGui::GetCurrentContext();
            const int ids = context.CurrentWindow->IDStack.Size, styles = context.StyleVarStack.Size;
            const int colors = context.ColorStack.Size, tables = context.TablesTempDataStacked;
            if (probeId != 0) {
                ImGui::SetNavWindow(probeWindow);
                ImGui::SetNavID(probeId, ImGuiNavLayer_Main, context.CurrentFocusScopeId, ImRect{});
                context.NavIdIsAlive = false;
            }
            state.renderEntityComponentEditors(tagged(state, "MaterialWidgetMesh"), &renderer);
            require(context.CurrentWindow->IDStack.Size == ids && context.StyleVarStack.Size == styles &&
                context.ColorStack.Size == colors && context.TablesTempDataStacked == tables,
                "Material editor leaked an ImGui ID, style, color or table scope");
            ImGui::End();
            graphPreview = editor::renderMaterialGraphEditor(library, tagged(state, "MaterialWidgetMesh"), &mesh());
            ImGui::Render();
            require(ImGui::GetDrawData()->Valid && ImGui::GetDrawData()->TotalVtxCount > 0, "Material editor produced no native draw data");
        };
        const auto window = [&]() {
            auto* result = ImGui::FindWindowByName("Material Editor Widgets");
            require(result != nullptr, "Material editor window was not rendered");
            return result;
        };
        const auto popup = [&]() {
            const auto& context = *ImGui::GetCurrentContext();
            require(!context.OpenPopupStack.empty() && context.OpenPopupStack.back().Window,
                "Material asset chooser did not open its real popup");
            return context.OpenPopupStack.back().Window;
        };
        const auto itemId = [&](const char* label, const bool inPopup) {
            if (inPopup) return ImHashStr(label, 0, popup()->ID);
            const auto rootText = library.root().generic_u8string();
            const std::string key = std::string(rootText.begin(), rootText.end()) + "|" + mesh().materialId;
            return ImHashStr(label, 0, ImHashStr(key.c_str(), 0, window()->ID));
        };
        const auto locate = [&](const char* label, const bool inPopup = false) {
            std::string diagnostic;
            for (int attempt = 0; attempt < 14; ++attempt) {
                auto* targetWindow = inPopup ? popup() : window();
                const auto target = itemId(label, inPopup);
                probeId = target; probeWindow = targetWindow;
                frame(away, false);
                probeId = 0; probeWindow = nullptr;
                auto& context = *ImGui::GetCurrentContext();
                if (!context.NavIdIsAlive || context.NavId != target)
                    throw std::runtime_error(std::string("Material widget is absent or disabled: ") + label);
                const ImRect rectangle = ImGui::WindowRectRelToAbs(targetWindow, targetWindow->NavRectRel[ImGuiNavLayer_Main]);
                require(rectangle.GetWidth() > 0 && rectangle.GetHeight() > 0, "Material widget has no navigation geometry");
                require(rectangle.Min.x >= targetWindow->InnerRect.Min.x - 1 && rectangle.Max.x <= targetWindow->InnerRect.Max.x + 1,
                    "Material widget extends beyond the narrow panel");
                const auto point = rectangle.GetCenter();
                if (targetWindow->InnerClipRect.Contains(point)) {
                    frame(point, false);
                    if (context.HoveredId == target) return point;
                }
                diagnostic = " target=" + std::to_string(target) + " hovered=" + std::to_string(context.HoveredId) +
                    " rect=" + std::to_string(rectangle.Min.x) + "," + std::to_string(rectangle.Min.y) + "," +
                    std::to_string(rectangle.Max.x) + "," + std::to_string(rectangle.Max.y) +
                    " clip=" + std::to_string(targetWindow->InnerClipRect.Min.x) + "," + std::to_string(targetWindow->InnerClipRect.Min.y) + "," +
                    std::to_string(targetWindow->InnerClipRect.Max.x) + "," + std::to_string(targetWindow->InnerClipRect.Max.y) +
                    " scroll=" + std::to_string(targetWindow->Scroll.y) + "/" + std::to_string(targetWindow->ScrollMax.y) +
                    " navHover=" + std::to_string(context.NavHighlightItemUnderNav) +
                    " hoveredWindow=" + (context.HoveredWindow ? context.HoveredWindow->Name : "<none>") +
                    " mouse=" + std::to_string(io.MousePos.x) + "," + std::to_string(io.MousePos.y);
                const float direction = point.y < targetWindow->InnerClipRect.Min.y ? 3.0F : -3.0F;
                const auto wheelPoint = targetWindow->InnerRect.GetCenter();
                frame(wheelPoint, false);
                io.AddMouseWheelEvent(0, direction);
                frame(wheelPoint, false);
            }
            throw std::runtime_error(std::string("Cannot scroll to actual material editor widget: ") + label + diagnostic);
        };
        const auto click = [&](const char* label, const bool inPopup = false) {
            const auto point = locate(label, inPopup);
            frame(point, true); frame(point, false);
        };
        const auto input = [&](const char* label, const char* value, const bool inPopup = false) {
            const auto point = locate(label, inPopup);
            frame(point, true); frame(point, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_LeftCtrl, true); io.AddKeyEvent(ImGuiKey_A, true);
            frame(point, false);
            io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiKey_LeftCtrl, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame(point, false);
            io.AddInputCharactersUTF8(value); frame(point, false);
            io.AddKeyEvent(ImGuiKey_Enter, true); frame(point, false);
            io.AddKeyEvent(ImGuiKey_Enter, false); frame(away, false);
            const auto& textState = ImGui::GetCurrentContext()->InputTextState;
            if (textState.ID != itemId(label, inPopup) || textState.TextA.empty() || std::string(textState.TextA.Data) != value) {
                throw std::runtime_error(std::string("Actual material text input mismatch: ") + label +
                    " expected=" + value + " actual=" + (textState.TextA.empty() ? "<empty>" : textState.TextA.Data));
            }
        };
        for (int i = 0; i < 3; ++i) frame(away, false);
        state.m_inspectorFilter = "Material";
        for (int i = 0; i < 3; ++i) frame(away, false);
        click("###CreateMaterial");
        state.m_inspectorFilter.clear();
        frame(away, false);
        const auto basePath = mesh().materialId;
        require(!basePath.empty() && fs::exists(project->assetRoot / basePath),
            "Actual Create Material button did not write and assign a material asset");
        require(state.m_undoStack.size() == 1 && !state.m_pendingSceneEdit, "Material assignment did not record one completed scene transaction");
        state.undoSceneEdit();
        frame(away, false);
        require(mesh().materialId.empty() && fs::exists(project->assetRoot / basePath), "Material assignment Undo changed the asset file or retained its assignment");
        state.redoSceneEdit();
        frame(away, false);
        require(mesh().materialId == basePath, "Material assignment Redo did not restore the chosen resource");
        auto base = library.load(basePath);
        require(base->ready && base->definition.depthTest, "Created material did not resolve its shader and default states");
        const auto original = read(project->assetRoot / basePath);
        click("###AdvancedMaterial");
        input("###Shader", "shaders_hlsl/alternate.shader.json");
        click("###DepthTest");
        require(read(project->assetRoot / basePath) == original && base->definition.depthTest,
            "Editing material draft changed disk or live state before saving");
        click("###SaveMaterial");
        require(base->ready && base->definition.shader == "shaders_hlsl/alternate.shader.json" && !base->definition.depthTest,
            "Actual Save Material did not commit shader/state draft changes");
        require(state.m_undoStack.size() == 1, "Saving shared material parameters created a scene assignment transaction");
        input("###Shader", "shaders_hlsl/textured.shader.json");
        auto external = Json::parse(read(project->assetRoot / basePath));
        external["tint"] = {0.2, 0.3, 0.4, 1};
        const auto externalBytes = external.dump(2) + "\n";
        write(project->assetRoot / basePath, externalBytes);
        click("###SaveMaterial");
        require(read(project->assetRoot / basePath) == externalBytes,
            "Material Save overwrote a source changed after the draft was opened");
        click("###DiscardMaterial");
        require(std::abs(base->definition.tint.r - 0.2F) < 0.0001F && base->definition.shader == "shaders_hlsl/alternate.shader.json",
            "Discard did not load external material values");
        click("###NewInstance");
        const auto baseBeforeAliasedCreate = read(project->assetRoot / basePath);
        input("###Destination", ("assets/" + basePath).c_str());
        click("###ConfirmCreate");
        require(mesh().materialId == basePath && read(project->assetRoot / basePath) == baseBeforeAliasedCreate && state.m_undoStack.size() == 1,
            "Instance creation overwrote an existing material through an assets-prefixed path alias");
        click("###CancelCreate");
        click("###NewInstance");
        click("###ConfirmCreate");
        const auto firstInstancePath = mesh().materialId;
        require(firstInstancePath != basePath && fs::exists(project->assetRoot / firstInstancePath),
            "Default instance destination reused the base or failed to create a new asset");
        auto instance = library.load(firstInstancePath);
        require(instance->ready && instance->definition.tint == base->definition.tint && !instance->definition.depthTest &&
            Json::parse(read(project->assetRoot / firstInstancePath)).at("base") == basePath,
            "Created material instance did not inherit its base asset");
        require(state.m_undoStack.size() == 2, "Creating an instance did not record exactly one assignment transaction");
        click("###AdvancedMaterial");
        click("###Override_depthTest");
        click("###DepthTest"); click("###SaveMaterial");
        require(instance->definition.depthTest, "Instance override did not apply to the live material");
        click("###Override_depthTest");
        require(instance->definition.depthTest, "Resetting a draft override applied before Save");
        click("###Override_depthTest"); click("###SaveMaterial");
        require(!instance->definition.depthTest && !Json::parse(read(project->assetRoot / firstInstancePath)).at("depthTest").get<bool>(),
            "Re-enabling an override copied stale values instead of the immediately resolved base");
        click("###ResetOverrides"); click("###SaveMaterial");
        require(!instance->definition.depthTest && !Json::parse(read(project->assetRoot / firstInstancePath)).contains("depthTest"),
            "Reset All and Save did not restore base inheritance");
        const std::string alternateBase = "materials/other.material.json";
        write(project->assetRoot / alternateBase, Json{{"shader", "shaders_hlsl/textured.shader.json"}, {"depthTest", true}, {"tint", {0.1, 0.8, 0.2, 1}}}.dump(2));
        input("###Base", alternateBase.c_str());
        require(!instance->definition.depthTest, "Editing the base draft changed the live material before Save");
        click("###Override_depthTest"); click("###SaveMaterial");
        const auto switched = Json::parse(read(project->assetRoot / firstInstancePath));
        require(instance->definition.depthTest && switched.at("base") == alternateBase && switched.at("depthTest") == true,
            "Changing base did not immediately preview inherited values before authoring an override");
        panelWidth = 320;
        for (int i = 0; i < 3; ++i) frame(away, false);
        require(window()->ContentSize.x <= window()->InnerRect.GetWidth() + 1, "Material Inspector requires horizontal scrolling at 320 pixels");
        click("###MaterialChoice");
        input("###AssetSearch", fs::path(basePath).filename().string().c_str(), true);
        click(("###" + basePath).c_str(), true);
        require(mesh().materialId == basePath, "Searchable material chooser did not assign the selected file");
        click("###NewInstance"); click("###ConfirmCreate");
        require(mesh().materialId != firstInstancePath && mesh().materialId != basePath && fs::exists(project->assetRoot / mesh().materialId)
            && Json::parse(read(project->assetRoot / mesh().materialId)).at("base") == basePath,
            "Creating another instance reused or overwrote an existing default destination");
        const auto validPath = mesh().materialId;
        const std::string malformed = "materials/invalid-base.material.json";
        write(project->assetRoot / malformed, R"({"base":17})");
        mesh().materialId = malformed;
        for (int i = 0; i < 3; ++i) frame(away, false);
        require(!library.load(malformed)->ready, "Malformed material was treated as ready by Inspector");
        mesh().materialId = validPath;
        for (int i = 0; i < 3; ++i) frame(away, false);
        const auto resourceBeforeReopen = library.load(validPath);
        auto reopenedSource = Json::parse(read(project->assetRoot / validPath));
        reopenedSource["depthTest"] = false;
        write(project->assetRoot / validPath, reopenedSource.dump(2) + "\n");
        library.setRoot(project->assetRoot);
        for (int i = 0; i < 3; ++i) frame(away, false);
        const auto reopenedResource = library.load(validPath);
        require(reopenedResource != resourceBeforeReopen && reopenedResource->ready && !reopenedResource->definition.depthTest &&
            reopenedResource->revision == resourceBeforeReopen->revision,
            "Material reopen fixture did not replace its resource with an equal-revision fresh definition");
        click("###AdvancedMaterial");
        click("###DepthTest"); click("###SaveMaterial");
        require(reopenedResource->definition.depthTest && Json::parse(read(project->assetRoot / validPath)).at("depthTest") == true,
            "Clean Inspector draft kept stale source or inherited controls after the material library reopened");

        mesh().materialId.clear();
        frame(away, false);
        input("###Destination", "materials/graph.material.json");
        click("###CreateGraphMaterial");
        frame(away, false);
        const auto graph = library.load(mesh().materialId);
        require(graph->ready && graph->definition.graphProgram && graph->path == "materials/graph.material.json" &&
            Json::parse(read(project->assetRoot / graph->path)).at("version") == 2,
            "Create Material Graph did not save and assign a valid graph asset");
        require(editor::materialGraphEditorFocused(), "Creating a graph did not focus its editor");
        platform::Event key{};
        key.type = platform::EventType::KeyPressed;
        key.key = platform::KeyCode::Delete;
        state.handleEvent(key);
        key.key = platform::KeyCode::Space;
        state.handleEvent(key);
        require(state.m_world.isAlive(tagged(state, "MaterialWidgetMesh")) && state.m_editorMode == GameplayState::EditorMode::Edit,
            "Material graph keyboard focus leaked Delete or Space to the scene editor");
        require(!graphPreview, "A saved graph unexpectedly created a draft preview");

        ImGui::SetWindowPos("Material Graph", {panelWidth + 30, 60});
        mesh().materialId = alternateBase;
        const auto legacy = library.load(alternateBase);
        const auto legacyRevision = legacy->revision;
        const auto legacyFile = project->assetRoot / alternateBase;
        const auto legacyBytes = read(legacyFile);
        const auto legacyWriteTime = fs::last_write_time(legacyFile);
        frame(away, false);
        click("###ConvertMaterialGraph");
        require(graphPreview && graphPreview != legacy && graphPreview->path == alternateBase && graphPreview->definition.graphProgram,
            "An unsaved graph conversion did not publish a transient preview for the selected material");
        require(legacy->revision == legacyRevision && !legacy->definition.graphProgram &&
            fs::last_write_time(legacyFile) == legacyWriteTime && read(legacyFile) == legacyBytes,
            "A graph preview mutated the saved material");
        mesh().materialId = graph->path;
        frame(away, false);
        require(!graphPreview, "A draft preview followed the object after its material assignment changed");

        const std::string historyPath = "materials/history.material.json";
        const Json historySource{{"version", 2}, {"graph", {
            {"nodes", Json::array({{{"id", 1}, {"type", "scalar"}, {"value", 0.25}, {"position", {40, 90}}}})},
            {"outputs", {{"baseColor", 1}}}, {"outputPosition", {340, 45}}
        }}};
        const auto historyFile = project->assetRoot / historyPath;
        const auto historyBytes = historySource.dump(2) + "\n";
        write(historyFile, historyBytes);
        const auto historyWriteTime = fs::last_write_time(historyFile);
        mesh().materialId = historyPath;
        io.DisplaySize = {1440, 1000};
        frame(away, false);
        click("###OpenMaterialGraph");
        const auto historyAsset = library.load(historyPath);
        require(historyAsset->ready && !graphPreview, "Material graph history fixture did not open cleanly");
        const auto historyRevision = historyAsset->revision;
        const auto sceneHistorySize = state.m_undoStack.size();

        const auto graphChild = [](const char* label) -> ImGuiWindow* {
            auto* parent = ImGui::FindWindowByName("Material Graph");
            require(parent != nullptr, "Material Graph window did not open");
            const auto childId = ImHashStr(label, 0, parent->ID);
            for (auto* child : ImGui::GetCurrentContext()->Windows) {
                if (child->Active && child->ParentWindow == parent && child->ChildId == childId) return child;
            }
            throw std::runtime_error(std::string("Material Graph child missing: ") + label);
        };
        const auto canvasPoint = [&](float x, float y) {
            const auto area = graphChild("Graph Canvas")->WorkRect;
            const auto size = area.GetSize();
            const ImVec2 graphSize{476.0F, 178.0F};
            const auto zoom = std::clamp(std::min((size.x - 64.0F) / graphSize.x, (size.y - 64.0F) / graphSize.y), 0.35F, 1.0F);
            return ImVec2{area.Min.x + (size.x - graphSize.x * zoom) * 0.5F + (x - 40.0F) * zoom,
                          area.Min.y + (size.y - graphSize.y * zoom) * 0.5F + (y - 45.0F) * zoom};
        };
        const auto shortcutKeys = [&](ImGuiKey letter, bool down) {
            io.AddKeyEvent(ImGuiMod_Ctrl, down); io.AddKeyEvent(ImGuiKey_LeftCtrl, down); io.AddKeyEvent(letter, down);
        };
        const auto shortcut = [&](ImGuiKey letter) {
            frame(away, false);
            shortcutKeys(letter, true);
            frame(away, false);
            shortcutKeys(letter, false);
            frame(away, false);
        };
        const auto displayedGraph = [&]() -> const Json& {
            return graphPreview ? graphPreview->definition.graph : historyAsset->definition.graph;
        };
        const auto connected = [&] { return displayedGraph().at("outputs").contains("baseColor"); };
        const auto scalar = [&] { return displayedGraph().at("nodes").at(0).at("value").get<float>(); };

        const auto baseColorPin = canvasPoint(340.0F, 88.5F);
        frame(baseColorPin, false);
        frame(baseColorPin, false, true);
        frame(baseColorPin, false);
        require(graphPreview && !connected(), "Disconnecting Base Color did not update the live draft");
        shortcut(ImGuiKey_Z);
        require(connected(), "Material Graph Ctrl+Z did not restore the Base Color link");
        shortcut(ImGuiKey_Y);
        require(graphPreview && !connected(), "Material Graph Ctrl+Y did not remove the Base Color link again");
        shortcut(ImGuiKey_Z);
        require(connected(), "Material Graph did not return to the linked baseline");

        const auto nodeControl = [&](const char* label, float titleX = 128.0F, float titleY = 105.0F) {
            const auto nodeTitle = canvasPoint(titleX, titleY);
            frame(nodeTitle, false); frame(nodeTitle, true); frame(nodeTitle, false);
            auto* details = graphChild("Node Properties");
            probeWindow = details;
            probeId = ImHashStr(label, 0, ImHashStr("1", 0, details->ID));
            frame(away, false);
            auto& context = *ImGui::GetCurrentContext();
            require(context.NavIdIsAlive && context.NavId == probeId, "Scalar node control is unavailable");
            const auto point = ImGui::WindowRectRelToAbs(details, details->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
            probeWindow = nullptr; probeId = 0;
            return point;
        };
        const auto scalarPoint = nodeControl("###GraphScalar");
        frame(scalarPoint, false); frame(scalarPoint, true);
        frame({scalarPoint.x + 22, scalarPoint.y}, true);
        frame({scalarPoint.x + 45, scalarPoint.y}, true);
        frame({scalarPoint.x + 45, scalarPoint.y}, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        frame(away, false);
        require(graphPreview && connected() && scalar() > 0.25F, "A scalar drag did not reach the live preview");
        const auto draggedValue = scalar();
        shortcut(ImGuiKey_Y);
        require(connected() && scalar() == draggedValue, "A new graph edit did not clear the abandoned redo branch");
        shortcut(ImGuiKey_Z);
        require(connected() && scalar() == 0.25F, "A multi-frame scalar drag did not undo as one edit");
        shortcut(ImGuiKey_Y);
        require(connected() && scalar() == draggedValue, "Redo did not restore the full scalar drag");
        const auto deletePoint = nodeControl("###DeleteGraphNode");
        frame(deletePoint, false); frame(deletePoint, true); frame(deletePoint, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        frame(away, false);
        require(!connected() && displayedGraph().at("nodes").empty(), "Deleting a node did not remove its connection");
        shortcut(ImGuiKey_Z);
        require(connected() && scalar() == draggedValue, "Undo did not restore the deleted node and its connection together");
        shortcut(ImGuiKey_Y);
        require(!connected() && displayedGraph().at("nodes").empty(), "Redo did not restore the node deletion");
        require(historyAsset->revision == historyRevision && state.m_undoStack.size() == sceneHistorySize &&
            fs::last_write_time(historyFile) == historyWriteTime && read(historyFile) == historyBytes,
            "Material Graph history modified scene history or the saved material");

        shortcut(ImGuiKey_Z);
        shortcut(ImGuiKey_S);
        require(!graphPreview && historyAsset->definition.graph.at("nodes").at(0).at("value") == draggedValue,
            "Saving the graph did not establish a clean history checkpoint");
        const auto beforeMove = read(historyFile);
        const auto title = canvasPoint(128.0F, 105.0F);
        frame(title, false); frame(title, true);
        frame({title.x + 18, title.y + 7}, true);
        frame({title.x + 42, title.y + 18}, true);
        frame({title.x + 42, title.y + 18}, false);
        require(!graphPreview, "Moving a graph node recompiled the live preview");
        shortcut(ImGuiKey_Z);
        shortcut(ImGuiKey_S);
        require(read(historyFile) == beforeMove, "A multi-frame node move did not undo to the saved layout in one step");
        shortcut(ImGuiKey_Y);
        shortcut(ImGuiKey_S);
        const auto moved = Json::parse(read(historyFile)).at("graph").at("nodes").at(0).at("position");
        require(std::abs(moved.at(0).get<float>() - 82.0F) <= 1.0F && std::abs(moved.at(1).get<float>() - 108.0F) <= 1.0F,
            "Redo did not restore the full node move");

        const auto checkpointGraph = historyAsset->definition.graph;
        const auto movedScalar = nodeControl("###GraphScalar", moved.at(0).get<float>() + 88.0F, moved.at(1).get<float>() + 15.0F);
        frame(movedScalar, false); frame(movedScalar, true);
        frame({movedScalar.x + 22, movedScalar.y}, true);
        frame({movedScalar.x + 45, movedScalar.y}, true);
        frame({movedScalar.x + 45, movedScalar.y}, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        frame(away, false);
        require(graphPreview && connected() && scalar() > draggedValue, "Recovery fixture could not edit the scalar after Save");
        const auto recoveredValue = scalar();
        frame(baseColorPin, false);
        frame(baseColorPin, false, true);
        frame(baseColorPin, false);
        shortcut(ImGuiKey_Z);
        require(graphPreview && connected() && scalar() == recoveredValue,
            "Docking fixture could not restore a live material draft between undo and redo actions");
        const auto savedSelection = state.createEditorEntity(GameplayState::EditorEntityKind::RenderCube);
        state.m_world.getComponent<ecs::Tag>(savedSelection)->value = "SavedSceneSelection";
        state.m_world.getComponent<ecs::MeshRenderer>(savedSelection)->materialId = historyPath;
        state.m_selectedEntity = savedSelection;
        require(state.saveSceneToDisk(), "Material Graph recovery fixture could not save its scene selection");
        state.m_selectedEntity = tagged(state, "MaterialWidgetMesh");
        state.m_showViewportPanel = true;
        state.m_showHierarchyPanel = state.m_showProjectPanel = state.m_showInspectorPanel = state.m_showStatsPanel = false;
        state.m_showToolbarPanel = state.m_showStatusPanel = state.m_showAssistant = state.m_showScriptWorkspace = false;
        state.m_viewportDockSlot = EditorDockSlot::Center;
        state.m_editorLayoutLocked = false;
        state.m_materialGraphDocked = true;
        auto* workspaceState = &state;
        const auto workspaceFrame = [&](ImVec2 mouse, bool down, bool middleDown = false) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, middleDown);
            ImGui::NewFrame();
            if (probeId != 0) {
                auto& context = *ImGui::GetCurrentContext();
                ImGui::SetNavWindow(probeWindow);
                ImGui::SetNavID(probeId, ImGuiNavLayer_Main, context.CurrentFocusScopeId, ImRect{});
                context.NavIdIsAlive = false;
            }
            workspaceState->renderEditorUi(renderer);
            ImGui::Render();
        };
        const auto workspaceShortcut = [&](ImGuiKey letter) {
            workspaceFrame(away, false);
            shortcutKeys(letter, true);
            workspaceFrame(away, false);
            shortcutKeys(letter, false);
        };
        const auto workspacePanel = [](const char* name) -> ImGuiWindow* {
            auto* panel = ImGui::FindWindowByName(name);
            if (!panel || !panel->Active || panel->Collapsed) throw std::runtime_error(std::string("Workspace panel is hidden: ") + name);
            return panel;
        };
        const auto graphButton = [&](const char* label) {
            auto* panel = workspacePanel("Material Graph");
            probeWindow = panel;
            const auto item = ImHashStr(label, 0, panel->ID);
            probeId = item;
            workspaceFrame(away, false);
            auto& context = *ImGui::GetCurrentContext();
            require(context.NavIdIsAlive && context.NavId == probeId, "Material Graph toolbar control is unavailable");
            const auto point = ImGui::WindowRectRelToAbs(panel, panel->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
            probeWindow = nullptr;
            probeId = 0;
            workspaceFrame(point, false);
            require(context.HoveredId == item, "Material Graph toolbar control is obstructed");
            workspaceFrame(point, true);
            workspaceFrame(point, false);
            workspaceFrame(away, false);
        };
        const auto checkDockedWorkspace = [&](bool stacked) {
            const auto graphBounds = workspacePanel("Material Graph")->Rect();
            const auto viewportBounds = workspacePanel("Viewport")->Rect();
            const ImRect viewportContent{{state.m_viewportPosition.x, state.m_viewportPosition.y},
                {state.m_viewportPosition.x + state.m_viewportSize.x, state.m_viewportPosition.y + state.m_viewportSize.y}};
            require(state.m_viewportUsesContentRect && !graphBounds.Overlaps(viewportBounds) && !graphBounds.Overlaps(viewportContent) &&
                viewportBounds.Contains(viewportContent) && viewportContent.GetWidth() > 100.0F && viewportContent.GetHeight() > 100.0F,
                "Docked Material Graph overlaps the live Viewport or leaves it unusably small");
            require(stacked ? viewportBounds.Max.y <= graphBounds.Min.y : viewportBounds.Max.x <= graphBounds.Min.x,
                "Material Graph workspace did not adapt to its available width");
            require(state.m_materialPreview && state.m_materialPreviewEntity == state.m_selectedEntity &&
                state.m_materialPreview->definition.graph.at("nodes").at(0).at("value") == recoveredValue,
                "Docked Material Graph did not publish the selected object's live draft");
            return viewportBounds.GetWidth();
        };
        const auto dragWorkspaceSplitter = [&](bool stacked) {
            const auto* splitter = workspacePanel("##MaterialGraphWorkspaceSplitter");
            const auto item = ImHashStr("##DockSplitter", 0, splitter->ID);
            const auto start = splitter->Rect().GetCenter();
            const auto middle = stacked ? ImVec2{start.x, start.y + 24.0F} : ImVec2{start.x + 24.0F, start.y};
            const auto end = stacked ? ImVec2{start.x, start.y + 56.0F} : ImVec2{start.x + 56.0F, start.y};
            const auto before = workspacePanel("Viewport")->Size;
            const float beforeFraction = state.m_materialGraphSplit;
            auto& context = *ImGui::GetCurrentContext();
            workspaceFrame(start, false);
            require(context.HoveredId == item, "Material Graph workspace splitter did not receive mouse hover");
            workspaceFrame(start, true);
            require(context.ActiveId == item, "Material Graph workspace splitter did not capture the mouse press");
            if (!stacked) {
                workspaceFrame(middle, true);
                workspaceFrame(end, true);
            }
            workspaceFrame(end, false);
            if (stacked) {
                require(ImGui::IsMouseReleased(ImGuiMouseButton_Left) && std::abs(io.MouseDelta.y - 56.0F) < 0.01F,
                    "Stacked splitter fixture did not deliver its final move and release in the same frame");
            }
            workspaceFrame(away, false);
            const auto after = workspacePanel("Viewport")->Size;
            const float growth = stacked ? after.y - before.y : after.x - before.x;
            require(growth >= 54.0F && growth <= 58.0F && state.m_materialGraphSplit < beforeFraction,
                "Material Graph workspace splitter did not resize the Viewport by the mouse movement");
            const auto persisted = Json::parse(read(state.editorLayoutPath())).at("materialGraph");
            require(persisted.at("dockBesideViewport") == true &&
                std::abs(persisted.at("split").get<float>() - state.m_materialGraphSplit) < 0.0001F,
                "Releasing the Material Graph workspace splitter did not persist its new split");
        };
        workspaceFrame(away, false);
        workspaceFrame(away, false);
        checkDockedWorkspace(false);
        dragWorkspaceSplitter(false);
        const auto splitWidth = checkDockedWorkspace(false);
        graphButton("###FloatMaterialGraph");
        require(!state.m_materialGraphDocked && !state.m_viewportUsesContentRect && workspacePanel("Viewport")->Size.x > splitWidth &&
            (workspacePanel("Material Graph")->Flags & ImGuiWindowFlags_NoMove) == 0,
            "Floating Material Graph did not release the docked Viewport space");
        require(!Json::parse(read(state.editorLayoutPath())).at("materialGraph").contains("session"),
            "Floating Material Graph was persisted for docked restoration");
        graphButton("###DockMaterialGraph");
        require(state.m_materialGraphDocked, "Material Graph did not return to its docked workspace");
        checkDockedWorkspace(false);
        io.DisplaySize = {720, 900};
        workspaceFrame(away, false);
        workspaceFrame(away, false);
        checkDockedWorkspace(true);
        dragWorkspaceSplitter(true);
        checkDockedWorkspace(true);

        const auto originalView = editor::materialGraphSession(library);
        require(originalView && originalView->canvas, "The docked graph has no canvas state");
        const auto canvasCenter = graphChild("Graph Canvas")->InnerRect.GetCenter();
        workspaceFrame(canvasCenter, false);
        io.AddMouseWheelEvent(0.0F, 2.0F);
        workspaceFrame(canvasCenter, false);
        const auto zoomedView = editor::materialGraphSession(library);
        require(zoomedView && zoomedView->canvas && zoomedView->canvas->zoom > originalView->canvas->zoom,
            "Mouse wheel did not change the Material Graph canvas zoom");
        const ImVec2 panTarget{canvasCenter.x + 47.0F, canvasCenter.y - 29.0F};
        workspaceFrame(canvasCenter, false, true);
        workspaceFrame(panTarget, false, true);
        workspaceFrame(panTarget, false);
        const auto remembered = editor::materialGraphSession(library);
        require(remembered && remembered->materialId == historyPath && remembered->canvas &&
            remembered->canvas->pan == zoomedView->canvas->pan + glm::vec2{47.0F, -29.0F},
            "Material Graph canvas did not capture the latest graph and middle-button pan");
        require(state.m_materialPreview != nullptr, "Material Graph draft preview disappeared before reopening the project");
        const auto expectedDraft = state.m_materialPreview->definition.graph;
        const auto beforeReopen = read(historyFile);
        const auto beforeReopenTime = fs::last_write_time(historyFile);
        const auto savedSceneBytes = read(project->scenePath);
        const auto draftsFile = project->layoutPath.parent_path() / "material_graph_drafts.json";
        state.onExit();
        require(!editor::materialGraphEditorOpen(library), "Project exit left the Material Graph window open");
        const auto savedLayout = Json::parse(read(state.editorLayoutPath()));
        const auto& savedSession = savedLayout.at("materialGraph").at("session");
        const auto& savedCanvas = savedSession.at("canvas");
        require(savedSession.at("materialId") == historyPath && savedCanvas.at("zoom").get<float>() == remembered->canvas->zoom &&
            savedCanvas.at("pan") == Json::array({remembered->canvas->pan.x, remembered->canvas->pan.y}),
            "Project layout did not persist the last open graph and exact canvas transform");
        const auto persistedDrafts = Json::parse(read(draftsFile)).at("drafts");
        require(persistedDrafts.at(historyPath).at("source").at("graph") == expectedDraft &&
            persistedDrafts.at(historyPath).at("original") == beforeReopen,
            "Project did not persist the unsaved graph and its original material bytes");
        require(persistedDrafts.at(alternateBase).at("source").at("graph").is_object() &&
            read(legacyFile) == legacyBytes, "Switching materials lost the unsaved conversion or wrote it to the asset");

        renderer.setProjectAssetRoot({});
        renderer.setProjectAssetRoot(project->assetRoot);
        GameplayState reopened(stack);
        reopened.onEnter();
        reopened.m_showStatusPanel = true;
        workspaceState = &reopened;
        workspaceFrame(away, false);
        workspacePanel("Status Bar");
        const auto* reopenedFocus = ImGui::GetCurrentContext()->NavWindow;
        require(reopenedFocus && reopenedFocus->RootWindow == workspacePanel("Material Graph"),
            "A newly opened editor panel took keyboard focus from the recovered Material Graph");
        const auto reopenedSelection = tagged(reopened, "MaterialWidgetMesh");
        require(reopenedSelection != tagged(reopened, "SavedSceneSelection") && reopened.m_selectedEntity == reopenedSelection &&
            reopened.m_materialPreviewEntity == reopenedSelection && reopened.m_materialPreview &&
            reopened.m_materialPreview->definition.graph == expectedDraft,
            "Opening a project did not immediately preview the restored draft on its remembered selection");
        workspaceFrame(away, false);
        require(reopened.m_viewportUsesContentRect && (workspacePanel("Material Graph")->Flags & ImGuiWindowFlags_NoMove) != 0 &&
            editor::materialGraphSession(library) == remembered,
            "Reopening the project did not auto-open the last docked graph at its saved zoom and pan");
        require(read(historyFile) == beforeReopen && fs::last_write_time(historyFile) == beforeReopenTime &&
            read(project->scenePath) == savedSceneBytes, "Restoring the draft changed the material asset or scene");

        workspaceShortcut(ImGuiKey_Z);
        require(!reopened.m_materialPreview && library.load(historyPath)->definition.graph == checkpointGraph,
            "Ctrl+Z after reopening did not immediately return the preview to the saved graph");
        workspaceShortcut(ImGuiKey_Y);
        require(reopened.m_materialPreviewEntity == reopenedSelection && reopened.m_materialPreview &&
            reopened.m_materialPreview->definition.graph == expectedDraft,
            "Ctrl+Y after reopening did not immediately restore the draft preview");
        workspaceShortcut(ImGuiKey_Y);
        require(reopened.m_materialPreviewEntity == reopenedSelection && reopened.m_materialPreview &&
            !reopened.m_materialPreview->definition.graph.at("outputs").contains("baseColor"),
            "Ctrl+Y did not recover the link change that was already in the redo stack at close");
        workspaceShortcut(ImGuiKey_Z);
        require(reopened.m_materialPreviewEntity == reopenedSelection && reopened.m_materialPreview &&
            reopened.m_materialPreview->definition.graph == expectedDraft,
            "Ctrl+Z did not immediately preview the reconnected graph");
        workspaceShortcut(ImGuiKey_Z);
        require(!reopened.m_materialPreview, "Ctrl+Z did not return the graph to the saved checkpoint");
        reopened.onExit();

        renderer.setProjectAssetRoot({});
        renderer.setProjectAssetRoot(project->assetRoot);
        GameplayState resumed(stack);
        resumed.onEnter();
        workspaceState = &resumed;
        workspaceFrame(away, false);
        const auto resumedSelection = tagged(resumed, "MaterialWidgetMesh");
        require(editor::materialGraphEditorOpen(library) && resumed.m_selectedEntity == resumedSelection && !resumed.m_materialPreview,
            "Reopening the graph at the saved checkpoint changed its clean preview or selection");
        workspaceShortcut(ImGuiKey_Y);
        require(resumed.m_materialPreviewEntity == resumedSelection && resumed.m_materialPreview &&
            resumed.m_materialPreview->definition.graph == expectedDraft && read(historyFile) == beforeReopen &&
            fs::last_write_time(historyFile) == beforeReopenTime && read(project->scenePath) == savedSceneBytes,
            "Ctrl+Y lost the draft after reopening at the saved checkpoint, changed the saved material or changed the scene");

        auto conflictingMaterial = Json::parse(beforeReopen);
        conflictingMaterial["depthWrite"] = false;
        const auto conflictingBytes = conflictingMaterial.dump(2) + "\n";
        write(historyFile, conflictingBytes);
        graphButton("###SaveGraphMaterial");
        require(read(historyFile) == conflictingBytes && resumed.m_materialPreview,
            "Saving a recovered draft silently overwrote an external material change or lost the draft");
        write(historyFile, beforeReopen);
        graphButton("###SaveGraphMaterial");
        const auto committedGraph = Json::parse(read(historyFile)).at("graph");
        require(!resumed.m_materialPreview && committedGraph.at("nodes") == expectedDraft.at("nodes") &&
            committedGraph.at("outputs") == expectedDraft.at("outputs"), "Explicit Save did not commit the recovered draft");
        resumed.onExit();
        const auto remainingDrafts = Json::parse(read(draftsFile)).at("drafts");
        require(!remainingDrafts.contains(historyPath) && remainingDrafts.contains(alternateBase),
            "Explicit Save did not clear only the saved material's recovery draft");

        renderer.setProjectAssetRoot({});
        renderer.setProjectAssetRoot(project->assetRoot);
        GameplayState committed(stack);
        committed.onEnter();
        workspaceState = &committed;
        workspaceFrame(away, false);
        require(editor::materialGraphEditorOpen(library) && !committed.m_materialPreview &&
            library.load(historyPath)->definition.graph == committedGraph,
            "Opening the project after Save revived the recovered draft");
        committed.onExit();

        auto missingLayout = savedLayout;
        missingLayout["materialGraph"]["session"]["materialId"] = "materials/missing.material.json";
        write(state.editorLayoutPath(), missingLayout.dump(2));
        GameplayState missingGraph(stack);
        missingGraph.onEnter();
        workspaceState = &missingGraph;
        workspaceFrame(away, false);
        require(!editor::materialGraphEditorOpen(library) && !missingGraph.m_viewportUsesContentRect,
            "A missing saved graph blocked the normal Viewport layout");
        missingGraph.onExit();
        std::cout << "MATERIAL_INSPECTOR_WIDGETS_PASS width_320=1 width_500=1 real_io=1 assignment_undo_redo=1 inheritance_preview=1"
                     " alias_overwrite_guard=1 unique_instance=1 conflict_guard=1 clean_reopen_identity=1 graph_create=1 graph_hotkeys=1 graph_preview=1 graph_undo_redo=1\n";
        std::cout << "MATERIAL_GRAPH_WORKSPACE_PASS beside_viewport=1 stacked=1 float_redock=1 live_preview=1 splitter_drag=1 persisted_split=1\n";
        std::cout << "MATERIAL_GRAPH_SESSION_PASS material=1 canvas_pan=1 canvas_zoom=1 docked_reopen=1 missing_asset=1\n";
        std::cout << "MATERIAL_GRAPH_DRAFT_RECOVERY_PASS restored_preview=1 unchanged_before_save=1 conflict_guard=1 explicit_save=1 clean_reopen=1\n";
        std::cout << "MATERIAL_GRAPH_HISTORY_RECOVERY_PASS undo=1 redo=1 clean_checkpoint_redo=1 live_preview=1\n";
    }

    static void dockResizeHandlesPreserveInspectorInput() {
        struct Context {
            ImGuiContext* previous{ImGui::GetCurrentContext()};
            ImGuiContext* owned{ImGui::CreateContext()};
            Context() { ImGui::SetCurrentContext(owned); }
            ~Context() { ImGui::DestroyContext(owned); ImGui::SetCurrentContext(previous); }
        } gui;
        editor::applyEditorTheme();
        ImGui::GetStyle().WindowMinSize = {32, 32};
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = {1280, 900}; io.DeltaTime = 1.0F / 60.0F;
        io.Fonts->AddFontDefault();
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        StateStack stack;
        GameplayState state(stack);
        state.m_editorLayoutLocked = false;
        state.m_showInspectorPanel = state.m_showViewportPanel = true;
        state.m_showHierarchyPanel = state.m_showProjectPanel = state.m_showStatsPanel = false;
        state.m_showToolbarPanel = state.m_showStatusPanel = state.m_showAssistant = false;
        state.m_inspectorDockSlot = EditorDockSlot::RightTop;
        state.m_editorRightWidth = 360;
        bool overridden = false;
        ImVec2 checkboxPoint{};
        const ImVec2 away{-1000, -1000};
        const auto frame = [&](const ImVec2 mouse, const bool down) {
            io.AddMousePosEvent(mouse.x, mouse.y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            ImGui::NewFrame();
            const auto* previousSplitter = ImGui::FindWindowByName("##DockSplitterRightColumn");
            const auto position = previousSplitter ? ImVec2(previousSplitter->Pos.x + 8, previousSplitter->Pos.y + 20) : ImVec2(800, 100);
            ImGui::SetNextWindowPos(position); ImGui::SetNextWindowSize({300, 400});
            const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
            ImGui::Begin("Dock Inspector Input", nullptr, flags);
            ImGui::Checkbox("Override###InspectorOverride", &overridden);
            checkboxPoint = ImVec2(ImGui::GetItemRectMin().x + 6,
                (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * .5F);
            ImGui::End();
            auto& context = *ImGui::GetCurrentContext();
            const auto styleCount = context.StyleVarStack.Size;
            const auto colorCount = context.ColorStack.Size;
            state.renderDockResizeHandles();
            require(context.StyleVarStack.Size == styleCount && context.ColorStack.Size == colorCount &&
                ImGui::GetStyle().WindowMinSize.x == 32 && ImGui::GetStyle().WindowMinSize.y == 32,
                "Dock splitter drawing leaked styles or changed the normal window minimum size");
            const auto* splitter = ImGui::FindWindowByName("##DockSplitterRightColumn");
            require(splitter && std::abs(splitter->Size.x - 8) < .01F,
                "Dock splitter window expanded beyond its eight-pixel input strip");
            if (previousSplitter)
                require(checkboxPoint.x > splitter->Pos.x + splitter->Size.x && checkboxPoint.x < splitter->Pos.x + 32,
                    "Inspector checkbox fixture no longer covers the formerly blocked strip");
            ImGui::Render();
        };
        for (int i = 0; i < 3; ++i) frame(away, false);
        frame(checkboxPoint, false);
        require(ImGui::GetCurrentContext()->HoveredWindow == ImGui::FindWindowByName("Dock Inspector Input"),
            "Dock splitter intercepted mouse input inside the Inspector body");
        frame(checkboxPoint, true); frame(checkboxPoint, false);
        require(overridden, "Inspector checkbox beside the unlocked splitter did not receive real mouse clicks");
        std::cout << "DOCK_SPLITTER_INPUT_PASS width=8 inspector_mouse=1 style_restore=1\n";
    }

    static void scriptPropertiesPersistAndSpawn(core::JobSystem& jobs) {
        namespace fs = std::filesystem;
        using Json = nlohmann::json;
        struct Fixture {
            fs::path root = fs::temp_directory_path() / ("tge-script-properties-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
        } fixture;
        fs::create_directory(fixture.root);
        core::ProjectManager projects(fixture.root / "recent.json");
        std::string error;
        auto project = projects.createBlank(fixture.root, "Properties", error);
        require(project.has_value(), "Script Properties project fixture failed");
        {
            std::ofstream script(project->assetRoot / "scripts/Tunable.luau");
            script << "return {fields={speed={type='number',default=2,min=0,max=20,step=0.25,label='Move speed'},"
                "enabled={type='boolean',default=true},title={type='string',default='Default'}},"
                "Init=function(self) self.state.spawnSpeed=self.fields.speed; self.state.title=self.fields.title end}";
        }
        StateStack stack; stack.setServices(&jobs, nullptr); stack.setProject(&*project);
        GameplayState state(stack); state.onEnter();
        const auto entity = state.createEditorEntity(GameplayState::EditorEntityKind::Empty);
        state.m_world.getComponent<ecs::Tag>(entity)->value = "Tunable Object";
        state.m_selectedEntity = entity;
        state.m_world.addComponent<scripting::ScriptComponent>(entity, scripting::ScriptComponent{"scripts/Tunable.luau", true, Json::object()});
        auto* script = state.m_world.getComponent<scripting::ScriptComponent>(entity);
        state.recordSceneHistory("Edit Script Field speed");
        require(state.m_scripts.setField(*script, "speed", 7.5) && state.m_scripts.setField(*script, "title", "Authored"), "Authored Inspector field edit failed");
        const auto expected = script->fields;
        state.undoSceneEdit();
        const auto restored = tagged(state, "Tunable Object");
        require(state.m_world.getComponent<scripting::ScriptComponent>(restored)->fields.empty(), "Undo lost the authored field defaults");
        state.redoSceneEdit();
        const auto edited = tagged(state, "Tunable Object");
        require(state.m_world.getComponent<scripting::ScriptComponent>(edited)->fields == expected, "Redo lost Inspector field overrides");
        auto* transform = state.m_world.getComponent<ecs::Transform>(edited);
        transform->rotationEulerRadians = {0.1F, 0.2F, 0.3F};
        const auto rotation = transform->rotationEulerRadians;
        ecs::MeshRenderer mesh; mesh.materialId = "materials/runner.material.json"; mesh.uvScale = {2, 3}; mesh.visible = false;
        state.m_world.addComponent<ecs::MeshRenderer>(edited, mesh);
        require(state.saveSceneToDisk(), "Inspector overrides did not save to the scene");
        require(state.saveEntityPrefab(edited, "prefabs/tunable.prefab.json"), "Inspector overrides did not export to a prefab");
        require(!state.saveEntityPrefab(edited, "prefabs/tunable.prefab.json") && !state.saveEntityPrefab(edited, "prefabs/../../escape.json"),
            "Prefab export overwrote an existing file or escaped project assets");
        const auto spawned = state.m_scripts.spawnPrefab("prefabs/tunable.prefab.json");
        require(state.m_world.isAlive(spawned), "Saved script prefab did not spawn");
        const auto* spawnedScript = state.m_world.getComponent<scripting::ScriptComponent>(spawned);
        require(spawnedScript && spawnedScript->fields["speed"] == 7.5 && spawnedScript->fields["title"] == "Authored", "Next prefab spawn lost Inspector values");
        const auto* spawnedMesh = state.m_world.getComponent<ecs::MeshRenderer>(spawned);
        require(spawnedMesh && spawnedMesh->materialId == mesh.materialId && spawnedMesh->uvScale == mesh.uvScale && !spawnedMesh->visible &&
            state.m_world.getComponent<ecs::Transform>(spawned)->rotationEulerRadians == rotation,
            "Prefab export lost authored material, UV, visibility or rotation");
        state.loadSceneFromDisk();
        const auto loaded = tagged(state, "Tunable Object");
        require(state.m_world.getComponent<scripting::ScriptComponent>(loaded)->fields == expected, "Reopened scene lost Inspector overrides");
        state.enterPlayMode(); state.update(1.0 / 60.0);
        require(state.m_scripts.inspect(loaded)["state"]["spawnSpeed"] == 7.5 && state.m_scripts.inspect(loaded)["state"]["title"] == "Authored",
            "Next Play did not apply saved script values in Init");
        require(!state.saveEntityPrefab(loaded, "prefabs/runtime.json"), "Temporary Play values could be saved as authored prefab values");
        auto* liveScript = state.m_world.getComponent<scripting::ScriptComponent>(loaded);
        state.m_scripts.setField(*liveScript, "speed", 12.0);
        state.stopPlayMode();
        require(state.m_world.getComponent<scripting::ScriptComponent>(tagged(state, "Tunable Object"))->fields == expected,
            "Stop retained temporary script property edits");
    }

    static void ribbonPreferencesRoundTrip(core::JobSystem& jobs) {
        using Json = nlohmann::json;
        namespace fs = std::filesystem;
        struct Fixture {
            fs::path root = fs::temp_directory_path() / ("tge-ribbon-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
        } fixture;
        fs::create_directory(fixture.root);
        core::ProjectManager projects(fixture.root / "recent.json");
        std::string error;
        auto project = projects.createBlank(fixture.root, "Ribbon", error);
        require(project.has_value(), "Ribbon project fixture failed");
        StateStack stack; stack.setServices(&jobs, nullptr); stack.setProject(&*project);
        GameplayState state(stack);
        state.m_ribbonState.tab = editor::EditorRibbonTab::Model;
        state.m_ribbonState.compact = true;
        state.m_ribbonState.showLabels = false;
        state.m_ribbonState.moveSnap = 2.0F;
        state.m_ribbonState.rotateSnap = 30.0F;
        state.m_ribbonState.scaleSnap = 0.25F;
        state.m_ribbonState.createCommand = editor::EditorCommandId::CreatePhysicsSphere;
        state.m_ribbonState.playCommand = editor::EditorCommandId::PlayWindow;
        state.m_ribbonState.networkTest = {3, 120, 25};
        state.m_snapEnabled = true;
        state.m_useLocalGizmoSpace = false;
        state.m_showToolbarPanel = false;
        state.m_showHierarchyPanel = true;
        state.m_showProjectPanel = false;
        state.m_showInspectorPanel = false;
        state.m_showStatsPanel = true;
        state.m_showViewportPanel = true;
        state.m_showStatusPanel = false;
        state.m_showAssistant = false;
        state.saveEditorLayout();
        GameplayState restored(stack);
        restored.loadEditorLayout();
        require(restored.m_ribbonState.tab == editor::EditorRibbonTab::Model && restored.m_ribbonState.compact &&
            !restored.m_ribbonState.showLabels && restored.m_ribbonState.moveSnap == 2.0F &&
            restored.m_ribbonState.rotateSnap == 30.0F && restored.m_ribbonState.scaleSnap == 0.25F &&
            restored.m_ribbonState.createCommand == editor::EditorCommandId::CreatePhysicsSphere &&
            restored.m_ribbonState.playCommand == editor::EditorCommandId::PlayWindow &&
            restored.m_snapEnabled && !restored.m_useLocalGizmoSpace, "Ribbon preferences did not round-trip independently of the scene");
        require(restored.m_ribbonState.networkTest.clientCount == 3 && restored.m_ribbonState.networkTest.latencyMs == 120 &&
            restored.m_ribbonState.networkTest.lossPercent == 25, "Network Test options did not round-trip in the project layout");
        require(!restored.m_showToolbarPanel && restored.m_showHierarchyPanel && !restored.m_showProjectPanel &&
            !restored.m_showInspectorPanel && restored.m_showStatsPanel && restored.m_showViewportPanel &&
            !restored.m_showStatusPanel && !restored.m_showAssistant, "Saved panel visibility, including hidden Assistant, did not round-trip");
        state.m_showAssistant = true;
        state.saveEditorLayout();
        restored.loadEditorLayout();
        require(restored.m_showAssistant, "Saved visible Assistant did not replace the previous hidden preference");

        Json layout;
        {
            std::ifstream saved(project->layoutPath);
            saved >> layout;
        }
        layout["ribbon"].erase("networkTest");
        {
            std::ofstream legacy(project->layoutPath);
            legacy << layout.dump();
        }
        restored.loadEditorLayout();
        require(restored.m_ribbonState.networkTest.clientCount == 2 && restored.m_ribbonState.networkTest.latencyMs == 0 &&
            restored.m_ribbonState.networkTest.lossPercent == 0, "Legacy layout did not load default Network Test options");
    }

    static void textInputDoesNotTriggerSceneShortcuts(core::JobSystem& jobs) {
        platform::InputManager::reset();
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        state.m_selectedEntity = addBody(state, "Typing Fixture", {0.0F, 0.0F, 0.0F});
        state.duplicateEntity(state.m_selectedEntity);
        const auto entityCount = state.m_world.aliveCount();
        const auto undoCount = state.m_undoStack.size();
        ImGui::GetIO().WantTextInput = true;
        platform::Event key;
        key.type = platform::EventType::KeyPressed;
        key.key = platform::KeyCode::Space;
        state.handleEvent(key);
        require(state.m_editorMode == GameplayState::EditorMode::Edit, "Space entered Play while entering script text");
        key.type = platform::EventType::TextInput;
        key.codepoint = ' ';
        state.handleEvent(key);
        require(state.m_editorMode == GameplayState::EditorMode::Edit, "TextInput entered Play");
        key.type = platform::EventType::KeyPressed;
        key.key = platform::KeyCode::LeftControl;
        platform::InputManager::onEvent(key);
        key.key = platform::KeyCode::D;
        platform::InputManager::onEvent(key);
        state.processEditorShortcuts();
        require(state.m_world.aliveCount() == entityCount && state.m_undoStack.size() == undoCount,
                "Ctrl+D mutated scene while entering script text");
        platform::InputManager::beginFrame();
        key.key = platform::KeyCode::Z;
        platform::InputManager::onEvent(key);
        state.processEditorShortcuts();
        require(state.m_world.aliveCount() == entityCount && state.m_undoStack.size() == undoCount,
                "Ctrl+Z undid scene edits while entering script text");
        ImGui::GetIO().WantTextInput = false;
        state.m_showScriptWorkspace = true;
        key.key = platform::KeyCode::Delete;
        state.handleEvent(key);
        require(state.m_world.aliveCount() == entityCount, "Delete edited the hidden scene after focus moved to the ribbon");
        state.m_showScriptWorkspace = false;
        platform::InputManager::reset();
    }
};

}

int main(int argc, char** argv) {
    ImGui::CreateContext();
    engine::platform::InputManager::reset();
    try {
        engine::core::JobSystem jobs(true, 4U);
        if (argc == 3 && std::string_view(argv[1]) == "--standalone-smoke") {
            engine::game::GameplayStateTestAccess::standaloneProcessSmoke(std::filesystem::u8path(argv[2]), jobs);
            ImGui::DestroyContext();
            std::cout << "Standalone D3D12: snapshot launch, auto-Play, parent isolation, Stop and cleanup passed\n";
            return EXIT_SUCCESS;
        }
        engine::game::GameplayStateTestAccess::sceneSelectionIsIndependentOfTracy(jobs);
        engine::game::GameplayStateTestAccess::authoredSceneIdsSurviveHistoryAndLiveSave(jobs);
        engine::game::GameplayStateTestAccess::runtimeDeleteAndRestore(jobs);
        engine::game::GameplayStateTestAccess::liveDeletionCleansPausedPhysics(jobs);
        engine::game::GameplayStateTestAccess::collisionEventsRejectWorkerDispatch(jobs);
        engine::game::GameplayStateTestAccess::parallelPhysicsAndRecycledIds(jobs);
        engine::game::GameplayStateTestAccess::inspectorHistory(jobs);
        engine::game::GameplayStateTestAccess::styledReactLayoutAndInput();
        engine::game::GameplayStateTestAccess::embeddedReactViewportLayoutAndInput();
        engine::game::GameplayStateTestAccess::editorHudUsesViewport(jobs);
        engine::game::GameplayStateTestAccess::robloxInstanceLayoutAndSignals();
        engine::game::GameplayStateTestAccess::advancedReactVisualsAndControls();
        engine::game::GameplayStateTestAccess::canvasGroupsFlattenAndGateInput();
        engine::game::GameplayStateTestAccess::tweenedNativePresentation();
        engine::game::GameplayStateTestAccess::editablePlayRestoresSceneAndHistory(jobs);
        engine::game::GameplayStateTestAccess::standaloneUsesUnsavedSnapshot(jobs);
        engine::game::GameplayStateTestAccess::importedScriptDemoHasEditableBehaviors(jobs);
        engine::game::GameplayStateTestAccess::scriptDemoLifecycleAndSceneHistory(jobs);
        engine::game::GameplayStateTestAccess::scriptPropertiesPersistAndSpawn(jobs);
        engine::game::GameplayStateTestAccess::renderedScriptPropertiesUseRealWidgets(jobs);
        engine::game::GameplayStateTestAccess::scriptFieldWidgetsKeepBinaryKeysDistinct(jobs);
        engine::game::GameplayStateTestAccess::nativeTextAndRootNamesPreserveBinaryStrings();
        engine::game::GameplayStateTestAccess::renderedMaterialEditorCreatesSavesAndRejectsConflicts();
        engine::game::GameplayStateTestAccess::dockResizeHandlesPreserveInspectorInput();
        engine::game::GameplayStateTestAccess::textInputDoesNotTriggerSceneShortcuts(jobs);
        engine::game::GameplayStateTestAccess::networkTestUsesUnsavedReplicaScene(jobs);
        engine::game::GameplayStateTestAccess::networkTestCsvPreservesSamples();
        engine::game::GameplayStateTestAccess::networkTestSummaryCsvPreservesComparison();
        engine::game::GameplayStateTestAccess::networkTestChangesOneLiveClient();
        engine::game::GameplayStateTestAccess::ribbonCommandsPreserveHistoryAndPlay(jobs);
        engine::game::GameplayStateTestAccess::ribbonPreferencesRoundTrip(jobs);
        engine::game::GameplayStateTestAccess::ribbonDockedGeometry(jobs);
        engine::game::GameplayStateTestAccess::referenceEditorLayoutGeometry(jobs);
        engine::game::GameplayStateTestAccess::renderedPanelsPreserveWidgetHistory(jobs);
        engine::game::GameplayStateTestAccess::scriptSaveDoesNotFollowExistingSidecars();
        engine::game::GameplayStateTestAccess::projectsHaveIndependentScenesAndScripts(jobs);
        engine::game::GameplayStateTestAccess::liveScenesPreservePlay(jobs);
    } catch (const std::exception& error) {
        std::cerr << "GameplayState test failure: " << error.what() << '\n';
        ImGui::DestroyContext();
        return EXIT_FAILURE;
    }
    ImGui::DestroyContext();
    std::cout << "Gameplay: scenes, physics, Undo/Redo, scripted Play, React lifecycle, Script Workspace and input isolation passed\n";
    return EXIT_SUCCESS;
}
