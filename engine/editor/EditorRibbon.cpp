#include "engine/editor/EditorRibbon.h"
#include "engine/editor/EditorTheme.h"
#include "engine/editor/NetworkChartPng.h"
#include "engine/network/NetworkSession.h"
#include "engine/network/NetworkTestSummary.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui_internal.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <format>
#endif

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
constexpr Id kNetworkTest[]{Id::StartNetworkTest, Id::StopNetworkTest};
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

enum class Kind { Commands, Create, Coordinates, Appearance, NetworkTest, Multiplayer };
struct Group { const char* label; std::span<const Id> commands; Kind kind{Kind::Commands}; };
constexpr Group kHome[]{
    {"Tools", kTools}, {"Coordinates and snap", {}, Kind::Coordinates}, {"Part", {}, Kind::Create},
    {"Insert", kInsert}, {"Appearance", kColor}, {"Physics", kPhysics}, {"Panels", kHomePanels},
    {"Selection", kSelection}, {"Project", kProject}};
constexpr Group kModel[]{
    {"Tools", kTools}, {"Coordinates and snap", {}, Kind::Coordinates}, {"Part", {}, Kind::Create},
    {"Transform", kTransform}, {"Appearance", kColor}, {"Physics", kModelPhysics}, {"Selection", kSelection}};
constexpr Group kTest[]{ {"Simulation", kSimulation}, {"Multiplayer", {}, Kind::Multiplayer},
    {"Network Test", kNetworkTest, Kind::NetworkTest}, {"Inspect", kTestInspect}, {"Examples", kDemo} };
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

auto steamFriendNameMatcher(std::string_view query) {
#if defined(_WIN32)
    std::array<wchar_t, net::MaxSteamFriendNameBytes> wideQuery{};
    const auto queryLength = query.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        query.data(), static_cast<int>(query.size()), wideQuery.data(), static_cast<int>(wideQuery.size()));
    return [wideQuery, queryLength, empty = query.empty()](std::string_view name) {
        if (empty) return true;
        if (!queryLength) return false;
        std::array<wchar_t, net::MaxSteamFriendNameBytes> wideName{};
        const auto nameLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
            static_cast<int>(name.size()), wideName.data(), static_cast<int>(wideName.size()));
        return nameLength && FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | LINGUISTIC_IGNORECASE,
            wideName.data(), nameLength, wideQuery.data(), queryLength, nullptr, nullptr, nullptr, 0) >= 0;
    };
#else
    return [query](std::string_view name) {
        return query.empty() || ImStristr(name.data(), name.data() + name.size(),
            query.data(), query.data() + query.size()) != nullptr;
    };
#endif
}

struct FriendStatusText {
    const char* label;
    ImVec4 color;
};

FriendStatusText friendStatusText(net::SteamFriendPresence presence) {
    switch (presence) {
    case net::SteamFriendPresence::Online: return {"Online", {0.54F, 0.83F, 0.62F, 1.0F}};
    case net::SteamFriendPresence::Away: return {"Away", {0.91F, 0.75F, 0.44F, 1.0F}};
    case net::SteamFriendPresence::Busy: return {"Busy", {0.95F, 0.57F, 0.55F, 1.0F}};
    case net::SteamFriendPresence::Offline:
    default: return {"Offline", {0.66F, 0.70F, 0.76F, 1.0F}};
    }
}

