#include "engine/core/Application.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#include <glm/vec4.hpp>

#include "engine/core/Log.h"
#include "engine/core/Time.h"
#include "engine/game/GameplayState.h"
#include "engine/game/MenuState.h"
#include "engine/game/PauseState.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/rhi/RHI.h"

namespace engine::core {

Application::Application(rhi::BackendType backend) : m_backend(backend) {}

Application::~Application() {
    shutdown();
}

bool Application::init() {
    Log::init();
    m_logInitialized = true;
    ENGINE_LOG_INFO("Application init started");

    m_config = Config::load("config.json");

    platform::WindowDesc windowDesc{};
    windowDesc.width = m_config.width;
    windowDesc.height = m_config.height;
    windowDesc.title = m_config.title;
    windowDesc.vsync = m_config.vsync;
    windowDesc.useOpenGLContext = (m_backend == rhi::BackendType::OpenGL);

    try {
        m_window = platform::Window::create(windowDesc);
    } catch (const std::exception& ex) {
        ENGINE_LOG_ERROR("Window creation failed: {}", ex.what());
        return false;
    }

    platform::Input::reset();
    m_window->setEventCallback([this](const platform::Event& event) { onEvent(event); });

    if (!m_renderer.init(*m_window, m_backend, m_config.vsync)) {
        ENGINE_LOG_ERROR("Renderer failed to initialize");
        return false;
    }

    if (m_config.initialState == "gameplay") {
        m_stateStack.push(std::make_unique<game::GameplayState>(m_stateStack));
    } else if (m_config.initialState == "pause") {
        m_stateStack.push(std::make_unique<game::GameplayState>(m_stateStack));
        m_stateStack.push(std::make_unique<game::PauseState>(m_stateStack));
    } else {
        m_stateStack.push(std::make_unique<game::MenuState>(m_stateStack));
    }
    m_stateStack.applyPendingChanges();

    m_running = true;
    m_initialized = true;
    ENGINE_LOG_INFO("Application init complete, backend={}", rhi::toString(m_backend));
    return true;
}

int Application::run() {
    if (!m_initialized) {
        return 1;
    }

    FrameTimer timer;
    timer.reset();

    constexpr double fixedDt = 1.0 / 60.0;
    double accumulator = 0.0;
    TimeStatsAggregator stats(1.0);

    while (m_running && m_window && !m_window->shouldClose()) {
        m_window->pollEvents();

        double frameDt = timer.tickSeconds();
        frameDt = std::min(frameDt, 0.25);

        accumulator += frameDt;
        while (accumulator >= fixedDt) {
            m_stateStack.update(fixedDt);
            m_stateStack.applyPendingChanges();
            accumulator -= fixedDt;
        }

        const glm::vec4 clearColor{
            m_config.clearColor[0], m_config.clearColor[1], m_config.clearColor[2], m_config.clearColor[3]};
        m_renderer.beginFrame(clearColor);
        m_stateStack.render(m_renderer);
        m_renderer.endFrame();

        stats.sample(frameDt);
        TimeStats sampled{};
        if (stats.consume(sampled)) {
            ENGINE_LOG_INFO(
                "Frame dt stats: avg={:.3f}ms min={:.3f}ms max={:.3f}ms samples={}",
                sampled.avgMs,
                sampled.minMs,
                sampled.maxMs,
                sampled.sampleCount);
        }

        if (m_stateStack.empty()) {
            ENGINE_LOG_WARN("State stack empty, exiting");
            m_running = false;
        }
    }

    shutdown();
    return 0;
}

void Application::shutdown() {
    if (!m_initialized && m_window == nullptr && !m_logInitialized) {
        return;
    }

    if (m_initialized) {
        ENGINE_LOG_INFO("Application shutdown started");
    }
    m_running = false;
    m_stateStack.clear();
    m_stateStack.applyPendingChanges();
    m_renderer.shutdown();
    m_window.reset();
    m_initialized = false;
    if (m_logInitialized) {
        ENGINE_LOG_INFO("Application shutdown complete");
        Log::shutdown();
        m_logInitialized = false;
    }
}

void Application::onEvent(const platform::Event& event) {
    platform::Input::onEvent(event);

    if (event.type == platform::EventType::Quit) {
        ENGINE_LOG_INFO("Quit event received");
        m_running = false;
    } else if (event.type == platform::EventType::Resize) {
        ENGINE_LOG_INFO("Window resized to {}x{}", event.width, event.height);
        m_renderer.onResize(static_cast<std::uint32_t>(event.width), static_cast<std::uint32_t>(event.height));
    }

    m_stateStack.handleEvent(event);
    m_stateStack.applyPendingChanges();
}

} // namespace engine::core
