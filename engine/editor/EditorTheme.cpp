#include "engine/editor/EditorTheme.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace engine::editor {
namespace {
constexpr ImVec4 rgb(const unsigned int value, const float alpha = 1.0F) {
    return {static_cast<float>((value >> 16U) & 0xffU) / 255.0F,
        static_cast<float>((value >> 8U) & 0xffU) / 255.0F,
        static_cast<float>(value & 0xffU) / 255.0F, alpha};
}

// Static lifetime is required by the legacy ImGui atlas API.
constexpr ImWchar kEditorGlyphRanges[]{0x0020, 0x024f, 0x0370, 0x052f,
    0x2000, 0x206f, 0x20ac, 0x20ac, 0x2116, 0x2116, 0};
// The inspected Builder Sans fonts have no Greek/Cyrillic cmap entries. Keep
// their Latin and punctuation untouched; merge only these missing scripts.
constexpr ImWchar kLocalFallbackRanges[]{0x0370, 0x052f, 0x1f00, 0x1fff,
    0x2116, 0x2116, 0x2de0, 0x2dff, 0xa640, 0xa69f, 0};

std::vector<char> readFont(const std::filesystem::path& path) {
    constexpr std::streamoff maxBytes = 4 * 1024 * 1024;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) { return {}; }
    const std::streamoff size = input.tellg();
    if (size < 12 || size > maxBytes) { return {}; }
    std::vector<char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(size))) { return {}; }
    // The bundled font is TrueType; the optional local Builder font uses CFF
    // OpenType. Both sfnt variants are supported by the vendored stb loader.
    const bool trueType = static_cast<unsigned char>(bytes[0]) == 0 &&
        static_cast<unsigned char>(bytes[1]) == 1 && bytes[2] == 0 && bytes[3] == 0;
    if (!trueType && std::memcmp(bytes.data(), "OTTO", 4) != 0) { return {}; }
    return bytes;
}

ImFont* addFont(ImGuiIO& io, const std::vector<char>& bytes, const char* name,
    ImFont* mergeTarget = nullptr, const ImWchar* ranges = kEditorGlyphRanges,
    const float size = kEditorFontSize) {
    if (bytes.empty()) { return nullptr; }
    auto* ownedData = ImGui::MemAlloc(bytes.size());
    if (!ownedData) { return nullptr; }
    std::memcpy(ownedData, bytes.data(), bytes.size());
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = false;
    config.FontDataOwnedByAtlas = true;
    config.MergeMode = mergeTarget != nullptr;
    config.DstFont = mergeTarget;
    std::snprintf(config.Name, sizeof(config.Name), "%s, %.3gpx", name, static_cast<double>(size));
    return io.Fonts->AddFontFromMemoryTTF(ownedData, static_cast<int>(bytes.size()),
        size, &config, ranges);
}
} // namespace

