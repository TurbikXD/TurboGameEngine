#include "engine/editor/EditorRibbon.h"
#include "engine/editor/EditorTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <span>
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace engine::editor {
namespace {
using Id = EditorCommandId;
using Tab = EditorRibbonTab;
constexpr Id kCreateChoices[]{Id::CreateEmpty, Id::CreateRenderCube, Id::CreatePhysicsCube,
    Id::CreatePhysicsSphere, Id::CreatePhysicsPyramid, Id::CreateCamera};
constexpr Id kPlayChoices[]{Id::Play, Id::PlayWindow};
constexpr Id kTools[]{Id::ToolSelect, Id::ToolMove, Id::ToolScale, Id::ToolRotate};
constexpr Id kInsert[]{Id::ScriptNew, Id::AddComponent};
constexpr Id kColor[]{Id::RandomizeTint, Id::ToggleVisible};
constexpr Id kPhysics[]{Id::ToggleStatic, Id::ToggleCollider};
constexpr Id kHomePanels[]{Id::ShowHierarchy, Id::ShowInspector, Id::ShowContent};
constexpr Id kTransform[]{Id::ResetTransform, Id::DropToGround, Id::ZeroVelocity};
constexpr Id kModelPhysics[]{Id::ToggleStatic, Id::ToggleGravity, Id::ToggleCollider, Id::AddComponent};
constexpr Id kSelection[]{Id::Duplicate, Id::Delete, Id::FocusSelected, Id::FrameScene};
constexpr Id kSimulation[]{Id::StepTick, Id::PlayWindow, Id::StopWindow};
constexpr Id kTestInspect[]{Id::ToggleColliderDebug, Id::ShowStats, Id::ScriptOutput, Id::ScriptState};
constexpr Id kDemo[]{Id::ScriptDemo};
constexpr Id kPanels[]{Id::ShowHierarchy, Id::ShowInspector, Id::ShowContent, Id::ShowStats, Id::ShowViewport, Id::ShowStatus};
constexpr Id kWorkspaces[]{Id::ShowScriptWorkspace, Id::ShowAssistant};
constexpr Id kDebugView[]{Id::ToggleColliderDebug, Id::ToggleHoverOutline};
constexpr Id kLayout[]{Id::ToggleLayoutLock, Id::ResetLayout, Id::SaveLayout};
constexpr Id kCamera[]{Id::BookmarkStore1, Id::BookmarkRecall1};
constexpr Id kProject[]{Id::ProjectSwitch, Id::SaveScene, Id::LoadScene, Id::Undo, Id::Redo};
constexpr Id kDocuments[]{Id::ScriptNew, Id::ScriptOpen, Id::ScriptSave, Id::ScriptSaveReload, Id::ScriptSaveAll, Id::ScriptReloadDisk};
constexpr Id kEditing[]{Id::ScriptFind, Id::ScriptGoToLine, Id::ScriptComment, Id::ScriptFormat, Id::ScriptCheck};
constexpr Id kScriptInspect[]{Id::ScriptOutput, Id::ScriptCommand, Id::ScriptState, Id::ScriptErrors};

enum class Kind { Commands, Create, Coordinates, Appearance };
struct Group { const char* label; std::span<const Id> commands; Kind kind{Kind::Commands}; };
constexpr Group kHome[]{
    {"Tools", kTools}, {"Coordinates and snap", {}, Kind::Coordinates}, {"Part", {}, Kind::Create},
    {"Insert", kInsert}, {"Appearance", kColor}, {"Physics", kPhysics}, {"Panels", kHomePanels},
    {"Selection", kSelection}, {"Project", kProject}};
constexpr Group kModel[]{
    {"Tools", kTools}, {"Coordinates and snap", {}, Kind::Coordinates}, {"Part", {}, Kind::Create},
    {"Transform", kTransform}, {"Appearance", kColor}, {"Physics", kModelPhysics}, {"Selection", kSelection}};
constexpr Group kTest[]{ {"Simulation", kSimulation}, {"Inspect", kTestInspect}, {"Examples", kDemo} };
constexpr Group kView[]{ {"Panels", kPanels}, {"Workspaces", kWorkspaces}, {"Diagnostics", kDebugView},
    {"Layout", kLayout}, {"Camera", kCamera}, {"Ribbon", {}, Kind::Appearance} };
constexpr Group kScript[]{ {"Documents", kDocuments}, {"Edit", kEditing}, {"Inspect", kScriptInspect}, {"Workspaces", kWorkspaces} };
constexpr std::size_t kMaxGroups = std::max({std::size(kHome), std::size(kModel), std::size(kTest), std::size(kView), std::size(kScript)});
constexpr ImU32 kBackground = IM_COL32(25, 26, 31, 255);
constexpr ImU32 kHover = IM_COL32(43, 45, 53, 255);
constexpr ImU32 kSelected = IM_COL32(50, 52, 60, 255);
constexpr ImU32 kBorder = IM_COL32(47, 49, 57, 255);
constexpr ImU32 kText = IM_COL32(197, 200, 207, 255);
constexpr ImU32 kMuted = IM_COL32(143, 147, 156, 255);

float uiScale() { return std::max(0.8F, ImGui::GetFontSize() / kEditorFontSize); }
float headerHeight() { return 36.0F * uiScale(); }
float rowHeight(const EditorRibbonState& state) { return (state.compact || !state.showLabels ? 44.0F : 58.0F) * uiScale(); }

std::span<const Group> groupsFor(const Tab tab) {
    switch (tab) { case Tab::Home: return kHome; case Tab::Model: return kModel; case Tab::Test: return kTest;
    case Tab::View: return kView; case Tab::Script: return kScript; }
    return kHome;
}
const char* tabName(const Tab tab) {
    switch (tab) { case Tab::Home: return "Home"; case Tab::Model: return "Model"; case Tab::Test: return "Test";
    case Tab::View: return "View"; case Tab::Script: return "Script"; }
    return "Home";
}
const char* caption(const Id id) {
    switch (id) {
    case Id::ShowHierarchy: return "Explorer"; case Id::ShowInspector: return "Properties"; case Id::ShowContent: return "Assets";
    case Id::ShowScriptWorkspace: case Id::ScriptNew: return "Script"; case Id::ShowStats: return "Stats";
    case Id::ToggleLocalSpace: return "Geometric"; case Id::ToggleSnap: return "Snap";
    case Id::RandomizeTint: return "Color"; case Id::ToggleVisible: return "Visible"; case Id::ToggleStatic: return "Anchor";
    case Id::ToggleGravity: return "Gravity"; case Id::ToggleCollider: return "Collision";
    case Id::AddComponent: return "Add"; case Id::FocusSelected: return "Focus"; case Id::FrameScene: return "Frame";
    case Id::ResetTransform: return "Reset"; case Id::DropToGround: return "Drop"; case Id::ZeroVelocity: return "Zero";
    case Id::ScriptSave: return "Save"; case Id::ScriptSaveReload: return "Save +"; case Id::ScriptSaveAll: return "Save all";
    case Id::ScriptReloadDisk: return "Reload"; case Id::ScriptOpen: return "Open"; case Id::ScriptCheck: return "Check";
    case Id::ScriptGoToLine: return "Go to line"; case Id::ScriptFormat: return "Format";
    case Id::ToggleColliderDebug: return "Bounds"; case Id::ToggleHoverOutline: return "Outline";
    case Id::ToggleLayoutLock: return "Lock"; case Id::ResetLayout: return "Reset"; case Id::SaveLayout: return "Save";
    case Id::BookmarkStore1: return "Store"; case Id::BookmarkRecall1: return "Recall";
    case Id::PlayWindow: return "Window"; case Id::StopWindow: return "Stop"; case Id::StepTick: return "Step";
    case Id::ScriptDemo: return "Demo"; case Id::ShowStatus: return "Status";
    default: { const auto* d = EditorCommandRegistry::definition(id); return d ? d->label.data() : ""; }
    }
}
void text(const std::string_view value) { ImGui::TextUnformatted(value.data(), value.data() + value.size()); }
void tooltip(const EditorCommandDefinition& definition, const CommandState& state) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { return; }
    ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0F);
    text(definition.label); if (!definition.shortcut.empty()) { ImGui::SameLine(); text(definition.shortcut); }
    text(definition.tooltip);
    if (!state.enabled && !state.disabledReason.empty()) { ImGui::Separator(); text(state.disabledReason); }
    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
}
constexpr float kCaptionScale = 13.0F / 15.0F;
float captionWidth(const char* label) { return ImGui::CalcTextSize(label).x * kCaptionScale; }
void centeredLabel(const char* label, const ImVec2 origin, const float width, const float y, const bool enabled) {
    const float fontSize = ImGui::GetFontSize() * kCaptionScale;
    const float x = origin.x + (width - captionWidth(label)) * 0.5F;
    const ImU32 color = enabled ? kText : kMuted;
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(origin.x, y), ImVec2(origin.x + width, y + fontSize + 2.0F * uiScale()), true);
    draw->AddText(ImGui::GetFont(), fontSize, ImVec2(x, y), color, label);
    draw->PopClipRect();
}
float tileWidth() {
    return 48.0F * uiScale();
}

