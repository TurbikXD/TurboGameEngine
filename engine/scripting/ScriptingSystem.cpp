#include "engine/scripting/ScriptingSystem.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>
#include "engine/core/JobSystem.h"
#include "engine/core/LiveFileWatcher.h"
#include "engine/core/Log.h"
#include "engine/core/Profiling.h"
#include "engine/ecs/components.h"
#include "engine/ecs/world.h"
#include "engine/platform/Input.h"

namespace engine::scripting {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::string compileSource(const std::string& source) {
    Luau::CompileOptions options;
    options.optimizationLevel = 1;
    options.debugLevel = 2;
    // UI Instances and Roblox value types are host-backed tables. Their typeof
    // override must run instead of the VM's table-only FASTCALL specialization.
    // Keep all other safe-environment/import optimizations enabled.
    static const char* const hostOverrides[] = { "typeof", nullptr };
    options.disabledBuiltins = hostOverrides;
    options.mutableGlobals = hostOverrides;
    return Luau::compile(source, options);
}

std::string compileError(const std::string& bytes) {
    return !bytes.empty() && bytes.front() == '\0' ? bytes.substr(1U) : std::string{};
}

void pushJson(lua_State* state, const Json& value) {
    if (value.is_boolean()) { lua_pushboolean(state, value.get<bool>()); }
    else if (value.is_number()) { lua_pushnumber(state, value.get<double>()); }
    else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        lua_pushlstring(state, text.data(), text.size());
    } else if (value.is_object()) {
        lua_createtable(state, 0, static_cast<int>(value.size()));
        for (const auto& [key, item] : value.items()) {
            pushJson(state, item);
            lua_setfield(state, -2, key.c_str());
        }
    } else if (value.is_array()) {
        lua_createtable(state, static_cast<int>(value.size()), 0);
        for (std::size_t index = 0; index < value.size(); ++index) {
            pushJson(state, value[index]);
            lua_rawseti(state, -2, static_cast<int>(index + 1U));
        }
    } else { lua_pushnil(state); }
}

Json readJson(lua_State* state, const int index, const unsigned depth = 0U, std::size_t* remaining = nullptr) {
    std::size_t budget = 512U;
    if (remaining == nullptr) { remaining = &budget; }
    if (depth > 8U) { return "<depth limit>"; }
    if (*remaining == 0U) { return "<item limit>"; }
    --*remaining;
    switch (lua_type(state, index)) {
    case LUA_TNIL: return nullptr;
    case LUA_TBOOLEAN: return lua_toboolean(state, index) != 0;
    case LUA_TNUMBER: return lua_tonumber(state, index);
    case LUA_TSTRING: return std::string(lua_tostring(state, index));
    case LUA_TVECTOR: {
        const auto* vector = lua_tovector(state, index);
        return Json::array({vector[0], vector[1], vector[2]});
    }
    case LUA_TTABLE: {
        Json value = Json::object();
        const int absolute = lua_absindex(state, index);
        lua_pushnil(state);
        std::size_t count = 0;
        while (lua_next(state, absolute) != 0) {
            if (++count > 128U) { lua_pop(state, 2); break; }
            if (lua_type(state, -2) == LUA_TSTRING) {
                value[lua_tostring(state, -2)] = readJson(state, -1, depth + 1U, remaining);
            } else if (lua_type(state, -2) == LUA_TNUMBER) {
                value[std::to_string(lua_tointeger(state, -2))] = readJson(state, -1, depth + 1U, remaining);
            }
            lua_pop(state, 1);
            if (*remaining == 0U) { lua_pop(state, 1); break; }
        }
        return value;
    }
    default: return std::string("<") + lua_typename(state, lua_type(state, index)) + ">";
    }
}

glm::vec3 jsonVector(const Json& value, const glm::vec3 fallback) {
    if (!value.is_array() || value.size() != 3U) { return fallback; }
    const glm::vec3 vector(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
    if (!std::isfinite(vector.x) || !std::isfinite(vector.y) || !std::isfinite(vector.z)) {
        throw std::invalid_argument("Vector components must be finite");
    }
    return vector;
}

glm::vec3 checkVector(lua_State* state, const int index) {
    glm::vec3 result(0.0F);
    if (const auto* vector = lua_tovector(state, index)) { result = {vector[0], vector[1], vector[2]}; }
    else {
        luaL_checktype(state, index, LUA_TTABLE);
        const int absolute = lua_absindex(state, index);
        for (int component = 0; component < 3; ++component) {
            lua_rawgeti(state, absolute, component + 1);
            result[component] = static_cast<float>(luaL_checknumber(state, -1));
            lua_pop(state, 1);
        }
    }
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) {
        luaL_error(state, "Vector components must be finite");
    }
    return result;
}

ecs::EntityId checkEntity(lua_State* state, const int index) {
    const double number = luaL_checknumber(state, index);
    if (!std::isfinite(number) || number < 0.0 || number > std::numeric_limits<ecs::EntityId>::max() || std::floor(number) != number) {
        luaL_error(state, "Expected an entity handle");
    }
    return static_cast<ecs::EntityId>(number);
}

platform::KeyCode keyCode(const std::string& name) {
    using Key = platform::KeyCode;
    static const std::unordered_map<std::string, Key> keys{
        {"W", Key::W}, {"A", Key::A}, {"S", Key::S}, {"D", Key::D}, {"Q", Key::Q}, {"E", Key::E},
        {"F", Key::F}, {"N", Key::N}, {"O", Key::O}, {"R", Key::R}, {"Y", Key::Y}, {"Z", Key::Z},
        {"Space", Key::Space}, {"Enter", Key::Enter}, {"Escape", Key::Escape}, {"Tab", Key::Tab},
        {"Left", Key::Left}, {"Right", Key::Right}, {"Up", Key::Up}, {"Down", Key::Down},
        {"LeftShift", Key::LeftShift}, {"RightShift", Key::RightShift}, {"LeftControl", Key::LeftControl},
        {"RightControl", Key::RightControl}, {"LeftAlt", Key::LeftAlt}, {"RightAlt", Key::RightAlt}
    };
    const auto found = keys.find(name);
    return found == keys.end() ? Key::Unknown : found->second;
}
} // namespace

struct ScriptingSystem::Impl final {
    struct Module final { int ref{LUA_NOREF}; std::string bytecode; };
    struct Instance final { int ref{LUA_NOREF}; std::string path; bool failed{false}; Json fields; };
    struct Reload final { std::unordered_map<std::string, std::string> bytecode; std::string error; };
    enum class Api : int { Alive, Position, SetPosition, Rotation, SetRotation, Velocity, SetVelocity, Force, Tag, Find, Radius, Spawn, Destroy, Status, ReadStatus, KeyDown, KeyPressed, KeyReleased, MousePosition };

