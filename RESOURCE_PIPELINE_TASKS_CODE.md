# Resource Pipeline: релевантный код по задачам 1-8

Ниже собран код из проекта, который закрывает задачи:

1. Подключение библиотек загрузки (`tinyobjloader`, `stb_image`, `nlohmann/json`).
2. `ResourceManager` с шаблонными загрузчиками и кэшированием.
3. Загрузчики `Mesh`, `Texture`, `Shader`.
4. Расширение `RenderAdapter` для создания GPU-ресурсов.
5. `MeshRenderer` с ID/ссылками на ресурсы.
6. `RenderSystem`, который рендерит загруженные меши и текстуры.
7. Демонстрация нескольких 3D-моделей в сцене.
8. Бонус: hot-reload шейдеров и конфигов.

---

## 1) Подключение библиотек (CMake)

### `CMakeLists.txt` (фрагменты)

```cmake
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Disable remote updates for already populated dependencies" FORCE)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG v2.0.0rc13
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(tinyobjloader)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    UPDATE_DISCONNECTED ON
)
FetchContent_MakeAvailable(stb)
```

```cmake
add_library(engine_renderer STATIC
    engine/renderer/Renderer.cpp
    engine/resources/resource_manager.cpp
    engine/resources/loaders.cpp
    engine/ecs/render_system.cpp
)
target_include_directories(engine_renderer PUBLIC "${ENGINE_ROOT_INCLUDE_DIR}")
target_include_directories(engine_renderer PRIVATE "${tinyobjloader_SOURCE_DIR}" "${stb_SOURCE_DIR}")
target_link_libraries(engine_renderer PUBLIC engine_core engine_rhi glm::glm nlohmann_json::nlohmann_json)
```

Примечание: в текущем проекте для моделей используется `tinyobjloader` (не Assimp).

---

## 2) Шаблонный `ResourceManager` с кэшированием

### `engine/resources/resource_manager.h`

```cpp
#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

#include "engine/core/Log.h"

namespace engine::resources {

class ResourceManager final {
public:
    ResourceManager() = default;
    ~ResourceManager() = default;

    template <class T>
    using LoaderFn = std::function<std::shared_ptr<T>(const std::string&)>;

    template <class T>
    void registerLoader(LoaderFn<T> loader);

    template <class T>
    void registerFallback(std::shared_ptr<T> fallback);

    template <class T>
    std::shared_ptr<T> load(const std::string& path);

    void clear();

private:
    using UntypedLoaderFn = std::function<std::shared_ptr<void>(const std::string&)>;

    template <class T>
    std::shared_ptr<T> fallbackForType() const;

    mutable std::mutex m_mutex;
    std::unordered_map<std::type_index, std::unordered_map<std::string, std::weak_ptr<void>>> m_cacheByType;
    std::unordered_map<std::type_index, UntypedLoaderFn> m_loaderByType;
    std::unordered_map<std::type_index, std::shared_ptr<void>> m_fallbackByType;
};

template <class T>
void ResourceManager::registerLoader(LoaderFn<T> loader) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loaderByType[typeid(T)] = [wrappedLoader = std::move(loader)](const std::string& path) {
        return std::static_pointer_cast<void>(wrappedLoader(path));
    };
}

template <class T>
void ResourceManager::registerFallback(std::shared_ptr<T> fallback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fallbackByType[typeid(T)] = std::static_pointer_cast<void>(std::move(fallback));
}

template <class T>
std::shared_ptr<T> ResourceManager::load(const std::string& path) {
    UntypedLoaderFn loader;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& cacheForType = m_cacheByType[typeid(T)];
        const auto cacheIt = cacheForType.find(path);
        if (cacheIt != cacheForType.end()) {
            if (const auto cached = std::static_pointer_cast<T>(cacheIt->second.lock())) {
                ENGINE_LOG_INFO("ResourceManager cache hit [{}] '{}'", typeid(T).name(), path);
                return cached;
            }
            cacheForType.erase(cacheIt);
        }

        const auto loaderIt = m_loaderByType.find(typeid(T));
        if (loaderIt != m_loaderByType.end()) {
            loader = loaderIt->second;
        }
    }

    ENGINE_LOG_INFO("ResourceManager cache miss [{}] '{}'", typeid(T).name(), path);
    if (!loader) {
        ENGINE_LOG_ERROR("ResourceManager has no loader for type [{}]", typeid(T).name());
        return fallbackForType<T>();
    }

    std::shared_ptr<T> loaded;
    try {
        loaded = std::static_pointer_cast<T>(loader(path));
    } catch (const std::exception& ex) {
        ENGINE_LOG_ERROR("ResourceManager loader exception [{}] '{}': {}", typeid(T).name(), path, ex.what());
    } catch (...) {
        ENGINE_LOG_ERROR("ResourceManager loader unknown exception [{}] '{}'", typeid(T).name(), path);
    }

    if (!loaded) {
        ENGINE_LOG_WARN("ResourceManager load failed [{}] '{}', using fallback", typeid(T).name(), path);
        return fallbackForType<T>();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cacheByType[typeid(T)][path] = loaded;
    }
    return loaded;
}

template <class T>
std::shared_ptr<T> ResourceManager::fallbackForType() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_fallbackByType.find(typeid(T));
    if (it == m_fallbackByType.end()) {
        return nullptr;
    }
    return std::static_pointer_cast<T>(it->second);
}

} // namespace engine::resources
```

