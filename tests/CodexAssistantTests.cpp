#include "engine/editor/CodexAssistant.h"

#include <iostream>
#include <chrono>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>

namespace {
using engine::editor::CodexAssistant;
using Access = engine::editor::CodexAssistantTestAccess;
void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void testPromptClipboard() {
    ImGui::CreateContext();
    struct Cleanup { ~Cleanup() { ImGui::DestroyContext(); } } cleanup;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0F, 800.0F);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::string clipboard = "print(\"Hello\")\n-- Привет";
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = &clipboard;
    platform.Platform_GetClipboardTextFn = [](ImGuiContext* ctx) {
        return static_cast<std::string*>(ctx->PlatformIO.Platform_ClipboardUserData)->c_str();
    };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext* ctx, const char* text) {
        *static_cast<std::string*>(ctx->PlatformIO.Platform_ClipboardUserData) = text;
    };
    CodexAssistant assistant;
    ImVec2 dockSize(360.0F, 760.0F);
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
        ImGui::SetNextWindowSize(dockSize, ImGuiCond_Always);
        assistant.render();
        ImGui::Render();
    };
    for (int index = 0; index < 3; ++index) frame();
    const auto findPrompt = [] {
        const auto* assistantWindow = ImGui::FindWindowByName("Assistant");
        ImGuiWindow* result = nullptr;
        for (auto* window : ImGui::GetCurrentContext()->Windows) {
            if (!window->Active || std::string_view(window->Name).find("/##Prompt") == std::string_view::npos) continue;
            for (auto* parent = window->ParentWindow; parent; parent = parent->ParentWindow) {
                if (parent != assistantWindow) continue;
                require(result == nullptr, "Multiple prompt inputs were rendered");
                result = window;
                break;
            }
        }
        require(result != nullptr, "Prompt input was not rendered");
        return result;
    };
    auto* promptWindow = findPrompt();
    require(promptWindow->InnerClipRect.GetWidth() > 20.0F && promptWindow->InnerClipRect.GetHeight() > 20.0F,
        "Prompt input was clipped outside the assistant dock");
    const auto click = promptWindow->InnerClipRect.GetCenter();
    io.AddMousePosEvent(click.x, click.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    require(ImGui::GetActiveID() == promptWindow->ChildId, "Click did not focus the actual multiline prompt");
    auto shortcut = [&](const ImGuiKey key) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
    };
    const auto original = clipboard;
    shortcut(ImGuiKey_V);
    require(Access::prompt(assistant) == original, "Ctrl+V must paste multiline UTF-8 before connecting");
    clipboard = "previous clipboard content";
    shortcut(ImGuiKey_A);
    shortcut(ImGuiKey_C);
    require(clipboard == original, "Ctrl+C must copy selected prompt text");
    shortcut(ImGuiKey_X);
    require(Access::prompt(assistant).empty(), "Ctrl+X must cut selected prompt text");
    Access::receive(assistant, R"({"method":"turn/started","params":{"turn":{"id":"turn1"}}})");
    shortcut(ImGuiKey_V);
    require(Access::prompt(assistant) == original, "Prompt must remain editable during an active turn");
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    require(Access::prompt(assistant) == original + "\n", "Enter must insert a prompt newline without sending");
    shortcut(ImGuiKey_Z);
    require(Access::prompt(assistant) == original, "Prompt undo did not restore the text before Enter");
    dockSize = ImVec2(320.0F, 360.0F);
    frame();
    promptWindow = findPrompt();
    require(promptWindow->InnerClipRect.GetWidth() > 20.0F && promptWindow->InnerClipRect.GetHeight() > 20.0F,
        "Compact Assistant clipped the prompt input");
    std::string longPrompt;
    for (int line = 0; line < 40; ++line) longPrompt += "-- Привет " + std::to_string(line) + "\n";
    clipboard = longPrompt;
    shortcut(ImGuiKey_A);
    shortcut(ImGuiKey_V);
    require(Access::prompt(assistant) == longPrompt && promptWindow->Scroll.y > 0.0F,
        "Multiline paste did not retain and scroll the complete compact prompt");
    shortcut(ImGuiKey_A);
    shortcut(ImGuiKey_C);
    require(clipboard == longPrompt, "Copy lost offscreen prompt lines after dock resize");
    shortcut(ImGuiKey_X);
    require(Access::prompt(assistant).empty(), "Cut did not clear all selected offscreen prompt lines");
    shortcut(ImGuiKey_V);
    require(Access::prompt(assistant) == longPrompt, "Clipboard lifetime was lost after cutting the compact prompt");
    require(Access::takeOutgoing(assistant).empty(), "Clipboard editing must not send any protocol requests");
}
void testHandshakeAndConfiguredModel() {
    CodexAssistant assistant;
    Access::initialize(assistant);
    auto outgoing = Access::takeOutgoing(assistant);
    const auto init = nlohmann::json::parse(outgoing.front());
    require(init["method"] == "initialize", "initialize must be the first protocol request");
    Access::receive(assistant, nlohmann::json{{"id", init["id"]}, {"result", nlohmann::json::object()}}.dump());
    require(assistant.connected(), "successful handshake should connect the client");
    outgoing = Access::takeOutgoing(assistant);
    require(outgoing.size() == 5U, "handshake should acknowledge and read account, config, model catalog, and requirements");
    require(nlohmann::json::parse(outgoing[0])["method"] == "initialized", "initialized must precede further requests");
    const auto account = nlohmann::json::parse(outgoing[1]);
    require(account["method"] == "account/read" && account["params"]["refreshToken"] == false, "account check must not force token refresh");
    const auto config = nlohmann::json::parse(outgoing[2]);
    const auto models = nlohmann::json::parse(outgoing[3]);
    require(nlohmann::json::parse(outgoing[4])["method"] == "configRequirements/read", "managed policy requirements must be read without changing them");
    require(config["method"] == "config/read" && models["method"] == "model/list", "Client must inspect runtime settings and models without overriding them");
    Access::receive(assistant, nlohmann::json{{"id", config["id"]}, {"result", {{"config", {{"model", "gpt-6.1-sol"}, {"model_reasoning_effort", "ultra"}}}}}}.dump());
    Access::receive(assistant, nlohmann::json{{"id", models["id"]}, {"result", {{"data", nlohmann::json::array({{{"model", "gpt-6.1-sol"}}})}, {"nextCursor", nullptr}}}}.dump());
    require(Access::hasModel(assistant, "gpt-6.1-sol") && !Access::hasModel(assistant, "missing"), "Model catalog must retain the runtime's model identifiers");
    Access::submit(assistant, "Explain the script API");
    outgoing = Access::takeOutgoing(assistant);
    const auto thread = nlohmann::json::parse(outgoing.front());
    require(thread["params"]["sandbox"] == "read-only", "assistant must default to read-only access");
    require(thread["params"]["approvalPolicy"] == "untrusted" && thread["params"]["approvalsReviewer"] == "user", "approvals must route to the human user");
    require(!thread["params"].contains("model") && !thread["params"].contains("effort"), "thread must inherit the configured model and effort");
    Access::receive(assistant, nlohmann::json{{"id", thread["id"]}, {"result", {{"thread", {{"id", "root-thread"}}}}}}.dump());
    outgoing = Access::takeOutgoing(assistant);
    const auto turn = nlohmann::json::parse(outgoing.front());
    require(turn["method"] == "turn/start" && turn["params"]["sandboxPolicy"]["type"] == "readOnly", "turn must retain read-only access");
    require(!turn["params"].contains("model") && !turn["params"].contains("effort"), "turn must inherit the configured model and effort");
    Access::receive(assistant, R"({"method":"turn/completed","params":{"threadId":"child-thread","turn":{"status":"completed"}}})");
    require(assistant.busy(), "child-thread completion must not clear the root turn");
}
void testSessionSettingsAndCatalog() {
    using Json = nlohmann::json;
    CodexAssistant assistant;
    Access::initialize(assistant);
    auto outgoing = Access::takeOutgoing(assistant);
    const auto init = Json::parse(outgoing.front());
    Access::receive(assistant, Json{{"id", init["id"]}, {"result", Json::object()}}.dump());
    outgoing = Access::takeOutgoing(assistant);
    const auto config = Json::parse(outgoing[2]);
    const auto catalog = Json::parse(outgoing[3]);
    const auto requirements = Json::parse(outgoing[4]);
    require(catalog["params"]["includeHidden"] == true, "configured hidden models must remain resolvable");
    Access::receive(assistant, Json{{"id", config["id"]}, {"result", {{"config", {{"model", "configured-model"}, {"model_reasoning_effort", "ultra"}}}}}}.dump());
    Access::receive(assistant, Json{{"id", requirements["id"]}, {"result", {{"requirements", nullptr}}}}.dump());
    const auto model = [](const char* value, const char* defaultEffort, Json efforts) {
        return Json{{"id", std::string("catalog-id-") + value}, {"model", value}, {"displayName", value},
                    {"defaultReasoningEffort", defaultEffort}, {"supportedReasoningEfforts", std::move(efforts)}};
    };
    const auto options = [](std::initializer_list<const char*> values) {
        Json result = Json::array();
        for (const auto* value : values) result.push_back({{"reasoningEffort", value}, {"description", "runtime option"}});
        return result;
    };
    const auto configured = model("configured-model", "medium", options({"medium", "ultra"}));
    const auto fast = model("fast-model", "low", options({"low", "medium"}));
    Access::receive(assistant, Json{{"id", catalog["id"]}, {"result", {{"data", Json::array({configured})}, {"nextCursor", "page-2"}}}}.dump());
    require(!Access::selectModel(assistant, "configured-model"), "settings must wait for complete catalog pagination");
    outgoing = Access::takeOutgoing(assistant);
    require(outgoing.size() == 1U, "catalog pagination should request exactly the next page");
    const auto page = Json::parse(outgoing.front());
    require(page["method"] == "model/list" && page["params"]["cursor"] == "page-2", "catalog cursor must be passed unchanged");
    Access::receive(assistant, Json{{"id", page["id"]}, {"result", {{"data", Json::array({configured, fast})}, {"nextCursor", nullptr}}}}.dump());
    require(Access::selectModel(assistant, "fast-model"), "model must be selected by runtime model identifier, not catalog id");
    require(!Access::selectModel(assistant, "catalog-id-fast-model") && !Access::selectModel(assistant, "invented-model"), "unknown aliases and model names must be rejected");
    require(Access::efforts(assistant) == std::vector<std::string>({"low", "medium"}), "efforts must come only from the selected model");
    require(!Access::selectEffort(assistant, "ultra") && Access::selectEffort(assistant, "medium"), "unsupported model effort must be rejected");
    require(Access::selectApprovals(assistant, "on-request", "auto_review"), "session must support Codex automatic approval review");
    require(!Access::selectApprovals(assistant, "always", "user") && !Access::selectApprovals(assistant, "never", "unknown"), "invalid approval choices must be rejected");
    Access::submit(assistant, "Explain my script");
    outgoing = Access::takeOutgoing(assistant);
    require(outgoing.size() == 1U, "settings changes must not write config or start model calls until Send");
    const auto thread = Json::parse(outgoing.front());
    require(thread["method"] == "thread/start" && thread["params"]["model"] == "fast-model", "new chat must use selected model");
    require(thread["params"]["config"]["model_reasoning_effort"] == "medium", "new chat must override incompatible configured effort only for this thread");
    require(thread["params"]["sandbox"] == "read-only" && thread["params"]["approvalPolicy"] == "on-request" && thread["params"]["approvalsReviewer"] == "auto_review",
            "reviewer selection must not grant write or network access");
    require(!Access::selectModel(assistant, "configured-model") && !Access::selectEffort(assistant, "low") && !Access::selectApprovals(assistant, "never", "user"),
            "settings must not change while a turn is starting or running");
    Access::receive(assistant, Json{{"id", thread["id"]}, {"result", {{"thread", {{"id", "settings-thread"}}}, {"model", "fast-model"}, {"reasoningEffort", "low"},
                                                                 {"approvalPolicy", "on-request"}, {"approvalsReviewer", "auto_review"}}}}.dump());
    outgoing = Access::takeOutgoing(assistant);
    const auto turn = Json::parse(outgoing.front());
    require(turn["params"]["model"] == "fast-model" && turn["params"]["effort"] == "medium", "turn must send selected model and supported effort");
    require(turn["params"]["sandboxPolicy"]["type"] == "readOnly" && turn["params"]["sandboxPolicy"]["networkAccess"] == false, "approval policy must preserve the sandbox");
    Access::receive(assistant, R"({"method":"thread/settings/updated","params":{"threadId":"settings-thread","threadSettings":{"model":"fast-model","effort":"medium","approvalPolicy":"on-request","approvalsReviewer":"auto_review"}}})");
    require(Access::settings(assistant).find("Last confirmed: fast-model | effort: medium") != std::string::npos, "panel must show server-confirmed settings");
    Access::receive(assistant, R"({"method":"turn/completed","params":{"threadId":"settings-thread","turn":{"status":"completed"}}})");
    require(Access::selectModel(assistant, "configured-model"), "idle chat must permit a model switch");
    require(Access::selectApprovals(assistant, "never", "user"), "never-ask policy must be selectable explicitly");
    Access::submit(assistant, "Continue");
    outgoing = Access::takeOutgoing(assistant);
    const auto switched = Json::parse(outgoing.front());
    require(outgoing.size() == 1U && switched["method"] == "turn/start" && switched["params"]["threadId"] == "settings-thread", "switching explicit models must retain the conversation");
    require(switched["params"]["model"] == "configured-model" && switched["params"]["effort"] == "medium", "model switch must use catalog default rather than stale or configured effort");
    require(switched["params"]["approvalPolicy"] == "never" && switched["params"]["sandboxPolicy"]["type"] == "readOnly", "never-ask must keep restricted access");
    Access::receive(assistant, R"({"method":"turn/completed","params":{"turn":{"status":"completed"}}})");
    require(Access::selectModel(assistant, ""), "configuration inheritance must be restorable");
    Access::submit(assistant, "Use configured settings");
    outgoing = Access::takeOutgoing(assistant);
    const auto reset = Json::parse(outgoing.front());
    require(reset["method"] == "turn/start" && reset["params"]["threadId"] == "settings-thread" && reset["params"]["model"] == "configured-model" && reset["params"]["effort"] == "ultra",
            "restoring known config must replace sticky overrides while preserving the conversation");
}

