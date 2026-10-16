#include "engine/editor/EditorCommands.h"
#include "engine/editor/EditorRibbon.h"
#include "engine/editor/EditorTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui_internal.h"

namespace {
using engine::editor::CommandState;
using engine::editor::EditorCommandId;
using engine::editor::EditorCommandRegistry;
using engine::editor::EditorRibbon;
using engine::editor::EditorRibbonState;
using engine::editor::EditorRibbonTab;
using Id = EditorCommandId;
using Tab = EditorRibbonTab;
namespace Icons = engine::editor::EditorIcons;

const std::filesystem::path kBuiltinAssets = std::filesystem::path(__FILE__).parent_path().parent_path() / "assets";

void require(const bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

bool samePreferences(const EditorRibbonState& a, const EditorRibbonState& b) {
    return a.tab == b.tab && a.compact == b.compact && a.showLabels == b.showLabels &&
        a.createCommand == b.createCommand && a.playCommand == b.playCommand &&
        a.moveSnap == b.moveSnap && a.rotateSnap == b.rotateSnap && a.scaleSnap == b.scaleSnap;
}

void registryContracts() {
    EditorCommandRegistry commands;
    int queries = 0;
    int effects = 0;
    bool enabled = true;
    bool checked = false;
    std::vector<Id> actions;
    const auto query = [&](const Id) {
        ++queries;
        return CommandState{enabled, checked, enabled ? "" : "Selection is gone."};
    };
    const auto action = [&](const Id id) { actions.push_back(id); ++effects; };
    const auto invalid = static_cast<Id>(std::numeric_limits<std::uint16_t>::max());
    for (const Id id : {Id::SaveScene, Id::Count, invalid}) {
        require(!commands.state(id).enabled && !commands.execute(id), "Unconnected registry accepted a command");
        require(!commands.state(id).disabledReason.empty(), "Unavailable command has no explanation");
    }
    commands.setHandlers(query, {});
    require(!commands.state(Id::Play).enabled && !commands.execute(Id::Play), "Missing action failed open");
    require(queries == 0, "Incomplete registration queried application state");
    commands.setHandlers({}, action);
    require(!commands.state(Id::Play).enabled && !commands.execute(Id::Play), "Missing query failed open");
    commands.setHandlers(query, action);
    for (const Id id : {Id::Count, invalid}) {
        require(EditorCommandRegistry::definition(id) == nullptr, "Unknown command has a definition");
        require(!commands.state(id).enabled && !commands.execute(id), "Unknown command reached handlers");
    }
    require(queries == 0 && effects == 0, "Invalid command called an application handler");

    checked = true;
    for (int i = 0; i < 4; ++i) {
        const auto state = commands.state(Id::Delete);
        require(state.enabled && state.checked, "State query lost availability or checked status");
    }
    require(effects == 0 && actions.empty(), "State queries performed a mutation");
    enabled = false; // Selection vanished after the UI queried the command.
    require(!commands.execute(Id::Delete) && effects == 0, "Execute used stale enabled state");
    require(!commands.state(Id::Delete).enabled, "Disabled state is not observable");
    enabled = true; // The command becomes available after an earlier disabled query.
    require(commands.execute(Id::Delete) && effects == 1 && actions.back() == Id::Delete,
        "Execute did not recheck current availability");

    const auto definitions = EditorCommandRegistry::definitions();
    require(definitions.size() == static_cast<std::size_t>(Id::Count), "Command catalog is incomplete");
    std::array<bool, static_cast<std::size_t>(Id::Count)> seen{};
    for (const auto& definition : definitions) {
        const auto index = static_cast<std::size_t>(definition.id);
        require(index < seen.size() && !seen[index], "Command catalog contains an invalid or duplicate ID");
        seen[index] = true;
        require(!definition.label.empty() && !definition.tooltip.empty(), "Command is missing usable presentation text");
        require(EditorCommandRegistry::definition(definition.id) == &definition, "Command lookup disagrees with catalog");
        const auto before = actions.size();
        require(commands.execute(definition.id), "Connected command did not execute");
        require(actions.size() == before + 1U && actions.back() == definition.id,
            "One execution dispatched multiple actions or the wrong command");
    }
    commands.setHandlers({}, {});
    const auto before = actions.size();
    require(!commands.execute(Id::Play) && actions.size() == before, "Disconnected registry retained an action");
}

void preferenceContracts() {
    EditorRibbonState state;
    require(!EditorRibbon::sanitizeState(state), "Default preferences need repair");
    const auto original = state;
    require(!EditorRibbon::sanitizeState(state) && samePreferences(state, original), "Valid preferences changed");
    const float normalHeight = EditorRibbon::preferredHeight(state);
    state.compact = true;
    require(std::isfinite(normalHeight) && normalHeight > EditorRibbon::preferredHeight(state) &&
        EditorRibbon::preferredHeight(state) > 0.0F, "Headless preferred heights are invalid");

    for (const Id create : {Id::CreateEmpty, Id::CreateRenderCube, Id::CreatePhysicsCube,
            Id::CreatePhysicsSphere, Id::CreatePhysicsPyramid, Id::CreateCamera}) {
        for (const Id play : {Id::Play, Id::PlayWindow}) {
            state.createCommand = create;
            state.playCommand = play;
            require(!EditorRibbon::sanitizeState(state) && state.createCommand == create && state.playCommand == play,
                "Valid remembered action was discarded");
        }
    }
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), -5.0F, 0.0F, std::numeric_limits<float>::max()}) {
        state.tab = static_cast<Tab>(-1);
        state.createCommand = Id::Delete;
        state.playCommand = Id::ScriptNew;
        state.moveSnap = state.rotateSnap = state.scaleSnap = bad;
        require(EditorRibbon::sanitizeState(state), "Corrupt preferences were accepted unchanged");
        require(state.tab == Tab::Home && state.createCommand == Id::CreatePhysicsCube && state.playCommand == Id::Play,
            "Invalid tab or remembered action did not recover safely");
        require(std::isfinite(state.moveSnap) && state.moveSnap > 0.0F && state.moveSnap <= 1000.0F &&
            std::isfinite(state.rotateSnap) && state.rotateSnap > 0.0F && state.rotateSnap <= 180.0F &&
            std::isfinite(state.scaleSnap) && state.scaleSnap > 0.0F && state.scaleSnap <= 10.0F,
            "Invalid snap reached the editor");
        require(!EditorRibbon::sanitizeState(state), "Preference repair is not idempotent");
    }
    state.tab = static_cast<Tab>(999);
    state.createCommand = state.playCommand = Id::Count;
    require(EditorRibbon::sanitizeState(state) && state.tab == Tab::Home &&
        EditorCommandRegistry::definition(state.createCommand) && EditorCommandRegistry::definition(state.playCommand),
        "Out-of-range command preferences survived repair");
}

