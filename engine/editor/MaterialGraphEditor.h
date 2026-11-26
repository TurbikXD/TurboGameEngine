#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <glm/vec2.hpp>
#include <nlohmann/json_fwd.hpp>

namespace engine::resources { struct MaterialGraphNodeInfo; }

namespace engine::editor {

inline constexpr float minMaterialGraphZoom = 0.35F;
inline constexpr float maxMaterialGraphZoom = 1.8F;

struct MaterialGraphView final {
    glm::vec2 pan{0.0F};
    glm::vec2 spawnPosition{60.0F, 60.0F};
    float zoom{1.0F};
    std::uint32_t selected{};
    std::uint32_t linkSource{};
    std::uint32_t dragNode{};
    bool dragOutput{};
    bool linkPressed{};
    bool fit{true};

    [[nodiscard]] bool draggingLayout() const { return dragNode != 0 || dragOutput; }
};

enum class MaterialGraphChange { None, Layout, Appearance };

const resources::MaterialGraphNodeInfo* materialGraphNodeType(const nlohmann::json& node);
nlohmann::json* findMaterialGraphNode(nlohmann::json& graph, std::uint32_t id);
[[nodiscard]] std::uint64_t nextMaterialGraphNodeId(const nlohmann::json& graph);
bool addMaterialGraphNode(nlohmann::json& graph, MaterialGraphView& view,
                          const resources::MaterialGraphNodeInfo& type, std::uint64_t minimumId = 1);
bool removeMaterialGraphNode(nlohmann::json& graph, MaterialGraphView& view);
MaterialGraphChange renderMaterialGraphCanvas(nlohmann::json& graph, MaterialGraphView& view,
    bool editable, std::string& linkError, const std::function<void(MaterialGraphChange)>& beforeEdit);

}