    Limits limits;
    std::size_t allocated{};
    lua_State* vm{};
    ecs::World* world{};
    core::JobSystem* jobs{};
    std::filesystem::path root;
    std::thread::id owner{std::this_thread::get_id()};
    std::unordered_map<std::string, Module> modules;
    std::unordered_map<std::string, std::string> registered;
    std::unordered_set<std::string> loading;
    std::unordered_set<std::string> failedModules;
    std::unordered_set<std::string> fieldErrors;
    std::unordered_map<ecs::EntityId, Instance> instances;
    std::unordered_set<std::string> behaviorPaths;
    std::unordered_set<ecs::EntityId> pendingDestroy;
    std::vector<std::pair<std::string, Json>> events;
    std::vector<ScriptMessage> messages;
    Json status = Json::object();
    bool playing{};
    bool allowMutation{};
    bool inputEnabled{true};
    Clock::time_point deadline{Clock::time_point::max()};
    unsigned protectedDepth{};
    int errorRef{LUA_NOREF};
    int hostRef{LUA_NOREF};
    struct HostOperation final { void (*invoke)(void*); void* data; };
    std::function<void(ecs::EntityId)> destroy;
    std::shared_ptr<Reload> reload;
    core::JobSystem::Handle reloadJob;
    core::LiveFileWatcher watcher;
    std::unordered_map<std::string, std::unordered_set<std::string>> dependencies;
    std::vector<std::string> evaluating;
    std::unordered_set<std::string> reloadQueue;
    const std::unordered_map<std::string, std::string>* prepared{};

    // File I/O and compilation are host services, not VM instruction execution.
    // Keep their cost out of the callback deadline; evaluating modules is guarded.
    struct NativePreparation final {
        Impl& self;
        Clock::time_point begin{Clock::now()};
        bool active;
        explicit NativePreparation(Impl& value) : self(value), active(value.protectedDepth > 0U) {}
        ~NativePreparation() {
            if (active && self.deadline != Clock::time_point::max()) { self.deadline += Clock::now() - begin; }
        }
    };

