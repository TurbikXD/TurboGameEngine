#pragma once

#include <glm/glm.hpp>

#include "engine/ecs/entity.h"

namespace engine::ecs {

class World;

glm::mat4 computeWorldMatrix(const World& world, EntityId entity);
glm::vec3 transformPoint(const glm::mat4& matrix, const glm::vec3& point);
glm::vec3 extractWorldScale(const glm::mat4& matrix);
glm::vec3 forwardFromEuler(const glm::vec3& rotationEulerRadians);
glm::vec3 rightFromEuler(const glm::vec3& rotationEulerRadians);
glm::vec3 upFromEuler(const glm::vec3& rotationEulerRadians);

} // namespace engine::ecs
