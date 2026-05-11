#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/EventBus.h"
#include "engine/ecs/collision_utils.h"
#include "engine/ecs/debug_render_system.h"
#include "engine/ecs/physics_system.h"
#include "engine/ecs/render_system.h"
#include "engine/ecs/world.h"
#include "engine/game/FreeCameraController.h"
#include "engine/game/IGameState.h"

namespace engine::renderer {
class RenderAdapter;
}

struct ImVec2;

namespace engine::game {

enum class EditorDockSlot : std::uint8_t {
    Top = 0,
    Center,
    LeftTop,
    LeftBottom,
    RightTop,
    RightBottom,
    Bottom,
    Floating
};

enum class EditorPanelId : std::uint8_t {
    Toolbar = 0,
    Hierarchy,
    Project,
    Inspector,
    Stats,
    Viewport,
    Status
};

class GameplayState final : public IGameState, public IGameStateUi {
public:
    explicit GameplayState(StateStack& stack);

    void onEnter() override;
    void onExit() override;
    void handleEvent(const platform::Event& event) override;
    void update(double dt) override;
    void render(renderer::Renderer& renderer) override;
    void renderUi(renderer::Renderer& renderer) override;

private:
    enum class EditorMode : std::uint8_t {
        Edit = 0,
        Play
    };

    enum class GizmoOperation : std::uint8_t {
        Translate = 0,
        Rotate,
        Scale
    };

    enum class EditorEntityKind : std::uint8_t {
        Empty = 0,
        RenderCube,
        PhysicsCube,
        PhysicsSphere,
        PhysicsPyramid,
        Camera
    };

    struct CameraFrame final {
        glm::mat4 view{1.0F};
        glm::mat4 projection{1.0F};
        glm::mat4 viewProjection{1.0F};
        float aspectRatio{16.0F / 9.0F};
    };

    struct PickRay final {
        glm::vec3 origin{0.0F};
        glm::vec3 direction{0.0F, 0.0F, 1.0F};
    };

    struct PickResult final {
        ecs::EntityId entity{ecs::kInvalidEntity};
        float distance{0.0F};
    };

    struct EntitySnapshot final {
        ecs::EntityId sourceId{ecs::kInvalidEntity};
        std::optional<ecs::Transform> transform;
        std::optional<ecs::Tag> tag;
        std::optional<ecs::Hierarchy> hierarchy;
        std::optional<ecs::Camera> camera;
        std::optional<ecs::Rigidbody> rigidbody;
        std::optional<ecs::Collider> collider;
        std::optional<ecs::MeshRenderer> meshRenderer;
    };

    struct SceneSnapshot final {
        std::vector<EntitySnapshot> entities;
        ecs::EntityId cameraEntity{ecs::kInvalidEntity};
        ecs::EntityId strikerEntity{ecs::kInvalidEntity};
        ecs::EntityId showcaseSphereEntity{ecs::kInvalidEntity};
        ecs::EntityId selectedEntity{ecs::kInvalidEntity};
        std::uint32_t spawnSequence{0};
        bool showColliderDebug{false};
        bool showcaseSphereLaunched{false};
    };

    struct EditorHistoryEntry final {
        std::string label;
        SceneSnapshot snapshot;
    };

    struct PendingSceneEdit final {
        std::string label;
        SceneSnapshot before;
        bool changed{false};
    };

    struct AssetEntry final {
        std::string path;
        std::string name;
        std::string kind;
        std::string details;
        std::array<std::uint32_t, 64> previewPixels{};
        std::uint32_t previewWidth{0};
        std::uint32_t previewHeight{0};
        bool hasPixelPreview{false};
    };

