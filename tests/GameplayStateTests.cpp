#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include <entt/entity/entity.hpp>
#include <imgui.h>
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/misc/cpp/imgui_stdlib.h"

#include "engine/core/JobSystem.h"
#include "engine/core/LabOptions.h"
#include "engine/ecs/components.h"
#include "engine/game/GameplayState.h"
#include "engine/game/StateStack.h"
#include "engine/platform/Input.h"

namespace engine::game {

struct GameplayStateTestAccess {
    static void require(const bool condition, const char* message) {
        if (!condition) { throw std::runtime_error(message); }
    }

    static ecs::EntityId addBody(GameplayState& state, const char* name, const glm::vec3 position) {
        const auto entity = state.m_world.createEntity();
        ecs::Transform transform;
        transform.position = position;
        state.m_world.addComponent<ecs::Transform>(entity, transform);
        state.m_world.addComponent<ecs::Tag>(entity, ecs::Tag{name});
        ecs::Rigidbody body;
        body.useGravity = false;
        state.m_world.addComponent<ecs::Rigidbody>(entity, body);
        state.m_world.addComponent<ecs::Collider>(entity);
        return entity;
    }

    static ecs::EntityId tagged(const GameplayState& state, const char* name) {
        ecs::EntityId found = ecs::kInvalidEntity;
        state.m_world.forEach<ecs::Tag>([&](const auto entity, const auto& tag) {
            if (tag.value == name) { found = entity; }
        });
        return found;
    }

    static void sceneSelectionIsIndependentOfTracy(core::JobSystem& jobs) {
        core::LabOptions options;
        StateStack stack;
        stack.setServices(&jobs, &options);
        {
            GameplayState state(stack);
            state.onEnter();
            require(state.m_world.aliveCount() == 21U, "Tracy/default launch added stress bodies");
            require(state.m_editorMode == GameplayState::EditorMode::Edit && !state.m_simulationRunning,
                    "Default launch automatically entered Play");
        }
        options.stressScene = true;
        {
            GameplayState state(stack);
            state.onEnter();
            require(state.m_world.aliveCount() == 1301U, "Explicit stress scene is missing its 1280 bodies");
            require(state.m_editorMode == GameplayState::EditorMode::Play && state.m_simulationRunning,
                    "Explicit stress scene did not start simulation");
        }
        options.scene = "ecs"; // Lab scenes retain priority, even with a stale stress environment flag.
        options.entityCount = 64U;
        {
            GameplayState state(stack);
            state.onEnter();
            const auto firstCount = state.m_world.aliveCount();
            require(state.m_labEntities.size() == 64U && state.m_editorMode == GameplayState::EditorMode::Edit,
                    "Stress option changed the existing ECS lab workload/mode");
            options.stressScene = false;
            state.resetDemoScene();
            require(state.m_world.aliveCount() == firstCount, "Stress option changed lab entity count");
        }
    }