bool commandTile(EditorCommandRegistry& commands, const Id id, const ImVec2 origin, const float width,
    const float height, const EditorRibbonState& ribbon, const EditorIcons::Resolver& resolve, const bool small = false,
    const char* overrideCaption = nullptr, const float visualWidth = 0.0F) {
    const auto* definition = EditorCommandRegistry::definition(id); if (!definition) { return false; }
    const auto state = commands.state(id); const float scale = uiScale();
    ImGui::PushID(static_cast<int>(id)); ImGui::SetCursorScreenPos(origin); ImGui::BeginDisabled(!state.enabled);
    const bool clicked = ImGui::InvisibleButton(definition->label.data(), ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered();
    const float box = (small ? 24.0F : 32.0F) * scale;
    const float layoutWidth = visualWidth > 0.0F ? visualWidth : width;
    const ImVec2 boxMin(origin.x + (layoutWidth - box) * .5F, origin.y + (small ? (height - box) * .5F : 3.0F * scale));
    if (state.checked || hovered || ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(boxMin, ImVec2(boxMin.x + box, boxMin.y + box), state.checked ? kSelected : kHover, 3.0F * scale);
    }
    const float iconSize = (small ? 16.0F : 24.0F) * scale;
    EditorIcons::draw(id, boxMin.x + (box - iconSize) * .5F, boxMin.y + (box - iconSize) * .5F, iconSize, resolve, state.enabled);
    if (!small && ribbon.showLabels && !ribbon.compact) {
        centeredLabel(overrideCaption ? overrideCaption : caption(id), origin, layoutWidth, origin.y + 39.0F * scale, state.enabled);
    }
    tooltip(*definition, state); ImGui::PopID();
    return clicked && commands.execute(id);
}

bool commandMenuItem(EditorCommandRegistry& commands, const Id id, const bool selected = false) {
    const auto* definition = EditorCommandRegistry::definition(id); if (!definition) { return false; }
    const auto state = commands.state(id); ImGui::PushID(static_cast<int>(id));
    const bool pressed = ImGui::MenuItem(definition->label.data(), definition->shortcut.data(), selected || state.checked, state.enabled);
    tooltip(*definition, state); ImGui::PopID(); return pressed && commands.execute(id);
}

bool createMenu(EditorCommandRegistry& commands, EditorRibbonState& state) {
    bool changed = false;
    for (const Id id : kCreateChoices) { if (commandMenuItem(commands, id, id == state.createCommand)) { changed = changed || state.createCommand != id; state.createCommand = id; } }
    return changed;
}

bool createTile(EditorCommandRegistry& commands, EditorRibbonState& state, const ImVec2 origin,
    const float height, const EditorIcons::Resolver& resolve) {
    const float scale = uiScale(), width = 48.0F * scale, arrowWidth = 12.0F * scale;
    commandTile(commands, state.createCommand, origin, width - arrowWidth, height, state, resolve, false, "Part", width);
    ImGui::PushID("CreateSplit"); ImGui::SetCursorScreenPos(ImVec2(origin.x + width - arrowWidth, origin.y + 3 * scale));
    if (ImGui::InvisibleButton("##Open", ImVec2(arrowWidth, 32 * scale), ImGuiButtonFlags_EnableNav)) { ImGui::OpenPopup("Choices"); }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Choose a part or entity to create"); }
    EditorIcons::draw(EditorIcons::Icon::ChevronDown, origin.x + width - arrowWidth, origin.y + 13 * scale, arrowWidth, resolve);
    bool changed = false; if (ImGui::BeginPopup("Choices")) { changed = createMenu(commands, state); ImGui::EndPopup(); }
    ImGui::PopID(); return changed;
}

bool modeDropdown(EditorCommandRegistry& commands, EditorRibbonState& state, const ImVec2 origin,
    const float width, const EditorIcons::Resolver& resolve) {
    const float scale = uiScale(), height = 24.0F * scale; ImGui::PushID("PlayMode"); ImGui::SetCursorScreenPos(origin);
    if (ImGui::InvisibleButton("##Mode", ImVec2(width, height), ImGuiButtonFlags_EnableNav)) { ImGui::OpenPopup("Modes"); }
    ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), ImGui::IsItemHovered() ? kSelected : kHover, 3 * scale);
    ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 8 * scale, origin.y + 4 * scale), kText, state.playCommand == Id::PlayWindow ? "Window" : "Test");
    EditorIcons::draw(EditorIcons::Icon::ChevronDown, origin.x + width - 20 * scale, origin.y + 4 * scale, 16 * scale, resolve);
    bool changed = false;
    if (ImGui::BeginPopup("Modes")) {
        for (const Id id : kPlayChoices) {
            const auto* definition = EditorCommandRegistry::definition(id); const auto availability = commands.state(id);
            if (ImGui::MenuItem(definition->label.data(), nullptr, state.playCommand == id, availability.enabled)) {
                changed = changed || state.playCommand != id; state.playCommand = id;
            }
            tooltip(*definition, availability);
        }
        ImGui::Separator(); commandMenuItem(commands, Id::StopWindow); ImGui::EndPopup();
    }
    ImGui::PopID(); return changed;
}

