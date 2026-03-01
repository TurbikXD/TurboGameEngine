#include "engine/resources/loaders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <glm/geometric.hpp>

namespace engine::resources {

namespace {

struct VertexKey final {
    int vertexIndex{-1};
    int normalIndex{-1};
    int texcoordIndex{-1};

    [[nodiscard]] bool operator==(const VertexKey& other) const {
        return vertexIndex == other.vertexIndex && normalIndex == other.normalIndex &&
               texcoordIndex == other.texcoordIndex;
    }
};

struct VertexKeyHash final {
    std::size_t operator()(const VertexKey& key) const noexcept {
        std::size_t seed = std::hash<int>{}(key.vertexIndex);
        seed ^= std::hash<int>{}(key.normalIndex) + 0x9e3779b9 + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<int>{}(key.texcoordIndex) + 0x9e3779b9 + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

glm::vec3 readVec3(const std::vector<float>& values, int index, glm::vec3 fallback) {
    const std::size_t base = static_cast<std::size_t>(index) * 3U;
    if (index < 0 || base + 2U >= values.size()) {
        return fallback;
    }
    return glm::vec3(values[base], values[base + 1U], values[base + 2U]);
}

glm::vec2 readVec2(const std::vector<float>& values, int index, glm::vec2 fallback) {
    const std::size_t base = static_cast<std::size_t>(index) * 2U;
    if (index < 0 || base + 1U >= values.size()) {
        return fallback;
    }
    return glm::vec2(values[base], values[base + 1U]);
}

void buildMissingNormals(MeshData& meshData) {
    if (meshData.indices.size() < 3U) {
        return;
    }

    for (auto& vertex : meshData.vertices) {
        vertex.normal = glm::vec3(0.0F);
    }

    for (std::size_t i = 0; i + 2U < meshData.indices.size(); i += 3U) {
        const std::uint32_t i0 = meshData.indices[i];
        const std::uint32_t i1 = meshData.indices[i + 1U];
        const std::uint32_t i2 = meshData.indices[i + 2U];

        const glm::vec3 e1 = meshData.vertices[i1].position - meshData.vertices[i0].position;
        const glm::vec3 e2 = meshData.vertices[i2].position - meshData.vertices[i0].position;
        const glm::vec3 faceNormal = glm::normalize(glm::cross(e1, e2));

        if (!std::isfinite(faceNormal.x) || !std::isfinite(faceNormal.y) || !std::isfinite(faceNormal.z)) {
            continue;
        }

        meshData.vertices[i0].normal += faceNormal;
        meshData.vertices[i1].normal += faceNormal;
        meshData.vertices[i2].normal += faceNormal;
    }

    for (auto& vertex : meshData.vertices) {
        if (glm::dot(vertex.normal, vertex.normal) < 1e-12F) {
            vertex.normal = glm::vec3(0.0F, 0.0F, 1.0F);
            continue;
        }
        vertex.normal = glm::normalize(vertex.normal);
    }
}

} // namespace

bool loadObjMeshData(const std::string& path, MeshData& outMeshData, std::string* errorMessage) {
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warnings;
    std::string errors;

    const auto parentPath = std::filesystem::path(path).parent_path();
    const std::string materialBaseDir = parentPath.empty() ? std::string() : (parentPath.string() + "/");

    const bool ok = tinyobj::LoadObj(
        &attrib,
        &shapes,
        &materials,
        &warnings,
        &errors,
        path.c_str(),
        materialBaseDir.empty() ? nullptr : materialBaseDir.c_str(),
        true);
    (void)materials;

    if (!warnings.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = warnings;
        }
    }

    if (!ok) {
        if (errorMessage != nullptr) {
            *errorMessage = errors.empty() ? "tinyobjloader failed to parse file" : errors;
        }
        return false;
    }

    outMeshData.vertices.clear();
    outMeshData.indices.clear();

    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> vertexCache;
    vertexCache.reserve(2048);

    bool hasAnyNormals = false;
    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            VertexKey key{};
            key.vertexIndex = index.vertex_index;
            key.normalIndex = index.normal_index;
            key.texcoordIndex = index.texcoord_index;

            const auto cacheIt = vertexCache.find(key);
            if (cacheIt != vertexCache.end()) {
                outMeshData.indices.push_back(cacheIt->second);
                continue;
            }

            MeshVertex vertex{};
            vertex.position = readVec3(attrib.vertices, key.vertexIndex, glm::vec3(0.0F));
            vertex.normal = readVec3(attrib.normals, key.normalIndex, glm::vec3(0.0F, 0.0F, 1.0F));
            vertex.uv = readVec2(attrib.texcoords, key.texcoordIndex, glm::vec2(0.0F));
            vertex.uv.y = 1.0F - vertex.uv.y;

            if (key.normalIndex >= 0) {
                hasAnyNormals = true;
            }

            const std::uint32_t newIndex = static_cast<std::uint32_t>(outMeshData.vertices.size());
            outMeshData.vertices.push_back(vertex);
            outMeshData.indices.push_back(newIndex);
            vertexCache.emplace(key, newIndex);
        }
    }

    if (outMeshData.vertices.empty() || outMeshData.indices.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "OBJ mesh has no geometry";
        }
        return false;
    }

    if (!hasAnyNormals) {
        buildMissingNormals(outMeshData);
    }
    return true;
}

bool loadTextureDataRgba8(const std::string& path, TextureData& outTextureData, std::string* errorMessage) {
    int width = 0;
    int height = 0;
    int channels = 0;

    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        if (errorMessage != nullptr) {
            const char* reason = stbi_failure_reason();
            *errorMessage = reason != nullptr ? reason : "stb_image load failed";
        }
        return false;
    }

    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    outTextureData.width = width;
    outTextureData.height = height;
    outTextureData.channels = 4;
    outTextureData.pixels.assign(pixels, pixels + pixelCount);

    stbi_image_free(pixels);
    return true;
}

bool loadShaderProgramSource(
    const std::string& descriptor,
    ShaderProgramSource& outSource,
    std::string* errorMessage) {
    outSource = ShaderProgramSource{};
    outSource.descriptorPath = descriptor;

    const std::size_t separatorPos = descriptor.find('|');
    if (separatorPos != std::string::npos) {
        outSource.vertexPath = descriptor.substr(0, separatorPos);
        outSource.fragmentPath = descriptor.substr(separatorPos + 1U);
        outSource.descriptorIsFile = false;
        if (outSource.vertexPath.empty() || outSource.fragmentPath.empty()) {
            if (errorMessage != nullptr) {
                *errorMessage = "Shader descriptor must provide both vertex and fragment paths";
            }
            return false;
        }
        return true;
    }

    std::ifstream in(descriptor);
    if (!in.is_open()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to open shader descriptor file";
        }
        return false;
    }

    try {
        nlohmann::json manifest;
        in >> manifest;
        if (!manifest.contains("vertex") || !manifest.contains("fragment")) {
            if (errorMessage != nullptr) {
                *errorMessage = "Shader descriptor must contain 'vertex' and 'fragment' keys";
            }
            return false;
        }

        outSource.vertexPath = manifest.at("vertex").get<std::string>();
        outSource.fragmentPath = manifest.at("fragment").get<std::string>();
        outSource.descriptorIsFile = true;
        if (outSource.vertexPath.empty() || outSource.fragmentPath.empty()) {
            if (errorMessage != nullptr) {
                *errorMessage = "Shader descriptor has empty vertex/fragment path";
            }
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage != nullptr) {
            *errorMessage = ex.what();
        }
        return false;
    }
}

} // namespace engine::resources