void testManagedApprovalsAndStaleCatalog() {
    using Json = nlohmann::json;
    CodexAssistant assistant;
    Access::initialize(assistant);
    auto outgoing = Access::takeOutgoing(assistant);
    Access::receive(assistant, Json{{"id", Json::parse(outgoing.front())["id"]}, {"result", Json::object()}}.dump());
    outgoing = Access::takeOutgoing(assistant);
    const auto catalog = Json::parse(outgoing[3]);
    const auto requirements = Json::parse(outgoing[4]);
    Access::receive(assistant, Json{{"id", catalog["id"]}, {"result", {{"data", Json::array({{{"model", "temporary"}, {"defaultReasoningEffort", "low"},
        {"supportedReasoningEfforts", Json::array({{{"reasoningEffort", "low"}}})}}})}, {"nextCursor", nullptr}}}}.dump());
    require(Access::selectModel(assistant, "temporary"), "initial catalog model should be selectable");
    Access::receive(assistant, Json{{"id", requirements["id"]}, {"result", {{"requirements", {{"allowedApprovalPolicies", Json::array({"on-request"})}}}}}}.dump());
    require(!Access::selectApprovals(assistant, "never", "user"), "managed approval allowlist must be respected");
    Access::submit(assistant, "Draft");
    require(!assistant.busy() && Access::takeOutgoing(assistant).empty() && Access::prompt(assistant) == "Draft", "blocked default policy must preserve the draft without starting a turn");
    require(Access::selectApprovals(assistant, "on-request", "user"), "allowed managed policy should be selectable");
    Access::receive(assistant, R"({"method":"account/updated","params":{}})");
    outgoing = Access::takeOutgoing(assistant);
    require(outgoing.size() == 2U, "account changes must refresh account and model capabilities");
    const auto staleRefresh = Json::parse(outgoing[1]);
    Access::receive(assistant, R"({"method":"account/updated","params":{}})");
    outgoing = Access::takeOutgoing(assistant);
    const auto refresh = Json::parse(outgoing[1]);
    Access::receive(assistant, Json{{"id", refresh["id"]}, {"result", {{"data", Json::array()}, {"nextCursor", nullptr}}}}.dump());
    Access::receive(assistant, Json{{"id", staleRefresh["id"]}, {"result", {{"data", Json::array({{{"model", "temporary"}}})}, {"nextCursor", nullptr}}}}.dump());
    require(!Access::hasModel(assistant, "temporary"), "late model replies from a previous account/catalog generation must be ignored");
    Access::submit(assistant, "Draft");
    require(!assistant.busy() && Access::takeOutgoing(assistant).empty() && Access::prompt(assistant) == "Draft", "removed catalog model must not start inference or discard the draft");
    require(Access::status(assistant).find("no longer") != std::string::npos, "stale model error must be visible");
    require(Access::selectModel(assistant, ""), "user must be able to restore config when an explicit model disappears");
    Access::submit(assistant, "Use Codex defaults");
    outgoing = Access::takeOutgoing(assistant);
    const auto reset = Json::parse(outgoing.front());
    require(reset["method"] == "thread/start" && !reset["params"].contains("model") && !reset["params"].contains("config"),
            "unknown config defaults must be inherited in a fresh conversation, without inventing a model or effort");
}