void drawSteamFriend(std::string_view name, FriendStatusText status, const EditorIcons::Image& avatar,
    ImVec2 origin, float right, float height, float avatarSide) {
    auto* draw = ImGui::GetWindowDrawList();
    const float gap = 6.0F * uiScale();
    const float textTop = origin.y + (height - ImGui::GetTextLineHeight()) * 0.5F;
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const float statusWidth = ImGui::CalcTextSize(status.label).x;
    const float statusLeft = right - statusWidth;
    if (statusLeft < origin.x + avatarSide + 2 * gap) {
        const ImVec4 clip(origin.x, origin.y, right, origin.y + height);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(origin.x, textTop), textColor,
            name.data(), name.data() + name.size(), 0.0F, &clip);
        return;
    }

    const ImVec2 avatarMin(origin.x, origin.y + (height - avatarSide) * 0.5F);
    const ImVec2 avatarMax(avatarMin.x + avatarSide, avatarMin.y + avatarSide);
    const float rounding = avatarSide * 0.18F;
    if (avatar.textureId) {
        const auto texture = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(avatar.textureId));
        draw->AddImageRounded(texture, avatarMin, avatarMax, ImVec2(0, 0), ImVec2(1, 1),
            ImGui::GetColorU32(IM_COL32_WHITE), rounding);
    } else {
        draw->AddRectFilled(avatarMin, avatarMax, ImGui::GetColorU32(ImGuiCol_FrameBgActive), rounding);
        const float center = avatarMin.x + avatarSide * 0.5F;
        const float bodyHalfWidth = avatarSide * 0.28F;
        draw->AddCircleFilled(ImVec2(center, avatarMin.y + avatarSide * 0.34F), avatarSide * 0.14F, textColor);
        draw->AddRectFilled(ImVec2(center - bodyHalfWidth, avatarMin.y + avatarSide * 0.54F),
            ImVec2(center + bodyHalfWidth, avatarMin.y + avatarSide * 0.81F), textColor, avatarSide * 0.12F);
    }

    const float nameLeft = avatarMax.x + gap;
    const ImVec4 nameClip(nameLeft, origin.y, statusLeft - gap, origin.y + height);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(nameLeft, textTop), textColor,
        name.data(), name.data() + name.size(), 0.0F, &nameClip);
    draw->AddText(ImVec2(statusLeft, textTop), ImGui::GetColorU32(status.color), status.label);
}

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
    case Id::StartNetworkTest: return "Start Net"; case Id::StopNetworkTest: return "Stop Net";
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
    const float height, const EditorRibbonState& ribbon, const EditorIcons::Resolver& resolve, const bool compactTile = false,
    const char* overrideCaption = nullptr, const float visualWidth = 0.0F) {
    const auto* definition = EditorCommandRegistry::definition(id); if (!definition) { return false; }
    const auto state = commands.state(id); const float scale = uiScale();
    ImGui::PushID(static_cast<int>(id)); ImGui::SetCursorScreenPos(origin); ImGui::BeginDisabled(!state.enabled);
    const bool clicked = ImGui::InvisibleButton(definition->label.data(), ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered();
    const float box = (compactTile ? 24.0F : 32.0F) * scale;
    const float layoutWidth = visualWidth > 0.0F ? visualWidth : width;
    const ImVec2 boxMin(origin.x + (layoutWidth - box) * .5F, origin.y + (compactTile ? (height - box) * .5F : 3.0F * scale));
    if (state.checked || hovered || ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(boxMin, ImVec2(boxMin.x + box, boxMin.y + box), state.checked ? kSelected : kHover, 3.0F * scale);
    }
    const float iconSize = (compactTile ? 16.0F : 24.0F) * scale;
    EditorIcons::draw(id, boxMin.x + (box - iconSize) * .5F, boxMin.y + (box - iconSize) * .5F, iconSize, resolve, state.enabled);
    if (!compactTile && ribbon.showLabels && !ribbon.compact) {
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

constexpr float kNetworkOptionsWidth = 76.0F;
constexpr float kMultiplayerButtonWidth = 100.0F;

struct NetworkOptionsEditor {
    bool live;
    std::function<bool()> draw;
    bool& statsOpen;
    bool multiplayerAvailable;
    bool& multiplayerOpen;
};

bool networkConditionFields(int& latencyMs, int& lossPercent) {
    const float width = 118.0F * uiScale();
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::SliderInt("One-way latency", &latencyMs, 0, net::MaxNetworkTestLatencyMs,
        "%d ms", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(width);
    changed = ImGui::SliderInt("Packet loss", &lossPercent, 0, net::MaxNetworkTestLossPercent,
        "%d%%", ImGuiSliderFlags_AlwaysClamp) || changed;
    return changed;
}

bool networkOptionsFields(net::NetworkTestOptions& options, const int maxClients) {
    ImGui::SetNextItemWidth(118.0F * uiScale());
    bool changed = ImGui::SliderInt("Clients", &options.clientCount, 1, maxClients,
        "%d", ImGuiSliderFlags_AlwaysClamp);
    changed = networkConditionFields(options.latencyMs, options.lossPercent) || changed;
    ImGui::BeginDisabled(options.conditions() == net::NetworkTestConditions{});
    const bool cleared = ImGui::Button("Clear Emulation");
    ImGui::EndDisabled();
    if (cleared) {
        options.latencyMs = 0;
        options.lossPercent = 0;
    }
    return cleared || changed;
}

const char* linkStateLabel(net::NetworkTestLinkState state) {
    using Link = net::NetworkTestLinkState;
    switch (state) {
    case Link::Starting: return "Starting";
    case Link::Connecting: return "Connecting";
    case Link::Connected: return "Connected";
    case Link::TimedOut: return "Timed out";
    case Link::Disconnected: return "Disconnected";
    case Link::Rejected: return "Rejected";
    case Link::Closed: return "Closed";
    }
    return "Unknown";
}

void lossMetric(const std::optional<double>& percent) {
    if (percent) ImGui::Text("%.1f%%", *percent);
    else ImGui::TextDisabled("--");
}

void trafficMetric(const std::optional<net::NetworkTestTrafficRate>& rate) {
    if (!rate) ImGui::TextDisabled("--");
    else if (rate->bytesPerSecond >= 1024.0) {
        ImGui::Text("%.1f KiB/s | %.0f pkt/s", rate->bytesPerSecond / 1024.0, rate->packetsPerSecond);
    } else {
        ImGui::Text("%.0f B/s | %.0f pkt/s", rate->bytesPerSecond, rate->packetsPerSecond);
    }
}

struct HistorySeries {
    const char* label;
    ImU32 color;
    ImU32 comparisonColor;
    std::optional<double> (*read)(const net::NetworkTestMetrics&);
};

constexpr ImU32 kUplinkColor = IM_COL32(90, 208, 197, 255);
constexpr ImU32 kDownlinkColor = IM_COL32(250, 173, 94, 255);
constexpr ImU32 kUplinkComparisonColor = IM_COL32(176, 242, 235, 255);
constexpr ImU32 kDownlinkComparisonColor = IM_COL32(255, 221, 172, 255);
constexpr HistorySeries kRttSeries[]{
    {"RTT", IM_COL32(121, 175, 255, 255), IM_COL32(188, 215, 255, 255),
        [](const auto& metrics) -> std::optional<double> { return metrics.rttMs; }}
};
constexpr HistorySeries kLossSeries[]{
    {"Up", kUplinkColor, kUplinkComparisonColor, [](const auto& metrics) { return metrics.uplinkLossPercent; }},
    {"Down", kDownlinkColor, kDownlinkComparisonColor, [](const auto& metrics) { return metrics.downlinkLossPercent; }},
    {"Probe", IM_COL32(232, 137, 194, 255), IM_COL32(255, 201, 232, 255),
        [](const auto& metrics) { return metrics.probeLossPercent; }}
};
constexpr HistorySeries kTrafficSeries[]{
    {"Up", kUplinkColor, kUplinkComparisonColor, [](const auto& metrics) -> std::optional<double> {
        if (metrics.upload) return metrics.upload->bytesPerSecond / 1024.0;
        return std::nullopt;
    }},
    {"Down", kDownlinkColor, kDownlinkComparisonColor, [](const auto& metrics) -> std::optional<double> {
        if (metrics.download) return metrics.download->bytesPerSecond / 1024.0;
        return std::nullopt;
    }}
};

struct HistoryChart {
    const char* title;
    const char* valueFormat;
    std::span<const HistorySeries> series;
    double minimumCeiling;
    bool fixedCeiling{};
};

constexpr HistoryChart kHistoryCharts[]{
    {"RTT (ms)", "%.0f ms", kRttSeries, 50.0},
    {"Loss (%)", "%.1f%%", kLossSeries, 100.0, true},
    {"UDP traffic (KiB/s)", "%.2f KiB/s", kTrafficSeries, 1.0}
};

struct HistoryAxis {
    std::uint64_t first;
    std::uint64_t last;
};

struct HistoryPlot {
    ImVec2 origin;
    ImVec2 min;
    ImVec2 max;
    bool visible;
    bool hovered;
};

using VisibleHistory = std::ranges::subrange<std::deque<net::NetworkTestHistoryPoint>::const_iterator>;

VisibleHistory visibleHistory(const net::NetworkTestHistory* history, HistoryAxis axis) {
    static const std::deque<net::NetworkTestHistoryPoint> empty;
    const auto& samples = history ? history->points() : empty;
    return {
        std::ranges::lower_bound(samples, axis.first, {}, &net::NetworkTestHistoryPoint::elapsedMs),
        std::ranges::upper_bound(samples, axis.last, {}, &net::NetworkTestHistoryPoint::elapsedMs)
    };
}

const net::NetworkTestHistory* clientHistory(const core::NetworkTestCsvRun& run, unsigned index) {
    const auto found = run.clients.find(index);
    return found == run.clients.end() ? nullptr : &found->second;
}

std::optional<double> seriesValue(const HistorySeries& series, const net::NetworkTestMetrics& metrics) {
    const auto value = series.read(metrics);
    return value && std::isfinite(*value) && *value >= 0.0 ? value : std::nullopt;
}

double chartCeiling(const HistoryChart& chart, std::span<const VisibleHistory> runs) {
    double maximum = chart.minimumCeiling;
    if (chart.fixedCeiling) return maximum;
    for (const auto& points : runs) {
        for (const auto& point : points) {
            for (const auto& series : chart.series) {
                if (const auto value = seriesValue(series, point.metrics)) maximum = std::max(maximum, *value);
            }
        }
    }
    const auto power = std::pow(10.0, std::floor(std::log10(maximum)));
    const auto normalized = maximum / power;
    const auto ceiling = power * (normalized <= 1.0 ? 1.0 : normalized <= 2.0 ? 2.0 : normalized <= 5.0 ? 5.0 : 10.0);
    return std::isfinite(ceiling) ? ceiling : maximum;
}

void formatElapsed(char (&label)[32], std::uint64_t milliseconds, const bool fractional = false) {
    const auto minutes = static_cast<unsigned long long>(milliseconds / 60'000);
    const auto seconds = static_cast<unsigned long long>(milliseconds / 1'000 % 60);
    if (fractional) {
        std::snprintf(label, sizeof(label), "%llu:%02llu.%llu", minutes, seconds,
            static_cast<unsigned long long>(milliseconds / 100 % 10));
    } else {
        std::snprintf(label, sizeof(label), "%llu:%02llu", minutes, seconds);
    }
}

HistoryPlot historyPlot(const HistoryChart& chart, const bool timeLabels) {
    ImGui::TextUnformatted(chart.title);
    for (const auto& series : chart.series) {
        if (chart.series.size() == 1) break;
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(series.color), "%s", series.label);
    }
    const float scale = uiScale();
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0F, ImGui::GetContentRegionAvail().x);
    const float height = 76.0F * scale;
    const ImVec2 min(origin.x + std::min(44.0F * scale, width * .33F), origin.y);
    const ImVec2 max(origin.x + width, origin.y + height);
    ImGui::Dummy(ImVec2(width, height + (timeLabels ? ImGui::GetTextLineHeightWithSpacing() : 0.0F)));
    return {origin, min, max, ImGui::IsItemVisible(), ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(min, max)};
}

float historyX(const HistoryPlot& plot, HistoryAxis axis, std::uint64_t milliseconds) {
    const auto fraction = static_cast<float>(static_cast<double>(milliseconds - axis.first) /
        static_cast<double>(axis.last - axis.first));
    return std::lerp(plot.min.x, plot.max.x, fraction);
}

template<typename Draw>
void drawDashedHistory(Draw& draw, std::span<const ImVec2> points, const float originX, const ImU32 color, const float scale) {
    const float dash = 5.0F * scale;
    const float period = 9.0F * scale;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto& a = points[i - 1];
        const auto& b = points[i];
        const float start = originX + std::floor((a.x - originX) / period) * period;
        if (b.x == a.x) {
            if (a.x <= start + dash) draw.AddLine(a, b, color, 1.8F * scale);
            continue;
        }
        for (float d = start; d < b.x; d += period) {
            const float first = std::max(a.x, d);
            const float last = std::min(b.x, d + dash);
            if (first >= last) continue;
            const float width = b.x - a.x;
            draw.AddLine(ImVec2(first, std::lerp(a.y, b.y, (first - a.x) / width)),
                ImVec2(last, std::lerp(a.y, b.y, (last - a.x) / width)), color, 1.8F * scale);
        }
    }
}

template<typename Draw>
void drawHistorySeries(Draw& draw, const HistoryChart& chart, const HistoryPlot& plot, HistoryAxis axis,
    const VisibleHistory& points, const double ceiling, const float scale, const bool dashed) {
    std::vector<ImVec2> segment;
    segment.reserve(points.size());
    for (const auto& series : chart.series) {
        const auto flush = [&] {
            if (segment.size() > 1) {
                if (dashed) drawDashedHistory(draw, segment, plot.min.x, series.comparisonColor, scale);
                else draw.AddPolyline(segment.data(), static_cast<int>(segment.size()), series.color, 0, 1.8F * scale);
            } else if (segment.size() == 1) {
                if (dashed) draw.AddCircle(segment.front(), 3.2F * scale, series.comparisonColor, 8);
                else draw.AddCircleFilled(segment.front(), 2.4F * scale, series.color, 8);
            }
            segment.clear();
        };
        for (const auto& point : points) {
            const auto value = seriesValue(series, point.metrics);
            if (point.breakBefore || !value) flush();
            if (!value) continue;
            const auto fraction = static_cast<float>(std::clamp(*value / ceiling, 0.0, 1.0));
            segment.emplace_back(historyX(plot, axis, point.elapsedMs), std::lerp(plot.max.y, plot.min.y, fraction));
        }
        flush();
    }
}

struct HistoryPlotStyle {
    float scale;
    float lineHeight;
    ImU32 background;
};

template<typename Draw, typename TextWidth>
void drawHistoryPlot(Draw& draw, const HistoryChart& chart, const HistoryPlot& plot, HistoryAxis axis,
    std::span<const VisibleHistory> runs, std::optional<std::uint64_t> cursorTime, const bool timeLabels,
    const HistoryPlotStyle& style, TextWidth textWidth) {
    if (!plot.visible) return;
    const auto ceiling = chartCeiling(chart, runs);
    const float scale = style.scale;
    const float lineHeight = style.lineHeight;
    draw.AddRectFilled(plot.min, plot.max, style.background);
    const unsigned divisions = plot.max.x - plot.min.x >= 260.0F * scale ? 4 : 2;
    const auto duration = axis.last - axis.first;
    for (unsigned tick = 0; tick <= divisions; ++tick) {
        const auto time = axis.first + (duration / divisions) * tick + (duration % divisions) * tick / divisions;
        const float x = historyX(plot, axis, time);
        draw.AddLine(ImVec2(x, plot.min.y), ImVec2(x, plot.max.y), kBorder);
        if (timeLabels) {
            char label[32]{};
            formatElapsed(label, time, duration < 10'000);
            const float width = textWidth(label);
            const float labelX = std::max(plot.min.x, std::min(x - width * .5F, plot.max.x - width));
            draw.AddText(ImVec2(labelX, plot.max.y + 2.0F * scale), kMuted, label);
        }
    }
    for (int tick = 0; tick <= 2; ++tick) {
        const float fraction = static_cast<float>(tick) / 2.0F;
        const float y = std::lerp(plot.max.y, plot.min.y, fraction);
        draw.AddLine(ImVec2(plot.min.x, y), ImVec2(plot.max.x, y), kBorder);
        char label[32]{};
        std::snprintf(label, sizeof(label), ceiling > 10'000.0 ? "%.1g" : (ceiling < 10.0 ? "%.1f" : "%.0f"), ceiling * fraction);
        const float labelY = std::clamp(y - lineHeight * .5F, plot.min.y, plot.max.y - lineHeight);
        draw.AddText(ImVec2(std::max(plot.origin.x, plot.min.x - textWidth(label) - 5.0F * scale), labelY),
            kMuted, label);
    }
    draw.PushClipRect(plot.min, plot.max, true);
    for (std::size_t i = 0; i < runs.size(); ++i) drawHistorySeries(draw, chart, plot, axis, runs[i], ceiling, scale, i == 1);
    if (cursorTime) {
        const float x = historyX(plot, axis, *cursorTime);
        draw.AddLine(ImVec2(x, plot.min.y), ImVec2(x, plot.max.y), kText);
    }
    draw.PopClipRect();
}

const net::NetworkTestHistoryPoint* nearestHistoryPoint(const VisibleHistory& points, const std::uint64_t time) {
    auto nearest = std::ranges::lower_bound(points, time, {}, &net::NetworkTestHistoryPoint::elapsedMs);
    if (nearest != points.begin() && (nearest == points.end() ||
        time - std::prev(nearest)->elapsedMs <= nearest->elapsedMs - time)) --nearest;
    if (nearest == points.end()) return nullptr;
    const auto distance = std::max(time, nearest->elapsedMs) - std::min(time, nearest->elapsedMs);
    return distance <= 750 ? &*nearest : nullptr;
}

constexpr const char* kRunLabels[]{"Run A", "Run B"};

void historyCharts(const HistoryAxis axis, std::span<const VisibleHistory> runs) {
    const auto duration = axis.last - axis.first;
    std::array<HistoryPlot, std::size(kHistoryCharts)> plots{};
    std::array<const net::NetworkTestHistoryPoint*, std::size(kRunLabels)> hoveredSamples{};
    std::optional<std::uint64_t> cursorTime;
    std::size_t hoveredChart{};
    for (std::size_t i = 0; i < plots.size(); ++i) {
        auto& plot = plots[i];
        plot = historyPlot(kHistoryCharts[i], i + 1 == plots.size());
        if (!plot.hovered) continue;
        hoveredChart = i;
        const auto fraction = std::clamp((ImGui::GetMousePos().x - plot.min.x) / (plot.max.x - plot.min.x), 0.0F, 1.0F);
        const auto offset = static_cast<double>(fraction) * static_cast<double>(duration);
        cursorTime = axis.first + (offset >= static_cast<double>(duration) ? duration : static_cast<std::uint64_t>(offset));
        for (std::size_t run = 0; run < runs.size(); ++run) hoveredSamples[run] = nearestHistoryPoint(runs[run], *cursorTime);
        if (runs.size() == 1 && hoveredSamples[0]) cursorTime = hoveredSamples[0]->elapsedMs;
    }
    auto& draw = *ImGui::GetWindowDrawList();
    const HistoryPlotStyle style{uiScale(), ImGui::GetTextLineHeight(), ImGui::GetColorU32(ImGuiCol_FrameBg)};
    for (std::size_t i = 0; i < plots.size(); ++i) {
        drawHistoryPlot(draw, kHistoryCharts[i], plots[i], axis, runs, cursorTime, i + 1 == plots.size(), style,
            [](const char* label) { return ImGui::CalcTextSize(label).x; });
    }
    ImGui::TextDisabled("Elapsed test time");
    if (cursorTime && ImGui::BeginTooltip()) {
        char label[32]{};
        formatElapsed(label, *cursorTime, true);
        ImGui::Text("Elapsed %s", label);
        const auto& chart = kHistoryCharts[hoveredChart];
        for (std::size_t run = 0; run < runs.size(); ++run) {
            const auto* sample = hoveredSamples[run];
            if (runs.size() > 1) {
                ImGui::Separator();
                if (sample) {
                    formatElapsed(label, sample->elapsedMs, true);
                    ImGui::Text("%s at %s", kRunLabels[run], label);
                } else {
                    ImGui::TextDisabled("%s: No sample near cursor", kRunLabels[run]);
                    continue;
                }
            }
            for (const auto& series : chart.series) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(run == 0 ? series.color : series.comparisonColor), "%s", series.label);
                ImGui::SameLine();
                const auto value = sample ? seriesValue(series, sample->metrics) : std::nullopt;
                if (value) ImGui::Text(chart.valueFormat, *value);
                else ImGui::TextDisabled("--");
            }
        }
        ImGui::EndTooltip();
    }
}

void networkHistoryPanel(const NetworkTestRibbonActions& actions, std::span<const net::NetworkTestClientView> clients,
    unsigned& clientIndex, std::size_t& rangeIndex) {
    auto selected = std::ranges::find(clients, clientIndex, &net::NetworkTestClientView::index);
    if (selected == clients.end()) selected = clients.begin();
    clientIndex = selected->index;
    const auto preview = "Client " + std::to_string(clientIndex);
    const float available = ImGui::GetContentRegionAvail().x;
    const float scale = uiScale();
    ImGui::SetNextItemWidth(114.0F * scale);
    if (ImGui::BeginCombo("Client", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (const auto& client : clients) {
            const auto name = "Client " + std::to_string(client.index);
            if (ImGui::Selectable(name.c_str(), client.index == clientIndex)) clientIndex = client.index;
        }
        ImGui::EndCombo();
    }
    struct Range { const char* label; std::uint64_t milliseconds; };
    constexpr Range ranges[]{{"30 s", 30'000}, {"2 min", 120'000}, {"5 min", net::NetworkTestHistoryWindowMs}};
    if (available >= 290.0F * scale) ImGui::SameLine();
    ImGui::SetNextItemWidth(85.0F * scale);
    if (ImGui::BeginCombo("Range", ranges[rangeIndex].label)) {
        for (std::size_t i = 0; i < std::size(ranges); ++i) {
            if (ImGui::Selectable(ranges[i].label, i == rangeIndex)) rangeIndex = i;
        }
        ImGui::EndCombo();
    }
    const auto history = actions.history(clientIndex);
    if (!history.history || history.history->points().empty()) {
        ImGui::TextDisabled("Waiting for history...");
        return;
    }
    const auto duration = ranges[rangeIndex].milliseconds;
    const auto last = std::max(duration, history.elapsedMs);
    const HistoryAxis axis{last - duration, last};
    const auto points = visibleHistory(history.history, axis);
    if (points.empty()) ImGui::TextDisabled("No samples in this range.");
    historyCharts(axis, std::span(&points, 1));
}

void comparisonSummaryValue(std::optional<double> value, bool delta, const char* unit) {
    if (!value) {
        ImGui::TextDisabled("--");
        return;
    }
    const auto shown = std::abs(*value) < 0.005 ? 0.0 : *value;
    const bool signedValue = delta && shown != 0.0;
    const char* format = std::abs(shown) >= 1e6 ? (signedValue ? "%+.3g%s" : "%.3g%s")
                                             : (signedValue ? "%+.2f%s" : "%.2f%s");
    ImGui::Text(format, shown, unit);
}

void networkComparisonSummary(const net::NetworkTestSummary& first, const net::NetworkTestSummary& second) {
    const bool open = ImGui::CollapsingHeader("Summary", ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Means use available samples in the selected range.\n"
            "p95 RTT uses the nearest-rank percentile.\n"
            "Delta is Run B minus Run A. Loss deltas are in percentage points (pp).");
        ImGui::EndTooltip();
    }
    if (!open || !ImGui::BeginTable("Comparison summary", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) return;

    ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.8F);
    ImGui::TableSetupColumn(kRunLabels[0]);
    ImGui::TableSetupColumn(kRunLabels[1]);
    ImGui::TableSetupColumn("B - A");
    ImGui::TableHeadersRow();

    for (const auto& metric : net::NetworkTestSummaryMetrics) {
        const auto values = metric.values(first, second);
        const bool isLoss = std::string_view(metric.deltaUnit) == "pp";
        ImGui::TableNextRow();
        if (ImGui::TableNextColumn()) ImGui::TextUnformatted(metric.label);
        if (ImGui::TableNextColumn()) comparisonSummaryValue(values.runA, false, isLoss ? "%" : "");
        if (ImGui::TableNextColumn()) comparisonSummaryValue(values.runB, false, isLoss ? "%" : "");
        if (ImGui::TableNextColumn()) comparisonSummaryValue(values.delta, true, isLoss ? " pp" : "");
    }
    ImGui::EndTable();
}

constexpr int kComparisonChartWidth = 1440;
constexpr int kComparisonChartHeight = 960;

void drawComparisonCharts(NetworkChartPng& canvas, unsigned clientIndex, const char* range,
    HistoryAxis bounds, HistoryAxis axis, const std::array<VisibleHistory, 2>& runs,
    const std::array<std::string_view, 2>& filenames) {
    constexpr float width = kComparisonChartWidth;
    constexpr float height = kComparisonChartHeight;
    constexpr float left = 38.0F;
    constexpr float right = width - left;
    constexpr ImU32 heading = IM_COL32(240, 242, 248, 255);
    constexpr ImU32 secondary = IM_COL32(176, 181, 193, 255);
    canvas.AddRectFilled({0, 0}, {width, height}, kBackground);
    canvas.setTextSize(32.0F);
    canvas.AddText({left, 28.0F}, heading, "Network Test Comparison");
    canvas.setTextSize(20.0F);
    char first[32]{}, last[32]{}, selection[160]{};
    formatElapsed(first, bounds.first, true);
    formatElapsed(last, bounds.last, true);
    std::snprintf(selection, sizeof(selection), "Client %u  |  %s  |  %s - %s elapsed", clientIndex, range, first, last);
    canvas.AddText({left, 76.0F}, secondary, selection);

    for (std::size_t i = 0; i < runs.size(); ++i) {
        const float y = 114.0F + static_cast<float>(i) * 30.0F;
        const auto& legend = kRttSeries[0];
        const auto color = i == 0 ? legend.color : legend.comparisonColor;
        if (i == 0) canvas.AddLine({left, y + 13.0F}, {left + 40.0F, y + 13.0F}, color, 3.0F);
        else {
            for (float x = left; x < left + 40.0F; x += 14.0F) canvas.AddLine({x, y + 13.0F}, {x + 8.0F, y + 13.0F}, color, 3.0F);
        }
        std::string label = std::string(kRunLabels[i]) + "  |  " + std::string(filenames[i]);
        if (runs[i].empty()) label += "  |  No samples in this range";
        canvas.PushClipRect({left, y}, {right, y + 30.0F}, true);
        canvas.AddText({left + 53.0F, y}, secondary, label.c_str());
        canvas.PopClipRect();
    }

    for (std::size_t i = 0; i < std::size(kHistoryCharts); ++i) {
        const auto& chart = kHistoryCharts[i];
        const float y = 188.0F + static_cast<float>(i) * 242.0F;
        canvas.AddRectFilled({left, y}, {right, y + 226.0F}, IM_COL32(29, 30, 36, 255));
        canvas.setTextSize(22.0F);
        canvas.AddText({left + 18.0F, y + 10.0F}, heading, chart.title);
        float legendX = left + 46.0F + canvas.textWidth(chart.title);
        if (chart.series.size() > 1) {
            canvas.setTextSize(17.0F);
            for (const auto& series : chart.series) {
                canvas.AddText({legendX, y + 15.0F}, series.color, series.label);
                legendX += canvas.textWidth(series.label) + 24.0F;
            }
        }
        canvas.setTextSize(17.0F);
        const HistoryPlot plot{{left + 15.0F, y + 53.0F}, {left + 74.0F, y + 53.0F},
            {right - 20.0F, y + 194.0F}, true, false};
        const HistoryPlotStyle style{1.65F, canvas.textHeight(), IM_COL32(36, 38, 46, 255)};
        drawHistoryPlot(canvas, chart, plot, axis, runs, std::nullopt, true, style,
            [&canvas](const char* label) { return canvas.textWidth(label); });
    }
    canvas.AddText({left, 918.0F}, secondary, "Elapsed test time  |  Up: client to server   Down: server to client");
}

struct NetworkReportPath {
    std::filesystem::path path;
    bool unavailable{};
};

enum class NetworkReportDialog { Open, History, Summary, Charts, Report, AllReports, Archive };

bool includesAllClients(NetworkReportDialog kind) {
    return kind == NetworkReportDialog::AllReports || kind == NetworkReportDialog::Archive;
}

NetworkReportPath pickNetworkReportPath(const NetworkReportDialog kind, const std::filesystem::path& initialFolder = {},
    const unsigned clientIndex = 0, const std::filesystem::path& suggestedStem = {}) {
#if defined(_WIN32)
    const bool save = kind != NetworkReportDialog::Open;
    const bool archive = kind == NetworkReportDialog::Archive;
    const bool allClients = includesAllClients(kind);
    const bool report = allClients || kind == NetworkReportDialog::Report;
    const bool png = !archive && (report || kind == NetworkReportDialog::Charts);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return {{}, true};
    struct Apartment final { ~Apartment() { CoUninitialize(); } } apartment;
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.GetAddressOf()))) || !dialog) return {{}, true};

    DWORD options{};
    const COMDLG_FILTERSPEC filter[]{archive ? COMDLG_FILTERSPEC{L"ZIP archives (*.zip)", L"*.zip"}
        : png ? COMDLG_FILTERSPEC{allClients ? L"PNG charts and CSV summaries"
        : report ? L"PNG charts and CSV summary" : L"PNG images (*.png)", L"*.png"}
        : COMDLG_FILTERSPEC{L"CSV files (*.csv)", L"*.csv"}};
    const auto* extension = archive ? L"zip" : png ? L"png" : L"csv";
    const auto* title = archive ? L"Export All Client Reports as ZIP" : allClients ? L"Export All Client Reports"
        : report ? L"Export Comparison Report" : png ? L"Export Comparison Charts"
        : kind == NetworkReportDialog::Summary ? L"Export Comparison Summary"
        : save ? L"Export Network Test History" : L"Open Network Test CSV";
    if (FAILED(dialog->GetOptions(&options))) return {{}, true};
    if (report) options &= ~static_cast<DWORD>(FOS_OVERWRITEPROMPT);
    const DWORD actionOptions = save ? (report ? 0 : FOS_OVERWRITEPROMPT) : FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT;
    if (FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | actionOptions |
        (png || archive ? FOS_STRICTFILETYPES : 0))) ||
        FAILED(dialog->SetFileTypes(1, filter)) || FAILED(dialog->SetDefaultExtension(extension)) ||
        FAILED(dialog->SetTitle(title)) || (report && FAILED(dialog->SetFileNameLabel(archive ? L"Archive name:" : L"Report name:")))) {
        return {{}, true};
    }
    if (save) {
        std::wstring filename;
        if (!suggestedStem.empty()) filename = std::format(L"{}.{}", suggestedStem.wstring(), extension);
        else {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const auto prefix = allClients ? std::wstring(L"network-comparison")
                : clientIndex ? std::format(L"network-comparison-client-{}", clientIndex) : std::wstring(L"network-test");
            filename = std::format(L"{}-{:04}-{:02}-{:02}_{:02}-{:02}-{:02}",
                prefix, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
            if (report) filename += std::format(L"-{:03}", now.wMilliseconds);
            filename += L'.';
            filename += extension;
        }
        if (FAILED(dialog->SetFileName(filename.c_str()))) return {{}, true};
    }
    if (!initialFolder.empty()) {
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder.c_str(), nullptr, IID_PPV_ARGS(folder.GetAddressOf()))) && folder) {
            dialog->SetFolder(folder.Get());
        }
    }

    const auto shown = dialog->Show(GetActiveWindow());
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(shown)) return {{}, true};
    Microsoft::WRL::ComPtr<IShellItem> selected;
    if (FAILED(dialog->GetResult(selected.GetAddressOf())) || !selected) return {{}, true};
    PWSTR rawPath{};
    const auto resolved = selected->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, &CoTaskMemFree);
    if (FAILED(resolved) || !path) return {{}, true};
    return {std::filesystem::path(path.get()), false};