    void requireOwner() const {
        if (owner != std::this_thread::get_id()) { throw std::logic_error("ScriptingSystem must run on its owner thread"); }
    }
    void message(const ScriptMessage::Kind kind, std::string text) {
        if (text.size() > 16384U) { text.resize(16384U); text += "\n<output truncated>"; }
        if (messages.size() >= 512U) { messages.erase(messages.begin(), messages.begin() + 128); }
        if (kind == ScriptMessage::Kind::Error) ENGINE_LOG_ERROR("Luau: {}", text);
        else if (text.starts_with("Reloaded ")) ENGINE_LOG_INFO("Luau live: {}", text);
        messages.push_back({kind, std::move(text)});
    }
    static Impl& from(lua_State* state) { return *static_cast<Impl*>(lua_callbacks(state)->userdata); }
    static void* allocate(void* user, void* pointer, const std::size_t oldSize, const std::size_t newSize) {
        auto& self = *static_cast<Impl*>(user);
        const std::size_t previous = pointer != nullptr ? oldSize : 0U;
        if (newSize == 0U) {
            std::free(pointer);
            self.allocated -= std::min(self.allocated, previous);
            return nullptr;
        }
        if (newSize > previous && newSize - previous > self.limits.memoryBytes - std::min(self.allocated, self.limits.memoryBytes)) { return nullptr; }
        void* resized = std::realloc(pointer, newSize);
        if (resized != nullptr) { self.allocated = self.allocated - previous + newSize; }
        return resized;
    }
    static void interrupt(lua_State* state, const int gc) {
        if (gc >= 0) { return; }
        auto& self = from(state);
        if (Clock::now() > self.deadline) { luaL_error(state, "Script execution time limit exceeded"); }
    }
    static int errorHandler(lua_State* state) {
        const char* error = lua_tostring(state, 1);
        std::string text = error != nullptr ? error : "Script error";
        // Upstream React raises Error objects rather than strings. Read only
        // their raw message: calling __tostring here could raise another error.
        if (!error && lua_istable(state, 1)) {
            lua_rawgetfield(state, 1, "message");
            if (lua_type(state, -1) == LUA_TSTRING) text = lua_tostring(state, -1);
            lua_pop(state, 1);
        }
        text += "\n"; text += lua_debugtrace(state);
        lua_pushlstring(state, text.data(), text.size());
        return 1;
    }
    static int print(lua_State* state) {
        std::string result;
        const int count = lua_gettop(state);
        for (int index = 1; index <= count; ++index) {
            std::size_t length{};
            const char* value = luaL_tolstring(state, index, &length);
            if (index > 1) { result += '\t'; }
            result.append(value, std::min(length, std::size_t{8192}));
            lua_pop(state, 1);
        }
        from(state).message(ScriptMessage::Kind::Output, std::move(result));
        return 0;
    }
    static int requireModule(lua_State* state) {
        std::string name;
        if (lua_istable(state, 1)) {
            lua_getfield(state, 1, "__module");
            name = luaL_checkstring(state, -1);
            lua_pop(state, 1);
        } else { name = luaL_checkstring(state, 1); }
        auto& self = from(state);
        if (!self.loadModule(name, state)) { luaL_error(state, "Cannot load module '%s' (see Output)", name.c_str()); }
        return 1;
    }
    std::filesystem::path resolve(const std::string& input) const {
        std::filesystem::path path(input);
        if (path.empty() || path.is_absolute() || path.has_root_name()) { throw std::invalid_argument("Asset path must be relative to assets"); }
        if (*path.begin() == "assets") { path = path.lexically_relative("assets"); }
        const auto resolved = std::filesystem::weakly_canonical(root / path);
        const auto relative = resolved.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") { throw std::invalid_argument("Asset path escapes assets directory"); }
        return resolved;
    }
    std::string moduleKey(const std::string& name) const {
        return registered.contains(name) ? name : resolve(name).lexically_relative(root).generic_string();
    }
    std::string readSource(const std::string& name) {
        if (const auto found = registered.find(name); found != registered.end()) { return found->second; }
        const auto path = resolve(name);
        watcher.watch(path);
        if (std::filesystem::file_size(path) > limits.sourceBytes) { throw std::invalid_argument("Script exceeds source size limit"); }
        std::ifstream file(path, std::ios::binary);
        if (!file) { throw std::runtime_error("Cannot open " + path.string()); }
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    bool protectedCall(lua_State* state, const int arguments, const int results, const int errorIndex) {
        const bool outermost = protectedDepth++ == 0U;
        if (outermost) {
            deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(limits.callbackMilliseconds));
        }
        const int result = lua_pcall(state, arguments, results, errorIndex);
        --protectedDepth;
        if (outermost) { deadline = Clock::time_point::max(); }
        if (result != LUA_OK) {
            const char* error = lua_tostring(state, -1);
            message(ScriptMessage::Kind::Error, error != nullptr ? error : "Luau memory limit exceeded");
            lua_pop(state, 1);
            return false;
        }
        return true;
    }
    // pcall protects the Lua callback, but host preparation can allocate too
    // (JSON arguments, instances, fields) or fail on a frozen script table.
    // Keep that entire boundary inside a protected C entry point. Reuse the
    // closure so every entity update does not add a new GC allocation.
    template<class Function> bool protectedHost(const std::string& context, Function&& function) {
        const int base = lua_gettop(vm);
        if (!lua_checkstack(vm, 3)) {
            message(ScriptMessage::Kind::Error, context + ": Luau stack/memory limit exceeded");
            return false;
        }
        HostOperation operation{[](void* data) { (*static_cast<std::remove_reference_t<Function>*>(data))(); }, &function};
        lua_getref(vm, hostRef);
        lua_pushlightuserdata(vm, &operation);
        const bool outermost = protectedDepth++ == 0U;
        if (outermost) deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(limits.callbackMilliseconds));
        const int result = lua_pcall(vm, 1, 0, 0);
        --protectedDepth;
        if (outermost) deadline = Clock::time_point::max();
        if (result != LUA_OK) {
            const char* error = lua_tostring(vm, -1);
            message(ScriptMessage::Kind::Error, context + ": " + (error ? error : "Luau memory limit exceeded"));
        }
        lua_settop(vm, base);
        return result == LUA_OK;
    }
    bool syncFields(const ecs::EntityId entity, const Json& fields) {
        const auto found = instances.find(entity);
        if (found == instances.end()) return false;
        const bool ok = protectedHost(found->second.path, [&] {
            lua_getref(vm, found->second.ref);
            if (lua_getreadonly(vm, -1)) luaL_error(vm, "Behavior self is frozen; the engine must update self.fields");
            pushJson(vm, fields);
            lua_rawsetfield(vm, -2, "fields");
        });
        if (ok) found->second.fields = fields;
        else found->second.failed = true;
        return ok;
    }
    bool evaluate(const std::string& name, const std::string& bytes, lua_State* state, const bool moduleEnvironment = true) {
        const auto error = compileError(bytes);
        if (!error.empty()) { message(ScriptMessage::Kind::Error, name + ":" + error); return false; }
        const int base = lua_gettop(state);
        lua_getref(state, errorRef);
        int environment = 0;
        try {
            const NativePreparation preparation(*this);
            if (moduleEnvironment) {
                lua_newtable(state); lua_newtable(state); lua_pushvalue(state, LUA_GLOBALSINDEX);
                lua_setfield(state, -2, "__index"); lua_setmetatable(state, -2); environment = base + 2;
            }
            if (luau_load(state, ("@" + name).c_str(), bytes.data(), bytes.size(), environment) != LUA_OK) {
                const char* loadError = lua_tostring(state, -1);
                message(ScriptMessage::Kind::Error, name + ": " + (loadError != nullptr ? loadError : "Luau memory limit exceeded"));
                lua_settop(state, base);
                return false;
            }
        } catch (const std::exception& exception) {
            lua_settop(state, base); message(ScriptMessage::Kind::Error, name + ": " + exception.what()); return false;
        }
        if (moduleEnvironment) { lua_remove(state, base + 2); }
        evaluating.push_back(name);
        const bool result = protectedCall(state, 0, 1, base + 1);
        evaluating.pop_back();
        lua_remove(state, base + 1);
        return result;
    }
    bool loadModule(const std::string& inputName, lua_State* state) {
        std::string name;
        try { name = moduleKey(inputName); }
        catch (const std::exception& error) { message(ScriptMessage::Kind::Error, inputName + ": " + error.what()); return false; }
        if (!evaluating.empty() && evaluating.back() != name) dependencies[evaluating.back()].insert(name);
        if (const auto found = modules.find(name); found != modules.end()) { lua_getref(state, found->second.ref); return true; }
        if (failedModules.contains(name)) { return false; }
        if (!loading.insert(name).second) { message(ScriptMessage::Kind::Error, "Circular require: " + name); return false; }
        std::string bytes;
        try {
            const NativePreparation preparation(*this);
            if (prepared && prepared->contains(name)) bytes = prepared->at(name);
            else bytes = compileSource(readSource(name));
        }
        catch (const std::exception& error) { message(ScriptMessage::Kind::Error, name + ": " + error.what()); loading.erase(name); failedModules.insert(name); return false; }
        const bool oldMutation = allowMutation;
        allowMutation = false;
        const bool result = evaluate(name, bytes, state);
        allowMutation = oldMutation;
        loading.erase(name);
        if (!result) { failedModules.insert(name); return false; }
        if (lua_isnil(state, -1)) { lua_pop(state, 1); lua_pushboolean(state, true); }
        const int ref = lua_ref(state, -1);
        modules.emplace(name, Module{ref, std::move(bytes)});
        return true;
    }
    bool alive(const ecs::EntityId entity) const { return world != nullptr && world->isAlive(entity) && !pendingDestroy.contains(entity); }
    void requireMutation(lua_State* state) const {
        if (!allowMutation || world == nullptr) { luaL_error(state, "World mutation is only available during Play or an explicit command"); }
    }
    static int api(lua_State* state) {
        auto& self = from(state);
        const auto operation = static_cast<Api>(lua_tointeger(state, lua_upvalueindex(1)));
        if (operation == Api::KeyDown || operation == Api::KeyPressed || operation == Api::KeyReleased) {
            const auto key = keyCode(luaL_checkstring(state, 1));
            const bool value = self.inputEnabled && (operation == Api::KeyDown ? platform::InputManager::isKeyDownRaw(key) :
                operation == Api::KeyPressed ? platform::InputManager::wasKeyPressedRaw(key) : platform::InputManager::wasKeyReleasedRaw(key));
            lua_pushboolean(state, value); return 1;
        }
        if (operation == Api::MousePosition) {
            const auto [x, y] = platform::InputManager::mousePosition();
            lua_pushnumber(state, x); lua_pushnumber(state, y); return 2;
        }
        if (operation == Api::Status) {
            const std::string key = luaL_checkstring(state, 1);
            self.status[key] = readJson(state, 2); return 0;
        }
        if (operation == Api::ReadStatus) {
            if (lua_isnoneornil(state, 1)) { pushJson(state, self.status); }
            else {
                const std::string key = luaL_checkstring(state, 1);
                const auto value = self.status.find(key);
                if (value == self.status.end()) { lua_pushnil(state); } else { pushJson(state, *value); }
            }
            return 1;
        }
        if (operation == Api::Spawn) {
            self.requireMutation(state);
            const std::string path = luaL_checkstring(state, 1);
            const Json overrides = lua_isnoneornil(state, 2) ? Json::object() : readJson(state, 2);
            try { lua_pushunsigned(state, self.spawnPrefab(path, overrides)); }
            catch (const std::exception& error) { luaL_error(state, "Prefab '%s': %s", path.c_str(), error.what()); }
            return 1;
        }
        if (operation == Api::Find || operation == Api::Radius) {
            const std::string tag = operation == Api::Find ? luaL_checkstring(state, 1) :
                lua_isnoneornil(state, 3) ? std::string{} : std::string(luaL_checkstring(state, 3));
            const glm::vec3 position = operation == Api::Radius ? checkVector(state, 1) : glm::vec3(0.0F);
            const double radius = operation == Api::Radius ? luaL_checknumber(state, 2) : 0.0;
            if (!std::isfinite(radius) || radius < 0.0) { luaL_error(state, "Radius must be finite and nonnegative"); }
            lua_newtable(state);
            int count = 0;
            self.world->forEachEntity([&](const ecs::EntityId entity) {
                if (!self.alive(entity)) { return; }
                const auto* entityTag = self.world->getComponent<ecs::Tag>(entity);
                if (!tag.empty() && (entityTag == nullptr || entityTag->value != tag)) { return; }
                if (operation == Api::Radius) {
                    const auto* transform = self.world->getComponent<ecs::Transform>(entity);
                    if (transform == nullptr || glm::distance(transform->position, position) > radius) { return; }
                }
                lua_pushunsigned(state, entity); lua_rawseti(state, -2, ++count);
            });
            return 1;
        }
        const auto entity = checkEntity(state, 1);
        if (operation == Api::Alive) { lua_pushboolean(state, self.alive(entity)); return 1; }
        if (!self.alive(entity)) { lua_pushnil(state); return 1; }
        if (operation == Api::Destroy) {
            self.requireMutation(state); self.pendingDestroy.insert(entity); return 0;
        }
        if (operation == Api::Tag) {
            const auto* tag = self.world->getComponent<ecs::Tag>(entity);
            if (tag == nullptr) { lua_pushnil(state); } else { lua_pushlstring(state, tag->value.data(), tag->value.size()); }
            return 1;
        }
        if (operation == Api::Position || operation == Api::SetPosition || operation == Api::Rotation || operation == Api::SetRotation) {
            auto* transform = self.world->getComponent<ecs::Transform>(entity);
            if (transform == nullptr) { lua_pushnil(state); return 1; }
            if (operation == Api::SetPosition) { self.requireMutation(state); transform->position = checkVector(state, 2); return 0; }
            if (operation == Api::SetRotation) { self.requireMutation(state); transform->rotationEulerRadians = checkVector(state, 2); return 0; }
            if (operation == Api::Rotation) { const auto& r = transform->rotationEulerRadians; lua_pushvector(state, r.x, r.y, r.z); return 1; }
            lua_pushvector(state, transform->position.x, transform->position.y, transform->position.z); return 1;
        }
        auto* body = self.world->getComponent<ecs::Rigidbody>(entity);
        if (body == nullptr) { lua_pushnil(state); return 1; }
        if (operation == Api::Velocity) { lua_pushvector(state, body->velocity.x, body->velocity.y, body->velocity.z); return 1; }
        self.requireMutation(state);
        if (operation == Api::SetVelocity) { body->velocity = checkVector(state, 2); body->wakeUp(); }
        else if (operation == Api::Force) { body->applyForce(checkVector(state, 2)); }
        return 0;
    }
    void bind(lua_State* state, const char* name, const Api operation) {
        lua_pushinteger(state, static_cast<int>(operation));
        lua_pushcclosure(state, &api, name, 1);
        lua_setfield(state, -2, name);
    }
    void installBindings() {
        lua_newtable(vm);
        bind(vm, "isAlive", Api::Alive); bind(vm, "getPosition", Api::Position); bind(vm, "setPosition", Api::SetPosition);
        bind(vm, "getRotation", Api::Rotation); bind(vm, "setRotation", Api::SetRotation);
        bind(vm, "getVelocity", Api::Velocity); bind(vm, "setVelocity", Api::SetVelocity); bind(vm, "applyForce", Api::Force);
        bind(vm, "getTag", Api::Tag); bind(vm, "find", Api::Find); bind(vm, "queryRadius", Api::Radius);
        bind(vm, "spawnPrefab", Api::Spawn); bind(vm, "destroy", Api::Destroy); bind(vm, "setStatus", Api::Status);
        bind(vm, "getStatus", Api::ReadStatus);
        lua_setglobal(vm, "Engine");
        lua_newtable(vm);
        bind(vm, "isKeyDown", Api::KeyDown); bind(vm, "wasKeyPressed", Api::KeyPressed); bind(vm, "wasKeyReleased", Api::KeyReleased);
        bind(vm, "mousePosition", Api::MousePosition); lua_setglobal(vm, "Input");
        lua_pushcfunction(vm, &print, "print"); lua_setglobal(vm, "print");
        lua_pushcfunction(vm, &requireModule, "require"); lua_setglobal(vm, "require");
        lua_pushcfunction(vm, &errorHandler, "traceback"); errorRef = lua_ref(vm, -1); lua_pop(vm, 1);
        lua_pushcfunction(vm, [](lua_State* state) -> int {
            auto& operation = *static_cast<HostOperation*>(lua_touserdata(state, 1));
            operation.invoke(operation.data);
            return 0;
        }, "protected host entry");
        hostRef = lua_ref(vm, -1); lua_pop(vm, 1);
    }
    std::vector<ScriptField> fieldDefinitions(const std::string& path) {
        std::vector<ScriptField> result;
        if (path.empty()) { return result; }
        if (!loadModule(path, vm)) { return result; }
        if (!lua_istable(vm, -1)) { lua_pop(vm, 1); return result; }
        lua_rawgetfield(vm, -1, "fields");
        const auto invalid = [&](const std::string& name, const std::string& reason) {
            const std::string diagnostic = path + ": fields" + (name.empty() ? std::string{} : "." + name) + ": " + reason;
            if (fieldErrors.insert(diagnostic).second) { message(ScriptMessage::Kind::Error, diagnostic); }
        };
        if (lua_istable(vm, -1)) {
            const Json definitions = readJson(vm, -1);
            for (const auto& [name, definition] : definitions.items()) {
                ScriptField field; field.name = name;
                if (definition.is_object()) {
                    const auto defaultValue = definition.find("default");
                    if (defaultValue == definition.end()) { invalid(name, "Declaration requires a scalar default"); continue; }
                    field.defaultValue = *defaultValue;
                    if (const auto type = definition.find("type"); type != definition.end()) {
                        if (!type->is_string()) { invalid(name, "type must be a string"); continue; }
                        field.type = type->get<std::string>();
                    }
                    bool validBounds = true;
                    const auto bound = [&](const char* key, double& value) {
                        if (const auto metadata = definition.find(key); metadata != definition.end()) {
                            if (!metadata->is_number() || !std::isfinite(metadata->get<double>())) {
                                invalid(name, std::string(key) + " must be a finite number"); validBounds = false;
                            } else { value = metadata->get<double>(); }
                        }
                    };
                    bound("min", field.minimum); bound("max", field.maximum);
                    if (!validBounds) { continue; }
                } else { field.defaultValue = definition; }
                if (field.type.empty()) {
                    field.type = field.defaultValue.is_boolean() ? "boolean" : field.defaultValue.is_number() ? "number" :
                        field.defaultValue.is_string() ? "string" : "invalid";
                }
                if (field.type == "bool") { field.type = "boolean"; }
                if (field.type == "integer") { field.type = "number"; field.integer = true; }
                const bool matches = field.type == "number" ? field.defaultValue.is_number() && std::isfinite(field.defaultValue.get<double>()) :
                    field.type == "boolean" ? field.defaultValue.is_boolean() : field.type == "string" && field.defaultValue.is_string();
                if (!matches) { invalid(name, "default must match supported type number, integer, boolean, bool or string"); continue; }
                if (field.minimum > field.maximum) { invalid(name, "min must not exceed max"); continue; }
                if (field.integer) {
                    field.minimum = std::ceil(field.minimum); field.maximum = std::floor(field.maximum);
                    if (field.minimum > field.maximum || std::round(field.defaultValue.get<double>()) != field.defaultValue.get<double>()) {
                        invalid(name, "integer declaration requires an integral default and a range containing an integer"); continue;
                    }
                }
                result.push_back(std::move(field));
            }
        } else if (!lua_isnil(vm, -1)) { invalid({}, "Declaration must be a table"); }
        lua_pop(vm, 2);
        return result;
    }
    void defaults(ScriptComponent& component) {
        if (component.path.empty()) { return; }
        try { component.path = moduleKey(component.path); }
        catch (const std::exception& error) { message(ScriptMessage::Kind::Error, component.path + ": " + error.what()); return; }
        behaviorPaths.insert(component.path);
        if (!component.fields.is_object()) { component.fields = Json::object(); }
        for (const auto& field : fieldDefinitions(component.path)) {
            auto found = component.fields.find(field.name);
            const bool valid = found != component.fields.end() &&
                (field.type == "number" ? found->is_number() : field.type == "boolean" ? found->is_boolean() : found->is_string());
            if (!valid) { component.fields[field.name] = field.defaultValue; }
            if (field.type == "number" && component.fields[field.name].is_number()) {
                double value = component.fields[field.name].get<double>();
                if (!std::isfinite(value)) { value = field.defaultValue.get<double>(); }
                value = std::clamp(value, field.minimum, field.maximum);
                component.fields[field.name] = field.integer ? std::round(value) : value;
            }
        }
    }
    void instance(const ecs::EntityId entity, ScriptComponent& component) {
        if (!component.enabled || component.path.empty() || !alive(entity)) { return; }
        if (instances.contains(entity)) { return; }
        int ref = LUA_NOREF;
        const bool ok = protectedHost(component.path, [&] {
        defaults(component);
        if (!loadModule(component.path, vm)) { return; }
        if (!lua_istable(vm, -1)) { message(ScriptMessage::Kind::Error, component.path + ": Behavior must return a table"); lua_pop(vm, 1); return; }
        lua_newtable(vm);
        lua_pushunsigned(vm, entity); lua_setfield(vm, -2, "entity");
        pushJson(vm, component.fields); lua_setfield(vm, -2, "fields");
        lua_newtable(vm); lua_setfield(vm, -2, "state");
        lua_newtable(vm); lua_pushvalue(vm, -3); lua_setfield(vm, -2, "__index"); lua_setmetatable(vm, -2);
        ref = lua_ref(vm, -1); lua_pop(vm, 2);
        });
        if (!ok || ref == LUA_NOREF) return;
        instances.emplace(entity, Instance{ref, component.path, false, component.fields});
        callback(entity, "Init", 0.0, nullptr);
    }
    void callback(const ecs::EntityId entity, const char* method, const double dt, const Json* event, const bool mutation = true) {
        ENGINE_PROFILE_ZONE("Luau Behavior Entry Point");
        const auto found = instances.find(entity);
        const bool cleanup = std::string(method) == "Destroy";
        if (found == instances.end() || (!cleanup && (found->second.failed || !alive(entity)))) { return; }
        const bool entered = protectedHost(found->second.path, [&] {
        const int base = lua_gettop(vm);
        lua_getref(vm, errorRef);
        const auto module = modules.find(found->second.path);
        if (module == modules.end()) { lua_settop(vm, base); return; }
        lua_getref(vm, module->second.ref);
        lua_rawgetfield(vm, -1, method);
        if (!lua_isfunction(vm, -1)) {
            lua_pop(vm, 1);
            std::string alternate(method); alternate.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(alternate.front())));
            lua_rawgetfield(vm, -1, alternate.c_str());
        }
        if (!lua_isfunction(vm, -1)) { lua_settop(vm, base); return; }
        lua_getref(vm, found->second.ref);
        lua_remove(vm, base + 2);
        int arguments = 1;
        if (event != nullptr) { pushJson(vm, *event); ++arguments; }
        else if (std::string(method) == "Update") { lua_pushnumber(vm, dt); ++arguments; }
        const bool oldMutation = allowMutation;
        allowMutation = mutation;
        const auto callbackPath = found->second.path;
        evaluating.push_back(callbackPath);
        const bool ok = protectedCall(vm, arguments, 0, base + 1);
        evaluating.pop_back();
        allowMutation = oldMutation;
        lua_settop(vm, base);
        // A callback can spawn other instances and rehash the map.
        if (!ok) { if (const auto current = instances.find(entity); current != instances.end()) { current->second.failed = true; } }
        });
        if (!entered) { if (const auto current = instances.find(entity); current != instances.end()) current->second.failed = true; }
    }
    void instantiateNew() {
        std::vector<ecs::EntityId> entities;
        world->forEach<ScriptComponent>([&](const ecs::EntityId entity, ScriptComponent&) { entities.push_back(entity); });
        std::sort(entities.begin(), entities.end());
        for (const auto entity : entities) {
            if (auto* component = world->getComponent<ScriptComponent>(entity)) { instance(entity, *component); }
        }
    }
    void flushDestroy() {
        const auto queued = std::move(pendingDestroy);
        pendingDestroy.clear();
        for (const auto entity : queued) {
            callback(entity, "Destroy", 0.0, nullptr, false);
            const auto found = instances.find(entity);
            if (found != instances.end()) { lua_unref(vm, found->second.ref); instances.erase(found); }
            if (world->isAlive(entity)) { if (destroy) { destroy(entity); } else { world->destroyEntity(entity); } }
        }
    }
    ecs::EntityId spawnPrefab(const std::string& path, const Json& overrides) {
        Json prefab;
        {
            const NativePreparation preparation(*this);
            const auto resolved = resolve(path);
            if (std::filesystem::file_size(resolved) > limits.sourceBytes) { throw std::invalid_argument("Prefab exceeds size limit"); }
            std::ifstream file(resolved);
            if (!file) { throw std::runtime_error("Cannot open prefab"); }
            file >> prefab;
        }
        if (!prefab.is_object()) { throw std::invalid_argument("Prefab must be an object"); }
        if (!overrides.is_object()) { throw std::invalid_argument("Prefab overrides must be an object"); }
        prefab.merge_patch(overrides);
        // Fully parse before touching the ECS: malformed data cannot leave half an entity.
        const auto& data = prefab.contains("components") ? prefab["components"] : prefab;
        ecs::Transform transform;
        if (data.contains("transform")) {
            const auto& item = data["transform"];
            transform.position = jsonVector(item.value("position", Json::array()), transform.position);
            transform.rotationEulerRadians = jsonVector(item.value("rotation", Json::array()), transform.rotationEulerRadians);
            transform.scale = jsonVector(item.value("scale", Json::array()), transform.scale);
        }
        std::optional<ecs::Rigidbody> rigidbody;
        if (data.contains("rigidbody")) {
            const auto& item = data["rigidbody"];
            rigidbody.emplace(); rigidbody->mass = item.value("mass", 1.0F);
            rigidbody->isStatic = item.value("isStatic", false); rigidbody->isKinematic = item.value("isKinematic", false);
            rigidbody->useGravity = item.value("useGravity", true);
            rigidbody->velocity = jsonVector(item.value("velocity", Json::array()), glm::vec3(0.0F));
            rigidbody->recalculateMassProperties();
        }
        std::optional<ecs::Collider> collider;
        if (data.contains("collider")) {
            const auto& item = data["collider"]; collider.emplace();
            collider->type = item.value("type", std::string("aabb")) == "sphere" ? ecs::ColliderType::Sphere : ecs::ColliderType::Aabb;
            collider->sphere.radius = std::max(0.001F, item.value("radius", 0.5F));
            collider->aabb.halfExtents = jsonVector(item.value("halfExtents", Json::array()), glm::vec3(0.5F));
            collider->enabled = item.value("enabled", true);
        }
        std::optional<ecs::MeshRenderer> mesh;
        if (data.contains("meshRenderer")) {
            const auto& item = data["meshRenderer"]; mesh.emplace();
            mesh->meshId = item.value("meshId", std::string("__diligent_cube__"));
            mesh->textureId = item.value("textureId", std::string("assets/textures/arena_wall.ppm"));
            mesh->shaderId = item.value("shaderId", std::string("assets/shaders_hlsl/textured.shader.json"));
            if (item.contains("tint") && item["tint"].is_array() && item["tint"].size() == 4U) {
                mesh->tint = {item["tint"][0].get<float>(), item["tint"][1].get<float>(), item["tint"][2].get<float>(), item["tint"][3].get<float>()};
            }
        }
        std::optional<ScriptComponent> script;
        if (data.contains("script")) {
            const auto& item = data["script"]; script.emplace();
            script->path = item.at("path").get<std::string>(); script->enabled = item.value("enabled", true);
            script->fields = item.value("fields", Json::object()); defaults(*script);
        }
        std::string tag = prefab.value("name", std::string("Prefab"));
        if (data.contains("tag")) { tag = data["tag"].is_string() ? data["tag"].get<std::string>() : data["tag"].value("value", tag); }
        const auto entity = world->createEntity();
        try {
            world->addComponent<ecs::Transform>(entity, transform); world->addComponent<ecs::Tag>(entity, ecs::Tag{tag});
            if (rigidbody) { world->addComponent<ecs::Rigidbody>(entity, *rigidbody); }
            if (collider) { world->addComponent<ecs::Collider>(entity, *collider); }
            if (mesh) { world->addComponent<ecs::MeshRenderer>(entity, *mesh); }
            if (script) { world->addComponent<ScriptComponent>(entity, *script); }
        } catch (...) { world->destroyEntity(entity); throw; }
        return entity;
    }
};

