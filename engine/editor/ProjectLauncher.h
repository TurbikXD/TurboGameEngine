#pragma once

#include <functional>
#include <memory>
#include <string>

namespace engine::core { struct Project; class ProjectManager; }

namespace engine::editor {

class ProjectLauncher final {
public:
    ProjectLauncher(core::ProjectManager& manager, std::function<void(core::Project)> onOpen,
                    std::function<void()> onEngineDemo);
    ~ProjectLauncher();
    ProjectLauncher(const ProjectLauncher&) = delete;
    ProjectLauncher& operator=(const ProjectLauncher&) = delete;

    void render();

private:
    friend struct ProjectLauncherTestAccess;
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

struct ProjectLauncherTestAccess final {
    enum class Action { NewProject, EngineDemo, CreateProject, OpenSelected, EnterPath, OpenTyped };
    static void actionCenter(const ProjectLauncher& launcher, Action action, float& x, float& y);
    static void fillCreateForm(ProjectLauncher& launcher, std::string name, std::string location);
    static void fillOpenForm(ProjectLauncher& launcher, std::string path);
    static std::string error(const ProjectLauncher& launcher);
};

}