void validateDrawData() {
    const auto* data = ImGui::GetDrawData();
    require(data && data->Valid && data->TotalVtxCount > 0 && data->TotalIdxCount > 0, "Ribbon produced no valid draw data");
    for (const auto* list : data->CmdLists) {
        for (const auto& vertex : list->VtxBuffer) {
            require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) &&
                std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y), "Nonfinite ribbon vertex");
        }
        for (const auto& command : list->CmdBuffer) {
            const auto& clip = command.ClipRect;
            require(std::isfinite(clip.x) && std::isfinite(clip.y) && std::isfinite(clip.z) && std::isfinite(clip.w) &&
                clip.x <= clip.z && clip.y <= clip.w, "Invalid ribbon clip rectangle");
            require(static_cast<std::size_t>(command.IdxOffset) + command.ElemCount <=
                static_cast<std::size_t>(list->IdxBuffer.Size), "Ribbon draw command exceeds index buffer");
            for (unsigned int index = command.IdxOffset; index < command.IdxOffset + command.ElemCount; ++index) {
                require(static_cast<std::size_t>(list->IdxBuffer[index]) + command.VtxOffset <
                    static_cast<std::size_t>(list->VtxBuffer.Size), "Ribbon draw index exceeds vertex buffer");
            }
        }
    }
}