### `engine/resources/resource_manager.cpp`

```cpp
#include "engine/resources/resource_manager.h"

namespace engine::resources {

void ResourceManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheByType.clear();
    m_loaderByType.clear();
    m_fallbackByType.clear();
}

} // namespace engine::resources
```

---

## 3) Типы ресурсов и загрузчики

### `engine/resources/mesh.h`

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rhi/Resources.h"

namespace engine::resources {

struct MeshVertex final {
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::vec3 normal{0.0F, 0.0F, 1.0F};
    glm::vec2 uv{0.0F, 0.0F};
};

struct MeshData final {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

class Mesh final {
public:
    std::string path;
    MeshData data;
    std::unique_ptr<rhi::IBuffer> vertexBuffer;
    std::unique_ptr<rhi::IBuffer> indexBuffer;
    std::uint32_t vertexCount{0};
    std::uint32_t indexCount{0};

    [[nodiscard]] bool isReady() const {
        if (!vertexBuffer) {
            return false;
        }
        if (!data.indices.empty() && !indexBuffer) {
            return false;
        }
        return true;
    }
};

} // namespace engine::resources
```

### `engine/resources/texture.h`

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/rhi/Resources.h"

namespace engine::resources {

struct TextureData final {
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t channels{4};
    std::vector<std::uint8_t> pixels;
};

class Texture final {
public:
    std::string path;
    TextureData data;
    std::unique_ptr<rhi::IImage> image;

    [[nodiscard]] bool isReady() const {
        return image != nullptr;
    }
};

} // namespace engine::resources
```

### `engine/resources/shader_program.h`

```cpp
#pragma once

#include <memory>
#include <string>

#include "engine/rhi/Pipeline.h"

namespace engine::resources {

class ShaderProgram final {
public:
    std::string key;
    std::string vertexPath;
    std::string fragmentPath;
    std::unique_ptr<rhi::IShaderModule> vertexShader;
    std::unique_ptr<rhi::IShaderModule> fragmentShader;
    std::unique_ptr<rhi::IPipelineLayout> pipelineLayout;
    std::unique_ptr<rhi::IGraphicsPipeline> pipeline;

    [[nodiscard]] bool isReady() const {
        return vertexShader != nullptr && fragmentShader != nullptr && pipelineLayout != nullptr && pipeline != nullptr;
    }
};

} // namespace engine::resources
```

### `engine/resources/loaders.h`

```cpp
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
```

### `engine/resources/loaders.cpp` (основная логика)

```cpp
#include "engine/resources/loaders.h"

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

    if (!ok) {
        if (errorMessage != nullptr) {
            *errorMessage = errors.empty() ? "tinyobjloader failed to parse file" : errors;
        }
        return false;
    }

    outMeshData.vertices.clear();
    outMeshData.indices.clear();

    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> vertexCache;
    bool hasAnyNormals = false;
    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            const VertexKey key{index.vertex_index, index.normal_index, index.texcoord_index};
            if (const auto cacheIt = vertexCache.find(key); cacheIt != vertexCache.end()) {
                outMeshData.indices.push_back(cacheIt->second);
                continue;
            }

            MeshVertex vertex{};
            vertex.position = readVec3(attrib.vertices, key.vertexIndex, glm::vec3(0.0F));
            vertex.normal = readVec3(attrib.normals, key.normalIndex, glm::vec3(0.0F, 0.0F, 1.0F));
            vertex.uv = readVec2(attrib.texcoords, key.texcoordIndex, glm::vec2(0.0F));
            vertex.uv.y = 1.0F - vertex.uv.y;
            hasAnyNormals = hasAnyNormals || key.normalIndex >= 0;

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

    if (const std::size_t separatorPos = descriptor.find('|'); separatorPos != std::string::npos) {
        outSource.vertexPath = descriptor.substr(0, separatorPos);
        outSource.fragmentPath = descriptor.substr(separatorPos + 1U);
        outSource.descriptorIsFile = false;
        return !outSource.vertexPath.empty() && !outSource.fragmentPath.empty();
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
        return !outSource.vertexPath.empty() && !outSource.fragmentPath.empty();
    } catch (const std::exception& ex) {
        if (errorMessage != nullptr) {
            *errorMessage = ex.what();
        }
        return false;
    }
}

} // namespace engine::resources
```

---

## 4) `RenderAdapter` + методы создания GPU-ресурсов

### `engine/renderer/RenderAdapter.h`

```cpp
#pragma once

#include <memory>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "engine/ecs/components.h"
#include "engine/renderer/Renderer.h"

namespace engine::renderer {

class RenderAdapter final {
public:
    explicit RenderAdapter(Renderer& renderer) : m_renderer(&renderer) {}

    std::shared_ptr<resources::Mesh> loadMesh(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadMesh(id) : nullptr;
    }
    std::shared_ptr<resources::Texture> loadTexture(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadTexture(id) : nullptr;
    }
    std::shared_ptr<resources::ShaderProgram> loadShaderProgram(const std::string& id) {
        return m_renderer != nullptr ? m_renderer->loadShaderProgram(id) : nullptr;
    }

    std::unique_ptr<rhi::IBuffer> createBuffer(const rhi::BufferDesc& desc, const void* initialData) {
        return m_renderer != nullptr ? m_renderer->createGpuBuffer(desc, initialData) : nullptr;
    }
    std::unique_ptr<rhi::IImage> createImage(const rhi::ImageDesc& desc, const void* initialData) {
        return m_renderer != nullptr ? m_renderer->createGpuImage(desc, initialData) : nullptr;
    }
    std::shared_ptr<resources::ShaderProgram> createShaderProgram(
        const std::string& key,
        const std::string& vertexPath,
        const std::string& fragmentPath) {
        return m_renderer != nullptr ? m_renderer->createShaderProgramFromFiles(key, vertexPath, fragmentPath) : nullptr;
    }

    void drawPrimitive(ecs::PrimitiveType primitiveType, const glm::mat4& transformMatrix, const glm::vec4& tint) {
        if (m_renderer != nullptr) {
            m_renderer->drawPrimitive(primitiveType, transformMatrix, tint);
        }
    }
    void drawMesh(
        const resources::Mesh& mesh,
        const resources::ShaderProgram& shader,
        const resources::Texture& texture,
        const glm::mat4& transformMatrix,
        const glm::vec4& tint) {
        if (m_renderer != nullptr) {
            m_renderer->drawMesh(mesh, shader, texture, transformMatrix, tint);
        }
    }

private:
    Renderer* m_renderer{nullptr};
};

} // namespace engine::renderer
```

### `engine/renderer/Renderer.h` / `Renderer.cpp` (методы, которые вызывает адаптер)

```cpp
// Renderer.h
std::unique_ptr<rhi::IBuffer> createGpuBuffer(const rhi::BufferDesc& desc, const void* initialData);
std::unique_ptr<rhi::IImage> createGpuImage(const rhi::ImageDesc& desc, const void* initialData);
std::shared_ptr<resources::ShaderProgram> createShaderProgramFromFiles(
    const std::string& key,
    const std::string& vertexPath,
    const std::string& fragmentPath);
```

```cpp
// Renderer.cpp
std::unique_ptr<rhi::IBuffer> Renderer::createGpuBuffer(const rhi::BufferDesc& desc, const void* initialData) {
    if (!m_device) {
        return nullptr;
    }
    return m_device->createBuffer(desc, initialData);
}

std::unique_ptr<rhi::IImage> Renderer::createGpuImage(const rhi::ImageDesc& desc, const void* initialData) {
    if (!m_device) {
        return nullptr;
    }
    return m_device->createImage(desc, initialData);
}

std::shared_ptr<resources::ShaderProgram> Renderer::createShaderProgramFromFiles(
    const std::string& key,
    const std::string& vertexPath,
    const std::string& fragmentPath) {
    auto program = createShaderProgram(key, vertexPath, fragmentPath);
    if (!program) {
        return nullptr;
    }

    resources::ShaderProgramSource source{};
    source.descriptorPath = key;
    {
        std::error_code ec;
        source.descriptorIsFile = std::filesystem::exists(key, ec) && !ec;
    }
    source.vertexPath = vertexPath;
    source.fragmentPath = fragmentPath;
    registerShaderForHotReload(key, program, source);
    return program;
}
```

```cpp
// Renderer.cpp: регистрация конкретных загрузчиков в ResourceManager
bool Renderer::initializeResourceSubsystem() {
    m_placeholderMesh = createPlaceholderMesh();
    m_whiteTexture = createWhiteTexture();
    m_fallbackShader = createFallbackShader();

    if (!m_placeholderMesh || !m_whiteTexture || !m_fallbackShader) {
        ENGINE_LOG_ERROR("Failed to create one or more fallback resources");
        return false;
    }

    m_resourceManager.registerFallback<resources::Mesh>(m_placeholderMesh);
    m_resourceManager.registerFallback<resources::Texture>(m_whiteTexture);
    m_resourceManager.registerFallback<resources::ShaderProgram>(m_fallbackShader);

    m_resourceManager.registerLoader<resources::Mesh>(
        [this](const std::string& path) { return loadMeshFromDisk(path); });
    m_resourceManager.registerLoader<resources::Texture>(
        [this](const std::string& path) { return loadTextureFromDisk(path); });
    m_resourceManager.registerLoader<resources::ShaderProgram>(
        [this](const std::string& path) { return loadShaderProgramFromManifest(path); });

    ENGINE_LOG_INFO("Resource subsystem initialized");
    return true;
}
```

```cpp
// Renderer.cpp: конкретные загрузчики, используемые ResourceManager
std::shared_ptr<resources::Mesh> Renderer::loadMeshFromDisk(const std::string& path) {
    resources::MeshData meshData;
    std::string errorMessage;
    if (!resources::loadObjMeshData(path, meshData, &errorMessage)) {
        ENGINE_LOG_ERROR("Mesh load failed '{}': {}", path, errorMessage);
        return nullptr;
    }

    auto mesh = std::make_shared<resources::Mesh>();
    mesh->path = path;
    mesh->data = std::move(meshData);
    if (!uploadMeshToGpu(*mesh)) {
        ENGINE_LOG_ERROR("Mesh upload failed '{}'", path);
        return nullptr;
    }
    return mesh;
}

std::shared_ptr<resources::Texture> Renderer::loadTextureFromDisk(const std::string& path) {
    resources::TextureData textureData;
    std::string errorMessage;
    if (!resources::loadTextureDataRgba8(path, textureData, &errorMessage)) {
        ENGINE_LOG_ERROR("Texture load failed '{}': {}", path, errorMessage);
        return nullptr;
    }

    auto texture = std::make_shared<resources::Texture>();
    texture->path = path;
    texture->data = std::move(textureData);

    rhi::ImageDesc imageDesc{};
    imageDesc.width = static_cast<std::uint32_t>(texture->data.width);
    imageDesc.height = static_cast<std::uint32_t>(texture->data.height);
    imageDesc.format = rhi::ImageFormat::RGBA8;
    imageDesc.generateMipmaps = true;
    texture->image = m_device->createImage(imageDesc, texture->data.pixels.data());
    return texture->image ? texture : nullptr;
}

std::shared_ptr<resources::ShaderProgram> Renderer::loadShaderProgramFromManifest(const std::string& path) {
    resources::ShaderProgramSource source{};
    std::string errorMessage;
    if (!resources::loadShaderProgramSource(path, source, &errorMessage)) {
        ENGINE_LOG_ERROR("Shader source load failed '{}': {}", path, errorMessage);
        return nullptr;
    }

    auto shaderProgram = createShaderProgram(path, source.vertexPath, source.fragmentPath);
    if (!shaderProgram) {
        return nullptr;
    }

    registerShaderForHotReload(path, shaderProgram, source);
    return shaderProgram;
}
```

---

## 5) `MeshRenderer` с ID/ссылками на ресурсы

### `engine/ecs/components.h` (фрагмент)

```cpp
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
```

---

## 6) `RenderSystem` для отрисовки мешей/текстур

### `engine/ecs/render_system.cpp`

```cpp
#include "engine/ecs/render_system.h"

#include <glm/mat4x4.hpp>

#include "engine/ecs/components.h"
#include "engine/ecs/world.h"
#include "engine/renderer/RenderAdapter.h"

namespace engine::ecs {

namespace {

glm::mat4 computeWorldMatrix(World& world, const EntityId entity) {
    auto* transform = world.getComponent<Transform>(entity);
    if (transform == nullptr) {
        return glm::mat4(1.0F);
    }

    glm::mat4 worldMatrix = transform->toMatrix();
    EntityId parent = kInvalidEntity;
    if (const auto* hierarchy = world.getComponent<Hierarchy>(entity)) {
        parent = hierarchy->parent;
    }

    int safetyCounter = 0;
    while (parent != kInvalidEntity && safetyCounter < 64) {
        if (const auto* parentTransform = world.getComponent<Transform>(parent)) {
            worldMatrix = parentTransform->toMatrix() * worldMatrix;
        }

        const auto* parentHierarchy = world.getComponent<Hierarchy>(parent);
        if (parentHierarchy == nullptr || parentHierarchy->parent == parent) {
            break;
        }
        parent = parentHierarchy->parent;
        ++safetyCounter;
    }

    return worldMatrix;
}

} // namespace

void RenderSystem::render(
    World& world,
    renderer::RenderAdapter& renderer,
    const glm::mat4& viewProjectionMatrix) {
    world.forEach<Transform, MeshRenderer>([&](EntityId entity, Transform& transform, MeshRenderer& meshRenderer) {
        (void)transform;
        if (!meshRenderer.visible) {
            return;
        }

        if (meshRenderer.mesh == nullptr && !meshRenderer.meshId.empty()) {
            meshRenderer.mesh = renderer.loadMesh(meshRenderer.meshId);
        }
        if (meshRenderer.texture == nullptr && !meshRenderer.textureId.empty()) {
            meshRenderer.texture = renderer.loadTexture(meshRenderer.textureId);
        }
        if (meshRenderer.shader == nullptr && !meshRenderer.shaderId.empty()) {
            meshRenderer.shader = renderer.loadShaderProgram(meshRenderer.shaderId);
        }

        const glm::mat4 modelMatrix = computeWorldMatrix(world, entity);
        if (meshRenderer.hasGpuResources()) {
            renderer.drawMesh(
                *meshRenderer.mesh,
                *meshRenderer.shader,
                *meshRenderer.texture,
                viewProjectionMatrix * modelMatrix,
                meshRenderer.tint);
            return;
        }

        renderer.drawPrimitive(meshRenderer.primitiveType, modelMatrix, meshRenderer.tint);
    });
}

} // namespace engine::ecs
```

---

## 7) Демонстрация нескольких 3D-моделей с текстурами

### `engine/game/GameplayState.cpp` (фрагменты)

```cpp
void GameplayState::setupScene(renderer::Renderer& rendererInstance) {
    if (m_sceneInitialized) {
        return;
    }

    m_world.clear();

    renderer::RenderAdapter renderAdapter{rendererInstance};

    constexpr std::array<const char*, 3> kModelMeshIds{
        "assets/models/plane.obj",
        "assets/models/cube.obj",
        "assets/models/pyramid.obj"};
    constexpr std::array<const char*, 3> kModelTextureIds{
        "assets/textures/checker.png",
        "assets/textures/DGLogo.png",
        "assets/textures/checker.png"};
    constexpr const char* kTexturedShaderId = "assets/shaders_hlsl/textured.shader.json";

    auto mesh = renderAdapter.loadMesh(kModelMeshIds[0]);
    auto texture = renderAdapter.loadTexture(kModelTextureIds[0]);
    auto shader = renderAdapter.loadShaderProgram(kTexturedShaderId);

    const auto meshCached = renderAdapter.loadMesh(kModelMeshIds[0]);
    const auto textureCached = renderAdapter.loadTexture(kModelTextureIds[0]);
    const auto shaderCached = renderAdapter.loadShaderProgram(kTexturedShaderId);

    ENGINE_LOG_INFO(
        "Resource share check: mesh={} texture={} shader={}",
        mesh.get() == meshCached.get(),
        texture.get() == textureCached.get(),
        shader.get() == shaderCached.get());

    const glm::vec3 modelPositions[] = {
        glm::vec3(-1.0F, -0.9F, 0.0F),
        glm::vec3(1.0F, 0.0F, 0.0F),
        glm::vec3(2.0F, 0.0F, 0.0F),
    };
    const glm::vec4 modelTint[] = {
        glm::vec4(1.0F, 1.0F, 1.0F, 1.0F),
        glm::vec4(0.85F, 0.95F, 1.0F, 1.0F),
        glm::vec4(1.0F, 0.9F, 0.9F, 1.0F),
    };

    ecs::EntityId rootModelEntity = ecs::kInvalidEntity;
    for (int i = 0; i < 3; ++i) {
        const ecs::EntityId entity = m_world.createEntity();

        ecs::Transform transform{};
        transform.position = modelPositions[i];
        transform.scale = glm::vec3(0.9F, 0.9F, 0.9F);

        ecs::MeshRenderer rendererComponent{};
        rendererComponent.meshId = kModelMeshIds[static_cast<std::size_t>(i)];
        rendererComponent.textureId = kModelTextureIds[static_cast<std::size_t>(i)];
        rendererComponent.shaderId = kTexturedShaderId;
        rendererComponent.tint = modelTint[i];

        if (i == 0) {
            rendererComponent.mesh = mesh;
            rendererComponent.texture = texture;
            rendererComponent.shader = shader;
        }

        m_world.addComponent<ecs::Transform>(entity, transform);
        m_world.addComponent<ecs::MeshRenderer>(entity, rendererComponent);
        if (i == 0) {
            rootModelEntity = entity;
        } else {
            ecs::Hierarchy hierarchy{};
            hierarchy.parent = rootModelEntity;
            m_world.addComponent<ecs::Hierarchy>(entity, hierarchy);
        }
        m_world.addComponent<ecs::Tag>(entity, ecs::Tag{std::string("model_") + std::to_string(i)});
    }
}

void GameplayState::render(renderer::Renderer& rendererInstance) {
    setupScene(rendererInstance);
    renderer::RenderAdapter renderAdapter{rendererInstance};
    const glm::mat4 projection = glm::ortho(-2.0F, 2.0F, -1.6F, 1.6F, -5.0F, 5.0F);
    const glm::mat4 view = glm::mat4(1.0F);
    m_renderSystem.render(m_world, renderAdapter, projection * view);
}
```

Используемые ассеты:

```text
assets/models/plane.obj
assets/models/cube.obj
assets/models/pyramid.obj
assets/textures/checker.png
assets/textures/DGLogo.png
assets/shaders_hlsl/textured.shader.json
```

### `assets/shaders_hlsl/textured.shader.json`

```json
{
  "vertex": "assets/shaders_hlsl/textured.vert.hlsl",
  "fragment": "assets/shaders_hlsl/textured.frag.hlsl"
}
```

---

## 8) Бонус: hot-reload шейдеров/конфигураций

### `engine/renderer/Renderer.cpp` (фрагменты)

```cpp
void Renderer::registerShaderForHotReload(
    const std::string& cacheKey,
    const std::shared_ptr<resources::ShaderProgram>& program,
    const resources::ShaderProgramSource& source) {
    if (!program) {
        return;
    }

    ShaderHotReloadEntry entry{};
    entry.program = program;
    entry.descriptorPath = source.descriptorPath;
    entry.descriptorIsFile = source.descriptorIsFile;
    entry.vertexPath = source.vertexPath;
    entry.fragmentPath = source.fragmentPath;
    entry.descriptorWriteTime =
        source.descriptorIsFile ? getFileWriteTimeOrMin(source.descriptorPath) : std::filesystem::file_time_type::min();
    entry.vertexWriteTime = getFileWriteTimeOrMin(source.vertexPath);
    entry.fragmentWriteTime = getFileWriteTimeOrMin(source.fragmentPath);
    m_shaderHotReloadEntries[cacheKey] = std::move(entry);
}

bool Renderer::reloadShaderInPlace(resources::ShaderProgram& program, const resources::ShaderProgramSource& source) {
    auto reloadedProgram = createShaderProgram(program.key, source.vertexPath, source.fragmentPath);
    if (!reloadedProgram) {
        return false;
    }

    program.vertexPath = source.vertexPath;
    program.fragmentPath = source.fragmentPath;
    program.vertexShader = std::move(reloadedProgram->vertexShader);
    program.fragmentShader = std::move(reloadedProgram->fragmentShader);
    program.pipelineLayout = std::move(reloadedProgram->pipelineLayout);
    program.pipeline = std::move(reloadedProgram->pipeline);
    return true;
}

void Renderer::pollHotReload() {
    if (!m_initialized || m_shaderHotReloadEntries.empty()) {
        return;
    }

    for (auto it = m_shaderHotReloadEntries.begin(); it != m_shaderHotReloadEntries.end();) {
        auto program = it->second.program.lock();
        if (!program) {
            it = m_shaderHotReloadEntries.erase(it);
            continue;
        }

        auto& entry = it->second;
        const auto descriptorWriteTime =
            entry.descriptorIsFile ? getFileWriteTimeOrMin(entry.descriptorPath) : std::filesystem::file_time_type::min();
        const auto vertexWriteTime = getFileWriteTimeOrMin(entry.vertexPath);
        const auto fragmentWriteTime = getFileWriteTimeOrMin(entry.fragmentPath);

        const bool descriptorChanged = descriptorWriteTime != entry.descriptorWriteTime;
        const bool vertexChanged = vertexWriteTime != entry.vertexWriteTime;
        const bool fragmentChanged = fragmentWriteTime != entry.fragmentWriteTime;
        if (!descriptorChanged && !vertexChanged && !fragmentChanged) {
            ++it;
            continue;
        }

        resources::ShaderProgramSource latestSource{};
        if (entry.descriptorIsFile) {
            std::string sourceError;
            if (!resources::loadShaderProgramSource(entry.descriptorPath, latestSource, &sourceError)) {
                ++it;
                continue;
            }
        } else {
            latestSource.descriptorPath = entry.descriptorPath;
            latestSource.descriptorIsFile = false;
            latestSource.vertexPath = entry.vertexPath;
            latestSource.fragmentPath = entry.fragmentPath;
        }

        if (!reloadShaderInPlace(*program, latestSource)) {
            ++it;
            continue;
        }

        entry.descriptorPath = latestSource.descriptorPath;
        entry.descriptorIsFile = latestSource.descriptorIsFile;
        entry.vertexPath = latestSource.vertexPath;
        entry.fragmentPath = latestSource.fragmentPath;
        entry.descriptorWriteTime =
            entry.descriptorIsFile ? getFileWriteTimeOrMin(entry.descriptorPath) : std::filesystem::file_time_type::min();
        entry.vertexWriteTime = getFileWriteTimeOrMin(entry.vertexPath);
        entry.fragmentWriteTime = getFileWriteTimeOrMin(entry.fragmentPath);
        ++it;
    }
}
```

### `engine/core/Application.cpp` (кадровый вызов)

```cpp
void Application::pollRuntimeHotReload() {
    m_renderer.pollHotReload();
    // hot-reload config.json
}
```
