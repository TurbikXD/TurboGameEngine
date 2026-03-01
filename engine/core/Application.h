#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/core/Config.h"
#include "engine/game/StateStack.h"
#include "engine/renderer/Renderer.h"
#include "engine/rhi/Types.h"

namespace engine::platform {
struct Event;
class Window;
}

namespace engine::resources {
class Mesh;
class ShaderProgram;
class Texture;
} // namespace engine::resources

namespace engine::core {

class Application final {
public:
    explicit Application(rhi::BackendType backend);
    ~Application();

    bool init();
    int run();
    void shutdown();

private:
    struct DiligentSampleEntry final {
        std::string name;
        std::string category;
        std::string executablePath;
        bool executableExists{false};
    };

    enum class WorkspaceMode : std::uint8_t {
        EngineStates = 0,
        Tutorial01HelloTriangle,
        Tutorial03Texturing,
        ImGuiDemo,
        DiligentSamplesHub
    };

    void onEvent(const platform::Event& event);
    void renderWorkspace(double frameDtSeconds);
    void renderWorkspaceUi(double frameDtSeconds);
    void renderTutorial01HelloTriangle();
    void renderTutorial03Texturing(double frameDtSeconds);
    void ensureTutorial03Resources();
    void discoverDiligentSamples();
    bool launchSelectedDiligentSample();
    void initializeConfigHotReload();
    void pollRuntimeHotReload();
    static const char* workspaceModeLabel(WorkspaceMode mode);

    rhi::BackendType m_backend;
    EngineConfig m_config{};
    std::unique_ptr<platform::Window> m_window;
    renderer::Renderer m_renderer;
    game::StateStack m_stateStack;
    WorkspaceMode m_workspaceMode{WorkspaceMode::EngineStates};
    bool m_showImGuiDemoWindow{true};
    double m_workspaceSampleTime{0.0};
    std::vector<DiligentSampleEntry> m_diligentSamples;
    std::size_t m_selectedDiligentSample{0};
    int m_selectedSampleBackendMode{0};
    std::string m_diligentSamplesStatus;
    std::string m_sampleLaunchStatus;
    std::shared_ptr<resources::Mesh> m_tutorial03Mesh;
    std::shared_ptr<resources::Texture> m_tutorial03Texture;
    std::shared_ptr<resources::ShaderProgram> m_tutorial03Shader;
    std::filesystem::file_time_type m_configWriteTime{};
    bool m_configWatchActive{false};
    bool m_running{false};
    bool m_initialized{false};
    bool m_logInitialized{false};
};

} // namespace engine::core