bool renderHeader(EditorCommandRegistry& commands, EditorRibbonState& state, const ImVec2 origin,
    const float available, const EditorIcons::Resolver& resolve) {
    const float scale = uiScale(), height = headerHeight(); bool changed = false;
    const float modeWidth = (available < 480 * scale ? 70.0F : 86.0F) * scale;
    changed = modeDropdown(commands, state, ImVec2(origin.x + 6 * scale, origin.y + 5 * scale), modeWidth, resolve);
    float x = origin.x + modeWidth + 12 * scale;
    ImGui::PushID("Transport");
    for (const Id id : {state.playCommand, Id::Pause, Id::Stop}) {
        commandTile(commands, id, ImVec2(x, origin.y), 28 * scale, height, state, resolve, true); x += 30 * scale;
    }
    if (available >= 780 * scale) {
        const Id secondary = commands.state(Id::StopWindow).enabled ? Id::StopWindow :
            (state.playCommand == Id::PlayWindow ? Id::Play : Id::PlayWindow);
        commandTile(commands, secondary, ImVec2(x, origin.y), 28 * scale, height, state, resolve, true);
    }
    ImGui::PopID();
    constexpr Tab tabs[]{Tab::Home, Tab::Model, Tab::Test, Tab::View, Tab::Script};
    const float tabsWidth = 5 * 78.0F * scale;
    if ((available - tabsWidth) * .5F > x - origin.x + 24 * scale) {
        float tabX = origin.x + (available - tabsWidth) * .5F;
        for (const Tab tab : tabs) {
            ImGui::PushID(static_cast<int>(tab)); ImGui::SetCursorScreenPos(ImVec2(tabX, origin.y + 5 * scale));
            const bool selected = state.tab == tab;
            if (ImGui::InvisibleButton("##Tab", ImVec2(76 * scale, 24 * scale), ImGuiButtonFlags_EnableNav) && !selected) { state.tab = tab; changed = true; }
            if (selected || ImGui::IsItemHovered()) { ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(tabX, origin.y + 5 * scale), ImVec2(tabX + 76 * scale, origin.y + 29 * scale), selected ? kSelected : kHover, 3 * scale); }
            const float textX = tabX + (76 * scale - ImGui::CalcTextSize(tabName(tab)).x) * .5F;
            ImGui::GetWindowDrawList()->AddText(ImVec2(textX, origin.y + 9 * scale), selected ? kText : kMuted, tabName(tab));
            ImGui::PopID(); tabX += 78 * scale;
        }
    } else {
        const float comboWidth = std::max(64 * scale, std::min(112 * scale, available - (x - origin.x) - 8 * scale));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + available - comboWidth - 4 * scale, origin.y + 6 * scale));
        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("##RibbonTabs", tabName(state.tab))) {
            for (const Tab tab : tabs) { if (ImGui::Selectable(tabName(tab), state.tab == tab) && state.tab != tab) { state.tab = tab; changed = true; } }
            ImGui::EndCombo();
        }
    }
    if (available >= 780 * scale) { commandTile(commands, Id::ShowAssistant, ImVec2(origin.x + available - 34 * scale, origin.y), 28 * scale, height, state, resolve, true); }
    ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x, origin.y + height - 1), ImVec2(origin.x + available, origin.y + height - 1), kBorder);
    return changed;
}