void applyEditorTheme() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.FontSizeBase = kEditorFontSize;
    style.WindowPadding = {8.0F, 6.0F};
    style.FramePadding = {6.0F, 3.0F};
    style.CellPadding = {6.0F, 2.0F};
    style.ItemSpacing = {6.0F, 3.0F};
    style.ItemInnerSpacing = {4.0F, 2.0F};
    style.IndentSpacing = 16.0F;
    style.ScrollbarSize = 10.0F;
    style.GrabMinSize = 8.0F;
    style.WindowRounding = 0.0F;
    style.ChildRounding = 0.0F;
    style.FrameRounding = 0.0F;
    style.PopupRounding = 2.0F;
    style.ScrollbarRounding = 0.0F;
    style.GrabRounding = 0.0F;
    style.TabRounding = 0.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextPadding = {8.0F, 2.0F};
    style.DisabledAlpha = 0.45F;

    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = rgb(0xe1e1e1);
    colors[ImGuiCol_TextDisabled] = rgb(0x818181);
    // Surfaces/dividers sampled from the supplied Studio reference image.
    colors[ImGuiCol_WindowBg] = rgb(0x191a1f);
    colors[ImGuiCol_ChildBg] = rgb(0x191a1f);
    colors[ImGuiCol_PopupBg] = rgb(0x202227);
    colors[ImGuiCol_Border] = rgb(0x30333b);
    colors[ImGuiCol_BorderShadow] = rgb(0x000000, 0.0F);
    colors[ImGuiCol_FrameBg] = rgb(0x121215);
    colors[ImGuiCol_FrameBgHovered] = rgb(0x272930);
    colors[ImGuiCol_FrameBgActive] = rgb(0x30333b);
    colors[ImGuiCol_TitleBg] = rgb(0x202227);
    colors[ImGuiCol_TitleBgActive] = rgb(0x202227);
    colors[ImGuiCol_TitleBgCollapsed] = rgb(0x202227);
    colors[ImGuiCol_MenuBarBg] = rgb(0x272930);
    colors[ImGuiCol_ScrollbarBg] = rgb(0x191a1f);
    colors[ImGuiCol_ScrollbarGrab] = rgb(0x545454);
    colors[ImGuiCol_ScrollbarGrabHovered] = rgb(0x686868);
    colors[ImGuiCol_ScrollbarGrabActive] = rgb(0x7c7c7c);
    colors[ImGuiCol_CheckMark] = rgb(0xe1e1e1);
    colors[ImGuiCol_SliderGrab] = rgb(0x7c7c7c);
    colors[ImGuiCol_SliderGrabActive] = rgb(0xa0a0a0);
    colors[ImGuiCol_Button] = rgb(0x272930);
    colors[ImGuiCol_ButtonHovered] = rgb(0x30333b);
    colors[ImGuiCol_ButtonActive] = rgb(0x3a3d46);
    colors[ImGuiCol_Header] = rgb(0x30333b);
    colors[ImGuiCol_HeaderHovered] = rgb(0x353840);
    colors[ImGuiCol_HeaderActive] = rgb(0x3a3d46);
    colors[ImGuiCol_Separator] = rgb(0x30333b);
    colors[ImGuiCol_SeparatorHovered] = rgb(0x686868);
    colors[ImGuiCol_SeparatorActive] = rgb(0x3c64fa);
    colors[ImGuiCol_ResizeGrip] = rgb(0x686868, 0.15F);
    colors[ImGuiCol_ResizeGripHovered] = rgb(0xa0a0a0, 0.55F);
    colors[ImGuiCol_ResizeGripActive] = rgb(0x3c64fa);
    colors[ImGuiCol_Tab] = rgb(0x191a1f);
    colors[ImGuiCol_TabHovered] = rgb(0x30333b);
    colors[ImGuiCol_TabSelected] = rgb(0x272930);
    colors[ImGuiCol_TabSelectedOverline] = rgb(0x3c64fa);
    colors[ImGuiCol_TabDimmed] = rgb(0x191a1f);
    colors[ImGuiCol_TabDimmedSelected] = rgb(0x272930);
    colors[ImGuiCol_TabDimmedSelectedOverline] = rgb(0x686868);
    colors[ImGuiCol_PlotLines] = rgb(0xbbc2d1);
    colors[ImGuiCol_PlotLinesHovered] = rgb(0x3c64fa);
    colors[ImGuiCol_PlotHistogram] = rgb(0x80c878);
    colors[ImGuiCol_PlotHistogramHovered] = rgb(0x9bde92);
    colors[ImGuiCol_TableHeaderBg] = rgb(0x202227);
    colors[ImGuiCol_TableBorderStrong] = rgb(0x30333b);
    colors[ImGuiCol_TableBorderLight] = rgb(0x272930);
    colors[ImGuiCol_TableRowBg] = rgb(0x000000, 0.0F);
    colors[ImGuiCol_TableRowBgAlt] = rgb(0xffffff, 0.025F);
    colors[ImGuiCol_TextSelectedBg] = rgb(0x3c64fa, 0.40F);
    colors[ImGuiCol_DragDropTarget] = rgb(0x3c64fa);
    colors[ImGuiCol_NavCursor] = rgb(0x3c64fa);
    colors[ImGuiCol_NavWindowingHighlight] = rgb(0xe1e1e1, 0.70F);
    colors[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, 0.30F);
    colors[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.50F);
}

