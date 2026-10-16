#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "engine/ecs/entity.h"
#include "engine/scripting/ScriptComponent.h"

struct lua_State;
namespace engine::core { class JobSystem; }
namespace engine::ecs { class World; }

namespace engine::scripting {

struct ScriptMessage final {
    enum class Kind { Output, Error, Info };
    Kind kind{Kind::Output};
    std::string text;
};

struct ScriptField final {
    std::string name;
    std::string type;
    bool integer{false}; // Canonical numeric schema with integral value semantics.
    nlohmann::json defaultValue;
    double minimum{-1000000.0};
    double maximum{1000000.0};
};

class ScriptingSystem final {
public:
    struct Limits final {
        std::size_t memoryBytes{64U * 1024U * 1024U};
#if defined(NDEBUG)
        double callbackMilliseconds{25.0};
#else
        // Unoptimized host bindings and VM checks are much slower in Debug.
        double callbackMilliseconds{100.0};
#endif
        std::size_t sourceBytes{2U * 1024U * 1024U};
    };

    ScriptingSystem();
    ~ScriptingSystem();
    ScriptingSystem(const ScriptingSystem&) = delete;
    ScriptingSystem& operator=(const ScriptingSystem&) = delete;

    // Must be called and used on the World's owner thread. Jobs only read/compile.
    bool initialize(ecs::World& world, core::JobSystem* jobs,
                    std::filesystem::path assetRoot = "assets");
    void shutdown();
    void setLimits(Limits limits);
    Limits limits() const;
    // Host gates gameplay focus; Raw input still works inside an ImGui viewport.
    void setInputEnabled(bool enabled);
    void setDestroyCallback(std::function<void(ecs::EntityId)> callback);
    void start();
    void stop();
    void update(double dt);
    void queueEvent(std::string type, nlohmann::json payload);
    void dispatchEvents();
    void forgetEntity(ecs::EntityId entity);

    bool loadBehavior(const std::string& path);
    std::vector<ScriptField> fields(const std::string& path);
    void populateDefaults(ScriptComponent& component);
    bool requestReload(const std::string& path);
    void pollReload();
    bool reloadPending() const;
    bool analyze(const std::string& source, const std::string& chunkName);
    bool execute(const std::string& source, const std::string& chunkName = "=Command");
    // Trusted host entry point: call an existing global without compiling a chunk per frame.
    bool callGlobal(const char* name, double argument = 0.0);
    // Named modules are used by the real React Luau package adapter.
    void registerModule(std::string name, std::string source);
    lua_State* state();

    ecs::EntityId spawnPrefab(const std::string& path, const nlohmann::json& overrides = nlohmann::json::object());
    const std::vector<ScriptMessage>& output() const;
    void clearOutput();
    nlohmann::json inspect(ecs::EntityId entity) const;
    const nlohmann::json& status() const;
    bool playing() const;
    std::size_t memoryUsed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::scripting