    void createDemoScene();
    void resetDemoScene();
    void startSimulation(bool relaunchStriker = false);
    void pauseSimulation();
    void launchStriker();
    void spawnDropWave();
    void processPendingSpawns();
    void renderEditorUi(renderer::Renderer& renderer);
    void renderMainMenuBar();
    void renderToolbar();
    void renderHierarchyPanel();
    void renderProjectPanel();
    void renderInspectorPanel();
    void renderStatsPanel(const renderer::Renderer& renderer);
    void renderViewportPanel(renderer::Renderer& renderer);
    void renderViewportGizmo(const CameraFrame& cameraFrame);
    void renderViewportOverlay(const CameraFrame& cameraFrame) const;
    void renderStatusBar();
    void renderPanelDockControls(EditorPanelId panelId, const char* popupId);
    void renderDockDropOverlay();
    void renderDockDropZone(EditorPanelId draggedPanel, EditorDockSlot targetSlot, const ImVec2& position, const ImVec2& size);
    void renderDockResizeHandles();
    void renderSelectionOutline(renderer::RenderAdapter& renderer, const glm::mat4& viewProjectionMatrix);
    void handleViewportPicking(const CameraFrame& cameraFrame);
    void renderEntityComponentEditors(ecs::EntityId entity);
    void renderCreateEntityMenu();
    void renderAddComponentMenu(ecs::EntityId entity);
    void renderRemoveComponentMenu(ecs::EntityId entity);
    void renderEntityContextMenu(ecs::EntityId entity);
    void enterPlayMode();
    void stopPlayMode();
    [[nodiscard]] SceneSnapshot captureSceneSnapshot() const;
    void restoreSceneSnapshot(const SceneSnapshot& snapshot);
    void pushHistorySnapshot(const std::string& label, SceneSnapshot snapshot);
    void recordSceneHistory(const std::string& label);
    void beginSceneEdit(const std::string& label, const SceneSnapshot& before);
    void trackEditedItem(const std::string& label, bool changed, const SceneSnapshot& before);
    void commitSceneEdit();
    void cancelSceneEdit();
    void undoSceneEdit();
    void redoSceneEdit();
    void saveSceneToDisk();
    void loadSceneFromDisk();
    [[nodiscard]] CameraFrame buildActiveCameraFrame(const renderer::Renderer& renderer) const;
    [[nodiscard]] std::optional<PickRay> buildPickRay(const CameraFrame& cameraFrame, const glm::vec2& mousePosition) const;
    [[nodiscard]] std::optional<PickResult> pickEntity(const PickRay& ray) const;
    void ensureSelectedEntityValid();
    void processEditorShortcuts();
    void applyViewportHotkeys();
    void wakeEntity(ecs::EntityId entity);
    [[nodiscard]] bool canEditScene() const;
    [[nodiscard]] bool hasUndo() const;
    [[nodiscard]] bool hasRedo() const;
    [[nodiscard]] bool computeEntityPickShape(ecs::EntityId entity, ecs::WorldColliderShape& outShape) const;
    [[nodiscard]] bool entityMatchesFilter(ecs::EntityId entity) const;
    [[nodiscard]] std::string entityDisplayName(ecs::EntityId entity) const;
    [[nodiscard]] std::vector<ecs::EntityId> sortedEntities() const;
    void refreshProjectAssets();
    [[nodiscard]] glm::vec3 editorSpawnPosition() const;
    [[nodiscard]] std::optional<ecs::WorldAabb> computeSceneBounds() const;
    void setCameraLookActive(bool active);
    void focusSelectedEntity();
    void focusSceneBounds();
    void selectNextEntity(int direction);
    void resetSelectedTransform();
    void dropSelectedToGround();
    void zeroSelectedVelocity();
    void randomizeSelectedTint();
    void setSelectedVisible(bool visible);
    void zeroAllVelocities();
    void setAllDynamicsStatic(bool makeStatic);
    void storeCameraBookmark(std::size_t index);
    void recallCameraBookmark(std::size_t index);
    void dockPanel(EditorDockSlot& panelSlot, EditorDockSlot newSlot);
    [[nodiscard]] EditorDockSlot* dockSlotForPanel(EditorPanelId panelId);
    [[nodiscard]] const EditorDockSlot* dockSlotForPanel(EditorPanelId panelId) const;
    void resetEditorLayout();
    void loadEditorLayout();
    void saveEditorLayout() const;
    void markEditorLayoutDirty();
    void updateHoveredEntity(const CameraFrame& cameraFrame);
    void updateFrameHistory();
    ecs::EntityId createEditorEntity(EditorEntityKind kind);
    ecs::EntityId duplicateEntity(ecs::EntityId entity);
    void deleteEntity(ecs::EntityId entity);
    ecs::EntityId spawnStaticBody(
        const std::string& tag,
        const glm::vec3& position,
        const glm::vec3& scale,
        const glm::vec4& tint,
        const std::string& textureId = "",
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F));
    ecs::EntityId spawnDynamicBody(
        const std::string& tag,
        const glm::vec3& position,
        const glm::vec3& scale,
        const glm::vec4& tint,
        float mass,
        bool pyramidMesh = false,
        const std::string& textureId = "",
        float restitution = 0.06F,
        float friction = 0.82F,
        const glm::vec2& uvScale = glm::vec2(1.0F, 1.0F));
    ecs::EntityId spawnDynamicSphere(
        const std::string& tag,
        const glm::vec3& position,
        float diameter,
        const glm::vec4& tint,
        float mass,
        float restitution = 0.04F,
        float friction = 0.92F);
    void bindCollisionEventHandlers();

