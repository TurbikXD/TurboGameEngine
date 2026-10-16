#include "engine/editor/CodexAssistant.h"
#include "engine/editor/EditorTheme.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#endif

namespace engine::editor {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxLine = 1024U * 1024U;
constexpr std::size_t kMaxTranscript = 512U * 1024U;
constexpr std::size_t kMaxQueue = 512U;
constexpr std::size_t kMaxPrompt = 16U * 1024U;

std::string pathUtf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string utf8Prefix(const std::string_view text, const std::size_t limit) {
    auto end = std::min(text.size(), limit);
    while (end > 0 && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    return std::string(text.substr(0, end));
}

std::filesystem::path discoverDesktopCodex(const std::filesystem::path& bin) {
    std::filesystem::path newest;
    auto newestTime = std::filesystem::file_time_type::min();
    const auto consider = [&](const std::filesystem::path& candidate) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error) return;
        const auto time = std::filesystem::last_write_time(candidate, error);
        if (!error && (newest.empty() || time > newestTime)) { newest = candidate; newestTime = time; }
    };
    consider(bin / "codex.exe");
    std::error_code error;
    std::filesystem::directory_iterator it(bin, error), end;
    while (!error && it != end) {
        if (it->is_directory(error) && !error) consider(it->path() / "codex.exe");
        it.increment(error);
    }
    return newest;
}

std::string stringField(const Json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

void appendBounded(std::string& text, const std::string_view addition, const std::size_t limit) {
    if (addition.size() >= limit) {
        text.assign(addition.substr(addition.size() - limit));
    } else {
        if (text.size() + addition.size() > limit) text.erase(0, text.size() + addition.size() - limit);
        text.append(addition);
    }
}

#if defined(_WIN32)
void closeHandle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

std::filesystem::path discoverCodex() {
    wchar_t buffer[32768]{};
    const DWORD localLength = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (localLength > 0 && localLength < 32768) {
        const auto desktop = discoverDesktopCodex(std::filesystem::path(buffer) / "OpenAI" / "Codex" / "bin");
        if (!desktop.empty()) return desktop;
    }
    const DWORD length = SearchPathW(nullptr, L"codex.exe", nullptr, 32768, buffer, nullptr);
    if (length > 0 && length < 32768) return buffer;
    const DWORD appDataLength = GetEnvironmentVariableW(L"APPDATA", buffer, 32768);
    if (appDataLength == 0 || appDataLength >= 32768) return {};
    const std::filesystem::path npm = std::filesystem::path(buffer) / "npm" / "node_modules" / "@openai";
#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr auto package = "codex-win32-arm64";
    constexpr auto target = "aarch64-pc-windows-msvc";
#else
    constexpr auto package = "codex-win32-x64";
    constexpr auto target = "x86_64-pc-windows-msvc";
#endif
    const std::filesystem::path candidates[] = {
        npm / "codex" / "node_modules" / "@openai" / package / "vendor" / target / "bin" / "codex.exe",
        npm / package / "vendor" / target / "bin" / "codex.exe",
        npm / "codex" / "vendor" / target / "bin" / "codex.exe"
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    return {};
}

bool openAuthUrl(const std::string& url) {
    // Only open the known OAuth browser destinations returned by the local server.
    if (!(url.starts_with("https://auth.openai.com/") || url.starts_with("https://chatgpt.com/") ||
          url.starts_with("https://auth.chatgpt.com/")) || url.size() > 8192U) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()), nullptr, 0);
    if (count <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()), wide.data(), count);
    return reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
#endif
} // namespace

struct CodexAssistant::Impl final {
    struct Message final { std::string id; std::string title; std::string text; };
    struct Approval final { Json id; std::string method; Json params; };
    struct Effort final { std::string value; std::string description; };
    struct Model final {
        std::string value;
        std::string name;
        std::string description;
        std::string defaultEffort;
        std::vector<Effort> efforts;
        bool hidden = false;
    };
    std::filesystem::path root = std::filesystem::current_path();
    std::string executable;
    std::string context;
    std::function<std::string()> contextProvider;
    std::string prompt;
    std::string status = "Connect the locally installed Codex CLI to start.";
    std::string account = "Not connected";
    std::string runtimeInfo;
    std::string configuredModel;
    std::string configuredEffort;
    std::vector<Model> models;
    std::unordered_set<std::string> modelCursors;
    std::unordered_map<int, unsigned> catalogRequests;
    unsigned catalogGeneration = 0;
    std::string selectedModel;
    std::string selectedEffort;
    bool restoreConfiguredModel = false;
    bool restoreConfiguredEffort = false;
    std::string approvalPolicy = "untrusted";
    std::string approvalsReviewer = "user";
    std::vector<std::string> allowedApprovalPolicies;
    bool approvalPolicyRestricted = false;
    bool catalogLoaded = false;
    bool catalogLoading = false;
    std::string effectiveModel;
    std::string effectiveEffort;
    std::string effectiveApprovalPolicy;
    std::string effectiveReviewer;
    std::string threadId;
    std::string turnId;
    std::string loginId;
    std::string loginUrl;
    std::string pendingPrompt;
    std::vector<Message> messages;
    std::deque<Approval> approvals;
    std::unordered_map<int, std::string> requests;
    int nextId = 1;
    bool ready = false;
    bool active = false;
    bool waitingForTurn = false;
    bool workspaceWrite = false;
    bool includeContext = true;
    bool scrollToBottom = false;
    bool protocolOnly = false;
    std::mutex queueMutex;
    std::deque<std::string> outgoing;
    std::deque<std::string> incoming;
    std::size_t outgoingBytes = 0;
    std::size_t incomingBytes = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> transportFailed{false};
    std::thread worker;
#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    HANDLE diagnostics = nullptr;
#endif

    Impl() {
#if defined(_WIN32)
        executable = pathUtf8(discoverCodex());
#endif
    }

    ~Impl() { stop(); }

    bool enqueue(const Json& message) {
        std::string encoded;
        try { encoded = message.dump() + '\n'; }
        catch (const Json::type_error&) {
            status = "Codex request contains invalid UTF-8. Check the message and script encoding.";
            return false;
        }
        std::lock_guard lock(queueMutex);
        if (outgoing.size() >= kMaxQueue || outgoingBytes + encoded.size() > kMaxLine) {
            status = "Codex outgoing queue is full. Cancel or reconnect.";
            return false;
        }
        outgoing.push_back(encoded);
        outgoingBytes += encoded.size();
        return true;
    }

    bool request(const std::string& method, Json params = Json::object()) {
        const int id = nextId++;
        if (!enqueue(Json{{"id", id}, {"method", method}, {"params", std::move(params)}})) return false;
        requests.emplace(id, method);
        if (method == "model/list") catalogRequests.emplace(id, catalogGeneration);
        return true;
    }

