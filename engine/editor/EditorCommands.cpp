#include "engine/editor/EditorCommands.h"

#include <array>
#include <cstddef>
#include <utility>

namespace engine::editor {
namespace {
using Id = EditorCommandId;

constexpr auto kDefinitions = std::to_array<EditorCommandDefinition>({
    {Id::ProjectSwitch, "Project...", "Open the project selector.", ""},
    {Id::SaveScene, "Save Scene", "Save the current authoring scene.", "Ctrl+S"},
    {Id::LoadScene, "Load Scene", "Reload the project's authoring scene from disk.", "Ctrl+O"},
    {Id::Undo, "Undo", "Undo the last scene edit.", "Ctrl+Z"},
    {Id::Redo, "Redo", "Redo the last undone scene edit.", "Ctrl+Y / Ctrl+Shift+Z"},
    {Id::CreateEmpty, "Empty", "Create an empty entity at the editor spawn position.", "Ctrl+N"},
    {Id::CreateRenderCube, "Render Cube", "Create a cube with a mesh renderer.", ""},
    {Id::CreatePhysicsCube, "Physics Cube", "Create a cube with rendering and physics components.", ""},
    {Id::CreatePhysicsSphere, "Physics Sphere", "Create a sphere with rendering and physics components.", ""},
    {Id::CreatePhysicsPyramid, "Physics Pyramid", "Create a pyramid with rendering and physics components.", ""},
    {Id::CreateCamera, "Camera", "Create a scene camera.", ""},
    {Id::Duplicate, "Duplicate", "Duplicate the selected entity and its components.", "Ctrl+D"},
    {Id::Delete, "Delete", "Delete the selected entity.", "Delete"},
    {Id::ToolSelect, "Select", "Select entities without a transform gizmo.", "Q"},
    {Id::ToolMove, "Move", "Move the selected entity with the transform gizmo.", "W"},
    {Id::ToolRotate, "Rotate", "Rotate the selected entity with the transform gizmo.", "E"},
    {Id::ToolScale, "Scale", "Scale the selected entity with the transform gizmo.", "R"},
    {Id::ToggleLocalSpace, "Local Space", "Toggle local and world gizmo coordinates.", ""},
    {Id::ToggleSnap, "Snap", "Enable transform snapping using the Model tab's increments.", ""},
    {Id::FocusSelected, "Focus", "Frame the selected entity with the editor camera.", "F"},
    {Id::FrameScene, "Frame Scene", "Frame the scene bounds with the editor camera.", ""},
    {Id::ResetTransform, "Reset Transform", "Reset the selected entity's transform.", ""},
    {Id::DropToGround, "Drop to Ground", "Place the selected entity on the ground plane.", ""},
    {Id::ZeroVelocity, "Zero Velocity", "Clear the selected rigid body's linear and angular motion.", ""},
    {Id::RandomizeTint, "Random Tint", "Choose a new tint for the selected mesh renderer.", ""},
    {Id::ToggleVisible, "Visible", "Show or hide the selected mesh renderer.", ""},
    {Id::ToggleStatic, "Static", "Toggle whether the selected rigid body is static.", ""},
    {Id::ToggleGravity, "Gravity", "Toggle gravity for the selected rigid body.", ""},
    {Id::ToggleCollider, "Collider", "Enable or disable the selected collider.", ""},
    {Id::Play, "Play in Editor", "Start or resume the simulation in the editor.", "Space"},
    {Id::PlayWindow, "Play Window", "Launch the current scene snapshot in a separate game window.", ""},
    {Id::StopWindow, "Stop Separate Game", "Stop the separate game process.", ""},
    {Id::Pause, "Pause", "Pause simulation ticks, including scripts, physics and React updates.", ""},
    {Id::Stop, "Stop", "Stop in-editor Play and restore the authoring scene and history.", "Esc"},
    {Id::StepTick, "Step Tick", "While paused, advance scripts, physics and React by one 1/60 second tick.", ""},
    {Id::ScriptDemo, "Script Demo", "Add the Collectathon and React UI demonstration to the scene.", ""},
    {Id::ShowHierarchy, "Hierarchy", "Show or hide the scene hierarchy.", ""},
    {Id::ShowInspector, "Inspector", "Show or hide entity properties.", ""},
    {Id::ShowContent, "Content", "Show or hide the project content browser.", ""},
    {Id::ShowStats, "Statistics", "Show or hide engine statistics.", ""},
    {Id::ShowScriptWorkspace, "Script Workspace", "Switch between the viewport and script documents.", ""},
    {Id::ShowAssistant, "Assistant", "Show or hide the local Codex assistant.", ""},
    {Id::ShowViewport, "Viewport", "Show or hide the scene viewport.", ""},
    {Id::ShowStatus, "Status Bar", "Show or hide the editor status bar.", ""},
    {Id::ToggleColliderDebug, "Collider Bounds", "Show or hide collider debug bounds.", "F1"},
    {Id::ToggleHoverOutline, "Hover Outline", "Show or hide the outline under the cursor.", ""},
    {Id::ToggleLayoutLock, "Lock Layout", "Lock or unlock editor panel placement.", ""},
    {Id::ResetLayout, "Studio Reference Layout", "Use the reference arrangement: viewport and a docked Assistant. Panels remain available from View.", ""},
    {Id::SaveLayout, "Save Layout", "Save this project's editor panel arrangement and ribbon preferences.", ""},
    {Id::BookmarkStore1, "Store Camera", "Store the editor camera in bookmark 1.", ""},
    {Id::BookmarkRecall1, "Recall Camera", "Restore the editor camera from bookmark 1.", ""},
    {Id::ScriptNew, "New Script", "Create an unsaved Luau document.", ""},
    {Id::ScriptOpen, "Open Script", "Choose a Luau script from the project.", ""},
    {Id::ScriptSave, "Save Script", "Save the active script, preserving disk conflicts.", "Ctrl+S"},
    {Id::ScriptSaveReload, "Save + Reload", "Save the active script and request a protected module reload.", ""},
    {Id::ScriptSaveAll, "Save All Scripts", "Save all changed script documents.", ""},
    {Id::ScriptCheck, "Syntax Check", "Check the active script and show syntax diagnostics.", ""},
    {Id::ScriptFind, "Find", "Find text in the active script.", "Ctrl+F"},
    {Id::ScriptGoToLine, "Go to Line", "Move to a line in the active script.", "Ctrl+G"},
    {Id::ScriptComment, "Comment", "Toggle the comment on the current script line.", ""},
    {Id::ScriptFormat, "Format", "Format the active script with StyLua.", ""},
    {Id::ScriptReloadDisk, "Reload Disk", "Review reloading the active script from disk.", ""},
    {Id::ScriptOutput, "Output", "Show script output and diagnostics.", ""},
    {Id::ScriptCommand, "Command", "Show the Luau command console for the current VM.", ""},
    {Id::ScriptState, "State", "Inspect the selected behavior's current fields and state.", ""},
    {Id::ScriptErrors, "Errors", "Show the last script error and its recorded traceback.", ""},
    {Id::AddComponent, "Add Component", "Choose a component for the selected entity.", ""},
    {Id::StartNetworkTest, "Start Network Test", "Launch a dedicated server and the configured client windows from the current scene snapshot.", ""},
    {Id::StopNetworkTest, "Stop Network Test", "Stop the dedicated server and all network test clients.", ""},
});

constexpr bool definitionsAreOrdered() {
    if (kDefinitions.size() != static_cast<std::size_t>(Id::Count)) { return false; }
    for (std::size_t i = 0; i < kDefinitions.size(); ++i) {
        if (static_cast<std::size_t>(kDefinitions[i].id) != i) { return false; }
    }
    return true;
}
static_assert(definitionsAreOrdered());
}

void EditorCommandRegistry::setHandlers(Query query, Action action) {
    m_query = std::move(query);
    m_action = std::move(action);
}

const EditorCommandDefinition* EditorCommandRegistry::definition(const EditorCommandId id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    return index < kDefinitions.size() ? &kDefinitions[index] : nullptr;
}

std::span<const EditorCommandDefinition> EditorCommandRegistry::definitions() noexcept {
    return kDefinitions;
}

CommandState EditorCommandRegistry::state(const EditorCommandId id) const {
    if (!definition(id)) { return {false, false, "Unknown editor command."}; }
    if (!m_query || !m_action) { return {false, false, "This editor command is not connected."}; }
    return m_query(id);
}

bool EditorCommandRegistry::execute(const EditorCommandId id) const {
    if (!state(id).enabled || !m_action) { return false; }
    m_action(id);
    return true;
}

}
