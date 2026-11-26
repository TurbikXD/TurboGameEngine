#include "engine/editor/ScriptWorkspace.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <regex>
#include <utility>
#include <vector>
#include "engine/core/AtomicFile.h"
#include "engine/core/LiveFileWatcher.h"
#include "engine/scripting/ScriptingSystem.h"
#include "engine/editor/ScriptFormatter.h"
#include "engine/editor/EditorTheme.h"
#include "third_party/ImGuiColorTextEdit/TextEditor.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/misc/cpp/imgui_stdlib.h"

namespace engine::editor {
namespace {
constexpr std::size_t kMaxDocumentBytes = 2U * 1024U * 1024U;

bool readDocument(const std::filesystem::path& path, std::string& text) {
    std::error_code error;
    if (std::filesystem::file_size(path, error) > kMaxDocumentBytes || error) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string contents;
    std::array<char, 16384> buffer{};
    while (file) {
        const auto remaining = kMaxDocumentBytes + 1U - contents.size();
        file.read(buffer.data(), static_cast<std::streamsize>(std::min(buffer.size(), remaining)));
        contents.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
        if (contents.size() > kMaxDocumentBytes) return false;
    }
    if (!file.eof()) return false;
    text = std::move(contents);
    return true;
}

TextEditor::Coordinates coordinatesAt(const std::string& text, const std::size_t offset) {
    int line = 0, column = 0;
    for (std::size_t i = 0; i < std::min(offset, text.size()); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\n') { ++line; column = 0; }
        else if (c == '\t') { column = (column / 4 + 1) * 4; }
        else if ((c & 0xc0U) != 0x80U) { ++column; }
    }
    return {line, column};
}

TextEditor::LanguageDefinition luauLanguage() {
    auto language = TextEditor::LanguageDefinition::Lua();
    language.mName = "Luau";
    for (const auto* keyword : {"continue", "type", "export", "typeof"}) { language.mKeywords.insert(keyword); }
    const std::pair<const char*, const char*> api[] = {
        {"Engine", "Engine: scene, input, prefab, proximity and game status services"},
        {"React", "require(\"React\"): the upstream React Luau package"},
        {"ReactTurbo", "require(\"ReactTurbo\"): TurboGameEngine's ImGui host renderer"},
        {"Input", "Input.isKeyDown(key), Input.wasKeyPressed(key): capture-aware engine input"},
        {"Init", "Init(self): called when a behavior is started"},
        {"Update", "Update(self, dt): called each simulation tick"},
        {"OnEvent", "OnEvent(self, event): engine collision and script events"}
    };
    for (const auto& [name, description] : api) {
        TextEditor::Identifier identifier;
        identifier.mDeclaration = description;
        language.mIdentifiers[name] = std::move(identifier);
    }
    return language;
}

}

struct ScriptWorkspace::Impl {
    struct Document {
        std::string path;
        TextEditor editor;
        std::string savedText;
        std::string diskText;
        bool exists{false};
        bool dirty{false};
        Document() { editor.SetLanguageDefinition(luauLanguage()); editor.SetTabSize(4); editor.SetShowWhitespaces(false); }
    };
    std::filesystem::path assets;
    scripting::ScriptingSystem* scripts{};
    Actions actions;
    EditorCommandRegistry* commandRegistry{};
    ScriptFormatter formatter;
    core::LiveFileWatcher watcher;
    std::vector<std::unique_ptr<Document>> documents;
    int active{0};
    bool focused{false};
    bool selectTab{false};
    bool showFind{false};
    enum class Popup { Open, GoToLine, ReloadDisk };
    enum class ToolTab { Output, Command, State, Errors, Activity };
    std::optional<Popup> popupRequest;
    std::string popupDocument;
    ToolTab activeToolTab{ToolTab::Output};
    std::optional<ToolTab> selectToolTab;
    bool autoScroll{true};
    std::string find;
    std::string command;
    std::string status{"Open a .luau file from the Content Browser or the Open menu."};
    std::size_t findOffset{0};
    std::size_t outputSize{0};
    int gotoLine{1};
    std::optional<std::size_t> closeRequested;

