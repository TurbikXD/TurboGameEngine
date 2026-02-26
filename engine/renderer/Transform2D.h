#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace engine::renderer {

struct Transform2D final {
    glm::vec2 position{0.0F, 0.0F};
    float rotationRadians{0.0F};
    glm::vec2 scale{1.0F, 1.0F};

    [[nodiscard]] glm::mat4 toMatrix() const {
        glm::mat4 matrix(1.0F);
        matrix = glm::translate(matrix, glm::vec3(position, 0.0F));
        matrix = glm::rotate(matrix, rotationRadians, glm::vec3(0.0F, 0.0F, 1.0F));
        matrix = glm::scale(matrix, glm::vec3(scale, 1.0F));
        return matrix;
    }
};

} // namespace engine::renderer