void themeContracts() {
    const auto& style = ImGui::GetStyle();
    struct Sample { ImGuiCol index; ImU32 expected; };
    constexpr Sample samples[]{
        {ImGuiCol_WindowBg, IM_COL32(25, 26, 31, 255)},
        {ImGuiCol_TitleBg, IM_COL32(32, 34, 39, 255)},
        {ImGuiCol_FrameBg, IM_COL32(18, 18, 21, 255)},
        {ImGuiCol_Border, IM_COL32(48, 51, 59, 255)},
        {ImGuiCol_MenuBarBg, IM_COL32(39, 41, 48, 255)},
    };
    for (const auto& sample : samples) {
        require(ImGui::ColorConvertFloat4ToU32(style.Colors[sample.index]) == sample.expected,
            "Editor surface color differs from the sampled reference palette");
    }
    require(style.WindowRounding == 0.0F && style.ChildRounding == 0.0F &&
        style.FrameRounding == 0.0F && style.TabRounding == 0.0F && style.ScrollbarRounding == 0.0F,
        "Editor surfaces no longer use the reference's flat corners");
    require(style.PopupRounding == 2.0F && style.WindowBorderSize == 1.0F &&
        style.ChildBorderSize == 1.0F, "Editor popup rounding or panel borders changed");
    require(style.FontSizeBase == engine::editor::kEditorFontSize,
        "Editor style does not use its configured default font size");
    auto* font = ImGui::GetIO().FontDefault;
    require(font && font->LegacySize == engine::editor::kEditorFontSize, "Editor font was not configured at the default size");
    auto* baked = font->GetFontBaked(engine::editor::kEditorFontSize);
    require(baked != nullptr, "Editor font has no baked glyphs at its default size");
    for (const ImWchar codepoint : {ImWchar(0x0041), ImWchar(0x0416), ImWchar(0x044f), ImWchar(0x0451)}) {
        const auto* glyph = baked->FindGlyphNoFallback(codepoint);
        require(glyph && glyph->Codepoint == codepoint && glyph->Visible,
            "Bundled editor font lacks a real Latin or Cyrillic glyph");
    }
}

struct Gui final {
    std::vector<std::string> errors;
    EditorRibbon ribbon;
    ImGuiID pauseId{};
    ImGuiID moreId{};
    float firstRowY{};
    bool popupDrawn{};
    Icons::Resolver resolveIcon;

