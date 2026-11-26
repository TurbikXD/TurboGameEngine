#include "engine/scripting/ScriptingSystem.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>
#include "engine/core/AtomicFile.h"
#include "engine/core/JobSystem.h"
#include "engine/core/LiveFileWatcher.h"
#include "engine/core/Log.h"
#include "engine/core/Profiling.h"
#include "engine/core/ProjectManager.h"
#include "engine/ecs/components.h"
#include "engine/ecs/light_json.h"
#include "engine/ecs/render_interpolation.h"
#include "engine/ecs/world.h"
#include "engine/network/PeerName.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/scripting/ScriptAudio.h"

namespace engine::scripting {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr double MinMouseSensitivity = 0.25;
constexpr double MaxMouseSensitivity = 4.0;

std::string utf8Path(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

std::string readAssetBytes(const std::filesystem::path& path, const std::size_t limit, const char* kind) {
    if (std::filesystem::file_size(path) > limit) throw std::invalid_argument(std::string(kind) + " exceeds source size limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + utf8Path(path));
    std::string contents;
    char chunk[16384];
    while (input) {
        input.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > limit - contents.size()) throw std::invalid_argument(std::string(kind) + " exceeds source size limit");
        contents.append(chunk, count);
    }
    if (!input.eof()) throw std::runtime_error("Cannot read " + utf8Path(path));
    return contents;
}

std::string compileSource(const std::string& source) {
    Luau::CompileOptions options;
    options.optimizationLevel = 1;
    options.debugLevel = 2;
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
            lua_pushlstring(state, key.data(), key.size());
            pushJson(state, item);
            lua_rawset(state, -3);
        }
    } else if (value.is_array()) {
        lua_createtable(state, static_cast<int>(value.size()), 0);
        for (std::size_t index = 0; index < value.size(); ++index) {
            pushJson(state, value[index]);
            lua_rawseti(state, -2, static_cast<int>(index + 1U));
        }
    } else { lua_pushnil(state); }
}

