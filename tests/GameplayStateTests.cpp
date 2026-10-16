#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <entt/entity/entity.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <lua.h>
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/misc/cpp/imgui_stdlib.h"

#include "engine/core/JobSystem.h"
#include "engine/core/LabOptions.h"
#include "engine/core/PlaySession.h"
#include "engine/ecs/components.h"
#include "engine/game/GameplayState.h"
#include "engine/game/StateStack.h"
#include "engine/editor/EditorTheme.h"
#include "engine/platform/Input.h"
#include "engine/renderer/Renderer.h"
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
            // Same session keeps the last valid scene through incomplete AI writes.
            write(project->scenePath, "{invalid");
            for (int i=0;i<45;++i) { state.update(0.016); jobs.waitAll(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            require(state.m_world.isAlive(added), "Invalid live JSON discarded the scene");
            state.stopPlayMode();
            require(tagged(state,"LiveAdded") == ecs::kInvalidEntity && tagged(state,"Remove") != ecs::kInvalidEntity, "Stop did not restore the pre-Play scene");
            require(state.m_world.getComponent<ecs::Transform>(tagged(state,"Body"))->position.x == 1, "Stop kept transient live positions");
            stack.setStandalone(true); state.onExit();
        }
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
        options.scene = "ecs"; // Lab scenes retain priority, even with a stale stress environment flag.
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
        state.m_simulationRunning = false; // Delete must also work while paused.
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
        state.deleteEntity(parent); // Inspector/context menu use this same entry.
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
        state.deleteEntity(parent); // Dead/invalid requests are harmless.
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
        state.stopPlayMode(); // Stop can happen before the next fixed update.
        state.update(1.0 / 60.0);
        require(state.m_world.isAlive(tagged(state, "parent")), "Pending Play delete targeted the restored Edit scene");
        state.deleteEntity(tagged(state, "parent"));
        require(!state.m_world.isAlive(tagged(state, "parent")) && state.hasUndo(), "Edit Delete/Undo regressed");
        state.undoSceneEdit();
        require(state.m_world.isAlive(tagged(state, "parent")), "Undo did not restore an Edit deletion");
        state.redoSceneEdit();
        require(!state.m_world.isAlive(tagged(state, "parent")), "Redo did not reapply an Edit deletion");
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
        state.update(1.0 / 60.0); // Real integration/proxy parallelFor, joined on return.
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
        state.deleteEntity(first); // A stale runtime request must not target the replacement.
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
            const auto entity = state.m_selectedEntity; // Undo/Redo remap handles.
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
        frame(gravityPoint, true); // Switching fields commits the text edit first.
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
            if (window->Active && std::string_view(window->Name).find("StandaloneHUD") != std::string_view::npos) {
                require((window->Flags & ImGuiWindowFlags_NoTitleBar) != 0, "Game HUD must not show editor window chrome");
                hud = true;
            }
        }
        require(hud, "Standalone hid the game's own React UI");
        child.onExit(); state.onExit();
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
        stack.setStandalone(true); // The smoke harness must not rewrite editor preferences.
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
            -- The native renderer must not execute metamethods, including nested layout values.
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
                if (!window->Active || std::string_view(window->Name).find("StyledNativeTest") == std::string_view::npos) continue;
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
            if (window->Active && !window->ParentWindow && std::string_view(window->Name).find("StyledNativeTest") != std::string_view::npos)
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

    static void robloxInstanceLayoutAndSignals() {
        ecs::World world;
        scripting::ScriptingSystem scripts;
        require(scripts.initialize(world, nullptr, "assets") && scripting::ReactRuntime::install(scripts), "Roblox native UI runtime initialization failed");
        require(scripts.execute(R"(
            local React=require('React'); local ReactRoblox=require('ReactRoblox'); local e=React.createElement
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
            -- Native drawing must ignore metatables even when publishing geometry.
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
            "A mutated Roblox-style Instance button stopped receiving clicks");
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
        for (const auto* window:ImGui::GetCurrentContext()->Windows) if (window->Active && std::string_view(window->Name).find("AdvancedNativeTest")!=std::string_view::npos) {
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
        // Tiny deterministic CPU oracle: source-over in premultiplied storage,
        // then unpremultiply each offscreen group before sampling its texture.
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
            for (const auto* window:ImGui::GetCurrentContext()->Windows) if (window->Active && std::string_view(window->Name).find("CanvasNativeTest")!=std::string_view::npos)
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

    static void scriptDemoLifecycleAndSceneHistory(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.onEnter(); // Reads editor preferences; no onExit/save is invoked by this test.
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
        state.undoSceneEdit(); // Keep one game controller so session assertions are deterministic.
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

        // The ribbon is drawn before the inspector, so it must commit a pending
        // inspector transaction before undoing it, even with no older history.
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
        // The reference workspace starts with both scene sidebars hidden.
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
        // Renderer::init is deliberately never called: only CPU ImGui layout and
        // the renderer's fallback extent are used, without any GPU device/window.
        renderer::Renderer renderer;
        struct Rect { ImVec2 pos, size; };
        struct Geometry { Rect viewport, assistant, ribbon, hierarchy, inspector; };
        const auto rect = [](const char* name) {
            const auto* window = ImGui::FindWindowByName(name);
            require(window && window->Active, "Expected editor layout window did not render");
            return Rect{window->Pos, window->Size};
        };
        const auto draw = [&]() {
            // Suppress incidental autosaves during geometry-only frames. Any
            // remaining save is still confined to this temporary project.
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

        // Saved/custom dock positions still work independently of new defaults.
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
            // Scene restoration may add the engine's fallback camera. Each live
            // entity must still consume exactly one reference-height row.
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

    static void ribbonPreferencesRoundTrip(core::JobSystem& jobs) {
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
        require(!restored.m_showToolbarPanel && restored.m_showHierarchyPanel && !restored.m_showProjectPanel &&
            !restored.m_showInspectorPanel && restored.m_showStatsPanel && restored.m_showViewportPanel &&
            !restored.m_showStatusPanel && !restored.m_showAssistant, "Saved panel visibility, including hidden Assistant, did not round-trip");
        state.m_showAssistant = true;
        state.saveEditorLayout();
        restored.loadEditorLayout();
        require(restored.m_showAssistant, "Saved visible Assistant did not replace the previous hidden preference");
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

} // namespace engine::game

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
        engine::game::GameplayStateTestAccess::runtimeDeleteAndRestore(jobs);
        engine::game::GameplayStateTestAccess::parallelPhysicsAndRecycledIds(jobs);
        engine::game::GameplayStateTestAccess::inspectorHistory(jobs);
        engine::game::GameplayStateTestAccess::styledReactLayoutAndInput();
        engine::game::GameplayStateTestAccess::robloxInstanceLayoutAndSignals();
        engine::game::GameplayStateTestAccess::advancedReactVisualsAndControls();
        engine::game::GameplayStateTestAccess::canvasGroupsFlattenAndGateInput();
        engine::game::GameplayStateTestAccess::tweenedNativePresentation();
        engine::game::GameplayStateTestAccess::editablePlayRestoresSceneAndHistory(jobs);
        engine::game::GameplayStateTestAccess::standaloneUsesUnsavedSnapshot(jobs);
        engine::game::GameplayStateTestAccess::scriptDemoLifecycleAndSceneHistory(jobs);
        engine::game::GameplayStateTestAccess::textInputDoesNotTriggerSceneShortcuts(jobs);
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
