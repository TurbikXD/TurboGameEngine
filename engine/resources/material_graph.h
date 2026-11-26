#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace engine::resources {

inline constexpr std::size_t maxMaterialGraphNodes = 64;
inline constexpr std::size_t maxMaterialGraphTextures = 8;

enum class MaterialGraphNodeKind { UV, Texture, Scalar, Color, Vector2, Add, Multiply, Lerp, Channel };

struct MaterialGraphPinInfo final {
    const char* key;
    const char* label;
};

struct MaterialGraphNodeInfo final {
    MaterialGraphNodeKind kind;
    const char* type;
    const char* label;
    std::span<const MaterialGraphPinInfo> inputs;
};

struct MaterialGraphProgram final {
    std::string pixelSource;
    std::vector<std::string> textures;

    bool operator==(const MaterialGraphProgram&) const = default;
};

std::span<const MaterialGraphNodeInfo> materialGraphNodeTypes();
std::span<const MaterialGraphPinInfo> materialGraphOutputs();
MaterialGraphProgram compileMaterialGraph(const nlohmann::json& graph);

}
