#pragma once

#include <memory>

#include "engine/core/Config.h"
#include "engine/game/StateStack.h"
#include "engine/renderer/Renderer.h"
#include "engine/rhi/Types.h"

namespace engine::platform {
struct Event;
class Window;
}

namespace engine::core {

class Application final {
public:
    explicit Application(rhi::BackendType backend);
    ~Application();

    bool init();
    int run();
    void shutdown();

private:
    void onEvent(const platform::Event& event);

    rhi::BackendType m_backend;
    EngineConfig m_config{};
    std::unique_ptr<platform::Window> m_window;
    renderer::Renderer m_renderer;
    game::StateStack m_stateStack;
    bool m_running{false};
    bool m_initialized{false};
    bool m_logInitialized{false};
};

} // namespace engine::core
