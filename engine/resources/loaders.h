#pragma once

#include <string>

#include "engine/resources/mesh.h"
#include "engine/resources/texture.h"

namespace engine::resources {

struct ShaderProgramSource final {
    std::string descriptorPath;
    std::string vertexPath;
    std::string fragmentPath;
    bool descriptorIsFile{false};
};

bool loadObjMeshData(const std::string& path, MeshData& outMeshData, std::string* errorMessage = nullptr);
bool loadTextureDataRgba8(const std::string& path, TextureData& outTextureData, std::string* errorMessage = nullptr);
bool loadShaderProgramSource(
    const std::string& descriptor,
    ShaderProgramSource& outSource,
    std::string* errorMessage = nullptr);

} // namespace engine::resources