    explicit Gui(const std::filesystem::path& localFonts = {}) {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DeltaTime = 1.0F / 60.0F;
        engine::editor::applyEditorTheme();
        const bool fontsLoaded = localFonts.empty() ? engine::editor::configureEditorFonts(io, kBuiltinAssets) :
            engine::editor::configureEditorFonts(io, kBuiltinAssets, localFonts);
        require(fontsLoaded, "Cannot load editor fonts");
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "Cannot build headless ImGui font atlas");
        // Emulate the legacy backend's completed atlas upload, so GetTexID()
        // can distinguish font commands from the synthetic icon texture.
        io.Fonts->SetTexID(static_cast<ImTextureID>(0xFFFFU));
        themeContracts();
        auto& context = *ImGui::GetCurrentContext();
        context.ErrorCallbackUserData = &errors;
        context.ErrorCallback = [](ImGuiContext*, void* user, const char* message) {
            static_cast<std::vector<std::string>*>(user)->emplace_back(message);
        };
    }
    ~Gui() { ImGui::DestroyContext(); }

    bool frame(EditorCommandRegistry& commands, EditorRibbonState& state, const float width,
        const ImVec2 mouse = {-100.0F, -100.0F}, const bool down = false, const bool keepPopups = false) {
        auto& io = ImGui::GetIO();
        io.DisplaySize = {width, 720.0F};
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        ImGui::NewFrame();
        require(ImGui::GetFontSize() == engine::editor::kEditorFontSize,
            "Ribbon is not rendering at the real editor's default font size");
        if (!keepPopups && !ImGui::GetCurrentContext()->OpenPopupStack.empty()) {
            ImGui::ClosePopupToLevel(0, true);
        }
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({width, EditorRibbon::preferredHeight(state)});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("Editor command ribbon test", nullptr, ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        firstRowY = ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5F;
        ImGui::PushID("EditorRibbon");
        moreId = ImGui::GetID("More...");
        ImGui::PushID("Transport");
        ImGui::PushID(static_cast<int>(Id::Pause));
        pauseId = ImGui::GetID("Pause");
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
        auto& context = *ImGui::GetCurrentContext();
        const auto styleDepth = context.StyleVarStack.Size;
        const auto colorDepth = context.ColorStack.Size;
        const auto idDepth = context.CurrentWindow->IDStack.Size;
        const bool changed = ribbon.render(commands, state, resolveIcon);
        require(context.StyleVarStack.Size == styleDepth && context.ColorStack.Size == colorDepth &&
            context.CurrentWindow->IDStack.Size == idDepth, "Ribbon leaked an ImGui style or ID stack");
        if (context.CurrentWindow->DC.CursorMaxPos.y > context.CurrentWindow->InnerRect.Max.y + 1.0F) {
            throw std::runtime_error("Ribbon controls extend below the allocated toolbar height: tab=" +
                std::to_string(static_cast<int>(state.tab)) + " compact=" + std::to_string(state.compact) +
                " labels=" + std::to_string(state.showLabels) + " width=" + std::to_string(width) +
                " cursorMaxY=" + std::to_string(context.CurrentWindow->DC.CursorMaxPos.y) +
                " innerMaxY=" + std::to_string(context.CurrentWindow->InnerRect.Max.y) +
                " preferredHeight=" + std::to_string(EditorRibbon::preferredHeight(state)));
        }
        popupDrawn = !context.OpenPopupStack.empty() && context.OpenPopupStack.back().Window &&
            context.OpenPopupStack.back().Window->Active;
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::Render();
        require(errors.empty() && context.ErrorCountCurrentFrame == 0, "ImGui reported a ribbon error");
        validateDrawData();
        return changed;
    }

    ImVec2 locatePause(EditorCommandRegistry& commands, EditorRibbonState& state) {
        // Probe actual hover hit-testing rather than assuming button coordinates or widths.
        frame(commands, state, 1600.0F);
        frame(commands, state, 1600.0F);
        for (float x = 8.0F; x < 1592.0F; x += 8.0F) {
            const ImVec2 point{x, firstRowY};
            frame(commands, state, 1600.0F, point);
            if (ImGui::GetCurrentContext()->HoveredId == pauseId) { return point; }
        }
        throw std::runtime_error("Could not find the real rendered Pause button");
    }

    void openOverflow(EditorCommandRegistry& commands, EditorRibbonState& state) {
        frame(commands, state, 320.0F);
        frame(commands, state, 320.0F);
        for (float y = 8.0F; y < EditorRibbon::preferredHeight(state); y += 8.0F) {
            for (float x = 8.0F; x < 312.0F; x += 8.0F) {
                const ImVec2 point{x, y};
                frame(commands, state, 320.0F, point);
                if (ImGui::GetCurrentContext()->HoveredId != moreId) { continue; }
                require(!frame(commands, state, 320.0F, point, true, true), "Overflow press changed preferences");
                require(!frame(commands, state, 320.0F, point, false, true), "Overflow click changed preferences");
                require(!frame(commands, state, 320.0F, {-100.0F, -100.0F}, false, true),
                    "Passive overflow render changed preferences");
                require(popupDrawn, "Clicking More did not render the overflow popup");
                return;
            }
        }
        throw std::runtime_error("Could not find the real rendered More button at 320px");
    }
};

