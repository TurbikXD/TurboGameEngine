#include "engine/resources/loaders.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <nlohmann/json.hpp>

#include "engine/core/Profiling.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <glm/geometric.hpp>

namespace engine::resources {

namespace {

std::string utf8Path(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

bool isPpmWhitespace(const int character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == '\f' || character == '\v';
}

void skipPpmSeparators(std::istream& input) {
    for (;;) {
        const auto character = input.peek();
        if (isPpmWhitespace(character)) {
            input.get();
        } else if (character == '#') {
            input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        } else {
            return;
        }
    }
}

bool readPpmNumber(std::istream& input, std::uint32_t& value) {
    skipPpmSeparators(input);
    value = 0;
    bool hasDigits = false;
    for (;;) {
        const auto character = input.peek();
        if (character < '0' || character > '9') {
            return hasDigits && (character == std::char_traits<char>::eof() ||
                                 isPpmWhitespace(character) || character == '#');
        }
        const auto digit = static_cast<std::uint32_t>(input.get() - '0');
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        hasDigits = true;
    }
}

bool validTextureSize(const std::uint64_t width, const std::uint64_t height) {
    return width > 0 && height > 0 && width <= 8192 && height <= 8192 && width * height <= 16777216;
}

// stb_image supports binary P5/P6, but the engine's editable PPM assets are P3.
// The caller has already consumed the two-byte magic; inspect contents, not extension.
bool loadPlainPpm(std::istream& input, TextureData& output, std::string* errorMessage) {
    const auto fail = [errorMessage](const char* message) {
        if (errorMessage != nullptr) { *errorMessage = message; }
        return false;
    };
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t maximum = 0;
    if (!isPpmWhitespace(input.peek()) || !readPpmNumber(input, width) ||
        !readPpmNumber(input, height) || !readPpmNumber(input, maximum)) {
        return fail("Invalid P3 PPM header");
    }
    if (!validTextureSize(width, height) ||
        maximum == 0 || maximum > 65535U) {
        return fail("Invalid P3 PPM dimensions or maximum sample value");
    }
    const auto pixelCount = static_cast<std::uint64_t>(width) * height;
    // Match stb_image's signed-int allocation bound and reject overflow before resize.
    if (pixelCount > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) / 4U) {
        return fail("P3 PPM image is too large");
    }
    const auto rasterStart = input.tellg();
    input.seekg(0, std::ios::end);
    const auto rasterEnd = input.tellg();
    // Each RGB sample needs at least one digit and a separator (except the last).
    // A tiny malformed file must not cause a huge allocation from its header.
    if (rasterStart < 0 || rasterEnd < rasterStart ||
        static_cast<std::uint64_t>(rasterEnd - rasterStart) < pixelCount * 6U - 1U) {
        return fail("Truncated P3 PPM pixel data");
    }
    input.seekg(rasterStart);

    TextureData decoded;
    decoded.width = static_cast<std::int32_t>(width);
    decoded.height = static_cast<std::int32_t>(height);
    decoded.pixels.resize(static_cast<std::size_t>(pixelCount) * 4U);
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(pixelCount); ++pixel) {
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            std::uint32_t sample = 0;
            if (!readPpmNumber(input, sample) || sample > maximum) {
                return fail("Invalid or truncated P3 PPM RGB sample");
            }
            decoded.pixels[pixel * 4U + channel] =
                static_cast<std::uint8_t>((sample * 255U + maximum / 2U) / maximum);
        }
        decoded.pixels[pixel * 4U + 3U] = 255U;
    }
    skipPpmSeparators(input);
    if (input.peek() != std::char_traits<char>::eof()) {
        return fail("Unexpected data after P3 PPM pixels");
    }
    output = std::move(decoded);
    return true;
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

void appendAssimpMesh(const aiMesh& sourceMesh, MeshData& outMeshData, bool& hasAnyNormals) {
    if (sourceMesh.mVertices == nullptr || sourceMesh.mNumVertices == 0) {
        return;
    }

    const std::uint32_t baseVertex = static_cast<std::uint32_t>(outMeshData.vertices.size());
    hasAnyNormals = hasAnyNormals || sourceMesh.HasNormals();

    outMeshData.vertices.reserve(outMeshData.vertices.size() + sourceMesh.mNumVertices);
    outMeshData.indices.reserve(outMeshData.indices.size() + static_cast<std::size_t>(sourceMesh.mNumFaces) * 3U);

    for (unsigned int vertexIndex = 0; vertexIndex < sourceMesh.mNumVertices; ++vertexIndex) {
        const aiVector3D& position = sourceMesh.mVertices[vertexIndex];
        const aiVector3D normal =
            sourceMesh.HasNormals() ? sourceMesh.mNormals[vertexIndex] : aiVector3D(0.0F, 0.0F, 1.0F);
        const aiVector3D texCoord =
            sourceMesh.HasTextureCoords(0) ? sourceMesh.mTextureCoords[0][vertexIndex] : aiVector3D(0.0F, 0.0F, 0.0F);

        MeshVertex vertex{};
        vertex.position = glm::vec3(position.x, position.y, position.z);
        vertex.normal = glm::vec3(normal.x, normal.y, normal.z);
        vertex.uv = glm::vec2(texCoord.x, texCoord.y);
        outMeshData.vertices.push_back(vertex);
    }

    for (unsigned int faceIndex = 0; faceIndex < sourceMesh.mNumFaces; ++faceIndex) {
        const aiFace& face = sourceMesh.mFaces[faceIndex];
        if (face.mIndices == nullptr || face.mNumIndices != 3) {
            continue;
        }

        outMeshData.indices.push_back(baseVertex + face.mIndices[0]);
        outMeshData.indices.push_back(baseVertex + face.mIndices[1]);
        outMeshData.indices.push_back(baseVertex + face.mIndices[2]);
    }
}

} // namespace

