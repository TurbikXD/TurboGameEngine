#include "engine/editor/ProjectLauncher.h"
#include "engine/core/ProjectManager.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <imgui.h>

namespace {
namespace fs = std::filesystem;
using engine::core::Project;
using engine::core::ProjectManager;
using engine::editor::ProjectLauncher;
using Access = engine::editor::ProjectLauncherTestAccess;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

struct Fixture final {
    fs::path root;
    Fixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("tge-launcher-test-" + std::to_string(nonce));
        require(fs::create_directory(root), "Could not exclusively create test fixture");
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

struct Gui final {
    Gui() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Gui() { ImGui::DestroyContext(); }
    void frame(ProjectLauncher* launcher, const ImVec2 mouse = ImVec2(-100.0F, -100.0F), const bool down = false) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        if (launcher != nullptr) launcher->render();
        ImGui::Render();
    }
    void settle(ProjectLauncher& launcher) { for (int index = 0; index < 4; ++index) frame(&launcher); }
    void click(ProjectLauncher& launcher, const Access::Action action) {
        ImVec2 point;
        Access::actionCenter(launcher, action, point.x, point.y);
        require(point.x >= 0.0F && point.y >= 0.0F, "Action was not rendered");
        frame(&launcher, point, false);
        frame(&launcher, point, true);
        frame(&launcher, point, false);
    }
};

void renderAndActions() {
    Fixture fixture;
    ProjectManager manager(fixture.root / "recent.json");
    int opened = 0;
    int demos = 0;
    std::optional<Project> result;
    ProjectLauncher launcher(manager, [&](Project project) { ++opened; result = std::move(project); }, [&] { ++demos; });
    Gui gui;
    const ImVec4 originalBackground = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const float originalRounding = ImGui::GetStyle().FrameRounding;
    gui.settle(launcher);
    require(opened == 0 && demos == 0 && manager.recent().empty(), "Rendering must not open or create projects");
    require(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x == originalBackground.x &&
            ImGui::GetStyle().FrameRounding == originalRounding, "Launcher leaked style changes");
    gui.click(launcher, Access::Action::EngineDemo);
    require(demos == 1 && opened == 0, "Engine Demo did not call its real rendered action");

    const auto bytes = std::u8string(u8"\u041c\u043e\u044f \u0418\u0433\u0440\u0430");
    const std::string name(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Access::fillCreateForm(launcher, name, utf8(fixture.root));
    gui.click(launcher, Access::Action::NewProject);
    gui.settle(launcher);
    gui.click(launcher, Access::Action::CreateProject);
    require(opened == 1 && result.has_value(), "Create Project did not dispatch a usable project");
    require(result->name == name && result->root.filename() == fs::u8path(name), "UTF-8 project name or location was corrupted");
    require(fs::is_regular_file(result->manifestPath) && fs::is_regular_file(result->scenePath), "Blank project files are missing");
    require(manager.recent().size() == 1U, "Created project was not registered as recent");
    gui.settle(launcher);

    Access::fillOpenForm(launcher, utf8(result->root));
    gui.click(launcher, Access::Action::EnterPath);
    gui.settle(launcher);
    gui.click(launcher, Access::Action::OpenTyped);
    require(opened == 2 && result->name == name, "Typed directory fallback did not open the project");
    gui.settle(launcher);
    Access::fillOpenForm(launcher, utf8(fixture.root / "missing.tgeproj"));
    gui.click(launcher, Access::Action::EnterPath);
    gui.settle(launcher);
    gui.click(launcher, Access::Action::OpenTyped);
    require(opened == 2 && !Access::error(launcher).empty(), "Invalid typed project path must report an error without opening");
}

void registryWarningDoesNotBlockOpening() {
    Fixture fixture;
    ProjectManager manager(fixture.root);
    int opened = 0;
    ProjectLauncher launcher(manager, [&](Project) { ++opened; }, [] {});
    Gui gui;
    Access::fillCreateForm(launcher, "Blank", utf8(fixture.root));
    gui.settle(launcher);
    gui.click(launcher, Access::Action::NewProject);
    gui.settle(launcher);
    gui.click(launcher, Access::Action::CreateProject);
    require(opened == 1, "Nonfatal registry persistence warning blocked opening the project");
    require(Access::error(launcher).empty(), "Registry warning was treated as a fatal creation error");
}

void callbackCanReplaceLauncher() {
    Fixture fixture;
    ProjectManager manager(fixture.root / "recent.json");
    Gui gui;
    std::unique_ptr<ProjectLauncher> launcher;
    launcher = std::make_unique<ProjectLauncher>(manager, [](Project) {}, [&] { launcher.reset(); });
    gui.settle(*launcher);
    ImVec2 point;
    Access::actionCenter(*launcher, Access::Action::EngineDemo, point.x, point.y);
    gui.frame(launcher.get(), point, false);
    gui.frame(launcher.get(), point, true);
    gui.frame(launcher.get(), point, false);
    require(!launcher, "Engine Demo replacement callback was not dispatched");
    gui.frame(nullptr);
}
}

int main() {
    try {
        renderAndActions();
        registryWarningDoesNotBlockOpening();
        callbackCanReplaceLauncher();
        std::cout << "ProjectLauncher tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
