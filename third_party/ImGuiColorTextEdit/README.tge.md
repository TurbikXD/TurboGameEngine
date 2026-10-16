Source: https://github.com/BalazsJako/ImGuiColorTextEdit
Pinned commit: ca2f9f1462e3b60e56351bc466acda448c5ea50d
License: MIT (see LICENSE).

TextEditor.cpp and TextEditor.h are vendored so the script editor also builds
offline. TurboGameEngine supplies a Luau language definition and its workspace
actions in engine/editor/ScriptWorkspace.cpp.

Local patch: ReplaceSelection records selection replacement as a single undo
operation for editor toolbar actions, without modifying the system clipboard.
GetText ends at the final actual line/column, so saving does not append another
blank line each time. Document baselines normalize CRLF through SetText.
