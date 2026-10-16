#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <lua.h>
#include "engine/core/Log.h"
#include "engine/ecs/world.h"
#include "engine/scripting/ScriptingSystem.h"
#include "engine/scripting/react/ReactRuntime.h"

namespace {
using Json = nlohmann::json;
using engine::scripting::ReactRuntime;
using engine::scripting::ScriptingSystem;

constexpr const char* kDriver = R"(
local B = {}
function B:Spawn()
    Engine.spawnPrefab('prefabs/collectathon_controller.json')
    Engine.spawnPrefab('prefabs/react_ui.json')
end
function B:Init()
    self.state.elapsed = 0
    self.state.cycles = 0
    self:Spawn()
end
function B:Update(dt)
    Engine.setStatus('soakCycles', self.state.cycles)
    self.state.elapsed += dt
    if self.state.elapsed < 3 then return end
    self.state.elapsed = 0
    for _, tag in {'ScriptToken', 'ScriptPlayer', 'Collectathon Controller', 'ReactLuauHUD'} do
        for _, id in Engine.find(tag) do Engine.destroy(id) end
    end
    self.state.cycles += 1
    self:Spawn()
end
return B
)";

std::size_t arrayLength(lua_State* state, const char* name) {
    const int top = lua_gettop(state);
    lua_getglobal(state, name);
    const auto count = lua_istable(state, -1) ? lua_objlen(state, -1) : 0;
    lua_settop(state, top);
    return count;
}

void fullGc(ScriptingSystem& scripts) {
    lua_gc(scripts.state(), LUA_GCCOLLECT, 0);
    lua_gc(scripts.state(), LUA_GCCOLLECT, 0);
}

Json runPhase(const bool cycleGc, const int cycles) {
    engine::ecs::World world;
    ScriptingSystem scripts;
    const std::string name = cycleGc ? "full_gc_each_cycle" : "normal_gc_pacing";
    Json result{{"phase", name}, {"requestedCycles", cycles}, {"completedCycles", 0},
        {"forcedGcDuringCycles", cycleGc}, {"passed", false}, {"samples", Json::array()}};
    const auto started = std::chrono::steady_clock::now();
    auto require = [](const bool ok, const char* text) {
        if (!ok) { throw std::runtime_error(text); }
    };
    auto sample = [&](const char* moment, const int cycle) {
        Json value{{"moment", moment}, {"cycle", cycle}, {"vmBytes", scripts.memoryUsed()},
            {"roots", arrayLength(scripts.state(), "__turboUIRoots")},
            {"queuedEvents", arrayLength(scripts.state(), "__turboUIEvents")},
            {"aliveEntities", world.aliveCount()}};
        result["samples"].push_back(value);
        std::cout << "MEMORY_SAMPLE " << name << ' ' << value.dump() << '\n' << std::flush;
        return value;
    };
    try {
        require(scripts.initialize(world, nullptr, ENGINE_TEST_ASSET_ROOT), "Initialize failed");
        require(ReactRuntime::install(scripts), "Real React bootstrap failed");
        scripts.registerModule("MemorySoakDriver", kDriver);
        // Earliest ID is essential: destroy happens in Driver.Update before
        // the later scripted victims are visited in the same update pass.
        const auto driver = world.createEntity();
        world.addComponent<engine::scripting::ScriptComponent>(driver,
            engine::scripting::ScriptComponent{"MemorySoakDriver", true, Json::object()});
        scripts.start();
        auto frame = [&] {
            scripts.update(1.0 / 60.0);
            require(ReactRuntime::pump(scripts, 1.0 / 60.0), "React pump failed");
            for (const auto& entry : scripts.output()) {
                if (entry.kind == engine::scripting::ScriptMessage::Kind::Error) {
                    throw std::runtime_error(entry.text);
                }
            }
            scripts.clearOutput();
        };
        for (int i = 0; i < 5; ++i) { frame(); }
        fullGc(scripts);
        const auto baseline = sample("baseline_after_full_gc", 0);
        require(world.aliveCount() == 16, "Initial actual demo entity count is not16");
        require(baseline["roots"] == 1, "Initial actual HUD root count is not1");
        int lastCycle = 0;
        for (int i = 0; i < cycles * 182 + 10; ++i) {
            frame();
            const int current = scripts.status().value("soakCycles", 0);
            if (current <= lastCycle) { continue; }
            lastCycle = current;
            result["completedCycles"] = current;
            if (cycleGc) { fullGc(scripts); }
            const auto currentSample = sample("settled_cycle", current);
            require(world.aliveCount() == 16, "Destroy/respawn retained or lost demo entities");
            require(currentSample["roots"] == 1, "Destroy/respawn retained or lost HUD roots");
            require(currentSample["queuedEvents"] == 0, "Idle HUD retained queued events");
            if (current >= cycles) { break; }
        }
        require(result["completedCycles"].get<int>() == cycles, "Cycle driver stopped progressing");
        sample("end_before_full_gc", cycles);
        fullGc(scripts);
        const auto afterGc = sample("end_after_full_gc", cycles);
        const auto baselineBytes = baseline["vmBytes"].get<std::size_t>();
        require(afterGc["vmBytes"].get<std::size_t>() < baselineBytes + 8U * 1024U * 1024U,
            "Post-GC live VM growth exceeds the bounded8MiB allowance");
        scripts.stop();
        require(ReactRuntime::reset(scripts), "Final React reset failed");
        fullGc(scripts);
        const auto cleanup = sample("after_stop_reset_full_gc", cycles);
        require(cleanup["roots"] == 0 && cleanup["queuedEvents"] == 0,
            "Stop/reset retained roots/events");
        result["passed"] = true;
    } catch (const std::exception& error) {
        result["failure"] = error.what();
        if (scripts.state()) {
            sample("failure_before_full_gc", result["completedCycles"].get<int>());
            fullGc(scripts);
            sample("failure_after_full_gc", result["completedCycles"].get<int>());
        }
    }
    scripts.shutdown();
    result["vmBytesAfterShutdown"] = scripts.memoryUsed();
    if (scripts.memoryUsed() != 0) { result["passed"] = false; }
    result["wallSeconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}
} // namespace

int main() {
    Json result{{"scope", "Same LinuxCPU VM, real controller/prefabs/demo_ui/React; no GPU, no hot reload"},
        {"productionCallbackBudgetMs", 100}, {"phases", Json::array()}};
    result["phases"].push_back(runPhase(false, 120));
    result["phases"].push_back(runPhase(true, 120));
    const bool passed = result["phases"][0]["passed"].get<bool>() && result["phases"][1]["passed"].get<bool>();
    result["passed"] = passed;
    engine::core::Log::shutdown();
    std::cout << "MEMORY_SUMMARY " << result.dump() << '\n';
    return passed ? 0 : 1;
}