ScriptingSystem::ScriptingSystem() : m_impl(std::make_unique<Impl>()) {}
ScriptingSystem::~ScriptingSystem() { shutdown(); }
bool ScriptingSystem::initialize(ecs::World& world, core::JobSystem* jobs, std::filesystem::path assetRoot) {
    auto& self = *m_impl; self.requireOwner();
    if (self.vm != nullptr) { return true; }
    self.world = &world; self.jobs = jobs; self.root = std::filesystem::weakly_canonical(assetRoot);
    self.watcher.initialize(jobs);
    self.vm = lua_newstate(&Impl::allocate, &self);
    if (self.vm == nullptr) { self.message(ScriptMessage::Kind::Error, "Cannot create Luau VM"); return false; }
    lua_callbacks(self.vm)->userdata = &self; lua_callbacks(self.vm)->interrupt = &Impl::interrupt;
    luaL_openlibs(self.vm); self.installBindings(); luaL_sandbox(self.vm); luaL_sandboxthread(self.vm);
    self.message(ScriptMessage::Kind::Info, "Luau 0.741 ready (" + std::to_string(self.limits.memoryBytes / (1024U*1024U)) +
        " MiB / " + std::to_string(static_cast<int>(self.limits.callbackMilliseconds)) + " ms callback budget)");
    return true;
}
void ScriptingSystem::shutdown() {
    auto& self = *m_impl; self.requireOwner();
    self.watcher.clear();
    if (self.reloadJob.valid() && self.jobs != nullptr) { self.jobs->wait(self.reloadJob); }
    self.reload.reset(); self.reloadJob = {};
    self.reloadQueue.clear(); self.dependencies.clear(); self.evaluating.clear(); self.prepared = nullptr;
    stop(); self.modules.clear(); self.loading.clear(); self.failedModules.clear(); self.fieldErrors.clear(); self.behaviorPaths.clear();
    if (self.vm != nullptr) { lua_close(self.vm); self.vm = nullptr; }
    self.world = nullptr; self.jobs = nullptr;
}
void ScriptingSystem::setLimits(Limits limits) {
    m_impl->requireOwner();
    limits.memoryBytes = std::max(limits.memoryBytes, std::size_t{1024U * 1024U});
    limits.callbackMilliseconds = std::clamp(limits.callbackMilliseconds, 1.0, 10000.0);
    m_impl->limits = limits;
}
ScriptingSystem::Limits ScriptingSystem::limits() const { return m_impl->limits; }
void ScriptingSystem::setInputEnabled(const bool enabled) { m_impl->requireOwner(); m_impl->inputEnabled = enabled; }
void ScriptingSystem::setDestroyCallback(std::function<void(ecs::EntityId)> callback) { m_impl->requireOwner(); m_impl->destroy = std::move(callback); }
void ScriptingSystem::start() {
    auto& self = *m_impl; self.requireOwner(); if (self.vm == nullptr) { return; }
    stop();
    self.failedModules.clear();
    // Edits made on disk also take effect at the next Play without a manual reload.
    std::vector<std::string> paths;
    self.world->forEach<ScriptComponent>([&](const ecs::EntityId, const ScriptComponent& component) {
        if (!component.path.empty() && std::find(paths.begin(), paths.end(), component.path) == paths.end()) { paths.push_back(component.path); }
    });
    for (const auto& path : self.behaviorPaths) {
        if (!path.empty() && !self.registered.contains(path) && std::find(paths.begin(), paths.end(), path) == paths.end()) { paths.push_back(path); }
    }
    for (const auto& path : paths) {
        if (self.registered.contains(path)) { continue; }
        if (requestReload(path)) {
            if (self.reloadJob.valid() && self.jobs != nullptr) { self.jobs->wait(self.reloadJob); }
            pollReload();
        }
    }
    self.playing = true; self.status = Json::object(); self.instantiateNew(); self.flushDestroy();
}
void ScriptingSystem::stop() {
    auto& self = *m_impl; self.requireOwner();
    if (self.vm != nullptr) {
        std::vector<ecs::EntityId> entities;
        for (const auto& [entity, instance] : self.instances) { (void)instance; entities.push_back(entity); }
        for (const auto entity : entities) { self.callback(entity, "Destroy", 0.0, nullptr, false); }
        for (const auto& [entity, instance] : self.instances) { (void)entity; lua_unref(self.vm, instance.ref); }
    }
    self.instances.clear(); self.events.clear(); self.pendingDestroy.clear(); self.playing = false;
}
void ScriptingSystem::update(const double dt) {
    auto& self = *m_impl; self.requireOwner(); pollReload();
    if (!self.playing || self.vm == nullptr) { return; }
    std::vector<ecs::EntityId> entities;
    for (const auto& [entity, instance] : self.instances) { (void)instance; entities.push_back(entity); }
    std::sort(entities.begin(), entities.end());
    for (const auto entity : entities) {
        const auto* component = self.world->getComponent<ScriptComponent>(entity);
        if (!component || !component->enabled || !self.alive(entity) || self.instances.at(entity).path != component->path) forgetEntity(entity);
    }
    self.instantiateNew();
    entities.clear();
    for (const auto& [entity, instance] : self.instances) { (void)instance; entities.push_back(entity); }
    std::sort(entities.begin(), entities.end());
    for (const auto entity : entities) {
        const auto* component = self.world->getComponent<ScriptComponent>(entity);
        if (component == nullptr || !component->enabled || !self.alive(entity)) { forgetEntity(entity); continue; }
        auto& instance = self.instances.at(entity);
        if (!instance.failed && instance.fields != component->fields && !self.syncFields(entity, component->fields)) continue;
        self.callback(entity, "Update", std::clamp(dt, 0.0, 0.25), nullptr);
    }
    self.flushDestroy();
}
void ScriptingSystem::queueEvent(std::string type, Json payload) {
    auto& self = *m_impl; self.requireOwner(); if (!self.playing || self.events.size() >= 1024U) { return; }
    payload["type"] = type; self.events.emplace_back(std::move(type), std::move(payload));
}
void ScriptingSystem::dispatchEvents() {
    auto& self = *m_impl; self.requireOwner(); if (!self.playing || self.vm == nullptr) { return; }
    const auto events = std::move(self.events); self.events.clear();
    std::vector<ecs::EntityId> entities;
    for (const auto& [entity, instance] : self.instances) { (void)instance; entities.push_back(entity); }
    for (const auto& [type, payload] : events) {
        (void)type;
        for (const auto entity : entities) { self.callback(entity, "OnEvent", 0.0, &payload); }
    }
    self.flushDestroy();
}
void ScriptingSystem::forgetEntity(const ecs::EntityId entity) {
    auto& self = *m_impl; self.requireOwner();
    if (self.instances.contains(entity)) { self.callback(entity, "Destroy", 0.0, nullptr, false); }
    const auto found = self.instances.find(entity);
    if (found != self.instances.end()) { lua_unref(self.vm, found->second.ref); self.instances.erase(found); }
    // Releasing the behavior must not cancel an already requested ECS destroy.
    // update() also forgets later instances marked dead by an earlier callback;
    // flushDestroy() still owns those entities until the safe mutation boundary.
}
bool ScriptingSystem::loadBehavior(const std::string& path) {
    auto& self = *m_impl; self.requireOwner(); if (self.vm == nullptr) { return false; }
    bool loaded = false;
    const bool ok = self.protectedHost(path, [&] {
        loaded = self.loadModule(path, self.vm);
        if (loaded) lua_pop(self.vm, 1);
    });
    return ok && loaded;
}
std::vector<ScriptField> ScriptingSystem::fields(const std::string& path) {
    auto& self = *m_impl; self.requireOwner(); if (self.vm == nullptr) { return {}; }
    std::vector<ScriptField> result;
    if (!self.protectedHost(path, [&] { result = self.fieldDefinitions(path); })) result.clear();
    return result;
}
void ScriptingSystem::populateDefaults(ScriptComponent& component) {
    auto& self = *m_impl; self.requireOwner();
    if (self.vm) self.protectedHost(component.path, [&] { self.defaults(component); });
}
bool ScriptingSystem::requestReload(const std::string& path) {
    auto& self = *m_impl; self.requireOwner(); if (self.vm == nullptr) { return false; }
    std::string key;
    try { key = self.moduleKey(path); if (self.registered.contains(key)) return false; }
    catch (const std::exception& error) { self.message(ScriptMessage::Kind::Error, error.what()); return false; }
    self.reloadQueue.insert(key);
    if (self.reload) return true;
    // Re-evaluate consumers too: their local require() bindings may contain functions/scalars.
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto& [parent, children] : self.dependencies) {
            if (self.registered.contains(parent) || !self.modules.contains(parent)) continue;
            for (const auto& child : children) if (self.reloadQueue.contains(child)) {
                expanded |= self.reloadQueue.insert(parent).second; break;
            }
        }
    }
    auto result = std::make_shared<Impl::Reload>();
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    try { for (const auto& name : self.reloadQueue) { const auto file = self.resolve(name); self.watcher.watch(file); files.emplace_back(name, file); } }
    catch (const std::exception& error) { self.message(ScriptMessage::Kind::Error, error.what()); return false; }
    self.reloadQueue.clear();
    const auto sourceLimit = self.limits.sourceBytes;
    const auto compile = [result, files = std::move(files), sourceLimit] {
        ENGINE_PROFILE_ZONE("Luau Hot Reload Read and Compile");
        for (const auto& [name, resolved] : files) try {
            if (std::filesystem::file_size(resolved) > sourceLimit) { throw std::runtime_error("Script exceeds source size limit"); }
            std::ifstream file(resolved, std::ios::binary);
            if (!file) { throw std::runtime_error("Cannot read script"); }
            auto bytes = compileSource(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
            const auto error = compileError(bytes);
            if (!error.empty()) { result->error = name + ": " + error; break; }
            result->bytecode.emplace(name, std::move(bytes));
        } catch (const std::exception& error) { result->error = name + ": " + error.what(); break; }
    };
    self.reload = result;
    if (self.jobs != nullptr) { self.reloadJob = self.jobs->dispatch(compile, core::JobSystem::Priority::Low); }
    else { compile(); }
    return true;
}
void ScriptingSystem::pollReload() {
    auto& self = *m_impl; self.requireOwner(); if (!self.vm) return;
    for (const auto& change : self.watcher.poll()) {
        const auto key = change.path.lexically_relative(self.root).generic_string();
        self.reloadQueue.insert(key);
    }
    if (!self.reload && !self.reloadQueue.empty()) requestReload(*self.reloadQueue.begin());
    if (!self.reload) return;
    if (self.reloadJob.valid() && self.jobs != nullptr) {
        if (!self.jobs->isComplete(self.reloadJob)) { return; }
        self.jobs->wait(self.reloadJob);
    }
    auto reload = std::move(self.reload); self.reloadJob = {};
    if (!reload->error.empty()) {
        self.message(ScriptMessage::Kind::Error, "Live reload retained previous code: " + reload->error);
        return;
    }
    const auto previous = self.modules;
    const auto previousDependencies = self.dependencies;
    const auto previousFailed = self.failedModules;
    const int base = lua_gettop(self.vm);
    for (const auto& [name, bytes] : reload->bytecode) {
        (void)bytes; self.modules.erase(name); self.dependencies.erase(name); self.failedModules.erase(name);
    }
    self.prepared = &reload->bytecode;
    bool ok = true;
    const bool evaluated = self.protectedHost("Live module reload", [&] {
    for (const auto& [name, bytes] : reload->bytecode) {
        (void)bytes;
        if (!self.loadModule(name, self.vm)) { ok = false; break; }
        if (self.behaviorPaths.contains(name) && !lua_istable(self.vm, -1)) ok = false;
        lua_pop(self.vm, 1);
        if (!ok) { self.message(ScriptMessage::Kind::Error, name + ": Behavior must return a table"); break; }
    }
    });
    ok = ok && evaluated;
    self.prepared = nullptr;
    lua_settop(self.vm, base);
    if (!ok) {
        for (const auto& [name, module] : self.modules) {
            const auto old = previous.find(name);
            if (old == previous.end() || old->second.ref != module.ref) lua_unref(self.vm, module.ref);
        }
        self.modules = previous; self.dependencies = previousDependencies; self.failedModules = previousFailed;
        self.message(ScriptMessage::Kind::Error, "Live reload failed; previous code retained"); return;
    }
    // Give React a mapping of exported component functions, including nested exports.
    self.protectedHost("Live React export refresh", [&] {
    const int refreshBase = lua_gettop(self.vm);
    lua_newtable(self.vm);
    const int replacements = lua_gettop(self.vm);
    std::unordered_set<const void*> visited;
    unsigned refreshBudget = 4096;
    const auto mapExports = [&](auto&& visit, const int oldValue, const int newValue, const int depth) -> void {
        if (depth > 6 || refreshBudget == 0 || lua_rawequal(self.vm, oldValue, newValue)) return;
        --refreshBudget;
        if ((lua_istable(self.vm, oldValue) || lua_isfunction(self.vm, oldValue)) && !visited.insert(lua_topointer(self.vm, oldValue)).second) return;
        if (lua_isfunction(self.vm, oldValue) && lua_isfunction(self.vm, newValue)) {
            lua_pushvalue(self.vm, oldValue); lua_pushvalue(self.vm, newValue); lua_rawset(self.vm, replacements);
            // Module-local React components are commonly captured by Init/Update.
            // Pair named upvalues, rather than rerunning Init and spawning a second game.
            if (!lua_iscfunction(self.vm, oldValue) && !lua_iscfunction(self.vm, newValue)) {
                for (int i=1; i<=200; ++i) {
                    const char* oldName = lua_getupvalue(self.vm, oldValue, i);
                    if (!oldName) break;
                    const int oldUpvalue = lua_gettop(self.vm);
                    for (int j=1; j<=200; ++j) {
                        const char* newName = lua_getupvalue(self.vm, newValue, j);
                        if (!newName) break;
                        const bool matches = *oldName && std::strcmp(oldName, newName) == 0;
                        if (matches) visit(visit, oldUpvalue, lua_gettop(self.vm), depth + 1);
                        lua_pop(self.vm, 1);
                        if (matches) break;
                    }
                    lua_pop(self.vm, 1);
                }
            }
        } else if (lua_istable(self.vm, oldValue) && lua_istable(self.vm, newValue)) {
            lua_pushnil(self.vm);
            while (lua_next(self.vm, oldValue)) {
                lua_pushvalue(self.vm, -2); lua_rawget(self.vm, newValue);
                visit(visit, lua_absindex(self.vm, -2), lua_absindex(self.vm, -1), depth + 1);
                lua_pop(self.vm, 2);
            }
        }
    };
    for (const auto& [name, bytes] : reload->bytecode) {
        (void)bytes;
        if (const auto old = previous.find(name); old != previous.end()) {
            lua_getref(self.vm, old->second.ref); lua_getref(self.vm, self.modules.at(name).ref);
            mapExports(mapExports, lua_absindex(self.vm, -2), lua_absindex(self.vm, -1), 0);
            lua_pop(self.vm, 2);
        }
    }
    lua_getref(self.vm, self.errorRef); const int errorIndex = lua_gettop(self.vm);
    lua_getglobal(self.vm, "__turboUIRefresh");
    if (lua_isfunction(self.vm, -1)) {
        lua_pushvalue(self.vm, replacements); self.protectedCall(self.vm, 1, 0, errorIndex);
    }
    lua_settop(self.vm, refreshBase);
    });
    for (const auto& [name, bytes] : reload->bytecode) {
        (void)bytes;
        if (const auto old = previous.find(name); old != previous.end()) lua_unref(self.vm, old->second.ref);
        self.message(ScriptMessage::Kind::Info, "Reloaded " + name + " (runtime state preserved)");
    }
    self.fieldErrors.clear();
    self.world->forEach<ScriptComponent>([&](const ecs::EntityId, ScriptComponent& component) {
        if (reload->bytecode.contains(component.path)) self.defaults(component);
    });
    // Preserve self.state and update the method table; declared scene fields remain data-owned.
    for (auto& [entity, instance] : self.instances) {
        if (!reload->bytecode.contains(instance.path)) { continue; }
        const bool attached = self.protectedHost(instance.path, [&] {
            lua_getref(self.vm, instance.ref);
            if (lua_getreadonly(self.vm, -1)) luaL_error(self.vm, "Behavior self is frozen; the engine must attach reloaded methods");
            lua_newtable(self.vm); lua_getref(self.vm, self.modules.at(instance.path).ref);
            lua_setfield(self.vm, -2, "__index"); lua_setmetatable(self.vm, -2);
        });
        instance.failed = !attached;
        if (!attached) continue;
        if (const auto* component = self.world->getComponent<ScriptComponent>(entity)) {
            self.syncFields(entity, component->fields);
        }
    }
    std::vector<ecs::EntityId> reloaded;
    for (const auto& [entity, instance] : self.instances) if (reload->bytecode.contains(instance.path)) reloaded.push_back(entity);
    for (const auto entity : reloaded) self.callback(entity, "OnReload", 0.0, nullptr);
    self.flushDestroy();
}
bool ScriptingSystem::reloadPending() const { return m_impl->reload != nullptr || !m_impl->reloadQueue.empty(); }
bool ScriptingSystem::callGlobal(const char* name, const double argument) {
    auto& self = *m_impl; self.requireOwner(); if (!self.vm) return false;
    const int base = lua_gettop(self.vm);
    lua_getref(self.vm, self.errorRef);
    lua_getglobal(self.vm, name);
    bool ok = true;
    if (lua_isfunction(self.vm, -1)) {
        lua_pushnumber(self.vm, argument);
        const bool mutation = self.allowMutation; self.allowMutation = self.playing;
        ok = self.protectedCall(self.vm, 1, 0, base + 1); self.allowMutation = mutation;
    }
    lua_settop(self.vm, base); return ok;
}
bool ScriptingSystem::analyze(const std::string& source, const std::string& chunkName) {
    auto& self = *m_impl; self.requireOwner();
    if (source.size() > self.limits.sourceBytes) { self.message(ScriptMessage::Kind::Error, chunkName + ": Source size limit exceeded"); return false; }
    try {
        const auto error = compileError(compileSource(source));
        self.message(error.empty() ? ScriptMessage::Kind::Info : ScriptMessage::Kind::Error, error.empty() ? chunkName + ": Syntax valid" : chunkName + ":" + error);
        return error.empty();
    } catch (const std::exception& error) { self.message(ScriptMessage::Kind::Error, chunkName + ": " + error.what()); return false; }
}
bool ScriptingSystem::execute(const std::string& source, const std::string& chunkName) {
    auto& self = *m_impl; self.requireOwner(); if (self.vm == nullptr || source.size() > self.limits.sourceBytes) { return false; }
    const bool oldMutation = self.allowMutation; self.allowMutation = true;
    bool ok = false;
    try { ok = self.evaluate(chunkName, compileSource(source), self.vm, false); }
    catch (const std::exception& error) { self.message(ScriptMessage::Kind::Error, chunkName + ": " + error.what()); }
    self.allowMutation = oldMutation;
    if (ok) { lua_pop(self.vm, 1); } self.flushDestroy(); return ok;
}
void ScriptingSystem::registerModule(std::string name, std::string source) {
    auto& self = *m_impl; self.requireOwner();
    self.failedModules.erase(name);
    self.fieldErrors.clear();
    if (const auto found = self.modules.find(name); found != self.modules.end()) {
        lua_unref(self.vm, found->second.ref); self.modules.erase(found);
    }
    self.registered[std::move(name)] = std::move(source);
}
lua_State* ScriptingSystem::state() { m_impl->requireOwner(); return m_impl->vm; }
ecs::EntityId ScriptingSystem::spawnPrefab(const std::string& path, const Json& overrides) {
    auto& self = *m_impl; self.requireOwner();
    if (self.world == nullptr || self.vm == nullptr) { self.message(ScriptMessage::Kind::Error, "Initialize scripting before spawning a prefab"); return ecs::kInvalidEntity; }
    try { return self.spawnPrefab(path, overrides); }
    catch (const std::exception& error) { self.message(ScriptMessage::Kind::Error, path + ": " + error.what()); return ecs::kInvalidEntity; }
}
const std::vector<ScriptMessage>& ScriptingSystem::output() const { return m_impl->messages; }
void ScriptingSystem::clearOutput() { m_impl->requireOwner(); m_impl->messages.clear(); }
Json ScriptingSystem::inspect(const ecs::EntityId entity) const {
    auto& self = *m_impl; self.requireOwner(); const auto found = self.instances.find(entity);
    if (self.vm == nullptr || found == self.instances.end()) { return Json::object(); }
    lua_getref(self.vm, found->second.ref); Json value = readJson(self.vm, -1); lua_pop(self.vm, 1);
    value["script"] = found->second.path; value["failed"] = found->second.failed; return value;
}
const Json& ScriptingSystem::status() const { return m_impl->status; }
bool ScriptingSystem::playing() const { return m_impl->playing; }
std::size_t ScriptingSystem::memoryUsed() const { return m_impl->allocated; }

} // namespace engine::scripting