#else
    (void)kind;
    (void)initialFolder;
    (void)clientIndex;
    (void)suggestedStem;
    return {{}, true};
#endif
}

}

void EditorRibbon::renderNetworkComparisonHistory(const core::NetworkTestCsvRun& runA, const core::NetworkTestCsvRun& runB) {
    const std::array histories{clientHistory(runA, m_comparisonClientIndex), clientHistory(runB, m_comparisonClientIndex)};
    std::optional<HistoryAxis> bounds;
    for (const auto* history : histories) {
        if (!history || history->points().empty()) continue;
        const auto first = history->points().front().elapsedMs;
        const auto last = history->points().back().elapsedMs;
        bounds = bounds ? HistoryAxis{std::min(bounds->first, first), std::max(bounds->last, last)} : HistoryAxis{first, last};
    }
    if (!bounds) {
        ImGui::TextDisabled("No samples for this client.");
        return;
    }

    struct Range { const char* label; std::uint64_t milliseconds; };
    constexpr Range ranges[]{{"All data", 0}, {"Last 30 s", 30'000}, {"Last 2 min", 120'000},
        {"Last 5 min", net::NetworkTestHistoryWindowMs}};
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(110.0F * uiScale());
    if (ImGui::BeginCombo("Range", ranges[m_comparisonRangeIndex].label)) {
        for (std::size_t i = 0; i < std::size(ranges); ++i) {
            if (ImGui::Selectable(ranges[i].label, i == m_comparisonRangeIndex) && i != m_comparisonRangeIndex) {
                m_comparisonRangeIndex = i;
                m_comparisonExportPath.clear();
                m_comparisonExportMessage.clear();
            }
        }
        ImGui::EndCombo();
    }
    const auto& range = ranges[m_comparisonRangeIndex];
    if (range.milliseconds) bounds->first = bounds->last > range.milliseconds ? bounds->last - range.milliseconds : 0;
    const std::array runs{visibleHistory(histories[0], *bounds), visibleHistory(histories[1], *bounds)};
    const auto first = net::summarizeNetworkTestHistory(runs[0]);
    const auto second = net::summarizeNetworkTestHistory(runs[1]);
    auto axis = *bounds;
    if (axis.last - axis.first < 1'000) {
        if (axis.last >= 1'000) axis.first = axis.last - 1'000;
        else axis = {0, 1'000};
    }

    const auto exportComparison = [&](NetworkReportDialog kind) {
        try {
            const auto& fileA = m_networkComparisonFiles[0];
            const auto& fileB = m_networkComparisonFiles[1];
            const bool archive = kind == NetworkReportDialog::Archive;
            const bool allClients = includesAllClients(kind);
            const bool report = allClients || kind == NetworkReportDialog::Report;
            const bool charts = kind == NetworkReportDialog::Charts;
            const bool paired = !report && !m_comparisonExportPath.empty() && m_comparisonExportWasPng &&
                charts != *m_comparisonExportWasPng;
            const auto folder = m_comparisonExportPath.empty() ? fileA.path.parent_path() : m_comparisonExportPath.parent_path();
            const auto target = pickNetworkReportPath(kind, folder, m_comparisonClientIndex,
                paired ? m_comparisonExportPath.stem() : std::filesystem::path{});
            if (target.unavailable) {
                m_comparisonExportFailed = true;
                m_comparisonExportMessage = "The save dialog is unavailable.";
            } else if (!target.path.empty()) {
                std::string error;
                bool saved = false;
                std::size_t clientCount = 0;
                const core::NetworkTestSummaryCsv summary{m_comparisonClientIndex, bounds->first, bounds->last, range.label,
                    fileA.path, fileB.path, first, second};
                if (allClients) {
                    std::vector<core::NetworkTestComparisonReport> reports;
                    reports.reserve(net::MaxNetworkTestClients);
                    for (unsigned index = 1; index <= net::MaxNetworkTestClients; ++index) {
                        const auto* historyA = clientHistory(runA, index);
                        const auto* historyB = clientHistory(runB, index);
                        if (!historyA && !historyB) continue;
                        const std::array clientRuns{visibleHistory(historyA, *bounds), visibleHistory(historyB, *bounds)};
                        NetworkChartPng canvas(kComparisonChartWidth, kComparisonChartHeight);
                        drawComparisonCharts(canvas, index, range.label, *bounds, axis, clientRuns, {fileA.filename, fileB.filename});
                        auto png = canvas.encode(error);
                        if (!png) {
                            if (error.empty()) error = "Could not encode the comparison charts.";
                            break;
                        }
                        reports.push_back({{index, bounds->first, bounds->last, range.label, fileA.path, fileB.path,
                            net::summarizeNetworkTestHistory(clientRuns[0]), net::summarizeNetworkTestHistory(clientRuns[1])},
                            std::move(*png)});
                        if (archive) {
                            auto& chartSamples = reports.back().chartSamples;
                            for (std::size_t run = 0; run < clientRuns.size(); ++run) {
                                chartSamples[run].assign(clientRuns[run].begin(), clientRuns[run].end());
                            }
                        }
                    }
                    clientCount = reports.size();
                    if (error.empty()) {
                        saved = archive ? core::writeNetworkTestComparisonArchive(target.path, reports, error)
                                        : core::writeNetworkTestComparisonReports(target.path, reports, error);
                    }
                } else if (!report && !charts) {
                    saved = core::writeNetworkTestSummaryCsv(target.path, summary, error);
                } else if (charts && core::isNetworkTestCsvSource(target.path, fileA.path, fileB.path)) {
                    error = "Choose a different file to keep both source CSVs.";
                } else {
                    NetworkChartPng canvas(kComparisonChartWidth, kComparisonChartHeight);
                    drawComparisonCharts(canvas, m_comparisonClientIndex, range.label, *bounds, axis, runs,
                        {fileA.filename, fileB.filename});
                    if (charts) saved = canvas.save(target.path, error);
                    else if (const auto png = canvas.encode(error)) {
                        saved = core::writeNetworkTestComparisonReport(target.path, *png, summary, error);
                    }
                }
                m_comparisonExportFailed = !saved;
                if (!saved) m_comparisonExportMessage = std::move(error);
                else if (archive) m_comparisonExportMessage = "Exported a ZIP archive for " + std::to_string(clientCount) +
                    (clientCount == 1 ? " client." : " clients.") + " Extract it and open index.html to view the report.";
                else if (allClients) m_comparisonExportMessage = "Exported PNG and CSV for " + std::to_string(clientCount) +
                    (clientCount == 1 ? " client" : " clients") + ", plus a combined summary CSV.";
                else m_comparisonExportMessage = report ? "Comparison report exported (.png and .csv)."
                    : charts ? "Comparison charts exported." : "Comparison summary exported.";
                if (saved) {
                    m_comparisonExportPath = target.path;
                    m_comparisonExportWasPng = report ? std::nullopt : std::optional{charts};
                }
            }
        } catch (const std::exception& error) {
            m_comparisonExportFailed = true;
            m_comparisonExportMessage = error.what();
        }
    };
    if (available >= 350.0F * uiScale()) ImGui::SameLine();
    if (ImGui::Button("Export Report...")) ImGui::OpenPopup("Export Report Options");
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Save PNG and CSV reports for the selected client or all clients.");
        ImGui::EndTooltip();
    }
    std::optional<NetworkReportDialog> reportRequest;
    if (ImGui::BeginPopup("Export Report Options")) {
        if (ImGui::MenuItem("Selected Client")) reportRequest = NetworkReportDialog::Report;
        if (ImGui::BeginMenu("All Clients")) {
            if (ImGui::MenuItem("Separate Files")) reportRequest = NetworkReportDialog::AllReports;
            if (ImGui::MenuItem("ZIP Archive")) reportRequest = NetworkReportDialog::Archive;
            if (ImGui::BeginItemTooltip()) {
                ImGui::TextUnformatted("Save the HTML report, PNG charts, and CSV summaries in one ZIP.");
                ImGui::EndTooltip();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    if (reportRequest) exportComparison(*reportRequest);
    if (available >= 560.0F * uiScale()) ImGui::SameLine();
    if (ImGui::Button("Export Summary CSV...")) exportComparison(NetworkReportDialog::Summary);
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Save both runs and B - A for this client and range.");
        ImGui::EndTooltip();
    }
    if (available >= 330.0F * uiScale()) ImGui::SameLine();
    if (ImGui::Button("Export Charts PNG...")) exportComparison(NetworkReportDialog::Charts);
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Save the three comparison charts for this client and range.");
        ImGui::EndTooltip();
    }
    if (!m_comparisonExportMessage.empty()) {
        if (m_comparisonExportFailed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, .43F, .42F, 1.0F));
        ImGui::TextWrapped("%s", m_comparisonExportMessage.c_str());
        if (m_comparisonExportFailed) ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("Run A: solid   Run B: dashed");
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].empty()) ImGui::TextDisabled("%s: %s", kRunLabels[i],
            histories[i] ? "No samples in this range." : "Client absent.");
    }
    networkComparisonSummary(first, second);
    historyCharts(axis, runs);
}

