#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include "engine/ecs/entity.h"
#include "engine/scripting/ScriptComponent.h"
#include "engine/scripting/ScriptNetwork.h"
#include "engine/scripting/ScriptSteam.h"

struct lua_State;
namespace engine::core { class JobSystem; }
namespace engine::ecs { class World; }
namespace engine::platform { class Window; }

namespace engine::scripting {

struct ScriptMessage final {
    enum class Kind { Output, Error, Info };
    Kind kind{Kind::Output};
    std::string text;
};

struct ScriptField final {
    std::string name;
    std::string type;
    std::string label;
    std::string description;
    std::string category;
    bool integer{false};
    nlohmann::json defaultValue;
    double minimum{-1000000.0};
    double maximum{1000000.0};
    double step{0.1};
};

class ScriptingSystem final {
public:
    struct Limits final {
        std::size_t memoryBytes{64U * 1024U * 1024U};
#if defined(NDEBUG)
        double callbackMilliseconds{25.0};
#else
        double callbackMilliseconds{100.0};
#endif
        std::size_t sourceBytes{2U * 1024U * 1024U};
    };

    ScriptingSystem();
    ~ScriptingSystem();
    ScriptingSystem(const ScriptingSystem&) = delete;
    ScriptingSystem& operator=(const ScriptingSystem&) = delete;

    bool initialize(ecs::World& world, core::JobSystem* jobs,
                    std::filesystem::path assetRoot = "assets");
    void shutdown();
    void setLimits(Limits limits);
    Limits limits() const;
    void setInputEnabled(bool enabled);
    void setPointerInputEnabled(bool enabled);
    void setInputWindow(platform::Window* window);
    [[nodiscard]] bool cursorLocked() const;
    bool releaseCursorLock();
    void setDestroyCallback(std::function<void(ecs::EntityId)> callback);
    void setNetwork(ScriptNetwork* network);
    void setSteam(ScriptSteam* steam);
    ScriptRpcReply invokeNetworkRpc(ecs::EntityId entity, uint32_t peer, uint8_t action, int32_t argument,
                                    std::string_view text = {});
    void start();
    void stop();
    void update(double dt);
    void queueEvent(std::string type, nlohmann::json payload);
    void dispatchEvents();
    void forgetEntity(ecs::EntityId entity);

    bool loadBehavior(const std::string& path);
    std::vector<ScriptField> fields(const std::string& path);
    void populateDefaults(ScriptComponent& component);
    bool setField(ScriptComponent& component, const std::string& name, const nlohmann::json& value);
    bool requestReload(const std::string& path);
    void pollReload();
    bool reloadPending() const;
    bool analyze(const std::string& source, const std::string& chunkName);
    bool execute(const std::string& source, const std::string& chunkName = "=Command");
    bool callGlobal(const char* name, double argument = 0.0);
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

}
