#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include "engine/ecs/world.h"
#include "engine/editor/ScriptWorkspace.h"
#include "engine/scripting/ScriptingSystem.h"
#include "third_party/ImGuiColorTextEdit/TextEditor.h"

namespace {
void require(const bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    require(static_cast<bool>(stream), "Cannot write command fixture");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
struct Fixture final {
    std::filesystem::path root;
    Fixture() {
        for (int i = 0; i < 100; ++i) {
            auto candidate = std::filesystem::temp_directory_path() / ("tge-workspace-commands-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(i));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) { root = std::move(candidate); break; }
        }
        require(!root.empty(), "Cannot create command fixture");
        std::filesystem::create_directories(root / "assets/scripts");
    }
    ~Fixture() {
        std::error_code error;
        for (const auto* name : {"assets/scripts/Original.luau", "assets/scripts/Script1.luau",
                "assets/scripts/Script2.luau", "assets/scripts/Script3.luau", "assets/scripts/Snapshot.luau",
                "assets/scripts", "assets"}) std::filesystem::remove(root / name, error);
        std::filesystem::remove(root, error);
    }
};
void draw(engine::editor::ScriptWorkspace& workspace) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = {1600.0F, 1000.0F};
    io.DeltaTime = 1.0F / 60.0F;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0F, 0.0F});
    ImGui::SetNextWindowSize({1600.0F, 1000.0F});
    bool visible = true;
    workspace.render(&visible, engine::ecs::kInvalidEntity, false, false);
    ImGui::Render();
}

void diskSnapshots(engine::scripting::ScriptingSystem& scripts, const Fixture& fixture) {
    using engine::editor::EditorCommandId;
    const auto path = fixture.root / "assets/scripts/Snapshot.luau";
    const std::string crlf = "local value = 3\r\nreturn value\r\n";
    write(path, crlf);
    engine::editor::ScriptWorkspace workspace;
    workspace.initialize(fixture.root / "assets", scripts, {});
    require(workspace.openScript("scripts/Snapshot.luau"), "Cannot open CRLF snapshot fixture");
    require(workspace.executeCommand(EditorCommandId::ScriptComment), "Cannot edit CRLF snapshot fixture");
    require(workspace.saveActive() && read(path) == "--local value = 3\nreturn value\n",
            "Raw disk snapshot confused editor CRLF normalization with an external edit");
    require(workspace.executeCommand(EditorCommandId::ScriptComment), "Cannot edit saved snapshot fixture");
    const auto savedTime = std::filesystem::last_write_time(path);
    const std::string external = "return 'preserve external bytes'\n";
    write(path, external);
    std::filesystem::last_write_time(path, savedTime);
    require(!workspace.saveActive() && read(path) == external,
            "Save overwrote external content whose timestamp was preserved");
    require(workspace.assistantContext().find("local value = 3") != std::string::npos,
            "Disk conflict discarded unsaved document text");

    engine::editor::ScriptWorkspace created;
    created.initialize(fixture.root / "assets", scripts, {});
    require(created.executeCommand(EditorCommandId::ScriptNew) && created.saveActive(), "Cannot create watched script fixture");
    const auto createdPath = fixture.root / "assets/scripts/Script3.luau";
    require(std::filesystem::is_regular_file(createdPath), "Unexpected new-script fixture path");
    write(createdPath, external);
    bool refreshed = false;
    for (int attempt = 0; attempt < 100 && !refreshed; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        created.update();
        refreshed = created.assistantContext().find(external) != std::string::npos;
    }
    require(refreshed, "Newly saved scripts were not registered for external-change refresh");
}
}

