#include "engine/scripting/react/ReactRuntime.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "engine/scripting/ScriptingSystem.h"

namespace engine::scripting {
namespace {
std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

std::string quote(const std::string& value) {
    // Module names are fixed ASCII paths from our checked-in manifest.
    std::string result = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"') result += '\\';
        result += character;
    }
    return result + '"';
}

bool failure(ScriptingSystem& scripting, const std::string& message) {
    return scripting.execute("error(" + quote("React Luau: " + message) + ")", "=ReactBootstrap");
}
} // namespace

bool ReactRuntime::install(ScriptingSystem& scripting, std::filesystem::path packageRoot) {
    if (packageRoot.empty()) {
        packageRoot = "assets/scripts/react-luau";
        if (!std::filesystem::exists(packageRoot / "manifest.tsv")) {
#ifdef ENGINE_REACT_PACKAGE_DIR
            packageRoot = ENGINE_REACT_PACKAGE_DIR;
#else
            packageRoot = "third_party/react-luau";
#endif
        }
    }
    std::ifstream manifest(packageRoot / "manifest.tsv");
    if (!manifest) return failure(scripting, "missing manifest at " + packageRoot.generic_string());
    std::map<std::string, std::string> modules;
    std::set<std::string> nodes;
    std::string line;
    while (std::getline(manifest, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find('\t');
        if (separator == std::string::npos) return failure(scripting, "invalid manifest row");
        const auto id = line.substr(0, separator);
        const auto relative = std::filesystem::path(line.substr(separator + 1));
        if (relative.is_absolute() || relative.generic_string().find("..") != std::string::npos ||
            id.find("..") != std::string::npos || id.empty()) return failure(scripting, "invalid package path");
        auto source = readText(packageRoot / relative);
        if (source.empty()) return failure(scripting, "missing module " + id);
        if (!modules.emplace(id, std::move(source)).second) return failure(scripting, "duplicate module " + id);
        auto ancestor = id;
        while (!ancestor.empty()) {
            nodes.insert(ancestor);
            const auto slash = ancestor.rfind('/');
            if (slash == std::string::npos) break;
            ancestor.resize(slash);
        }
    }
    nodes.insert("SafeFlags");
    nodes.insert("ReactRoblox");
    std::ostringstream hierarchy;
    hierarchy << "__reactModuleTree = {}\nlocal t = __reactModuleTree\n"
                 "local packages = { Name = 'Packages' }\npackages.Parent = packages\n"
                 "function packages:FindFirstChild(name) return self[name] end\n"
                 "function packages:GetFullName() return 'Packages' end\n";
    for (const auto& node : nodes) hierarchy << "t[" << quote(node) << "] = {}\n";
    for (const auto& node : nodes) {
        const auto slash = node.rfind('/');
        const auto name = node.substr(slash == std::string::npos ? 0 : slash + 1);
        const auto parent = slash == std::string::npos ? "packages" : "t[" + quote(node.substr(0, slash)) + "]";
        hierarchy << "t[" << quote(node) << "].Name = " << quote(name) << '\n'
                  << "t[" << quote(node) << "].Parent = " << parent << '\n'
                  << parent << '[' << quote(name) << "] = t[" << quote(node) << "]\n"
                  << "t[" << quote(node) << "].FindFirstChild = packages.FindFirstChild\n"
                  << "t[" << quote(node) << "].GetFullName = function() return " << quote("Packages/" + node) << " end\n";
        if (modules.contains(node) || node == "SafeFlags" || node == "ReactRoblox")
            hierarchy << "t[" << quote(node) << "].__module = " << quote(node) << '\n';
    }
    if (!scripting.execute(hierarchy.str(), "=ReactModuleHierarchy")) return false;
    const auto compatibility = readText(packageRoot / "adapters/Compatibility.luau");
    const auto robloxUI = readText(packageRoot / "adapters/RobloxUI.luau");
    const auto tweens = readText(packageRoot / "adapters/TweenService.luau");
    const auto renderer = readText(packageRoot / "adapters/ReactTurbo.luau");
    if (compatibility.empty() || robloxUI.empty() || tweens.empty() || renderer.empty()) return failure(scripting, "missing engine adapters");
    if (!scripting.execute(compatibility, "=ReactCompatibility")) return false;
    if (!scripting.execute(robloxUI, "=RobloxUICompatibility")) return false;
    if (!scripting.execute(tweens, "=TweenServiceCompatibility")) return false;
    // Advertise native capabilities from the binary, not from replaceable Luau
    // adapters. Projects can keep their older pages usable until restart.
    if (!scripting.execute("__turboNativeUIVersion = 3", "=NativeUIVersion")) return false;
    for (auto& [id, source] : modules) {
        // Use the module's private environment: BeginWork is exactly at Luau's
        // local-register limit, so introducing even one local breaks upstream.
        const auto taskAlias = id == "Scheduler" || id.starts_with("Scheduler/") ? "task = __turboSchedulerTask\n" : "";
        scripting.registerModule(id, "script = __reactModuleTree[" + quote(id) + "]\n" + taskAlias + source);
    }
    scripting.registerModule("SafeFlags", "return __turboSafeFlags");
    scripting.registerModule("ReactTurbo", renderer);
    scripting.registerModule("ReactRoblox", "return require('ReactTurbo').ReactRoblox");
    const auto previousLimits = scripting.limits();
    auto bootstrapLimits = previousLimits;
    bootstrapLimits.callbackMilliseconds = std::max(5000.0, previousLimits.callbackMilliseconds);
    scripting.setLimits(bootstrapLimits);
    const bool result = scripting.execute(
        "local UI = require('ReactTurbo'); __turboUIRoots = UI.getRoots(); "
        "__turboUIEvents = UI.events; __turboUIPump = UI.pump; __turboUIReset = UI.reset; "
        "__turboUIRefresh = UI.refresh; "
        // Upstream lazily loads ReactFiberHooks on the first function component.
        // Prewarm it within the trusted bootstrap budget, before any game code.
        "local React=require('React'); local function Warmup() React.useState(0); React.useEffect(function() end, {}); return nil end; "
        "local warmRoot=UI.createRoot(); warmRoot:render(React.createElement(Warmup)); UI.pump(0); warmRoot:unmount()", "=ReactBootstrap");
    scripting.setLimits(previousLimits);
    return result;
}

bool ReactRuntime::pump(ScriptingSystem& scripting, const double dt) {
    if (!std::isfinite(dt)) return false;
    return scripting.callGlobal("__turboUIPump", std::max(0.0, dt));
}

bool ReactRuntime::reset(ScriptingSystem& scripting) {
    return scripting.callGlobal("__turboUIReset");
}
} // namespace engine::scripting
