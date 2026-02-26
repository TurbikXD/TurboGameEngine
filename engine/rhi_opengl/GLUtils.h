#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <glm/mat4x4.hpp>

#include <glad/gl.h>

#include "engine/rhi/Types.h"

namespace engine::rhi::gl {

std::string readTextFile(const std::string& path);
std::uint32_t compileShaderFromSource(
    ShaderStage stage,
    const std::string& source,
    const std::string& debugName);
std::uint32_t linkProgram(std::uint32_t vertexShaderId, std::uint32_t fragmentShaderId);
void setMat4Uniform(std::uint32_t programId, std::string_view uniformName, const glm::mat4& matrix);
void checkGlError(std::string_view where);

} // namespace engine::rhi::gl