bool loadMeshData(const std::string& path, MeshData& outMeshData, std::string* errorMessage) {
    ENGINE_PROFILE_ZONE("Parse Mesh Data");
    Assimp::Importer importer;
    constexpr unsigned int kImportFlags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                          aiProcess_PreTransformVertices | aiProcess_GenSmoothNormals |
                                          aiProcess_FlipUVs | aiProcess_SortByPType | aiProcess_ImproveCacheLocality |
                                          aiProcess_FindInvalidData;

    const aiScene* scene = importer.ReadFile(path, kImportFlags);
    if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 || scene->mRootNode == nullptr) {
        if (errorMessage != nullptr) {
            const char* importerError = importer.GetErrorString();
            *errorMessage = (importerError != nullptr && importerError[0] != '\0')
                                ? importerError
                                : "Assimp failed to parse mesh file";
        }
        return false;
    }

    outMeshData.vertices.clear();
    outMeshData.indices.clear();

    std::size_t totalVertexCount = 0;
    std::size_t totalIndexCount = 0;
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (mesh == nullptr) {
            continue;
        }

        totalVertexCount += mesh->mNumVertices;
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
            if (mesh->mFaces[faceIndex].mNumIndices == 3) {
                totalIndexCount += 3U;
            }
        }
    }

    outMeshData.vertices.reserve(totalVertexCount);
    outMeshData.indices.reserve(totalIndexCount);

    bool hasAnyNormals = false;
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (mesh == nullptr) {
            continue;
        }

        appendAssimpMesh(*mesh, outMeshData, hasAnyNormals);
    }

    if (outMeshData.vertices.empty() || outMeshData.indices.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Mesh file has no triangle geometry";
        }
        return false;
    }

    if (!hasAnyNormals) {
        buildMissingNormals(outMeshData);
    }
    return true;
}

bool inspectTextureRgba8(const std::string& path, std::int32_t& width, std::int32_t& height, std::string* errorMessage) {
    width = height = 0;
    if (errorMessage) errorMessage->clear();
    const auto fail = [&](const char* message) { if (errorMessage) *errorMessage = message; return false; };
    {
        std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
        char magic[2]{};
        if (input.read(magic, sizeof(magic)) && magic[0] == 'P' && magic[1] == '3') {
            std::uint32_t w = 0, h = 0, maximum = 0;
            if (!isPpmWhitespace(input.peek()) || !readPpmNumber(input, w) || !readPpmNumber(input, h) ||
                !readPpmNumber(input, maximum) || maximum == 0 || maximum > 65535 || !validTextureSize(w, h))
                return fail("Invalid or oversized P3 PPM header (maximum 8192 per side, 16 megapixels)");
            width = static_cast<std::int32_t>(w); height = static_cast<std::int32_t>(h);
            return true;
        }
    }
    FILE* rawFile = nullptr;
#if defined(_WIN32)
    const auto nativePath = std::filesystem::u8path(path);
    (void)_wfopen_s(&rawFile, nativePath.c_str(), L"rb");
#else
    rawFile = std::fopen(path.c_str(), "rb");
#endif
    const std::unique_ptr<FILE, decltype(&std::fclose)> file(rawFile, &std::fclose);
    int w = 0, h = 0, channels = 0;
    if (!file || !stbi_info_from_file(file.get(), &w, &h, &channels)) return fail("Cannot inspect image header");
    if (!validTextureSize(static_cast<std::uint64_t>(w), static_cast<std::uint64_t>(h)))
        return fail("Image exceeds maximum 8192 per side or 16 megapixels");
    width = w; height = h;
    return true;
}

