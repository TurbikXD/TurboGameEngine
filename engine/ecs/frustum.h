#pragma once
#include <glm/glm.hpp>

namespace engine::ecs {
// Conservative local AABB against the six RH_ZO clip planes (D3D/Vulkan).
inline bool intersectsFrustum(const glm::mat4& mvp, const glm::vec3& minimum, const glm::vec3& maximum) {
    const glm::vec3 center = (minimum + maximum) * 0.5F, extent = (maximum - minimum) * 0.5F;
    const glm::vec4 x(mvp[0][0], mvp[1][0], mvp[2][0], mvp[3][0]);
    const glm::vec4 y(mvp[0][1], mvp[1][1], mvp[2][1], mvp[3][1]);
    const glm::vec4 z(mvp[0][2], mvp[1][2], mvp[2][2], mvp[3][2]);
    const glm::vec4 w(mvp[0][3], mvp[1][3], mvp[2][3], mvp[3][3]);
    for (const auto& plane : {w + x, w - x, w + y, w - y, z, w - z}) {
        if (glm::dot(glm::vec3(plane), center) + plane.w + glm::dot(glm::abs(glm::vec3(plane)), extent) < 0.0F) return false;
    }
    return true;
}
}
