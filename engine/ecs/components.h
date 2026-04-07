#pragma once

#include <cmath>
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

struct Camera final {
    float verticalFovRadians{glm::radians(60.0F)};
    float nearPlane{0.1F};
    float farPlane{200.0F};
    bool active{true};
};

struct Rigidbody final {
    glm::vec3 velocity{0.0F, 0.0F, 0.0F};
    glm::vec3 acceleration{0.0F, 0.0F, 0.0F};
    glm::vec3 accumulatedForce{0.0F, 0.0F, 0.0F};
    glm::vec3 angularVelocity{0.0F, 0.0F, 0.0F};
    glm::vec3 accumulatedTorque{0.0F, 0.0F, 0.0F};
    float mass{1.0F};
    float inverseMass{1.0F};
    float linearDamping{0.08F};
    float angularDamping{0.6F};
    float restitution{0.06F};
    float friction{0.7F};
    float sleepTimer{0.0F};
    bool useGravity{true};
    bool isStatic{false};
    bool isKinematic{false};
    bool canSleep{true};
    bool isSleeping{false};

    void applyForce(const glm::vec3& force) {
        accumulatedForce += force;
        wakeUp();
    }

    void applyTorque(const glm::vec3& torque) {
        accumulatedTorque += torque;
        wakeUp();
    }

    void wakeUp() {
        isSleeping = false;
        sleepTimer = 0.0F;
    }

    void recalculateMassProperties() {
        if (isStatic || isKinematic || !std::isfinite(mass) || mass <= 0.0F) {
            inverseMass = 0.0F;
            return;
        }
        inverseMass = 1.0F / mass;
    }

    [[nodiscard]] bool isDynamic() const {
        return !isStatic && !isKinematic && std::isfinite(inverseMass) && inverseMass > 0.0F;
    }
};

enum class ColliderType : std::uint8_t {
    Aabb = 0,
    Sphere
};

struct AabbCollider final {
    glm::vec3 halfExtents{0.5F, 0.5F, 0.5F};
};

struct SphereCollider final {
    float radius{0.5F};
};

struct Collider final {
    ColliderType type{ColliderType::Aabb};
    glm::vec3 offset{0.0F, 0.0F, 0.0F};
    AabbCollider aabb{};
    SphereCollider sphere{};
    bool enabled{true};
};

enum class PrimitiveType : std::uint8_t {
    Triangle = 0
};

struct MeshRenderer final {
    PrimitiveType primitiveType{PrimitiveType::Triangle};
    glm::vec4 tint{1.0F, 1.0F, 1.0F, 1.0F};
    glm::vec2 uvScale{1.0F, 1.0F};
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
