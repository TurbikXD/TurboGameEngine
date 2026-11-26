#pragma once

#include <filesystem>
#include <memory>

namespace engine::scripting {

class ScriptAudio final {
public:
    ScriptAudio();
    ~ScriptAudio();
    ScriptAudio(const ScriptAudio&) = delete;
    ScriptAudio& operator=(const ScriptAudio&) = delete;

    [[nodiscard]] bool play(const std::filesystem::path& resolvedWave, float gain = 1.0F);
    void clear() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