bool snapField(EditorCommandRegistry& commands, EditorRibbonState& state, const char* name, float& value,
    const EditorIcons::Icon icon, const ImVec2 origin, const float minimum, const float maximum, const char* format,
    const EditorIcons::Resolver& resolve) {
    const float scale = uiScale(); ImGui::PushID(name);
    bool snap = commands.state(Id::ToggleSnap).checked;
    ImGui::SetCursorScreenPos(origin); const auto availability = commands.state(Id::ToggleSnap);
    ImGui::BeginDisabled(!availability.enabled);
    if (ImGui::Checkbox("##Snap", &snap)) { commands.execute(Id::ToggleSnap); }
    ImGui::EndDisabled(); tooltip(*EditorCommandRegistry::definition(Id::ToggleSnap), availability);
    EditorIcons::draw(icon, origin.x + 21 * scale, origin.y + 1 * scale, 16 * scale, resolve);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 39 * scale, origin.y)); ImGui::SetNextItemWidth(72 * scale);
    const bool changed = ImGui::DragFloat("##Value", &value, minimum == 0.1F ? 0.1F : 0.01F, minimum, maximum, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID(); return EditorRibbon::sanitizeState(state) || changed;
}

bool coordinates(EditorCommandRegistry& commands, EditorRibbonState& state, const ImVec2 origin,
    const float height, const EditorIcons::Resolver& resolve) {
    const float scale = uiScale(); bool changed = false;
    commandTile(commands, Id::ToggleLocalSpace, origin, 48 * scale, height, state, resolve);
    const float y = origin.y + (state.tab == Tab::Model ? 1.0F : (height < 58 * scale ? 5.0F : 7.0F)) * scale;
    changed = snapField(commands, state, "Move", state.moveSnap, EditorIcons::Icon::MoveSnap, ImVec2(origin.x + 52 * scale, y), .001F, 1000.0F, "%.3g u", resolve) || changed;
    changed = snapField(commands, state, "Rotate", state.rotateSnap, EditorIcons::Icon::RotateSnap, ImVec2(origin.x + 52 * scale, y + 21 * scale), .1F, 180.0F, "%.3g deg", resolve) || changed;
    if (state.tab == Tab::Model && height >= 60 * scale) {
        changed = snapField(commands, state, "Scale", state.scaleSnap, EditorIcons::Icon::Coordinates, ImVec2(origin.x + 52 * scale, y + 42 * scale), .001F, 10.0F, "%.3g x", resolve) || changed;
    }
    return changed;
}