    void pushMessage(std::string title, std::string text, std::string id = {}) {
        if (messages.size() >= 128U) messages.erase(messages.begin());
        if (text.size() > kMaxTranscript) text.resize(kMaxTranscript);
        messages.push_back({std::move(id), std::move(title), std::move(text)});
        trimTranscript();
        scrollToBottom = true;
    }

    void trimTranscript() {
        std::size_t total = 0;
        for (const auto& message : messages) total += message.text.size();
        while (total > kMaxTranscript && messages.size() > 1U) {
            total -= messages.front().text.size();
            messages.erase(messages.begin());
        }
    }

    Message& agentMessage(const std::string& id) {
        const auto found = std::find_if(messages.begin(), messages.end(), [&](const Message& message) { return message.id == id; });
        if (found != messages.end()) return *found;
        pushMessage("Codex", {}, id);
        return messages.back();
    }

    void refreshAccount() { request("account/read", Json{{"refreshToken", false}}); }

    const Model* findModel(const std::string_view value) const {
        const auto found = std::find_if(models.begin(), models.end(), [&](const Model& model) { return model.value == value; });
        return found == models.end() ? nullptr : &*found;
    }

    const Model* currentModel() const { return findModel(selectedModel.empty() ? configuredModel : selectedModel); }

    static bool supportsEffort(const Model& model, const std::string_view value) {
        return std::any_of(model.efforts.begin(), model.efforts.end(), [&](const Effort& effort) { return effort.value == value; });
    }

    std::string nextEffort() const {
        if (!selectedEffort.empty()) return selectedEffort;
        if (selectedModel.empty()) return configuredEffort;
        const Model* model = currentModel();
        return model != nullptr && supportsEffort(*model, model->defaultEffort) ? model->defaultEffort : std::string{};
    }

    bool policyAllowed(const std::string_view policy) const {
        return !approvalPolicyRestricted || std::find(allowedApprovalPolicies.begin(), allowedApprovalPolicies.end(), policy) != allowedApprovalPolicies.end();
    }

    void clearConfirmedSettings() {
        effectiveModel.clear(); effectiveEffort.clear(); effectiveApprovalPolicy.clear(); effectiveReviewer.clear();
    }

    bool selectModel(const std::string_view model) {
        if (active || !ready || catalogLoading || (!model.empty() && findModel(model) == nullptr)) return false;
        if (selectedModel == model) return true;
        selectedModel = model;
        selectedEffort.clear();
        restoreConfiguredModel = model.empty();
        restoreConfiguredEffort = model.empty();
        // turn/start overrides persist on the server. Returning to config must re-inherit
        // both settings, even when config does not expose a concrete model/effort.
        const bool restart = model.empty() && (configuredModel.empty() || configuredEffort.empty());
        const Model* selected = currentModel();
        if (restart || (!model.empty() && selected != nullptr && !supportsEffort(*selected, selected->defaultEffort))) {
            threadId.clear(); clearConfirmedSettings();
        }
        status = restart ? "Codex defaults restored; next message starts a new conversation to clear previous overrides." : "Model changed for the next turn.";
        return true;
    }

    bool selectEffort(const std::string_view effort) {
        const Model* model = currentModel();
        if (active || !ready || catalogLoading || (!effort.empty() && (model == nullptr || !supportsEffort(*model, effort)))) return false;
        if (selectedEffort == effort) return true;
        selectedEffort = effort;
        restoreConfiguredEffort = effort.empty() && selectedModel.empty();
        const bool restart = restoreConfiguredEffort && configuredEffort.empty();
        if (restart) {
            threadId.clear(); clearConfirmedSettings();
        }
        status = restart ? "Codex effort default restored; next message starts a new conversation to clear the previous override." : "Reasoning effort changed for the next turn.";
        return true;
    }

    bool selectApprovals(const std::string_view policy, const std::string_view reviewer) {
        if (active || !ready || (policy != "untrusted" && policy != "on-request" && policy != "never") ||
            (reviewer != "user" && reviewer != "auto_review") || !policyAllowed(policy)) return false;
        approvalPolicy = policy;
        approvalsReviewer = reviewer;
        status = "Approval settings changed for the next turn; project access is unchanged.";
        return true;
    }

    void refreshModels() {
        ++catalogGeneration;
        models.clear(); modelCursors.clear(); catalogLoaded = false; catalogLoading = true;
        // Hidden entries remain valid for a model already selected in Codex config.
        // They are not offered as new selections in this client's picker.
        if (!request("model/list", Json{{"limit", 100}, {"includeHidden", true}})) catalogLoading = false;
    }

    void readEffectiveSettings(const Json& settings, const char* effortKey) {
        if (!settings.is_object()) return;
        const auto model = stringField(settings, "model");
        if (!model.empty()) effectiveModel = model;
        if (settings.contains(effortKey)) effectiveEffort = stringField(settings, effortKey);
        const auto policy = stringField(settings, "approvalPolicy");
        if (!policy.empty()) effectiveApprovalPolicy = policy;
        const auto reviewer = stringField(settings, "approvalsReviewer");
        if (!reviewer.empty()) effectiveReviewer = reviewer;
    }

    std::string settingsText() const {
        const auto model = selectedModel.empty() ? configuredModel : selectedModel;
        const auto effort = nextEffort();
        std::string text = "Next turn: " + (model.empty() ? std::string("Codex model default") : model) + " | effort: " +
                           (effort.empty() ? std::string("Codex default") : effort) + " | approvals: " + approvalPolicy + " / " + approvalsReviewer;
        if (!effectiveModel.empty()) {
            text += "\nLast confirmed: " + effectiveModel + " | effort: " + (effectiveEffort.empty() ? std::string("runtime default") : effectiveEffort);
            if (!effectiveApprovalPolicy.empty()) text += " | approvals: " + effectiveApprovalPolicy + " / " + effectiveReviewer;
        }
        return text;
    }