void EditorRibbon::renderNetworkComparison() {
    constexpr const char* labels[]{"Load Run A...", "Load Run B..."};
    for (std::size_t slot = 0; slot < m_networkComparisonFiles.size(); ++slot) {
        auto& file = m_networkComparisonFiles[slot];
        if (ImGui::Button(labels[slot])) {
            const auto& other = m_networkComparisonFiles[1 - slot];
            try {
                const auto target = pickNetworkReportPath(NetworkReportDialog::Open, (file.path.empty() ? other.path : file.path).parent_path());
                if (target.unavailable) {
                    file.error = "The open dialog is unavailable.";
                } else if (!target.path.empty()) {
                    std::string error;
                    auto run = core::readNetworkTestCsv(target.path, error);
                    if (run) {
                        const auto name = target.path.filename().u8string();
                        std::string filename(reinterpret_cast<const char*>(name.data()), name.size());
                        auto path = target.path;
                        file = {std::move(run), std::move(path), std::move(filename), {}};
                        m_comparisonRangeIndex = 0;
                        m_comparisonExportPath.clear();
                        m_comparisonExportMessage.clear();
                    } else {
                        file.error = std::move(error);
                    }
                }
            } catch (const std::exception& error) {
                file.error = error.what();
            }
        }
        ImGui::SameLine();
        if (file.run) {
            ImGui::TextUnformatted(file.filename.c_str());
            if (ImGui::BeginItemTooltip()) {
                const auto path = file.path.u8string();
                ImGui::TextUnformatted(reinterpret_cast<const char*>(path.c_str()));
                ImGui::EndTooltip();
            }
        } else {
            ImGui::TextDisabled("No file loaded");
        }
        if (!file.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, .43F, .42F, 1.0F));
            ImGui::TextWrapped("%s", file.error.c_str());
            ImGui::PopStyleColor();
        }
    }

    const auto& first = m_networkComparisonFiles[0].run;
    const auto& second = m_networkComparisonFiles[1].run;
    if (!first || !second) return;

    const auto& clientsA = first->clients;
    const auto& clientsB = second->clients;
    if (clientsA.empty() || clientsB.empty()) return;
    if (!clientsA.contains(m_comparisonClientIndex) && !clientsB.contains(m_comparisonClientIndex)) {
        const auto common = std::ranges::find_if(clientsA, [&](const auto& entry) { return clientsB.contains(entry.first); });
        m_comparisonClientIndex = common != clientsA.end() ? common->first : std::min(clientsA.begin()->first, clientsB.begin()->first);
    }
    const auto preview = "Client " + std::to_string(m_comparisonClientIndex);
    ImGui::SetNextItemWidth(114.0F * uiScale());
    if (ImGui::BeginCombo("Client##comparison", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (unsigned index = 1; index <= net::MaxNetworkTestClients; ++index) {
            const bool inA = clientsA.contains(index);
            const bool inB = clientsB.contains(index);
            if (!inA && !inB) continue;
            auto label = "Client " + std::to_string(index);
            if (!inA) label += " (Run B)";
            else if (!inB) label += " (Run A)";
            if (ImGui::Selectable(label.c_str(), index == m_comparisonClientIndex) && index != m_comparisonClientIndex) {
                m_comparisonClientIndex = index;
                m_comparisonExportPath.clear();
                m_comparisonExportMessage.clear();
            }
        }
        ImGui::EndCombo();
    }
    renderNetworkComparisonHistory(*first, *second);
}

