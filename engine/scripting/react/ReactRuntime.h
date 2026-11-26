#pragma once

#include <filesystem>

namespace engine::scripting {
class ScriptingSystem;

class ReactRuntime final {
public:
    static bool install(ScriptingSystem& scripting, std::filesystem::path packageRoot = {});
    static bool pump(ScriptingSystem& scripting, double dt);
    static bool reset(ScriptingSystem& scripting);
};
}