    ecs::World m_world{};
    ecs::RenderSystem m_renderSystem{};
    ecs::DebugRenderSystem m_debugRenderSystem{};
    ecs::PhysicsSystem m_physicsSystem{};
    core::EventBus m_eventBus{};
    FreeCameraController m_cameraController{};
    std::deque<std::function<void()>> m_pendingSpawnJobs{};
    std::vector<EditorHistoryEntry> m_undoStack{};
    std::vector<EditorHistoryEntry> m_redoStack{};
    std::vector<AssetEntry> m_projectAssets{};
    std::optional<SceneSnapshot> m_editModeSnapshot{};
    std::optional<PendingSceneEdit> m_pendingSceneEdit{};
    std::optional<SceneSnapshot> m_gizmoEditSnapshot{};
    std::array<std::optional<ecs::Transform>, 4> m_cameraBookmarks{};
    std::array<float, 180> m_frameTimeHistory{};
    std::array<float, 180> m_renderTimeHistory{};
    ecs::EntityId m_cameraEntity{ecs::kInvalidEntity};
    ecs::EntityId m_strikerEntity{ecs::kInvalidEntity};
    ecs::EntityId m_showcaseSphereEntity{ecs::kInvalidEntity};
    ecs::EntityId m_selectedEntity{ecs::kInvalidEntity};
    ecs::EntityId m_hoveredEntity{ecs::kInvalidEntity};
    EditorMode m_editorMode{EditorMode::Edit};
    GizmoOperation m_gizmoOperation{GizmoOperation::Translate};
    glm::vec2 m_viewportPosition{0.0F, 0.0F};
    glm::vec2 m_viewportSize{1280.0F, 720.0F};
    glm::vec2 m_lastHoverPickMouse{-100000.0F, -100000.0F};
    std::string m_hierarchyFilter{};
    std::string m_assetFilter{};
    std::string m_selectedAssetPath{};
    bool m_sceneInitialized{false};
    bool m_simulationRunning{false};
    bool m_showColliderDebug{false};
    bool m_cameraLookActive{false};
    bool m_skipNextCameraLookDelta{false};
    bool m_showcaseSphereLaunched{false};
    bool m_viewportHovered{false};
    bool m_viewportFocused{false};
    bool m_useLocalGizmoSpace{true};
    bool m_snapEnabled{false};
    bool m_showControlsPopup{false};
    bool m_resetEditorLayout{false};
    bool m_editorLayoutLocked{false};
    bool m_showToolbarPanel{true};
    bool m_showHierarchyPanel{true};
    bool m_showProjectPanel{true};
    bool m_showInspectorPanel{true};
    bool m_showStatsPanel{true};
    bool m_showViewportPanel{true};
    bool m_showStatusPanel{true};
    EditorDockSlot m_toolbarDockSlot{EditorDockSlot::Top};
    EditorDockSlot m_hierarchyDockSlot{EditorDockSlot::LeftTop};
    EditorDockSlot m_projectDockSlot{EditorDockSlot::LeftBottom};
    EditorDockSlot m_inspectorDockSlot{EditorDockSlot::RightTop};
    EditorDockSlot m_statsDockSlot{EditorDockSlot::RightBottom};
    EditorDockSlot m_viewportDockSlot{EditorDockSlot::Center};
    EditorDockSlot m_statusDockSlot{EditorDockSlot::Bottom};
    float m_editorLeftWidth{0.0F};
    float m_editorRightWidth{0.0F};
    float m_editorLeftSplit{0.72F};
    float m_editorRightSplit{0.68F};
    EditorPanelId m_draggedDockPanel{EditorPanelId::Toolbar};
    bool m_dockDragActive{false};
    bool m_dockDropAcceptedThisFrame{false};
    bool m_editorLayoutDirty{false};
    bool m_projectAssetsDirty{true};
    bool m_showHoverOutline{true};
    bool m_gizmoWasUsing{false};
    bool m_gizmoChanged{false};
    double m_lastSceneRenderMs{0.0};
    std::size_t m_historyCursor{0};
    std::uint32_t m_spawnSequence{0};
    std::uint32_t m_hoverPickFrame{0};
    std::uint32_t m_collisionEnterCount{0};
    std::uint32_t m_collisionStayCount{0};
    std::uint32_t m_collisionExitCount{0};
    std::string m_lastCollisionMessage{"No collisions yet."};
    std::string m_editorStatusMessage{"Editor ready."};
};

} // namespace engine::game