float groupWidth(const Group& group) {
    if (group.kind == Kind::Create) { return 48 * uiScale(); }
    if (group.kind == Kind::Coordinates) { return 164 * uiScale(); }
    if (group.kind == Kind::Appearance) { return 115 * uiScale(); }
    const float spacing = 4 * uiScale();
    return static_cast<float>(group.commands.size()) * tileWidth() +
        static_cast<float>(group.commands.empty() ? 0 : group.commands.size() - 1) * spacing;
}

bool renderGroup(EditorCommandRegistry& commands, EditorRibbonState& state, const Group& group,
    const ImVec2 origin, const float height, const EditorIcons::Resolver& resolve) {
    ImGui::PushID(group.label); bool changed = false;
    if (group.kind == Kind::Create) { changed = createTile(commands, state, origin, height, resolve); }
    else if (group.kind == Kind::Coordinates) { changed = coordinates(commands, state, origin, height, resolve); }
    else if (group.kind == Kind::Appearance) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 2 * uiScale(), origin.y + 6 * uiScale())); changed = ImGui::Checkbox("Compact", &state.compact);
        const float secondRow = height < 58 * uiScale() ? 24.0F : 29.0F;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 2 * uiScale(), origin.y + secondRow * uiScale())); changed = ImGui::Checkbox("Show labels", &state.showLabels) || changed;
    } else {
        float x = origin.x;
        for (const Id id : group.commands) { const float width = tileWidth(); commandTile(commands, id, ImVec2(x, origin.y), width, height, state, resolve); x += width + 4 * uiScale(); }
    }
    ImGui::PopID(); return changed;
}

