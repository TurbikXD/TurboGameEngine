#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace engine::editor {

class ScriptFormatter final {
public:
    struct Result final {
        std::string documentPath;
        std::string original;
        std::string formatted;
        std::string error;
        bool success{false};
    };

    ScriptFormatter();
    ~ScriptFormatter();
    ScriptFormatter(const ScriptFormatter&) = delete;
    ScriptFormatter& operator=(const ScriptFormatter&) = delete;

    void setExecutable(std::filesystem::path executable);
    [[nodiscard]] const std::filesystem::path& executable() const;
    [[nodiscard]] bool available() const;
    [[nodiscard]] bool busy() const;
    bool start(std::string documentPath, std::string source);
    std::optional<Result> poll();
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
