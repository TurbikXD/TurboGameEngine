#pragma once

#include "engine/editor/EditorCommands.h"
#include "engine/editor/EditorIcons.h"

namespace engine::editor {

enum class EditorRibbonTab { Home, Model, Test, View, Script };

struct EditorRibbonState {
    EditorRibbonTab tab{EditorRibbonTab::Home};
    bool compact{false};
    bool showLabels{true};
    EditorCommandId createCommand{EditorCommandId::CreatePhysicsCube};
    EditorCommandId playCommand{EditorCommandId::Play};
    float moveSnap{0.5F};
    float rotateSnap{15.0F};
    float scaleSnap{0.1F};
};

// Draws inside the caller's existing toolbar window. Commands go exclusively
// through the registry; only ribbon preferences are changed directly.
class EditorRibbon final {
public:
    // True means preferences changed and should be persisted by the host.
    bool render(EditorCommandRegistry& commands, EditorRibbonState& state, const EditorIcons::Resolver& resolveIcon = {});
    // Content height only. The toolbar host supplies zero window padding.
    [[nodiscard]] static float preferredHeight(const EditorRibbonState& state);
    // Also used when loading project preferences, before a GUI frame exists.
    static bool sanitizeState(EditorRibbonState& state) noexcept;
};

} // namespace engine::editor