bool overflowGroup(EditorCommandRegistry& commands, EditorRibbonState& state, const Group& group) {
    bool changed = false; ImGui::PushID(group.label); ImGui::SeparatorText(group.label);
    if (group.kind == Kind::Create) { changed = createMenu(commands, state); }
    else if (group.kind == Kind::Coordinates) {
        commandMenuItem(commands, Id::ToggleLocalSpace); commandMenuItem(commands, Id::ToggleSnap);
        ImGui::SetNextItemWidth(160); changed = ImGui::DragFloat("Move", &state.moveSnap, .01F, .001F, 1000, "%.3g", ImGuiSliderFlags_AlwaysClamp) || changed;
        ImGui::SetNextItemWidth(160); changed = ImGui::DragFloat("Rotate", &state.rotateSnap, .1F, .1F, 180, "%.3g deg", ImGuiSliderFlags_AlwaysClamp) || changed;
        ImGui::SetNextItemWidth(160); changed = ImGui::DragFloat("Scale", &state.scaleSnap, .01F, .001F, 10, "%.3g", ImGuiSliderFlags_AlwaysClamp) || changed;
    } else if (group.kind == Kind::Appearance) {
        changed = ImGui::Checkbox("Compact", &state.compact); changed = ImGui::Checkbox("Show labels", &state.showLabels) || changed;
    } else { for (const Id id : group.commands) { commandMenuItem(commands, id); } }
    ImGui::PopID(); return changed;
}

bool clampPreference(float& value, const float fallback, const float minimum, const float maximum) noexcept {
    const float normalized = std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
    const bool changed = value != normalized; value = normalized; return changed;
}
} // namespace

