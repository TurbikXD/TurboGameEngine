#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace engine::scripting {

struct ScriptComponent final {
    std::string path;
    bool enabled{true};
    nlohmann::json fields = nlohmann::json::object();
};

}