Json readJson(lua_State* state, const int index, const unsigned depth = 0U, std::size_t* remaining = nullptr,
              const bool preserveArrays = false) {
    std::size_t budget = 512U;
    if (remaining == nullptr) { remaining = &budget; }
    if (depth > 8U) { return "<depth limit>"; }
    if (*remaining == 0U) { return "<item limit>"; }
    --*remaining;
    switch (lua_type(state, index)) {
    case LUA_TNIL: return nullptr;
    case LUA_TBOOLEAN: return lua_toboolean(state, index) != 0;
    case LUA_TNUMBER: return lua_tonumber(state, index);
    case LUA_TSTRING: {
        std::size_t length{};
        const char* text=lua_tolstring(state,index,&length);
        return std::string(text,length);
    }
    case LUA_TVECTOR: {
        const auto* vector = lua_tovector(state, index);
        return Json::array({vector[0], vector[1], vector[2]});
    }
    case LUA_TTABLE: {
        Json value = Json::object();
        const int absolute = lua_absindex(state, index);
        const int arrayLength = preserveArrays ? lua_objlen(state, absolute) : 0;
        bool dense = arrayLength > 0 && arrayLength <= 128;
        lua_pushnil(state);
        std::size_t count = 0;
        while (lua_next(state, absolute) != 0) {
            if (++count > 128U) { lua_pop(state, 2); break; }
            if (lua_type(state, -2) == LUA_TSTRING) {
                std::size_t length{};
                const char* text=lua_tolstring(state,-2,&length);
                const std::string key(text,length);
                value[key] = readJson(state, -1, depth + 1U, remaining, preserveArrays);
                dense = false;
            } else if (lua_type(state, -2) == LUA_TNUMBER) {
                const auto key = lua_tointeger(state, -2);
                dense = dense && key >= 1 && key <= arrayLength && lua_tonumber(state, -2) == key;
                value[std::to_string(key)] = readJson(state, -1, depth + 1U, remaining, preserveArrays);
            } else {
                dense = false;
            }
            lua_pop(state, 1);
            if (*remaining == 0U) { lua_pop(state, 1); break; }
        }
        if (dense && count == static_cast<std::size_t>(arrayLength) && value.size() == count) {
            Json array = Json::array();
            for (int item = 1; item <= arrayLength; ++item) array.push_back(std::move(value[std::to_string(item)]));
            return array;
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

template<class T>
std::optional<T> networkInteger(lua_State* state, const int index, const T minimum,
                                const T maximum = std::numeric_limits<T>::max()) {
    if (lua_type(state, index) != LUA_TNUMBER) return std::nullopt;
    const double number = lua_tonumber(state, index);
    if (!std::isfinite(number) || number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum) || std::trunc(number) != number) return std::nullopt;
    return static_cast<T>(number);
}

template<class T>
T checkNetworkInteger(lua_State* state, const int index, const T minimum,
                      const T maximum = std::numeric_limits<T>::max()) {
    const auto value = networkInteger(state, index, minimum, maximum);
    if (!value) luaL_argerror(state, index, "expected an integer in range");
    return *value;
}

uint32_t checkNetworkKey(lua_State* state, const int index) {
    return checkNetworkInteger<uint32_t>(state, index, 1, 0x7fffffff);
}

float checkNetworkAxis(lua_State* state, const int index) {
    luaL_checktype(state, index, LUA_TNUMBER);
    const double number = lua_tonumber(state, index);
    if (!std::isfinite(number) || number < -1.0 || number > 1.0) {
        luaL_argerror(state, index, "expected an input axis between -1 and 1");
    }
    return static_cast<float>(number);
}

std::string_view checkSteamString(lua_State* state, int index, std::size_t maximum,
                                  bool required, int argument) {
    if (lua_type(state, index) != LUA_TSTRING) luaL_argerror(state, argument, "expected a string");
    std::size_t length{};
    const auto* text = lua_tolstring(state, index, &length);
    const std::string_view value(text, length);
    if ((required && value.empty()) || value.size() > maximum || value.find('\0') != value.npos) {
        luaL_argerror(state, argument, "string is out of range or contains NUL");
    }
    return value;
}

std::uint64_t checkSteamId(lua_State* state, int index) {
    const auto text = checkSteamString(state, index, 20, true, index);
    std::uint64_t id{};
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, id);
    if (text.front() == '0' || parsed.ec != std::errc{} || parsed.ptr != end || !id) {
        luaL_argerror(state, index, "expected a decimal Steam ID string");
    }
    return id;
}

unsigned steamPageLimit(lua_State* state, int index) {
    return lua_isnoneornil(state, index) ? 32U : checkNetworkInteger<unsigned>(state, index, 1, 32);
}

ScriptSteamOptions checkSteamOptions(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    ScriptSteamOptions options;
    lua_rawgetfield(state, 1, "name");
    if (!lua_isnil(state, -1)) options.name = checkSteamString(state, -1, 64, false, 1);
    lua_pop(state, 1);

    lua_rawgetfield(state, 1, "visibility");
    if (!lua_isnil(state, -1)) {
        const auto visibility = checkSteamString(state, -1, 7, true, 1);
        if (visibility != "friends" && visibility != "private" && visibility != "public") {
            luaL_argerror(state, 1, "visibility must be friends, private, or public");
        }
        options.visibility = visibility;
    }
    lua_pop(state, 1);

    lua_rawgetfield(state, 1, "maxPlayers");
    if (!lua_isnil(state, -1)) {
        const auto maxPlayers = networkInteger<unsigned>(state, -1, 2, 32);
        if (!maxPlayers) luaL_argerror(state, 1, "maxPlayers must be an integer from 2 to 32");
        options.maxPlayers = *maxPlayers;
    }
    lua_pop(state, 1);

    lua_rawgetfield(state, 1, "joinable");
    if (!lua_isnil(state, -1)) {
        if (!lua_isboolean(state, -1)) luaL_argerror(state, 1, "joinable must be a boolean");
        options.joinable = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);

    lua_rawgetfield(state, 1, "properties");
    if (!lua_isnil(state, -1)) {
        if (!lua_istable(state, -1)) luaL_argerror(state, 1, "properties must be a table of strings");
        lua_pushnil(state);
        while (lua_next(state, -2)) {
            if (options.properties.size() == 8) luaL_argerror(state, 1, "at most 8 properties are supported");
            const auto key = checkSteamString(state, -2, 32, true, 1);
            if (key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != key.npos) {
                luaL_argerror(state, 1, "property keys must use ASCII letters, digits, underscore, or hyphen");
            }
            const auto value = checkSteamString(state, -1, 128, false, 1);
            options.properties.emplace(key, value);
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    return options;
}

platform::KeyCode keyCode(const std::string& name) {
    using Key = platform::KeyCode;
    static const std::unordered_map<std::string, Key> keys{
        {"W", Key::W}, {"A", Key::A}, {"S", Key::S}, {"D", Key::D}, {"Q", Key::Q}, {"E", Key::E},
        {"F", Key::F}, {"G", Key::G}, {"N", Key::N}, {"O", Key::O}, {"R", Key::R}, {"V", Key::V},
        {"X", Key::X}, {"Y", Key::Y}, {"Z", Key::Z}, {"1", Key::Digit1}, {"2", Key::Digit2},
        {"Space", Key::Space}, {"Enter", Key::Enter}, {"Escape", Key::Escape}, {"Tab", Key::Tab},
        {"Left", Key::Left}, {"Right", Key::Right}, {"Up", Key::Up}, {"Down", Key::Down},
        {"LeftShift", Key::LeftShift}, {"RightShift", Key::RightShift}, {"LeftControl", Key::LeftControl},
        {"RightControl", Key::RightControl}, {"LeftAlt", Key::LeftAlt}, {"RightAlt", Key::RightAlt}
    };
    const auto found = keys.find(name);
    return found == keys.end() ? Key::Unknown : found->second;
}

platform::MouseButton mouseButton(const std::string_view name) {
    using Button = platform::MouseButton;
    if (name == "Left") return Button::Left;
    if (name == "Right") return Button::Right;
    if (name == "Middle") return Button::Middle;
    return Button::Count;
}
}

struct ScriptingSystem::Impl final {
    struct Module final { int ref{LUA_NOREF}; };
    struct Instance final { int ref{LUA_NOREF}; std::string path; bool failed{false}; Json fields; };
    struct Reload final { std::unordered_map<std::string, std::string> bytecode; std::string error; };
    enum class Api : int { Alive, Position, SetPosition, Rotation, SetRotation, Velocity, SetVelocity, Force, Tag, Find, Radius,
        Spawn, Destroy, Status, ReadStatus, KeyDown, KeyPressed, KeyReleased, MousePosition, MouseDelta, MouseDown,
        MousePressed, CursorLock, CursorLocked, SetCameraFov, PlaySound, MouseSensitivity, SetMouseSensitivity,
        SetRenderInterpolation, SnapRenderInterpolation };
    enum class NetworkApi : int {
        Role, Connected, LocalPeer, IsHost, UsesSteam, Peers, Stats, Bind, Unbind, RegisterPrefab, Entity, SetState, State,
        RawState, LocalMotion, NextInputSequence, SetPeerMotion,
        SetInput, Input, SetControls, Controls, SetPeerValue, PeerValue, NormalizePeerName, SetPeerName, PeerName, PeerAvatar, Request
    };
    enum class SteamQuery { Available, LocalUser, Friends, Session, Sessions, Avatar };
    struct RpcInvocation final {
        uint32_t peer;
        uint8_t action;
        int32_t argument;
        std::string_view text;
        ScriptRpcReply reply{};
    };

    Limits limits;
    std::size_t allocated{};
    lua_State* vm{};
    ecs::World* world{};
    core::JobSystem* jobs{};
    ScriptNetwork* network{};
    ScriptSteam* steam{};
    platform::Window* inputWindow{};
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
    std::vector<Json> events;
    std::vector<ScriptMessage> messages;
    ScriptAudio audio;
    Json status = Json::object();
    bool playing{};
    bool allowMutation{};
    bool allowCleanupDestroy{};
    bool flushingDestroy{};
    bool inputEnabled{true};
    bool pointerInputEnabled{true};
    bool ownsCursorLock{};
    std::optional<double> mouseSensitivity;
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
        std::filesystem::path path = std::filesystem::u8path(input);
        if (path.empty() || path.is_absolute() || path.has_root_name()) { throw std::invalid_argument("Asset path must be relative to assets"); }
        if (*path.begin() == "assets") { path = path.lexically_relative("assets"); }
        const auto resolved = std::filesystem::weakly_canonical(root / path);
        const auto relative = resolved.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") { throw std::invalid_argument("Asset path escapes assets directory"); }
        return resolved;
    }
    std::string moduleKey(const std::string& name) const {
        return registered.contains(name) ? name : utf8Path(resolve(name).lexically_relative(root));
    }
    std::string readSource(const std::string& name) {
        if (const auto found = registered.find(name); found != registered.end()) { return found->second; }
        const auto path = resolve(name);
        watcher.watch(path);
        return readAssetBytes(path, limits.sourceBytes, "Script");
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
        modules.emplace(name, Module{ref});
        return true;
    }
    bool alive(const ecs::EntityId entity) const { return world != nullptr && world->isAlive(entity) && !pendingDestroy.contains(entity); }
    void requireMutation(lua_State* state) const {
        if (!allowMutation || world == nullptr) { luaL_error(state, "World mutation is only available during Play or an explicit command"); }
    }
    bool hasPointerInput() const {
        return inputEnabled && pointerInputEnabled && (!inputWindow || inputWindow->focused());
    }
    bool cursorLocked() const {
        return ownsCursorLock && inputWindow && inputWindow->cursorMode() == platform::CursorMode::Disabled;
    }
    bool releaseCursorLock() {
        const bool locked = cursorLocked();
        ownsCursorLock = false;
        if (locked) {
            inputWindow->setCursorMode(platform::CursorMode::Normal);
            platform::InputManager::resetMouseDelta();
        }
        return locked;
    }
    std::filesystem::path mouseSettingsPath() const {
        std::uint64_t identity = 14695981039346656037ULL;
        for (const unsigned char byte : utf8Path(root)) {
            identity = (identity ^ byte) * 1099511628211ULL;
        }
        return core::ProjectManager::defaultRegistryPath().parent_path() / "player-input" /
            (std::to_string(identity) + ".json");
    }
    double loadMouseSensitivity() {
        if (mouseSensitivity) return *mouseSensitivity;
        mouseSensitivity = 1.0;
        try {
            const NativePreparation preparation(*this);
            const auto data = Json::parse(readAssetBytes(mouseSettingsPath(), 128, "Mouse settings"), nullptr, false);
            if (data.is_number()) {
                const auto value = data.get<double>();
                if (std::isfinite(value) && value >= MinMouseSensitivity && value <= MaxMouseSensitivity) {
                    mouseSensitivity = value;
                }
            }
        } catch (const std::exception&) {}
        return *mouseSensitivity;
    }
    bool saveMouseSensitivity(const double value) {
        if (!playing || !inputWindow) return false;
        mouseSensitivity = value;
        try {
            const NativePreparation preparation(*this);
            const auto path = mouseSettingsPath();
            std::filesystem::create_directories(path.parent_path());
            std::string error;
            return core::writeFileAtomically(path, Json(value).dump(), error);
        } catch (const std::exception&) { return false; }
    }
    static int api(lua_State* state) {
        auto& self = from(state);
        const auto operation = static_cast<Api>(lua_tointeger(state, lua_upvalueindex(1)));
        if (operation == Api::KeyDown || operation == Api::KeyPressed || operation == Api::KeyReleased) {
            const auto key = keyCode(luaL_checkstring(state, 1));
            using Input = platform::InputManager;
            bool value = false;
            if (self.inputEnabled) {
                const bool fixedInput = Input::fixedTickActive() || (self.network && self.network->role() == "client");
                if (operation == Api::KeyDown) value = Input::isKeyDownRaw(key);
                else if (operation == Api::KeyPressed) {
                    value = fixedInput ? Input::wasKeyPressedForTickRaw(key) : Input::wasKeyPressedRaw(key);
                } else {
                    value = fixedInput ? Input::wasKeyReleasedForTickRaw(key) : Input::wasKeyReleasedRaw(key);
                }
            }
            lua_pushboolean(state, value); return 1;
        }
        if (operation == Api::MousePosition) {
            const auto [x, y] = platform::InputManager::mousePosition();
            lua_pushnumber(state, x); lua_pushnumber(state, y); return 2;
        }
        if (operation == Api::MouseDelta) {
            const auto [x, y] = self.hasPointerInput() ? platform::InputManager::mouseDeltaForTickRaw() :
                std::pair{0.0, 0.0};
            lua_pushnumber(state, x); lua_pushnumber(state, y); return 2;
        }
        if (operation == Api::MouseSensitivity) {
            lua_pushnumber(state, self.loadMouseSensitivity()); return 1;
        }
        if (operation == Api::SetMouseSensitivity) {
            const auto value = luaL_checknumber(state, 1);
            if (!std::isfinite(value) || value < MinMouseSensitivity || value > MaxMouseSensitivity) {
                luaL_argerror(state, 1, "Mouse sensitivity must be between 0.25 and 4");
            }
            lua_pushboolean(state, self.saveMouseSensitivity(value)); return 1;
        }
        if (operation == Api::MouseDown || operation == Api::MousePressed) {
            size_t size = 0;
            const char* name = luaL_checklstring(state, 1, &size);
            const auto button = mouseButton(std::string_view{name, size});
            using Input = platform::InputManager;
            lua_pushboolean(state, self.hasPointerInput() && (operation == Api::MouseDown ?
                Input::isMouseButtonDownRaw(button) : Input::wasMouseButtonPressedForTickRaw(button)));
            return 1;
        }
        if (operation == Api::CursorLocked) {
            lua_pushboolean(state, self.cursorLocked()); return 1;
        }
        if (operation == Api::CursorLock) {
            luaL_checktype(state, 1, LUA_TBOOLEAN);
            if (!lua_toboolean(state, 1)) {
                self.releaseCursorLock();
                lua_pushboolean(state, self.inputWindow != nullptr); return 1;
            }
            if (!self.cursorLocked() && self.playing && self.allowMutation && self.inputWindow &&
                self.hasPointerInput() && self.inputWindow->cursorMode() == platform::CursorMode::Normal) {
                self.ownsCursorLock = true;
                self.inputWindow->setCursorMode(platform::CursorMode::Disabled);
                platform::InputManager::resetMouseDelta();
            }
            lua_pushboolean(state, self.cursorLocked()); return 1;
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
            const Json overrides = lua_isnoneornil(state, 2) ? Json::object() : readJson(state, 2, 0U, nullptr, true);
            try { lua_pushunsigned(state, self.spawnPrefab(path, overrides)); }
            catch (const std::exception& error) { luaL_error(state, "Prefab '%s': %s", path.c_str(), error.what()); }
            return 1;
        }
        if (operation == Api::PlaySound) {
            std::size_t length{};
            const char* rawPath = luaL_checklstring(state, 1, &length);
            const auto gain = luaL_optnumber(state, 2, 1.0);
            bool started = false;
            if (length != 0U && std::string_view(rawPath, length).find('\0') == std::string_view::npos &&
                std::isfinite(gain) && gain >= 0.0 && gain <= 1.0 && self.playing && self.inputWindow) {
                try {
                    const NativePreparation preparation(self);
                    started = self.audio.play(self.resolve(std::string(rawPath, length)), static_cast<float>(gain));
                } catch (const std::exception&) {}
            }
            lua_pushboolean(state, started);
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
            if (!self.allowCleanupDestroy) self.requireMutation(state);
            self.pendingDestroy.insert(entity); return 0;
        }
        if (operation == Api::Tag) {
            const auto* tag = self.world->getComponent<ecs::Tag>(entity);
            if (tag == nullptr) { lua_pushnil(state); } else { lua_pushlstring(state, tag->value.data(), tag->value.size()); }
            return 1;
        }
        if (operation == Api::SetCameraFov) {
            self.requireMutation(state);
            const auto fov = luaL_checknumber(state, 2);
            if (!std::isfinite(fov) || fov < glm::radians(1.0) || fov > glm::radians(179.0)) {
                luaL_argerror(state, 2, "Expected a vertical field of view between 1 and 179 degrees in radians");
            }
            auto* camera = self.world->getComponent<ecs::Camera>(entity);
            if (camera) { camera->verticalFovRadians = static_cast<float>(fov); }
            lua_pushboolean(state, camera != nullptr);
            return 1;
        }
        if (operation == Api::SetRenderInterpolation || operation == Api::SnapRenderInterpolation) {
            self.requireMutation(state);
            const auto* transform = self.world->getComponent<ecs::Transform>(entity);
            if (!transform) { lua_pushboolean(state, false); return 1; }
            auto* history = self.world->getComponent<ecs::RenderPoseHistory>(entity);
            if (operation == Api::SnapRenderInterpolation) {
                if (history) *history = ecs::captureRenderPose(*self.world, entity, *transform);
                lua_pushboolean(state, history != nullptr);
            } else {
                luaL_checktype(state, 2, LUA_TBOOLEAN);
                if (lua_toboolean(state, 2)) {
                    if (!history) self.world->addComponent<ecs::RenderPoseHistory>(
                        entity, ecs::captureRenderPose(*self.world, entity, *transform));
                } else if (history) {
                    self.world->removeComponent<ecs::RenderPoseHistory>(entity);
                }
                lua_pushboolean(state, true);
            }
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

    static int networkApi(lua_State* state) {
        auto& self = from(state);
        auto* network = self.network;
        const bool writable = network && self.playing && self.allowMutation;
        const auto operation = static_cast<NetworkApi>(lua_tointeger(state, lua_upvalueindex(1)));
        switch (operation) {
        case NetworkApi::Role: {
            const std::string_view role = network ? network->role() : "offline";
            lua_pushlstring(state, role.data(), role.size());
            return 1;
        }
        case NetworkApi::Connected:
            lua_pushboolean(state, network && network->connected());
            return 1;
        case NetworkApi::LocalPeer: {
            const auto peer = network ? network->localPeer() : 0U;
            if (peer) lua_pushunsigned(state, peer);
            else lua_pushnil(state);
            return 1;
        }
        case NetworkApi::IsHost: {
            const auto peer = lua_isnoneornil(state, 1) ? 0U : checkNetworkKey(state, 1);
            lua_pushboolean(state, network && network->isHost(peer));
            return 1;
        }
        case NetworkApi::UsesSteam:
            lua_pushboolean(state, network && network->usesSteam());
            return 1;
        case NetworkApi::Peers: {
            const auto peers = network ? network->peers() : std::vector<uint32_t>{};
            lua_createtable(state, static_cast<int>(peers.size()), 0);
            int index = 0;
            for (const auto peer : peers) {
                lua_pushunsigned(state, peer);
                lua_rawseti(state, -2, ++index);
            }
            return 1;
        }
        case NetworkApi::Stats:
            if (network) pushJson(state, network->stats());
            else lua_newtable(state);
            return 1;
        case NetworkApi::Bind: {
            const auto entity = checkNetworkInteger<ecs::EntityId>(state, 1, 1);
            const auto key = checkNetworkKey(state, 2);
            lua_pushboolean(state, writable && self.alive(entity) && network->bind(entity, key));
            return 1;
        }
        case NetworkApi::Unbind: {
            const auto key = checkNetworkKey(state, 1);
            lua_pushboolean(state, network && self.playing && (self.allowMutation || self.allowCleanupDestroy) &&
                network->unbind(key));
            return 1;
        }
        case NetworkApi::RegisterPrefab: {
            const auto key = checkNetworkKey(state, 1);
            luaL_checktype(state, 2, LUA_TSTRING);
            std::size_t size{};
            const auto* text = lua_tolstring(state, 2, &size);
            const std::string_view path(text, size);
            if (path.empty() || path.size() > 1024 || path.find('\0') != std::string_view::npos) {
                luaL_argerror(state, 2, "expected a prefab path of 1 to 1024 bytes without NUL");
            }
            lua_pushboolean(state, writable && network->registerPrefab(key, path));
            return 1;
        }
        case NetworkApi::Entity: {
            const auto key = checkNetworkKey(state, 1);
            const auto entity = network ? network->entity(key) : ecs::kInvalidEntity;
            if (self.alive(entity)) lua_pushunsigned(state, entity);
            else lua_pushnil(state);
            return 1;
        }
        case NetworkApi::SetState: {
            const auto key = checkNetworkKey(state, 1);
            const auto value = checkNetworkInteger<uint16_t>(state, 2, 0);
            const auto flags = checkNetworkInteger<uint8_t>(state, 3, 0);
            lua_pushboolean(state, writable && network->setState(key, value, flags));
            return 1;
        }
        case NetworkApi::State:
        case NetworkApi::RawState: {
            const auto key = checkNetworkKey(state, 1);
            const auto replica = network ? (operation == NetworkApi::RawState ? network->rawState(key) : network->state(key)) :
                std::nullopt;
            if (!replica) { lua_pushnil(state); return 1; }
            lua_createtable(state, 0, 9);
            const auto field = [state](const char* name, double value) {
                lua_pushnumber(state, value);
                lua_rawsetfield(state, -2, name);
            };
            field("x", replica->x);
            field("y", replica->y);
            field("z", replica->z);
            field("pitch", replica->pitch);
            field("yaw", replica->yaw);
            field("roll", replica->roll);
            field("value", replica->value);
            field("flags", replica->flags);
            field("sampledFlags", replica->sampledFlags);
            return 1;
        }
        case NetworkApi::LocalMotion: {
            const auto motion = network ? network->localMotion() : std::nullopt;
            if (!motion) { lua_pushnil(state); return 1; }
            lua_createtable(state, 0, 7);
            const auto field = [state](const char* name, double value) {
                lua_pushnumber(state, value);
                lua_rawsetfield(state, -2, name);
            };
            field("x", motion->x);
            field("y", motion->y);
            field("z", motion->z);
            field("jumpBuffer", motion->jumpBuffer);
            field("coyote", motion->coyote);
            field("tick", static_cast<double>(motion->tick));
            field("inputAck", static_cast<double>(motion->inputAck));
            return 1;
        }
        case NetworkApi::NextInputSequence: {
            const auto sequence = network ? network->nextInputSequence() : 0;
            if (sequence) lua_pushnumber(state, static_cast<double>(sequence));
            else lua_pushnil(state);
            return 1;
        }
        case NetworkApi::SetPeerMotion: {
            const auto peer = checkNetworkKey(state, 1);
            std::array<float, 5> values{};
            for (std::size_t index = 0; index < values.size(); ++index) {
                const auto argument = static_cast<int>(index) + 2;
                luaL_checktype(state, argument, LUA_TNUMBER);
                const auto value = lua_tonumber(state, argument);
                if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                    luaL_argerror(state, argument, "expected a finite motion value");
                }
                values[index] = static_cast<float>(value);
            }
            lua_pushboolean(state, writable && network->role() == "server" &&
                network->setPeerMotion(peer, {values[0], values[1], values[2], values[3], values[4]}));
            return 1;
        }
        case NetworkApi::SetInput: {
            const auto x = checkNetworkAxis(state, 1);
            const auto z = checkNetworkAxis(state, 2);
            lua_pushboolean(state, writable && network->setInput(x, z));
            return 1;
        }
        case NetworkApi::Input: {
            const auto peer = checkNetworkInteger<uint32_t>(state, 1, 1);
            const auto input = network ? network->input(peer) : std::nullopt;
            if (!input) { lua_pushnil(state); return 1; }
            lua_pushnumber(state, (*input)[0]);
            lua_pushnumber(state, (*input)[1]);
            return 2;
        }
        case NetworkApi::SetControls: {
            const auto x = checkNetworkAxis(state, 1);
            const auto z = checkNetworkAxis(state, 2);
            luaL_checktype(state, 3, LUA_TNUMBER);
            luaL_checktype(state, 4, LUA_TNUMBER);
            const auto yaw = lua_tonumber(state, 3);
            const auto pitch = lua_tonumber(state, 4);
            if (!std::isfinite(yaw)) luaL_argerror(state, 3, "expected a finite yaw in radians");
            if (!std::isfinite(pitch) || std::abs(pitch) > std::numbers::pi / 2) {
                luaL_argerror(state, 4, "expected pitch within +/- pi/2 radians");
            }
            const auto buttons = checkNetworkInteger<uint16_t>(state, 5, 0);
            const auto pressed = checkNetworkInteger<uint8_t>(state, 6, 0);
            lua_pushboolean(state, writable && network->setControls({x, z,
                float(std::remainder(yaw, 2 * std::numbers::pi)), float(pitch), buttons, pressed}));
            return 1;
        }
        case NetworkApi::Controls: {
            const auto peer = checkNetworkKey(state, 1);
            const auto controls = network ? network->controls(peer) : std::nullopt;
            if (!controls) { lua_pushnil(state); return 1; }
            lua_createtable(state, 0, 6);
            const auto field = [state](const char* name, double value) {
                lua_pushnumber(state, value);
                lua_rawsetfield(state, -2, name);
            };
            field("x", controls->x);
            field("z", controls->z);
            field("yaw", controls->yaw);
            field("pitch", controls->pitch);
            field("buttons", controls->buttons);
            field("pressed", controls->pressed);
            return 1;
        }
        case NetworkApi::SetPeerValue: {
            const auto peer = checkNetworkKey(state, 1);
            const auto value = checkNetworkInteger<uint16_t>(state, 2, 0);
            lua_pushboolean(state, writable && network->role() == "server" && network->setPeerValue(peer, value));
            return 1;
        }
        case NetworkApi::PeerValue: {
            const auto peer = checkNetworkKey(state, 1);
            const auto value = network ? network->peerValue(peer) : std::nullopt;
            if (value) lua_pushunsigned(state, *value);
            else lua_pushnil(state);
            return 1;
        }
        case NetworkApi::NormalizePeerName:
        case NetworkApi::SetPeerName: {
            const bool set = operation == NetworkApi::SetPeerName;
            const auto peer = set ? checkNetworkKey(state, 1) : 0U;
            const auto argument = set ? 2 : 1;
            luaL_checktype(state, argument, LUA_TSTRING);
            size_t size{};
            const auto* text = lua_tolstring(state, argument, &size);
            const auto name = net::normalizePeerName({text, size});
            if (set) {
                lua_pushboolean(state, name && writable && network->role() == "server" && network->setPeerName(peer, *name));
            } else if (name) {
                lua_pushlstring(state, name->empty() ? "" : name->data(), name->size());
            } else {
                lua_pushnil(state);
            }
            return 1;
        }
        case NetworkApi::PeerName: {
            const auto peer = checkNetworkKey(state, 1);
            const auto name = network ? network->peerName(peer) : std::string_view{};
            if (name.empty()) lua_pushnil(state);
            else lua_pushlstring(state, name.data(), name.size());
            return 1;
        }
        case NetworkApi::PeerAvatar: {
            const auto peer = checkNetworkKey(state, 1);
            const auto image = network ? network->peerAvatar(peer) : std::string{};
            if (image.empty()) lua_pushnil(state);
            else lua_pushlstring(state, image.data(), image.size());
            return 1;
        }
        case NetworkApi::Request: {
            const auto key = checkNetworkKey(state, 1);
            const auto action = checkNetworkInteger<uint8_t>(state, 2, 0);
            const auto argument = checkNetworkInteger<int32_t>(state, 3, std::numeric_limits<int32_t>::min());
            std::string_view text;
            if (!lua_isnoneornil(state, 4)) {
                luaL_checktype(state, 4, LUA_TSTRING);
                size_t size{};
                const auto* bytes = lua_tolstring(state, 4, &size);
                if (size > net::MaxPeerNameBytes) luaL_argerror(state, 4, "network RPC text exceeds 128 bytes");
                text = {bytes, size};
            }
            const auto request = writable ? network->request(key, action, argument, text) : 0U;
            if (request) lua_pushunsigned(state, request);
            else lua_pushnil(state);
            return 1;
        }
        }
        return 0;
    }

    void installNetworkBindings() {
        static constexpr std::pair<const char*, NetworkApi> bindings[] = {
            {"role", NetworkApi::Role}, {"connected", NetworkApi::Connected},
            {"localPeer", NetworkApi::LocalPeer}, {"isHost", NetworkApi::IsHost}, {"usesSteam", NetworkApi::UsesSteam},
            {"peers", NetworkApi::Peers}, {"stats", NetworkApi::Stats},
            {"bind", NetworkApi::Bind}, {"unbind", NetworkApi::Unbind}, {"registerPrefab", NetworkApi::RegisterPrefab},
            {"entity", NetworkApi::Entity}, {"setState", NetworkApi::SetState}, {"state", NetworkApi::State},
            {"rawState", NetworkApi::RawState}, {"localMotion", NetworkApi::LocalMotion},
            {"nextInputSequence", NetworkApi::NextInputSequence}, {"setPeerMotion", NetworkApi::SetPeerMotion},
            {"setInput", NetworkApi::SetInput}, {"input", NetworkApi::Input},
            {"setControls", NetworkApi::SetControls}, {"controls", NetworkApi::Controls},
            {"setPeerValue", NetworkApi::SetPeerValue}, {"peerValue", NetworkApi::PeerValue},
            {"normalizePeerName", NetworkApi::NormalizePeerName}, {"setPeerName", NetworkApi::SetPeerName},
            {"peerName", NetworkApi::PeerName}, {"peerAvatar", NetworkApi::PeerAvatar},
            {"request", NetworkApi::Request}
        };
        lua_createtable(vm, 0, static_cast<int>(std::size(bindings)));
        for (const auto& [name, operation] : bindings) {
            lua_pushinteger(vm, static_cast<int>(operation));
            lua_pushcclosure(vm, &networkApi, name, 1);
            lua_rawsetfield(vm, -2, name);
        }
        lua_setglobal(vm, "Network");
    }

    static int steamQuery(lua_State* state) {
        auto* steam = from(state).steam;
        const bool available = steam && steam->available();
        const auto query = static_cast<SteamQuery>(lua_tointeger(state, lua_upvalueindex(1)));
        switch (query) {
        case SteamQuery::Available:
            lua_pushboolean(state, available);
            break;
        case SteamQuery::LocalUser:
            if (available) pushJson(state, steam->localUser());
            else lua_pushnil(state);
            break;
        case SteamQuery::Session:
            if (available) pushJson(state, steam->session());
            else lua_pushnil(state);
            break;
        case SteamQuery::Friends:
        case SteamQuery::Sessions: {
            const auto offset = lua_isnoneornil(state, 1) ? 0U : checkNetworkInteger<unsigned>(state, 1, 0);
            const auto limit = steamPageLimit(state, 2);
            if (available) pushJson(state, query == SteamQuery::Friends ? steam->friends(offset, limit) : steam->sessions(offset, limit));
            else lua_newtable(state);
            break;
        }
        case SteamQuery::Avatar: {
            const auto friendId = checkSteamId(state, 1);
            const auto image = available ? steam->avatar(friendId) : std::string{};
            if (image.empty()) lua_pushnil(state);
            else lua_pushlstring(state, image.data(), image.size());
            break;
        }
        }
        return 1;
    }

    static int steamRequest(lua_State* state) {
        auto& self = from(state);
        ScriptSteamRequest request;
        request.action = static_cast<ScriptSteamAction>(lua_tointeger(state, lua_upvalueindex(1)));
        switch (request.action) {
        case ScriptSteamAction::CreateSession:
        case ScriptSteamAction::UpdateSession:
            request.options = checkSteamOptions(state);
            break;
        case ScriptSteamAction::FindSessions:
            request.limit = steamPageLimit(state, 1);
            break;
        case ScriptSteamAction::JoinSession:
        case ScriptSteamAction::InviteFriend:
        case ScriptSteamAction::OpenProfile:
            request.id = checkSteamId(state, 1);
            break;
        case ScriptSteamAction::SetRichPresence:
            request.key = checkSteamString(state, 1, 63, true, 1);
            request.value = checkSteamString(state, 2, 255, false, 2);
            break;
        case ScriptSteamAction::LeaveSession:
        case ScriptSteamAction::OpenInviteOverlay:
            break;
        }

        std::uint32_t id{};
        std::string error;
        if (!self.steam) error = "Steam is unavailable in this scene.";
        else if (!self.playing || !self.allowMutation) error = "Steam requests require a running behavior or game UI callback.";
        else id = self.steam->submit(std::move(request), error);
        if (id) {
            lua_pushunsigned(state, id);
            return 1;
        }
        if (error.empty()) error = "Steam request rejected.";
        lua_pushnil(state);
        lua_pushlstring(state, error.data(), error.size());
        return 2;
    }

    void installSteamBindings() {
        static constexpr std::pair<const char*, SteamQuery> queries[] = {
            {"available", SteamQuery::Available}, {"localUser", SteamQuery::LocalUser},
            {"friends", SteamQuery::Friends}, {"session", SteamQuery::Session},
            {"sessions", SteamQuery::Sessions}, {"avatar", SteamQuery::Avatar}
        };
        static constexpr std::pair<const char*, ScriptSteamAction> requests[] = {
            {"createSession", ScriptSteamAction::CreateSession}, {"findSessions", ScriptSteamAction::FindSessions},
            {"joinSession", ScriptSteamAction::JoinSession}, {"leaveSession", ScriptSteamAction::LeaveSession},
            {"updateSession", ScriptSteamAction::UpdateSession}, {"inviteFriend", ScriptSteamAction::InviteFriend},
            {"openProfile", ScriptSteamAction::OpenProfile}, {"openInviteOverlay", ScriptSteamAction::OpenInviteOverlay},
            {"setRichPresence", ScriptSteamAction::SetRichPresence}
        };
        lua_createtable(vm, 0, static_cast<int>(std::size(queries) + std::size(requests)));
        const auto install = [this](const auto& entries, lua_CFunction handler) {
            for (const auto& [name, operation] : entries) {
                lua_pushinteger(vm, static_cast<int>(operation));
                lua_pushcclosure(vm, handler, name, 1);
                lua_rawsetfield(vm, -2, name);
            }
        };
        install(queries, &steamQuery);
        install(requests, &steamRequest);
        lua_setglobal(vm, "Steam");
    }

    void installBindings() {
        lua_newtable(vm);
        bind(vm, "isAlive", Api::Alive); bind(vm, "getPosition", Api::Position); bind(vm, "setPosition", Api::SetPosition);
        bind(vm, "getRotation", Api::Rotation); bind(vm, "setRotation", Api::SetRotation);
        bind(vm, "setCameraFov", Api::SetCameraFov);
        bind(vm, "setRenderInterpolation", Api::SetRenderInterpolation);
        bind(vm, "snapRenderInterpolation", Api::SnapRenderInterpolation);
        bind(vm, "getVelocity", Api::Velocity); bind(vm, "setVelocity", Api::SetVelocity); bind(vm, "applyForce", Api::Force);
        bind(vm, "getTag", Api::Tag); bind(vm, "find", Api::Find); bind(vm, "queryRadius", Api::Radius);
        bind(vm, "spawnPrefab", Api::Spawn); bind(vm, "destroy", Api::Destroy); bind(vm, "setStatus", Api::Status);
        bind(vm, "playSound", Api::PlaySound);
        bind(vm, "getStatus", Api::ReadStatus);
        lua_setglobal(vm, "Engine");
        lua_newtable(vm);
        bind(vm, "isKeyDown", Api::KeyDown); bind(vm, "wasKeyPressed", Api::KeyPressed); bind(vm, "wasKeyReleased", Api::KeyReleased);
        bind(vm, "mousePosition", Api::MousePosition); bind(vm, "mouseDelta", Api::MouseDelta);
        bind(vm, "mouseSensitivity", Api::MouseSensitivity); bind(vm, "setMouseSensitivity", Api::SetMouseSensitivity);
        bind(vm, "isMouseButtonDown", Api::MouseDown); bind(vm, "wasMouseButtonPressed", Api::MousePressed);
        bind(vm, "setCursorLocked", Api::CursorLock); bind(vm, "isCursorLocked", Api::CursorLocked);
        lua_setglobal(vm, "Input");
        installNetworkBindings();
        installSteamBindings();
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
            const int fieldsIndex=lua_absindex(vm,-1);
            const auto scalar=[](int type) { return type==LUA_TNUMBER || type==LUA_TBOOLEAN || type==LUA_TSTRING; };
            Json definitions = Json::object();
            std::size_t declarations = 0;
            lua_pushnil(vm);
            while (lua_next(vm, fieldsIndex)) {
                if (++declarations > 128U) {
                    invalid({}, "A behavior may declare at most 128 fields");
                    definitions.clear();
                    lua_pop(vm, 2);
                    break;
                }
                if (lua_type(vm, -2) != LUA_TSTRING) {
                    invalid({}, "Field names must be strings");
                    lua_pop(vm, 1);
                    continue;
                }
                std::size_t nameLength = 0;
                const char* rawName = lua_tolstring(vm, -2, &nameLength);
                const std::string name(rawName, nameLength);
                Json definition;
                if (lua_istable(vm, -1)) {
                    definition = Json::object();
                    for (const auto* key : {"default", "type", "min", "max", "step", "label", "description", "category"}) {
                        lua_rawgetfield(vm, -1, key);
                        if (!lua_isnil(vm, -1)) definition[key] = scalar(lua_type(vm, -1)) ? readJson(vm, -1) : Json{};
                        lua_pop(vm, 1);
                    }
                } else if (scalar(lua_type(vm, -1))) {
                    definition = readJson(vm, -1);
                }
                definitions[name] = std::move(definition);
                lua_pop(vm, 1);
            }
            for (const auto& [name, definition] : definitions.items()) {
                lua_pushlstring(vm,name.data(),name.size()); lua_rawget(vm,fieldsIndex);
                std::string rawError;
                if (lua_istable(vm,-1)) {
                    lua_rawgetfield(vm,-1,"default");
                    if (!scalar(lua_type(vm,-1))) rawError="Declaration requires a scalar default";
                    lua_pop(vm,1);
                    for (const auto* key : {"type","label","description","category"}) {
                        lua_rawgetfield(vm,-1,key);
                        if (!lua_isnil(vm,-1) && lua_type(vm,-1)!=LUA_TSTRING) rawError=std::string(key)+" must be a string";
                        lua_pop(vm,1);
                    }
                } else if (!scalar(lua_type(vm,-1))) rawError="Declaration requires a scalar value or a field table";
                lua_pop(vm,1);
                if (!rawError.empty()) { invalid(name,rawError); continue; }
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
                    bound("step", field.step);
                    if (!validBounds) { continue; }
                    bool validMetadata = true;
                    const auto text = [&](const char* key, std::string& value) {
                        if (const auto metadata = definition.find(key); metadata != definition.end()) {
                            if (!metadata->is_string()) { invalid(name, std::string(key) + " must be a string"); validMetadata = false; }
                            else { value = metadata->get<std::string>(); }
                        }
                    };
                    text("label", field.label); text("description", field.description); text("category", field.category);
                    if (!validMetadata || field.step <= 0.0 || field.step > 1000000.0) {
                        if (validMetadata) invalid(name, "step must be positive and at most 1000000");
                        continue;
                    }
                } else { field.defaultValue = definition; }
                if (field.type.empty()) {
                    field.type = field.defaultValue.is_boolean() ? "boolean" : field.defaultValue.is_number() ? "number" :
                        field.defaultValue.is_string() ? "string" : "invalid";
                }
                if (field.type == "bool") { field.type = "boolean"; }
                if (field.type == "integer") { field.type = "number"; field.integer = true; }
                if (field.integer && field.step < 1.0) { field.step = 1.0; }
                const bool matches = field.type == "number" ? field.defaultValue.is_number() && std::isfinite(field.defaultValue.get<double>()) :
                    field.type == "boolean" ? field.defaultValue.is_boolean() : field.type == "string" && field.defaultValue.is_string();
                if (!matches) { invalid(name, "default must match supported type number, integer, boolean, bool or string"); continue; }
                if (field.minimum > field.maximum) { invalid(name, "min must not exceed max"); continue; }
                if (field.type == "number" && (field.defaultValue.get<double>() < field.minimum || field.defaultValue.get<double>() > field.maximum)) {
                    invalid(name, "default must be inside min/max"); continue;
                }
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
        std::stable_sort(result.begin(), result.end(), [](const ScriptField& left, const ScriptField& right) {
            return left.category != right.category ? left.category < right.category : left.name < right.name;
        });
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
    void callback(const ecs::EntityId entity, const char* method, const double dt, const Json* event,
                  const bool mutation = true, RpcInvocation* rpc = nullptr) {
        ENGINE_PROFILE_ZONE("Luau Behavior Entry Point");
        const auto found = instances.find(entity);
        const bool cleanup = std::string(method) == "Destroy";
        if (found == instances.end() || (!cleanup && (found->second.failed || !alive(entity)))) { return; }
        if (!cleanup) {
            const auto* component = world->getComponent<ScriptComponent>(entity);
            if (!component || !component->enabled || component->path != found->second.path) return;
        }
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
        if (rpc) {
            rpc->reply.handled = true;
            lua_pushunsigned(vm, rpc->peer);
            lua_pushunsigned(vm, rpc->action);
            lua_pushnumber(vm, rpc->argument);
            arguments += 3;
            if (!rpc->text.empty()) {
                lua_pushlstring(vm, rpc->text.data(), rpc->text.size());
                ++arguments;
            }
        } else if (event != nullptr) { pushJson(vm, *event); ++arguments; }
        else if (std::string(method) == "Update") { lua_pushnumber(vm, dt); ++arguments; }
        struct PermissionRestore final {
            Impl& self;
            bool mutation;
            bool cleanupDestroy;
            ~PermissionRestore() { self.allowMutation = mutation; self.allowCleanupDestroy = cleanupDestroy; }
        } restore{*this, allowMutation, allowCleanupDestroy};
        allowMutation = mutation;
        allowCleanupDestroy = cleanup;
        const auto callbackPath = found->second.path;
        evaluating.push_back(callbackPath);
        const bool ok = protectedCall(vm, arguments, rpc ? 2 : 0, base + 1);
        evaluating.pop_back();
        if (rpc && ok) {
            const auto value = lua_isnil(vm, -1) ? std::optional<uint16_t>{0} : networkInteger<uint16_t>(vm, -1, 0);
            if (lua_type(vm, -2) != LUA_TBOOLEAN || !value) {
                message(ScriptMessage::Kind::Error, callbackPath + ": OnNetworkRpc must return a boolean and an optional integer from 0 to 65535");
            } else if (lua_toboolean(vm, -2)) {
                rpc->reply.accepted = true;
                rpc->reply.value = *value;
            }
        }
        lua_settop(vm, base);
        if (!ok && !rpc) { if (const auto current = instances.find(entity); current != instances.end()) { current->second.failed = true; } }
        });
        if (!entered) {
            if (rpc) rpc->reply = {true, false, 0};
            else if (const auto current = instances.find(entity); current != instances.end()) current->second.failed = true;
        }
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
        if (flushingDestroy || world == nullptr) return;
        struct FlushRestore final {
            bool& active;
            ~FlushRestore() { active = false; }
        } restore{flushingDestroy};
        flushingDestroy = true;
        while (!pendingDestroy.empty()) {
            const auto entity = *pendingDestroy.begin();
            callback(entity, "Destroy", 0.0, nullptr, false);
            const auto found = instances.find(entity);
            if (found != instances.end()) { lua_unref(vm, found->second.ref); instances.erase(found); }
            if (world->isAlive(entity)) { if (destroy) { destroy(entity); } else { world->destroyEntity(entity); } }
            pendingDestroy.erase(entity);
        }
    }
    ecs::EntityId spawnPrefab(const std::string& path, const Json& overrides) {
        Json prefab;
        {
            const NativePreparation preparation(*this);
            const auto resolved = resolve(path);
            prefab = Json::parse(readAssetBytes(resolved, limits.sourceBytes, "Prefab"));
        }
        if (!prefab.is_object()) { throw std::invalid_argument("Prefab must be an object"); }
        if (!overrides.is_object()) { throw std::invalid_argument("Prefab overrides must be an object"); }
        prefab.merge_patch(overrides);
        const auto& data = prefab.contains("components") ? prefab["components"] : prefab;
        ecs::Transform transform;
        if (data.contains("transform")) {
            const auto& item = data["transform"];
            transform.position = jsonVector(item.value("position", Json::array()), transform.position);
            transform.rotationEulerRadians = jsonVector(item.value("rotationEulerRadians", item.value("rotation", Json::array())), transform.rotationEulerRadians);
            transform.scale = jsonVector(item.value("scale", Json::array()), transform.scale);
        }
        std::optional<ecs::Rigidbody> rigidbody;
        if (data.contains("rigidbody")) {
            const auto& item = data["rigidbody"];
            rigidbody.emplace(); rigidbody->mass = item.value("mass", 1.0F);
            rigidbody->isStatic = item.value("isStatic", false); rigidbody->isKinematic = item.value("isKinematic", false);
            rigidbody->useGravity = item.value("useGravity", true);
            rigidbody->velocity = jsonVector(item.value("velocity", Json::array()), glm::vec3(0.0F));
            rigidbody->acceleration = jsonVector(item.value("acceleration", Json::array()), rigidbody->acceleration);
            rigidbody->angularVelocity = jsonVector(item.value("angularVelocity", Json::array()), rigidbody->angularVelocity);
            rigidbody->linearDamping = item.value("linearDamping", rigidbody->linearDamping);
            rigidbody->angularDamping = item.value("angularDamping", rigidbody->angularDamping);
            rigidbody->restitution = item.value("restitution", rigidbody->restitution);
            rigidbody->friction = item.value("friction", rigidbody->friction);
            rigidbody->canSleep = item.value("canSleep", rigidbody->canSleep);
            rigidbody->recalculateMassProperties();
        }
        std::optional<ecs::Collider> collider;
        if (data.contains("collider")) {
            const auto& item = data["collider"]; collider.emplace();
            collider->type = item.value("type", std::string("aabb")) == "sphere" ? ecs::ColliderType::Sphere : ecs::ColliderType::Aabb;
            collider->sphere.radius = std::max(0.001F, item.value("radius", 0.5F));
            collider->aabb.halfExtents = jsonVector(item.value("halfExtents", Json::array()), glm::vec3(0.5F));
            collider->offset = jsonVector(item.value("offset", Json::array()), collider->offset);
            collider->enabled = item.value("enabled", true);
        }
        std::optional<ecs::MeshRenderer> mesh;
        if (data.contains("meshRenderer")) {
            const auto& item = data["meshRenderer"]; mesh.emplace();
            mesh->meshId = item.value("meshId", std::string("__diligent_cube__"));
            mesh->textureId = item.value("textureId", std::string("assets/textures/arena_wall.ppm"));
            mesh->shaderId = item.value("shaderId", std::string("assets/shaders_hlsl/textured.shader.json"));
            mesh->materialId = item.value("materialId", std::string{});
            mesh->visible = item.value("visible", true);
            mesh->primitiveType = static_cast<ecs::PrimitiveType>(item.value("primitiveType", static_cast<int>(mesh->primitiveType)));
            if (item.contains("uvScale") && item["uvScale"].is_array() && item["uvScale"].size() == 2U) {
                mesh->uvScale = {item["uvScale"][0].get<float>(), item["uvScale"][1].get<float>()};
            }
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
        std::optional<ecs::Camera> camera;
        if (data.contains("camera")) {
            const auto& item = data["camera"]; camera.emplace();
            camera->verticalFovRadians = item.value("verticalFovRadians", camera->verticalFovRadians);
            camera->nearPlane = item.value("nearPlane", camera->nearPlane);
            camera->farPlane = item.value("farPlane", camera->farPlane);
            camera->active = item.value("active", camera->active);
        }
        std::optional<ecs::Light> light;
        if (data.contains("light")) light = ecs::lightFromJson(data["light"]);
        std::string tag = prefab.value("name", std::string("Prefab"));
        if (data.contains("tag")) { tag = data["tag"].is_string() ? data["tag"].get<std::string>() : data["tag"].value("value", tag); }
        const auto entity = world->createEntity();
        try {
            world->addComponent<ecs::Transform>(entity, transform); world->addComponent<ecs::Tag>(entity, ecs::Tag{tag});
            if (rigidbody) { world->addComponent<ecs::Rigidbody>(entity, *rigidbody); }
            if (collider) { world->addComponent<ecs::Collider>(entity, *collider); }
            if (mesh) { world->addComponent<ecs::MeshRenderer>(entity, *mesh); }
            if (script) { world->addComponent<ScriptComponent>(entity, *script); }
            if (camera) { world->addComponent<ecs::Camera>(entity, *camera); }
            if (light) { world->addComponent<ecs::Light>(entity, *light); }
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
    self.mouseSensitivity.reset();
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
    self.world = nullptr; self.jobs = nullptr; self.network = nullptr; self.steam = nullptr; self.inputWindow = nullptr;
}
void ScriptingSystem::setLimits(Limits limits) {
    m_impl->requireOwner();
    limits.memoryBytes = std::max(limits.memoryBytes, std::size_t{1024U * 1024U});
    limits.callbackMilliseconds = std::clamp(limits.callbackMilliseconds, 1.0, 10000.0);
    m_impl->limits = limits;
}
ScriptingSystem::Limits ScriptingSystem::limits() const { return m_impl->limits; }
void ScriptingSystem::setInputEnabled(const bool enabled) { m_impl->requireOwner(); m_impl->inputEnabled = enabled; }
void ScriptingSystem::setPointerInputEnabled(const bool enabled) { m_impl->requireOwner(); m_impl->pointerInputEnabled = enabled; }
void ScriptingSystem::setInputWindow(platform::Window* window) {
    auto& self = *m_impl; self.requireOwner();
    if (self.inputWindow != window) self.releaseCursorLock();
    self.inputWindow = window;
}
bool ScriptingSystem::cursorLocked() const { return m_impl->cursorLocked(); }
bool ScriptingSystem::releaseCursorLock() { m_impl->requireOwner(); return m_impl->releaseCursorLock(); }
void ScriptingSystem::setDestroyCallback(std::function<void(ecs::EntityId)> callback) { m_impl->requireOwner(); m_impl->destroy = std::move(callback); }
void ScriptingSystem::setNetwork(ScriptNetwork* network) { m_impl->requireOwner(); m_impl->network = network; }
void ScriptingSystem::setSteam(ScriptSteam* steam) { m_impl->requireOwner(); m_impl->steam = steam; }
ScriptRpcReply ScriptingSystem::invokeNetworkRpc(ecs::EntityId entity, uint32_t peer, uint8_t action, int32_t argument,
                                                std::string_view text) {
    auto& self = *m_impl;
    self.requireOwner();
    if (!self.vm || !self.playing || self.protectedDepth || !self.network || self.network->role() != "server" || !peer) {
        return {};
    }
    Impl::RpcInvocation rpc{peer, action, argument, text};
    self.callback(entity, "OnNetworkRpc", 0.0, nullptr, true, &rpc);
    return rpc.reply;
}
void ScriptingSystem::start() {
    auto& self = *m_impl; self.requireOwner(); if (self.vm == nullptr) { return; }
    stop();
    self.failedModules.clear();
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
    self.instances.clear(); self.events.clear(); self.flushDestroy(); self.playing = false;
    self.releaseCursorLock();
    self.audio.clear();
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
    payload["type"] = std::move(type); self.events.push_back(std::move(payload));
}
void ScriptingSystem::dispatchEvents() {
    auto& self = *m_impl; self.requireOwner(); if (!self.playing || self.vm == nullptr) { return; }
    const auto events = std::move(self.events); self.events.clear();
    std::vector<ecs::EntityId> entities;
    for (const auto& [entity, instance] : self.instances) { (void)instance; entities.push_back(entity); }
    for (const auto& payload : events) {
        for (const auto entity : entities) { self.callback(entity, "OnEvent", 0.0, &payload); }
    }
    self.flushDestroy();
}
void ScriptingSystem::forgetEntity(const ecs::EntityId entity) {
    auto& self = *m_impl; self.requireOwner();
    if (self.instances.contains(entity)) { self.callback(entity, "Destroy", 0.0, nullptr, false); }
    const auto found = self.instances.find(entity);
    if (found != self.instances.end()) { lua_unref(self.vm, found->second.ref); self.instances.erase(found); }
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
bool ScriptingSystem::setField(ScriptComponent& component, const std::string& name, const Json& value) {
    const auto definitions = fields(component.path);
    const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const ScriptField& field) { return field.name == name; });
    if (found == definitions.end()) return false;
    Json normalized = value;
    if (found->type == "number") {
        if (!value.is_number() || !std::isfinite(value.get<double>())) return false;
        double number = std::clamp(value.get<double>(), found->minimum, found->maximum);
        normalized = found->integer ? std::round(number) : number;
    } else if (found->type == "boolean" ? !value.is_boolean() : !value.is_string()) return false;
    if (!component.fields.is_object()) component.fields = Json::object();
    if (component.fields.contains(name) && component.fields.at(name) == normalized) return false;
    component.fields[name] = std::move(normalized);
    return true;
}
bool ScriptingSystem::requestReload(const std::string& path) {
    auto& self = *m_impl; self.requireOwner(); if (self.vm == nullptr) { return false; }
    std::string key;
    try { key = self.moduleKey(path); if (self.registered.contains(key)) return false; }
    catch (const std::exception& error) { self.message(ScriptMessage::Kind::Error, error.what()); return false; }
    self.reloadQueue.insert(key);
    if (self.reload) return true;
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
            auto bytes = compileSource(readAssetBytes(resolved, sourceLimit, "Script"));
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
        const auto key = utf8Path(change.path.lexically_relative(self.root));
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

}