void testRestoreUnknownEffortDefault() {
    using Json = nlohmann::json;
    CodexAssistant assistant;
    Access::initialize(assistant);
    auto outgoing = Access::takeOutgoing(assistant);
    Access::receive(assistant, Json{{"id", Json::parse(outgoing.front())["id"]}, {"result", Json::object()}}.dump());
    outgoing = Access::takeOutgoing(assistant);
    const auto config = Json::parse(outgoing[2]);
    const auto catalog = Json::parse(outgoing[3]);
    Access::receive(assistant, Json{{"id", config["id"]}, {"result", {{"config", {{"model", "configured-model"}, {"model_reasoning_effort", nullptr}}}}}}.dump());
    Access::receive(assistant, Json{{"id", catalog["id"]}, {"result", {{"data", Json::array({{{"model", "configured-model"}, {"defaultReasoningEffort", "medium"},
        {"supportedReasoningEfforts", Json::array({{{"reasoningEffort", "medium"}}, {{"reasoningEffort", "high"}}})}}})}, {"nextCursor", nullptr}}}}.dump());
    require(Access::selectEffort(assistant, "high"), "known model must allow explicit effort even if config default is unknown");
    Access::submit(assistant, "Explain");
    outgoing = Access::takeOutgoing(assistant);
    const auto thread = Json::parse(outgoing.front());
    Access::receive(assistant, Json{{"id", thread["id"]}, {"result", {{"thread", {{"id", "unknown-effort-thread"}}}, {"model", "configured-model"},
                                                                  {"reasoningEffort", "high"}}}}.dump());
    outgoing = Access::takeOutgoing(assistant);
    require(Json::parse(outgoing.front())["params"]["effort"] == "high", "explicit effort must be applied to the turn");
    Access::receive(assistant, R"({"method":"turn/completed","params":{"turn":{"status":"completed"}}})");
    require(Access::selectEffort(assistant, ""), "unknown configured effort must be restorable");
    require(Access::settings(assistant).find("Last confirmed") == std::string::npos, "reset conversation must clear previous server-confirmed settings");
    Access::submit(assistant, "Use configured effort");
    outgoing = Access::takeOutgoing(assistant);
    const auto reset = Json::parse(outgoing.front());
    require(reset["method"] == "thread/start" && !reset["params"].contains("config") && !reset["params"].contains("model"),
            "restoring null configured effort must start fresh instead of keeping sticky effort or inventing a default");
}
void testUnicodeRequestsAndInvalidEncoding() {
    CodexAssistant assistant;
    Access::initialize(assistant);
    auto outgoing = Access::takeOutgoing(assistant);
    const auto init = nlohmann::json::parse(outgoing.front());
    Access::receive(assistant, nlohmann::json{{"id", init["id"]}, {"result", nlohmann::json::object()}}.dump());
    Access::takeOutgoing(assistant);
    const std::filesystem::path root = std::filesystem::current_path() / std::filesystem::path(u8"Мой проект 😀");
    assistant.setProjectRoot(root);
    const auto bytes = root.generic_u8string();
    const std::string expectedRoot(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    assistant.setContext(std::string(16383U, 'a') + "😀tail");
    Access::submit(assistant, "Привет, создай интерфейс");
    outgoing = Access::takeOutgoing(assistant);
    require(outgoing.size() == 1U, "Unicode request must start a conversation");
    const auto thread = nlohmann::json::parse(outgoing.front());
    require(thread["params"]["cwd"] == expectedRoot, "Conversation cwd must use UTF-8 for Cyrillic and emoji");
    Access::receive(assistant, nlohmann::json{{"id", thread["id"]}, {"result", {{"thread", {{"id", "unicode-thread"}}}}}}.dump());
    outgoing = Access::takeOutgoing(assistant);
    require(outgoing.size() == 1U, "Truncated Unicode context must still start a turn");
    const auto turn = nlohmann::json::parse(outgoing.front());
    require(turn["params"]["cwd"] == expectedRoot, "Turn cwd must use UTF-8");
    const auto text = turn["params"]["input"][0]["text"].get<std::string>();
    require(text.ends_with(std::string(16383U, 'a')) && text.find("😀") == std::string::npos,
            "Context truncation must remove the entire symbol at its byte limit");
    Access::receive(assistant, R"({"method":"turn/completed","params":{"turn":{"status":"completed"}}})");
    assistant.setContext({});
    const std::string invalid = std::string("bad") + static_cast<char>(0xFF);
    Access::submit(assistant, invalid);
    require(!assistant.busy() && Access::takeOutgoing(assistant).empty(), "Invalid UTF-8 must be rejected without a model request");
    require(Access::prompt(assistant) == invalid, "Encoding error must preserve the draft");
    require(Access::status(assistant).find("invalid UTF-8") != std::string::npos, "Encoding failure must be visible in the assistant");
    assistant.setContext(invalid);
    Access::submit(assistant, "valid draft");
    require(!assistant.busy() && Access::takeOutgoing(assistant).empty(), "Invalid script context must not crash the editor");
    require(Access::prompt(assistant) == "valid draft", "Invalid script context must preserve the user message");
    assistant.setContext("исправленный скрипт");
    Access::submit(assistant, "valid draft");
    require(assistant.busy() && Access::takeOutgoing(assistant).size() == 1U, "Editor must recover after correcting the encoding");
}
void testDesktopRuntimeDiscovery() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("tge-codex-discovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(root), "Could not exclusively create the discovery fixture");
    struct Cleanup {
        fs::path root;
        ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); }
    } cleanup{root};
    require(Access::desktopBinary(root).empty(), "Empty cache must allow CLI fallback");
    fs::create_directories(root / "old-runtime");
    fs::create_directories(root / "current-runtime");
    fs::create_directories(root / "other-tool");
    const auto old = root / "old-runtime" / "codex.exe";
    const auto current = root / "current-runtime" / "codex.exe";
    std::ofstream(old).put('\n');
    std::ofstream(current).put('\n');
    std::ofstream(root / "other-tool" / "rg.exe").put('\n');
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(old, now - std::chrono::hours(24));
    fs::last_write_time(current, now);
    require(Access::desktopBinary(root) == current, "Discovery must prefer the newest desktop runtime and ignore other tools");
}
void testStreamingAndCompletion() {
    CodexAssistant assistant;
    Access::receive(assistant, R"({"method":"turn/started","params":{"turn":{"id":"turn1"}}})");
    require(assistant.busy(), "turn/started should mark the assistant busy");
    Access::receive(assistant, R"({"method":"item/agentMessage/delta","params":{"itemId":"msg1","delta":"Hello "}})");
    Access::receive(assistant, R"({"method":"item/agentMessage/delta","params":{"itemId":"msg1","delta":"world"}})");
    require(Access::transcript(assistant) == "Hello world\n", "stream deltas should append to the same item");
    Access::receive(assistant, R"({"method":"item/completed","params":{"item":{"type":"agentMessage","id":"msg1","text":"Authoritative final text"}}})");
    require(Access::transcript(assistant) == "Authoritative final text\n", "completed item should replace streamed content");
    Access::receive(assistant, R"({"method":"turn/completed","params":{"turn":{"status":"completed"}}})");
    require(!assistant.busy(), "turn completion should release the prompt");
}
void testExplicitApprovals() {
    CodexAssistant assistant;
    Access::receive(assistant, R"({"id":42,"method":"item/commandExecution/requestApproval","params":{"command":"echo test"}})");
    require(Access::approvalCount(assistant) == 1U, "approval should remain pending");
    require(Access::takeOutgoing(assistant).empty(), "commands must not be auto-approved");
    Access::decide(assistant, false);
    auto outgoing = Access::takeOutgoing(assistant);
    require(outgoing.size() == 1U, "one decision should be sent");
    const auto declined = nlohmann::json::parse(outgoing.front());
    require(declined["id"] == 42 && declined["result"]["decision"] == "decline", "decline must echo the server request id");
    Access::receive(assistant, R"({"id":"file1","method":"item/fileChange/requestApproval","params":{"reason":"Edit script"}})");
    Access::decide(assistant, true);
    outgoing = Access::takeOutgoing(assistant);
    const auto accepted = nlohmann::json::parse(outgoing.front());
    require(accepted["id"] == "file1" && accepted["result"]["decision"] == "accept", "explicit file approval should allow exactly one request");
    Access::receive(assistant, R"({"id":43,"method":"item/fileChange/requestApproval","params":{}})");
    Access::receive(assistant, R"({"method":"serverRequest/resolved","params":{"requestId":43}})");
    require(Access::approvalCount(assistant) == 0U, "resolved request should clear its controls");
}
void testUnsupportedRequestsFailClosed() {
    CodexAssistant assistant;
    Access::receive(assistant, R"({"id":7,"method":"item/permissions/requestApproval","params":{"permissions":{"network":true}}})");
    const auto outgoing = Access::takeOutgoing(assistant);
    require(outgoing.size() == 1U, "unsupported request should get a response");
    const auto denied = nlohmann::json::parse(outgoing.front());
    require(denied["id"] == 7 && denied.contains("error") && !denied.contains("result"), "unknown permissions must never be granted");
    Access::receive(assistant, "invalid JSON");
    Access::receive(assistant, R"({"method":"item/agentMessage/delta","params":{"delta":123,"itemId":false}})");
    require(Access::transcript(assistant) == "\n", "malformed field types should be ignored safely");
}
void testTranscriptBoundsAndDiffs() {
    CodexAssistant assistant;
    const std::string large(600U * 1024U, 'x');
    Access::receive(assistant, nlohmann::json{{"method", "item/agentMessage/delta"}, {"params", {{"itemId", "large"}, {"delta", large}}}}.dump());
    require(Access::transcript(assistant).size() <= 512U * 1024U + 1U, "transcript must remain bounded");
    Access::receive(assistant, R"json({"method":"item/started","params":{"item":{"id":"change1","type":"fileChange","changes":[{"path":"script.luau","diff":"+print(1)"}]}}})json");
    require(Access::transcript(assistant).find("script.luau\n+print(1)") != std::string::npos, "proposed diff should be reviewable before approval");
    Access::receive(assistant, R"({"id":8,"method":"item/commandExecution/requestApproval","params":{}})");
    Access::receive(assistant, R"({"method":"turn/completed","params":{"turn":{"status":"interrupted"}}})");
    require(Access::approvalCount(assistant) == 0U, "interruption should clear stale approval controls");
}
}