int main() {
    using engine::editor::EditorCommandId;
    try {
        Fixture fixture;
        engine::ecs::World world;
        engine::scripting::ScriptingSystem scripts;
        require(scripts.initialize(world, nullptr, fixture.root / "assets"), "Cannot initialize generated-behavior runtime");
        engine::editor::ScriptWorkspace workspace;
        require(!workspace.commandState(EditorCommandId::ScriptNew).enabled, "Uninitialized New is enabled");
        workspace.initialize(fixture.root / "assets", scripts, {});
        require(!workspace.commandState(EditorCommandId::ScriptSave).enabled, "Save is enabled without a document");
        require(!workspace.executeCommand(EditorCommandId::ScriptSave), "Save executed without a document");
        require(!workspace.executeCommand(EditorCommandId::Delete), "Workspace accepted a scene command");

        require(workspace.executeCommand(EditorCommandId::ScriptOpen), "Open popup request failed without ImGui");
        require(workspace.executeCommand(EditorCommandId::ScriptNew), "New document command failed");
        require(!std::filesystem::exists(fixture.root / "assets/scripts/Script1.luau"), "New wrote the document before Save");
        require(!workspace.commandState(EditorCommandId::ScriptSaveReload).enabled, "Unsaved document allows Save+Reload");
        require(!workspace.commandState(EditorCommandId::ScriptReloadDisk).enabled, "Unsaved document allows Reload Disk");
        require(workspace.executeCommand(EditorCommandId::ScriptSave), "New document Save failed");
        require(std::filesystem::is_regular_file(fixture.root / "assets/scripts/Script1.luau"), "Save did not write new document");
        const auto entity = world.createEntity();
        engine::scripting::ScriptComponent component;
        component.path = "scripts/Script1.luau";
        world.addComponent<engine::scripting::ScriptComponent>(entity, component);
        scripts.clearOutput();
        scripts.start();
        bool initialized = false;
        for (const auto& output : scripts.output()) {
            initialized = initialized || (output.kind == engine::scripting::ScriptMessage::Kind::Output && output.text == "Hello world!");
        }
        require(initialized, "New-script template did not dispatch its Init callback");
        scripts.stop();
        world.destroyEntity(entity);
        require(workspace.executeCommand(EditorCommandId::ScriptNew), "Second new document command failed");
        require(workspace.executeCommand(EditorCommandId::ScriptSaveAll), "Save All did not save unsaved documents");
        require(std::filesystem::is_regular_file(fixture.root / "assets/scripts/Script2.luau"), "Save All omitted the second document");

        const auto originalPath = fixture.root / "assets/scripts/Original.luau";
        const std::string original = "local value = 3\nreturn value\n";
        write(originalPath, original);
        require(workspace.openScript("scripts/Original.luau"), "Could not open command fixture");
        require(workspace.executeCommand(EditorCommandId::ScriptComment), "Comment command failed");
        require(workspace.assistantContext().find("--local value = 3") != std::string::npos, "Comment did not edit the document buffer");
        require(read(originalPath) == original, "Comment wrote the authoring file before Save");
        require(workspace.executeCommand(EditorCommandId::ScriptCheck), "Syntax check command failed");
        require(!scripts.output().empty(), "Syntax check did not emit a result");
        require(workspace.executeCommand(EditorCommandId::ScriptSave), "Commented document Save failed");
        require(read(originalPath).starts_with("--local value = 3"), "Save omitted buffer edit");
        require(workspace.executeCommand(EditorCommandId::ScriptComment), "Second comment command failed");
        const std::string external = "return 'external edit'\n";
        write(originalPath, external);
        std::filesystem::last_write_time(originalPath, std::filesystem::last_write_time(originalPath) + std::chrono::seconds(2));
        require(!workspace.executeCommand(EditorCommandId::ScriptSave), "Command bypassed disk conflict protection");
        require(read(originalPath) == external, "Conflict Save overwrote external edit");
        require(workspace.executeCommand(EditorCommandId::ScriptReloadDisk), "Reload confirmation request failed");
        require(read(originalPath) == external && workspace.assistantContext().find("local value = 3") != std::string::npos,
            "Reload request changed the file or buffer before confirmation");
        require(workspace.executeCommand(EditorCommandId::ScriptGoToLine), "Go-to-line request failed without ImGui");
        require(workspace.executeCommand(EditorCommandId::ScriptFind) && workspace.commandState(EditorCommandId::ScriptFind).checked,
            "Find command did not expose checked state");
        require(workspace.executeCommand(EditorCommandId::ScriptFind) && !workspace.commandState(EditorCommandId::ScriptFind).checked,
            "Find command did not toggle off");

        engine::editor::EditorCommandRegistry registry;
        int dispatched = 0;
        registry.setHandlers([&](auto id) { return workspace.commandState(id); }, [&](auto id) { ++dispatched; workspace.executeCommand(id); });
        workspace.setCommandRegistry(&registry);
        require(registry.execute(EditorCommandId::ScriptOutput) && dispatched == 1, "Shared command recursed or failed to reach workspace");
        for (const auto id : {EditorCommandId::ScriptCommand, EditorCommandId::ScriptState, EditorCommandId::ScriptErrors}) {
            require(workspace.executeCommand(id) && workspace.commandState(id).checked, "Diagnostic tab request has no checked state");
        }
        require(!workspace.commandState(EditorCommandId::ScriptState).checked, "Multiple diagnostic tabs are selected");
        diskSnapshots(scripts, fixture);
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels{}; int width{}, height{};
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        draw(workspace);
        draw(workspace);
        require(workspace.commandState(EditorCommandId::ScriptErrors).checked, "Deferred Errors tab selection was lost in render");
        workspace.setCommandRegistry(nullptr);
        ImGui::DestroyContext();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        return 1;
    }
    std::cout << "Script workspace command dispatch, buffer/save conflicts, deferred UI and diagnostics passed\n";
    return 0;
}
