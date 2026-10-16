#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

struct CodexAssistantTestAccess;

// Local Codex app-server client. Credentials and global configuration remain owned by Codex.
class CodexAssistant final {
public:
    CodexAssistant();
    ~CodexAssistant();
    CodexAssistant(const CodexAssistant&) = delete;
    CodexAssistant& operator=(const CodexAssistant&) = delete;

    void setProjectRoot(std::filesystem::path root);
    void setContext(std::string context);
    void setContextProvider(std::function<std::string()> provider);
    void update();
    void render(bool* open = nullptr);
    void shutdown();
    [[nodiscard]] bool connected() const;
    [[nodiscard]] bool busy() const;

private:
    friend struct CodexAssistantTestAccess;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Protocol-only regression hooks: never start a process, authenticate, or invoke a model.
struct CodexAssistantTestAccess final {
    static bool connectLocalTransport(CodexAssistant& assistant);
    static void initialize(CodexAssistant& assistant);
    static void submit(CodexAssistant& assistant, std::string prompt);
    static void receive(CodexAssistant& assistant, std::string_view message);
    static std::vector<std::string> takeOutgoing(CodexAssistant& assistant);
    static std::string transcript(const CodexAssistant& assistant);
    static std::string prompt(const CodexAssistant& assistant);
    static std::string status(const CodexAssistant& assistant);
    static std::string executable(const CodexAssistant& assistant);
    static bool hasModel(const CodexAssistant& assistant, std::string_view model);
    static bool selectModel(CodexAssistant& assistant, std::string_view model);
    static bool selectEffort(CodexAssistant& assistant, std::string_view effort);
    static bool selectApprovals(CodexAssistant& assistant, std::string_view policy, std::string_view reviewer);
    static std::vector<std::string> efforts(const CodexAssistant& assistant);
    static std::string settings(const CodexAssistant& assistant);
    static std::filesystem::path desktopBinary(const std::filesystem::path& bin);
    static std::size_t approvalCount(const CodexAssistant& assistant);
    static void decide(CodexAssistant& assistant, bool accept);
};

} // namespace engine::editor