bool loadTextureDataRgba8(const std::string& path, TextureData& outTextureData, std::string* errorMessage) {
    ENGINE_PROFILE_ZONE("Decode Texture");
    if (errorMessage != nullptr) { errorMessage->clear(); }
    std::int32_t headerWidth = 0, headerHeight = 0;
    if (!inspectTextureRgba8(path, headerWidth, headerHeight, errorMessage)) return false;
    // Decode the exact bytes whose header was validated. AI/live edits can
    // replace or grow a file between an earlier metadata probe and the job.
    // A bounded immutable snapshot closes that race before stb allocates pixels.
    constexpr std::size_t encodedLimit = 128 * 1024 * 1024;
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) { if (errorMessage) *errorMessage = "Cannot open texture file"; return false; }
    input.seekg(0, std::ios::end);
    const auto encodedSize = input.tellg();
    if (encodedSize < 0 || static_cast<std::uint64_t>(encodedSize) > encodedLimit) {
        if (errorMessage) *errorMessage = "Invalid encoded image size or exceeds 128 MiB";
        return false;
    }
    input.seekg(0);
    std::string encoded;
    encoded.reserve(static_cast<std::size_t>(encodedSize));
    char chunk[16384];
    while (input) {
        input.read(chunk, sizeof(chunk));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > encodedLimit - encoded.size()) {
            if (errorMessage) *errorMessage = "Encoded image exceeds 128 MiB";
            return false;
        }
        encoded.append(chunk, count);
    }
    if (!input.eof()) { if (errorMessage) *errorMessage = "Cannot read texture file"; return false; }
    if (encoded.size() >= 2 && encoded[0] == 'P' && encoded[1] == '3') {
        std::istringstream snapshot(std::move(encoded));
        snapshot.ignore(2);
        return loadPlainPpm(snapshot, outTextureData, errorMessage);
    }
    int width = 0;
    int height = 0;
    int channels = 0;

    const auto* bytes = reinterpret_cast<const stbi_uc*>(encoded.data());
    const auto byteCount = static_cast<int>(encoded.size());
    if (!stbi_info_from_memory(bytes, byteCount, &width, &height, &channels) ||
        !validTextureSize(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height))) {
        if (errorMessage) *errorMessage = "Invalid or oversized immutable image header";
        return false;
    }
    stbi_uc* pixels = stbi_load_from_memory(bytes, byteCount, &width, &height, &channels, 4);
    if (pixels == nullptr) {
        if (errorMessage != nullptr) {
            const char* reason = stbi_failure_reason();
            *errorMessage = reason != nullptr ? reason : "stb_image load failed";
        }
        return false;
    }
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> ownedPixels(pixels, &stbi_image_free);

    if (!validTextureSize(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height))) {
        if (errorMessage) *errorMessage = "Image dimensions changed or exceed decode limits";
        return false;
    }

    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    outTextureData.width = width;
    outTextureData.height = height;
    outTextureData.channels = 4;
    outTextureData.pixels.assign(pixels, pixels + pixelCount);

    return true;
}

bool loadShaderProgramSource(
    const std::string& descriptor,
    ShaderProgramSource& outSource,
    std::string* errorMessage,
    const std::filesystem::path& assetRoot) {
    ENGINE_PROFILE_ZONE("Parse Shader Manifest");
    outSource = ShaderProgramSource{};
    outSource.descriptorPath = descriptor;
    const auto resolvePaths = [&] {
        const auto parent = outSource.descriptorIsFile
            ? std::filesystem::absolute(std::filesystem::u8path(descriptor)).parent_path()
            : (assetRoot.empty() ? std::filesystem::current_path() : assetRoot);
        auto root = assetRoot;
        if (root.empty()) {
            for (auto ancestor = parent; !ancestor.empty(); ancestor = ancestor.parent_path()) {
                if (ancestor.filename() == "assets") { root = ancestor; break; }
                if (ancestor == ancestor.parent_path()) { break; }
            }
            if (root.empty()) { root = parent; }
        }
        const auto resolve = [&](const std::string& input) {
            auto path = std::filesystem::u8path(input);
            if (path.is_absolute()) { return utf8Path(std::filesystem::weakly_canonical(path)); }
            if (*path.begin() == "assets") { path = root / path.lexically_relative("assets"); }
            else { path = parent / path; }
            return utf8Path(std::filesystem::weakly_canonical(path));
        };
        outSource.vertexPath = resolve(outSource.vertexPath);
        outSource.fragmentPath = resolve(outSource.fragmentPath);
        if (outSource.descriptorIsFile) {
            outSource.descriptorPath = utf8Path(std::filesystem::weakly_canonical(std::filesystem::absolute(std::filesystem::u8path(descriptor))));
        }
    };

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
        try { resolvePaths(); }
        catch (const std::exception& error) {
            if (errorMessage != nullptr) { *errorMessage = error.what(); }
            return false;
        }
        return true;
    }

    std::ifstream in(std::filesystem::u8path(descriptor));
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
        resolvePaths();
        return true;
    } catch (const std::exception& ex) {
        if (errorMessage != nullptr) {
            *errorMessage = ex.what();
        }
        return false;
    }
}

} // namespace engine::resources
