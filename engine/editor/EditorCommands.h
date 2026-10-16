#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace engine::editor {

enum class EditorCommandId : std::uint16_t {
    ProjectSwitch, SaveScene, LoadScene, Undo, Redo,
    CreateEmpty, CreateRenderCube, CreatePhysicsCube, CreatePhysicsSphere, CreatePhysicsPyramid, CreateCamera,
    Duplicate, Delete, ToolSelect, ToolMove, ToolRotate, ToolScale, ToggleLocalSpace, ToggleSnap,
    FocusSelected, FrameScene, ResetTransform, DropToGround, ZeroVelocity, RandomizeTint,
    ToggleVisible, ToggleStatic, ToggleGravity, ToggleCollider,
    Play, PlayWindow, StopWindow, Pause, Stop, StepTick, ScriptDemo,
    ShowHierarchy, ShowInspector, ShowContent, ShowStats, ShowScriptWorkspace, ShowAssistant,
    ShowViewport, ShowStatus, ToggleColliderDebug, ToggleHoverOutline, ToggleLayoutLock,
    ResetLayout, SaveLayout, BookmarkStore1, BookmarkRecall1,
    ScriptNew, ScriptOpen, ScriptSave, ScriptSaveReload, ScriptSaveAll, ScriptCheck,
    ScriptFind, ScriptGoToLine, ScriptComment, ScriptFormat, ScriptReloadDisk,
    ScriptOutput, ScriptCommand, ScriptState, ScriptErrors,
    AddComponent,
    Count
};

struct CommandState {
    bool enabled{true};
    bool checked{false};
    std::string_view disabledReason;
};

struct EditorCommandDefinition {
    EditorCommandId id;
    std::string_view label;
    std::string_view tooltip;
    std::string_view shortcut;
};

// Presentation and shortcuts share these handlers. The owner of the scene/VM
// supplies state and actions; the registry never owns or mutates either one.
class EditorCommandRegistry final {
public:
    using Query = std::function<CommandState(EditorCommandId)>;
    using Action = std::function<void(EditorCommandId)>;

    void setHandlers(Query query, Action action);
    [[nodiscard]] CommandState state(EditorCommandId id) const;
    // Rechecks current availability, even if the caller already queried it.
    bool execute(EditorCommandId id) const;

    [[nodiscard]] static const EditorCommandDefinition* definition(EditorCommandId id) noexcept;
    [[nodiscard]] static std::span<const EditorCommandDefinition> definitions() noexcept;

private:
    Query m_query;
    Action m_action;
};

} // namespace engine::editor
