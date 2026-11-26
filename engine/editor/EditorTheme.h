#pragma once

#include <filesystem>

struct ImGuiIO;
struct ImFont;

namespace engine::editor {

inline constexpr float kEditorFontSize = 15.0F;
inline constexpr float kExplorerFontSize = 18.0F;
inline constexpr float kCodeFontSize = 16.0F;
inline constexpr float kPanelTitleHeight = 26.0F;
inline constexpr float kPanelRowHeight = 20.0F;
inline constexpr float kPropertyRowHeight = 24.0F;

void applyEditorTheme();

[[nodiscard]] bool configureEditorFonts(ImGuiIO& io, const std::filesystem::path& assetRoot,
    const std::filesystem::path& localFontDirectory = {});

[[nodiscard]] ImFont* codeFont() noexcept;
[[nodiscard]] ImFont* semiboldFont() noexcept;
[[nodiscard]] ImFont* nativeFont() noexcept;
[[nodiscard]] ImFont* nativeSemiboldFont() noexcept;

}
