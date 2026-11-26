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

## Coordinates and rendering

`Coordinates` contains zero-based display line and column positions, rather than
byte offsets. Tabs advance to the next `mTabSize` stop. With a tab size of four,
the `B` in `\tABC` occupies display column five. `mTextStart` is the code line's
horizontal pixel offset relative to the editor's left edge, including the line
number gutter and margin. Rendering measures the current scaled font and applies
the current ImGui style alpha to the palette. Palette arrays follow the
`PaletteIndex` enum order.

## UTF-8 and syntax highlighting

`UTF8CharLength` distinguishes ASCII and leading bytes when advancing through
text; word-start traversal skips UTF-8 continuation bytes. The local UTF-8 patch
bounds advancement by the actual line length, including malformed and incomplete
text. `ImTextCharToUtf8` was adapted from ImGui's conversion helper.

Lua and Luau use the local linear `TokenizeLua` scanner. Do not replace it with
unbounded regular expressions: MSVC's recursive regex matcher can exhaust the
native stack on ordinary long literals and identifiers. The editor must accept
incomplete quoted strings while the user types. Quoted-string scanning handles
escapes, and a trailing escape may advance to the end of a line; the colorizer
checks the index before accessing the next glyph. This also supports escaped
newlines in Lua strings without an out-of-bounds access.

The comment pass tracks whether a line has a preceding non-whitespace character
and whether a final backslash continues it. A single-line comment prevents a
multiline-comment opener on that same line from starting another comment.
Language definitions retain their literal comment delimiters, identifier tables,
token callbacks and regex definitions; these are executable highlighting data.

Explanatory source comments and inactive debug snippets were removed from the
local fork. The MIT license remains in `LICENSE`.
