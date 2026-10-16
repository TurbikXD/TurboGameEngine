#include "engine/editor/ProjectLauncher.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "engine/core/ProjectManager.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shobjidl.h>
#endif

namespace engine::editor {
namespace {
constexpr ImVec4 kBackground{0.055F, 0.068F, 0.09F, 1.0F};
constexpr ImVec4 kPanel{0.08F, 0.10F, 0.135F, 1.0F};
constexpr ImVec4 kBorder{0.16F, 0.19F, 0.25F, 1.0F};
constexpr ImVec4 kBlue{0.22F, 0.45F, 0.95F, 1.0F};
constexpr ImVec4 kMuted{0.55F, 0.61F, 0.70F, 1.0F};

struct PickerResult final {
    std::optional<std::filesystem::path> path;
    bool unavailable{false};
};

PickerResult pickPath(const bool folder) {
#if defined(_WIN32)
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return {{}, true};
    struct Apartment final { ~Apartment() { CoUninitialize(); } } apartment;
    IFileOpenDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&dialog));
    if (FAILED(created) || dialog == nullptr) return {{}, true};
    struct Dialog final { IFileOpenDialog* value; ~Dialog() { value->Release(); } } owner{dialog};
    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
        FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST)))) return {{}, true};
    if (folder) {
        dialog->SetTitle(L"Choose a folder for your projects");
    } else {
        const COMDLG_FILTERSPEC filter[] = {{L"TurboGameEngine projects (*.tgeproj)", L"*.tgeproj"}};
        if (FAILED(dialog->SetFileTypes(1, filter))) return {{}, true};
        dialog->SetDefaultExtension(L"tgeproj");
        dialog->SetTitle(L"Open a TurboGameEngine project");
    }
    const HRESULT shown = dialog->Show(GetActiveWindow());
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(shown)) return {{}, true};
    IShellItem* selected = nullptr;
    if (FAILED(dialog->GetResult(&selected)) || selected == nullptr) return {{}, true};
    struct Item final { IShellItem* value; ~Item() { value->Release(); } } item{selected};
    PWSTR selectedPath = nullptr;
    if (FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)) || selectedPath == nullptr) return {{}, true};
    const std::filesystem::path path(selectedPath);
    CoTaskMemFree(selectedPath);
    return {path, false};
#else
    (void)folder;
    return {{}, true};
#endif
}

std::string displayPath(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

void heading(const char* text, const float multiplier) {
    ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * multiplier);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

bool primaryButton(const char* label, const ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30F, 0.53F, 1.0F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16F, 0.37F, 0.83F, 1.0F));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}
} // namespace

struct ProjectLauncher::Impl final {
    core::ProjectManager& manager;
    std::function<void(core::Project)> onOpen;
    std::function<void()> onDemo;
    std::vector<core::RecentProject> recent;
    int selected = -1;
    std::string name = "My Game";
    std::string location = displayPath(core::ProjectManager::defaultProjectsDirectory());
    std::string openPath;
    std::string error;
    std::string notice;
    bool focusName = false;
    bool requestedNew = false;
    bool requestedOpen = false;
    bool actionTaken = false;
    std::optional<core::Project> pendingProject;
    bool pendingDemo = false;
    std::array<ImVec2, 6> actionCenters{};

    Impl(core::ProjectManager& service, std::function<void(core::Project)> open, std::function<void()> demo)
        : manager(service), onOpen(std::move(open)), onDemo(std::move(demo)), recent(manager.recent()) {}

