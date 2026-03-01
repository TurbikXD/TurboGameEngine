#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/ecs/entity.h"

namespace engine::resources {
class Mesh;
class ShaderProgram;
class Texture;
} // namespace engine::resources

namespace engine::ecs {

struct Transform final {
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::vec3 rotationEulerRadians{0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F, 1.0F, 1.0F};

    [[nodiscard]] glm::mat4 toMatrix() const {
        glm::mat4 matrix(1.0F);
        matrix = glm::translate(matrix, position);
        matrix = glm::rotate(matrix, rotationEulerRadians.x, glm::vec3(1.0F, 0.0F, 0.0F));
        matrix = glm::rotate(matrix, rotationEulerRadians.y, glm::vec3(0.0F, 1.0F, 0.0F));
        matrix = glm::rotate(matrix, rotationEulerRadians.z, glm::vec3(0.0F, 0.0F, 1.0F));
        matrix = glm::scale(matrix, scale);
        return matrix;
    }
};

struct Tag final {
    std::string value;
};

struct Hierarchy final {
    EntityId parent{kInvalidEntity};
};

enum class PrimitiveType : std::uint8_t {
    Triangle = 0
};

struct MeshRenderer final {
    PrimitiveType primitiveType{PrimitiveType::Triangle};
    glm::vec4 tint{1.0F, 1.0F, 1.0F, 1.0F};
    std::string meshId;
    std::string textureId;
    std::string shaderId;
    std::shared_ptr<resources::Mesh> mesh;
    std::shared_ptr<resources::Texture> texture;
    std::shared_ptr<resources::ShaderProgram> shader;
    bool visible{true};

    [[nodiscard]] bool hasResourceIds() const {
        return !meshId.empty() && !textureId.empty() && !shaderId.empty();
    }

    [[nodiscard]] bool hasGpuResources() const {
        return mesh != nullptr && texture != nullptr && shader != nullptr;
    }
};

} // namespace engine::ecs
