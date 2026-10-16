#pragma once

#include <filesystem>
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

bool loadMeshData(const std::string& path, MeshData& outMeshData, std::string* errorMessage = nullptr);
bool loadTextureDataRgba8(const std::string& path, TextureData& outTextureData, std::string* errorMessage = nullptr);
// Header-only inspection with the same decode-size bounds as the loader.
bool inspectTextureRgba8(const std::string& path, std::int32_t& width, std::int32_t& height,
                         std::string* errorMessage = nullptr);
// Resource strings are UTF-8. Manifest sources are relative to the manifest;
// an assets/ prefix is relative to assetRoot (otherwise inferred from its path).
bool loadShaderProgramSource(
    const std::string& descriptor,
    ShaderProgramSource& outSource,
    std::string* errorMessage = nullptr,
    const std::filesystem::path& assetRoot = {});

} // namespace engine::resources