int main(const int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--local-transport-smoke") {
            CodexAssistant assistant;
            require(Access::connectLocalTransport(assistant), "native client could not start the locally installed Codex");
            for (int attempt = 0; attempt < 1000 && !assistant.connected(); ++attempt) {
                assistant.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(assistant.connected(), "native client initialize timed out");
            for (int attempt = 0; attempt < 1000; ++attempt) {
                assistant.update();
                if (Access::hasModel(assistant, "gpt-6.1-sol") && Access::selectModel(assistant, "gpt-6.1-sol")) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(Access::hasModel(assistant, "gpt-6.1-sol"), "selected runtime does not list the requested GPT-6.1 Sol model");
            const auto efforts = Access::efforts(assistant);
            require(!efforts.empty() && Access::selectEffort(assistant, efforts.front()), "runtime catalog must provide selectable reasoning efforts");
            std::cout << "Runtime: " << Access::executable(assistant) << "; catalog includes gpt-6.1-sol with " << efforts.size() << " reasoning effort choices.\n";
            assistant.shutdown();
            require(!assistant.connected(), "shutdown should clear the connection");
            std::cout << "Native C++ transport smoke passed. No login, thread, turn, or model call.\n";
            return 0;
        }
        testHandshakeAndConfiguredModel();
        testSessionSettingsAndCatalog();
        testManagedApprovalsAndStaleCatalog();
        testRestoreUnknownEffortDefault();
        testDesktopRuntimeDiscovery();
        testUnicodeRequestsAndInvalidEncoding();
        testPromptClipboard();
        testStreamingAndCompletion();
        testExplicitApprovals();
        testUnsupportedRequestsFailClosed();
        testTranscriptBoundsAndDiffs();
        std::cout << "Codex assistant protocol tests passed. No process, login, or model calls were made.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