void renderingAndClicks() {
    Gui gui;
    EditorCommandRegistry commands;
    int effects = 0;
    bool pauseEnabled = true;
    Id lastAction = Id::Count;
    commands.setHandlers([&](const Id id) {
        return CommandState{id != Id::Pause || pauseEnabled,
            id == Id::ToolMove || id == Id::ShowHierarchy, "Simulation is not playing."};
    }, [&](const Id id) { ++effects; lastAction = id; });
    for (const Tab tab : {Tab::Home, Tab::Model, Tab::Test, Tab::View, Tab::Script}) {
        for (const bool compact : {false, true}) {
            for (const bool labels : {false, true}) {
                for (const float width : {1600.0F, 320.0F}) {
                    EditorRibbonState state;
                    state.tab = tab;
                    state.compact = compact;
                    state.showLabels = labels;
                    state.createCommand = Id::CreatePhysicsSphere;
                    state.playCommand = Id::PlayWindow;
                    const auto before = state;
                    for (int frame = 0; frame < 2; ++frame) {
                        require(!gui.frame(commands, state, width), "Passive ribbon render changed preferences");
                    }
                    require(samePreferences(state, before) && effects == 0, "Rendering mutated preferences or dispatched a command");
                }
            }
        }
    }
    // The native image path is exercised with a borrowed synthetic handle. No
    // backend runs, but draw commands must actually carry the image texture ID.
    std::vector<std::string> requestedIcons;
    constexpr std::uintptr_t imageHandle = 0x123456U;
    gui.resolveIcon = [&](const std::string_view path) {
        const std::filesystem::path relative(path);
        require(path.starts_with("editor/icons/") && !relative.is_absolute() &&
            relative.lexically_normal() == relative && path.find("..") == std::string_view::npos,
            "Ribbon requested an icon outside the built-in editor icon directory");
        require(std::filesystem::is_regular_file(kBuiltinAssets / relative), "Ribbon requested a missing built-in icon");
        requestedIcons.emplace_back(path);
        return Icons::Image{reinterpret_cast<void*>(imageHandle), 48.0F, 48.0F};
    };
    EditorRibbonState imageState;
    require(!gui.frame(commands, imageState, 1600.0F), "Image-backed ribbon rendering changed preferences");
    for (const char* icon : {"editor/icons/RibbonSelect.png", "editor/icons/RibbonMove.png",
            "editor/icons/RibbonPauseSmall.png", "editor/icons/RibbonStopActiveSmall.png"}) {
        require(std::find(requestedIcons.begin(), requestedIcons.end(), icon) != requestedIcons.end(),
            "A core toolbar pictogram did not reach the native image resolver");
    }
    bool imageSubmitted = false;
    for (const auto* list : ImGui::GetDrawData()->CmdLists) {
        for (const auto& command : list->CmdBuffer) {
            imageSubmitted = imageSubmitted || (command.ElemCount > 0 && command.GetTexID() == static_cast<ImTextureID>(imageHandle));
        }
    }
    require(imageSubmitted && effects == 0, "Ribbon did not submit the resolved icon image or invoked a command while drawing");
    gui.resolveIcon = {};
    // With labels, every tab has real overflow at this width. Open it through
    // its rendered button so popup lifetime/focus follows normal ImGui input.
    for (const Tab tab : {Tab::Home, Tab::Model, Tab::Test, Tab::View, Tab::Script}) {
        EditorRibbonState narrow;
        narrow.tab = tab;
        const auto before = narrow;
        gui.openOverflow(commands, narrow);
        require(samePreferences(narrow, before) && effects == 0, "Overflow render invoked a command or changed preferences");
    }

    EditorRibbonState state;
    const auto point = gui.locatePause(commands, state);
    require(effects == 0, "Hovering a ribbon command invoked it");
    gui.frame(commands, state, 1600.0F, point, true);
    require(effects == 0, "Command fired on mouse-down instead of a completed click");
    gui.frame(commands, state, 1600.0F, point, false);
    require(effects == 1 && lastAction == Id::Pause, "Real button click did not dispatch exactly one Pause action");
    gui.frame(commands, state, 1600.0F, point, false);
    require(effects == 1, "Button release repeated the command on the next frame");

    pauseEnabled = false;
    gui.frame(commands, state, 1600.0F, point, false);
    gui.frame(commands, state, 1600.0F, point, true);
    gui.frame(commands, state, 1600.0F, point, false);
    require(effects == 1, "Disabled ribbon button dispatched an action");

    pauseEnabled = true;
    gui.frame(commands, state, 1600.0F, point, false);
    gui.frame(commands, state, 1600.0F, point, true);
    pauseEnabled = false; // Availability changes between press and release.
    gui.frame(commands, state, 1600.0F, point, false);
    require(effects == 1, "Button ignored availability lost during a click");
}

void localFontContracts() {
    const auto localFonts = kBuiltinAssets.parent_path() / ".local/editor-fonts";
    if (!std::filesystem::is_regular_file(localFonts / "BuilderSans-Regular.otf") ||
        !std::filesystem::is_regular_file(localFonts / "BuilderSans-Medium.otf")) { return; }
    // The ordinary Gui above covers the portable free-font path. This separate
    // atlas also verifies an explicitly installed local face and merged real
    // Cyrillic glyphs through the same themeContracts checks in Gui's setup.
    Gui gui(localFonts);
    require(std::string_view(ImGui::GetIO().FontDefault->GetDebugName()).starts_with("Builder Sans"),
        "Explicit local Builder fonts were ignored");
    EditorCommandRegistry commands;
    int effects = 0;
    commands.setHandlers([](Id) { return CommandState{}; }, [&](Id) { ++effects; });
    EditorRibbonState state;
    require(!gui.frame(commands, state, 1600.0F) && effects == 0,
        "Local Builder font rendering changed preferences or invoked a command");
}
} // namespace

int main() {
    try {
        registryContracts();
        preferenceContracts();
        renderingAndClicks();
        localFontContracts();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Editor commands, sampled theme, default-size Cyrillic fonts, image icons, ribbon overflow and real mouse clicks passed\n";
    return 0;
}
