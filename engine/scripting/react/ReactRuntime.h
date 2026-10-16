#pragma once

#include <filesystem>

namespace engine::scripting {
class ScriptingSystem;

// Bootstrap of real upstream React Luau, independent of graphics for tests.
class ReactRuntime final {
public:
    static bool install(ScriptingSystem& scripting, std::filesystem::path packageRoot = {});
    static bool pump(ScriptingSystem& scripting, double dt);
    static bool reset(ScriptingSystem& scripting);
};
} // namespace engine::scripting
