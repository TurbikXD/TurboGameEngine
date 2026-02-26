#pragma once

#include <array>
#include <string>
#include <string_view>

namespace engine::core {

struct EngineConfig final {
    int width{1280};
    int height{720};
    std::string title{"TurboGameEngine"};
    bool vsync{true};
    std::array<float, 4> clearColor{0.10F, 0.10F, 0.16F, 1.0F};
    std::string initialState{"menu"};
};

class Config final {
public:
    static EngineConfig load(std::string_view path);
};

} // namespace engine::core