void EditorRibbon::renderNetworkStats(const NetworkTestRibbonActions& actions, const bool running, const bool hasHistory) {
    const float scale = uiScale();
    const auto* viewport = ImGui::GetMainViewport();
    const float margin = 12.0F * scale;
    const float width = std::min(756.0F * scale, std::max(1.0F, viewport->WorkSize.x - 2 * margin));
    const float top = std::min(140.0F * scale, viewport->WorkSize.y / 4.0F);
    const float height = std::min(510.0F * scale, std::max(1.0F, viewport->WorkSize.y - top - margin));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width - margin,
        viewport->WorkPos.y + top), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Network Test Stats", &m_networkStatsOpen, ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::Button(m_networkCompare ? "Back to Test Stats" : "Compare CSV")) m_networkCompare = !m_networkCompare;
        if (m_networkCompare) {
            renderNetworkComparison();
            ImGui::End();
            return;
        }
        if (!running && !hasHistory) {
            ImGui::TextDisabled("No network test history yet.");
            ImGui::End();
            return;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasHistory || !actions.exportCsv);
        if (ImGui::Button("Export CSV...")) {
            try {
                const auto target = pickNetworkReportPath(NetworkReportDialog::History);
                if (target.unavailable) {
                    m_networkExportFailed = true;
                    m_networkExportMessage = "The save dialog is unavailable.";
                } else if (!target.path.empty()) {
                    std::string error;
                    m_networkExportFailed = !actions.exportCsv(target.path, error);
                    m_networkExportMessage = m_networkExportFailed
                        ? (error.empty() ? "Could not export network test history." : std::move(error))
                        : "Network test history exported.";
                }
            } catch (const std::exception& error) {
                m_networkExportFailed = true;
                m_networkExportMessage = error.what();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Export up to five minutes of history for all clients.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", running ? "Live run" : "Last run");
        if (!m_networkExportMessage.empty()) {
            if (m_networkExportFailed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, .43F, .42F, 1.0F));
            ImGui::TextWrapped("%s", m_networkExportMessage.c_str());
            if (m_networkExportFailed) ImGui::PopStyleColor();
        }
        const auto clients = actions.clients();
        if (clients.empty()) {
            ImGui::TextUnformatted("Waiting for client windows...");
        } else {
            ImGui::TextDisabled("Up = client to server   Down = server to client");
            constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings;
            struct Column {
                const char* label;
                float width;
                const char* tooltip;
            };
            const Column columns[]{
                {"Client", 58.0F, "The client window number."},
                {running ? "Link" : "Last link", 90.0F, "The client's latest recorded connection state."},
                {"RTT", 57.0F, "The latest measured round-trip time."},
                {"Probe loss", 76.0F, "Recent RTT probes that received no reply, measured end to end."},
                {"Up loss", 57.0F, "Measured share of client packets dropped by the emulator."},
                {"Down loss", 72.0F, "Measured share of server packets to this client dropped by the emulator."},
                {"Up traffic", 129.0F, "UDP payload sent by the client per second."},
                {"Down traffic", 129.0F, "UDP payload received by the client per second."}
            };
            const auto& style = ImGui::GetStyle();
            const float rowHeight = ImGui::GetTextLineHeight() + 2 * style.CellPadding.y;
            const auto rows = std::min(clients.size(), std::size_t{5}) + 1;
            if (ImGui::BeginTable("Client stats", static_cast<int>(std::size(columns)), flags,
                ImVec2(0.0F, rowHeight * static_cast<float>(rows) + style.ScrollbarSize))) {
                for (const auto& column : columns) {
                    ImGui::TableSetupColumn(column.label, ImGuiTableColumnFlags_WidthFixed, column.width * scale);
                }
                ImGui::TableSetupScrollFreeze(1, 1);
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                for (int i = 0; i < static_cast<int>(std::size(columns)); ++i) {
                    if (!ImGui::TableSetColumnIndex(i)) continue;
                    ImGui::TableHeader(columns[i].label);
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260.0F * scale);
                        ImGui::TextUnformatted(columns[i].tooltip);
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }
                for (const auto& client : clients) {
                    const auto& metrics = client.metrics;
                    ImGui::TableNextRow();
                    if (ImGui::TableNextColumn()) ImGui::Text("Client %u", client.index);
                    if (ImGui::TableNextColumn()) ImGui::TextUnformatted(linkStateLabel(client.state));
                    if (ImGui::TableNextColumn()) {
                        if (metrics.rttMs) ImGui::Text("%u ms", static_cast<unsigned>(*metrics.rttMs));
                        else ImGui::TextDisabled("--");
                    }
                    if (ImGui::TableNextColumn()) lossMetric(metrics.probeLossPercent);
                    if (ImGui::TableNextColumn()) lossMetric(metrics.uplinkLossPercent);
                    if (ImGui::TableNextColumn()) lossMetric(metrics.downlinkLossPercent);
                    if (ImGui::TableNextColumn()) trafficMetric(metrics.upload);
                    if (ImGui::TableNextColumn()) trafficMetric(metrics.download);
                }
                ImGui::EndTable();
            }
            if (actions.history) networkHistoryPanel(actions, clients, m_historyClientIndex, m_historyRangeIndex);
        }
    }
    ImGui::End();
}

