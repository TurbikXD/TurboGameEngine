#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include "engine/ecs/entity.h"
#include "engine/editor/EditorCommands.h"

namespace engine::scripting { class ScriptingSystem; }
namespace engine::core { class JobSystem; }
namespace engine::editor {

// Editor documents are separate from the VM: unsaved edits never affect Play.
class ScriptWorkspace final {
public:
    struct Actions {
        std::function<void()> play, pause, stop, step, demo;
        std::function<void()> playWindow;
    };
    ScriptWorkspace();
    ~ScriptWorkspace();
    void initialize(std::filesystem::path assetRoot, scripting::ScriptingSystem& scripts, Actions actions, core::JobSystem* jobs = nullptr);
    bool openScript(const std::string& assetPath);
    void update();
    void render(bool* open, ecs::EntityId selected, bool playing, bool running);
    bool focused() const;
    std::string assistantContext() const;
    bool saveActive();
    bool saveAll();
    // Commands edit the document buffer; its TextEditor undo history remains
    // independent from scene Undo/Redo. Popup requests are consumed by render().
    CommandState commandState(EditorCommandId command) const;
    bool executeCommand(EditorCommandId command);
    void setCommandRegistry(EditorCommandRegistry* registry);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace engine::editor
