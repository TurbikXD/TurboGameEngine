#include "engine/game/GameplayState.h"

#include "engine/game/StateStack.h"
#include "engine/platform/Window.h"
#include "engine/ecs/components.h"
#include "engine/editor/MaterialInspector.h"
#include "engine/network/NetworkSession.h"
#include <exception>
#include <unordered_set>
#include <imgui.h>

namespace engine::game {
namespace {
using Command = editor::EditorCommandId;
editor::CommandState available(bool enabled, bool checked = false, std::string_view reason = {}) {
    return {enabled, checked, enabled ? std::string_view{} : reason};
}
bool isScriptCommand(Command id) {
    return id >= Command::ScriptNew && id <= Command::ScriptErrors;
}
}

void GameplayState::initializeEditorCommands() {
    m_editorCommands.setHandlers(
        [this](Command id) { return queryEditorCommand(id); },
        [this](Command id) { executeEditorAction(id); });
    m_scriptWorkspace.setCommandRegistry(&m_editorCommands);
}

editor::CommandState GameplayState::queryEditorCommand(Command id) const {
    if (stack().standalone() || labActive()) return available(false, false, "Editor commands are unavailable in this mode.");
    if (isScriptCommand(id)) return m_scriptWorkspace.commandState(id);
    const bool selected = m_world.isAlive(m_selectedEntity);
    const bool editable = canEditScene();
    const bool playing = m_editorMode == EditorMode::Play;
    switch (id) {
        case Command::SaveScene: case Command::LoadScene:
            return available(!playing, false, "Stop Play before saving or loading the authored scene.");
        case Command::Undo: return available(editable && (hasUndo() || m_gizmoChanged || (m_pendingSceneEdit && m_pendingSceneEdit->changed)), false, "No scene edit to undo.");
        case Command::Redo: return available(editable && hasRedo() && !m_gizmoChanged && !(m_pendingSceneEdit && m_pendingSceneEdit->changed), false, "No scene edit to redo.");
        case Command::CreateEmpty: case Command::CreateRenderCube: case Command::CreatePhysicsCube:
        case Command::CreatePhysicsSphere: case Command::CreatePhysicsPyramid: case Command::CreateCamera:
        case Command::ScriptDemo: return available(editable, false, "Scene editing is unavailable.");
        case Command::Duplicate: case Command::Delete: case Command::AddComponent:
            return available(editable && selected, false, "Select a live entity.");
        case Command::FocusSelected: case Command::ResetTransform: case Command::DropToGround:
            return available(selected && m_world.hasComponent<ecs::Transform>(m_selectedEntity), false, "Select an entity with a Transform.");
        case Command::FrameScene: return available(m_world.aliveCount() != 0, false, "The scene is empty.");
        case Command::ZeroVelocity: return available(selected && m_world.hasComponent<ecs::Rigidbody>(m_selectedEntity), false, "Select an entity with a Rigidbody.");
        case Command::RandomizeTint: return available(selected && m_world.hasComponent<ecs::MeshRenderer>(m_selectedEntity), false, "Select an entity with a MeshRenderer.");
        case Command::ToggleVisible: {
            const auto* mesh = selected ? m_world.getComponent<ecs::MeshRenderer>(m_selectedEntity) : nullptr;
            return available(mesh != nullptr, mesh && mesh->visible, "Select an entity with a MeshRenderer.");
        }
        case Command::ToggleStatic: case Command::ToggleGravity: {
            const auto* body = selected ? m_world.getComponent<ecs::Rigidbody>(m_selectedEntity) : nullptr;
            return available(body != nullptr, body && (id == Command::ToggleStatic ? body->isStatic : body->useGravity), "Select an entity with a Rigidbody.");
        }
        case Command::ToggleCollider: {
            const auto* collider = selected ? m_world.getComponent<ecs::Collider>(m_selectedEntity) : nullptr;
            return available(collider != nullptr, collider && collider->enabled, "Select an entity with a Collider.");
        }
        case Command::ToolSelect: return available(true, m_selectionToolActive);
        case Command::ToolMove: return available(true, !m_selectionToolActive && m_gizmoOperation == GizmoOperation::Translate);
        case Command::ToolRotate: return available(true, !m_selectionToolActive && m_gizmoOperation == GizmoOperation::Rotate);
        case Command::ToolScale: return available(true, !m_selectionToolActive && m_gizmoOperation == GizmoOperation::Scale);
        case Command::ToggleLocalSpace: return available(true, m_useLocalGizmoSpace);
        case Command::ToggleSnap: return available(true, m_snapEnabled);
        case Command::Play: return available(!playing || !m_simulationRunning, playing, "Simulation is already running.");
        case Command::PlayWindow: return available(!stack().playWindowRunning(), false, "The separate game is already running.");
        case Command::StopWindow: return available(stack().playWindowRunning(), false, "No separate game is running.");
        case Command::StartNetworkTest:
            return available(!playing && !stack().networkPlayRunning(), false,
                playing ? "Stop in-editor Play before starting a network test." : "A network test is already running.");
        case Command::StopNetworkTest:
            return available(stack().networkPlayRunning(), false, "No network test is running.");
        case Command::Pause: return available(playing, playing && !m_simulationRunning, "Start Play first.");
        case Command::Stop: return available(playing, false, "The editor is already stopped.");
        case Command::StepTick: return available(playing && !m_simulationRunning && !m_stepScriptTick, false, "Pause Play before stepping one simulation tick.");
        case Command::ShowHierarchy: return available(true, m_showHierarchyPanel);
        case Command::ShowInspector: return available(true, m_showInspectorPanel);
        case Command::ShowContent: return available(true, m_showProjectPanel);
        case Command::ShowStats: return available(true, m_showStatsPanel);
        case Command::ShowScriptWorkspace: return available(true, m_showScriptWorkspace);
        case Command::ShowAssistant: return available(true, m_showAssistant);
        case Command::ShowViewport: return available(true, m_showViewportPanel);
        case Command::ShowStatus: return available(true, m_showStatusPanel);
        case Command::ToggleColliderDebug: return available(true, m_showColliderDebug);
        case Command::ToggleHoverOutline: return available(true, m_showHoverOutline);
        case Command::ToggleLayoutLock: return available(true, m_editorLayoutLocked);
        case Command::BookmarkStore1: return available(m_world.isAlive(m_cameraEntity), false, "No editor camera.");
        case Command::BookmarkRecall1: return available(m_cameraBookmarks[0].has_value(), false, "Store a camera bookmark first.");
        case Command::ProjectSwitch: case Command::ResetLayout: case Command::SaveLayout: return available(true);
        default: return available(false, false, "Unknown editor command.");
    }
}

void GameplayState::finishGizmoEdit() {
    if (canEditScene() && m_gizmoChanged && m_gizmoEditSnapshot) {
        pushHistorySnapshot("Gizmo Transform", std::move(*m_gizmoEditSnapshot));
    }
    m_gizmoEditSnapshot.reset();
    m_gizmoChanged = false;
    m_gizmoWasUsing = false;
}

void GameplayState::executeEditorAction(Command id) {
    finishGizmoEdit();
    commitSceneEdit();
    if (isScriptCommand(id)) {
        m_showScriptWorkspace = true;
        m_scriptWorkspace.executeCommand(id);
        return;
    }
    auto toggle = [this](bool& value) { value = !value; markEditorLayoutDirty(); };
    switch (id) {
        case Command::ProjectSwitch: m_projectSwitchRequested = true; break;
        case Command::SaveScene: saveSceneToDisk(); break;
        case Command::LoadScene: loadSceneFromDisk(); break;
        case Command::Undo: undoSceneEdit(); break;
        case Command::Redo: redoSceneEdit(); break;
        case Command::CreateEmpty: createEditorEntity(EditorEntityKind::Empty); break;
        case Command::CreateRenderCube: createEditorEntity(EditorEntityKind::RenderCube); break;
        case Command::CreatePhysicsCube: createEditorEntity(EditorEntityKind::PhysicsCube); break;
        case Command::CreatePhysicsSphere: createEditorEntity(EditorEntityKind::PhysicsSphere); break;
        case Command::CreatePhysicsPyramid: createEditorEntity(EditorEntityKind::PhysicsPyramid); break;
        case Command::CreateCamera: createEditorEntity(EditorEntityKind::Camera); break;
        case Command::Duplicate: duplicateEntity(m_selectedEntity); break;
        case Command::Delete: deleteEntity(m_selectedEntity); break;
        case Command::AddComponent: m_componentMenuEntity = m_selectedEntity; m_addComponentRequested = true; break;
        case Command::ToolSelect: m_selectionToolActive = true; break;
        case Command::ToolMove: case Command::ToolRotate: case Command::ToolScale:
            m_selectionToolActive = false;
            m_gizmoOperation = id == Command::ToolMove ? GizmoOperation::Translate : id == Command::ToolRotate ? GizmoOperation::Rotate : GizmoOperation::Scale;
            break;
        case Command::ToggleLocalSpace: toggle(m_useLocalGizmoSpace); break;
        case Command::ToggleSnap: toggle(m_snapEnabled); break;
        case Command::FocusSelected: focusSelectedEntity(); break;
        case Command::FrameScene: focusSceneBounds(); break;
        case Command::ResetTransform: resetSelectedTransform(); break;
        case Command::DropToGround: dropSelectedToGround(); break;
        case Command::ZeroVelocity: zeroSelectedVelocity(); break;
        case Command::RandomizeTint: randomizeSelectedTint(); break;
        case Command::ToggleVisible: setSelectedVisible(!m_world.getComponent<ecs::MeshRenderer>(m_selectedEntity)->visible); break;
        case Command::ToggleStatic: case Command::ToggleGravity: {
            auto* body = m_world.getComponent<ecs::Rigidbody>(m_selectedEntity);
            recordSceneHistory(id == Command::ToggleStatic ? "Toggle Static" : "Toggle Gravity");
            if (id == Command::ToggleStatic) { body->isStatic = !body->isStatic; body->recalculateMassProperties(); }
            else { body->useGravity = !body->useGravity; }
            body->wakeUp();
            break;
        }
        case Command::ToggleCollider: {
            recordSceneHistory("Toggle Collider");
            auto* collider = m_world.getComponent<ecs::Collider>(m_selectedEntity);
            collider->enabled = !collider->enabled;
            wakeEntity(m_selectedEntity);
            break;
        }
        case Command::Play: if (m_editorMode == EditorMode::Edit) enterPlayMode(); else startSimulation(); break;
        case Command::PlayWindow: launchStandalonePlay(); break;
        case Command::StopWindow: stack().stopPlayWindow(); break;
        case Command::StartNetworkTest: {
            try {
                const auto options = m_ribbonState.networkTest;
                if (!options.valid()) {
                    m_editorStatusMessage = "Network test settings are out of range.";
                    break;
                }
                std::unordered_set<std::uint32_t> keys;
                std::string error;
                m_world.forEach<net::NetworkReplica>([&](ecs::EntityId entity, const net::NetworkReplica& replica) {
                    if (!error.empty()) return;
                    if (!m_world.hasComponent<ecs::Transform>(entity)) {
                        error = entityDisplayName(entity) + " needs a Transform for network replication.";
                    } else if (replica.key == 0 || replica.key > net::MaxReplicaKey || !keys.insert(replica.key).second) {
                        error = entityDisplayName(entity) + " needs a unique Network Replica key from 1 to 2147483647.";
                    }
                });
                const auto replicaCapacity = net::MaxReplicas - static_cast<std::size_t>(options.clientCount);
                if (error.empty() && keys.size() > replicaCapacity) {
                    error = "The selected client count leaves room for " + std::to_string(replicaCapacity) +
                        " network replicas. The scene has " + std::to_string(keys.size()) + ".";
                }
                if (error.empty() && stack().launchNetworkPlay(
                    serializeScene(captureSceneSnapshot(), true), editorAssetsRoot(), options, error)) {
                    m_editorStatusMessage = "Starting the network server and " + std::to_string(options.clientCount) +
                        (options.clientCount == 1 ? " client." : " clients.");
                } else {
                    m_editorStatusMessage = std::move(error);
                }
            } catch (const std::exception& failure) {
                m_editorStatusMessage = failure.what();
            }
            break;
        }
        case Command::StopNetworkTest:
            stack().stopNetworkPlay();
            m_editorStatusMessage = "Network test stopped.";
            break;
        case Command::Pause: if (m_simulationRunning) pauseSimulation(); else startSimulation(); break;
        case Command::Stop: stopPlayMode(); break;
        case Command::StepTick: m_stepScriptTick = true; break;
        case Command::ScriptDemo: createScriptDemo(); break;
        case Command::ShowHierarchy: toggle(m_showHierarchyPanel); break;
        case Command::ShowInspector: toggle(m_showInspectorPanel); break;
        case Command::ShowContent: toggle(m_showProjectPanel); break;
        case Command::ShowStats: toggle(m_showStatsPanel); break;
        case Command::ShowScriptWorkspace: toggle(m_showScriptWorkspace); break;
        case Command::ShowAssistant: toggle(m_showAssistant); break;
        case Command::ShowViewport: toggle(m_showViewportPanel); break;
        case Command::ShowStatus: toggle(m_showStatusPanel); break;
        case Command::ToggleColliderDebug: toggle(m_showColliderDebug); break;
        case Command::ToggleHoverOutline: toggle(m_showHoverOutline); break;
        case Command::ToggleLayoutLock: toggle(m_editorLayoutLocked); break;
        case Command::ResetLayout: resetEditorLayout(); break;
        case Command::SaveLayout: saveEditorLayout(); break;
        case Command::BookmarkStore1: storeCameraBookmark(0); break;
        case Command::BookmarkRecall1: recallCameraBookmark(0); break;
        default: break;
    }
}

bool GameplayState::renderCommandMenuItem(Command id, const char* label) {
    const auto* definition = editor::EditorCommandRegistry::definition(id);
    if (!definition) return false;
    const auto state = m_editorCommands.state(id);
    const bool clicked = ImGui::MenuItem(label ? label : definition->label.data(),
        definition->shortcut.empty() ? nullptr : definition->shortcut.data(), state.checked, state.enabled);
    if (!state.enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%.*s", static_cast<int>(state.disabledReason.size()), state.disabledReason.data());
    }
    return clicked && m_editorCommands.execute(id);
}

bool GameplayState::sceneShortcutsBlocked() const {
    return ImGui::GetCurrentContext() && (ImGui::GetIO().WantTextInput ||
        m_showScriptWorkspace || editor::materialGraphEditorFocused() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
}
}