namespace {

void networkStatsButton(const ImVec2 origin, const float height, const bool showSummary, const NetworkOptionsEditor& editor) {
    const float scale = uiScale();
    const float buttonHeight = 24.0F * scale;
    const float top = showSummary ? 7.0F * scale : (height - buttonHeight) * .5F;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top));
    const bool wasOpen = editor.statsOpen;
    if (wasOpen) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Net Stats", ImVec2(kNetworkOptionsWidth * scale, buttonHeight))) editor.statsOpen = !wasOpen;
    if (wasOpen) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("View, export, and compare per-client network statistics.");
    }
}

bool networkOptionsButton(EditorCommandRegistry& commands, const net::NetworkTestOptions& options,
    const ImVec2 origin, const float height, const bool showSummary, const NetworkOptionsEditor& editor) {
    const float scale = uiScale();
    const float buttonHeight = 24.0F * scale;
    const auto availability = commands.state(Id::StartNetworkTest);
    const bool enabled = editor.live || availability.enabled;
    const bool emulationEnabled = !editor.live && options.conditions() != net::NetworkTestConditions{};
    const float top = showSummary ? 7.0F * scale : (height - buttonHeight) * .5F;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top));
    ImGui::BeginDisabled(!enabled);
    const char* label = emulationEnabled && !showSummary ? "Emulated" : "Options...";
    if (ImGui::Button(label, ImVec2(kNetworkOptionsWidth * scale, buttonHeight))) {
        ImGui::OpenPopup("Network Test Options");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        if (editor.live) {
            ImGui::TextUnformatted("Adjust a running client's network conditions.");
        } else {
            ImGui::Text("%d %s | %d ms | %d%% loss", options.clientCount,
                options.clientCount == 1 ? "client" : "clients", options.latencyMs, options.lossPercent);
            if (!enabled && !availability.disabledReason.empty()) text(availability.disabledReason);
        }
        ImGui::EndTooltip();
    }
    if (showSummary) {
        std::array<char, 24> summary{};
        if (emulationEnabled) {
            std::snprintf(summary.data(), summary.size(), "%dms %d%%", options.latencyMs, options.lossPercent);
        } else {
            std::snprintf(summary.data(), summary.size(), "%d %s", options.clientCount,
                options.clientCount == 1 ? "client" : "clients");
        }
        centeredLabel(summary.data(), origin, kNetworkOptionsWidth * scale, origin.y + 39.0F * scale, enabled);
    }
    bool changed = false;
    if (ImGui::BeginPopup("Network Test Options")) {
        if (enabled) changed = editor.draw();
        else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return changed;
}

float groupWidth(const Group& group) {
    if (group.kind == Kind::Create) { return 48 * uiScale(); }
    if (group.kind == Kind::Coordinates) { return 164 * uiScale(); }
    if (group.kind == Kind::Appearance) { return 115 * uiScale(); }
    if (group.kind == Kind::Multiplayer) { return kMultiplayerButtonWidth * uiScale(); }
    const float spacing = 4 * uiScale();
    const float commandsWidth = static_cast<float>(group.commands.size()) * tileWidth() +
        static_cast<float>(group.commands.empty() ? 0 : group.commands.size() - 1) * spacing;
    return group.kind == Kind::NetworkTest ? commandsWidth + 2 * (spacing + kNetworkOptionsWidth * uiScale()) : commandsWidth;
}

bool renderGroup(EditorCommandRegistry& commands, EditorRibbonState& state, const Group& group,
    const ImVec2 origin, const float height, const EditorIcons::Resolver& resolve, const NetworkOptionsEditor& networkEditor) {
    ImGui::PushID(group.label); bool changed = false;
    if (group.kind == Kind::Create) { changed = createTile(commands, state, origin, height, resolve); }
    else if (group.kind == Kind::Coordinates) { changed = coordinates(commands, state, origin, height, resolve); }
    else if (group.kind == Kind::Appearance) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 2 * uiScale(), origin.y + 6 * uiScale())); changed = ImGui::Checkbox("Compact", &state.compact);
        const float secondRow = height < 58 * uiScale() ? 24.0F : 29.0F;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 2 * uiScale(), origin.y + secondRow * uiScale())); changed = ImGui::Checkbox("Show labels", &state.showLabels) || changed;
    } else if (group.kind == Kind::Multiplayer) {
        const float scale = uiScale();
        const float buttonHeight = 24.0F * scale;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (height - buttonHeight) * 0.5F));
        ImGui::BeginDisabled(!networkEditor.multiplayerAvailable);
        const bool wasOpen = networkEditor.multiplayerOpen;
        if (wasOpen) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Multiplayer", ImVec2(kMultiplayerButtonWidth * scale, buttonHeight))) {
            networkEditor.multiplayerOpen = !wasOpen;
        }
        if (wasOpen) ImGui::PopStyleColor();
        ImGui::EndDisabled();
    } else {
        float x = origin.x;
        for (const Id id : group.commands) { const float width = tileWidth(); commandTile(commands, id, ImVec2(x, origin.y), width, height, state, resolve); x += width + 4 * uiScale(); }
        if (group.kind == Kind::NetworkTest) {
            const bool showSummary = state.showLabels && !state.compact;
            changed = networkOptionsButton(commands, state.networkTest, ImVec2(x, origin.y), height, showSummary, networkEditor);
            networkStatsButton(ImVec2(x + (kNetworkOptionsWidth + 4.0F) * uiScale(), origin.y),
                height, showSummary, networkEditor);
        }
    }
    ImGui::PopID(); return changed;
}