    Document* current() { return active >= 0 && static_cast<std::size_t>(active) < documents.size() ? documents[active].get() : nullptr; }
    const Document* current() const { return active >= 0 && static_cast<std::size_t>(active) < documents.size() ? documents[active].get() : nullptr; }
    std::optional<std::filesystem::path> resolve(std::string relative) const {
        if (relative.starts_with("assets/")) { relative.erase(0, 7); }
        const std::filesystem::path path(relative);
        if (path.is_absolute() || (path.extension() != ".lua" && path.extension() != ".luau")) { return std::nullopt; }
        std::error_code error;
        const auto target = std::filesystem::weakly_canonical(assets / path, error);
        if (error) { return std::nullopt; }
        const auto rel = target.lexically_relative(assets);
        if (rel.empty() || *rel.begin() == "..") { return std::nullopt; }
        return target;
    }
    bool load(Document& document) {
        const auto path = resolve(document.path);
        if (!path) { status = "Script path must remain inside assets."; return false; }
        std::string text;
        if (!readDocument(*path, text)) { status = "Cannot read script (missing file or exceeds 2 MiB): " + document.path; return false; }
        document.editor.SetText(text);
        document.savedText = document.editor.GetText();
        document.diskText = std::move(text);
        document.exists = true;
        document.dirty = false;
        status = "Opened " + document.path;
        return true;
    }
    bool save() {
        auto* document = current();
        if (!document) { return false; }
        const auto path = resolve(document->path);
        if (!path) { status = "Script path must remain inside assets."; return false; }
        std::error_code error;
        const bool exists = std::filesystem::exists(*path, error);
        if (error) { status = "Cannot inspect script path."; return false; }
        std::string diskText;
        if ((document->exists && (!exists || !readDocument(*path, diskText) || diskText != document->diskText)) || (!document->exists && exists)) {
            status = "File changed on disk. Use Reload Disk to review it before saving.";
            return false;
        }
        const auto text = document->editor.GetText();
        if (text.size() > kMaxDocumentBytes) { status = "Script exceeds 2 MiB."; return false; }
        std::filesystem::create_directories(path->parent_path(), error);
        if (error) { status = "Cannot create script directory."; return false; }
        std::string detail;
        if (!core::writeFileAtomically(*path, text, detail)) { status = "Script save failed; original preserved. " + detail; return false; }
        if (!document->exists) watcher.watch(*path);
        document->savedText = text;
        document->diskText = text;
        document->exists = true;
        document->dirty = false;
        status = "Saved " + document->path;
        return true;
    }
    void check() {
        auto* document = current();
        if (!document || !scripts) { return; }
        const bool valid = scripts->analyze(document->editor.GetText(), document->path);
        TextEditor::ErrorMarkers markers;
        if (!valid && !scripts->output().empty()) {
            const auto& message = scripts->output().back().text;
            std::smatch match;
            if (std::regex_search(message, match, std::regex(":([0-9]+):"))) { markers[std::stoi(match[1])] = message; }
        }
        document->editor.SetErrorMarkers(markers);
        status = valid ? "Syntax OK: " + document->path : "Syntax error; see Output.";
    }
    void findNext() {
        auto* document = current();
        if (!document || find.empty()) { return; }
        const auto text = document->editor.GetText();
        auto offset = text.find(find, findOffset);
        if (offset == std::string::npos) { offset = text.find(find); }
        if (offset == std::string::npos) { status = "No matches."; return; }
        const auto start = coordinatesAt(text, offset);
        const auto end = coordinatesAt(text, offset + find.size());
        document->editor.SetCursorPosition(start);
        document->editor.SetSelection(start, end);
        findOffset = offset + find.size();
    }
    void toggleComment() {
        auto* document = current();
        if (!document) { return; }
        auto& editor = document->editor;
        const auto cursor = editor.GetCursorPosition();
        const auto line = editor.GetCurrentLineText();
        const auto first = line.find_first_not_of(" \t");
        auto replacement = line;
        if (first != std::string::npos && line.compare(first, 2, "--") == 0) { replacement.erase(first, 2); }
        else { replacement.insert(first == std::string::npos ? 0 : first, "--"); }
        editor.SetSelection({cursor.mLine, 0}, {cursor.mLine, coordinatesAt(line, line.size()).mColumn});
        editor.ReplaceSelection(replacement);
        document->dirty = editor.GetText() != document->savedText;
    }
};

ScriptWorkspace::ScriptWorkspace() : m_impl(std::make_unique<Impl>()) {}
ScriptWorkspace::~ScriptWorkspace() = default;
void ScriptWorkspace::initialize(std::filesystem::path assets, scripting::ScriptingSystem& scripts, Actions actions, core::JobSystem* jobs) {
    std::error_code error;
    m_impl->assets = std::filesystem::weakly_canonical(std::move(assets), error);
    m_impl->scripts = &scripts;
    m_impl->actions = std::move(actions);
    m_impl->watcher.initialize(jobs);
}
bool ScriptWorkspace::openScript(const std::string& path) {
    auto relative = path;
    if (relative.starts_with("assets/")) { relative.erase(0, 7); }
    const auto target = m_impl->resolve(relative);
    if (!target) { return false; }
    relative = target->lexically_relative(m_impl->assets).generic_string();
    for (std::size_t i = 0; i < m_impl->documents.size(); ++i) {
        if (m_impl->documents[i]->path == relative) { m_impl->active = static_cast<int>(i); m_impl->selectTab = true; return true; }
    }
    auto document = std::make_unique<Impl::Document>();
    document->path = relative;
    if (!m_impl->load(*document)) { return false; }
    m_impl->watcher.watch(*target);
    m_impl->documents.push_back(std::move(document));
    m_impl->active = static_cast<int>(m_impl->documents.size() - 1);
    m_impl->selectTab = true;
    return true;
}
bool ScriptWorkspace::focused() const { return m_impl->focused; }
void ScriptWorkspace::update() {
    auto& impl = *m_impl;
    for (const auto& change : impl.watcher.poll()) {
        for (auto& document : impl.documents) {
            if (impl.assets / document->path != change.path) continue;
            if (!change.error.empty()) { impl.status = "External change unavailable: " + change.error; continue; }
            if (change.contents == document->diskText) continue;
            if (document->editor.GetText() != document->savedText) {
                impl.status = "Changed on disk; local edits kept. Use Reload Disk to resolve."; continue;
            }
            if (impl.load(*document)) impl.status = "Live: updated " + document->path + " from disk.";
        }
    }
    const auto result = impl.formatter.poll();
    if (!result) { return; }
    if (!result->success) { impl.status = "Format failed: " + result->error; return; }
    const auto document = std::find_if(impl.documents.begin(), impl.documents.end(), [&](const auto& item) { return item->path == result->documentPath; });
    if (document == impl.documents.end() || (*document)->editor.GetText() != result->original) {
        impl.status = "Format finished after the document changed; no edits were applied.";
        return;
    }
    auto& editor = (*document)->editor;
    editor.SelectAll();
    editor.ReplaceSelection(result->formatted);
    (*document)->dirty = editor.GetText() != (*document)->savedText;
    impl.status = "Formatted " + result->documentPath + " with StyLua (verified).";
}
bool ScriptWorkspace::saveActive() { return m_impl->save(); }
bool ScriptWorkspace::saveAll() {
    const int previous = m_impl->active;
    for (std::size_t index = 0; index < m_impl->documents.size(); ++index) {
        const auto& document = *m_impl->documents[index];
        if (document.exists && document.editor.GetText() == document.savedText) { continue; }
        m_impl->active = static_cast<int>(index);
        if (!m_impl->save()) { m_impl->selectTab = true; return false; }
    }
    m_impl->active = previous;
    return true;
}
void ScriptWorkspace::setCommandRegistry(EditorCommandRegistry* registry) { m_impl->commandRegistry = registry; }
CommandState ScriptWorkspace::commandState(const EditorCommandId command) const {
    const auto& impl = *m_impl;
    const auto* document = impl.current();
    const auto needsDocument = [&]() -> CommandState {
        return {document != nullptr, false, document ? std::string_view{} : "Open a script document first."};
    };
    const auto toolTab = impl.selectToolTab.value_or(impl.activeToolTab);
    switch (command) {
    case EditorCommandId::ScriptNew:
    case EditorCommandId::ScriptOpen:
        return {!impl.assets.empty(), false, impl.assets.empty() ? "Open a project first." : std::string_view{}};
    case EditorCommandId::ScriptSave:
    case EditorCommandId::ScriptGoToLine:
    case EditorCommandId::ScriptComment:
        return needsDocument();
    case EditorCommandId::ScriptFind: {
        auto state = needsDocument(); state.checked = impl.showFind; return state;
    }
    case EditorCommandId::ScriptSaveAll:
        return {!impl.documents.empty(), false, impl.documents.empty() ? "Open a script document first." : std::string_view{}};
    case EditorCommandId::ScriptSaveReload:
        if (!document) return needsDocument();
        if (!document->exists) return {false, false, "Save the new script first."};
        if (!impl.scripts) return {false, false, "Scripting is unavailable."};
        return {!impl.scripts->reloadPending(), false, impl.scripts->reloadPending() ? "A script reload is pending." : std::string_view{}};
    case EditorCommandId::ScriptCheck:
        if (!document) return needsDocument();
        return {impl.scripts != nullptr, false, impl.scripts ? std::string_view{} : "Scripting is unavailable."};
    case EditorCommandId::ScriptFormat:
        if (!document) return needsDocument();
        if (impl.formatter.busy()) return {false, false, "Formatting is in progress."};
        return {impl.formatter.available(), false, impl.formatter.available() ? std::string_view{} : "StyLua is unavailable."};
    case EditorCommandId::ScriptReloadDisk:
        if (!document) return needsDocument();
        return {document->exists, false, document->exists ? std::string_view{} : "This script has not been saved yet."};
    case EditorCommandId::ScriptOutput:
    case EditorCommandId::ScriptCommand:
    case EditorCommandId::ScriptState:
    case EditorCommandId::ScriptErrors: {
        const auto target = command == EditorCommandId::ScriptOutput ? Impl::ToolTab::Output :
            command == EditorCommandId::ScriptCommand ? Impl::ToolTab::Command :
            command == EditorCommandId::ScriptState ? Impl::ToolTab::State : Impl::ToolTab::Errors;
        return {impl.scripts != nullptr, toolTab == target, impl.scripts ? std::string_view{} : "Scripting is unavailable."};
    }
    default: return {false, false, "Not a script workspace command."};
    }
}
bool ScriptWorkspace::executeCommand(const EditorCommandId command) {
    if (!commandState(command).enabled) return false;
    auto& impl = *m_impl;
    auto* document = impl.current();
    switch (command) {
    case EditorCommandId::ScriptNew:
        for (int i = 1; i < 10000; ++i) {
            auto created = std::make_unique<Impl::Document>();
            created->path = "scripts/Script" + std::to_string(i) + ".luau";
            const auto path = impl.resolve(created->path);
            const bool inTabs = std::any_of(impl.documents.begin(), impl.documents.end(), [&](const auto& item) { return item->path == created->path; });
            std::error_code error;
            if (path && !std::filesystem::exists(*path, error) && !error && !inTabs) {
                created->editor.SetText("return {\n    fields = {},\n    Init = function(self)\n        print(\"Hello world!\")\n    end,\n    Update = function(self, dt)\n    end,\n}\n");
                created->dirty = true;
                impl.documents.push_back(std::move(created));
                impl.active = static_cast<int>(impl.documents.size() - 1);
                impl.selectTab = true;
                return true;
            }
        }
        impl.status = "Cannot choose an unused script path.";
        return false;
    case EditorCommandId::ScriptOpen: impl.popupRequest = Impl::Popup::Open; return true;
    case EditorCommandId::ScriptSave: return impl.save();
    case EditorCommandId::ScriptSaveAll: return saveAll();
    case EditorCommandId::ScriptSaveReload:
        if (!impl.save()) return false;
        return impl.scripts->requestReload(document->path);
    case EditorCommandId::ScriptCheck: impl.check(); return true;
    case EditorCommandId::ScriptFind: impl.showFind = !impl.showFind; return true;
    case EditorCommandId::ScriptGoToLine:
        impl.popupDocument = document->path;
        impl.popupRequest = Impl::Popup::GoToLine;
        return true;
    case EditorCommandId::ScriptComment: impl.toggleComment(); return true;
    case EditorCommandId::ScriptFormat: return impl.formatter.start(document->path, document->editor.GetText());
    case EditorCommandId::ScriptReloadDisk:
        impl.popupDocument = document->path;
        impl.popupRequest = Impl::Popup::ReloadDisk;
        return true;
    case EditorCommandId::ScriptOutput: impl.selectToolTab = Impl::ToolTab::Output; return true;
    case EditorCommandId::ScriptCommand: impl.selectToolTab = Impl::ToolTab::Command; return true;
    case EditorCommandId::ScriptState: impl.selectToolTab = Impl::ToolTab::State; return true;
    case EditorCommandId::ScriptErrors: impl.selectToolTab = Impl::ToolTab::Errors; return true;
    default: return false;
    }
}
std::string ScriptWorkspace::assistantContext() const {
    const auto* document = m_impl->current();
    if (!document) { return {}; }
    const auto selected = document->editor.GetSelectedText();
    return "Active Luau script: assets/" + document->path + "\n" +
        (selected.empty() ? document->editor.GetText() : selected);
}

void ScriptWorkspace::render(bool* open, const ecs::EntityId selected, const bool playing, const bool running) {
    auto& impl = *m_impl;
    impl.focused = false;
    if (open && !*open) { return; }
    const bool docked = impl.commandRegistry != nullptr;
    if (docked) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    const auto windowFlags = ImGuiWindowFlags_NoCollapse | (docked ?
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoTitleBar : ImGuiWindowFlags_None);
    if (!ImGui::Begin("Script Workspace", open, windowFlags)) {
        ImGui::End(); if (docked) ImGui::PopStyleVar(); return;
    }
    if (docked) ImGui::PopStyleVar();
    impl.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const auto dispatch = [&](const EditorCommandId command) {
        return impl.commandRegistry ? impl.commandRegistry->execute(command) : executeCommand(command);
    };
    const auto commandButton = [&](const char* label, const EditorCommandId command) {
        const auto state = impl.commandRegistry ? impl.commandRegistry->state(command) : commandState(command);
        ImGui::BeginDisabled(!state.enabled);
        const bool pressed = ImGui::Button(label);
        ImGui::EndDisabled();
        if (!state.enabled && !state.disabledReason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(state.disabledReason.data(), state.disabledReason.data() + state.disabledReason.size());
            ImGui::EndTooltip();
        }
        if (pressed) dispatch(command);
    };
    const auto playButton = [&](const char* label, const EditorCommandId command, const std::function<void()>& fallback, const bool enabled = true) {
        const bool allowed = impl.commandRegistry ? impl.commandRegistry->state(command).enabled : enabled && static_cast<bool>(fallback);
        ImGui::BeginDisabled(!allowed);
        if (ImGui::Button(label)) {
            if (impl.commandRegistry) impl.commandRegistry->execute(command);
            else if (fallback) fallback();
        }
        ImGui::EndDisabled();
    };
    if (!impl.commandRegistry && impl.focused && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt) {
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) dispatch(ImGui::GetIO().KeyShift ? EditorCommandId::ScriptSaveAll : EditorCommandId::ScriptSave);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) impl.showFind = true;
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) dispatch(EditorCommandId::ScriptGoToLine);
    }
    if (!docked) {
        playButton(playing ? "Resume" : "Play in Editor", EditorCommandId::Play, impl.actions.play);
        ImGui::SameLine();
        playButton("Play Window", EditorCommandId::PlayWindow, impl.actions.playWindow);
        ImGui::SameLine();
        playButton("Pause", EditorCommandId::Pause, impl.actions.pause, playing);
        ImGui::SameLine(); playButton("Stop", EditorCommandId::Stop, impl.actions.stop, playing);
        ImGui::SameLine(); playButton("Step Tick", EditorCommandId::StepTick, impl.actions.step, playing);
        ImGui::SameLine(); ImGui::TextDisabled("%s", playing ? (running ? "RUNNING" : "PAUSED") : "EDIT");
        ImGui::SameLine();
        playButton("Lab 2 Demo", EditorCommandId::ScriptDemo, impl.actions.demo);
        ImGui::Separator();
        commandButton("New Script", EditorCommandId::ScriptNew);
        ImGui::SameLine(); commandButton("Open", EditorCommandId::ScriptOpen);
        ImGui::SameLine(); commandButton("Save", EditorCommandId::ScriptSave);
        ImGui::SameLine(); commandButton("Save All", EditorCommandId::ScriptSaveAll);
        ImGui::SameLine(); commandButton("Save + Reload", EditorCommandId::ScriptSaveReload);
        ImGui::SameLine(); commandButton("Syntax Check", EditorCommandId::ScriptCheck);
        ImGui::SameLine(); commandButton("Find", EditorCommandId::ScriptFind);
        ImGui::SameLine(); commandButton("Go to Line", EditorCommandId::ScriptGoToLine);
        ImGui::SameLine(); commandButton("Comment", EditorCommandId::ScriptComment);
        ImGui::SameLine(); commandButton(impl.formatter.busy() ? "Formatting..." : "Format", EditorCommandId::ScriptFormat);
        ImGui::SameLine(); commandButton("Reload Disk", EditorCommandId::ScriptReloadDisk);
    }
    if (impl.popupRequest) {
        switch (*impl.popupRequest) {
        case Impl::Popup::Open: ImGui::OpenPopup("Open Script"); break;
        case Impl::Popup::GoToLine: ImGui::OpenPopup("Go to Line"); break;
        case Impl::Popup::ReloadDisk: ImGui::OpenPopup("Reload Disk?"); break;
        }
        impl.popupRequest.reset();
    }
    if (ImGui::BeginPopup("Open Script")) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(impl.assets / "scripts", error), end; !error && it != end; it.increment(error)) {
            if (it->is_directory(error) && it->path().filename() == "react-luau") { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(error)) { continue; }
            const auto relative = it->path().lexically_relative(impl.assets).generic_string();
            if ((it->path().extension() == ".luau" || it->path().extension() == ".lua") && ImGui::Selectable(relative.c_str())) { openScript(relative); }
        }
        ImGui::EndPopup();
    }
    const auto popupDocument = [&]() -> Impl::Document* {
        const auto it = std::find_if(impl.documents.begin(), impl.documents.end(), [&](const auto& item) { return item->path == impl.popupDocument; });
        return it == impl.documents.end() ? nullptr : it->get();
    };
    if (ImGui::BeginPopupModal("Reload Disk?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Replace %s with the file on disk?", impl.popupDocument.c_str());
        if (ImGui::Button("Reload")) { if (auto* target = popupDocument()) impl.load(*target); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("Go to Line")) {
        ImGui::InputInt("Line", &impl.gotoLine);
        if (ImGui::Button("Go")) {
            if (auto* target = popupDocument()) {
                target->editor.SetCursorPosition({std::clamp(impl.gotoLine - 1, 0, target->editor.GetTotalLines() - 1), 0});
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (impl.showFind) {
        ImGui::SetNextItemWidth(220.0F);
        if (ImGui::InputText("Find text", &impl.find, ImGuiInputTextFlags_EnterReturnsTrue)) { impl.findNext(); }
        ImGui::SameLine(); if (ImGui::Button("Next")) { impl.findNext(); }
    }
    const float editorHeight = std::max(80.0F, ImGui::GetContentRegionAvail().y - 205.0F - (docked ? kPanelTitleHeight : 0.0F));
    if (ImGui::BeginTabBar("Script Documents", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (std::size_t i = 0; i < impl.documents.size(); ++i) {
            auto& item = *impl.documents[i];
            bool tabOpen = true;
            ImGuiTabItemFlags flags = item.dirty ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
            if (impl.selectTab && static_cast<int>(i) == impl.active) { flags |= ImGuiTabItemFlags_SetSelected; }
            if (ImGui::BeginTabItem(item.path.c_str(), &tabOpen, flags)) {
                impl.active = static_cast<int>(i);
                ImGui::PushFont(codeFont(), kCodeFontSize);
                item.editor.Render("Luau Source", {0.0F, editorHeight}, !docked);
                ImGui::PopFont();
                if (item.editor.IsTextChanged()) { item.dirty = item.editor.GetText() != item.savedText; }
                ImGui::EndTabItem();
            }
            if (!tabOpen) { impl.closeRequested = i; }
        }
        ImGui::EndTabBar();
    }
    impl.selectTab = false;
    if (impl.closeRequested) {
        const auto i = *impl.closeRequested;
        if (!impl.documents[i]->dirty) {
            impl.documents.erase(impl.documents.begin() + static_cast<std::ptrdiff_t>(i));
            impl.active = std::clamp(impl.active, 0, std::max(0, static_cast<int>(impl.documents.size()) - 1));
            impl.closeRequested.reset();
        } else { ImGui::OpenPopup("Unsaved Script"); }
    }
    if (ImGui::BeginPopupModal("Unsaved Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Save this script before closing?");
        if (ImGui::Button("Save and Close") && impl.closeRequested) {
            impl.active = static_cast<int>(*impl.closeRequested);
            if (impl.save()) { impl.documents.erase(impl.documents.begin() + impl.active); impl.closeRequested.reset(); ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard") && impl.closeRequested) { impl.documents.erase(impl.documents.begin() + static_cast<std::ptrdiff_t>(*impl.closeRequested)); impl.closeRequested.reset(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) { impl.closeRequested.reset(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (impl.documents.empty()) { ImGui::Dummy({0.0F, editorHeight}); }
    if (docked) {
        const char* title = "Output";
        switch (impl.selectToolTab.value_or(impl.activeToolTab)) {
        case Impl::ToolTab::Output: break;
        case Impl::ToolTab::Command: title = "Command"; break;
        case Impl::ToolTab::State: title = "State"; break;
        case Impl::ToolTab::Errors: title = "Errors"; break;
        case Impl::ToolTab::Activity: title = "Script Activity"; break;
        }
        const auto origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, {origin.x + width, origin.y + kPanelTitleHeight}, IM_COL32(32, 34, 39, 255));
        auto* font = nativeFont();
        if (!font) font = ImGui::GetFont();
        const auto size = font->CalcTextSizeA(kEditorFontSize, width, 0.0F, title);
        draw->AddText(font, kEditorFontSize,
            {origin.x + std::max(4.0F, (width - 36.0F - size.x) * 0.5F), origin.y + (kPanelTitleHeight - size.y) * 0.5F},
            IM_COL32(197, 200, 207, 255), title);
        ImGui::Dummy({0.0F, kPanelTitleHeight});
        ImGui::SetCursorScreenPos({origin.x, origin.y + kPanelTitleHeight});
    }
    if (ImGui::BeginTabBar("Script Tools", ImGuiTabBarFlags_FittingPolicyScroll)) {
        const auto beginToolTab = [&](const char* label, const Impl::ToolTab tab) {
            const bool requested = impl.selectToolTab && *impl.selectToolTab == tab;
            const auto flags = requested ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (!ImGui::BeginTabItem(label, nullptr, flags)) return false;
            impl.activeToolTab = tab;
            if (requested) impl.selectToolTab.reset();
            return true;
        };
        if (beginToolTab("Output###ScriptOutput", Impl::ToolTab::Output)) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, docked ? 0.0F : ImGui::GetStyle().FrameBorderSize);
            if (ImGui::SmallButton("Clear") && impl.scripts) { impl.scripts->clearOutput(); }
            ImGui::SameLine(); ImGui::Checkbox("Auto-scroll", &impl.autoScroll);
            ImGui::PopStyleVar();
            if (docked) ImGui::Separator();
            if (ImGui::BeginChild("Script Output", {0.0F, 104.0F}, docked ? ImGuiChildFlags_None : ImGuiChildFlags_Borders)) {
                ImGui::PushFont(codeFont(), kCodeFontSize);
                if (impl.scripts) {
                    const auto& output = impl.scripts->output();
                    for (const auto& message : output) {
                        const bool error = message.kind == scripting::ScriptMessage::Kind::Error;
                        if (error) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F)); }
                        ImGui::TextUnformatted(message.text.c_str());
                        if (error) { ImGui::PopStyleColor(); }
                    }
                    if (impl.autoScroll && impl.outputSize != output.size()) { ImGui::SetScrollHereY(1.0F); }
                    impl.outputSize = output.size();
                }
                ImGui::PopFont();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (beginToolTab("Command###ScriptCommand", Impl::ToolTab::Command)) {
            ImGui::TextDisabled("Execute Luau in the current VM. Commands can change the scene.");
            ImGui::PushFont(codeFont(), kCodeFontSize);
            ImGui::InputTextMultiline("##Command", &impl.command, {0.0F, 78.0F}, ImGuiInputTextFlags_AllowTabInput);
            ImGui::PopFont();
            if (ImGui::Button("Execute") && impl.scripts && !impl.command.empty()) { impl.scripts->execute(impl.command); }
            ImGui::EndTabItem();
        }
        if (beginToolTab("State###ScriptState", Impl::ToolTab::State)) {
            ImGui::TextDisabled("Selected entity runtime state.");
            if (impl.scripts) {
                const auto text = impl.scripts->inspect(selected).dump(2);
                ImGui::BeginChild("Watch State", {0.0F, 124.0F}, docked ? ImGuiChildFlags_None : ImGuiChildFlags_Borders);
                ImGui::PushFont(codeFont(), kCodeFontSize);
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopFont();
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        if (beginToolTab("Errors###ScriptErrors", Impl::ToolTab::Errors)) {
            ImGui::TextDisabled("Latest recorded script error and traceback.");
            ImGui::BeginChild("Last Script Stack", {0.0F, 124.0F}, docked ? ImGuiChildFlags_None : ImGuiChildFlags_Borders);
            ImGui::PushFont(codeFont(), kCodeFontSize);
            bool found = false;
            if (impl.scripts) {
                for (auto it = impl.scripts->output().rbegin(); it != impl.scripts->output().rend(); ++it) {
                    if (it->kind == scripting::ScriptMessage::Kind::Error) {
                        ImGui::TextUnformatted(it->text.c_str()); found = true; break;
                    }
                }
            }
            if (!found) { ImGui::TextDisabled("No script error. Stack traces appear when a callback fails."); }
            ImGui::PopFont();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (beginToolTab("Script Activity###ScriptActivity", Impl::ToolTab::Activity)) {
            if (impl.scripts) {
                ImGui::Text("VM memory: %.2f MiB", static_cast<double>(impl.scripts->memoryUsed()) / (1024.0 * 1024.0));
                ImGui::Text("Reload: %s", impl.scripts->reloadPending() ? "compiling on JobSystem" : "idle");
                ImGui::TextWrapped("Game status: %s", impl.scripts->status().dump().c_str());
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::TextDisabled("%s", impl.status.c_str());
    const auto* document = impl.current();
    if (document) {
        const auto cursor = document->editor.GetCursorPosition();
        ImGui::SameLine(); ImGui::TextDisabled("Ln %d, Col %d | Luau", cursor.mLine + 1, cursor.mColumn + 1);
    }
    ImGui::End();
}
}