bool EditorRibbon::sanitizeState(EditorRibbonState& state) noexcept {
    bool changed = false;
    switch (state.tab) { case Tab::Home: case Tab::Model: case Tab::Test: case Tab::View: case Tab::Script: break;
    default: state.tab = Tab::Home; changed = true; break; }
    if (std::find(std::begin(kCreateChoices), std::end(kCreateChoices), state.createCommand) == std::end(kCreateChoices)) { state.createCommand = Id::CreatePhysicsCube; changed = true; }
    if (state.playCommand != Id::Play && state.playCommand != Id::PlayWindow) { state.playCommand = Id::Play; changed = true; }
    changed = clampPreference(state.moveSnap, .5F, .001F, 1000) || changed;
    changed = clampPreference(state.rotateSnap, 15, .1F, 180) || changed;
    changed = clampPreference(state.scaleSnap, .1F, .001F, 10) || changed;
    return changed;
}

float EditorRibbon::preferredHeight(const EditorRibbonState& state) {
    const float scale = ImGui::GetCurrentContext() ? uiScale() : 1.0F;
    // Three numeric fields on Model need the full tool-row height even when compact.
    const float tools = state.tab == Tab::Model ? 64.0F : (state.compact || !state.showLabels ? 44.0F : 58.0F);
    return (36 + tools + 1) * scale;
}

bool EditorRibbon::render(EditorCommandRegistry& commands, EditorRibbonState& state, const EditorIcons::Resolver& resolve) {
    bool changed = sanitizeState(state); ImGui::PushID("EditorRibbon");
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5 * uiScale(), 1 * uiScale()));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2 * uiScale());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3 * uiScale(), 2 * uiScale()));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.17F, .18F, .21F, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(.21F, .22F, .26F, 1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(.80F, .82F, .87F, 1));
    const ImVec2 origin = ImGui::GetCursorScreenPos(); const float available = std::max(1.0F, ImGui::GetContentRegionAvail().x);
    const float header = headerHeight();
    const float row = state.tab == Tab::Model ? 64 * uiScale() : rowHeight(state);
    ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + available, origin.y + header + row + 1 * uiScale()), kBackground);
    changed = renderHeader(commands, state, origin, available, resolve) || changed;
    const auto groups = groupsFor(state.tab); std::array<const Group*, kMaxGroups> overflow{}; std::size_t count = 0;
    const float gap = 4 * uiScale(), moreWidth = 36 * uiScale(); float used = 4 * uiScale(); bool first = true;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const float width = groupWidth(groups[i]), reserve = (i + 1 < groups.size() || count) ? moreWidth + gap : 4 * uiScale();
        if (used + (first ? 0 : gap) + width + reserve > available) { overflow[count++] = &groups[i]; continue; }
        if (!first) {
            ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x + used + gap * .5F, origin.y + header + 4 * uiScale()),
                ImVec2(origin.x + used + gap * .5F, origin.y + header + row - 4 * uiScale()), kBorder); used += gap;
        }
        changed = renderGroup(commands, state, groups[i], ImVec2(origin.x + used, origin.y + header + 1 * uiScale()), row, resolve) || changed;
        used += width; first = false;
    }
    if (count) {
        const float x = origin.x + std::min(used + (first ? 0 : gap), std::max(0.0F, available - moreWidth));
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y + header + 3 * uiScale()));
        if (ImGui::InvisibleButton("More...", ImVec2(std::min(moreWidth, available), 34 * uiScale()), ImGuiButtonFlags_EnableNav)) { ImGui::OpenPopup("Overflow"); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("More tools"); }
        EditorIcons::draw(EditorIcons::Icon::More, x + 6 * uiScale(), origin.y + header + 9 * uiScale(), 24 * uiScale(), resolve);
        if (ImGui::BeginPopup("Overflow")) {
            for (std::size_t i = 0; i < count; ++i) { changed = overflowGroup(commands, state, *overflow[i]) || changed; }
            ImGui::EndPopup();
        }
    }
    ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x, origin.y + header + row), ImVec2(origin.x + available, origin.y + header + row), kBorder);
    changed = sanitizeState(state) || changed;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header + row)); ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopStyleColor(3); ImGui::PopStyleVar(4); ImGui::PopID(); return changed;
}

} // namespace engine::editor