bool overflowGroup(EditorCommandRegistry& commands, EditorRibbonState& state, const Group& group,
    const NetworkOptionsEditor& networkEditor) {
    if (group.kind == Kind::Multiplayer) {
        ImGui::MenuItem("Multiplayer", nullptr, &networkEditor.multiplayerOpen, networkEditor.multiplayerAvailable);
        return false;
    }
    bool changed = false; ImGui::PushID(group.label); ImGui::SeparatorText(group.label);
    if (group.kind == Kind::Create) { changed = createMenu(commands, state); }
    else if (group.kind == Kind::Coordinates) {
        commandMenuItem(commands, Id::ToggleLocalSpace); commandMenuItem(commands, Id::ToggleSnap);
        ImGui::SetNextItemWidth(160); changed = ImGui::DragFloat("Move", &state.moveSnap, .01F, .001F, 1000, "%.3g", ImGuiSliderFlags_AlwaysClamp) || changed;
        ImGui::SetNextItemWidth(160); changed = ImGui::DragFloat("Rotate", &state.rotateSnap, .1F, .1F, 180, "%.3g deg", ImGuiSliderFlags_AlwaysClamp) || changed;
        ImGui::SetNextItemWidth(160); changed = ImGui::DragFloat("Scale", &state.scaleSnap, .01F, .001F, 10, "%.3g", ImGuiSliderFlags_AlwaysClamp) || changed;
    } else if (group.kind == Kind::Appearance) {
        changed = ImGui::Checkbox("Compact", &state.compact); changed = ImGui::Checkbox("Show labels", &state.showLabels) || changed;
    } else {
        for (const Id id : group.commands) { commandMenuItem(commands, id); }
        if (group.kind == Kind::NetworkTest) {
            const bool emulationEnabled = !networkEditor.live && state.networkTest.conditions() != net::NetworkTestConditions{};
            if (ImGui::BeginMenu(emulationEnabled ? "Options... (Emulation)" : "Options...",
                networkEditor.live || commands.state(Id::StartNetworkTest).enabled)) {
                changed = networkEditor.draw();
                ImGui::EndMenu();
            }
            ImGui::MenuItem("Net Stats", nullptr, &networkEditor.statsOpen);
        }
    }
    ImGui::PopID(); return changed;
}

bool clampPreference(float& value, const float fallback, const float minimum, const float maximum) noexcept {
    const float normalized = std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
    const bool changed = value != normalized; value = normalized; return changed;
}
}

bool EditorRibbon::sanitizeState(EditorRibbonState& state) noexcept {
    bool changed = false;
    switch (state.tab) { case Tab::Home: case Tab::Model: case Tab::Test: case Tab::View: case Tab::Script: break;
    default: state.tab = Tab::Home; changed = true; break; }
    if (std::find(std::begin(kCreateChoices), std::end(kCreateChoices), state.createCommand) == std::end(kCreateChoices)) { state.createCommand = Id::CreatePhysicsCube; changed = true; }
    if (state.playCommand != Id::Play && state.playCommand != Id::PlayWindow) { state.playCommand = Id::Play; changed = true; }
    changed = clampPreference(state.moveSnap, .5F, .001F, 1000) || changed;
    changed = clampPreference(state.rotateSnap, 15, .1F, 180) || changed;
    changed = clampPreference(state.scaleSnap, .1F, .001F, 10) || changed;
    auto& network = state.networkTest;
    const int clients = std::clamp(network.clientCount, 1, net::MaxNetworkTestClients);
    const int latency = std::clamp(network.latencyMs, 0, net::MaxNetworkTestLatencyMs);
    const int loss = std::clamp(network.lossPercent, 0, net::MaxNetworkTestLossPercent);
    changed = network.clientCount != clients || network.latencyMs != latency || network.lossPercent != loss || changed;
    network = {clients, latency, loss};
    return changed;
}

float EditorRibbon::preferredHeight(const EditorRibbonState& state) {
    const float scale = ImGui::GetCurrentContext() ? uiScale() : 1.0F;
    const float tools = state.tab == Tab::Model ? 64.0F : (state.compact || !state.showLabels ? 44.0F : 58.0F);
    return (36 + tools + 1) * scale;
}