    static void runtimeDeleteAndRestore(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        const auto parent = addBody(state, "parent", {2.0F, 0.0F, 0.0F});
        const auto child = addBody(state, "child", {6.0F, 0.0F, 0.0F});
        state.m_world.addComponent<ecs::Hierarchy>(child, ecs::Hierarchy{parent});
        state.m_world.addComponent<ecs::Camera>(parent);
        state.m_selectedEntity = state.m_hoveredEntity = parent;
        state.m_cameraEntity = state.m_strikerEntity = state.m_showcaseSphereEntity = parent;
        state.enterPlayMode();
        state.m_simulationRunning = false; // Delete must also work while paused.
        state.m_gizmoWasUsing = state.m_gizmoChanged = true;

        platform::Event key;
        key.type = platform::EventType::KeyPressed;
        key.key = platform::KeyCode::Delete;
        ImGui::GetIO().WantTextInput = true;
        state.handleEvent(key);
        require(state.m_pendingRuntimeDeletes.empty(), "Delete in text input queued an entity deletion");
        ImGui::GetIO().WantTextInput = false;
        key.repeat = true;
        state.handleEvent(key);
        require(state.m_pendingRuntimeDeletes.empty(), "Repeated key queued a deletion");
        key.repeat = false;
        state.handleEvent(key);
        state.deleteEntity(parent); // Inspector/context menu use this same entry.
        require(state.m_pendingRuntimeDeletes.size() == 1, "Duplicate requests were not coalesced");
        require(state.m_world.isAlive(parent), "UI deleted entity before the safe update boundary");
        state.update(1.0 / 60.0);
        require(!state.m_world.isAlive(parent), "Runtime deletion was not applied");
        require(state.m_world.getComponent<ecs::Rigidbody>(parent) == nullptr, "Components survived deletion");
        require(state.m_world.isAlive(child), "Deleting a parent deleted its child");
        require(state.m_world.getComponent<ecs::Hierarchy>(child)->parent == ecs::kInvalidEntity,
                "Child kept a dangling parent ID");
        require(state.m_hoveredEntity == ecs::kInvalidEntity && state.m_selectedEntity != parent &&
                state.m_cameraEntity == ecs::kInvalidEntity && state.m_strikerEntity == ecs::kInvalidEntity &&
                state.m_showcaseSphereEntity == ecs::kInvalidEntity, "Entity references survived deletion");
        require(!state.m_gizmoWasUsing && !state.m_gizmoChanged, "Deleted selection left an active gizmo");
        require(state.m_undoStack.empty(), "Runtime deletion changed Edit undo history");
        state.deleteEntity(parent); // Dead/invalid requests are harmless.
        state.deleteEntity(ecs::kInvalidEntity);
        state.stopPlayMode();
        const auto restoredParent = tagged(state, "parent");
        const auto restoredChild = tagged(state, "child");
        require(state.m_world.isAlive(restoredParent) && state.m_world.isAlive(restoredChild), "Stop lost snapshot entities");
        require(state.m_world.getComponent<ecs::Hierarchy>(restoredChild)->parent == restoredParent,
                "Stop did not restore parent relationship");
        require(state.m_world.getComponent<ecs::Transform>(restoredParent)->position.x == 2.0F,
                "Stop did not restore the pre-Play transform");

        state.enterPlayMode();
        state.deleteEntity(restoredParent);
        state.stopPlayMode(); // Stop can happen before the next fixed update.
        state.update(1.0 / 60.0);
        require(state.m_world.isAlive(tagged(state, "parent")), "Pending Play delete targeted the restored Edit scene");
        state.deleteEntity(tagged(state, "parent"));
        require(!state.m_world.isAlive(tagged(state, "parent")) && state.hasUndo(), "Edit Delete/Undo regressed");
        state.undoSceneEdit();
        require(state.m_world.isAlive(tagged(state, "parent")), "Undo did not restore an Edit deletion");
        state.redoSceneEdit();
        require(!state.m_world.isAlive(tagged(state, "parent")), "Redo did not reapply an Edit deletion");
    }

