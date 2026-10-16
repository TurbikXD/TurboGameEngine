#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include <lua.h>

#include "engine/ecs/world.h"
#include "engine/scripting/ScriptingSystem.h"
#include "engine/scripting/react/ReactRuntime.h"

namespace {
void require(const bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void printErrors(const engine::scripting::ScriptingSystem& scripting) {
    for (const auto& message : scripting.output())
        if (message.kind == engine::scripting::ScriptMessage::Kind::Error) std::cerr << message.text << '\n';
}
} // namespace

int main() {
    using engine::scripting::ReactRuntime;
    engine::ecs::World world;
    engine::scripting::ScriptingSystem scripting;
    try {
        require(scripting.initialize(world, nullptr, "assets"), "Scripting initialization failed");
        const auto originalLimits = scripting.limits();
        require(ReactRuntime::install(scripting), "Real React Luau bootstrap failed");
        require(scripting.limits().callbackMilliseconds == originalLimits.callbackMilliseconds, "Bootstrap changed configured execution limits");
        const int stackTop = lua_gettop(scripting.state());
        std::ifstream source(std::filesystem::path(ENGINE_REACT_TEST_DIR) / "ReactRuntimeTests.luau", std::ios::binary);
        require(static_cast<bool>(source), "Missing React contract fixture");
        const std::string text{ std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>() };
        require(scripting.execute(text, "=ReactContract"), "Upstream React contract failed");
        require(lua_gettop(scripting.state()) == stackTop, "React execute leaked VM stack entries");
        std::ifstream tweenSource(std::filesystem::path(ENGINE_REACT_TEST_DIR) / "TweenServiceTests.luau", std::ios::binary);
        require(static_cast<bool>(tweenSource), "Missing TweenService contract fixture");
        const std::string tweenText{ std::istreambuf_iterator<char>(tweenSource), std::istreambuf_iterator<char>() };
        require(scripting.execute(tweenText, "=TweenContract"), "UI TweenService contract failed");
        require(lua_gettop(scripting.state()) == stackTop, "Tween contract leaked VM stack entries");
        std::ifstream canvasSource(std::filesystem::path(ENGINE_REACT_TEST_DIR) / "CanvasGroupTests.luau", std::ios::binary);
        require(static_cast<bool>(canvasSource), "Missing CanvasGroup contract fixture");
        const std::string canvasText{ std::istreambuf_iterator<char>(canvasSource), std::istreambuf_iterator<char>() };
        require(scripting.execute(canvasText, "=CanvasContract"), "UI CanvasGroup contract failed");
        require(lua_gettop(scripting.state()) == stackTop, "Canvas contract leaked VM stack entries");
        require(ReactRuntime::pump(scripting, 1.0 / 60.0), "React frame pump failed");
        require(ReactRuntime::reset(scripting), "React reset failed");
        require(scripting.execute("assert(#__turboUIRoots == 0 and #__turboUIEvents == 0)", "=ReactNativeGlobals"), "Native UI globals not connected to renderer");
        require(!scripting.execute("local UI=require('ReactTurbo'); UI.createRoot():render(require('React').createElement('Frame',{DefinitelyUnsupported=true}))", "=InvalidReactHostProperty"), "Unsupported React property escaped the protected runtime boundary");
        require(ReactRuntime::reset(scripting), "React did not recover after an unsupported host property");
        require(lua_gettop(scripting.state()) == stackTop, "React host error/reset leaked VM stack entries");
        const auto hud = world.createEntity();
        engine::scripting::ScriptComponent component;
        component.path = "scripts/ui/demo_ui.luau";
        scripting.populateDefaults(component);
        world.addComponent<engine::scripting::ScriptComponent>(hud, std::move(component));
        scripting.start();
        scripting.update(1.0 / 60.0);
        require(ReactRuntime::pump(scripting, 1.0 / 60.0), "Styled demo React pump failed");
        require(scripting.execute("assert(#__turboUIRoots==1); local screen=__turboUIRoots[1].children[1]; assert(screen.type=='ScreenGui'); local card=screen.children[1]; assert(card.type=='Frame' and card.props.Size.X.Offset==340 and card.props.BackgroundColor3.R>0)", "=StyledDemoSmoke"), "Real styled demo failed to mount");
        scripting.stop();
        require(ReactRuntime::reset(scripting), "Styled demo cleanup failed");
        require(scripting.execute("local UI=require('ReactTurbo'); local root=UI.createRoot(); root:render(require('React').createElement('TextButton', {Text='guard', [require('React').Event.Activated]=function() while true do end end})); table.insert(__turboUIEvents,{instance=root.container.children[1],name='Activated'})", "=ReactTimeoutSetup"), "Callback guard setup failed");
        auto smallLimits = originalLimits;
        smallLimits.callbackMilliseconds = 5.0;
        scripting.setLimits(smallLimits);
        require(!ReactRuntime::pump(scripting, 0.0), "UI callback escaped the runtime execution guard");
        scripting.setLimits(originalLimits);
        require(ReactRuntime::reset(scripting), "Runtime could not recover after UI callback timeout");
        std::cout << "React native bootstrap, upstream contract, stack and callback guard tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        printErrors(scripting);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
