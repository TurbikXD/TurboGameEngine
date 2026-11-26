#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <glm/vec2.hpp>

#include "engine/ecs/entity.h"

namespace engine::ecs { struct MeshRenderer; }
namespace engine::resources { struct Material; class MaterialLibrary; }

namespace engine::editor {
struct MaterialGraphDock final {
    glm::vec2 position{};
    glm::vec2 size{};
    bool& docked;
};

struct MaterialGraphCanvasState final {
    glm::vec2 pan{0.0F};
    float zoom{1.0F};

    bool operator==(const MaterialGraphCanvasState&) const = default;
};

struct MaterialGraphSession final {
    std::string materialId;
    std::optional<MaterialGraphCanvasState> canvas;

    bool operator==(const MaterialGraphSession&) const = default;
};

void renderMaterialInspector(ecs::MeshRenderer& mesh, resources::MaterialLibrary& library);
bool materialGraphEditorOpen(const resources::MaterialLibrary& library);
std::optional<MaterialGraphSession> materialGraphSession(const resources::MaterialLibrary& library);
bool restoreMaterialGraphSession(resources::MaterialLibrary& library, const MaterialGraphSession& session);
void focusRestoredMaterialGraph();
void closeMaterialGraphEditor();
void loadMaterialGraphDrafts(resources::MaterialLibrary& library, const std::filesystem::path& path);
void flushMaterialGraphDrafts(bool force = false);
void closeMaterialGraphDrafts();
std::shared_ptr<resources::Material> materialGraphPreview(resources::MaterialLibrary& library,
    ecs::EntityId selected, const ecs::MeshRenderer* mesh);
std::shared_ptr<resources::Material> renderMaterialGraphEditor(resources::MaterialLibrary& library,
    ecs::EntityId selected = ecs::kInvalidEntity, const ecs::MeshRenderer* mesh = nullptr,
    const MaterialGraphDock* dock = nullptr);
bool materialGraphEditorFocused();
}