    void remember(const ProjectLauncherTestAccess::Action action) {
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        actionCenters[static_cast<std::size_t>(action)] = ImVec2((minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F);
    }

    void refresh() {
        std::filesystem::path selectedPath;
        if (selected >= 0 && static_cast<std::size_t>(selected) < recent.size()) selectedPath = recent[static_cast<std::size_t>(selected)].manifestPath;
        manager.refresh();
        recent = manager.recent();
        selected = -1;
        for (std::size_t index = 0; index < recent.size(); ++index) {
            if (recent[index].manifestPath == selectedPath) { selected = static_cast<int>(index); break; }
        }
    }

    void activate(core::Project project, const std::string& warning) {
        error.clear();
        notice = warning;
        recent = manager.recent();
        if (!onOpen) { error = "The editor is not available."; return; }
        pendingProject = std::move(project);
        actionTaken = true;
    }

    bool open(const std::filesystem::path& path) {
        std::string detail;
        const auto project = manager.open(path, detail);
        if (!project) { error = detail.empty() ? "This project could not be opened." : detail; return false; }
        activate(*project, detail);
        return actionTaken;
    }

    bool create() {
        std::string detail;
        try {
            const auto project = manager.createBlank(std::filesystem::u8path(location), name, detail);
            if (!project) { error = detail.empty() ? "This project could not be created." : detail; return false; }
            activate(*project, detail);
        } catch (const std::filesystem::filesystem_error&) {
            error = "The project location is not a valid path.";
            return false;
        }
        return actionTaken;
    }

    bool openTypedPath() {
        try { return open(std::filesystem::u8path(openPath)); }
        catch (const std::filesystem::filesystem_error&) { error = "The project path is not valid."; return false; }
    }

    void dispatch() {
        if (pendingProject) {
            auto project = std::move(*pendingProject);
            pendingProject.reset();
            try { onOpen(std::move(project)); }
            catch (const std::exception& failure) { error = std::string("Could not open the editor: ") + failure.what(); }
        } else if (pendingDemo) {
            pendingDemo = false;
            try { onDemo(); }
            catch (const std::exception& failure) { error = std::string("Could not open the engine demo: ") + failure.what(); }
        }
    }

    void openPicker() {
        const auto chosen = pickPath(false);
        if (chosen.path) { openPath = displayPath(*chosen.path); open(*chosen.path); }
        else if (chosen.unavailable) { requestedOpen = true; error.clear(); }
    }
};

ProjectLauncher::ProjectLauncher(core::ProjectManager& manager, std::function<void(core::Project)> onOpen,
                                 std::function<void()> onEngineDemo)
    : m_impl(std::make_shared<Impl>(manager, std::move(onOpen), std::move(onEngineDemo))) {}
ProjectLauncher::~ProjectLauncher() = default;

void ProjectLauncher::render() {
    // A callback may replace the launcher; keep its state alive through dispatch.
    const auto lifetime = m_impl;
    Impl& state = *lifetime;
    state.actionTaken = false;
    state.actionCenters.fill(ImVec2(-1.0F, -1.0F));
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88F, 0.91F, 0.96F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, kMuted);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36.0F, 30.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0F, 10.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0F, 12.0F));
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("TurboGameEngine Projects", nullptr, flags)) {
        ImGui::End(); ImGui::PopStyleVar(5); ImGui::PopStyleColor(5); return;
    }
    ImGui::TextColored(ImVec4(0.44F, 0.65F, 1.0F, 1.0F), "TURBO / GAME ENGINE");
    ImGui::Spacing();
    heading("Projects", 2.4F);
    ImGui::TextDisabled("Create a game, open a project, or explore the engine.");
    ImGui::Spacing();
    if (primaryButton("New Project", ImVec2(150.0F, 42.0F))) { state.requestedNew = true; state.error.clear(); state.focusName = true; }
    state.remember(ProjectLauncherTestAccess::Action::NewProject);
    ImGui::SameLine();
    if (ImGui::Button("Open Existing Project", ImVec2(205.0F, 42.0F))) state.openPicker();
    ImGui::SameLine();
    if (ImGui::Button("Enter path", ImVec2(110.0F, 42.0F))) { state.requestedOpen = true; state.error.clear(); }
    state.remember(ProjectLauncherTestAccess::Action::EnterPath);
    ImGui::Spacing();

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const bool stacked = availableWidth < 760.0F;
    const float leftWidth = stacked ? availableWidth : (availableWidth - 22.0F) * 0.64F;
    const float listHeight = stacked ? 235.0F : std::max(230.0F, ImGui::GetContentRegionAvail().y - 64.0F);
    if (ImGui::BeginChild("Recent Projects", ImVec2(leftWidth, listHeight), ImGuiChildFlags_Borders)) {
        ImGui::TextUnformatted("RECENT PROJECTS");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) state.refresh();
        ImGui::Separator();
        if (state.recent.empty()) {
            ImGui::Dummy(ImVec2(0, 30));
            heading("Your next game starts here", 1.25F);
            ImGui::TextWrapped("Create a Blank Project to start with an empty scene. Your projects will appear here after opening them.");
        }
        for (std::size_t index = 0; index < state.recent.size(); ++index) {
            const auto project = state.recent[index];
            ImGui::PushID(static_cast<int>(index));
            const ImVec2 position = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##ProjectRow", state.selected == static_cast<int>(index), ImGuiSelectableFlags_AllowDoubleClick,
                ImVec2(ImGui::GetContentRegionAvail().x, 64.0F))) {
                state.selected = static_cast<int>(index);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !state.actionTaken) {
                    // Copy before backend.open refreshes the recent list.
                    const auto manifest = project.manifestPath;
                    state.open(manifest);
                }
            }
            // Backend activation may replace the list; draw using copied values after that action.
            if (!state.actionTaken) {
                const std::string name = project.name.empty() ? "Untitled project" : project.name;
                const std::string path = displayPath(project.manifestPath.parent_path());
                const ImVec2 maximum(position.x + ImGui::GetContentRegionAvail().x, position.y + 64.0F);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->PushClipRect(position, maximum, true);
                draw->AddText(ImVec2(position.x + 10.0F, position.y + 8.0F), IM_COL32(223, 230, 243, 255), name.c_str());
                draw->AddText(ImVec2(position.x + 10.0F, position.y + 32.0F), IM_COL32(137, 153, 176, 255), path.c_str());
                draw->PopClipRect();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", displayPath(project.manifestPath).c_str());
            }
            ImGui::PopID();
            if (state.actionTaken) break;
        }
        ImGui::Spacing();
        const bool selectedValid = state.selected >= 0 && static_cast<std::size_t>(state.selected) < state.recent.size();
        ImGui::BeginDisabled(!selectedValid || state.actionTaken);
        if (primaryButton("Open Selected", ImVec2(145.0F, 38.0F))) {
            const auto manifest = state.recent[static_cast<std::size_t>(state.selected)].manifestPath;
            state.open(manifest);
        }
        state.remember(ProjectLauncherTestAccess::Action::OpenSelected);
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    if (!stacked) ImGui::SameLine(0.0F, 22.0F);
    if (ImGui::BeginChild("Create a Project", ImVec2(stacked ? availableWidth : 0.0F, stacked ? 230.0F : listHeight), ImGuiChildFlags_Borders)) {
        ImGui::TextUnformatted("START SOMETHING NEW");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11F, 0.18F, 0.29F, 1.0F));
        if (ImGui::Button("+   Blank Project", ImVec2(-1.0F, 100.0F))) { state.requestedNew = true; state.error.clear(); state.focusName = true; }
        ImGui::PopStyleColor();
        ImGui::TextWrapped("An empty scene and your own assets. Build gameplay with Luau and UI with React Luau.");
        ImGui::Spacing();
        if (ImGui::Button("Engine Demo", ImVec2(-1.0F, 38.0F)) && !state.actionTaken) {
            if (state.onDemo) { state.pendingDemo = true; state.actionTaken = true; }
            else state.error = "The engine demo is not available.";
        }
        state.remember(ProjectLauncherTestAccess::Action::EngineDemo);
        ImGui::TextDisabled("Explore the built-in engine scene.");
    }
    ImGui::EndChild();
    if (!state.error.empty()) ImGui::TextColored(ImVec4(1.0F, 0.43F, 0.42F, 1.0F), "%s", state.error.c_str());
    if (!state.notice.empty()) ImGui::TextWrapped("%s", state.notice.c_str());

    if (state.requestedNew) { ImGui::OpenPopup("New Blank Project"); state.requestedNew = false; }
    if (state.requestedOpen) { ImGui::OpenPopup("Open Existing Project"); state.requestedOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(580.0F, 0.0F), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("New Blank Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Blank Project");
        ImGui::TextDisabled("Start with an empty scene in a new project folder.");
        ImGui::Separator();
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-1.0F);
        if (state.focusName) { ImGui::SetKeyboardFocusHere(); state.focusName = false; }
        ImGui::InputTextWithHint("##Name", "Project name", &state.name);
        ImGui::TextUnformatted("Location");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0F);
        ImGui::InputText("##Location", &state.location);
        ImGui::SameLine();
        if (ImGui::Button("Browse", ImVec2(98.0F, 0.0F))) {
            const auto selected = pickPath(true);
            if (selected.path) state.location = displayPath(*selected.path);
            else if (selected.unavailable) state.error = "Folder picker unavailable. Enter the location above.";
        }
        ImGui::TextWrapped("Project folder: %s / %s", state.location.c_str(), state.name.c_str());
        if (!state.error.empty()) ImGui::TextWrapped("%s", state.error.c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(state.name.empty() || state.location.empty() || state.actionTaken);
        if (primaryButton("Create Project", ImVec2(165.0F, 0.0F)) && state.create()) ImGui::CloseCurrentPopup();
        state.remember(ProjectLauncherTestAccess::Action::CreateProject);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0F, 0.0F))) { state.error.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSize(ImVec2(580.0F, 0.0F), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Open Existing Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Open a .tgeproj file or its project folder.");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##ProjectPath", "Path to your project", &state.openPath);
        if (ImGui::Button("Browse files")) {
            const auto selected = pickPath(false);
            if (selected.path) state.openPath = displayPath(*selected.path);
            else if (selected.unavailable) state.error = "File picker unavailable. Enter the path above.";
        }
        if (!state.error.empty()) ImGui::TextWrapped("%s", state.error.c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(state.openPath.empty() || state.actionTaken);
        if (primaryButton("Open Project", ImVec2(165.0F, 0.0F)) && state.openTypedPath()) ImGui::CloseCurrentPopup();
        state.remember(ProjectLauncherTestAccess::Action::OpenTyped);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0F, 0.0F))) { state.error.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(5);
    state.dispatch();
}

void ProjectLauncherTestAccess::actionCenter(const ProjectLauncher& launcher, const Action action, float& x, float& y) {
    const auto point = launcher.m_impl->actionCenters[static_cast<std::size_t>(action)];
    x = point.x; y = point.y;
}
void ProjectLauncherTestAccess::fillCreateForm(ProjectLauncher& launcher, std::string name, std::string location) {
    launcher.m_impl->name = std::move(name);
    launcher.m_impl->location = std::move(location);
}
void ProjectLauncherTestAccess::fillOpenForm(ProjectLauncher& launcher, std::string path) { launcher.m_impl->openPath = std::move(path); }
std::string ProjectLauncherTestAccess::error(const ProjectLauncher& launcher) { return launcher.m_impl->error; }

} // namespace engine::editor
