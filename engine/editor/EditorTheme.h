#pragma once

#include <filesystem>

struct ImGuiIO;
struct ImFont;

namespace engine::editor {

inline constexpr float kEditorFontSize = 15.0F;
// Legacy Explorer's 14 px/em Source Sans role is 17.598 stb pixels, rounded.
// Keep this separate from native Qt panels and the Builder-based ribbon.
inline constexpr float kExplorerFontSize = 18.0F;
// Fira Mono hhea span is 1200 / 1000 units/em. Qt 10 pt at 96 DPI
// is 13 1/3 px per em, equivalent to 16 px in stb's ascent/descent size.
inline constexpr float kCodeFontSize = 16.0F;
inline constexpr float kPanelTitleHeight = 26.0F;
inline constexpr float kPanelRowHeight = 20.0F;
inline constexpr float kPropertyRowHeight = 24.0F;

// Apply once after creating the ImGui context. Sizes are logical pixels at 1x;
// the platform remains responsible for monitor/framebuffer scaling.
void applyEditorTheme();

// Call before the renderer creates the font texture and outside an ImGui frame.
// assetRoot is the engine's built-in assets directory, not the active project.
// Defaults to bundled OFL Source Sans Pro. An explicit localFontDirectory may
// supply BuilderSans-Regular.otf + BuilderSans-Medium.otf for this machine only;
// Source Sans fills their missing Greek/Cyrillic glyphs. No installed-version
// paths are discovered or persisted. Returns false and leaves existing fonts
// intact if the bundled regular fallback is absent or the atlas is locked.
[[nodiscard]] bool configureEditorFonts(ImGuiIO& io, const std::filesystem::path& assetRoot,
    const std::filesystem::path& localFontDirectory = {});

// Look up roles in the current context; no pointers survive atlas/context reset.
// Without configured role fonts return FontDefault; without a context return null.
[[nodiscard]] ImFont* codeFont() noexcept;
[[nodiscard]] ImFont* semiboldFont() noexcept;
// Source Sans Pro for native Qt-style panels, independent of ribbon overrides.
[[nodiscard]] ImFont* nativeFont() noexcept;
[[nodiscard]] ImFont* nativeSemiboldFont() noexcept;

} // namespace engine::editor