    static void parallelPhysicsAndRecycledIds(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        auto settings = state.m_physicsSystem.settings();
        settings.gravity = glm::vec3(0.0F);
        state.m_physicsSystem.setSettings(settings);
        const auto first = addBody(state, "first", {0.0F, 0.0F, 0.0F});
        addBody(state, "second", {0.4F, 0.0F, 0.0F});
        for (int i = 0; i < 256; ++i) {
            addBody(state, "remote", {20.0F + static_cast<float>(i) * 3.0F, 0.0F, 0.0F});
        }
        state.enterPlayMode();
        std::size_t exits = 0;
        state.m_eventBus.subscribe<ecs::CollisionExitEvent>([&](const auto&) { ++exits; });
        state.update(1.0 / 60.0); // Real integration/proxy parallelFor, joined on return.
        require(state.m_physicsSystem.activeCollisionCount() != 0, "Fixture generated no contacts");
        state.m_simulationRunning = false;
        state.deleteEntity(first);
        state.update(1.0 / 60.0);
        require(state.m_physicsSystem.activeCollisionCount() == 0 && exits == 1,
                "Runtime deletion did not remove contacts/emit one exit while paused");
        const auto replacement = addBody(state, "replacement", {-40.0F, 0.0F, 0.0F});
        require(replacement != first &&
                entt::to_entity(static_cast<entt::entity>(replacement - 1U)) ==
                entt::to_entity(static_cast<entt::entity>(first - 1U)),
                "Fixture did not reuse an entity slot with a distinct generation");
        state.deleteEntity(first); // A stale runtime request must not target the replacement.
        require(state.m_pendingRuntimeDeletes.empty(), "Stale handle queued a replacement deletion");
        state.m_simulationRunning = true;
        for (int i = 0; i < 6; ++i) { state.update(1.0 / 60.0); }
        require(state.m_world.isAlive(replacement) && exits == 1, "Stale deletion/contact affected recycled ID");
        state.deleteEntity(replacement);
        state.resetDemoScene();
        state.update(0.0);
        require(state.m_pendingRuntimeDeletes.empty(), "Reset retained a pending runtime deletion");
    }
    static void inspectorHistory(core::JobSystem& jobs) {
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        GameplayState state(stack);
        state.m_sceneInitialized = true;
        state.m_selectedEntity = addBody(state, "edited", {1.0F, 2.0F, 3.0F});
        const auto other = addBody(state, "other", {8.0F, 0.0F, 0.0F});
        state.m_world.addComponent<ecs::Hierarchy>(other, ecs::Hierarchy{state.m_selectedEntity});

        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800.0F, 600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        ImVec2 namePoint{}, massLeft{}, massRight{}, gravityPoint{};
        const ImVec2 away(600.0F, 500.0F);
        const auto frame = [&](const ImVec2 mouse, const bool down, const char* input = nullptr) {
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            if (input != nullptr) { io.AddInputCharactersUTF8(input); }
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(10.0F, 10.0F));
            ImGui::SetNextWindowSize(ImVec2(480.0F, 360.0F));
            ImGui::Begin("Inspector History Test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const auto entity = state.m_selectedEntity; // Undo/Redo remap handles.
            auto* tag = state.m_world.getComponent<ecs::Tag>(entity);
            auto* body = state.m_world.getComponent<ecs::Rigidbody>(entity);

            auto before = state.captureEntitySnapshot(entity);
            ImGui::SetNextItemWidth(300.0F);
            bool changed = ImGui::InputText("Name", &tag->value);
            state.trackEditedItem("Rename Entity", changed, before);
            const auto nameMin = ImGui::GetItemRectMin();
            const auto nameMax = ImGui::GetItemRectMax();
            namePoint = ImVec2(nameMin.x + 30.0F, (nameMin.y + nameMax.y) * 0.5F);

            before = state.captureEntitySnapshot(entity);
            ImGui::SetNextItemWidth(300.0F);
            changed = ImGui::SliderFloat("Mass", &body->mass, 0.1F, 10.0F);
            state.trackEditedItem("Edit Mass", changed, before);
            if (changed) { body->recalculateMassProperties(); body->wakeUp(); }
            const auto massMin = ImGui::GetItemRectMin();
            const auto massMax = ImGui::GetItemRectMax();
            massLeft = ImVec2(massMin.x + 105.0F, (massMin.y + massMax.y) * 0.5F);
            massRight = ImVec2(massMin.x + 210.0F, (massMin.y + massMax.y) * 0.5F);

            before = state.captureEntitySnapshot(entity);
            changed = ImGui::Checkbox("Use Gravity", &body->useGravity);
            state.trackEditedItem("Edit Gravity", changed, before);
            const auto gravityMin = ImGui::GetItemRectMin();
            const auto gravityMax = ImGui::GetItemRectMax();
            gravityPoint = ImVec2(gravityMin.x + 8.0F, (gravityMin.y + gravityMax.y) * 0.5F);
            ImGui::End();
            ImGui::Render();
        };

        for (int i = 0; i < 30; ++i) { frame(away, false); }
        require(state.m_undoStack.empty() && !state.m_pendingSceneEdit.has_value(),
                "Idle Inspector fields created history or an active edit");
        frame(massRight, true);
        require(state.m_pendingSceneEdit.has_value() && state.m_pendingSceneEdit->changed && state.m_undoStack.empty(),
                "Slider activation did not begin a single pending edit");
        const auto* dragSnapshot = state.m_pendingSceneEdit->before.entities.data();
        for (int i = 0; i < 5; ++i) { frame(massLeft, true); }
        require(state.m_pendingSceneEdit->before.entities.data() == dragSnapshot && state.m_undoStack.empty(),
                "Holding a slider replaced its initial snapshot or split its history");
        const float editedMass = state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->mass;
        require(editedMass != 1.0F, "Slider fixture did not change mass");
        frame(massLeft, false);
        require(state.m_undoStack.size() == 1U && !state.m_pendingSceneEdit.has_value(),
                "Slider release did not commit exactly one history entry");
        state.undoSceneEdit();
        require(state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->mass == 1.0F &&
                state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->inverseMass == 1.0F &&
                state.m_world.getComponent<ecs::Hierarchy>(tagged(state, "other"))->parent == state.m_selectedEntity,
                "Lazy Undo lost pre-widget values, mass properties or another entity's hierarchy");
        state.redoSceneEdit();
        require(state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->mass == editedMass &&
                state.m_world.getComponent<ecs::Transform>(tagged(state, "other"))->position.x == 8.0F,
                "Redo did not restore the final drag or changed another entity");

        frame(namePoint, true);
        frame(namePoint, false);
        require(state.m_pendingSceneEdit.has_value() && !state.m_pendingSceneEdit->changed,
                "Focusing text without editing did not preserve its initial value");
        frame(away, true);
        frame(away, false);
        require(state.m_undoStack.size() == 1U && !state.m_pendingSceneEdit.has_value(),
                "Focus without edits left history or a stale pending edit");

        frame(namePoint, true);
        frame(namePoint, false);
        frame(namePoint, false, "Z");
        const auto renamed = state.m_world.getComponent<ecs::Tag>(state.m_selectedEntity)->value;
        require(renamed != "edited", "InputText fixture did not rename the entity");
        frame(gravityPoint, true); // Switching fields commits the text edit first.
        frame(gravityPoint, false);
        require(state.m_undoStack.size() == 3U && !state.m_pendingSceneEdit.has_value() &&
                state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->useGravity,
                "Rename and checkbox were not independent history entries");
        state.undoSceneEdit();
        require(state.m_world.getComponent<ecs::Tag>(state.m_selectedEntity)->value == renamed &&
                !state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->useGravity,
                "Undoing a checkbox also undid the previous field");
        state.undoSceneEdit();
        require(state.m_world.getComponent<ecs::Tag>(state.m_selectedEntity)->value == "edited" &&
                state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->mass == editedMass,
                "Undoing text lost its pre-edit string or changed the previous drag");
        state.redoSceneEdit();
        state.redoSceneEdit();
        require(state.m_world.getComponent<ecs::Tag>(state.m_selectedEntity)->value == renamed &&
                state.m_world.getComponent<ecs::Rigidbody>(state.m_selectedEntity)->useGravity,
                "Redo lost the text or discrete field value");

        const auto historyCount = state.m_undoStack.size();
        state.enterPlayMode();
        frame(gravityPoint, true);
        frame(gravityPoint, false);
        require(state.m_undoStack.size() == historyCount && !state.m_pendingSceneEdit.has_value(),
                "Play Inspector created Edit history");
        state.stopPlayMode();
    }
};

} // namespace engine::game

int main() {
    ImGui::CreateContext();
    engine::platform::InputManager::reset();
    try {
        engine::core::JobSystem jobs(true, 4U);
        engine::game::GameplayStateTestAccess::sceneSelectionIsIndependentOfTracy(jobs);
        engine::game::GameplayStateTestAccess::runtimeDeleteAndRestore(jobs);
        engine::game::GameplayStateTestAccess::parallelPhysicsAndRecycledIds(jobs);
        engine::game::GameplayStateTestAccess::inspectorHistory(jobs);
    } catch (const std::exception& error) {
        std::cerr << "GameplayState test failure: " << error.what() << '\n';
        ImGui::DestroyContext();
        return EXIT_FAILURE;
    }
    ImGui::DestroyContext();
    std::cout << "Gameplay: scenes, runtime deletion, parallel physics, Stop and Inspector Undo/Redo passed\n";
    return EXIT_SUCCESS;
}
