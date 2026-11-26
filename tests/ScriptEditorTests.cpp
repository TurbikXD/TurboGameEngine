#include <iostream>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif
#include "third_party/ImGuiColorTextEdit/TextEditor.h"

namespace {
void require(const bool valid, const char* message) { if (!valid) { throw std::runtime_error(message); } }
void drawEditor(TextEditor& editor, const bool focused = false) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = {1280.0F, 720.0F};
    io.DeltaTime = 1.0F / 60.0F;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0F, 0.0F});
    ImGui::SetNextWindowSize({1280.0F, 720.0F});
    if (focused) ImGui::SetNextWindowFocus();
    ImGui::Begin("Script editor regression");
    editor.Render("Luau Source", {1200.0F, 640.0F}, true);
    ImGui::End();
    ImGui::Render();
}
}

int main(int argc, char** argv) {
#if defined(_MSC_VER)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels{};
    int width{}, height{};
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    try {
        TextEditor editor;
        const std::string original = "local value = 3\nreturn value\n";
        editor.SetText(original);
        require(editor.GetText() == original, "Editor added a trailing newline on load");
        for (int i = 0; i < 5; ++i) { editor.SetText(editor.GetText()); }
        require(editor.GetText() == original, "Repeated save/load accumulated blank lines");
        editor.SetText("return 3");
        require(editor.GetText() == "return 3", "Editor changed a file without final newline");
        editor.SetText("return 3\r\n");
        require(editor.GetText() == "return 3\n", "CRLF normalization failed");
        editor.SetText(original);
        editor.SetSelection({0, 0}, {0, 15});
        editor.ReplaceSelection("--local value = 3");
        require(editor.GetText() == "--local value = 3\nreturn value\n", "Comment replacement duplicated source");
        require(editor.CanUndo(), "Toolbar replacement is not undoable");
        editor.Undo();
        require(editor.GetText() == original, "Undo did not restore original source");
        editor.Redo();
        require(editor.GetText().starts_with("--local"), "Redo did not restore comment");
        editor.SelectAll();
        editor.ReplaceSelection("local count: number = 3\n");
        editor.Undo();
        require(editor.GetText() == "--local value = 3\nreturn value\n", "Formatter undo lost source");
        editor.SetText("ab");
        editor.SetTabSize(0);
        require(editor.GetTabSize() == 1, "Zero tab width was accepted despite column arithmetic dividing by tab size");
        editor.SetTabSize(4);
        editor.SetSelection({0, 1}, {0, 1});
        editor.SetCursorPosition({0, 1});
        editor.ReplaceSelection("\t");
        require(editor.GetText() == "a\tb" && editor.GetCursorPosition().mColumn == 4,
                "Tab insertion left the cursor inside its visual column span");
        editor.Undo();
        require(editor.GetText() == "ab", "Undo did not remove the inserted tab");
        editor.Redo();
        require(editor.GetText() == "a\tb" && editor.GetCursorPosition().mColumn == 4,
                "Redo did not restore the tab and its cursor position");
        editor.SetImGuiChildIgnored(true);
        editor.SetText("\tvalue");
        editor.SetSelection({0, 4}, {0, 4});
        editor.SetCursorPosition({0, 4});
        drawEditor(editor, true);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
        drawEditor(editor, true);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
        drawEditor(editor, true);
        require(editor.GetText() == "value" && editor.GetCursorPosition().mColumn == 0,
                "Backspace did not return to the removed tab's real starting column");
        editor.Undo();
        require(editor.GetText() == "\tvalue" && editor.GetCursorPosition().mColumn == 4,
                "Undo moved a removed tab after the following text");
        editor.Redo();
        require(editor.GetText() == "value" && editor.GetCursorPosition().mColumn == 0,
                "Redo after tab Backspace corrupted text or cursor position");
        editor.SetImGuiChildIgnored(false);
        editor.SetText("");
        require(editor.GetText().empty(), "Empty file gained a newline");
        editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
        editor.SetText("-- UTF-8: \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xf0\x9f\x9a\x80\nlocal title = '\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0'\nreturn title\n");
        drawEditor(editor);
        drawEditor(editor);
        editor.SetText("local value = \"continued\\\ntext\"\nreturn value");
        drawEditor(editor);
        editor.SetText("local payload = \"" + std::string(32768U, 'x') + "\"\nreturn payload");
        drawEditor(editor);
        editor.SetText("local payload = '" + std::string(32768U, 'x') + "'\nreturn payload");
        drawEditor(editor);
        editor.SetText("local " + std::string(32768U, 'a') + " = 0\nreturn 1");
        drawEditor(editor);
        editor.SelectAll();
        editor.SetText("return 1");
        require(editor.GetSelectedText() == "return 1", "Live reload retained an out-of-range selection");
        drawEditor(editor);
        editor.SetTextLines({"a", "b", "c"});
        editor.SelectAll();
        editor.SetTextLines({"x"});
        require(editor.GetSelectedText() == "x", "SetTextLines retained an out-of-range selection");
        for (const std::string suffix : {std::string("\xc2"), std::string("\xe2\x82"), std::string("\xf0\x9f\x9a")}) {
            const std::string source = "local incomplete = " + suffix;
            editor.SetText(source);
            editor.SelectAll();
            require(editor.GetSelectedText() == source, "Truncated UTF-8 changed document bytes");
            drawEditor(editor);
            editor.ReplaceSelection("return 1");
            editor.Undo();
            require(editor.GetText() == source, "Undo corrupted a truncated UTF-8 document");
        }
        const auto assets = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(__FILE__).parent_path().parent_path() / "assets/scripts";
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assets)) {
            if (entry.path().extension() != ".luau") continue;
            std::ifstream file(entry.path(), std::ios::binary);
            const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            editor.SetText(source);
            for (int frame = 0; frame < editor.GetTotalLines() / 10 + 2; ++frame) drawEditor(editor);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ImGui::DestroyContext();
        return 1;
    }
    ImGui::DestroyContext();
    std::cout << "Script editor rendering, long Luau tokens, UTF-8, live replacement and undo regressions passed\n";
    return 0;
}