void EditorRibbon::renderLiveNetworkOptions(const NetworkTestRibbonActions& actions) {
    const auto clients = actions.clients();
    if (clients.empty()) {
        ImGui::TextUnformatted("Waiting for client windows...");
        return;
    }
    auto& live = m_liveNetworkOptions;
    auto selected = std::find_if(clients.begin(), clients.end(), [&](const auto& client) {
        return client.index == live.clientIndex;
    });
    if (selected == clients.end()) selected = clients.begin();
    const auto preview = "Client " + std::to_string(selected->index);
    ImGui::SetNextItemWidth(148.0F * uiScale());
    if (ImGui::BeginCombo("Client", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (auto client = clients.begin(); client != clients.end(); ++client) {
            ImGui::PushID(static_cast<int>(client->index));
            const auto label = "Client " + std::to_string(client->index) + " (" + linkStateLabel(client->state) + ")###Client";
            const bool wasSelected = client == selected;
            if (ImGui::Selectable(label.c_str(), wasSelected)) selected = client;
            if (wasSelected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    if (live.clientIndex != selected->index) {
        live = {};
        live.clientIndex = selected->index;
        live.profile = live.draft = selected->conditions;
    } else if (live.profile != selected->conditions) {
        if (live.draft == live.profile) live.draft = selected->conditions;
        live.profile = selected->conditions;
    }

    ImGui::TextUnformatted(linkStateLabel(selected->state));
    ImGui::Text("%s %d ms | %d%% loss", selected->applying ? "Requested" : "Current",
        selected->conditions.latencyMs, selected->conditions.lossPercent);
    const bool editable = selected->editable && static_cast<bool>(actions.apply);
    ImGui::BeginDisabled(!editable);
    if (networkConditionFields(live.draft.latencyMs, live.draft.lossPercent)) live.error.clear();
    ImGui::EndDisabled();
    const bool canApply = editable && live.draft.valid() && live.draft != selected->conditions;
    ImGui::BeginDisabled(!canApply);
    bool submitted = false;
    if (ImGui::Button("Apply")) {
        live.error.clear();
        submitted = actions.apply(selected->index, live.draft, live.error);
        if (submitted) live.requested = live.draft;
        else if (live.error.empty()) live.error = "Could not apply network conditions.";
    }
    ImGui::EndDisabled();
    if (submitted || selected->applying) {
        using Link = net::NetworkTestLinkState;
        ImGui::SameLine();
        ImGui::TextUnformatted(selected->state == Link::TimedOut || selected->state == Link::Disconnected
            ? "Waiting for connection" : "Applying...");
    } else if (live.requested && *live.requested == selected->conditions && live.draft == selected->conditions) {
        ImGui::SameLine();
        ImGui::TextUnformatted("Applied");
    }
    if (!live.error.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0F * uiScale());
        text(live.error);
        ImGui::PopTextWrapPos();
    }
}

void EditorRibbon::renderMultiplayer(const MultiplayerRibbonActions& actions, const int maxPlayers) {
    auto& panel = m_multiplayer;
    const float scale = uiScale();
    const auto* viewport = ImGui::GetMainViewport();
    const float width = std::min(490.0F * scale, std::max(1.0F, viewport->WorkSize.x - 24.0F * scale));
    const float top = std::min(125.0F * scale, viewport->WorkSize.y * 0.25F);
    const float height = std::min(440.0F * scale, std::max(1.0F, viewport->WorkSize.y - top - 12.0F * scale));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5F,
        viewport->WorkPos.y + top), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F * scale, 12.0F * scale));
    if (!ImGui::Begin("Multiplayer", &panel.open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    const auto status = actions.status();
    const float inputWidth = 235.0F * scale;
    const auto transport = [&](bool& steam) {
        ImGui::SetNextItemWidth(inputWidth);
        if (ImGui::BeginCombo("Transport", steam ? "Steam" : "UDP")) {
            if (ImGui::Selectable("UDP", !steam)) steam = false;
            if (ImGui::Selectable("Steam", steam)) steam = true;
            ImGui::EndCombo();
        }
    };
    const auto notice = [](const std::string& message) {
        if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    };

    ImGui::TextWrapped("Uses the saved scene and local project assets.");
    if (!status.canLaunch) ImGui::TextWrapped("Open a project in Edit mode to launch multiplayer.");
    if (actions.saveScene) {
        ImGui::BeginDisabled(!status.canLaunch);
        if (ImGui::Button("Save Scene")) {
            panel.saveStatus.clear();
            if (actions.saveScene(panel.saveStatus)) panel.saveStatus = "Scene saved.";
            else if (panel.saveStatus.empty()) panel.saveStatus = "Could not save the scene.";
        }
        ImGui::EndDisabled();
        notice(panel.saveStatus);
    }

    ImGui::SeparatorText("Dedicated Server");
    ImGui::PushID("Server");
    ImGui::BeginDisabled(!status.canLaunch || status.serverRunning);
    transport(panel.serverSteam);
    if (!panel.serverSteam) {
        ImGui::SetNextItemWidth(inputWidth);
        ImGui::InputText("Bind address", panel.bind, sizeof(panel.bind), ImGuiInputTextFlags_CharsNoBlank);
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted("Use 0.0.0.0:27020 to listen on all local interfaces.");
            ImGui::TextUnformatted("Internet clients need a reachable UDP port.");
            ImGui::EndTooltip();
        }
    }
    ImGui::SetNextItemWidth(inputWidth);
    panel.maxPlayers = std::clamp(panel.maxPlayers, 1, maxPlayers);
    ImGui::SliderInt("Max players", &panel.maxPlayers, 1, maxPlayers, "%d",
        ImGuiSliderFlags_AlwaysClamp);
    if (!status.serverRunning) {
        ImGui::BeginDisabled(!actions.startServer || (!panel.serverSteam && !panel.bind[0]));
        if (ImGui::Button("Start Server")) {
            panel.serverError.clear();
            panel.inviteError.clear();
            panel.friendSteamId = 0;
            if (!actions.startServer(panel.serverSteam, panel.bind, static_cast<unsigned>(panel.maxPlayers), panel.serverError) &&
                panel.serverError.empty()) panel.serverError = "Could not start the dedicated server.";
        }
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    if (status.serverRunning) {
        ImGui::BeginDisabled(!actions.stopServer);
        if (ImGui::Button("Stop Server")) {
            panel.serverError.clear();
            panel.inviteError.clear();
            panel.friendSteamId = 0;
            actions.stopServer();
        }
        ImGui::EndDisabled();
    }
    notice(panel.serverError.empty() ? status.serverStatus : panel.serverError);
    if (status.serverReady && !status.serverEndpoint.empty()) {
        if (status.serverSteam) {
            constexpr std::string_view prefix = "SteamID ";
            const char* steamId = status.serverEndpoint.c_str();
            if (std::string_view(steamId).starts_with(prefix)) steamId += prefix.size();
            ImGui::Text("SteamID %s", steamId);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy SteamID")) ImGui::SetClipboardText(steamId);
        } else {
            ImGui::Text("UDP %s", status.serverEndpoint.c_str());
        }
    }
    if (status.serverRunning && status.serverSteam) {
        const std::span<const net::SteamFriend> friends = status.friends ? std::span(*status.friends)
            : std::span<const net::SteamFriend>{};
        ImGui::BeginDisabled(!status.serverReady || friends.empty() || status.invitePending);
        ImGui::SetNextItemWidth(inputWidth);
        const bool nameChanged = ImGui::InputTextWithHint("##FriendSearch", "Search by name",
            panel.friendSearch, sizeof(panel.friendSearch));
        ImGui::SameLine();
        const bool onlineChanged = ImGui::Checkbox("Online only", &panel.onlineFriendsOnly);
        ImGui::EndDisabled();

        auto& visible = panel.visibleFriends;
        if (nameChanged || onlineChanged || panel.friendFilterSnapshot != status.friends) {
            panel.friendFilterSnapshot = status.friends;
            visible.clear();
            visible.reserve(friends.size());
            const auto nameMatches = steamFriendNameMatcher(panel.friendSearch);
            for (std::size_t index = 0; index < friends.size(); ++index) {
                const auto& entry = friends[index];
                if ((!panel.onlineFriendsOnly || entry.online()) && nameMatches(entry.name)) visible.push_back(index);
            }
            if (status.friends && std::ranges::find(friends, panel.friendSteamId, &net::SteamFriend::id) == friends.end()) {
                panel.friendSteamId = 0;
            }
        }
        auto selected = std::ranges::find(visible, panel.friendSteamId,
            [friends](std::size_t index) { return friends[index].id; });
        const auto displayName = [&](const net::SteamFriend& entry) {
            if (std::ranges::count(friends, entry.name, &net::SteamFriend::name) > 1) {
                return entry.name + " (" + std::to_string(entry.id) + ")";
            }
            return entry.name;
        };
        const auto avatar = [&](std::uint64_t id) {
            return actions.avatar ? actions.avatar(id) : EditorIcons::Image{};
        };
        const float friendRowHeight = std::max(24.0F * scale, ImGui::GetTextLineHeight() + 4.0F * scale);
        const float friendWidth = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Friend").x - ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(std::clamp(friendWidth, 1.0F, 320.0F * scale));
        ImGui::BeginDisabled(!status.serverReady || visible.empty() || status.invitePending);
        if (ImGui::BeginCombo("Friend", nullptr,
                ImGuiComboFlags_HeightLarge | static_cast<ImGuiComboFlags>(ImGuiComboFlags_CustomPreview))) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            if (ImGui::IsWindowAppearing() && selected != visible.end()) {
                clipper.IncludeItemByIndex(static_cast<int>(selected - visible.begin()));
            }
            while (clipper.Step()) {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                    const auto& entry = friends[visible[static_cast<std::size_t>(index)]];
                    const auto id = std::to_string(entry.id);
                    const auto label = displayName(entry);
                    const ImVec2 origin = ImGui::GetCursorScreenPos();
                    const float right = origin.x + ImGui::GetContentRegionAvail().x;
                    const bool isSelected = entry.id == panel.friendSteamId;
                    ImGui::PushID(id.c_str());
                    if (ImGui::Selectable("##FriendChoice", isSelected, 0, ImVec2(0.0F, friendRowHeight))) {
                        panel.friendSteamId = entry.id;
                        selected = visible.begin() + index;
                        panel.inviteError.clear();
                    }
                    const auto presence = friendStatusText(entry.presence);
                    if (ImGui::IsItemVisible()) {
                        drawSteamFriend(label, presence, avatar(entry.id), origin, right, friendRowHeight, 20.0F * scale);
                    }
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::TextUnformatted(label.c_str());
                        ImGui::TextUnformatted(presence.label);
                        ImGui::EndTooltip();
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginComboPreview()) {
            if (selected == visible.end()) {
                ImGui::TextUnformatted(!status.serverReady || !status.friends ? "Waiting for Steam friends..."
                    : friends.empty() ? "No Steam friends" : visible.empty() ? "No matching friends" : "Select a Steam friend");
            } else {
                const auto& entry = friends[*selected];
                const auto label = displayName(entry);
                const auto& clip = ImGui::GetCurrentContext()->ComboPreviewData.PreviewRect;
                const float previewHeight = ImGui::GetTextLineHeight();
                drawSteamFriend(label, friendStatusText(entry.presence), avatar(entry.id), ImGui::GetCursorScreenPos(),
                    clip.Max.x - ImGui::GetStyle().FramePadding.x, previewHeight, previewHeight);
            }
            ImGui::EndComboPreview();
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!status.serverReady || !actions.invite || selected == visible.end() || status.invitePending);
        if (ImGui::Button("Invite Friend")) {
            panel.inviteError.clear();
            if (!actions.invite(panel.friendSteamId, panel.inviteError) && panel.inviteError.empty()) {
                panel.inviteError = "Could not request the Steam invitation.";
            }
        }
        ImGui::EndDisabled();
        if (status.invitePending) ImGui::TextDisabled("Sending invitation...");
        else notice(panel.inviteError.empty() ? status.inviteStatus : panel.inviteError);
    } else {
        panel.friendSteamId = 0;
        panel.friendFilterSnapshot.reset();
        panel.visibleFriends.clear();
    }
    ImGui::PopID();

    ImGui::SeparatorText("Join Game");
    ImGui::PushID("Client");
    ImGui::BeginDisabled(!status.canLaunch || status.clientRunning);
    transport(panel.clientSteam);
    ImGui::SetNextItemWidth(inputWidth);
    if (panel.clientSteam) {
        ImGui::InputTextWithHint("Host", "SteamID64 or wait for an invitation", panel.hostSteamId,
            sizeof(panel.hostSteamId), ImGuiInputTextFlags_CharsNoBlank);
    } else {
        ImGui::InputTextWithHint("Address", "IPv4 or hostname[:port]", panel.address, sizeof(panel.address),
            ImGuiInputTextFlags_CharsNoBlank);
    }
    if (!status.clientRunning) {
        ImGui::BeginDisabled(!actions.connect || (!panel.clientSteam && !panel.address[0]));
        if (ImGui::Button(panel.clientSteam && !panel.hostSteamId[0] ? "Wait for Invite" : "Connect")) {
            panel.clientError.clear();
            const std::string_view address = panel.clientSteam ? panel.hostSteamId : panel.address;
            if (!actions.connect(panel.clientSteam, address, panel.clientError) && panel.clientError.empty()) {
                panel.clientError = "Could not start the game client.";
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    if (status.clientRunning) {
        ImGui::BeginDisabled(!actions.stopClient);
        if (ImGui::Button("Close Client")) {
            panel.clientError.clear();
            actions.stopClient();
        }
        ImGui::EndDisabled();
    }
    notice(panel.clientError.empty() ? status.clientStatus : panel.clientError);
    ImGui::PopID();
    ImGui::End();
    ImGui::PopStyleVar();
}

bool EditorRibbon::render(EditorCommandRegistry& commands, EditorRibbonState& state, const EditorIcons::Resolver& resolve,
    const NetworkTestRibbonActions& networkActions, const MultiplayerRibbonActions& multiplayerActions,
    const unsigned networkPlayerLimit) {
    bool changed = sanitizeState(state); ImGui::PushID("EditorRibbon");
    const bool networkRunning = networkActions.clients && commands.state(Id::StopNetworkTest).enabled;
    const int maxPlayers = static_cast<int>(networkPlayerLimit ? networkPlayerLimit : net::MaxPeers);
    const int maxTestClients = std::min(maxPlayers, net::MaxNetworkTestClients);
    if (!networkRunning && state.networkTest.clientCount > maxTestClients) {
        state.networkTest.clientCount = maxTestClients;
        changed = true;
    }
    const bool hasNetworkHistory = networkActions.clients && networkActions.hasHistory && networkActions.hasHistory();
    const bool newRun = networkRunning && !m_networkWasRunning;
    m_networkWasRunning = networkRunning;
    if ((!networkRunning || newRun) && m_liveNetworkOptions.clientIndex != 0) m_liveNetworkOptions = {};
    if (newRun || (!networkRunning && !hasNetworkHistory)) {
        m_historyClientIndex = 0;
        m_historyRangeIndex = 0;
        m_networkExportMessage.clear();
        m_networkExportFailed = false;
    }
    const NetworkOptionsEditor networkEditor{networkRunning, [&] {
        if (!networkRunning) return networkOptionsFields(state.networkTest, maxTestClients);
        renderLiveNetworkOptions(networkActions);
        return false;
    }, m_networkStatsOpen, static_cast<bool>(multiplayerActions.status), m_multiplayer.open};
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
        changed = renderGroup(commands, state, groups[i], ImVec2(origin.x + used, origin.y + header + 1 * uiScale()),
            row, resolve, networkEditor) || changed;
        used += width; first = false;
    }
    if (count) {
        const float x = origin.x + std::min(used + (first ? 0 : gap), std::max(0.0F, available - moreWidth));
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y + header + 3 * uiScale()));
        if (ImGui::InvisibleButton("More...", ImVec2(std::min(moreWidth, available), 34 * uiScale()), ImGuiButtonFlags_EnableNav)) { ImGui::OpenPopup("Overflow"); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("More tools"); }
        EditorIcons::draw(EditorIcons::Icon::More, x + 6 * uiScale(), origin.y + header + 9 * uiScale(), 24 * uiScale(), resolve);
        if (ImGui::BeginPopup("Overflow")) {
            for (std::size_t i = 0; i < count; ++i) {
                changed = overflowGroup(commands, state, *overflow[i], networkEditor) || changed;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x, origin.y + header + row), ImVec2(origin.x + available, origin.y + header + row), kBorder);
    changed = sanitizeState(state) || changed;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header + row)); ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopStyleColor(3); ImGui::PopStyleVar(4); ImGui::PopID();
    if (m_networkStatsOpen) renderNetworkStats(networkActions, networkRunning, hasNetworkHistory);
    if (m_multiplayer.open && multiplayerActions.status) renderMultiplayer(multiplayerActions, maxPlayers);
    return changed;
}

}
