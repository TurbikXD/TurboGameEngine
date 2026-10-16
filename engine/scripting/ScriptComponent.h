#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace engine::scripting {

// Scene-owned data only. VM references live in ScriptingSystem and never in ECS.
struct ScriptComponent final {
    std::string path;
    bool enabled{true};
    nlohmann::json fields = nlohmann::json::object();
};

} // namespace engine::scripting