    void renderSettings() {
        ImGui::BeginDisabled(!ready || active || catalogLoading);
        const std::string modelLabel = selectedModel.empty() ? "Codex config: " + (configuredModel.empty() ? std::string("default") : configuredModel) : selectedModel;
        ImGui::SetNextItemWidth(-100.0F);
        if (ImGui::BeginCombo("Model", modelLabel.c_str())) {
            if (ImGui::Selectable("Use Codex configuration", selectedModel.empty())) selectModel({});
            for (const auto& model : models) {
                if (model.hidden && model.value != selectedModel && model.value != configuredModel) continue;
                ImGui::PushID(model.value.c_str());
                const std::string label = model.name.empty() || model.name == model.value ? model.value : model.name + " (" + model.value + ")";
                if (ImGui::Selectable(label.c_str(), model.value == selectedModel)) selectModel(model.value);
                if (ImGui::IsItemHovered() && !model.description.empty()) ImGui::SetTooltip("%s", model.description.c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        const Model* model = currentModel();
        const std::string defaultEffort = selectedModel.empty() ? configuredEffort : model != nullptr ? model->defaultEffort : std::string{};
        const std::string effortLabel = selectedEffort.empty() ? std::string(selectedModel.empty() ? "Codex config" : "Model default") +
            (defaultEffort.empty() ? "" : ": " + defaultEffort) : selectedEffort;
        ImGui::SetNextItemWidth(-100.0F);
        if (ImGui::BeginCombo("Effort", effortLabel.c_str())) {
            if (ImGui::Selectable(selectedModel.empty() ? "Use Codex configuration" : "Use model default", selectedEffort.empty())) selectEffort({});
            if (model != nullptr) for (const auto& effort : model->efforts) {
                if (ImGui::Selectable(effort.value.c_str(), effort.value == selectedEffort)) selectEffort(effort.value);
                if (ImGui::IsItemHovered() && !effort.description.empty()) ImGui::SetTooltip("%s", effort.description.c_str());
            }
            ImGui::EndCombo();
        }
        struct Policy { const char* value; const char* label; const char* description; };
        constexpr Policy policies[] = {
            {"untrusted", "Ask for untrusted commands", "Prompt for commands outside Codex's trusted set. Approval requests are reviewed by the selected reviewer."},
            {"on-request", "Ask when needed", "Codex decides when to request approval, including sandbox escalation."},
            {"never", "Never ask; deny escalation", "Do not request approval. Blocked actions fail. This does not grant filesystem or network access."}
        };
        const auto currentPolicy = std::find_if(std::begin(policies), std::end(policies), [&](const Policy& value) { return approvalPolicy == value.value; });
        ImGui::SetNextItemWidth(-100.0F);
        if (ImGui::BeginCombo("Approvals", currentPolicy != std::end(policies) ? currentPolicy->label : approvalPolicy.c_str())) {
            for (const auto& policy : policies) {
                ImGui::BeginDisabled(!policyAllowed(policy.value));
                if (ImGui::Selectable(policy.label, approvalPolicy == policy.value)) selectApprovals(policy.value, approvalsReviewer);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s%s", policy.description, policyAllowed(policy.value) ? "" : " Blocked by managed requirements.");
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(approvalPolicy == "never");
        ImGui::SetNextItemWidth(-100.0F);
        if (ImGui::BeginCombo("Reviewer", approvalsReviewer == "user" ? "Human (this panel)" : "Codex automatic review")) {
            if (ImGui::Selectable("Human (this panel)", approvalsReviewer == "user")) selectApprovals(approvalPolicy, "user");
            if (ImGui::Selectable("Codex automatic review", approvalsReviewer == "auto_review")) selectApprovals(approvalPolicy, "auto_review");
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Refresh models")) refreshModels();
        ImGui::EndDisabled();
        if (catalogLoading) ImGui::TextDisabled("Loading available models...");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reload model and effort options from this runtime and account.");
        ImGui::TextWrapped("%s", settingsText().c_str());
        if (catalogLoaded && !configuredModel.empty() && selectedModel.empty() && currentModel() == nullptr)
            ImGui::TextWrapped("This runtime does not list the configured model. Select an available model or reconnect with a newer runtime.");
        ImGui::TextDisabled("Session only. Project access is controlled separately below.");
    }

    bool initializeConnection() {
        return request("initialize", Json{{"clientInfo", Json{{"name", "turbo_game_engine"}, {"title", "TurboGameEngine"}, {"version", "0.1.0"}}}});
    }

    void beginTurn() {
        if (threadId.empty() || pendingPrompt.empty()) return;
        Json sandbox = workspaceWrite
            ? Json{{"type", "workspaceWrite"}, {"writableRoots", Json::array({pathUtf8(root)})}, {"networkAccess", false},
                   {"excludeSlashTmp", true}, {"excludeTmpdirEnvVar", true}}
            : Json{{"type", "readOnly"}, {"networkAccess", false}};
        Json params{{"threadId", threadId}, {"input", Json::array({Json{{"type", "text"}, {"text", pendingPrompt}}})},
                    {"cwd", pathUtf8(root)}, {"approvalPolicy", approvalPolicy}, {"approvalsReviewer", approvalsReviewer}, {"sandboxPolicy", std::move(sandbox)}};
        if (!selectedModel.empty()) params["model"] = selectedModel;
        else if (restoreConfiguredModel && !configuredModel.empty()) params["model"] = configuredModel;
        // A model switch chooses that model's catalog default instead of carrying
        // an incompatible effort from the previous model into the same thread.
        if (!selectedEffort.empty() || !selectedModel.empty() || restoreConfiguredEffort) {
            const auto effort = nextEffort();
            if (!effort.empty()) params["effort"] = effort;
        }
        if (request("turn/start", std::move(params))) {
            waitingForTurn = true;
            active = true;
            status = "Codex is working...";
            pendingPrompt.clear();
        } else {
            active = false;
            waitingForTurn = false;
        }
    }

    void sendPrompt() {
        if (!ready || active || prompt.empty()) return;
        if (!selectedModel.empty() && (catalogLoading || !catalogLoaded || currentModel() == nullptr)) {
            status = "Selected model is no longer in the runtime catalog. Refresh models or choose another model."; return;
        }
        if (!selectedEffort.empty() && (currentModel() == nullptr || !supportsEffort(*currentModel(), selectedEffort))) {
            status = "Selected effort is no longer supported by this model. Choose another effort."; return;
        }
        if (!selectedModel.empty() && selectedEffort.empty() && currentModel() != nullptr && !currentModel()->efforts.empty() && nextEffort().empty()) {
            status = "Runtime catalog has no supported default effort for this model. Choose an effort."; return;
        }
        if (!policyAllowed(approvalPolicy)) { status = "This approval policy is blocked by managed Codex requirements. Choose an allowed policy."; return; }
        if (prompt.size() > kMaxPrompt) { status = "Prompt is too long (maximum 16 KiB)."; return; }
        pendingPrompt = prompt;
        if (includeContext && contextProvider) context = utf8Prefix(contextProvider(), kMaxPrompt);
        if (includeContext && !context.empty()) pendingPrompt += "\n\nCurrent engine editor context:\n" + context;
        try { (void)Json(pendingPrompt).dump(); }
        catch (const Json::type_error&) {
            status = "Codex request contains invalid UTF-8. Check the message and script encoding.";
            pendingPrompt.clear();
            return;
        }
        active = true;
        if (threadId.empty()) {
            Json params{{"cwd", pathUtf8(root)}, {"sandbox", workspaceWrite ? "workspace-write" : "read-only"},
                        {"approvalPolicy", approvalPolicy}, {"approvalsReviewer", approvalsReviewer}};
            if (!selectedModel.empty()) params["model"] = selectedModel;
            if (!selectedEffort.empty() || !selectedModel.empty()) {
                const auto effort = nextEffort();
                if (!effort.empty()) params["config"] = Json{{"model_reasoning_effort", effort}};
            }
            if (!request("thread/start", std::move(params))) active = false;
        } else beginTurn();
        if (active) { pushMessage("You", prompt); prompt.clear(); }
        else pendingPrompt.clear();
    }

    void decide(const bool accept) {
        if (approvals.empty()) return;
        const Approval approval = approvals.front();
        if (enqueue(Json{{"id", approval.id}, {"result", Json{{"decision", accept ? "accept" : "decline"}}}})) approvals.pop_front();
    }

    void receive(const std::string_view line) {
        if (line.size() > kMaxLine) { status = "Codex protocol message exceeded the size limit."; return; }
        const Json message = Json::parse(line, nullptr, false);
        if (!message.is_object()) { status = "Ignored an invalid Codex protocol message."; return; }
        const std::string method = stringField(message, "method");
        const Json params = message.contains("params") && message["params"].is_object() ? message["params"] : Json::object();
        if (message.contains("id") && !method.empty()) {
            if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval") {
                if (approvals.size() < 32U) approvals.push_back({message["id"], method, params});
                else enqueue(Json{{"id", message["id"]}, {"result", Json{{"decision", "decline"}}}});
                status = "Codex needs your approval.";
            } else {
                // Fail closed for unsupported tools, elicitation, and permission requests.
                enqueue(Json{{"id", message["id"]}, {"error", Json{{"code", -32601}, {"message", "This editor does not support this request; no action was approved."}}}});
                status = "An unsupported Codex request was declined.";
            }
            return;
        }
        if (message.contains("id") && method.empty()) {
            if (!message["id"].is_number_integer()) return;
            const auto found = requests.find(message["id"].get<int>());
            if (found == requests.end()) return;
            const std::string requestMethod = found->second;
            requests.erase(found);
            if (requestMethod == "model/list") {
                const auto catalogRequest = catalogRequests.find(message["id"].get<int>());
                const bool stale = catalogRequest == catalogRequests.end() || catalogRequest->second != catalogGeneration;
                if (catalogRequest != catalogRequests.end()) catalogRequests.erase(catalogRequest);
                if (stale) return;
            }
            if (message.contains("error")) {
                status = "Codex request failed: " + stringField(message["error"], "message").substr(0, 1024U);
                if (requestMethod == "thread/start" || requestMethod == "turn/start") { active = false; waitingForTurn = false; pendingPrompt.clear(); }
                if (requestMethod == "model/list") catalogLoading = false;
                return;
            }
            const Json result = message.contains("result") && message["result"].is_object() ? message["result"] : Json::object();
            if (requestMethod == "initialize") {
                enqueue(Json{{"method", "initialized"}, {"params", Json::object()}});
                ready = true;
                status = "Connected. Choose session settings below; Codex configuration stays unchanged.";
                runtimeInfo = stringField(result, "userAgent");
                refreshAccount();
                request("config/read", Json{{"includeLayers", false}});
                refreshModels();
                request("configRequirements/read");
            } else if (requestMethod == "config/read") {
                if (result.contains("config")) {
                    configuredModel = stringField(result["config"], "model");
                    configuredEffort = stringField(result["config"], "model_reasoning_effort");
                }
            } else if (requestMethod == "model/list") {
                if (result.contains("data") && result["data"].is_array()) {
                    for (const auto& item : result["data"]) {
                        const auto value = stringField(item, "model");
                        if (value.empty() || findModel(value) != nullptr || models.size() >= 1024U) continue;
                        Model model;
                        model.value = value; model.name = stringField(item, "displayName"); model.description = stringField(item, "description");
                        model.defaultEffort = stringField(item, "defaultReasoningEffort");
                        model.hidden = item.contains("hidden") && item["hidden"].is_boolean() && item["hidden"].get<bool>();
                        if (item.contains("supportedReasoningEfforts") && item["supportedReasoningEfforts"].is_array()) {
                            for (const auto& effort : item["supportedReasoningEfforts"]) {
                                const auto option = stringField(effort, "reasoningEffort");
                                if (!option.empty() && !supportsEffort(model, option) && model.efforts.size() < 32U)
                                    model.efforts.push_back({option, stringField(effort, "description")});
                            }
                        }
                        models.push_back(std::move(model));
                    }
                }
                const auto cursor = stringField(result, "nextCursor");
                if (!cursor.empty() && models.size() < 1024U && modelCursors.size() < 32U && modelCursors.insert(cursor).second) {
                    if (!request("model/list", Json{{"limit", 100}, {"includeHidden", true}, {"cursor", cursor}})) catalogLoading = false;
                } else { catalogLoaded = true; catalogLoading = false; }
            } else if (requestMethod == "configRequirements/read") {
                allowedApprovalPolicies.clear(); approvalPolicyRestricted = false;
                if (result.contains("requirements") && result["requirements"].is_object()) {
                    const auto& requirements = result["requirements"];
                    if (requirements.contains("allowedApprovalPolicies") && requirements["allowedApprovalPolicies"].is_array()) {
                        approvalPolicyRestricted = true;
                        for (const auto& policy : requirements["allowedApprovalPolicies"]) if (policy.is_string()) allowedApprovalPolicies.push_back(policy.get<std::string>());
                    }
                }
            } else if (requestMethod == "account/read") {
                if (result.contains("account") && result["account"].is_object()) {
                    const Json& details = result["account"];
                    account = stringField(details, "type");
                    const std::string email = stringField(details, "email");
                    const std::string plan = stringField(details, "planType");
                    if (!email.empty()) account += " / " + email;
                    if (!plan.empty()) account += " / " + plan;
                } else account = "Not signed in";
            } else if (requestMethod == "account/login/start") {
                loginId = stringField(result, "loginId");
                loginUrl = stringField(result, "authUrl");
#if defined(_WIN32)
                if (!protocolOnly && !loginUrl.empty() && !openAuthUrl(loginUrl)) status = "Could not open the sign-in browser. Use Open sign-in page.";
                else
#endif
                    status = "Complete sign-in in your browser.";
            } else if (requestMethod == "account/login/cancel") {
                loginId.clear(); loginUrl.clear(); status = "Sign-in cancelled.";
            } else if (requestMethod == "thread/start") {
                if (result.contains("thread")) threadId = stringField(result["thread"], "id");
                readEffectiveSettings(result, "reasoningEffort");
                if (threadId.empty()) { active = false; status = "Codex did not return a conversation id."; }
                else beginTurn();
            } else if (requestMethod == "turn/start") {
                if (result.contains("turn")) turnId = stringField(result["turn"], "id");
                waitingForTurn = false;
                if (turnId.empty()) { active = false; status = "Codex did not return a turn id."; }
            }
            return;
        }
        const std::string eventThread = stringField(params, "threadId");
        if (!eventThread.empty() && !threadId.empty() && eventThread != threadId) return;
        if (method == "account/login/completed") {
            loginId.clear(); loginUrl.clear();
            const bool success = params.contains("success") && params["success"].is_boolean() && params["success"].get<bool>();
            status = success ? "Signed in." : "Sign-in did not complete.";
            refreshAccount();
        } else if (method == "account/updated") {
            refreshAccount();
            refreshModels();
        } else if (method == "thread/settings/updated" && params.contains("threadSettings")) {
            readEffectiveSettings(params["threadSettings"], "effort");
        } else if (method == "turn/started") {
            if (params.contains("turn")) turnId = stringField(params["turn"], "id");
            active = true;
        } else if (method == "turn/completed") {
            active = false; waitingForTurn = false; turnId.clear(); approvals.clear();
            const Json turn = params.contains("turn") && params["turn"].is_object() ? params["turn"] : Json::object();
            status = "Turn " + stringField(turn, "status") + ".";
            if (turn.contains("error") && turn["error"].is_object()) status += " " + stringField(turn["error"], "message").substr(0, 1024U);
        } else if (method == "item/agentMessage/delta") {
            auto& item = agentMessage(stringField(params, "itemId"));
            appendBounded(item.text, stringField(params, "delta"), kMaxTranscript);
            trimTranscript(); scrollToBottom = true;
        } else if (method == "item/started" || method == "item/completed") {
            if (!params.contains("item") || !params["item"].is_object()) return;
            const Json& item = params["item"];
            const std::string type = stringField(item, "type");
            const std::string id = stringField(item, "id");
            if (type == "agentMessage") {
                const std::string text = stringField(item, "text");
                if (!text.empty()) agentMessage(id).text = text.substr(0, kMaxTranscript);
                trimTranscript(); scrollToBottom = true;
            } else if (method == "item/completed" && type == "commandExecution") {
                pushMessage("Command", stringField(item, "command") + "\nStatus: " + stringField(item, "status"), id);
            } else if (type == "fileChange" && item.contains("changes") && item["changes"].is_array()) {
                std::string diff;
                for (const auto& change : item["changes"]) appendBounded(diff, stringField(change, "path") + "\n" + stringField(change, "diff") + "\n", kMaxTranscript);
                const auto found = std::find_if(messages.begin(), messages.end(), [&](const Message& m) { return m.id == id; });
                if (found == messages.end()) pushMessage("File changes", std::move(diff), id);
                else found->text = std::move(diff);
                trimTranscript();
            }
        } else if (method == "serverRequest/resolved" && params.contains("requestId")) {
            std::erase_if(approvals, [&](const Approval& approval) { return approval.id == params["requestId"]; });
        } else if (method == "error") {
            status = "Codex reported an error.";
            if (params.contains("error")) status += " " + stringField(params["error"], "message").substr(0, 1024U);
        }
    }

#if defined(_WIN32)
    void pump() {
        std::string buffered;
        char buffer[8192];
        while (!stopRequested.load()) {
            std::string next;
            {
                std::lock_guard lock(queueMutex);
                if (!outgoing.empty()) { next = std::move(outgoing.front()); outgoing.pop_front(); outgoingBytes -= next.size(); }
            }
            if (!next.empty()) {
                std::size_t written = 0;
                while (written < next.size() && !stopRequested.load()) {
                    DWORD count = 0;
                    if (!WriteFile(input, next.data() + written, static_cast<DWORD>(std::min<std::size_t>(4096U, next.size() - written)), &count, nullptr) || count == 0) {
                        transportFailed = true; running = false; return;
                    }
                    written += count;
                }
            }
            DWORD available = 0;
            if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) { transportFailed = true; break; }
            if (available > 0) {
                DWORD count = 0;
                if (!ReadFile(output, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr)) { transportFailed = true; break; }
                buffered.append(buffer, count);
                std::size_t end = 0;
                while ((end = buffered.find('\n')) != std::string::npos) {
                    std::lock_guard lock(queueMutex);
                    if (incoming.size() >= kMaxQueue || end > kMaxLine || incomingBytes + end > 4U * kMaxLine) { transportFailed = true; running = false; return; }
                    incoming.push_back(buffered.substr(0, end));
                    incomingBytes += end;
                    buffered.erase(0, end + 1U);
                }
                if (buffered.size() > kMaxLine) { transportFailed = true; break; }
            }
            // Drain diagnostics so a noisy child cannot stall on its stderr pipe. Never expose credentials.
            if (PeekNamedPipe(diagnostics, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                DWORD count = 0;
                ReadFile(diagnostics, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr);
            }
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        running = false;
    }

    bool start() {
        stop();
        std::error_code error;
        const std::filesystem::path binary = std::filesystem::absolute(std::filesystem::u8path(executable), error);
        if (error || !std::filesystem::is_regular_file(binary, error) || binary.extension() != ".exe") {
            status = "Choose the native codex.exe executable (npm .cmd/.ps1 wrappers are not supported)."; return false;
        }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE childInput = nullptr, childOutput = nullptr, childDiagnostics = nullptr;
        const auto closeChildren = [&] { closeHandle(childInput); closeHandle(childOutput); closeHandle(childDiagnostics); };
        if (!CreatePipe(&childInput, &input, &security, 65536) || !CreatePipe(&output, &childOutput, &security, 65536) ||
            !CreatePipe(&diagnostics, &childDiagnostics, &security, 65536)) {
            closeChildren(); stop(); status = "Could not create Codex process pipes."; return false;
        }
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(diagnostics, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childInput;
        startup.StartupInfo.hStdOutput = childOutput;
        startup.StartupInfo.hStdError = childDiagnostics;
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<std::byte> attributes(attributeBytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        HANDLE inherited[] = {childInput, childOutput, childDiagnostics};
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) {
            closeChildren(); stop(); status = "Could not initialize Codex process handles."; return false;
        }
        const bool attributesReady = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                               inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        const bool jobReady = job != nullptr && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        PROCESS_INFORMATION info{};
        std::wstring command = L"\"" + binary.wstring() + L"\" app-server --stdio";
        const bool created = attributesReady && jobReady && CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, root.c_str(), &startup.StartupInfo, &info);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        closeChildren();
        if (!created) { stop(); status = "Could not start Codex. Check the executable and project directory."; return false; }
        process = info.hProcess;
        if (!AssignProcessToJobObject(job, process)) {
            TerminateProcess(process, 1); closeHandle(info.hThread); stop(); status = "Could not contain the Codex helper process."; return false;
        }
        const DWORD resumed = ResumeThread(info.hThread);
        closeHandle(info.hThread);
        if (resumed == static_cast<DWORD>(-1)) { stop(); status = "Could not resume the Codex helper."; return false; }
        stopRequested = false; transportFailed = false; running = true;
        worker = std::thread([this] { pump(); });
        status = "Connecting to Codex...";
        return initializeConnection();
    }
#else
    bool start() { status = "Embedded Codex transport is currently available on Windows."; return false; }
#endif

    void stop() {
        stopRequested = true;
#if defined(_WIN32)
        // Kill only this client's private process tree; cancellation also unblocks a stalled pipe writer.
        closeHandle(job);
        if (worker.joinable()) CancelSynchronousIo(static_cast<HANDLE>(worker.native_handle()));
#endif
        if (worker.joinable()) worker.join();
#if defined(_WIN32)
        closeHandle(input); closeHandle(output); closeHandle(diagnostics); closeHandle(process);
#endif
        running = false; ready = false; active = false; waitingForTurn = false;
        threadId.clear(); turnId.clear(); loginId.clear(); loginUrl.clear(); pendingPrompt.clear();
        requests.clear(); catalogRequests.clear(); approvals.clear(); account = "Not connected";
        runtimeInfo.clear(); configuredModel.clear(); configuredEffort.clear(); models.clear(); modelCursors.clear();
        catalogLoaded = false; catalogLoading = false;
        clearConfirmedSettings();
        allowedApprovalPolicies.clear(); approvalPolicyRestricted = false;
        std::lock_guard lock(queueMutex);
        outgoing.clear(); incoming.clear(); outgoingBytes = 0; incomingBytes = 0;
    }
};

CodexAssistant::CodexAssistant() : m_impl(std::make_unique<Impl>()) {}
CodexAssistant::~CodexAssistant() = default;
void CodexAssistant::setProjectRoot(std::filesystem::path root) {
    if (!m_impl->running.load()) m_impl->root = std::filesystem::absolute(std::move(root)).lexically_normal();
}
void CodexAssistant::setContext(std::string context) { m_impl->context = utf8Prefix(context, kMaxPrompt); }
void CodexAssistant::setContextProvider(std::function<std::string()> provider) { m_impl->contextProvider = std::move(provider); }
bool CodexAssistant::connected() const { return m_impl->ready; }
bool CodexAssistant::busy() const { return m_impl->active; }
void CodexAssistant::shutdown() { m_impl->stop(); }

void CodexAssistant::update() {
    std::deque<std::string> messages;
    {
        std::lock_guard lock(m_impl->queueMutex);
        messages.swap(m_impl->incoming);
        m_impl->incomingBytes = 0;
    }
    for (const auto& message : messages) m_impl->receive(message);
    if (m_impl->worker.joinable() && !m_impl->running.load()) {
        const bool failed = m_impl->transportFailed.load();
        m_impl->stop();
        m_impl->status = failed ? "Codex transport failed or exceeded its limits. Reconnect to continue." : "Codex process exited. Reconnect to continue.";
    }
}

void CodexAssistant::render(bool* open) {
    update();
    if (open != nullptr && !*open) return;
    // A flat, docked assistant surface. Position and size belong to the editor's
    // dock layout; these local styles never change the application's theme.
    const ImVec4 surface(25.0F / 255.0F, 26.0F / 255.0F, 31.0F / 255.0F, 1.0F);
    const ImVec4 border(48.0F / 255.0F, 51.0F / 255.0F, 59.0F / 255.0F, 1.0F);
    const ImVec4 muted(143.0F / 255.0F, 147.0F / 255.0F, 156.0F / 255.0F, 1.0F);
    const ImVec4 foreground(197.0F / 255.0F, 200.0F / 255.0F, 207.0F / 255.0F, 1.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, surface);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, surface);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleColor(ImGuiCol_Separator, border);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, surface);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.145F, 0.153F, 0.18F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.19F, 0.20F, 0.23F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.165F, 0.173F, 0.20F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.205F, 0.216F, 0.25F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Text, foreground);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, muted);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0F, 2.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0F, 6.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0.5F, 0.5F));
    if (!ImGui::Begin("Assistant", open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::End(); ImGui::PopStyleVar(8); ImGui::PopStyleColor(12); return;
    }
    Impl& state = *m_impl;
    const auto titleOrigin = ImGui::GetCursorScreenPos();
    const float titleWidth = ImGui::GetContentRegionAvail().x;
    auto* titleDraw = ImGui::GetWindowDrawList();
    titleDraw->AddRectFilled(titleOrigin, {titleOrigin.x + titleWidth, titleOrigin.y + kPanelTitleHeight}, IM_COL32(32, 34, 39, 255));
    auto* titleFont = nativeFont();
    if (!titleFont) titleFont = ImGui::GetFont();
    const auto titleSize = titleFont->CalcTextSizeA(kEditorFontSize, titleWidth, 0.0F, "Assistant");
    titleDraw->AddText(titleFont, kEditorFontSize,
        {titleOrigin.x + std::max(4.0F, (titleWidth - 36.0F - titleSize.x) * 0.5F),
            titleOrigin.y + (kPanelTitleHeight - titleSize.y) * 0.5F},
        ImGui::GetColorU32(foreground), "Assistant");
    if (open) {
        ImGui::SetCursorScreenPos({titleOrigin.x + std::max(0.0F, titleWidth - kPanelTitleHeight), titleOrigin.y});
        if (ImGui::InvisibleButton("##AssistantClose", {kPanelTitleHeight, kPanelTitleHeight})) *open = false;
        const ImU32 ink = ImGui::GetColorU32(ImGui::IsItemHovered() ? foreground : muted);
        const ImVec2 center(titleOrigin.x + titleWidth - kPanelTitleHeight * 0.5F, titleOrigin.y + kPanelTitleHeight * 0.5F);
        titleDraw->AddLine({center.x - 3.0F, center.y - 3.0F}, {center.x + 3.0F, center.y + 3.0F}, ink);
        titleDraw->AddLine({center.x + 3.0F, center.y - 3.0F}, {center.x - 3.0F, center.y + 3.0F}, ink);
    }
    ImGui::SetCursorScreenPos({titleOrigin.x, titleOrigin.y + kPanelTitleHeight});
    enum class Icon { More, Add, Controls, Send, Stop };
    const auto iconButton = [&](const char* id, const Icon icon, const char* tooltip, const bool enabled = true) {
        ImGui::BeginDisabled(!enabled);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(24.0F, 24.0F));
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        auto* draw = ImGui::GetWindowDrawList();
        const ImU32 ink = ImGui::GetColorU32(enabled ? foreground : muted);
        const ImVec2 center(pos.x + 12.0F, pos.y + 12.0F);
        if (hovered && enabled) draw->AddRectFilled(pos, ImVec2(pos.x + 24.0F, pos.y + 24.0F), ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0F);
        if (icon == Icon::More) {
            for (int i = -1; i <= 1; ++i) draw->AddCircleFilled(ImVec2(center.x + static_cast<float>(i) * 4.0F, center.y), 1.1F, ink);
        } else if (icon == Icon::Add) {
            draw->AddCircleFilled(center, 10.0F, ImGui::GetColorU32(border));
            draw->AddLine(ImVec2(center.x - 4.0F, center.y), ImVec2(center.x + 4.0F, center.y), ink, 1.2F);
            draw->AddLine(ImVec2(center.x, center.y - 4.0F), ImVec2(center.x, center.y + 4.0F), ink, 1.2F);
        } else if (icon == Icon::Controls) {
            for (int row = 0; row < 3; ++row) {
                const float y = center.y - 5.0F + static_cast<float>(row) * 5.0F;
                draw->AddLine(ImVec2(center.x - 6.0F, y), ImVec2(center.x + 6.0F, y), ink, 1.0F);
                draw->AddCircleFilled(ImVec2(center.x + (row == 1 ? -2.5F : 2.5F), y), 2.0F, ink);
            }
        } else if (icon == Icon::Stop) {
            draw->AddRectFilled(ImVec2(center.x - 4.0F, center.y - 4.0F), ImVec2(center.x + 4.0F, center.y + 4.0F), ink, 1.0F);
        } else {
            draw->AddCircleFilled(center, 10.0F, ImGui::GetColorU32(border));
            draw->AddLine(ImVec2(center.x, center.y + 5.0F), ImVec2(center.x, center.y - 5.0F), ink, 1.3F);
            draw->AddLine(ImVec2(center.x - 4.0F, center.y - 1.0F), ImVec2(center.x, center.y - 5.0F), ink, 1.3F);
            draw->AddLine(ImVec2(center.x + 4.0F, center.y - 1.0F), ImVec2(center.x, center.y - 5.0F), ink, 1.3F);
        }
        ImGui::EndDisabled();
        if (hovered) ImGui::SetTooltip("%s", tooltip);
        return clicked;
    };
    bool openSettings = false;
    bool enableProjectEdits = false;
    const float edge = 20.0F;
    const float headerTop = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(edge, headerTop + 12.0F));
    ImGui::BeginDisabled(state.active);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0F, 0.5F));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0F, 2.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    ImGui::PushFont(editor::semiboldFont(), 19.0F);
    if (ImGui::Button("New chat##AssistantNew", ImVec2(100.0F, 24.0F))) {
        state.threadId.clear(); state.turnId.clear(); state.messages.clear(); state.clearConfirmedSettings(); state.status = "New conversation ready.";
    }
    ImGui::PopFont();
    ImGui::PopStyleVar(3);
    ImGui::EndDisabled();
    const auto newChatMax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(newChatMax.x - 12.0F, newChatMax.y - 14.0F),
        ImVec2(newChatMax.x - 6.0F, newChatMax.y - 14.0F), ImVec2(newChatMax.x - 9.0F, newChatMax.y - 10.0F), ImGui::GetColorU32(muted));
    ImGui::SameLine();
    const float detailsX = std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 96.0F);
    ImGui::SetCursorPosX(detailsX);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Codex");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", state.account.c_str(), state.status.c_str());
    ImGui::SameLine();
    openSettings = iconButton("AssistantMore", Icon::More, "Assistant settings");
    ImGui::SetCursorPosY(headerTop + 48.0F);
    ImGui::Separator();

    if (!state.approvals.empty()) {
        ImGui::SetCursorPosX(edge);
        const float approvalHeight = std::min(210.0F, std::max(100.0F, ImGui::GetContentRegionAvail().y * 0.35F));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0F, 8.0F));
        ImGui::BeginChild("Assistant Approval", ImVec2(ImGui::GetContentRegionAvail().x - edge, approvalHeight), ImGuiChildFlags_Borders);
        const auto& approval = state.approvals.front();
        ImGui::TextColored(ImVec4(1.0F, 0.76F, 0.3F, 1.0F), "Approval required (%zu)", state.approvals.size());
        const Json network = approval.params.contains("networkApprovalContext") && approval.params["networkApprovalContext"].is_object()
            ? approval.params["networkApprovalContext"] : Json::object();
        const bool isFileChange = approval.method == "item/fileChange/requestApproval";
        const bool isNetwork = !stringField(network, "host").empty();
        ImGui::TextWrapped("%s", isFileChange ? "Apply the file changes shown in the conversation?" : isNetwork ? "Allow this network connection?" : "Run this command?");
        const std::string command = stringField(approval.params, "command");
        const std::string reason = stringField(approval.params, "reason");
        if (!command.empty()) ImGui::TextWrapped("%s", command.c_str());
        if (!reason.empty()) ImGui::TextWrapped("%s", reason.c_str());
        const std::string cwd = stringField(approval.params, "cwd");
        const std::string grantRoot = stringField(approval.params, "grantRoot");
        if (!cwd.empty()) ImGui::TextWrapped("Directory: %s", cwd.c_str());
        if (!grantRoot.empty()) ImGui::TextWrapped("Requested write root: %s", grantRoot.c_str());
        if (isNetwork) ImGui::TextWrapped("Destination: %s (%s)", stringField(network, "host").c_str(), stringField(network, "protocol").c_str());
        const std::string itemId = stringField(approval.params, "itemId");
        const bool hasDiff = std::any_of(state.messages.begin(), state.messages.end(), [&](const Impl::Message& message) {
            return message.id == itemId && message.title == "File changes" && !message.text.empty();
        });
        ImGui::BeginDisabled(isFileChange ? !hasDiff : command.empty() && !isNetwork);
        if (ImGui::Button("Allow once")) state.decide(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Decline")) state.decide(false);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    const float composerY = std::max(ImGui::GetCursorPosY() + 32.0F, ImGui::GetWindowHeight() - 128.0F);
    const float transcriptHeight = std::max(1.0F, composerY - ImGui::GetCursorPosY() - 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(edge, 8.0F));
    ImGui::BeginChild("Conversation", ImVec2(0.0F, transcriptHeight), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(nativeFont(), 17.0F);
    if (state.messages.empty()) {
        ImGui::TextWrapped("Hi there! What can I help you build?");
    }
    for (const auto& message : state.messages) {
        ImGui::TextDisabled("%s", message.title.c_str());
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(message.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0F, 12.0F));
    }
    if (state.scrollToBottom) { ImGui::SetScrollHereY(1.0F); state.scrollToBottom = false; }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SetCursorPos(ImVec2(edge, composerY));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0F, 6.0F));
    ImGui::BeginChild("Assistant Composer", ImVec2(ImGui::GetContentRegionAvail().x - edge, 90.0F), ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    ImGui::PushFont(nullptr, 15.5F);
    ImGui::InputTextMultiline("##Prompt", &state.prompt, ImVec2(-1.0F, 44.0F));
    ImGui::PopStyleVar();
    if (state.prompt.empty()) {
        const auto pos = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 6.0F, pos.y + 4.0F), ImGui::GetColorU32(muted), "Ask Assistant...");
    }
    ImGui::PopFont();
    if (iconButton("AssistantContext", Icon::Add, "Editor context")) ImGui::OpenPopup("Assistant Context");
    if (ImGui::BeginPopup("Assistant Context")) {
        ImGui::Checkbox("Include current editor context", &state.includeContext);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    openSettings = iconButton("AssistantControls", Icon::Controls, "Model and access settings") || openSettings;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 128.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0F);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.153F, 0.161F, 0.188F, 1.0F));
    if (ImGui::Button(state.workspaceWrite ? "Edits##AssistantAccess" : "Read only##AssistantAccess", ImVec2(80.0F, 24.0F))) openSettings = true;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Project access and approval settings");
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 40.0F));
    if (state.active) {
        if (iconButton("AssistantCancel", Icon::Stop, "Cancel request")) {
            if (!state.turnId.empty()) state.request("turn/interrupt", Json{{"threadId", state.threadId}, {"turnId", state.turnId}});
            else { state.stop(); state.status = "Pending request cancelled. Reconnect to continue."; }
        }
    } else if (iconButton("AssistantSend", Icon::Send, "Send message", state.ready)) {
        state.sendPrompt();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::SetCursorPos(ImVec2(edge + 4.0F, composerY + 102.0F));
    ImGui::PushFont(nullptr, 15.5F);
    ImGui::TextDisabled("%s", state.status.c_str());
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", state.status.c_str());

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 10.0F));
    if (openSettings) ImGui::OpenPopup("Assistant Settings");
    ImGui::SetNextWindowSizeConstraints(ImVec2(340.0F, 0.0F), ImVec2(420.0F, 720.0F));
    if (ImGui::BeginPopup("Assistant Settings")) {
        ImGui::TextUnformatted("Assistant settings");
        ImGui::Separator();
        if (!state.running.load()) {
            ImGui::InputText("codex.exe", &state.executable);
            if (ImGui::Button("Connect")) state.start();
        } else {
            if (ImGui::Button("Disconnect")) { state.stop(); state.status = "Disconnected."; }
            ImGui::SameLine();
            ImGui::BeginDisabled(!state.ready || !state.loginId.empty());
            if (ImGui::Button("Sign in with ChatGPT")) state.request("account/login/start", Json{{"type", "chatgpt"}});
            ImGui::EndDisabled();
            if (!state.loginId.empty()) {
#if defined(_WIN32)
                if (ImGui::Button("Open sign-in page")) {
                    if (!openAuthUrl(state.loginUrl)) state.status = "Sign-in URL could not be opened.";
                }
                ImGui::SameLine();
#endif
                if (ImGui::Button("Cancel sign-in")) state.request("account/login/cancel", Json{{"loginId", state.loginId}});
            }
        }
        ImGui::TextWrapped("Account: %s", state.account.c_str());
        if (!state.runtimeInfo.empty()) ImGui::TextWrapped("Runtime: %s", state.runtimeInfo.c_str());
        state.renderSettings();
        ImGui::Separator();
        ImGui::BeginDisabled(state.active);
        if (ImGui::Button(state.workspaceWrite ? "Mode: project edits" : "Mode: read only")) {
            if (state.workspaceWrite) { state.workspaceWrite = false; state.threadId.clear(); state.clearConfirmedSettings(); state.status = "Read-only mode enabled; next message starts a new conversation."; }
            else enableProjectEdits = true;
        }
        ImGui::EndDisabled();
        ImGui::Checkbox("Include current editor context", &state.includeContext);
        ImGui::TextWrapped("%s", state.status.c_str());
        ImGui::EndPopup();
    }
    if (enableProjectEdits) ImGui::OpenPopup("Enable project edits?");
    if (ImGui::BeginPopupModal("Enable project edits?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Allow Codex to modify files in this project? Network access remains restricted. Commands requiring approval are shown in this pane.");
        ImGui::TextWrapped("Project: %s", pathUtf8(state.root).c_str());
        if (ImGui::Button("Allow project edits")) { state.workspaceWrite = true; state.threadId.clear(); state.clearConfirmedSettings(); state.status = "Project edits enabled; next message starts a new conversation."; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Keep read only")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::PopStyleVar(8);
    ImGui::PopStyleColor(12);
}

bool CodexAssistantTestAccess::connectLocalTransport(CodexAssistant& assistant) { return assistant.m_impl->start(); }
std::string CodexAssistantTestAccess::prompt(const CodexAssistant& assistant) { return assistant.m_impl->prompt; }
std::string CodexAssistantTestAccess::status(const CodexAssistant& assistant) { return assistant.m_impl->status; }
std::string CodexAssistantTestAccess::executable(const CodexAssistant& assistant) { return assistant.m_impl->executable; }
bool CodexAssistantTestAccess::hasModel(const CodexAssistant& assistant, const std::string_view model) {
    return assistant.m_impl->findModel(model) != nullptr;
}
bool CodexAssistantTestAccess::selectModel(CodexAssistant& assistant, const std::string_view model) { return assistant.m_impl->selectModel(model); }
bool CodexAssistantTestAccess::selectEffort(CodexAssistant& assistant, const std::string_view effort) { return assistant.m_impl->selectEffort(effort); }
bool CodexAssistantTestAccess::selectApprovals(CodexAssistant& assistant, const std::string_view policy, const std::string_view reviewer) { return assistant.m_impl->selectApprovals(policy, reviewer); }
std::vector<std::string> CodexAssistantTestAccess::efforts(const CodexAssistant& assistant) {
    std::vector<std::string> result;
    const auto* model = assistant.m_impl->currentModel();
    if (model != nullptr) for (const auto& effort : model->efforts) result.push_back(effort.value);
    return result;
}
std::string CodexAssistantTestAccess::settings(const CodexAssistant& assistant) { return assistant.m_impl->settingsText(); }
std::filesystem::path CodexAssistantTestAccess::desktopBinary(const std::filesystem::path& bin) { return discoverDesktopCodex(bin); }
void CodexAssistantTestAccess::initialize(CodexAssistant& assistant) {
    assistant.m_impl->protocolOnly = true;
    assistant.m_impl->initializeConnection();
}
void CodexAssistantTestAccess::submit(CodexAssistant& assistant, std::string prompt) {
    assistant.m_impl->prompt = std::move(prompt);
    assistant.m_impl->sendPrompt();
}
void CodexAssistantTestAccess::receive(CodexAssistant& assistant, const std::string_view message) {
    assistant.m_impl->protocolOnly = true;
    assistant.m_impl->receive(message);
}
std::vector<std::string> CodexAssistantTestAccess::takeOutgoing(CodexAssistant& assistant) {
    std::lock_guard lock(assistant.m_impl->queueMutex);
    std::vector<std::string> result(assistant.m_impl->outgoing.begin(), assistant.m_impl->outgoing.end());
    assistant.m_impl->outgoing.clear(); assistant.m_impl->outgoingBytes = 0;
    return result;
}
std::string CodexAssistantTestAccess::transcript(const CodexAssistant& assistant) {
    std::string text;
    for (const auto& message : assistant.m_impl->messages) text += message.text + '\n';
    return text;
}
std::size_t CodexAssistantTestAccess::approvalCount(const CodexAssistant& assistant) { return assistant.m_impl->approvals.size(); }
void CodexAssistantTestAccess::decide(CodexAssistant& assistant, const bool accept) { assistant.m_impl->decide(accept); }

} // namespace engine::editor