bool configureEditorFonts(ImGuiIO& io, const std::filesystem::path& assetRoot,
    const std::filesystem::path& localFontDirectory) {
    if (!io.Fonts || io.Fonts->Locked) { return false; }
    const auto regular = readFont(assetRoot / "fonts/SourceSansPro-Regular.ttf");
    if (regular.empty()) { return false; }
    const auto semibold = readFont(assetRoot / "fonts/SourceSansPro-Semibold.ttf");
    const auto code = readFont(assetRoot / "fonts/FiraMono-Regular.ttf");
    std::vector<char> localRegular;
    std::vector<char> localMedium;
    if (!localFontDirectory.empty()) {
        localRegular = readFont(localFontDirectory / "BuilderSans-Regular.otf");
        localMedium = readFont(localFontDirectory / "BuilderSans-Medium.otf");
    }
    const bool useLocal = !localRegular.empty() && !localMedium.empty();
    io.Fonts->Clear();
    io.FontDefault = useLocal ? addFont(io, localRegular, "Builder Sans local") :
        addFont(io, regular, "Source Sans Pro");
    if (!io.FontDefault) {
        ImFontConfig fallback;
        fallback.SizePixels = kEditorFontSize;
        io.FontDefault = io.Fonts->AddFontDefault(&fallback);
        return false;
    }
    if (useLocal) {
        addFont(io, regular, "Source Sans script fallback", io.FontDefault, kLocalFallbackRanges);
        if (auto* medium = addFont(io, localMedium, "Builder Sans Medium local")) {
            addFont(io, semibold.empty() ? regular : semibold, "Source Sans script fallback",
                medium, kLocalFallbackRanges);
        }
    } else if (!semibold.empty()) {
        addFont(io, semibold, "Source Sans Pro Semibold");
    }
    // Source Sans Pro's 1257-unit hhea span makes 15 stb pixels closely match
    // the native Qt application default of 9 pt / 12 px per em at 96 DPI.
    addFont(io, regular, "Source Sans Native");
    if (!semibold.empty()) { addFont(io, semibold, "Source Sans Native Semibold"); }
    if (!code.empty()) { addFont(io, code, "Fira Mono", nullptr, kEditorGlyphRanges, kCodeFontSize); }
    return true;
}

ImFont* codeFont() noexcept {
    if (!ImGui::GetCurrentContext()) { return nullptr; }
    auto& io = ImGui::GetIO();
    if (io.Fonts) {
        for (auto* font : io.Fonts->Fonts) {
            if (std::strncmp(font->GetDebugName(), "Fira Mono", 9) == 0) { return font; }
        }
    }
    return io.FontDefault;
}

ImFont* semiboldFont() noexcept {
    if (!ImGui::GetCurrentContext()) { return nullptr; }
    auto& io = ImGui::GetIO();
    if (io.Fonts) {
        for (auto* font : io.Fonts->Fonts) {
            const char* name = font->GetDebugName();
            if (std::strncmp(name, "Builder Sans Medium", 19) == 0 ||
                std::strncmp(name, "Source Sans Pro Semibold", 24) == 0) { return font; }
        }
    }
    return io.FontDefault;
}

ImFont* nativeFont() noexcept {
    if (!ImGui::GetCurrentContext()) { return nullptr; }
    auto& io = ImGui::GetIO();
    if (io.Fonts) {
        for (auto* font : io.Fonts->Fonts) {
            if (std::strncmp(font->GetDebugName(), "Source Sans Native", 18) == 0) { return font; }
        }
    }
    return io.FontDefault;
}

ImFont* nativeSemiboldFont() noexcept {
    if (!ImGui::GetCurrentContext()) { return nullptr; }
    auto& io = ImGui::GetIO();
    if (io.Fonts) {
        for (auto* font : io.Fonts->Fonts) {
            if (std::strncmp(font->GetDebugName(), "Source Sans Native Semibold", 26) == 0) { return font; }
        }
    }
    return nativeFont();
}

} // namespace engine::editor
