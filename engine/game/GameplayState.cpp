#include "engine/game/GameplayState.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/ecs/collision_utils.h"
#include "engine/ecs/components.h"
#include "engine/ecs/transform_utils.h"
#include "engine/game/StateStack.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/renderer/RenderAdapter.h"
#include "engine/renderer/Renderer.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/misc/cpp/imgui_stdlib.h"
#include "ImGuizmo.h"

namespace {

using engine::ecs::EntityId;

constexpr const char* kCubeMeshId = "__diligent_cube__";
constexpr const char* kPyramidMeshId = "__procedural_pyramid__";
constexpr const char* kSphereMeshId = "__diligent_sphere__";
constexpr const char* kShaderId = "assets/shaders_hlsl/textured.shader.json";
constexpr const char* kLogoTextureId = "assets/textures/DGLogo.png";
constexpr const char* kArenaFloorTextureId = "assets/textures/arena_floor.ppm";
constexpr const char* kArenaWallTextureId = "assets/textures/arena_wall.ppm";
constexpr const char* kEditorScenePath = "assets/scenes/editor_scene.json";
constexpr const char* kEditorLayoutPath = "assets/editor_layout.json";

const glm::vec3 kCameraStartPosition{0.0F, 6.5F, -18.0F};
const glm::vec3 kCameraStartRotation{glm::radians(-11.0F), 0.0F, 0.0F};
const glm::vec3 kFloorPosition{0.0F, -2.5F, 10.0F};
const glm::vec3 kFloorScale{28.0F, 1.0F, 20.0F};
const glm::vec3 kStrikerStartPosition{-10.8F, -1.0F, 10.0F};
const glm::vec3 kStrikerScale{1.85F, 1.85F, 1.85F};
const glm::vec3 kStrikerLaunchVelocity{22.0F, 0.0F, 0.0F};
const glm::vec3 kShowcaseSphereStartPosition{-9.2F, -1.1F, 16.0F};
const glm::vec3 kShowcaseSphereLaunchVelocity{7.5F, 0.0F, -2.2F};
constexpr std::size_t kSpawnBudgetPerFrame = 6U;
constexpr std::size_t kMaxEditorHistory = 64U;
constexpr float kWindowMargin = 8.0F;
constexpr float kToolbarHeight = 44.0F;
constexpr float kStatusBarHeight = 28.0F;
constexpr float kViewportHeaderHeight = 34.0F;
constexpr float kMinGizmoScale = 0.05F;
constexpr float kMaxGizmoScale = 200.0F;
constexpr const char* kEditorDockPayload = "TGE_DOCK_PANEL";

struct EditorLayout final {
    ImVec2 toolbarPos;
    ImVec2 toolbarSize;
    ImVec2 hierarchyPos;
    ImVec2 hierarchySize;
    ImVec2 projectPos;
    ImVec2 projectSize;
    ImVec2 viewportPos;
    ImVec2 viewportSize;
    ImVec2 inspectorPos;
    ImVec2 inspectorSize;
    ImVec2 statsPos;
    ImVec2 statsSize;
    ImVec2 statusPos;
    ImVec2 statusSize;
};

struct TransformComponents final {
    glm::vec3 position{0.0F};
    glm::vec3 rotationEulerRadians{0.0F};
    glm::vec3 scale{1.0F};
};

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::filesystem::path editorScenePath() {
#if defined(ENGINE_SOURCE_ROOT)
    return std::filesystem::path(ENGINE_SOURCE_ROOT) / kEditorScenePath;
#else
    return std::filesystem::path(kEditorScenePath);
#endif
}

std::filesystem::path editorLayoutPath() {
#if defined(ENGINE_SOURCE_ROOT)
    return std::filesystem::path(ENGINE_SOURCE_ROOT) / kEditorLayoutPath;
#else
    return std::filesystem::path(kEditorLayoutPath);
#endif
}

std::filesystem::path editorAssetsRoot() {
#if defined(ENGINE_SOURCE_ROOT)
    return std::filesystem::path(ENGINE_SOURCE_ROOT) / "assets";
#else
    return std::filesystem::path("assets");
#endif
}

std::string toAssetPath(const std::filesystem::path& path) {
    const std::filesystem::path root = editorAssetsRoot();
    std::error_code error;
    std::filesystem::path relative = std::filesystem::relative(path, root, error);
    if (error) {
        relative = path.filename();
    }
    return (std::filesystem::path("assets") / relative).generic_string();
}

std::string classifyAsset(const std::filesystem::path& path) {
    const std::string extension = toLowerCopy(path.extension().string());
    const std::string filename = toLowerCopy(path.filename().string());
    if (filename.ends_with(".shader.json")) {
        return "Shader";
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".ppm" || extension == ".tga") {
        return "Texture";
    }
    if (extension == ".obj") {
        return "Mesh";
    }
    if (extension == ".json") {
        return "Scene";
    }
    if (extension == ".hlsl" || extension == ".glsl") {
        return "Shader Source";
    }
    return "Asset";
}

std::string assetDisplayName(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    if (name.ends_with(".shader.json")) {
        name.resize(name.size() - std::string(".json").size());
    }
    return name;
}

std::string ellipsizeText(const std::string& value, const std::size_t maxChars) {
    if (value.size() <= maxChars || maxChars <= 3U) {
        return value;
    }
    return value.substr(0U, maxChars - 3U) + "...";
}

bool isFiniteVec3(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool isFiniteMat4(const glm::mat4& value) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(value[column][row])) {
                return false;
            }
        }
    }
    return true;
}

float unwrapAngleNear(const float angle, const float reference) {
    if (!std::isfinite(angle)) {
        return reference;
    }

    float result = angle;
    const float pi = glm::pi<float>();
    const float twoPi = glm::two_pi<float>();
    while (result - reference > pi) {
        result -= twoPi;
    }
    while (result - reference < -pi) {
        result += twoPi;
    }
    return result;
}

glm::vec3 unwrapEulerNear(const glm::vec3& euler, const glm::vec3& reference) {
    return glm::vec3(
        unwrapAngleNear(euler.x, reference.x),
        unwrapAngleNear(euler.y, reference.y),
        unwrapAngleNear(euler.z, reference.z));
}

glm::vec3 extractXyzEulerRadians(const glm::mat3& rotation, const glm::vec3& previousRotation) {
    const float r02 = std::clamp(rotation[2][0], -1.0F, 1.0F);
    const float y = std::asin(r02);
    const float cosY = std::cos(y);

    float x = previousRotation.x;
    float z = previousRotation.z;
    if (std::abs(cosY) > 1e-4F) {
        x = std::atan2(-rotation[2][1], rotation[2][2]);
        z = std::atan2(-rotation[1][0], rotation[0][0]);
    } else {
        z = 0.0F;
        if (r02 > 0.0F) {
            x = std::atan2(rotation[0][1], rotation[1][1]);
        } else {
            x = std::atan2(-rotation[0][1], rotation[1][1]);
        }
    }

    return unwrapEulerNear(glm::vec3(x, y, z), previousRotation);
}

std::optional<TransformComponents> decomposeEditorTransform(
    const glm::mat4& matrix,
    const glm::vec3& previousRotation,
    const glm::vec3& previousScale) {
    if (!isFiniteMat4(matrix)) {
        return std::nullopt;
    }

    TransformComponents result{};
    result.position = glm::vec3(matrix[3]);
    if (!isFiniteVec3(result.position)) {
        return std::nullopt;
    }

    glm::vec3 axisX(matrix[0]);
    glm::vec3 axisY(matrix[1]);
    glm::vec3 axisZ(matrix[2]);
    result.scale = glm::vec3(glm::length(axisX), glm::length(axisY), glm::length(axisZ));
    if (!isFiniteVec3(result.scale)) {
        return std::nullopt;
    }

    result.scale = glm::clamp(result.scale, glm::vec3(kMinGizmoScale), glm::vec3(kMaxGizmoScale));
    if (glm::length(axisX) <= 1e-5F || glm::length(axisY) <= 1e-5F || glm::length(axisZ) <= 1e-5F) {
        result.rotationEulerRadians = previousRotation;
        result.scale = glm::clamp(previousScale, glm::vec3(kMinGizmoScale), glm::vec3(kMaxGizmoScale));
        return result;
    }

    axisX = glm::normalize(axisX);
    axisY = axisY - axisX * glm::dot(axisX, axisY);
    if (glm::length(axisY) <= 1e-5F) {
        axisY = glm::vec3(0.0F, 1.0F, 0.0F);
        if (std::abs(glm::dot(axisX, axisY)) > 0.95F) {
            axisY = glm::vec3(0.0F, 0.0F, 1.0F);
        }
        axisY = glm::normalize(axisY - axisX * glm::dot(axisX, axisY));
    } else {
        axisY = glm::normalize(axisY);
    }
    axisZ = glm::normalize(glm::cross(axisX, axisY));
    axisY = glm::normalize(glm::cross(axisZ, axisX));

    glm::mat3 rotation(1.0F);
    rotation[0] = axisX;
    rotation[1] = axisY;
    rotation[2] = axisZ;
    if (glm::determinant(rotation) < 0.0F) {
        rotation[2] = -rotation[2];
    }

    result.rotationEulerRadians = extractXyzEulerRadians(rotation, previousRotation);
    if (!isFiniteVec3(result.rotationEulerRadians)) {
        result.rotationEulerRadians = previousRotation;
    }
    return result;
}

void stabilizeDirectlyEditedBody(engine::ecs::Rigidbody& rigidbody) {
    rigidbody.velocity = glm::vec3(0.0F);
    rigidbody.angularVelocity = glm::vec3(0.0F);
    rigidbody.accumulatedForce = glm::vec3(0.0F);
    rigidbody.accumulatedTorque = glm::vec3(0.0F);
    rigidbody.wakeUp();
}

std::string formatBytes(const std::uintmax_t bytes) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(bytes >= 1024U * 1024U ? 1 : 0);
    if (bytes >= 1024U * 1024U) {
        stream << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
    } else if (bytes >= 1024U) {
        stream << static_cast<double>(bytes) / 1024.0 << " KB";
    } else {
        stream << bytes << " B";
    }
    return stream.str();
}

std::string readTokenSkippingPpmComments(std::istream& stream) {
    std::string token;
    while (stream >> token) {
        if (!token.empty() && token[0] == '#') {
            std::string ignored;
            std::getline(stream, ignored);
            continue;
        }
        return token;
    }
    return {};
}

bool loadPpmPreview(
    const std::filesystem::path& path,
    std::array<std::uint32_t, 64>& outPixels,
    std::uint32_t& outWidth,
    std::uint32_t& outHeight) {
    if (toLowerCopy(path.extension().string()) != ".ppm") {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    const std::string magic = readTokenSkippingPpmComments(file);
    if (magic != "P3") {
        return false;
    }

    const int width = std::max(0, std::atoi(readTokenSkippingPpmComments(file).c_str()));
    const int height = std::max(0, std::atoi(readTokenSkippingPpmComments(file).c_str()));
    const int maxValue = std::max(1, std::atoi(readTokenSkippingPpmComments(file).c_str()));
    if (width <= 0 || height <= 0) {
        return false;
    }

    std::vector<std::uint32_t> sourcePixels;
    sourcePixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int i = 0; i < width * height; ++i) {
        const int r = std::atoi(readTokenSkippingPpmComments(file).c_str());
        const int g = std::atoi(readTokenSkippingPpmComments(file).c_str());
        const int b = std::atoi(readTokenSkippingPpmComments(file).c_str());
        const int sr = std::clamp((r * 255) / maxValue, 0, 255);
        const int sg = std::clamp((g * 255) / maxValue, 0, 255);
        const int sb = std::clamp((b * 255) / maxValue, 0, 255);
        sourcePixels.push_back(IM_COL32(sr, sg, sb, 255));
    }
    if (sourcePixels.empty()) {
        return false;
    }

    outWidth = 8U;
    outHeight = 8U;
    for (std::uint32_t y = 0; y < outHeight; ++y) {
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            const int sourceX = std::min(width - 1, static_cast<int>((x * static_cast<std::uint32_t>(width)) / outWidth));
            const int sourceY = std::min(height - 1, static_cast<int>((y * static_cast<std::uint32_t>(height)) / outHeight));
            outPixels[static_cast<std::size_t>(y * outWidth + x)] =
                sourcePixels[static_cast<std::size_t>(sourceY * width + sourceX)];
        }
    }
    return true;
}

std::string buildAssetDetails(const std::filesystem::path& path, const std::string& kind) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    std::ostringstream details;
    details << kind;
    if (!error) {
        details << " - " << formatBytes(size);
    }

    const std::string extension = toLowerCopy(path.extension().string());
    if (kind == "Mesh" && extension == ".obj") {
        std::ifstream file(path);
        std::size_t vertices = 0;
        std::size_t faces = 0;
        std::string line;
        while (std::getline(file, line)) {
            if (line.rfind("v ", 0) == 0) {
                ++vertices;
            } else if (line.rfind("f ", 0) == 0) {
                ++faces;
            }
        }
        details << " - " << vertices << " verts, " << faces << " faces";
    } else if (kind == "Shader" || kind == "Shader Source") {
        details << " - editable code";
    } else if (kind == "Scene") {
        details << " - scene file";
    } else if (kind == "Texture") {
        details << " - texture preview";
    }

    return details.str();
}

const char* assetKindGlyph(const std::string& kind) {
    if (kind == "Texture") {
        return "T";
    }
    if (kind == "Mesh") {
        return "M";
    }
    if (kind == "Scene") {
        return "S";
    }
    if (kind == "Shader" || kind == "Shader Source") {
        return "H";
    }
    return "A";
}

ImVec4 assetKindColor(const std::string& kind) {
    if (kind == "Texture") {
        return ImVec4(0.30F, 0.62F, 0.95F, 1.0F);
    }
    if (kind == "Mesh") {
        return ImVec4(0.92F, 0.67F, 0.28F, 1.0F);
    }
    if (kind == "Scene") {
        return ImVec4(0.36F, 0.78F, 0.58F, 1.0F);
    }
    if (kind == "Shader" || kind == "Shader Source") {
        return ImVec4(0.78F, 0.48F, 0.96F, 1.0F);
    }
    return ImVec4(0.58F, 0.64F, 0.72F, 1.0F);
}

bool assetMatchesFilter(
    const std::string& path,
    const std::string& name,
    const std::string& kind,
    const std::string& loweredFilter) {
    if (loweredFilter.empty()) {
        return true;
    }
    const std::string searchable = toLowerCopy(path + " " + name + " " + kind);
    return searchable.find(loweredFilter) != std::string::npos;
}

nlohmann::json toJson(const glm::vec2& value) {
    return nlohmann::json::array({value.x, value.y});
}

nlohmann::json toJson(const glm::vec3& value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

nlohmann::json toJson(const glm::vec4& value) {
    return nlohmann::json::array({value.x, value.y, value.z, value.w});
}

glm::vec2 vec2FromJson(const nlohmann::json& value, const glm::vec2& fallback) {
    if (!value.is_array() || value.size() != 2) {
        return fallback;
    }
    return glm::vec2(value[0].get<float>(), value[1].get<float>());
}

glm::vec3 vec3FromJson(const nlohmann::json& value, const glm::vec3& fallback) {
    if (!value.is_array() || value.size() != 3) {
        return fallback;
    }
    return glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
}

glm::vec4 vec4FromJson(const nlohmann::json& value, const glm::vec4& fallback) {
    if (!value.is_array() || value.size() != 4) {
        return fallback;
    }
    return glm::vec4(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>());
}

nlohmann::json transformToJson(const engine::ecs::Transform& transform) {
    return nlohmann::json{
        {"position", toJson(transform.position)},
        {"rotationEulerRadians", toJson(transform.rotationEulerRadians)},
        {"scale", toJson(transform.scale)}};
}

engine::ecs::Transform transformFromJson(const nlohmann::json& value) {
    engine::ecs::Transform transform{};
    transform.position = vec3FromJson(value.value("position", nlohmann::json::array()), transform.position);
    transform.rotationEulerRadians =
        vec3FromJson(value.value("rotationEulerRadians", nlohmann::json::array()), transform.rotationEulerRadians);
    transform.scale = vec3FromJson(value.value("scale", nlohmann::json::array()), transform.scale);
    return transform;
}

nlohmann::json cameraToJson(const engine::ecs::Camera& camera) {
    return nlohmann::json{
        {"verticalFovRadians", camera.verticalFovRadians},
        {"nearPlane", camera.nearPlane},
        {"farPlane", camera.farPlane},
        {"active", camera.active}};
}

engine::ecs::Camera cameraFromJson(const nlohmann::json& value) {
    engine::ecs::Camera camera{};
    camera.verticalFovRadians = value.value("verticalFovRadians", camera.verticalFovRadians);
    camera.nearPlane = value.value("nearPlane", camera.nearPlane);
    camera.farPlane = value.value("farPlane", camera.farPlane);
    camera.active = value.value("active", camera.active);
    return camera;
}

nlohmann::json rigidbodyToJson(const engine::ecs::Rigidbody& rigidbody) {
    return nlohmann::json{
        {"velocity", toJson(rigidbody.velocity)},
        {"acceleration", toJson(rigidbody.acceleration)},
        {"angularVelocity", toJson(rigidbody.angularVelocity)},
        {"mass", rigidbody.mass},
        {"linearDamping", rigidbody.linearDamping},
        {"angularDamping", rigidbody.angularDamping},
        {"restitution", rigidbody.restitution},
        {"friction", rigidbody.friction},
        {"useGravity", rigidbody.useGravity},
        {"isStatic", rigidbody.isStatic},
        {"isKinematic", rigidbody.isKinematic},
        {"canSleep", rigidbody.canSleep}};
}

engine::ecs::Rigidbody rigidbodyFromJson(const nlohmann::json& value) {
    engine::ecs::Rigidbody rigidbody{};
    rigidbody.velocity = vec3FromJson(value.value("velocity", nlohmann::json::array()), rigidbody.velocity);
    rigidbody.acceleration = vec3FromJson(value.value("acceleration", nlohmann::json::array()), rigidbody.acceleration);
    rigidbody.angularVelocity =
        vec3FromJson(value.value("angularVelocity", nlohmann::json::array()), rigidbody.angularVelocity);
    rigidbody.mass = value.value("mass", rigidbody.mass);
    rigidbody.linearDamping = value.value("linearDamping", rigidbody.linearDamping);
    rigidbody.angularDamping = value.value("angularDamping", rigidbody.angularDamping);
    rigidbody.restitution = value.value("restitution", rigidbody.restitution);
    rigidbody.friction = value.value("friction", rigidbody.friction);
    rigidbody.useGravity = value.value("useGravity", rigidbody.useGravity);
    rigidbody.isStatic = value.value("isStatic", rigidbody.isStatic);
    rigidbody.isKinematic = value.value("isKinematic", rigidbody.isKinematic);
    rigidbody.canSleep = value.value("canSleep", rigidbody.canSleep);
    rigidbody.recalculateMassProperties();
    return rigidbody;
}

nlohmann::json colliderToJson(const engine::ecs::Collider& collider) {
    return nlohmann::json{
        {"type", collider.type == engine::ecs::ColliderType::Sphere ? "sphere" : "aabb"},
        {"offset", toJson(collider.offset)},
        {"halfExtents", toJson(collider.aabb.halfExtents)},
        {"radius", collider.sphere.radius},
        {"enabled", collider.enabled}};
}

engine::ecs::Collider colliderFromJson(const nlohmann::json& value) {
    engine::ecs::Collider collider{};
    collider.type = value.value("type", std::string("aabb")) == "sphere" ? engine::ecs::ColliderType::Sphere
                                                                          : engine::ecs::ColliderType::Aabb;
    collider.offset = vec3FromJson(value.value("offset", nlohmann::json::array()), collider.offset);
    collider.aabb.halfExtents = vec3FromJson(value.value("halfExtents", nlohmann::json::array()), collider.aabb.halfExtents);
    collider.sphere.radius = value.value("radius", collider.sphere.radius);
    collider.enabled = value.value("enabled", collider.enabled);
    return collider;
}

nlohmann::json meshRendererToJson(const engine::ecs::MeshRenderer& meshRenderer) {
    return nlohmann::json{
        {"primitiveType", static_cast<int>(meshRenderer.primitiveType)},
        {"tint", toJson(meshRenderer.tint)},
        {"uvScale", toJson(meshRenderer.uvScale)},
        {"meshId", meshRenderer.meshId},
        {"textureId", meshRenderer.textureId},
        {"shaderId", meshRenderer.shaderId},
        {"visible", meshRenderer.visible}};
}

engine::ecs::MeshRenderer meshRendererFromJson(const nlohmann::json& value) {
    engine::ecs::MeshRenderer meshRenderer{};
    meshRenderer.primitiveType =
        static_cast<engine::ecs::PrimitiveType>(value.value("primitiveType", static_cast<int>(meshRenderer.primitiveType)));
    meshRenderer.tint = vec4FromJson(value.value("tint", nlohmann::json::array()), meshRenderer.tint);
    meshRenderer.uvScale = vec2FromJson(value.value("uvScale", nlohmann::json::array()), meshRenderer.uvScale);
    meshRenderer.meshId = value.value("meshId", meshRenderer.meshId);
    meshRenderer.textureId = value.value("textureId", meshRenderer.textureId);
    meshRenderer.shaderId = value.value("shaderId", meshRenderer.shaderId);
    meshRenderer.visible = value.value("visible", meshRenderer.visible);
    return meshRenderer;
}

glm::vec4 paletteColor(const std::size_t index) {
    static constexpr std::array<glm::vec4, 8> kPalette{
        glm::vec4(0.82F, 0.42F, 0.32F, 1.0F),
        glm::vec4(0.88F, 0.63F, 0.24F, 1.0F),
        glm::vec4(0.55F, 0.68F, 0.32F, 1.0F),
        glm::vec4(0.23F, 0.63F, 0.61F, 1.0F),
        glm::vec4(0.28F, 0.52F, 0.79F, 1.0F),
        glm::vec4(0.55F, 0.44F, 0.74F, 1.0F),
        glm::vec4(0.80F, 0.46F, 0.62F, 1.0F),
        glm::vec4(0.77F, 0.72F, 0.58F, 1.0F)};
    return kPalette[index % kPalette.size()];
}

glm::vec4 pyramidAccentColor(const std::size_t index) {
    static constexpr std::array<glm::vec4, 5> kPalette{
        glm::vec4(0.90F, 0.50F, 0.24F, 1.0F),
        glm::vec4(0.27F, 0.68F, 0.56F, 1.0F),
        glm::vec4(0.29F, 0.52F, 0.88F, 1.0F),
        glm::vec4(0.80F, 0.34F, 0.54F, 1.0F),
        glm::vec4(0.76F, 0.64F, 0.22F, 1.0F)};
    return kPalette[index % kPalette.size()];
}

EditorLayout computeEditorLayout(
    const float requestedLeftWidth,
    const float requestedRightWidth,
    const float requestedLeftSplit,
    const float requestedRightSplit) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float menuBarHeight = ImGui::GetFrameHeight();
    const float defaultLeftWidth = std::clamp(viewport->Size.x * 0.19F, 250.0F, 360.0F);
    const float defaultRightWidth = std::clamp(viewport->Size.x * 0.24F, 320.0F, 430.0F);
    const float minLeftWidth = 210.0F;
    const float minRightWidth = 280.0F;
    const float minViewportWidth = 420.0F;
    const float sidebarBudget = std::max(
        minLeftWidth + minRightWidth,
        viewport->Size.x - minViewportWidth - (kWindowMargin * 4.0F));
    float leftWidth = requestedLeftWidth > 0.0F ? requestedLeftWidth : defaultLeftWidth;
    float rightWidth = requestedRightWidth > 0.0F ? requestedRightWidth : defaultRightWidth;
    leftWidth = std::clamp(leftWidth, minLeftWidth, std::min(560.0F, sidebarBudget - minRightWidth));
    rightWidth = std::clamp(rightWidth, minRightWidth, std::min(620.0F, sidebarBudget - minLeftWidth));
    if (leftWidth + rightWidth > sidebarBudget) {
        const float overflow = leftWidth + rightWidth - sidebarBudget;
        const float shrinkLeft = std::min(overflow * 0.5F, leftWidth - minLeftWidth);
        leftWidth -= shrinkLeft;
        rightWidth -= std::min(overflow - shrinkLeft, rightWidth - minRightWidth);
    }

    const float statusPosY = viewport->Pos.y + viewport->Size.y - kStatusBarHeight - kWindowMargin;
    const float panelsTop = viewport->Pos.y + menuBarHeight + kToolbarHeight + (kWindowMargin * 2.0F);
    const float panelsHeight = std::max(220.0F, statusPosY - panelsTop - kWindowMargin);
    const float stackHeight = std::max(160.0F, panelsHeight - kWindowMargin);
    const float leftSplit = std::clamp(requestedLeftSplit, 0.22F, 0.82F);
    const float rightSplit = std::clamp(requestedRightSplit, 0.22F, 0.82F);
    const float hierarchyHeight = std::clamp(stackHeight * leftSplit, 96.0F, stackHeight - 96.0F);
    const float projectHeight = std::max(96.0F, stackHeight - hierarchyHeight);
    const float inspectorHeight = std::clamp(stackHeight * rightSplit, 120.0F, stackHeight - 96.0F);
    const float statsHeight = std::max(96.0F, stackHeight - inspectorHeight);
    const float viewportWidth =
        std::max(380.0F, viewport->Size.x - leftWidth - rightWidth - (kWindowMargin * 4.0F));

    EditorLayout layout{};
    layout.toolbarPos = ImVec2(viewport->Pos.x + kWindowMargin, viewport->Pos.y + menuBarHeight + kWindowMargin);
    layout.toolbarSize = ImVec2(viewport->Size.x - (kWindowMargin * 2.0F), kToolbarHeight);
    layout.hierarchyPos = ImVec2(viewport->Pos.x + kWindowMargin, panelsTop);
    layout.hierarchySize = ImVec2(leftWidth, hierarchyHeight);
    layout.projectPos = ImVec2(layout.hierarchyPos.x, layout.hierarchyPos.y + layout.hierarchySize.y + kWindowMargin);
    layout.projectSize = ImVec2(leftWidth, projectHeight);
    layout.viewportPos = ImVec2(layout.hierarchyPos.x + layout.hierarchySize.x + kWindowMargin, panelsTop);
    layout.viewportSize = ImVec2(viewportWidth, panelsHeight);
    layout.inspectorPos = ImVec2(layout.viewportPos.x + layout.viewportSize.x + kWindowMargin, panelsTop);
    layout.inspectorSize = ImVec2(rightWidth, inspectorHeight);
    layout.statsPos = ImVec2(layout.inspectorPos.x, layout.inspectorPos.y + layout.inspectorSize.y + kWindowMargin);
    layout.statsSize = ImVec2(rightWidth, statsHeight);
    layout.statusPos = ImVec2(viewport->Pos.x + kWindowMargin, statusPosY);
    layout.statusSize = ImVec2(viewport->Size.x - (kWindowMargin * 2.0F), kStatusBarHeight);
    return layout;
}

bool isHierarchyParentValid(const engine::ecs::World& world, const EntityId entity, EntityId candidateParent) {
    if (candidateParent == engine::ecs::kInvalidEntity || candidateParent == entity) {
        return candidateParent == engine::ecs::kInvalidEntity;
    }

    EntityId current = candidateParent;
    int depth = 0;
    while (current != engine::ecs::kInvalidEntity && depth < 64) {
        if (current == entity) {
            return false;
        }
        const auto* hierarchy = world.getComponent<engine::ecs::Hierarchy>(current);
        if (hierarchy == nullptr || hierarchy->parent == current) {
            break;
        }
        current = hierarchy->parent;
        ++depth;
    }
    return true;
}

std::string componentSubtitle(const engine::ecs::World& world, const EntityId entity) {
    std::string subtitle = "#" + std::to_string(entity);
    if (world.hasComponent<engine::ecs::Camera>(entity)) {
        subtitle += "  CAM";
    }
    if (const auto* rigidbody = world.getComponent<engine::ecs::Rigidbody>(entity);
        rigidbody != nullptr && rigidbody->isDynamic()) {
        subtitle += "  RB";
    }
    return subtitle;
}

glm::mat3 absoluteBasis(const glm::mat4& matrix) {
    glm::mat3 basis(matrix);
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            basis[column][row] = std::abs(basis[column][row]);
        }
    }
    return basis;
}

bool intersectRayAabb(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const engine::ecs::WorldAabb& aabb,
    float& outDistance) {
    float tMin = 0.0F;
    float tMax = std::numeric_limits<float>::max();

    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-6F) {
            if (origin[axis] < aabb.min[axis] || origin[axis] > aabb.max[axis]) {
                return false;
            }
            continue;
        }

        float nearDistance = (aabb.min[axis] - origin[axis]) / direction[axis];
        float farDistance = (aabb.max[axis] - origin[axis]) / direction[axis];
        if (nearDistance > farDistance) {
            std::swap(nearDistance, farDistance);
        }

        tMin = std::max(tMin, nearDistance);
        tMax = std::min(tMax, farDistance);
        if (tMin > tMax) {
            return false;
        }
    }

    outDistance = tMin;
    return true;
}

bool intersectRayLocalAabb(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const glm::mat4& localToWorld,
    const glm::vec3& localCenter,
    const glm::vec3& localHalfExtents,
    float& outDistance) {
    const glm::mat4 worldToLocal = glm::inverse(localToWorld);
    const glm::vec3 localOrigin = glm::vec3(worldToLocal * glm::vec4(origin, 1.0F));
    const glm::vec3 localDirection = glm::vec3(worldToLocal * glm::vec4(direction, 0.0F));

    const engine::ecs::WorldAabb localAabb{localCenter - localHalfExtents, localCenter + localHalfExtents};
    return intersectRayAabb(localOrigin, localDirection, localAabb, outDistance);
}

bool intersectRaySphere(
    const glm::vec3& origin,
    const glm::vec3& direction,
    const engine::ecs::WorldSphere& sphere,
    float& outDistance) {
    const glm::vec3 toCenter = sphere.center - origin;
    const float projectedDistance = glm::dot(toCenter, direction);
    const float closestDistanceSq = glm::dot(toCenter, toCenter) - projectedDistance * projectedDistance;
    const float radiusSq = sphere.radius * sphere.radius;
    if (closestDistanceSq > radiusSq) {
        return false;
    }

    const float halfChord = std::sqrt(std::max(0.0F, radiusSq - closestDistanceSq));
    const float nearDistance = projectedDistance - halfChord;
    const float farDistance = projectedDistance + halfChord;
    if (farDistance < 0.0F) {
        return false;
    }

    outDistance = std::max(0.0F, nearDistance);
    return true;
}

bool computeFallbackPickShape(
    const engine::ecs::World& world,
    EntityId entity,
    engine::ecs::WorldColliderShape& outShape);

bool intersectRayEntityBounds(
    const engine::ecs::World& world,
    const EntityId entity,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float& outDistance) {
    if (!world.hasComponent<engine::ecs::Transform>(entity)) {
        return false;
    }

    const glm::mat4 worldMatrix = engine::ecs::computeWorldMatrix(world, entity);
    const auto* meshRenderer = world.getComponent<engine::ecs::MeshRenderer>(entity);
    if (meshRenderer != nullptr && !meshRenderer->visible) {
        return false;
    }

    if (const auto* collider = world.getComponent<engine::ecs::Collider>(entity)) {
        if (!collider->enabled) {
            return false;
        }

        if (collider->type == engine::ecs::ColliderType::Sphere) {
            engine::ecs::WorldColliderShape shape{};
            return engine::ecs::computeWorldColliderShape(world, entity, *collider, shape) &&
                   intersectRaySphere(origin, direction, shape.sphere, outDistance);
        }

        return intersectRayLocalAabb(
            origin,
            direction,
            worldMatrix,
            collider->offset,
            glm::max(collider->aabb.halfExtents, glm::vec3(0.001F)),
            outDistance);
    }

    if (meshRenderer != nullptr && meshRenderer->meshId == kSphereMeshId) {
        engine::ecs::WorldColliderShape shape{};
        return computeFallbackPickShape(world, entity, shape) &&
               intersectRaySphere(origin, direction, shape.sphere, outDistance);
    }

    const glm::vec3 localHalfExtents = meshRenderer != nullptr ? glm::vec3(0.5F) : glm::vec3(0.35F);
    return intersectRayLocalAabb(origin, direction, worldMatrix, glm::vec3(0.0F), localHalfExtents, outDistance);
}

bool computeFallbackPickShape(
    const engine::ecs::World& world,
    const EntityId entity,
    engine::ecs::WorldColliderShape& outShape) {
    if (!world.hasComponent<engine::ecs::Transform>(entity)) {
        return false;
    }

    const auto* meshRenderer = world.getComponent<engine::ecs::MeshRenderer>(entity);
    if (meshRenderer != nullptr && !meshRenderer->visible) {
        return false;
    }

    const glm::mat4 worldMatrix = engine::ecs::computeWorldMatrix(world, entity);
    const glm::vec3 worldCenter = engine::ecs::transformPoint(worldMatrix, glm::vec3(0.0F));

    outShape = engine::ecs::WorldColliderShape{};
    if (meshRenderer != nullptr && meshRenderer->meshId == kSphereMeshId) {
        const glm::vec3 worldScale = engine::ecs::extractWorldScale(worldMatrix);
        outShape.type = engine::ecs::ColliderType::Sphere;
        outShape.sphere.center = worldCenter;
        outShape.sphere.radius = 0.5F * std::max(worldScale.x, std::max(worldScale.y, worldScale.z));
        return outShape.sphere.radius > 0.0F;
    }

    const glm::vec3 localHalfExtents = meshRenderer != nullptr ? glm::vec3(0.5F) : glm::vec3(0.35F);
    const glm::vec3 worldHalfExtents = absoluteBasis(worldMatrix) * localHalfExtents;
    outShape.type = engine::ecs::ColliderType::Aabb;
    outShape.aabb.min = worldCenter - worldHalfExtents;
    outShape.aabb.max = worldCenter + worldHalfExtents;
    return true;
}

constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoCollapse;

ImGuiWindowFlags editorPanelFlags(const bool locked, const ImGuiWindowFlags extra = 0) {
    ImGuiWindowFlags flags = kPanelFlags | extra;
    if (locked) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    }
    return flags;
}

ImGuiCond editorLayoutCondition(const bool resetLayout) {
    return resetLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
}

using EditorDockSlot = engine::game::EditorDockSlot;
using EditorPanelId = engine::game::EditorPanelId;

bool isFloatingSlot(const EditorDockSlot slot) {
    return slot == EditorDockSlot::Floating;
}

std::string dockSlotToString(const EditorDockSlot slot) {
    switch (slot) {
        case EditorDockSlot::Top:
            return "top";
        case EditorDockSlot::Center:
            return "center";
        case EditorDockSlot::LeftTop:
            return "left_top";
        case EditorDockSlot::LeftBottom:
            return "left_bottom";
        case EditorDockSlot::RightTop:
            return "right_top";
        case EditorDockSlot::RightBottom:
            return "right_bottom";
        case EditorDockSlot::Bottom:
            return "bottom";
        case EditorDockSlot::Floating:
            return "floating";
    }
    return "floating";
}

std::optional<EditorDockSlot> dockSlotFromString(const std::string& value) {
    if (value == "top") {
        return EditorDockSlot::Top;
    }
    if (value == "center") {
        return EditorDockSlot::Center;
    }
    if (value == "left_top") {
        return EditorDockSlot::LeftTop;
    }
    if (value == "left_bottom") {
        return EditorDockSlot::LeftBottom;
    }
    if (value == "right_top") {
        return EditorDockSlot::RightTop;
    }
    if (value == "right_bottom") {
        return EditorDockSlot::RightBottom;
    }
    if (value == "bottom") {
        return EditorDockSlot::Bottom;
    }
    if (value == "floating") {
        return EditorDockSlot::Floating;
    }
    return std::nullopt;
}

const char* panelLabel(const EditorPanelId panelId) {
    switch (panelId) {
        case EditorPanelId::Toolbar:
            return "Toolbar";
        case EditorPanelId::Hierarchy:
            return "Scene Hierarchy";
        case EditorPanelId::Project:
            return "Content Browser";
        case EditorPanelId::Inspector:
            return "Inspector";
        case EditorPanelId::Stats:
            return "Statistics";
        case EditorPanelId::Viewport:
            return "Viewport";
        case EditorPanelId::Status:
            return "Status Bar";
    }
    return "Panel";
}

ImGuiWindowFlags dockedPanelChromeFlags(const EditorDockSlot slot) {
    return isFloatingSlot(slot) ? 0 : (ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
}

const char* dockSlotLabel(const EditorDockSlot slot) {
    switch (slot) {
        case EditorDockSlot::Top:
            return "Top Toolbar";
        case EditorDockSlot::Center:
            return "Center View";
        case EditorDockSlot::LeftTop:
            return "Left Top";
        case EditorDockSlot::LeftBottom:
            return "Left Bottom";
        case EditorDockSlot::RightTop:
            return "Right Top";
        case EditorDockSlot::RightBottom:
            return "Right Bottom";
        case EditorDockSlot::Bottom:
            return "Bottom Bar";
        case EditorDockSlot::Floating:
            return "Floating";
    }
    return "Unknown";
}

ImVec2 dockedPanelPosition(const EditorLayout& layout, const EditorDockSlot slot) {
    switch (slot) {
        case EditorDockSlot::Top:
            return layout.toolbarPos;
        case EditorDockSlot::Center:
            return layout.viewportPos;
        case EditorDockSlot::LeftTop:
            return layout.hierarchyPos;
        case EditorDockSlot::LeftBottom:
            return layout.projectPos;
        case EditorDockSlot::RightTop:
            return layout.inspectorPos;
        case EditorDockSlot::RightBottom:
            return layout.statsPos;
        case EditorDockSlot::Bottom:
            return layout.statusPos;
        case EditorDockSlot::Floating:
            return layout.viewportPos;
    }
    return layout.viewportPos;
}

ImVec2 dockedPanelSize(const EditorLayout& layout, const EditorDockSlot slot) {
    switch (slot) {
        case EditorDockSlot::Top:
            return layout.toolbarSize;
        case EditorDockSlot::Center:
            return layout.viewportSize;
        case EditorDockSlot::LeftTop:
            return layout.hierarchySize;
        case EditorDockSlot::LeftBottom:
            return layout.projectSize;
        case EditorDockSlot::RightTop:
            return layout.inspectorSize;
        case EditorDockSlot::RightBottom:
            return layout.statsSize;
        case EditorDockSlot::Bottom:
            return layout.statusSize;
        case EditorDockSlot::Floating:
            return ImVec2(520.0F, 360.0F);
    }
    return layout.viewportSize;
}

void placeEditorPanel(const EditorLayout& layout, const EditorDockSlot slot, const bool resetLayout) {
    const ImGuiCond condition = isFloatingSlot(slot) ? editorLayoutCondition(resetLayout) : ImGuiCond_Always;
    ImGui::SetNextWindowPos(dockedPanelPosition(layout, slot), condition);
    ImGui::SetNextWindowSize(dockedPanelSize(layout, slot), condition);
}

bool renderSplitterHandle(const char* id, const ImVec2& position, const ImVec2& size, const bool vertical) {
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoScrollWithMouse;

    bool active = false;
    if (ImGui::Begin(id, nullptr, flags)) {
        ImGui::InvisibleButton("##DockSplitter", size);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        active = ImGui::IsItemActive();
        if (hovered || active) {
            ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        }

        if (hovered || active) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 min = ImGui::GetWindowPos();
            const ImVec2 max(min.x + size.x, min.y + size.y);
            const ImU32 fill = ImGui::GetColorU32(
                active ? ImVec4(0.28F, 0.52F, 0.84F, 0.44F) : ImVec4(0.25F, 0.43F, 0.66F, 0.28F));
            const ImU32 line = ImGui::GetColorU32(
                active ? ImVec4(0.52F, 0.77F, 1.0F, 0.92F) : ImVec4(0.42F, 0.66F, 0.96F, 0.74F));
            drawList->AddRectFilled(min, max, fill, 4.0F);
            if (vertical) {
                const float centerX = min.x + size.x * 0.5F;
                drawList->AddLine(ImVec2(centerX, min.y + 8.0F), ImVec2(centerX, max.y - 8.0F), line, 1.5F);
            } else {
                const float centerY = min.y + size.y * 0.5F;
                drawList->AddLine(ImVec2(min.x + 8.0F, centerY), ImVec2(max.x - 8.0F, centerY), line, 1.5F);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    return active;
}

} // namespace

namespace engine::game {

GameplayState::GameplayState(StateStack& stack) : IGameState(stack) {}

void GameplayState::onEnter() {
    ENGINE_LOG_INFO("Entered GameplayState editor scene");
    m_eventBus.clear();
    loadEditorLayout();
    bindCollisionEventHandlers();
    resetDemoScene();
}

void GameplayState::onExit() {
    ENGINE_LOG_INFO("Exited GameplayState editor scene");
    saveEditorLayout();
    setCameraLookActive(false);
    m_pendingSpawnJobs.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_editModeSnapshot.reset();
    m_pendingSceneEdit.reset();
    m_gizmoEditSnapshot.reset();
    m_eventBus.clear();
    m_physicsSystem.clear();
    m_world.clear();
    m_projectAssets.clear();
    m_projectAssetsDirty = true;
    m_sceneInitialized = false;
    m_simulationRunning = false;
    m_selectedEntity = ecs::kInvalidEntity;
    m_hoveredEntity = ecs::kInvalidEntity;
    m_lastHoverPickMouse = glm::vec2(-100000.0F);
    m_hoverPickFrame = 0U;
    m_cameraEntity = ecs::kInvalidEntity;
    m_strikerEntity = ecs::kInvalidEntity;
    m_showcaseSphereEntity = ecs::kInvalidEntity;
    m_showcaseSphereLaunched = false;
    m_gizmoWasUsing = false;
    m_gizmoChanged = false;
}

void GameplayState::handleEvent(const platform::Event& event) {
    if (event.type == platform::EventType::MouseButtonPressed && event.mouseButton == platform::MouseButton::Right) {
        if (m_viewportHovered) {
            setCameraLookActive(true);
        }
        return;
    }

    if (event.type == platform::EventType::MouseButtonReleased && event.mouseButton == platform::MouseButton::Right) {
        setCameraLookActive(false);
        return;
    }

    if (event.type != platform::EventType::KeyPressed || event.repeat) {
        return;
    }

    const bool wantsTextInput = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput;

    if (!wantsTextInput && event.key == platform::KeyCode::Delete) {
        deleteEntity(m_selectedEntity);
        return;
    }

    if (event.key == platform::KeyCode::Escape) {
        if (m_cameraLookActive) {
            setCameraLookActive(false);
        } else if (m_editorMode == EditorMode::Play) {
            stopPlayMode();
        }
        return;
    }

    if (event.key == platform::KeyCode::F1) {
        m_showColliderDebug = !m_showColliderDebug;
        return;
    }

    if (event.key == platform::KeyCode::Space) {
        if (m_editorMode == EditorMode::Edit) {
            enterPlayMode();
        } else {
            launchStriker();
        }
        return;
    }

    if (!(m_viewportHovered || m_viewportFocused) || m_cameraLookActive) {
        return;
    }

    if (event.key == platform::KeyCode::W) {
        m_gizmoOperation = GizmoOperation::Translate;
    } else if (event.key == platform::KeyCode::E) {
        m_gizmoOperation = GizmoOperation::Rotate;
    } else if (event.key == platform::KeyCode::F) {
        focusSelectedEntity();
    } else if (event.key == platform::KeyCode::R) {
        m_gizmoOperation = GizmoOperation::Scale;
    }
}

void GameplayState::bindCollisionEventHandlers() {
    m_eventBus.subscribe<ecs::CollisionEnterEvent>([this](const ecs::CollisionEnterEvent& event) {
        ++m_collisionEnterCount;
        m_lastCollisionMessage = "Enter: " + std::to_string(event.contact.entityA) + " <-> " +
                                 std::to_string(event.contact.entityB) + " depth=" +
                                 std::to_string(event.contact.penetrationDepth);
    });
    m_eventBus.subscribe<ecs::CollisionStayEvent>([this](const ecs::CollisionStayEvent&) {
        ++m_collisionStayCount;
    });
    m_eventBus.subscribe<ecs::CollisionExitEvent>([this](const ecs::CollisionExitEvent& event) {
        ++m_collisionExitCount;
        m_lastCollisionMessage =
            "Exit: " + std::to_string(event.contact.entityA) + " <-> " + std::to_string(event.contact.entityB);
    });
}

ecs::EntityId GameplayState::spawnStaticBody(
    const std::string& tag,
    const glm::vec3& position,
    const glm::vec3& scale,
    const glm::vec4& tint,
    const std::string& textureId,
    const glm::vec2& uvScale) {
    const ecs::EntityId entity = m_world.createEntity();

    ecs::Transform transform{};
    transform.position = position;
    transform.scale = scale;

    ecs::MeshRenderer meshRenderer{};
    meshRenderer.meshId = kCubeMeshId;
    meshRenderer.shaderId = kShaderId;
    meshRenderer.textureId = textureId;
    meshRenderer.tint = tint;
    meshRenderer.uvScale = uvScale;

    ecs::Rigidbody rigidbody{};
    rigidbody.isStatic = true;
    rigidbody.useGravity = false;
    rigidbody.friction = 0.92F;
    rigidbody.recalculateMassProperties();

    ecs::Collider collider{};
    collider.type = ecs::ColliderType::Aabb;
    collider.aabb.halfExtents = glm::vec3(0.5F);

    m_world.addComponent<ecs::Transform>(entity, transform);
    m_world.addComponent<ecs::MeshRenderer>(entity, meshRenderer);
    m_world.addComponent<ecs::Rigidbody>(entity, rigidbody);
    m_world.addComponent<ecs::Collider>(entity, collider);
    m_world.addComponent<ecs::Tag>(entity, ecs::Tag{tag});
    return entity;
}

ecs::EntityId GameplayState::spawnDynamicBody(
    const std::string& tag,
    const glm::vec3& position,
    const glm::vec3& scale,
    const glm::vec4& tint,
    const float mass,
    const bool pyramidMesh,
    const std::string& textureId,
    const float restitution,
    const float friction,
    const glm::vec2& uvScale) {
    const ecs::EntityId entity = m_world.createEntity();

    ecs::Transform transform{};
    transform.position = position;
    transform.scale = scale;

    ecs::MeshRenderer meshRenderer{};
    meshRenderer.meshId = pyramidMesh ? kPyramidMeshId : kCubeMeshId;
    meshRenderer.shaderId = kShaderId;
    meshRenderer.textureId = textureId;
    meshRenderer.tint = tint;
    meshRenderer.uvScale = uvScale;

    ecs::Rigidbody rigidbody{};
    rigidbody.mass = mass;
    rigidbody.linearDamping = pyramidMesh ? 0.18F : 0.14F;
    rigidbody.angularDamping = pyramidMesh ? 0.78F : 0.62F;
    rigidbody.restitution = restitution;
    rigidbody.friction = friction;
    rigidbody.recalculateMassProperties();

    ecs::Collider collider{};
    collider.type = ecs::ColliderType::Aabb;
    collider.aabb.halfExtents = glm::vec3(0.5F);

    m_world.addComponent<ecs::Transform>(entity, transform);
    m_world.addComponent<ecs::MeshRenderer>(entity, meshRenderer);
    m_world.addComponent<ecs::Rigidbody>(entity, rigidbody);
    m_world.addComponent<ecs::Collider>(entity, collider);
    m_world.addComponent<ecs::Tag>(entity, ecs::Tag{tag});
    return entity;
}

ecs::EntityId GameplayState::spawnDynamicSphere(
    const std::string& tag,
    const glm::vec3& position,
    const float diameter,
    const glm::vec4& tint,
    const float mass,
    const float restitution,
    const float friction) {
    const ecs::EntityId entity = m_world.createEntity();

    ecs::Transform transform{};
    transform.position = position;
    transform.scale = glm::vec3(diameter);

    ecs::MeshRenderer meshRenderer{};
    meshRenderer.meshId = kSphereMeshId;
    meshRenderer.shaderId = kShaderId;
    meshRenderer.tint = tint;

    ecs::Rigidbody rigidbody{};
    rigidbody.mass = mass;
    rigidbody.linearDamping = 0.03F;
    rigidbody.angularDamping = 0.08F;
    rigidbody.restitution = restitution;
    rigidbody.friction = friction;
    rigidbody.recalculateMassProperties();

    ecs::Collider collider{};
    collider.type = ecs::ColliderType::Sphere;
    collider.sphere.radius = 0.5F;

    m_world.addComponent<ecs::Transform>(entity, transform);
    m_world.addComponent<ecs::MeshRenderer>(entity, meshRenderer);
    m_world.addComponent<ecs::Rigidbody>(entity, rigidbody);
    m_world.addComponent<ecs::Collider>(entity, collider);
    m_world.addComponent<ecs::Tag>(entity, ecs::Tag{tag});
    return entity;
}

void GameplayState::createDemoScene() {
    if (m_sceneInitialized) {
        return;
    }

    m_cameraEntity = m_world.createEntity();
    ecs::Transform cameraTransform{};
    cameraTransform.position = kCameraStartPosition;
    cameraTransform.rotationEulerRadians = kCameraStartRotation;
    ecs::Camera camera{};
    camera.verticalFovRadians = glm::radians(52.0F);
    camera.nearPlane = 0.1F;
    camera.farPlane = 220.0F;
    m_world.addComponent<ecs::Transform>(m_cameraEntity, cameraTransform);
    m_world.addComponent<ecs::Camera>(m_cameraEntity, camera);
    m_world.addComponent<ecs::Tag>(m_cameraEntity, ecs::Tag{"editor_camera"});

    spawnStaticBody(
        "floor",
        kFloorPosition,
        kFloorScale,
        glm::vec4(0.96F, 0.98F, 1.0F, 1.0F),
        kArenaFloorTextureId,
        glm::vec2(8.0F, 6.0F));
    spawnStaticBody(
        "wall_left",
        glm::vec3(-14.3F, -1.1F, 10.0F),
        glm::vec3(0.7F, 4.0F, 20.0F),
        glm::vec4(0.60F, 0.64F, 0.72F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(6.0F, 2.0F));
    spawnStaticBody(
        "wall_right",
        glm::vec3(14.3F, -1.1F, 10.0F),
        glm::vec3(0.7F, 4.0F, 20.0F),
        glm::vec4(0.60F, 0.64F, 0.72F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(6.0F, 2.0F));
    spawnStaticBody(
        "wall_back",
        glm::vec3(0.0F, -1.1F, 19.7F),
        glm::vec3(28.0F, 4.0F, 0.7F),
        glm::vec4(0.58F, 0.62F, 0.70F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(7.0F, 2.0F));
    spawnStaticBody(
        "pillar_left",
        glm::vec3(-5.8F, 0.2F, 10.0F),
        glm::vec3(1.4F, 5.0F, 1.4F),
        glm::vec4(0.68F, 0.72F, 0.79F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(2.0F, 5.0F));
    spawnStaticBody(
        "pillar_right",
        glm::vec3(5.8F, 0.2F, 10.0F),
        glm::vec3(1.4F, 5.0F, 1.4F),
        glm::vec4(0.68F, 0.72F, 0.79F, 1.0F),
        kArenaWallTextureId,
        glm::vec2(2.0F, 5.0F));

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4 - row; ++column) {
            const float x = -1.2F * static_cast<float>(3 - row) + static_cast<float>(column) * 2.4F;
            const float y = -1.35F + static_cast<float>(row) * 1.18F;
            spawnDynamicBody(
                "stack_" + std::to_string(row) + "_" + std::to_string(column),
                glm::vec3(x, y, 13.0F),
                glm::vec3(1.1F, 1.1F, 1.1F),
                paletteColor(static_cast<std::size_t>(row * 4 + column)),
                0.95F + static_cast<float>(row) * 0.08F);
        }
    }

    spawnDynamicBody(
        "accent_pyramid_left",
        glm::vec3(-3.2F, -1.25F, 7.0F),
        glm::vec3(1.2F, 1.2F, 1.2F),
        pyramidAccentColor(0),
        0.78F,
        true,
        "",
        0.1F,
        0.58F,
        glm::vec2(1.4F, 1.4F));
    spawnDynamicBody(
        "accent_pyramid_mid",
        glm::vec3(0.0F, -1.25F, 7.8F),
        glm::vec3(1.05F, 1.05F, 1.05F),
        pyramidAccentColor(1),
        0.74F,
        true,
        "",
        0.1F,
        0.58F,
        glm::vec2(1.3F, 1.3F));
    spawnDynamicBody(
        "accent_pyramid_right",
        glm::vec3(3.2F, -1.25F, 7.0F),
        glm::vec3(1.2F, 1.2F, 1.2F),
        pyramidAccentColor(2),
        0.78F,
        true,
        "",
        0.1F,
        0.58F,
        glm::vec2(1.4F, 1.4F));

    m_showcaseSphereEntity = spawnDynamicSphere(
        "showcase_sphere",
        kShowcaseSphereStartPosition,
        1.35F,
        glm::vec4(0.92F, 0.38F, 0.16F, 1.0F),
        2.4F,
        0.03F,
        0.95F);
    m_strikerEntity = spawnDynamicBody(
        "striker",
        kStrikerStartPosition,
        kStrikerScale,
        glm::vec4(1.0F, 1.0F, 1.0F, 1.0F),
        8.0F,
        false,
        kLogoTextureId,
        0.08F,
        0.42F,
        glm::vec2(1.0F, 1.0F));

    m_sceneInitialized = true;
    m_selectedEntity = m_strikerEntity;
    m_lastCollisionMessage = "Editor scene ready.";
}

void GameplayState::resetDemoScene() {
    m_world.clear();
    m_physicsSystem.clear();
    m_cameraEntity = ecs::kInvalidEntity;
    m_strikerEntity = ecs::kInvalidEntity;
    m_showcaseSphereEntity = ecs::kInvalidEntity;
    m_selectedEntity = ecs::kInvalidEntity;
    m_hoveredEntity = ecs::kInvalidEntity;
    m_lastHoverPickMouse = glm::vec2(-100000.0F);
    m_hoverPickFrame = 0U;
    m_sceneInitialized = false;
    m_editorMode = EditorMode::Edit;
    m_simulationRunning = false;
    m_pendingSpawnJobs.clear();
    m_editModeSnapshot.reset();
    m_pendingSceneEdit.reset();
    m_gizmoEditSnapshot.reset();
    m_spawnSequence = 0;
    m_collisionEnterCount = 0;
    m_collisionStayCount = 0;
    m_collisionExitCount = 0;
    m_showcaseSphereLaunched = false;
    m_showColliderDebug = false;
    m_gizmoWasUsing = false;
    m_gizmoChanged = false;
    setCameraLookActive(false);
    createDemoScene();
    m_editorStatusMessage = "Demo scene reset.";
}

void GameplayState::startSimulation(const bool relaunchStriker) {
    if (!m_sceneInitialized) {
        createDemoScene();
    }

    m_simulationRunning = true;
    m_lastCollisionMessage = "Simulation running.";

    if (!m_showcaseSphereLaunched) {
        if (auto* sphereBody = m_world.getComponent<ecs::Rigidbody>(m_showcaseSphereEntity)) {
            sphereBody->velocity = kShowcaseSphereLaunchVelocity;
            sphereBody->angularVelocity = glm::vec3(0.0F);
            sphereBody->acceleration = glm::vec3(0.0F);
            sphereBody->accumulatedForce = glm::vec3(0.0F);
            sphereBody->accumulatedTorque = glm::vec3(0.0F);
            sphereBody->wakeUp();
            m_showcaseSphereLaunched = true;
        }
    }

    if (relaunchStriker) {
        launchStriker();
    }
}

void GameplayState::pauseSimulation() {
    m_simulationRunning = false;
    m_lastCollisionMessage = "Simulation paused.";
}

void GameplayState::launchStriker() {
    auto* strikerTransform = m_world.getComponent<ecs::Transform>(m_strikerEntity);
    auto* strikerBody = m_world.getComponent<ecs::Rigidbody>(m_strikerEntity);
    if (strikerTransform == nullptr || strikerBody == nullptr) {
        return;
    }

    strikerTransform->position = kStrikerStartPosition;
    strikerBody->velocity = kStrikerLaunchVelocity;
    strikerBody->acceleration = glm::vec3(0.0F);
    strikerBody->accumulatedForce = glm::vec3(0.0F);
    strikerBody->angularVelocity = glm::vec3(0.0F);
    strikerBody->accumulatedTorque = glm::vec3(0.0F);
    strikerBody->useGravity = true;
    strikerBody->wakeUp();
    m_simulationRunning = true;
    m_lastCollisionMessage = "Striker launched.";
}

void GameplayState::spawnDropWave() {
    const std::uint32_t waveIndex = m_spawnSequence++;
    const float baseHeight = 7.2F + static_cast<float>(waveIndex % 3U) * 0.8F;
    const float zBase = 7.0F + static_cast<float>(waveIndex % 3U) * 1.1F;

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 5; ++column) {
            const int objectIndex = row * 5 + column;
            const float x = -4.8F + static_cast<float>(column) * 2.4F;
            const float y = baseHeight + static_cast<float>(row) * 1.3F + static_cast<float>(column % 2) * 0.25F;
            const float z = zBase + static_cast<float>(row) * 2.8F;
            const glm::vec3 scale(0.9F + 0.12F * static_cast<float>((objectIndex + row) % 3), 0.9F, 0.9F);
            const bool pyramidMesh = ((waveIndex + static_cast<std::uint32_t>(objectIndex)) % 4U) == 0U;
            const bool sphereMesh = ((waveIndex + static_cast<std::uint32_t>(objectIndex)) % 7U) == 3U;
            const std::string tag = "wave_" + std::to_string(waveIndex) + "_" + std::to_string(objectIndex);
            const glm::vec3 position(x, y, z);
            const glm::vec4 tint =
                pyramidMesh ? pyramidAccentColor(static_cast<std::size_t>(waveIndex + static_cast<std::uint32_t>(objectIndex)))
                            : paletteColor(static_cast<std::size_t>(waveIndex + static_cast<std::uint32_t>(objectIndex)));
            const float mass = 0.7F + static_cast<float>(objectIndex % 5) * 0.12F;

            m_pendingSpawnJobs.push_back([=, this]() {
                if (sphereMesh) {
                    spawnDynamicSphere(tag, position, scale.x * 1.2F, tint, mass, 0.03F, 0.9F);
                    return;
                }

                spawnDynamicBody(
                    tag,
                    position,
                    scale,
                    tint,
                    mass,
                    pyramidMesh,
                    "",
                    0.08F,
                    0.72F,
                    pyramidMesh ? glm::vec2(1.35F, 1.35F) : glm::vec2(1.0F, 1.0F));
            });
        }
    }

    m_simulationRunning = true;
    m_lastCollisionMessage = "Drop wave queued.";
}

void GameplayState::processPendingSpawns() {
    std::size_t spawnedThisFrame = 0;
    while (!m_pendingSpawnJobs.empty() && spawnedThisFrame < kSpawnBudgetPerFrame) {
        std::function<void()> spawnJob = std::move(m_pendingSpawnJobs.front());
        m_pendingSpawnJobs.pop_front();
        if (spawnJob) {
            spawnJob();
        }
        ++spawnedThisFrame;
    }
}

void GameplayState::enterPlayMode() {
    if (m_editorMode == EditorMode::Play) {
        return;
    }

    m_editModeSnapshot = captureSceneSnapshot();
    m_editorMode = EditorMode::Play;
    startSimulation();
}

void GameplayState::stopPlayMode() {
    if (m_editorMode == EditorMode::Edit) {
        return;
    }

    if (m_editModeSnapshot.has_value()) {
        restoreSceneSnapshot(*m_editModeSnapshot);
        m_editModeSnapshot.reset();
    }
    m_editorMode = EditorMode::Edit;
    setCameraLookActive(false);
    m_lastCollisionMessage = "Returned to Edit mode.";
}

GameplayState::SceneSnapshot GameplayState::captureSceneSnapshot() const {
    SceneSnapshot snapshot{};
    snapshot.cameraEntity = m_cameraEntity;
    snapshot.strikerEntity = m_strikerEntity;
    snapshot.showcaseSphereEntity = m_showcaseSphereEntity;
    snapshot.selectedEntity = m_selectedEntity;
    snapshot.spawnSequence = m_spawnSequence;
    snapshot.showColliderDebug = m_showColliderDebug;
    snapshot.showcaseSphereLaunched = m_showcaseSphereLaunched;

    m_world.forEachEntity([&](const ecs::EntityId entity) {
        EntitySnapshot entitySnapshot{};
        entitySnapshot.sourceId = entity;
        if (const auto* transform = m_world.getComponent<ecs::Transform>(entity)) {
            entitySnapshot.transform = *transform;
        }
        if (const auto* tag = m_world.getComponent<ecs::Tag>(entity)) {
            entitySnapshot.tag = *tag;
        }
        if (const auto* hierarchy = m_world.getComponent<ecs::Hierarchy>(entity)) {
            entitySnapshot.hierarchy = *hierarchy;
        }
        if (const auto* camera = m_world.getComponent<ecs::Camera>(entity)) {
            entitySnapshot.camera = *camera;
        }
        if (const auto* rigidbody = m_world.getComponent<ecs::Rigidbody>(entity)) {
            entitySnapshot.rigidbody = *rigidbody;
        }
        if (const auto* collider = m_world.getComponent<ecs::Collider>(entity)) {
            entitySnapshot.collider = *collider;
        }
        if (const auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(entity)) {
            entitySnapshot.meshRenderer = *meshRenderer;
        }
        snapshot.entities.push_back(std::move(entitySnapshot));
    });

    return snapshot;
}

void GameplayState::restoreSceneSnapshot(const SceneSnapshot& snapshot) {
    m_world.clear();
    m_physicsSystem.clear();
    m_pendingSpawnJobs.clear();
    m_pendingSceneEdit.reset();
    m_gizmoEditSnapshot.reset();
    m_sceneInitialized = false;
    m_simulationRunning = false;
    m_gizmoWasUsing = false;
    m_gizmoChanged = false;

    std::unordered_map<ecs::EntityId, ecs::EntityId> entityMap;
    entityMap.reserve(snapshot.entities.size());

    for (const auto& entitySnapshot : snapshot.entities) {
        const ecs::EntityId recreated = m_world.createEntity();
        entityMap.emplace(entitySnapshot.sourceId, recreated);
    }

    for (const auto& entitySnapshot : snapshot.entities) {
        const ecs::EntityId entity = entityMap.at(entitySnapshot.sourceId);
        if (entitySnapshot.transform.has_value()) {
            m_world.addComponent<ecs::Transform>(entity, *entitySnapshot.transform);
        }
        if (entitySnapshot.tag.has_value()) {
            m_world.addComponent<ecs::Tag>(entity, *entitySnapshot.tag);
        }
        if (entitySnapshot.hierarchy.has_value()) {
            ecs::Hierarchy hierarchy = *entitySnapshot.hierarchy;
            if (hierarchy.parent != ecs::kInvalidEntity) {
                const auto remapped = entityMap.find(hierarchy.parent);
                hierarchy.parent = remapped != entityMap.end() ? remapped->second : ecs::kInvalidEntity;
            }
            m_world.addComponent<ecs::Hierarchy>(entity, hierarchy);
        }
        if (entitySnapshot.camera.has_value()) {
            m_world.addComponent<ecs::Camera>(entity, *entitySnapshot.camera);
        }
        if (entitySnapshot.rigidbody.has_value()) {
            m_world.addComponent<ecs::Rigidbody>(entity, *entitySnapshot.rigidbody);
        }
        if (entitySnapshot.collider.has_value()) {
            m_world.addComponent<ecs::Collider>(entity, *entitySnapshot.collider);
        }
        if (entitySnapshot.meshRenderer.has_value()) {
            m_world.addComponent<ecs::MeshRenderer>(entity, *entitySnapshot.meshRenderer);
        }
    }

    const auto remapEntity = [&](const ecs::EntityId source) {
        if (source == ecs::kInvalidEntity) {
            return ecs::kInvalidEntity;
        }
        const auto it = entityMap.find(source);
        return it != entityMap.end() ? it->second : ecs::kInvalidEntity;
    };

    m_cameraEntity = remapEntity(snapshot.cameraEntity);
    m_strikerEntity = remapEntity(snapshot.strikerEntity);
    m_showcaseSphereEntity = remapEntity(snapshot.showcaseSphereEntity);
    m_selectedEntity = remapEntity(snapshot.selectedEntity);
    m_spawnSequence = snapshot.spawnSequence;
    m_showColliderDebug = snapshot.showColliderDebug;
    m_showcaseSphereLaunched = snapshot.showcaseSphereLaunched;
    m_collisionEnterCount = 0;
    m_collisionStayCount = 0;
    m_collisionExitCount = 0;
    m_sceneInitialized = true;
    ensureSelectedEntityValid();
}

bool GameplayState::canEditScene() const {
    return m_editorMode == EditorMode::Edit;
}

bool GameplayState::hasUndo() const {
    return !m_undoStack.empty();
}

bool GameplayState::hasRedo() const {
    return !m_redoStack.empty();
}

bool GameplayState::computeEntityPickShape(const ecs::EntityId entity, ecs::WorldColliderShape& outShape) const {
    if (!m_world.isAlive(entity) || !m_world.hasComponent<ecs::Transform>(entity)) {
        return false;
    }

    if (const auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(entity);
        meshRenderer != nullptr && !meshRenderer->visible) {
        return false;
    }

    if (const auto* collider = m_world.getComponent<ecs::Collider>(entity)) {
        if (ecs::computeWorldColliderShape(m_world, entity, *collider, outShape)) {
            return true;
        }
    }

    return computeFallbackPickShape(m_world, entity, outShape);
}

void GameplayState::pushHistorySnapshot(const std::string& label, SceneSnapshot snapshot) {
    if (!canEditScene()) {
        return;
    }

    if (m_undoStack.size() >= kMaxEditorHistory) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_undoStack.push_back(EditorHistoryEntry{label, std::move(snapshot)});
    m_redoStack.clear();
}

void GameplayState::recordSceneHistory(const std::string& label) {
    if (m_pendingSceneEdit.has_value()) {
        commitSceneEdit();
    }
    pushHistorySnapshot(label, captureSceneSnapshot());
}

void GameplayState::beginSceneEdit(const std::string& label, const SceneSnapshot& before) {
    if (!canEditScene() || m_pendingSceneEdit.has_value()) {
        return;
    }
    m_pendingSceneEdit = PendingSceneEdit{label, before, false};
}

void GameplayState::trackEditedItem(const std::string& label, const bool changed, const SceneSnapshot& before) {
    if (!canEditScene()) {
        return;
    }

    if (ImGui::IsItemActivated()) {
        beginSceneEdit(label, before);
    }

    if (changed) {
        if (m_pendingSceneEdit.has_value()) {
            m_pendingSceneEdit->changed = true;
        } else {
            pushHistorySnapshot(label, before);
            m_editorStatusMessage = label;
        }
    }

    if (ImGui::IsItemDeactivatedAfterEdit()) {
        commitSceneEdit();
    }
}

void GameplayState::commitSceneEdit() {
    if (!m_pendingSceneEdit.has_value()) {
        return;
    }

    if (m_pendingSceneEdit->changed) {
        const std::string label = m_pendingSceneEdit->label;
        pushHistorySnapshot(label, std::move(m_pendingSceneEdit->before));
        m_editorStatusMessage = label;
    }
    m_pendingSceneEdit.reset();
}

void GameplayState::cancelSceneEdit() {
    m_pendingSceneEdit.reset();
    m_gizmoEditSnapshot.reset();
    m_gizmoWasUsing = false;
    m_gizmoChanged = false;
}

void GameplayState::undoSceneEdit() {
    if (!canEditScene() || m_undoStack.empty()) {
        return;
    }

    cancelSceneEdit();
    EditorHistoryEntry entry = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    m_redoStack.push_back(EditorHistoryEntry{entry.label, captureSceneSnapshot()});
    restoreSceneSnapshot(entry.snapshot);
    m_editorStatusMessage = "Undo: " + entry.label;
}

void GameplayState::redoSceneEdit() {
    if (!canEditScene() || m_redoStack.empty()) {
        return;
    }

    cancelSceneEdit();
    EditorHistoryEntry entry = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    if (m_undoStack.size() >= kMaxEditorHistory) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_undoStack.push_back(EditorHistoryEntry{entry.label, captureSceneSnapshot()});
    restoreSceneSnapshot(entry.snapshot);
    m_editorStatusMessage = "Redo: " + entry.label;
}

void GameplayState::saveSceneToDisk() {
    if (!canEditScene()) {
        m_editorStatusMessage = "Stop Play mode before saving.";
        return;
    }

    commitSceneEdit();
    const SceneSnapshot snapshot = captureSceneSnapshot();

    nlohmann::json sceneJson;
    sceneJson["version"] = 1;
    sceneJson["cameraEntity"] = snapshot.cameraEntity;
    sceneJson["strikerEntity"] = snapshot.strikerEntity;
    sceneJson["showcaseSphereEntity"] = snapshot.showcaseSphereEntity;
    sceneJson["selectedEntity"] = snapshot.selectedEntity;
    sceneJson["spawnSequence"] = snapshot.spawnSequence;
    sceneJson["showColliderDebug"] = snapshot.showColliderDebug;
    sceneJson["showcaseSphereLaunched"] = snapshot.showcaseSphereLaunched;
    sceneJson["entities"] = nlohmann::json::array();

    for (const EntitySnapshot& entitySnapshot : snapshot.entities) {
        nlohmann::json entityJson;
        entityJson["sourceId"] = entitySnapshot.sourceId;
        if (entitySnapshot.transform.has_value()) {
            entityJson["transform"] = transformToJson(*entitySnapshot.transform);
        }
        if (entitySnapshot.tag.has_value()) {
            entityJson["tag"] = nlohmann::json{{"value", entitySnapshot.tag->value}};
        }
        if (entitySnapshot.hierarchy.has_value()) {
            entityJson["hierarchy"] = nlohmann::json{{"parent", entitySnapshot.hierarchy->parent}};
        }
        if (entitySnapshot.camera.has_value()) {
            entityJson["camera"] = cameraToJson(*entitySnapshot.camera);
        }
        if (entitySnapshot.rigidbody.has_value()) {
            entityJson["rigidbody"] = rigidbodyToJson(*entitySnapshot.rigidbody);
        }
        if (entitySnapshot.collider.has_value()) {
            entityJson["collider"] = colliderToJson(*entitySnapshot.collider);
        }
        if (entitySnapshot.meshRenderer.has_value()) {
            entityJson["meshRenderer"] = meshRendererToJson(*entitySnapshot.meshRenderer);
        }
        sceneJson["entities"].push_back(std::move(entityJson));
    }

    const std::filesystem::path scenePath = editorScenePath();
    try {
        if (scenePath.has_parent_path()) {
            std::filesystem::create_directories(scenePath.parent_path());
        }

        std::ofstream file(scenePath);
        if (!file.is_open()) {
            m_editorStatusMessage = "Failed to save scene.";
            ENGINE_LOG_WARN("Failed to open scene '{}' for writing", scenePath.string());
            return;
        }

        file << sceneJson.dump(4);
        m_projectAssetsDirty = true;
        m_editorStatusMessage = "Saved scene to " + scenePath.string();
        ENGINE_LOG_INFO("Saved editor scene to '{}'", scenePath.string());
    } catch (const std::exception& ex) {
        m_editorStatusMessage = "Failed to save scene.";
        ENGINE_LOG_WARN("Failed saving scene '{}': {}", scenePath.string(), ex.what());
    }
}

void GameplayState::loadSceneFromDisk() {
    if (!canEditScene()) {
        m_editorStatusMessage = "Stop Play mode before loading.";
        return;
    }

    const std::filesystem::path scenePath = editorScenePath();
    if (!std::filesystem::exists(scenePath)) {
        m_editorStatusMessage = "Scene file not found.";
        ENGINE_LOG_WARN("Scene file '{}' not found", scenePath.string());
        return;
    }

    try {
        std::ifstream file(scenePath);
        if (!file.is_open()) {
            m_editorStatusMessage = "Failed to load scene.";
            ENGINE_LOG_WARN("Failed to open scene '{}' for reading", scenePath.string());
            return;
        }

        nlohmann::json sceneJson;
        file >> sceneJson;

        SceneSnapshot snapshot{};
        snapshot.cameraEntity = sceneJson.value("cameraEntity", ecs::kInvalidEntity);
        snapshot.strikerEntity = sceneJson.value("strikerEntity", ecs::kInvalidEntity);
        snapshot.showcaseSphereEntity = sceneJson.value("showcaseSphereEntity", ecs::kInvalidEntity);
        snapshot.selectedEntity = sceneJson.value("selectedEntity", ecs::kInvalidEntity);
        snapshot.spawnSequence = sceneJson.value("spawnSequence", 0U);
        snapshot.showColliderDebug = sceneJson.value("showColliderDebug", false);
        snapshot.showcaseSphereLaunched = sceneJson.value("showcaseSphereLaunched", false);

        for (const auto& entityJson : sceneJson.value("entities", nlohmann::json::array())) {
            EntitySnapshot entitySnapshot{};
            entitySnapshot.sourceId = entityJson.value("sourceId", ecs::kInvalidEntity);
            if (entitySnapshot.sourceId == ecs::kInvalidEntity) {
                continue;
            }
            if (entityJson.contains("transform")) {
                entitySnapshot.transform = transformFromJson(entityJson["transform"]);
            }
            if (entityJson.contains("tag")) {
                entitySnapshot.tag = ecs::Tag{entityJson["tag"].value("value", std::string{})};
            }
            if (entityJson.contains("hierarchy")) {
                entitySnapshot.hierarchy = ecs::Hierarchy{entityJson["hierarchy"].value("parent", ecs::kInvalidEntity)};
            }
            if (entityJson.contains("camera")) {
                entitySnapshot.camera = cameraFromJson(entityJson["camera"]);
            }
            if (entityJson.contains("rigidbody")) {
                entitySnapshot.rigidbody = rigidbodyFromJson(entityJson["rigidbody"]);
            }
            if (entityJson.contains("collider")) {
                entitySnapshot.collider = colliderFromJson(entityJson["collider"]);
            }
            if (entityJson.contains("meshRenderer")) {
                entitySnapshot.meshRenderer = meshRendererFromJson(entityJson["meshRenderer"]);
            }
            snapshot.entities.push_back(std::move(entitySnapshot));
        }

        if (snapshot.entities.empty()) {
            m_editorStatusMessage = "Scene file has no entities.";
            return;
        }

        if (canEditScene()) {
            recordSceneHistory("Load Scene");
        }
        restoreSceneSnapshot(snapshot);
        m_editorStatusMessage = "Loaded scene from " + scenePath.string();
        ENGINE_LOG_INFO("Loaded editor scene from '{}'", scenePath.string());
    } catch (const std::exception& ex) {
        m_editorStatusMessage = "Failed to load scene.";
        ENGINE_LOG_WARN("Failed loading scene '{}': {}", scenePath.string(), ex.what());
    }
}

glm::vec3 GameplayState::editorSpawnPosition() const {
    if (const auto* cameraTransform = m_world.getComponent<ecs::Transform>(m_cameraEntity)) {
        return cameraTransform->position + ecs::forwardFromEuler(cameraTransform->rotationEulerRadians) * 6.0F;
    }
    return glm::vec3(0.0F, 0.0F, 6.0F);
}

void GameplayState::focusSelectedEntity() {
    if (m_selectedEntity == ecs::kInvalidEntity || !m_world.isAlive(m_selectedEntity)) {
        m_editorStatusMessage = "Nothing selected to focus.";
        return;
    }

    auto* cameraTransform = m_world.getComponent<ecs::Transform>(m_cameraEntity);
    if (cameraTransform == nullptr) {
        m_editorStatusMessage = "No active editor camera.";
        return;
    }

    ecs::WorldColliderShape shape{};
    glm::vec3 center(0.0F);
    float radius = 1.0F;
    if (computeEntityPickShape(m_selectedEntity, shape)) {
        const ecs::WorldAabb bounds = ecs::computeWorldBounds(shape);
        center = bounds.center();
        radius = std::max(1.0F, glm::length(bounds.extents()));
    } else if (const auto* selectedTransform = m_world.getComponent<ecs::Transform>(m_selectedEntity)) {
        center = ecs::transformPoint(ecs::computeWorldMatrix(m_world, m_selectedEntity), glm::vec3(0.0F));
        radius = std::max(1.0F, glm::length(selectedTransform->scale) * 0.5F);
    } else {
        m_editorStatusMessage = "Selected entity has no transform.";
        return;
    }

    const glm::vec3 forward = ecs::forwardFromEuler(cameraTransform->rotationEulerRadians);
    const float distance = std::clamp(radius * 3.2F, 4.0F, 42.0F);
    cameraTransform->position = center - forward * distance;
    m_editorStatusMessage = "Focused " + entityDisplayName(m_selectedEntity);
}

ecs::EntityId GameplayState::createEditorEntity(const EditorEntityKind kind) {
    if (!canEditScene()) {
        return ecs::kInvalidEntity;
    }

    recordSceneHistory("Create Entity");
    const glm::vec3 spawnPosition = editorSpawnPosition();
    ecs::EntityId entity = ecs::kInvalidEntity;

    switch (kind) {
        case EditorEntityKind::Empty: {
            entity = m_world.createEntity();
            ecs::Transform transform{};
            transform.position = spawnPosition;
            m_world.addComponent<ecs::Transform>(entity, transform);
            m_world.addComponent<ecs::Tag>(entity, ecs::Tag{"empty_" + std::to_string(entity)});
            break;
        }
        case EditorEntityKind::RenderCube: {
            entity = m_world.createEntity();
            ecs::Transform transform{};
            transform.position = spawnPosition;
            ecs::MeshRenderer meshRenderer{};
            meshRenderer.meshId = kCubeMeshId;
            meshRenderer.shaderId = kShaderId;
            meshRenderer.tint = glm::vec4(0.30F, 0.63F, 0.84F, 1.0F);
            m_world.addComponent<ecs::Transform>(entity, transform);
            m_world.addComponent<ecs::MeshRenderer>(entity, meshRenderer);
            m_world.addComponent<ecs::Tag>(entity, ecs::Tag{"render_cube_" + std::to_string(entity)});
            break;
        }
        case EditorEntityKind::PhysicsCube:
            entity = spawnDynamicBody(
                "physics_cube_" + std::to_string(m_spawnSequence++),
                spawnPosition,
                glm::vec3(1.0F),
                glm::vec4(0.86F, 0.55F, 0.30F, 1.0F),
                1.0F,
                false,
                "",
                0.08F,
                0.72F);
            break;
        case EditorEntityKind::PhysicsSphere:
            entity = spawnDynamicSphere(
                "physics_sphere_" + std::to_string(m_spawnSequence++),
                spawnPosition,
                1.0F,
                glm::vec4(0.34F, 0.72F, 0.64F, 1.0F),
                1.0F,
                0.05F,
                0.84F);
            break;
        case EditorEntityKind::PhysicsPyramid:
            entity = spawnDynamicBody(
                "physics_pyramid_" + std::to_string(m_spawnSequence++),
                spawnPosition,
                glm::vec3(1.0F),
                pyramidAccentColor(m_spawnSequence),
                0.85F,
                true,
                "",
                0.08F,
                0.68F,
                glm::vec2(1.25F));
            break;
        case EditorEntityKind::Camera: {
            entity = m_world.createEntity();
            ecs::Transform transform{};
            transform.position = spawnPosition;
            transform.rotationEulerRadians = kCameraStartRotation;
            ecs::Camera camera{};
            camera.active = false;
            camera.verticalFovRadians = glm::radians(52.0F);
            m_world.addComponent<ecs::Transform>(entity, transform);
            m_world.addComponent<ecs::Camera>(entity, camera);
            m_world.addComponent<ecs::Tag>(entity, ecs::Tag{"camera_" + std::to_string(entity)});
            break;
        }
    }

    m_selectedEntity = entity;
    m_editorStatusMessage = "Created " + entityDisplayName(entity);
    return entity;
}

ecs::EntityId GameplayState::duplicateEntity(const ecs::EntityId entity) {
    if (!canEditScene() || !m_world.isAlive(entity)) {
        return ecs::kInvalidEntity;
    }

    recordSceneHistory("Duplicate Entity");
    const ecs::EntityId copy = m_world.createEntity();

    if (const auto* transform = m_world.getComponent<ecs::Transform>(entity)) {
        ecs::Transform copiedTransform = *transform;
        copiedTransform.position += glm::vec3(1.25F, 0.0F, 1.25F);
        m_world.addComponent<ecs::Transform>(copy, copiedTransform);
    }
    if (const auto* tag = m_world.getComponent<ecs::Tag>(entity)) {
        m_world.addComponent<ecs::Tag>(copy, ecs::Tag{tag->value + "_copy"});
    } else {
        m_world.addComponent<ecs::Tag>(copy, ecs::Tag{"entity_" + std::to_string(copy)});
    }
    if (const auto* hierarchy = m_world.getComponent<ecs::Hierarchy>(entity)) {
        m_world.addComponent<ecs::Hierarchy>(copy, *hierarchy);
    }
    if (const auto* camera = m_world.getComponent<ecs::Camera>(entity)) {
        ecs::Camera copiedCamera = *camera;
        copiedCamera.active = false;
        m_world.addComponent<ecs::Camera>(copy, copiedCamera);
    }
    if (const auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(entity)) {
        m_world.addComponent<ecs::MeshRenderer>(copy, *meshRenderer);
    }
    if (const auto* collider = m_world.getComponent<ecs::Collider>(entity)) {
        m_world.addComponent<ecs::Collider>(copy, *collider);
    }
    if (const auto* rigidbody = m_world.getComponent<ecs::Rigidbody>(entity)) {
        ecs::Rigidbody copiedRigidbody = *rigidbody;
        copiedRigidbody.velocity = glm::vec3(0.0F);
        copiedRigidbody.acceleration = glm::vec3(0.0F);
        copiedRigidbody.accumulatedForce = glm::vec3(0.0F);
        copiedRigidbody.angularVelocity = glm::vec3(0.0F);
        copiedRigidbody.accumulatedTorque = glm::vec3(0.0F);
        copiedRigidbody.wakeUp();
        m_world.addComponent<ecs::Rigidbody>(copy, copiedRigidbody);
    }

    m_selectedEntity = copy;
    m_editorStatusMessage = "Duplicated " + entityDisplayName(entity);
    return copy;
}

void GameplayState::deleteEntity(const ecs::EntityId entity) {
    if (!canEditScene() || !m_world.isAlive(entity)) {
        return;
    }

    recordSceneHistory("Delete Entity");
    m_world.forEachEntity([&](const ecs::EntityId candidate) {
        if (auto* hierarchy = m_world.getComponent<ecs::Hierarchy>(candidate);
            hierarchy != nullptr && hierarchy->parent == entity) {
            hierarchy->parent = ecs::kInvalidEntity;
        }
    });

    const std::string deletedName = entityDisplayName(entity);
    m_world.destroyEntity(entity);
    if (m_cameraEntity == entity) {
        m_cameraEntity = ecs::kInvalidEntity;
    }
    if (m_strikerEntity == entity) {
        m_strikerEntity = ecs::kInvalidEntity;
    }
    if (m_showcaseSphereEntity == entity) {
        m_showcaseSphereEntity = ecs::kInvalidEntity;
    }
    if (m_selectedEntity == entity) {
        m_selectedEntity = ecs::kInvalidEntity;
    }
    ensureSelectedEntityValid();
    m_editorStatusMessage = "Deleted " + deletedName;
}

void GameplayState::setCameraLookActive(const bool active) {
    if (m_cameraLookActive == active) {
        return;
    }

    platform::InputManager::resetMouseDelta();
    m_cameraLookActive = active;
    m_skipNextCameraLookDelta = active;
    if (platform::Window* window = stack().window()) {
        window->setCursorMode(active ? platform::CursorMode::Disabled : platform::CursorMode::Normal);
    }
    platform::InputManager::resetMouseDelta();
}

void GameplayState::ensureSelectedEntityValid() {
    if (m_selectedEntity != ecs::kInvalidEntity && m_world.isAlive(m_selectedEntity)) {
        return;
    }

    m_selectedEntity = ecs::kInvalidEntity;
    m_world.forEachEntity([&](const ecs::EntityId entity) {
        if (m_selectedEntity == ecs::kInvalidEntity && entity != m_cameraEntity) {
            m_selectedEntity = entity;
        }
    });
}

void GameplayState::processEditorShortcuts() {
    const bool controlDown = platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftControl) ||
                             platform::InputManager::isKeyDownRaw(platform::KeyCode::RightControl);
    if (!controlDown) {
        return;
    }

    const bool shiftDown = platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftShift) ||
                           platform::InputManager::isKeyDownRaw(platform::KeyCode::RightShift);

    if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::S)) {
        saveSceneToDisk();
        return;
    }
    if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::O)) {
        loadSceneFromDisk();
        return;
    }
    if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::N)) {
        createEditorEntity(EditorEntityKind::Empty);
        return;
    }
    if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::D)) {
        duplicateEntity(m_selectedEntity);
        return;
    }
    if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::Y) ||
        (shiftDown && platform::InputManager::wasKeyPressedRaw(platform::KeyCode::Z))) {
        redoSceneEdit();
        return;
    }
    if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::Z)) {
        undoSceneEdit();
    }
}

void GameplayState::applyViewportHotkeys() {
    if (!(m_viewportHovered || m_viewportFocused) || ImGui::GetIO().WantTextInput || m_cameraLookActive) {
        return;
    }

    const bool controlDown = platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftControl) ||
                             platform::InputManager::isKeyDownRaw(platform::KeyCode::RightControl);
    const bool altDown = platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftAlt) ||
                         platform::InputManager::isKeyDownRaw(platform::KeyCode::RightAlt);
    if (controlDown || altDown) {
        return;
    }

    if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::W)) {
        m_gizmoOperation = GizmoOperation::Translate;
    } else if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::E)) {
        m_gizmoOperation = GizmoOperation::Rotate;
    } else if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::F)) {
        focusSelectedEntity();
    } else if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::R)) {
        m_gizmoOperation = GizmoOperation::Scale;
    }
}

void GameplayState::wakeEntity(const ecs::EntityId entity) {
    if (auto* rigidbody = m_world.getComponent<ecs::Rigidbody>(entity)) {
        rigidbody->wakeUp();
    }
}

bool GameplayState::entityMatchesFilter(const ecs::EntityId entity) const {
    if (m_hierarchyFilter.empty()) {
        return true;
    }

    const std::string loweredLabel = toLowerCopy(entityDisplayName(entity));
    const std::string loweredFilter = toLowerCopy(m_hierarchyFilter);
    return loweredLabel.find(loweredFilter) != std::string::npos;
}

std::string GameplayState::entityDisplayName(const ecs::EntityId entity) const {
    if (const auto* tag = m_world.getComponent<ecs::Tag>(entity);
        tag != nullptr && !tag->value.empty()) {
        return tag->value;
    }
    return "Entity " + std::to_string(entity);
}

std::vector<ecs::EntityId> GameplayState::sortedEntities() const {
    std::vector<ecs::EntityId> entities;
    entities.reserve(m_world.aliveCount());
    m_world.forEachEntity([&](const ecs::EntityId entity) {
        entities.push_back(entity);
    });
    std::sort(entities.begin(), entities.end());
    return entities;
}

void GameplayState::refreshProjectAssets() {
    m_projectAssets.clear();
    const std::filesystem::path root = editorAssetsRoot();
    if (!std::filesystem::exists(root)) {
        m_projectAssetsDirty = false;
        return;
    }

    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
        if (error || !entry.is_regular_file()) {
            continue;
        }

        AssetEntry asset{};
        asset.path = toAssetPath(entry.path());
        asset.name = assetDisplayName(entry.path());
        asset.kind = classifyAsset(entry.path());
        asset.details = buildAssetDetails(entry.path(), asset.kind);
        asset.hasPixelPreview =
            loadPpmPreview(entry.path(), asset.previewPixels, asset.previewWidth, asset.previewHeight);
        m_projectAssets.push_back(std::move(asset));
    }

    std::sort(m_projectAssets.begin(), m_projectAssets.end(), [](const AssetEntry& lhs, const AssetEntry& rhs) {
        if (lhs.kind == rhs.kind) {
            return lhs.path < rhs.path;
        }
        return lhs.kind < rhs.kind;
    });

    m_projectAssetsDirty = false;
}

std::optional<ecs::WorldAabb> GameplayState::computeSceneBounds() const {
    std::optional<ecs::WorldAabb> sceneBounds;
    m_world.forEachEntity([&](const ecs::EntityId entity) {
        if (entity == m_cameraEntity) {
            return;
        }

        ecs::WorldColliderShape shape{};
        if (!computeEntityPickShape(entity, shape)) {
            return;
        }

        const ecs::WorldAabb bounds = ecs::computeWorldBounds(shape);
        if (!sceneBounds.has_value()) {
            sceneBounds = bounds;
            return;
        }

        sceneBounds->min = glm::min(sceneBounds->min, bounds.min);
        sceneBounds->max = glm::max(sceneBounds->max, bounds.max);
    });
    return sceneBounds;
}

void GameplayState::focusSceneBounds() {
    const auto bounds = computeSceneBounds();
    auto* cameraTransform = m_world.getComponent<ecs::Transform>(m_cameraEntity);
    if (!bounds.has_value() || cameraTransform == nullptr) {
        m_editorStatusMessage = "Nothing to frame.";
        return;
    }

    const glm::vec3 center = bounds->center();
    const float radius = std::max(1.0F, glm::length(bounds->extents()));
    const glm::vec3 forward = ecs::forwardFromEuler(cameraTransform->rotationEulerRadians);
    cameraTransform->position = center - forward * std::clamp(radius * 2.6F, 6.0F, 80.0F);
    m_editorStatusMessage = "Framed scene.";
}

void GameplayState::selectNextEntity(const int direction) {
    const std::vector<ecs::EntityId> entities = sortedEntities();
    std::vector<ecs::EntityId> selectable;
    selectable.reserve(entities.size());
    for (const ecs::EntityId entity : entities) {
        if (entity != m_cameraEntity) {
            selectable.push_back(entity);
        }
    }
    if (selectable.empty()) {
        m_selectedEntity = ecs::kInvalidEntity;
        return;
    }

    auto it = std::find(selectable.begin(), selectable.end(), m_selectedEntity);
    if (it == selectable.end()) {
        m_selectedEntity = selectable.front();
    } else {
        const int current = static_cast<int>(std::distance(selectable.begin(), it));
        const int count = static_cast<int>(selectable.size());
        const int next = (current + direction + count) % count;
        m_selectedEntity = selectable[static_cast<std::size_t>(next)];
    }
    m_editorStatusMessage = "Selected " + entityDisplayName(m_selectedEntity);
}

void GameplayState::resetSelectedTransform() {
    if (!canEditScene() || !m_world.isAlive(m_selectedEntity)) {
        return;
    }
    auto* transform = m_world.getComponent<ecs::Transform>(m_selectedEntity);
    if (transform == nullptr) {
        return;
    }

    recordSceneHistory("Reset Transform");
    *transform = ecs::Transform{};
    wakeEntity(m_selectedEntity);
    m_editorStatusMessage = "Reset transform.";
}

void GameplayState::dropSelectedToGround() {
    if (!canEditScene() || !m_world.isAlive(m_selectedEntity)) {
        return;
    }
    auto* transform = m_world.getComponent<ecs::Transform>(m_selectedEntity);
    if (transform == nullptr) {
        return;
    }

    ecs::WorldColliderShape shape{};
    if (!computeEntityPickShape(m_selectedEntity, shape)) {
        return;
    }

    const ecs::WorldAabb bounds = ecs::computeWorldBounds(shape);
    const float floorTop = kFloorPosition.y + (kFloorScale.y * 0.5F);
    recordSceneHistory("Drop To Ground");
    transform->position.y += floorTop - bounds.min.y;
    wakeEntity(m_selectedEntity);
    m_editorStatusMessage = "Dropped to ground.";
}

void GameplayState::zeroSelectedVelocity() {
    if (!canEditScene() || !m_world.isAlive(m_selectedEntity)) {
        return;
    }
    auto* rigidbody = m_world.getComponent<ecs::Rigidbody>(m_selectedEntity);
    if (rigidbody == nullptr) {
        return;
    }

    recordSceneHistory("Zero Velocity");
    rigidbody->velocity = glm::vec3(0.0F);
    rigidbody->acceleration = glm::vec3(0.0F);
    rigidbody->angularVelocity = glm::vec3(0.0F);
    rigidbody->accumulatedForce = glm::vec3(0.0F);
    rigidbody->accumulatedTorque = glm::vec3(0.0F);
    rigidbody->wakeUp();
    m_editorStatusMessage = "Zeroed selected velocity.";
}

void GameplayState::randomizeSelectedTint() {
    if (!canEditScene() || !m_world.isAlive(m_selectedEntity)) {
        return;
    }
    auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(m_selectedEntity);
    if (meshRenderer == nullptr) {
        return;
    }

    recordSceneHistory("Randomize Tint");
    meshRenderer->tint = paletteColor(m_spawnSequence++);
    m_editorStatusMessage = "Changed selected tint.";
}

void GameplayState::setSelectedVisible(const bool visible) {
    if (!canEditScene() || !m_world.isAlive(m_selectedEntity)) {
        return;
    }
    auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(m_selectedEntity);
    if (meshRenderer == nullptr || meshRenderer->visible == visible) {
        return;
    }

    recordSceneHistory(visible ? "Show Selected" : "Hide Selected");
    meshRenderer->visible = visible;
    m_editorStatusMessage = visible ? "Selected visible." : "Selected hidden.";
}

void GameplayState::zeroAllVelocities() {
    if (!canEditScene()) {
        return;
    }

    recordSceneHistory("Zero All Velocities");
    std::size_t changedCount = 0;
    m_world.forEach<ecs::Rigidbody>([&](ecs::EntityId entity, ecs::Rigidbody& rigidbody) {
        (void)entity;
        rigidbody.velocity = glm::vec3(0.0F);
        rigidbody.acceleration = glm::vec3(0.0F);
        rigidbody.angularVelocity = glm::vec3(0.0F);
        rigidbody.accumulatedForce = glm::vec3(0.0F);
        rigidbody.accumulatedTorque = glm::vec3(0.0F);
        rigidbody.wakeUp();
        ++changedCount;
    });
    m_editorStatusMessage = "Zeroed velocities: " + std::to_string(changedCount);
}

void GameplayState::setAllDynamicsStatic(const bool makeStatic) {
    if (!canEditScene()) {
        return;
    }

    recordSceneHistory(makeStatic ? "Freeze Dynamics" : "Unfreeze Dynamics");
    std::size_t changedCount = 0;
    m_world.forEach<ecs::Rigidbody>([&](ecs::EntityId entity, ecs::Rigidbody& rigidbody) {
        (void)entity;
        if (rigidbody.isStatic == makeStatic) {
            return;
        }
        rigidbody.isStatic = makeStatic;
        rigidbody.recalculateMassProperties();
        rigidbody.wakeUp();
        ++changedCount;
    });
    m_editorStatusMessage = (makeStatic ? "Frozen bodies: " : "Unfrozen bodies: ") + std::to_string(changedCount);
}

void GameplayState::storeCameraBookmark(const std::size_t index) {
    if (index >= m_cameraBookmarks.size()) {
        return;
    }
    if (const auto* cameraTransform = m_world.getComponent<ecs::Transform>(m_cameraEntity)) {
        m_cameraBookmarks[index] = *cameraTransform;
        m_editorStatusMessage = "Stored camera bookmark " + std::to_string(index + 1U);
    }
}

void GameplayState::recallCameraBookmark(const std::size_t index) {
    if (index >= m_cameraBookmarks.size() || !m_cameraBookmarks[index].has_value()) {
        m_editorStatusMessage = "Camera bookmark is empty.";
        return;
    }
    if (auto* cameraTransform = m_world.getComponent<ecs::Transform>(m_cameraEntity)) {
        *cameraTransform = *m_cameraBookmarks[index];
        m_editorStatusMessage = "Recalled camera bookmark " + std::to_string(index + 1U);
    }
}

void GameplayState::dockPanel(EditorDockSlot& panelSlot, const EditorDockSlot newSlot) {
    if (&panelSlot == &m_viewportDockSlot) {
        if (panelSlot != EditorDockSlot::Center) {
            panelSlot = EditorDockSlot::Center;
            markEditorLayoutDirty();
        }
        return;
    }
    if (newSlot == EditorDockSlot::Center) {
        m_editorStatusMessage = "Viewport is fixed in the center.";
        return;
    }

    if (panelSlot == newSlot) {
        return;
    }

    const EditorDockSlot oldSlot = panelSlot;
    auto swapOccupant = [&](EditorDockSlot& otherSlot) {
        if (&otherSlot != &panelSlot && otherSlot == newSlot && !isFloatingSlot(newSlot)) {
            otherSlot = oldSlot;
        }
    };

    swapOccupant(m_toolbarDockSlot);
    swapOccupant(m_hierarchyDockSlot);
    swapOccupant(m_projectDockSlot);
    swapOccupant(m_inspectorDockSlot);
    swapOccupant(m_statsDockSlot);
    swapOccupant(m_viewportDockSlot);
    swapOccupant(m_statusDockSlot);

    panelSlot = newSlot;
    markEditorLayoutDirty();
}

EditorDockSlot* GameplayState::dockSlotForPanel(const EditorPanelId panelId) {
    switch (panelId) {
        case EditorPanelId::Toolbar:
            return &m_toolbarDockSlot;
        case EditorPanelId::Hierarchy:
            return &m_hierarchyDockSlot;
        case EditorPanelId::Project:
            return &m_projectDockSlot;
        case EditorPanelId::Inspector:
            return &m_inspectorDockSlot;
        case EditorPanelId::Stats:
            return &m_statsDockSlot;
        case EditorPanelId::Viewport:
            return &m_viewportDockSlot;
        case EditorPanelId::Status:
            return &m_statusDockSlot;
    }
    return nullptr;
}

const EditorDockSlot* GameplayState::dockSlotForPanel(const EditorPanelId panelId) const {
    switch (panelId) {
        case EditorPanelId::Toolbar:
            return &m_toolbarDockSlot;
        case EditorPanelId::Hierarchy:
            return &m_hierarchyDockSlot;
        case EditorPanelId::Project:
            return &m_projectDockSlot;
        case EditorPanelId::Inspector:
            return &m_inspectorDockSlot;
        case EditorPanelId::Stats:
            return &m_statsDockSlot;
        case EditorPanelId::Viewport:
            return &m_viewportDockSlot;
        case EditorPanelId::Status:
            return &m_statusDockSlot;
    }
    return nullptr;
}

void GameplayState::resetEditorLayout() {
    m_showToolbarPanel = true;
    m_showHierarchyPanel = true;
    m_showProjectPanel = true;
    m_showInspectorPanel = true;
    m_showStatsPanel = true;
    m_showViewportPanel = true;
    m_showStatusPanel = true;
    m_toolbarDockSlot = EditorDockSlot::Top;
    m_hierarchyDockSlot = EditorDockSlot::LeftTop;
    m_projectDockSlot = EditorDockSlot::LeftBottom;
    m_inspectorDockSlot = EditorDockSlot::RightTop;
    m_statsDockSlot = EditorDockSlot::RightBottom;
    m_viewportDockSlot = EditorDockSlot::Center;
    m_statusDockSlot = EditorDockSlot::Bottom;
    m_editorLeftWidth = 0.0F;
    m_editorRightWidth = 0.0F;
    m_editorLeftSplit = 0.72F;
    m_editorRightSplit = 0.68F;
    m_resetEditorLayout = true;
    markEditorLayoutDirty();
}

void GameplayState::loadEditorLayout() {
    const std::filesystem::path path = editorLayoutPath();
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    try {
        const nlohmann::json layout = nlohmann::json::parse(file);
        if (const auto visibility = layout.find("visibility"); visibility != layout.end() && visibility->is_object()) {
            m_showToolbarPanel = visibility->value("toolbar", m_showToolbarPanel);
            m_showHierarchyPanel = visibility->value("hierarchy", m_showHierarchyPanel);
            m_showProjectPanel = visibility->value("project", m_showProjectPanel);
            m_showInspectorPanel = visibility->value("inspector", m_showInspectorPanel);
            m_showStatsPanel = visibility->value("stats", m_showStatsPanel);
            m_showViewportPanel = visibility->value("viewport", m_showViewportPanel);
            m_showStatusPanel = visibility->value("status", m_showStatusPanel);
        }

        if (const auto slots = layout.find("dockSlots"); slots != layout.end() && slots->is_object()) {
            auto readSlot = [&](const char* key, EditorDockSlot& slot) {
                if (const auto found = slots->find(key); found != slots->end() && found->is_string()) {
                    if (const std::optional<EditorDockSlot> parsed = dockSlotFromString(found->get<std::string>())) {
                        slot = *parsed;
                    }
                }
            };
            readSlot("toolbar", m_toolbarDockSlot);
            readSlot("hierarchy", m_hierarchyDockSlot);
            readSlot("project", m_projectDockSlot);
            readSlot("inspector", m_inspectorDockSlot);
            readSlot("stats", m_statsDockSlot);
            readSlot("status", m_statusDockSlot);
        }
        m_viewportDockSlot = EditorDockSlot::Center;
        auto keepCenterForViewport = [](EditorDockSlot& slot) {
            if (slot == EditorDockSlot::Center) {
                slot = EditorDockSlot::Floating;
            }
        };
        keepCenterForViewport(m_toolbarDockSlot);
        keepCenterForViewport(m_hierarchyDockSlot);
        keepCenterForViewport(m_projectDockSlot);
        keepCenterForViewport(m_inspectorDockSlot);
        keepCenterForViewport(m_statsDockSlot);
        keepCenterForViewport(m_statusDockSlot);

        if (const auto sizing = layout.find("sizing"); sizing != layout.end() && sizing->is_object()) {
            m_editorLeftWidth = std::clamp(sizing->value("leftWidth", m_editorLeftWidth), 0.0F, 900.0F);
            m_editorRightWidth = std::clamp(sizing->value("rightWidth", m_editorRightWidth), 0.0F, 900.0F);
            m_editorLeftSplit = std::clamp(sizing->value("leftSplit", m_editorLeftSplit), 0.22F, 0.82F);
            m_editorRightSplit = std::clamp(sizing->value("rightSplit", m_editorRightSplit), 0.22F, 0.82F);
        }

        m_editorLayoutLocked = layout.value("locked", m_editorLayoutLocked);
        m_editorLayoutDirty = false;
    } catch (const std::exception& exception) {
        ENGINE_LOG_WARN("Failed to load editor layout '{}': {}", path.string(), exception.what());
    }
}

void GameplayState::saveEditorLayout() const {
    const std::filesystem::path path = editorLayoutPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    nlohmann::json layout;
    layout["version"] = 1;
    layout["locked"] = m_editorLayoutLocked;
    layout["visibility"] = {
        {"toolbar", m_showToolbarPanel},
        {"hierarchy", m_showHierarchyPanel},
        {"project", m_showProjectPanel},
        {"inspector", m_showInspectorPanel},
        {"stats", m_showStatsPanel},
        {"viewport", m_showViewportPanel},
        {"status", m_showStatusPanel},
    };
    layout["dockSlots"] = {
        {"toolbar", dockSlotToString(m_toolbarDockSlot)},
        {"hierarchy", dockSlotToString(m_hierarchyDockSlot)},
        {"project", dockSlotToString(m_projectDockSlot)},
        {"inspector", dockSlotToString(m_inspectorDockSlot)},
        {"stats", dockSlotToString(m_statsDockSlot)},
        {"viewport", dockSlotToString(m_viewportDockSlot)},
        {"status", dockSlotToString(m_statusDockSlot)},
    };
    layout["sizing"] = {
        {"leftWidth", m_editorLeftWidth},
        {"rightWidth", m_editorRightWidth},
        {"leftSplit", m_editorLeftSplit},
        {"rightSplit", m_editorRightSplit},
    };

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        ENGINE_LOG_WARN("Failed to save editor layout '{}'", path.string());
        return;
    }
    file << std::setw(2) << layout << '\n';
}

void GameplayState::markEditorLayoutDirty() {
    m_editorLayoutDirty = true;
}

void GameplayState::updateFrameHistory() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    const float deltaSeconds = ImGui::GetIO().DeltaTime;
    m_frameTimeHistory[m_historyCursor] = deltaSeconds > 0.0F ? deltaSeconds * 1000.0F : 0.0F;
    m_renderTimeHistory[m_historyCursor] = static_cast<float>(m_lastSceneRenderMs);
    m_historyCursor = (m_historyCursor + 1U) % m_frameTimeHistory.size();
}

void GameplayState::update(const double dt) {
    if (!m_sceneInitialized) {
        createDemoScene();
    }

    ensureSelectedEntityValid();

    if ((m_viewportHovered || m_cameraLookActive) && stack().window() != nullptr) {
        const auto [scrollX, scrollY] = platform::InputManager::scrollDelta();
        (void)scrollX;
        if (scrollY != 0.0) {
            auto cameraSettings = m_cameraController.settings();
            cameraSettings.moveSpeed =
                std::clamp(cameraSettings.moveSpeed + static_cast<float>(scrollY), 1.0F, 32.0F);
            m_cameraController.setSettings(cameraSettings);
        }
    }

    if (!platform::InputManager::isMouseButtonDownRaw(platform::MouseButton::Right) && m_cameraLookActive) {
        setCameraLookActive(false);
    }

    if (auto* cameraTransform = m_world.getComponent<ecs::Transform>(m_cameraEntity)) {
        const bool allowMouseLook = m_cameraLookActive && !m_skipNextCameraLookDelta;
        if (m_skipNextCameraLookDelta) {
            platform::InputManager::resetMouseDelta();
        }
        m_cameraController.update(*cameraTransform, dt, m_cameraLookActive, allowMouseLook);
        m_skipNextCameraLookDelta = false;
    }

    if (m_editorMode == EditorMode::Play) {
        processPendingSpawns();
        if (m_simulationRunning) {
            m_physicsSystem.update(m_world, dt, m_eventBus);
        }
    }
}

GameplayState::CameraFrame GameplayState::buildActiveCameraFrame(const renderer::Renderer& renderer) const {
    const rhi::Extent2D frameExtent = renderer.frameExtent();
    const float viewportWidth = m_viewportSize.x > 1.0F ? m_viewportSize.x : static_cast<float>(frameExtent.width);
    const float viewportHeight = m_viewportSize.y > 1.0F ? m_viewportSize.y : static_cast<float>(frameExtent.height);
    const float aspectRatio = viewportHeight > 0.0F ? viewportWidth / viewportHeight : (16.0F / 9.0F);

    CameraFrame cameraFrame{};
    cameraFrame.aspectRatio = aspectRatio;

    bool foundCamera = false;
    m_world.forEach<ecs::Transform, ecs::Camera>([&](ecs::EntityId entity, const ecs::Transform& transform, const ecs::Camera& camera) {
        (void)entity;
        if (foundCamera || !camera.active) {
            return;
        }

        const glm::vec3 forward = ecs::forwardFromEuler(transform.rotationEulerRadians);
        const glm::vec3 up = ecs::upFromEuler(transform.rotationEulerRadians);
        cameraFrame.projection =
            glm::perspectiveRH_ZO(camera.verticalFovRadians, aspectRatio, camera.nearPlane, camera.farPlane);
        cameraFrame.view = glm::lookAtRH(transform.position, transform.position + forward, up);
        cameraFrame.viewProjection = cameraFrame.projection * cameraFrame.view;
        foundCamera = true;
    });

    if (!foundCamera) {
        cameraFrame.projection = glm::perspectiveRH_ZO(glm::radians(52.0F), aspectRatio, 0.1F, 220.0F);
        cameraFrame.view = glm::lookAtRH(
            kCameraStartPosition,
            glm::vec3(0.0F, 1.5F, 10.0F),
            glm::vec3(0.0F, 1.0F, 0.0F));
        cameraFrame.viewProjection = cameraFrame.projection * cameraFrame.view;
    }

    return cameraFrame;
}

std::optional<GameplayState::PickRay> GameplayState::buildPickRay(
    const CameraFrame& cameraFrame,
    const glm::vec2& mousePosition) const {
    if (m_viewportSize.x <= 1.0F || m_viewportSize.y <= 1.0F) {
        return std::nullopt;
    }

    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const glm::vec2 renderPosition(mainViewport->Pos.x, mainViewport->Pos.y);
    const glm::vec2 renderSize(mainViewport->Size.x, mainViewport->Size.y);
    if (renderSize.x <= 1.0F || renderSize.y <= 1.0F) {
        return std::nullopt;
    }

    const glm::vec2 renderRelative = (mousePosition - renderPosition) / renderSize;
    if (renderRelative.x < 0.0F || renderRelative.x > 1.0F ||
        renderRelative.y < 0.0F || renderRelative.y > 1.0F) {
        return std::nullopt;
    }

    const float ndcX = renderRelative.x * 2.0F - 1.0F;
    const float ndcY = 1.0F - renderRelative.y * 2.0F;
    const glm::mat4 inverseViewProjection = glm::inverse(cameraFrame.viewProjection);
    const glm::mat4 inverseView = glm::inverse(cameraFrame.view);

    glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 0.0F, 1.0F);
    glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 1.0F, 1.0F);
    if (std::abs(nearPoint.w) < 1e-6F || std::abs(farPoint.w) < 1e-6F) {
        return std::nullopt;
    }

    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    PickRay ray{};
    ray.origin = glm::vec3(inverseView[3]);
    ray.direction = glm::vec3(farPoint - nearPoint);
    if (glm::dot(ray.direction, ray.direction) <= 1e-8F) {
        return std::nullopt;
    }
    ray.direction = glm::normalize(ray.direction);
    return ray;
}

std::optional<GameplayState::PickResult> GameplayState::pickEntity(const PickRay& ray) const {
    PickResult bestHit{};
    bestHit.distance = std::numeric_limits<float>::max();

    m_world.forEachEntity([&](const ecs::EntityId entity) {
        if (entity == m_cameraEntity || !m_world.hasComponent<ecs::Transform>(entity)) {
            return;
        }

        float distance = 0.0F;
        if (intersectRayEntityBounds(m_world, entity, ray.origin, ray.direction, distance) &&
            distance < bestHit.distance) {
            bestHit.entity = entity;
            bestHit.distance = distance;
        }
    });

    if (bestHit.entity == ecs::kInvalidEntity) {
        return std::nullopt;
    }
    return bestHit;
}

void GameplayState::handleViewportPicking(const CameraFrame& cameraFrame) {
    if (!m_viewportHovered || m_cameraLookActive || ImGuizmo::IsUsing() || ImGuizmo::IsOver()) {
        return;
    }

    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        return;
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto ray = buildPickRay(cameraFrame, glm::vec2(mouse.x, mouse.y));
    if (!ray.has_value()) {
        return;
    }

    const auto hit = pickEntity(*ray);
    if (!hit.has_value()) {
        m_editorStatusMessage = "Viewport pick missed.";
        return;
    }

    m_selectedEntity = hit->entity;
    m_editorStatusMessage = "Selected " + entityDisplayName(hit->entity);
}

void GameplayState::updateHoveredEntity(const CameraFrame& cameraFrame) {
    if (!m_viewportHovered || m_cameraLookActive || ImGuizmo::IsUsing()) {
        m_hoveredEntity = ecs::kInvalidEntity;
        return;
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const glm::vec2 mousePosition(mouse.x, mouse.y);
    const bool mouseMoved = std::abs(mousePosition.x - m_lastHoverPickMouse.x) > 0.5F ||
                            std::abs(mousePosition.y - m_lastHoverPickMouse.y) > 0.5F;
    if (!mouseMoved && (m_hoverPickFrame++ % 4U) != 0U) {
        return;
    }
    m_lastHoverPickMouse = mousePosition;
    m_hoveredEntity = ecs::kInvalidEntity;

    const auto ray = buildPickRay(cameraFrame, glm::vec2(mouse.x, mouse.y));
    if (!ray.has_value()) {
        return;
    }

    const auto hit = pickEntity(*ray);
    if (hit.has_value()) {
        m_hoveredEntity = hit->entity;
    }
}

void GameplayState::renderSelectionOutline(
    renderer::RenderAdapter& renderer,
    const glm::mat4& viewProjectionMatrix) {
    const auto drawEntityOutline = [&](const ecs::EntityId entity, const glm::vec4& color, const float thickness) {
        if (entity == ecs::kInvalidEntity || !m_world.isAlive(entity)) {
            return;
        }

        ecs::WorldColliderShape shape{};
        if (!computeEntityPickShape(entity, shape)) {
            return;
        }

        ecs::WorldAabb bounds = ecs::computeWorldBounds(shape);
        const glm::vec3 padding = glm::max(bounds.extents() * 0.035F, glm::vec3(0.035F));
        bounds.min -= padding;
        bounds.max += padding;
        renderer.drawDebugAabb(bounds.min, bounds.max, viewProjectionMatrix, color, thickness);
    };

    if (m_showHoverOutline && m_hoveredEntity != m_selectedEntity) {
        drawEntityOutline(m_hoveredEntity, glm::vec4(0.62F, 0.86F, 1.0F, 1.0F), 0.025F);
    }
    drawEntityOutline(m_selectedEntity, glm::vec4(1.0F, 0.78F, 0.22F, 1.0F), 0.04F);
}

void GameplayState::render(renderer::Renderer& rendererInstance) {
    const auto renderStart = std::chrono::high_resolution_clock::now();
    const CameraFrame cameraFrame = buildActiveCameraFrame(rendererInstance);
    renderer::RenderAdapter renderAdapter{rendererInstance};
    m_renderSystem.render(m_world, renderAdapter, cameraFrame.viewProjection);
    if (m_showColliderDebug) {
        m_debugRenderSystem.render(m_world, renderAdapter, cameraFrame.viewProjection);
    }
    renderSelectionOutline(renderAdapter, cameraFrame.viewProjection);

    m_lastSceneRenderMs = std::chrono::duration<double, std::milli>(
                              std::chrono::high_resolution_clock::now() - renderStart)
                              .count();
}

void GameplayState::renderCreateEntityMenu() {
    const bool enabled = canEditScene();
    if (!enabled) {
        ImGui::BeginDisabled();
    }

    if (ImGui::MenuItem("Empty", "Ctrl+N")) {
        createEditorEntity(EditorEntityKind::Empty);
    }
    if (ImGui::MenuItem("Render Cube")) {
        createEditorEntity(EditorEntityKind::RenderCube);
    }
    if (ImGui::MenuItem("Physics Cube")) {
        createEditorEntity(EditorEntityKind::PhysicsCube);
    }
    if (ImGui::MenuItem("Physics Sphere")) {
        createEditorEntity(EditorEntityKind::PhysicsSphere);
    }
    if (ImGui::MenuItem("Physics Pyramid")) {
        createEditorEntity(EditorEntityKind::PhysicsPyramid);
    }
    if (ImGui::MenuItem("Camera")) {
        createEditorEntity(EditorEntityKind::Camera);
    }

    if (!enabled) {
        ImGui::EndDisabled();
    }
}

void GameplayState::renderAddComponentMenu(const ecs::EntityId entity) {
    if (!canEditScene() || !m_world.isAlive(entity)) {
        ImGui::BeginDisabled();
    }

    if (!m_world.hasComponent<ecs::Transform>(entity) && ImGui::MenuItem("Transform")) {
        recordSceneHistory("Add Transform");
        ecs::Transform transform{};
        transform.position = editorSpawnPosition();
        m_world.addComponent<ecs::Transform>(entity, transform);
        m_editorStatusMessage = "Added Transform";
    }
    if (!m_world.hasComponent<ecs::Tag>(entity) && ImGui::MenuItem("Tag")) {
        recordSceneHistory("Add Tag");
        m_world.addComponent<ecs::Tag>(entity, ecs::Tag{"entity_" + std::to_string(entity)});
        m_editorStatusMessage = "Added Tag";
    }
    if (!m_world.hasComponent<ecs::MeshRenderer>(entity) && ImGui::MenuItem("Mesh Renderer")) {
        recordSceneHistory("Add Mesh Renderer");
        ecs::MeshRenderer meshRenderer{};
        meshRenderer.meshId = kCubeMeshId;
        meshRenderer.shaderId = kShaderId;
        meshRenderer.tint = glm::vec4(0.30F, 0.63F, 0.84F, 1.0F);
        m_world.addComponent<ecs::MeshRenderer>(entity, meshRenderer);
        m_editorStatusMessage = "Added Mesh Renderer";
    }
    if (!m_world.hasComponent<ecs::Camera>(entity) && ImGui::MenuItem("Camera")) {
        recordSceneHistory("Add Camera");
        ecs::Camera camera{};
        camera.active = false;
        camera.verticalFovRadians = glm::radians(52.0F);
        m_world.addComponent<ecs::Camera>(entity, camera);
        m_editorStatusMessage = "Added Camera";
    }
    if (!m_world.hasComponent<ecs::Rigidbody>(entity) && ImGui::MenuItem("Rigidbody")) {
        recordSceneHistory("Add Rigidbody");
        ecs::Rigidbody rigidbody{};
        rigidbody.recalculateMassProperties();
        m_world.addComponent<ecs::Rigidbody>(entity, rigidbody);
        m_editorStatusMessage = "Added Rigidbody";
    }
    if (!m_world.hasComponent<ecs::Collider>(entity) && ImGui::MenuItem("Collider")) {
        recordSceneHistory("Add Collider");
        ecs::Collider collider{};
        m_world.addComponent<ecs::Collider>(entity, collider);
        m_editorStatusMessage = "Added Collider";
    }
    if (!m_world.hasComponent<ecs::Hierarchy>(entity) && ImGui::MenuItem("Hierarchy")) {
        recordSceneHistory("Add Hierarchy");
        m_world.addComponent<ecs::Hierarchy>(entity, ecs::Hierarchy{});
        m_editorStatusMessage = "Added Hierarchy";
    }

    if (!canEditScene() || !m_world.isAlive(entity)) {
        ImGui::EndDisabled();
    }
}

void GameplayState::renderRemoveComponentMenu(const ecs::EntityId entity) {
    if (!canEditScene() || !m_world.isAlive(entity)) {
        ImGui::BeginDisabled();
    }

    if (m_world.hasComponent<ecs::Transform>(entity) && ImGui::MenuItem("Transform")) {
        recordSceneHistory("Remove Transform");
        m_world.removeComponent<ecs::Transform>(entity);
        m_editorStatusMessage = "Removed Transform";
    }
    if (m_world.hasComponent<ecs::Tag>(entity) && ImGui::MenuItem("Tag")) {
        recordSceneHistory("Remove Tag");
        m_world.removeComponent<ecs::Tag>(entity);
        m_editorStatusMessage = "Removed Tag";
    }
    if (m_world.hasComponent<ecs::MeshRenderer>(entity) && ImGui::MenuItem("Mesh Renderer")) {
        recordSceneHistory("Remove Mesh Renderer");
        m_world.removeComponent<ecs::MeshRenderer>(entity);
        m_editorStatusMessage = "Removed Mesh Renderer";
    }
    if (m_world.hasComponent<ecs::Camera>(entity) && ImGui::MenuItem("Camera")) {
        recordSceneHistory("Remove Camera");
        m_world.removeComponent<ecs::Camera>(entity);
        if (m_cameraEntity == entity) {
            m_cameraEntity = ecs::kInvalidEntity;
        }
        m_editorStatusMessage = "Removed Camera";
    }
    if (m_world.hasComponent<ecs::Rigidbody>(entity) && ImGui::MenuItem("Rigidbody")) {
        recordSceneHistory("Remove Rigidbody");
        m_world.removeComponent<ecs::Rigidbody>(entity);
        m_editorStatusMessage = "Removed Rigidbody";
    }
    if (m_world.hasComponent<ecs::Collider>(entity) && ImGui::MenuItem("Collider")) {
        recordSceneHistory("Remove Collider");
        m_world.removeComponent<ecs::Collider>(entity);
        m_editorStatusMessage = "Removed Collider";
    }
    if (m_world.hasComponent<ecs::Hierarchy>(entity) && ImGui::MenuItem("Hierarchy")) {
        recordSceneHistory("Remove Hierarchy");
        m_world.removeComponent<ecs::Hierarchy>(entity);
        m_editorStatusMessage = "Removed Hierarchy";
    }

    if (!canEditScene() || !m_world.isAlive(entity)) {
        ImGui::EndDisabled();
    }
}

void GameplayState::renderEntityContextMenu(const ecs::EntityId entity) {
    if (ImGui::MenuItem("Select")) {
        m_selectedEntity = entity;
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, canEditScene() && m_world.isAlive(entity))) {
        duplicateEntity(entity);
    }
    if (ImGui::BeginMenu("Create")) {
        renderCreateEntityMenu();
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Delete", "Delete", false, canEditScene() && m_world.isAlive(entity))) {
        deleteEntity(entity);
    }
}

void GameplayState::renderPanelDockControls(const EditorPanelId panelId, const char* popupId) {
    if (panelId == EditorPanelId::Viewport) {
        return;
    }

    EditorDockSlot* slotPtr = dockSlotForPanel(panelId);
    if (slotPtr == nullptr) {
        return;
    }

    EditorDockSlot& slot = *slotPtr;
    const float handleHeight = ImGui::GetFrameHeight();
    const float handleWidth = 34.0F;
    const ImVec2 handleMin = ImGui::GetCursorScreenPos();
    const ImVec2 handleSize(handleWidth, handleHeight);

    ImGui::PushID(popupId);
    ImGui::InvisibleButton("DockDragGrip", handleSize);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool active = ImGui::IsItemActive();
    const bool canChangeLayout = !m_editorLayoutLocked;
    if (canChangeLayout && (hovered || active)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 handleMax(handleMin.x + handleSize.x, handleMin.y + handleSize.y);
    const ImU32 bgColor = ImGui::GetColorU32(
        !canChangeLayout ? ImVec4(0.10F, 0.12F, 0.16F, 1.0F)
               : active ? ImVec4(0.22F, 0.34F, 0.50F, 1.0F)
               : (hovered ? ImVec4(0.18F, 0.25F, 0.34F, 1.0F) : ImVec4(0.12F, 0.15F, 0.20F, 1.0F)));
    const ImU32 borderColor = ImGui::GetColorU32(
        !canChangeLayout ? ImVec4(0.18F, 0.21F, 0.27F, 1.0F)
                         : (hovered || active ? ImVec4(0.35F, 0.58F, 0.88F, 1.0F)
                                             : ImVec4(0.22F, 0.27F, 0.34F, 1.0F)));
    drawList->AddRectFilled(handleMin, handleMax, bgColor, 6.0F);
    drawList->AddRect(handleMin, handleMax, borderColor, 6.0F, 0, hovered || active ? 1.5F : 1.0F);
    for (int i = 0; i < 3; ++i) {
        const float x = handleMin.x + 8.0F + static_cast<float>(i) * 4.0F;
        drawList->AddCircleFilled(ImVec2(x, handleMin.y + handleHeight * 0.5F), 1.35F, IM_COL32(151, 165, 184, 255));
    }
    if (hovered) {
        ImGui::SetTooltip(
            "%s\nDrag to dock, double-click to float, right-click for menu.",
            dockSlotLabel(slot));
    }

    if (hovered && !canChangeLayout) {
        ImGui::SetTooltip("Layout is locked. Disable View > Lock Editor Layout to move panels.");
    }

    if (canChangeLayout && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        ImGui::SetDragDropPayload(kEditorDockPayload, &panelId, sizeof(panelId));
        m_draggedDockPanel = panelId;
        m_dockDragActive = true;
        ImGui::Text("Move %s", panelLabel(panelId));
        ImGui::TextDisabled("Drop on a highlighted dock zone.");
        ImGui::EndDragDropSource();
    }

    if (canChangeLayout && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        dockPanel(slot, EditorDockSlot::Floating);
        m_editorStatusMessage = std::string("Floated ") + panelLabel(panelId) + ".";
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(popupId);
    }

    if (ImGui::BeginPopup(popupId)) {
        if (!canChangeLayout) {
            ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Float / Undock", nullptr, slot == EditorDockSlot::Floating)) {
            dockPanel(slot, EditorDockSlot::Floating);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Dock Top Toolbar", nullptr, slot == EditorDockSlot::Top)) {
            dockPanel(slot, EditorDockSlot::Top);
        }
        if (ImGui::MenuItem("Dock Left Top", nullptr, slot == EditorDockSlot::LeftTop)) {
            dockPanel(slot, EditorDockSlot::LeftTop);
        }
        if (ImGui::MenuItem("Dock Left Bottom", nullptr, slot == EditorDockSlot::LeftBottom)) {
            dockPanel(slot, EditorDockSlot::LeftBottom);
        }
        if (ImGui::MenuItem("Dock Right Top", nullptr, slot == EditorDockSlot::RightTop)) {
            dockPanel(slot, EditorDockSlot::RightTop);
        }
        if (ImGui::MenuItem("Dock Right Bottom", nullptr, slot == EditorDockSlot::RightBottom)) {
            dockPanel(slot, EditorDockSlot::RightBottom);
        }
        if (ImGui::MenuItem("Dock Bottom Bar", nullptr, slot == EditorDockSlot::Bottom)) {
            dockPanel(slot, EditorDockSlot::Bottom);
        }
        if (!canChangeLayout) {
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void GameplayState::renderDockDropZone(
    const EditorPanelId draggedPanel,
    const EditorDockSlot targetSlot,
    const ImVec2& position,
    const ImVec2& size) {
    if (size.x <= 8.0F || size.y <= 8.0F) {
        return;
    }

    const EditorDockSlot* draggedSlot = dockSlotForPanel(draggedPanel);
    const bool sameSlot = draggedSlot != nullptr && *draggedSlot == targetSlot;
    ImGui::PushID(static_cast<int>(targetSlot));
    ImGui::SetCursorScreenPos(position);
    ImGui::InvisibleButton("DockDropZone", size);

    bool preview = false;
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(kEditorDockPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (payload != nullptr && payload->DataSize == static_cast<int>(sizeof(EditorPanelId))) {
            preview = payload->IsPreview();
            if (payload->IsDelivery()) {
                const EditorPanelId sourcePanel = *static_cast<const EditorPanelId*>(payload->Data);
                if (EditorDockSlot* sourceSlot = dockSlotForPanel(sourcePanel)) {
                    dockPanel(*sourceSlot, targetSlot);
                    m_dockDropAcceptedThisFrame = true;
                    m_editorStatusMessage =
                        std::string("Docked ") + panelLabel(sourcePanel) + " to " + dockSlotLabel(targetSlot) + ".";
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(position.x + size.x, position.y + size.y);
    const ImU32 fill = ImGui::GetColorU32(
        preview || hovered ? ImVec4(0.22F, 0.46F, 0.78F, sameSlot ? 0.20F : 0.34F)
                           : ImVec4(0.10F, 0.18F, 0.28F, sameSlot ? 0.10F : 0.18F));
    const ImU32 border = ImGui::GetColorU32(
        preview || hovered ? ImVec4(0.44F, 0.70F, 1.0F, 0.95F) : ImVec4(0.24F, 0.38F, 0.58F, 0.70F));
    drawList->AddRectFilled(position, max, fill, 9.0F);
    drawList->AddRect(position, max, border, 9.0F, 0, preview || hovered ? 2.5F : 1.25F);

    const char* label = dockSlotLabel(targetSlot);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    drawList->AddText(
        ImVec2(position.x + (size.x - textSize.x) * 0.5F, position.y + (size.y - textSize.y) * 0.5F),
        IM_COL32(226, 235, 247, 255),
        label);
    ImGui::PopID();
}

void GameplayState::renderDockDropOverlay() {
    m_dockDropAcceptedThisFrame = false;
    if (m_editorLayoutLocked) {
        m_dockDragActive = false;
        return;
    }

    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload == nullptr || !payload->IsDataType(kEditorDockPayload) ||
        payload->DataSize != static_cast<int>(sizeof(EditorPanelId))) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_dockDragActive = false;
        }
        return;
    }

    const EditorPanelId draggedPanel = *static_cast<const EditorPanelId*>(payload->Data);
    m_draggedDockPanel = draggedPanel;
    m_dockDragActive = true;

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("##EditorDockDropOverlay", nullptr, flags)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            viewport->Pos,
            ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
            IM_COL32(4, 7, 11, 78));

        renderDockDropZone(draggedPanel, EditorDockSlot::Top, layout.toolbarPos, layout.toolbarSize);
        renderDockDropZone(draggedPanel, EditorDockSlot::LeftTop, layout.hierarchyPos, layout.hierarchySize);
        renderDockDropZone(draggedPanel, EditorDockSlot::LeftBottom, layout.projectPos, layout.projectSize);
        renderDockDropZone(draggedPanel, EditorDockSlot::RightTop, layout.inspectorPos, layout.inspectorSize);
        renderDockDropZone(draggedPanel, EditorDockSlot::RightBottom, layout.statsPos, layout.statsSize);
        renderDockDropZone(draggedPanel, EditorDockSlot::Bottom, layout.statusPos, layout.statusSize);

        const ImVec2 centerMin = layout.viewportPos;
        const ImVec2 centerMax(centerMin.x + layout.viewportSize.x, centerMin.y + layout.viewportSize.y);
        drawList->AddRect(centerMin, centerMax, IM_COL32(86, 105, 128, 150), 9.0F, 0, 1.5F);
        const char* fixedLabel = "Viewport fixed";
        const ImVec2 fixedLabelSize = ImGui::CalcTextSize(fixedLabel);
        drawList->AddText(
            ImVec2(centerMin.x + (layout.viewportSize.x - fixedLabelSize.x) * 0.5F, centerMin.y + 12.0F),
            IM_COL32(150, 166, 187, 210),
            fixedLabel);

        const ImVec2 mouse = ImGui::GetMousePos();
        const std::string hint =
            std::string("Move ") + panelLabel(draggedPanel) + " - drop outside zones to float";
        const ImVec2 hintSize = ImGui::CalcTextSize(hint.c_str());
        const ImVec2 hintMin(mouse.x + 18.0F, mouse.y + 18.0F);
        const ImVec2 hintMax(hintMin.x + hintSize.x + 20.0F, hintMin.y + hintSize.y + 14.0F);
        drawList->AddRectFilled(hintMin, hintMax, IM_COL32(9, 13, 20, 238), 7.0F);
        drawList->AddRect(hintMin, hintMax, IM_COL32(71, 118, 179, 220), 7.0F);
        drawList->AddText(ImVec2(hintMin.x + 10.0F, hintMin.y + 7.0F), IM_COL32(225, 235, 247, 255), hint.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();

    if (m_dockDragActive && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (!m_dockDropAcceptedThisFrame) {
            if (EditorDockSlot* sourceSlot = dockSlotForPanel(m_draggedDockPanel)) {
                dockPanel(*sourceSlot, EditorDockSlot::Floating);
                m_editorStatusMessage = std::string("Floated ") + panelLabel(m_draggedDockPanel) + ".";
            }
        }
        m_dockDragActive = false;
    }
}

void GameplayState::renderDockResizeHandles() {
    if (m_editorLayoutLocked || m_dockDragActive) {
        return;
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float handleThickness = 8.0F;

    auto applyChanged = [this](const bool changed) {
        if (changed) {
            markEditorLayoutDirty();
            m_editorStatusMessage = "Editor layout resized.";
        }
    };

    const bool leftColumnActive = renderSplitterHandle(
        "##DockSplitterLeftColumn",
        ImVec2(layout.hierarchyPos.x + layout.hierarchySize.x, layout.hierarchyPos.y),
        ImVec2(handleThickness, layout.hierarchySize.y + kWindowMargin + layout.projectSize.y),
        true);
    if (leftColumnActive && std::abs(io.MouseDelta.x) > 0.001F) {
        m_editorLeftWidth = std::clamp(layout.hierarchySize.x + io.MouseDelta.x, 210.0F, 560.0F);
        applyChanged(true);
    }

    const bool rightColumnActive = renderSplitterHandle(
        "##DockSplitterRightColumn",
        ImVec2(layout.viewportPos.x + layout.viewportSize.x, layout.viewportPos.y),
        ImVec2(handleThickness, layout.viewportSize.y),
        true);
    if (rightColumnActive && std::abs(io.MouseDelta.x) > 0.001F) {
        m_editorRightWidth = std::clamp(layout.inspectorSize.x - io.MouseDelta.x, 280.0F, 620.0F);
        applyChanged(true);
    }

    const bool leftStackActive = renderSplitterHandle(
        "##DockSplitterLeftStack",
        ImVec2(layout.hierarchyPos.x, layout.hierarchyPos.y + layout.hierarchySize.y),
        ImVec2(layout.hierarchySize.x, handleThickness),
        false);
    if (leftStackActive && std::abs(io.MouseDelta.y) > 0.001F) {
        const float stackHeight = std::max(1.0F, layout.hierarchySize.y + layout.projectSize.y);
        m_editorLeftSplit = std::clamp((layout.hierarchySize.y + io.MouseDelta.y) / stackHeight, 0.22F, 0.82F);
        applyChanged(true);
    }

    const bool rightStackActive = renderSplitterHandle(
        "##DockSplitterRightStack",
        ImVec2(layout.inspectorPos.x, layout.inspectorPos.y + layout.inspectorSize.y),
        ImVec2(layout.inspectorSize.x, handleThickness),
        false);
    if (rightStackActive && std::abs(io.MouseDelta.y) > 0.001F) {
        const float stackHeight = std::max(1.0F, layout.inspectorSize.y + layout.statsSize.y);
        m_editorRightSplit = std::clamp((layout.inspectorSize.y + io.MouseDelta.y) / stackHeight, 0.22F, 0.82F);
        applyChanged(true);
    }
}

void GameplayState::renderMainMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
            saveSceneToDisk();
        }
        if (ImGui::MenuItem("Load Scene", "Ctrl+O", false, canEditScene())) {
            loadSceneFromDisk();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Demo Scene")) {
            if (canEditScene()) {
                commitSceneEdit();
                recordSceneHistory("Reset Demo Scene");
            }
            resetDemoScene();
        }
        if (ImGui::MenuItem("Exit")) {
            if (platform::Window* window = stack().window()) {
                window->requestClose();
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canEditScene() && hasUndo())) {
            undoSceneEdit();
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canEditScene() && hasRedo())) {
            redoSceneEdit();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate Selected", "Ctrl+D", false, canEditScene() && m_world.isAlive(m_selectedEntity))) {
            duplicateEntity(m_selectedEntity);
        }
        if (ImGui::MenuItem("Delete Selected", "Delete", false, canEditScene() && m_world.isAlive(m_selectedEntity))) {
            deleteEntity(m_selectedEntity);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("GameObject")) {
        renderCreateEntityMenu();
        ImGui::Separator();
        if (ImGui::MenuItem("Select Next", nullptr, false, m_world.aliveCount() > 1U)) {
            selectNextEntity(1);
        }
        if (ImGui::MenuItem("Select Previous", nullptr, false, m_world.aliveCount() > 1U)) {
            selectNextEntity(-1);
        }
        if (ImGui::MenuItem("Focus Selected", "F", false, m_world.isAlive(m_selectedEntity))) {
            focusSelectedEntity();
        }
        if (ImGui::MenuItem("Frame Scene", nullptr, false, computeSceneBounds().has_value())) {
            focusSceneBounds();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Transform", nullptr, false, canEditScene() && m_world.isAlive(m_selectedEntity))) {
            resetSelectedTransform();
        }
        if (ImGui::MenuItem("Drop To Ground", nullptr, false, canEditScene() && m_world.isAlive(m_selectedEntity))) {
            dropSelectedToGround();
        }
        if (ImGui::MenuItem("Zero Velocity", nullptr, false, canEditScene() && m_world.hasComponent<ecs::Rigidbody>(m_selectedEntity))) {
            zeroSelectedVelocity();
        }
        if (ImGui::MenuItem("Randomize Tint", nullptr, false, canEditScene() && m_world.hasComponent<ecs::MeshRenderer>(m_selectedEntity))) {
            randomizeSelectedTint();
        }
        if (ImGui::MenuItem("Hide Selected", nullptr, false, canEditScene() && m_world.hasComponent<ecs::MeshRenderer>(m_selectedEntity))) {
            setSelectedVisible(false);
        }
        if (ImGui::MenuItem("Show Selected", nullptr, false, canEditScene() && m_world.hasComponent<ecs::MeshRenderer>(m_selectedEntity))) {
            setSelectedVisible(true);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Zero All Velocities", nullptr, false, canEditScene())) {
            zeroAllVelocities();
        }
        if (ImGui::MenuItem("Freeze All Rigidbodies", nullptr, false, canEditScene())) {
            setAllDynamicsStatic(true);
        }
        if (ImGui::MenuItem("Unfreeze All Rigidbodies", nullptr, false, canEditScene())) {
            setAllDynamicsStatic(false);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Camera Bookmarks")) {
            for (std::size_t index = 0; index < m_cameraBookmarks.size(); ++index) {
                const std::string storeLabel = "Store " + std::to_string(index + 1U);
                const std::string recallLabel = "Recall " + std::to_string(index + 1U);
                if (ImGui::MenuItem(storeLabel.c_str())) {
                    storeCameraBookmark(index);
                }
                if (ImGui::MenuItem(recallLabel.c_str(), nullptr, false, m_cameraBookmarks[index].has_value())) {
                    recallCameraBookmark(index);
                }
                if (index + 1U < m_cameraBookmarks.size()) {
                    ImGui::Separator();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Scene")) {
        if (m_editorMode == EditorMode::Edit) {
            if (ImGui::MenuItem("Enter Play Mode", "Space")) {
                enterPlayMode();
            }
        } else {
            if (ImGui::MenuItem("Stop Play Mode", "Esc")) {
                stopPlayMode();
            }
            if (ImGui::MenuItem(m_simulationRunning ? "Pause Simulation" : "Resume Simulation")) {
                if (m_simulationRunning) {
                    pauseSimulation();
                } else {
                    startSimulation();
                }
            }
            if (ImGui::MenuItem("Launch Striker")) {
                launchStriker();
            }
            if (ImGui::MenuItem("Spawn Drop Wave")) {
                spawnDropWave();
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hover Outline", nullptr, &m_showHoverOutline);
        ImGui::MenuItem("Collider Bounds", nullptr, &m_showColliderDebug);
        ImGui::MenuItem("Local Gizmo Space", nullptr, &m_useLocalGizmoSpace);
        ImGui::MenuItem("Snap", nullptr, &m_snapEnabled);
        ImGui::Separator();
        if (ImGui::MenuItem("Lock Editor Layout", nullptr, &m_editorLayoutLocked)) {
            markEditorLayoutDirty();
        }
        if (ImGui::MenuItem("Reset Editor Layout")) {
            resetEditorLayout();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        if (ImGui::MenuItem("Toolbar", nullptr, &m_showToolbarPanel)) {
            markEditorLayoutDirty();
        }
        if (ImGui::MenuItem("Viewport", nullptr, &m_showViewportPanel)) {
            markEditorLayoutDirty();
        }
        if (ImGui::MenuItem("Scene Hierarchy", nullptr, &m_showHierarchyPanel)) {
            markEditorLayoutDirty();
        }
        if (ImGui::MenuItem("Content Browser", nullptr, &m_showProjectPanel)) {
            markEditorLayoutDirty();
        }
        if (ImGui::MenuItem("Inspector", nullptr, &m_showInspectorPanel)) {
            markEditorLayoutDirty();
        }
        if (ImGui::MenuItem("Statistics", nullptr, &m_showStatsPanel)) {
            markEditorLayoutDirty();
        }
        if (ImGui::MenuItem("Status Bar", nullptr, &m_showStatusPanel)) {
            markEditorLayoutDirty();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show All Panels")) {
            m_showToolbarPanel = true;
            m_showHierarchyPanel = true;
            m_showProjectPanel = true;
            m_showInspectorPanel = true;
            m_showStatsPanel = true;
            m_showViewportPanel = true;
            m_showStatusPanel = true;
            markEditorLayoutDirty();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Controls")) {
            m_showControlsPopup = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void GameplayState::renderToolbar() {
    if (!m_showToolbarPanel) {
        return;
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    placeEditorPanel(layout, m_toolbarDockSlot, m_resetEditorLayout);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 8.0F));
    if (ImGui::Begin(
            "Toolbar",
            &m_showToolbarPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_toolbarDockSlot),
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                    dockedPanelChromeFlags(m_toolbarDockSlot)))) {
        renderPanelDockControls(EditorPanelId::Toolbar, "Dock##Toolbar");
        ImGui::SameLine(0.0F, 18.0F);

        auto toolbarButton = [](const char* label, const bool active, const ImVec2& size = ImVec2(0.0F, 0.0F)) {
            const ImVec4 idle = ImVec4(0.15F, 0.19F, 0.24F, 1.0F);
            const ImVec4 hover = active ? ImVec4(0.27F, 0.52F, 0.88F, 1.0F) : ImVec4(0.20F, 0.28F, 0.40F, 1.0F);
            const ImVec4 pressed = active ? ImVec4(0.24F, 0.47F, 0.81F, 1.0F) : ImVec4(0.24F, 0.35F, 0.51F, 1.0F);
            const ImVec4 color = active ? ImVec4(0.24F, 0.47F, 0.81F, 1.0F) : idle;

            ImGui::PushStyleColor(ImGuiCol_Button, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, pressed);
            const bool pressedButton = ImGui::Button(label, size);
            ImGui::PopStyleColor(3);
            return pressedButton;
        };

        if (toolbarButton("Edit", m_editorMode == EditorMode::Edit, ImVec2(62.0F, 28.0F))) {
            stopPlayMode();
        }
        ImGui::SameLine();
        if (toolbarButton(
                m_editorMode == EditorMode::Play ? "Stop" : "Play",
                m_editorMode == EditorMode::Play,
                ImVec2(62.0F, 28.0F))) {
            if (m_editorMode == EditorMode::Edit) {
                enterPlayMode();
            } else {
                stopPlayMode();
            }
        }

        if (m_editorMode == EditorMode::Play) {
            ImGui::SameLine();
            if (toolbarButton(m_simulationRunning ? "Pause" : "Resume", false, ImVec2(72.0F, 28.0F))) {
                if (m_simulationRunning) {
                    pauseSimulation();
                } else {
                    startSimulation();
                }
            }
        }

        ImGui::SameLine(0.0F, 18.0F);
        if (toolbarButton("Undo", hasUndo(), ImVec2(58.0F, 28.0F))) {
            undoSceneEdit();
        }
        ImGui::SameLine();
        if (toolbarButton("Redo", hasRedo(), ImVec2(58.0F, 28.0F))) {
            redoSceneEdit();
        }
        ImGui::SameLine();
        if (toolbarButton("+ Cube", false, ImVec2(70.0F, 28.0F))) {
            createEditorEntity(EditorEntityKind::PhysicsCube);
        }
        ImGui::SameLine();
        if (toolbarButton("Dup", false, ImVec2(52.0F, 28.0F))) {
            duplicateEntity(m_selectedEntity);
        }
        ImGui::SameLine();
        if (toolbarButton("Focus", false, ImVec2(66.0F, 28.0F))) {
            focusSelectedEntity();
        }
        ImGui::SameLine();
        if (toolbarButton("Frame", false, ImVec2(62.0F, 28.0F))) {
            focusSceneBounds();
        }

        ImGui::SameLine(0.0F, 18.0F);
        if (toolbarButton("Move", m_gizmoOperation == GizmoOperation::Translate, ImVec2(64.0F, 28.0F))) {
            m_gizmoOperation = GizmoOperation::Translate;
        }
        ImGui::SameLine();
        if (toolbarButton("Rotate", m_gizmoOperation == GizmoOperation::Rotate, ImVec2(70.0F, 28.0F))) {
            m_gizmoOperation = GizmoOperation::Rotate;
        }
        ImGui::SameLine();
        if (toolbarButton("Scale", m_gizmoOperation == GizmoOperation::Scale, ImVec2(64.0F, 28.0F))) {
            m_gizmoOperation = GizmoOperation::Scale;
        }

        ImGui::SameLine(0.0F, 18.0F);
        if (toolbarButton(m_useLocalGizmoSpace ? "Local" : "World", m_useLocalGizmoSpace, ImVec2(70.0F, 28.0F))) {
            m_useLocalGizmoSpace = !m_useLocalGizmoSpace;
        }
        ImGui::SameLine();
        if (toolbarButton(m_snapEnabled ? "Snap On" : "Snap Off", m_snapEnabled, ImVec2(80.0F, 28.0F))) {
            m_snapEnabled = !m_snapEnabled;
        }

        ImGui::SameLine(0.0F, 18.0F);
        auto cameraSettings = m_cameraController.settings();
        ImGui::TextDisabled("Camera");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0F);
        if (ImGui::SliderFloat("##CameraSpeed", &cameraSettings.moveSpeed, 1.0F, 32.0F, "%.1f")) {
            m_cameraController.setSettings(cameraSettings);
        }

        if (m_editorMode == EditorMode::Play) {
            ImGui::SameLine(0.0F, 18.0F);
            if (toolbarButton("Launch Striker", false, ImVec2(110.0F, 28.0F))) {
                launchStriker();
            }
            ImGui::SameLine();
            if (toolbarButton("Drop Wave", false, ImVec2(92.0F, 28.0F))) {
                spawnDropWave();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void GameplayState::renderHierarchyPanel() {
    if (!m_showHierarchyPanel) {
        return;
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    placeEditorPanel(layout, m_hierarchyDockSlot, m_resetEditorLayout);
    if (ImGui::Begin(
            "Scene Hierarchy",
            &m_showHierarchyPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_hierarchyDockSlot),
                dockedPanelChromeFlags(m_hierarchyDockSlot)))) {
        const std::string subtitle = std::to_string(m_world.aliveCount()) + " entities";
        ImGui::TextColored(ImVec4(0.76F, 0.82F, 0.91F, 1.0F), "Scene Hierarchy");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", subtitle.c_str());
        ImGui::SameLine();
        renderPanelDockControls(EditorPanelId::Hierarchy, "Dock##Hierarchy");
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##HierarchyFilter", "Filter entities", &m_hierarchyFilter);
        ImGui::Spacing();
        if (ImGui::Button("+ Create", ImVec2(78.0F, 0.0F))) {
            ImGui::OpenPopup("HierarchyCreateEntity");
        }
        if (ImGui::BeginPopup("HierarchyCreateEntity")) {
            renderCreateEntityMenu();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate", ImVec2(78.0F, 0.0F))) {
            duplicateEntity(m_selectedEntity);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(60.0F, 0.0F))) {
            deleteEntity(m_selectedEntity);
        }
        ImGui::Spacing();

        if (ImGui::BeginChild("HierarchyList", ImVec2(0.0F, 0.0F), true)) {
            const std::vector<ecs::EntityId> entities = sortedEntities();

            bool anyVisible = false;
            for (const ecs::EntityId entity : entities) {
                if (!entityMatchesFilter(entity)) {
                    continue;
                }

                anyVisible = true;
                std::string label = entityDisplayName(entity) + "##" + std::to_string(entity);
                if (ImGui::Selectable(label.c_str(), m_selectedEntity == entity)) {
                    m_selectedEntity = entity;
                }
                if (ImGui::BeginPopupContextItem()) {
                    renderEntityContextMenu(entity);
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", componentSubtitle(m_world, entity).c_str());
                }
            }

            if (!anyVisible) {
                ImGui::TextDisabled("No entities match the current filter.");
            }
            if (ImGui::BeginPopupContextWindow("HierarchyBackgroundContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
                if (ImGui::BeginMenu("Create")) {
                    renderCreateEntityMenu();
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void GameplayState::renderProjectPanel() {
    if (!m_showProjectPanel) {
        return;
    }

    if (m_projectAssetsDirty) {
        refreshProjectAssets();
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    placeEditorPanel(layout, m_projectDockSlot, m_resetEditorLayout);
    if (ImGui::Begin(
            "Content Browser",
            &m_showProjectPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_projectDockSlot),
                dockedPanelChromeFlags(m_projectDockSlot)))) {
        ImGui::TextColored(ImVec4(0.76F, 0.82F, 0.91F, 1.0F), "Content Browser");
        ImGui::SameLine();
        ImGui::TextDisabled("%zu assets", m_projectAssets.size());
        ImGui::SameLine();
        renderPanelDockControls(EditorPanelId::Project, "Dock##ContentBrowser");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) {
            m_projectAssetsDirty = true;
            refreshProjectAssets();
        }
        ImGui::Separator();

        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##AssetFilter", "Search assets by name, type, or path", &m_assetFilter);

        auto applyAssetToSelection = [this](const std::string& assetPath, const std::string& assetKind) {
            auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(m_selectedEntity);
            const bool renderAsset = assetKind == "Texture" || assetKind == "Shader" || assetKind == "Mesh";
            if (!canEditScene()) {
                m_editorStatusMessage = "Stop Play mode before applying assets.";
                return false;
            }
            if (!renderAsset) {
                m_editorStatusMessage = "This asset type cannot be applied to a Mesh Renderer.";
                return false;
            }
            if (meshRenderer == nullptr) {
                m_editorStatusMessage = "Selected entity has no Mesh Renderer.";
                return false;
            }

            recordSceneHistory("Apply Asset");
            if (assetKind == "Texture") {
                meshRenderer->textureId = assetPath;
                meshRenderer->texture.reset();
            } else if (assetKind == "Shader") {
                meshRenderer->shaderId = assetPath;
                meshRenderer->shader.reset();
            } else if (assetKind == "Mesh") {
                meshRenderer->meshId = assetPath;
                meshRenderer->mesh.reset();
            }
            m_editorStatusMessage = "Applied " + assetPath + " to selected entity.";
            return true;
        };

        const std::string loweredFilter = toLowerCopy(m_assetFilter);
        std::size_t visibleAssetCount = 0U;

        if (ImGui::BeginChild(
                "ProjectAssetTiles",
                ImVec2(0.0F, -74.0F),
                true,
                ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            constexpr float tileWidth = 94.0F;
            constexpr float tileHeight = 112.0F;
            constexpr float tileGap = 8.0F;
            const float availableWidth = std::max(1.0F, ImGui::GetContentRegionAvail().x);
            const int columns = std::max(1, static_cast<int>((availableWidth + tileGap) / (tileWidth + tileGap)));
            int column = 0;

            for (const AssetEntry& asset : m_projectAssets) {
                if (!assetMatchesFilter(asset.path, asset.name, asset.kind, loweredFilter)) {
                    continue;
                }
                ++visibleAssetCount;

                ImGui::PushID(asset.path.c_str());
                const bool selected = m_selectedAssetPath == asset.path;
                const ImVec2 tileMin = ImGui::GetCursorScreenPos();
                const ImVec2 tileSize(tileWidth, tileHeight);
                ImGui::InvisibleButton("##AssetTile", tileSize);

                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    m_selectedAssetPath = asset.path;
                    m_editorStatusMessage = "Selected asset " + asset.path;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%s", asset.path.c_str(), asset.details.c_str());
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_selectedAssetPath = asset.path;
                    if (asset.kind == "Scene") {
                        if (asset.path == kEditorScenePath) {
                            loadSceneFromDisk();
                        } else {
                            m_editorStatusMessage = "Only editor_scene.json can be loaded for now.";
                        }
                    } else {
                        applyAssetToSelection(asset.path, asset.kind);
                    }
                }

                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 tileMax(tileMin.x + tileWidth, tileMin.y + tileHeight);
                const ImVec4 accent = assetKindColor(asset.kind);
                const ImU32 cardColor = ImGui::GetColorU32(
                    selected ? ImVec4(0.18F, 0.27F, 0.38F, 1.0F) : ImVec4(0.105F, 0.125F, 0.155F, 1.0F));
                const ImU32 borderColor = ImGui::GetColorU32(
                    selected ? ImVec4(0.34F, 0.62F, 0.96F, 1.0F) : ImVec4(0.20F, 0.24F, 0.30F, 1.0F));
                drawList->AddRectFilled(tileMin, tileMax, cardColor, 8.0F);
                drawList->AddRect(tileMin, tileMax, borderColor, 8.0F, 0, selected ? 2.0F : 1.0F);

                const ImVec2 iconMin(tileMin.x + 14.0F, tileMin.y + 10.0F);
                const ImVec2 iconMax(tileMax.x - 14.0F, tileMin.y + 64.0F);
                drawList->AddRectFilled(iconMin, iconMax, ImGui::GetColorU32(ImVec4(0.07F, 0.09F, 0.12F, 1.0F)), 7.0F);
                if (asset.hasPixelPreview && asset.previewWidth > 0U && asset.previewHeight > 0U) {
                    const float cellWidth = (iconMax.x - iconMin.x) / static_cast<float>(asset.previewWidth);
                    const float cellHeight = (iconMax.y - iconMin.y) / static_cast<float>(asset.previewHeight);
                    for (std::uint32_t y = 0; y < asset.previewHeight; ++y) {
                        for (std::uint32_t x = 0; x < asset.previewWidth; ++x) {
                            const ImVec2 cellMin(iconMin.x + static_cast<float>(x) * cellWidth, iconMin.y + static_cast<float>(y) * cellHeight);
                            const ImVec2 cellMax(cellMin.x + cellWidth + 0.5F, cellMin.y + cellHeight + 0.5F);
                            drawList->AddRectFilled(
                                cellMin,
                                cellMax,
                                asset.previewPixels[static_cast<std::size_t>(y * asset.previewWidth + x)]);
                        }
                    }
                } else if (asset.kind == "Texture") {
                    const ImU32 a = ImGui::GetColorU32(ImVec4(0.22F, 0.26F, 0.32F, 1.0F));
                    const ImU32 b = ImGui::GetColorU32(accent);
                    const float cell = 12.0F;
                    for (int y = 0; y < 5; ++y) {
                        for (int x = 0; x < 5; ++x) {
                            const ImVec2 cellMin(iconMin.x + static_cast<float>(x) * cell, iconMin.y + static_cast<float>(y) * cell);
                            drawList->AddRectFilled(cellMin, ImVec2(cellMin.x + cell, cellMin.y + cell), ((x + y) % 2) == 0 ? a : b);
                        }
                    }
                } else if (asset.kind == "Mesh") {
                    const ImU32 line = ImGui::GetColorU32(accent);
                    const ImVec2 frontMin(iconMin.x + 16.0F, iconMin.y + 20.0F);
                    const ImVec2 frontMax(iconMax.x - 18.0F, iconMax.y - 10.0F);
                    const ImVec2 offset(10.0F, -10.0F);
                    const ImVec2 backMin(frontMin.x + offset.x, frontMin.y + offset.y);
                    const ImVec2 backMax(frontMax.x + offset.x, frontMax.y + offset.y);
                    drawList->AddQuadFilled(
                        backMin,
                        ImVec2(backMax.x, backMin.y),
                        backMax,
                        ImVec2(backMin.x, backMax.y),
                        ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.22F)));
                    drawList->AddRectFilled(frontMin, frontMax, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.36F)), 3.0F);
                    drawList->AddRect(frontMin, frontMax, line, 3.0F, 0, 2.0F);
                    drawList->AddRect(backMin, backMax, line, 3.0F, 0, 2.0F);
                    drawList->AddLine(frontMin, backMin, line, 1.5F);
                    drawList->AddLine(ImVec2(frontMax.x, frontMin.y), ImVec2(backMax.x, backMin.y), line, 1.5F);
                    drawList->AddLine(frontMax, backMax, line, 1.5F);
                    drawList->AddLine(ImVec2(frontMin.x, frontMax.y), ImVec2(backMin.x, backMax.y), line, 1.5F);
                } else if (asset.kind == "Shader" || asset.kind == "Shader Source") {
                    const ImU32 line = ImGui::GetColorU32(accent);
                    for (int i = 0; i < 5; ++i) {
                        const float y = iconMin.y + 10.0F + static_cast<float>(i) * 8.0F;
                        const float width = (i % 2 == 0) ? 42.0F : 30.0F;
                        drawList->AddRectFilled(ImVec2(iconMin.x + 10.0F, y), ImVec2(iconMin.x + 10.0F + width, y + 3.0F), line, 1.5F);
                    }
                } else if (asset.kind == "Scene") {
                    const ImU32 line = ImGui::GetColorU32(accent);
                    drawList->AddRect(ImVec2(iconMin.x + 12.0F, iconMin.y + 10.0F), ImVec2(iconMin.x + 38.0F, iconMin.y + 28.0F), line, 3.0F, 0, 2.0F);
                    drawList->AddRect(ImVec2(iconMax.x - 38.0F, iconMax.y - 28.0F), ImVec2(iconMax.x - 12.0F, iconMax.y - 10.0F), line, 3.0F, 0, 2.0F);
                    drawList->AddLine(ImVec2(iconMin.x + 38.0F, iconMin.y + 20.0F), ImVec2(iconMax.x - 38.0F, iconMax.y - 20.0F), line, 2.0F);
                } else {
                    drawList->AddRectFilled(iconMin, iconMax, ImGui::GetColorU32(accent), 7.0F);
                    const char* glyph = assetKindGlyph(asset.kind);
                    const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
                    drawList->AddText(
                        ImVec2(
                            iconMin.x + ((iconMax.x - iconMin.x) - glyphSize.x) * 0.5F,
                            iconMin.y + ((iconMax.y - iconMin.y) - glyphSize.y) * 0.5F),
                        ImGui::GetColorU32(ImVec4(0.06F, 0.08F, 0.10F, 1.0F)),
                        glyph);
                }
                drawList->AddRect(
                    ImVec2(iconMin.x + 2.0F, iconMin.y + 2.0F),
                    ImVec2(iconMax.x - 2.0F, iconMax.y - 2.0F),
                    ImGui::GetColorU32(ImVec4(1.0F, 1.0F, 1.0F, 0.18F)),
                    5.0F);

                const std::string title = ellipsizeText(asset.name, 14U);
                const std::string kind = ellipsizeText(asset.kind, 13U);
                drawList->AddText(
                    ImVec2(tileMin.x + 8.0F, tileMin.y + 72.0F),
                    ImGui::GetColorU32(ImVec4(0.86F, 0.90F, 0.96F, 1.0F)),
                    title.c_str());
                drawList->AddText(
                    ImVec2(tileMin.x + 8.0F, tileMin.y + 92.0F),
                    ImGui::GetColorU32(ImVec4(0.52F, 0.59F, 0.68F, 1.0F)),
                    kind.c_str());

                ++column;
                if (column < columns) {
                    ImGui::SameLine(0.0F, tileGap);
                } else {
                    column = 0;
                }
                ImGui::PopID();
            }

            if (visibleAssetCount == 0U) {
                ImGui::TextDisabled("No matching assets.");
            }
        }
        ImGui::EndChild();

        const bool hasAsset = !m_selectedAssetPath.empty();
        const auto selectedAssetIt = std::find_if(m_projectAssets.begin(), m_projectAssets.end(), [this](const AssetEntry& asset) {
            return asset.path == m_selectedAssetPath;
        });
        const AssetEntry* selectedAsset = selectedAssetIt != m_projectAssets.end() ? &*selectedAssetIt : nullptr;
        const std::string selectedKind =
            selectedAsset != nullptr ? selectedAsset->kind
                                     : (hasAsset ? classifyAsset(std::filesystem::path(m_selectedAssetPath)) : std::string{});
        ImGui::TextDisabled(
            "%s",
            selectedAsset != nullptr ? ellipsizeText(selectedAsset->name + " - " + selectedAsset->details, 58U).c_str()
                                     : (hasAsset ? ellipsizeText(m_selectedAssetPath, 58U).c_str() : "No asset selected"));

        auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(m_selectedEntity);
        const bool assetAppliesToRenderer = selectedKind == "Texture" || selectedKind == "Shader" || selectedKind == "Mesh";
        const bool canApplyAsset = canEditScene() && meshRenderer != nullptr && hasAsset && assetAppliesToRenderer;
        if (!canApplyAsset) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Apply", ImVec2(72.0F, 0.0F))) {
            applyAssetToSelection(m_selectedAssetPath, selectedKind);
        }
        if (!canApplyAsset) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        const bool canLoadSceneAsset = canEditScene() && hasAsset && selectedKind == "Scene";
        if (!canLoadSceneAsset) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Load Scene", ImVec2(96.0F, 0.0F))) {
            if (m_selectedAssetPath == kEditorScenePath) {
                loadSceneFromDisk();
            } else {
                m_editorStatusMessage = "Only editor_scene.json can be loaded for now.";
            }
        }
        if (!canLoadSceneAsset) {
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
}

void GameplayState::renderEntityComponentEditors(const ecs::EntityId entity) {
    auto captureBeforeEdit = [this]() {
        return canEditScene() ? captureSceneSnapshot() : SceneSnapshot{};
    };

    auto drawVec3Editor = [this, &captureBeforeEdit](const char* label, glm::vec3& value, const float speed, const char* format) {
        const SceneSnapshot before = captureBeforeEdit();
        bool changed = false;
        ImGui::PushID(label);
        if (ImGui::BeginTable("##Vec3Table", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0F);
            changed = ImGui::DragFloat3("##Value", glm::value_ptr(value), speed, 0.0F, 0.0F, format);
            trackEditedItem(std::string("Edit ") + label, changed, before);
            ImGui::EndTable();
        }
        ImGui::PopID();
        return changed;
    };

    if (auto* transform = m_world.getComponent<ecs::Transform>(entity)) {
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (drawVec3Editor("Position", transform->position, 0.05F, "%.2f")) {
                wakeEntity(entity);
            }

            glm::vec3 rotationDegrees = glm::degrees(transform->rotationEulerRadians);
            if (drawVec3Editor("Rotation", rotationDegrees, 0.3F, "%.1f deg")) {
                transform->rotationEulerRadians = glm::radians(rotationDegrees);
                wakeEntity(entity);
            }

            if (drawVec3Editor("Scale", transform->scale, 0.03F, "%.2f")) {
                transform->scale = glm::max(transform->scale, glm::vec3(0.05F));
                wakeEntity(entity);
            }
        }
    }

    if (auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(entity)) {
        if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
            SceneSnapshot before = captureBeforeEdit();
            if (const bool changed = ImGui::Checkbox("Visible", &meshRenderer->visible)) {
                trackEditedItem("Edit Mesh Visibility", changed, before);
                wakeEntity(entity);
            }
            before = captureBeforeEdit();
            trackEditedItem(
                "Edit Mesh Tint",
                ImGui::ColorEdit4("Tint", glm::value_ptr(meshRenderer->tint)),
                before);
            before = captureBeforeEdit();
            trackEditedItem(
                "Edit UV Scale",
                ImGui::DragFloat2("UV Scale", glm::value_ptr(meshRenderer->uvScale), 0.05F, 0.05F, 16.0F, "%.2f"),
                before);

            static constexpr const char* kPrimitiveLabels[] = {"Triangle"};
            int primitiveIndex = static_cast<int>(meshRenderer->primitiveType);
            before = captureBeforeEdit();
            if (ImGui::Combo("Primitive", &primitiveIndex, kPrimitiveLabels, IM_ARRAYSIZE(kPrimitiveLabels))) {
                meshRenderer->primitiveType = static_cast<ecs::PrimitiveType>(primitiveIndex);
                trackEditedItem("Edit Primitive", true, before);
            }

            before = captureBeforeEdit();
            const bool meshChanged = ImGui::InputText("Mesh", &meshRenderer->meshId);
            trackEditedItem("Edit Mesh Resource", meshChanged, before);
            before = captureBeforeEdit();
            const bool textureChanged = ImGui::InputText("Texture", &meshRenderer->textureId);
            trackEditedItem("Edit Texture Resource", textureChanged, before);
            before = captureBeforeEdit();
            const bool shaderChanged = ImGui::InputText("Shader", &meshRenderer->shaderId);
            trackEditedItem("Edit Shader Resource", shaderChanged, before);
            if (meshChanged) {
                meshRenderer->mesh.reset();
            }
            if (textureChanged) {
                meshRenderer->texture.reset();
            }
            if (shaderChanged) {
                meshRenderer->shader.reset();
            }
        }
    }

    if (auto* camera = m_world.getComponent<ecs::Camera>(entity)) {
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            SceneSnapshot before = captureBeforeEdit();
            trackEditedItem("Edit Camera Active", ImGui::Checkbox("Active", &camera->active), before);
            float fovDegrees = glm::degrees(camera->verticalFovRadians);
            before = captureBeforeEdit();
            if (ImGui::SliderFloat("Vertical FOV", &fovDegrees, 20.0F, 110.0F, "%.1f deg")) {
                camera->verticalFovRadians = glm::radians(fovDegrees);
                trackEditedItem("Edit Camera FOV", true, before);
            }
            before = captureBeforeEdit();
            trackEditedItem(
                "Edit Near Plane",
                ImGui::DragFloat("Near Plane", &camera->nearPlane, 0.01F, 0.01F, camera->farPlane - 0.1F, "%.2f"),
                before);
            before = captureBeforeEdit();
            trackEditedItem(
                "Edit Far Plane",
                ImGui::DragFloat("Far Plane", &camera->farPlane, 1.0F, camera->nearPlane + 0.1F, 1000.0F, "%.1f"),
                before);
        }
    }

    if (auto* rigidbody = m_world.getComponent<ecs::Rigidbody>(entity)) {
        if (ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool massPropertiesDirty = false;
            bool changed = false;
            SceneSnapshot before = captureBeforeEdit();
            bool fieldChanged = ImGui::Checkbox("Use Gravity", &rigidbody->useGravity);
            trackEditedItem("Edit Gravity", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = ImGui::Checkbox("Static", &rigidbody->isStatic);
            trackEditedItem("Edit Static Flag", fieldChanged, before);
            changed |= fieldChanged;
            massPropertiesDirty |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = ImGui::Checkbox("Kinematic", &rigidbody->isKinematic);
            trackEditedItem("Edit Kinematic Flag", fieldChanged, before);
            changed |= fieldChanged;
            massPropertiesDirty |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = ImGui::DragFloat("Mass", &rigidbody->mass, 0.05F, 0.0F, 100.0F, "%.2f");
            trackEditedItem("Edit Mass", fieldChanged, before);
            changed |= fieldChanged;
            massPropertiesDirty |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = ImGui::DragFloat("Restitution", &rigidbody->restitution, 0.01F, 0.0F, 1.0F, "%.2f");
            trackEditedItem("Edit Restitution", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = ImGui::DragFloat("Friction", &rigidbody->friction, 0.01F, 0.0F, 2.0F, "%.2f");
            trackEditedItem("Edit Friction", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = ImGui::DragFloat("Linear Damping", &rigidbody->linearDamping, 0.01F, 0.0F, 3.0F, "%.2f");
            trackEditedItem("Edit Linear Damping", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = ImGui::DragFloat("Angular Damping", &rigidbody->angularDamping, 0.01F, 0.0F, 3.0F, "%.2f");
            trackEditedItem("Edit Angular Damping", fieldChanged, before);
            changed |= fieldChanged;
            ImGui::TextDisabled("Runtime");
            before = captureBeforeEdit();
            fieldChanged =
                ImGui::DragFloat3("Velocity", glm::value_ptr(rigidbody->velocity), 0.05F, -50.0F, 50.0F, "%.2f");
            trackEditedItem("Edit Velocity", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = ImGui::DragFloat3(
                "Angular Velocity",
                glm::value_ptr(rigidbody->angularVelocity),
                0.05F,
                -50.0F,
                50.0F,
                "%.2f");
            trackEditedItem("Edit Angular Velocity", fieldChanged, before);
            changed |= fieldChanged;
            if (massPropertiesDirty) {
                rigidbody->recalculateMassProperties();
            }
            if (changed) {
                wakeEntity(entity);
            }
        }
    }

    if (auto* collider = m_world.getComponent<ecs::Collider>(entity)) {
        if (ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
            SceneSnapshot before = captureBeforeEdit();
            trackEditedItem("Edit Collider Enabled", ImGui::Checkbox("Enabled", &collider->enabled), before);
            drawVec3Editor("Offset", collider->offset, 0.02F, "%.2f");

            static constexpr const char* kColliderLabels[] = {"AABB", "Sphere"};
            int colliderIndex = static_cast<int>(collider->type);
            before = captureBeforeEdit();
            if (ImGui::Combo("Shape", &colliderIndex, kColliderLabels, IM_ARRAYSIZE(kColliderLabels))) {
                collider->type = static_cast<ecs::ColliderType>(colliderIndex);
                trackEditedItem("Edit Collider Shape", true, before);
            }

            if (collider->type == ecs::ColliderType::Aabb) {
                drawVec3Editor("Half Extents", collider->aabb.halfExtents, 0.02F, "%.2f");
                collider->aabb.halfExtents = glm::max(collider->aabb.halfExtents, glm::vec3(0.05F));
            } else {
                before = captureBeforeEdit();
                trackEditedItem(
                    "Edit Sphere Radius",
                    ImGui::DragFloat("Radius", &collider->sphere.radius, 0.02F, 0.05F, 50.0F, "%.2f"),
                    before);
            }
        }
    }

    if (auto* hierarchy = m_world.getComponent<ecs::Hierarchy>(entity)) {
        if (ImGui::CollapsingHeader("Hierarchy")) {
            const ecs::EntityId currentParent = hierarchy->parent;
            const std::string preview =
                currentParent == ecs::kInvalidEntity ? std::string("None") : entityDisplayName(currentParent);
            if (ImGui::BeginCombo("Parent", preview.c_str())) {
                const bool noParentSelected = hierarchy->parent == ecs::kInvalidEntity;
                if (ImGui::Selectable("None", noParentSelected)) {
                    recordSceneHistory("Change Parent");
                    hierarchy->parent = ecs::kInvalidEntity;
                    m_editorStatusMessage = "Changed parent";
                }

                m_world.forEachEntity([&](const ecs::EntityId candidate) {
                    if (!isHierarchyParentValid(m_world, entity, candidate)) {
                        return;
                    }
                    const bool selected = hierarchy->parent == candidate;
                    if (ImGui::Selectable(entityDisplayName(candidate).c_str(), selected)) {
                        recordSceneHistory("Change Parent");
                        hierarchy->parent = candidate;
                        m_editorStatusMessage = "Changed parent";
                    }
                });
                ImGui::EndCombo();
            }
        }
    }
}

void GameplayState::renderInspectorPanel() {
    if (!m_showInspectorPanel) {
        return;
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    placeEditorPanel(layout, m_inspectorDockSlot, m_resetEditorLayout);
    if (ImGui::Begin(
            "Inspector",
            &m_showInspectorPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_inspectorDockSlot),
                dockedPanelChromeFlags(m_inspectorDockSlot)))) {
        ImGui::TextColored(ImVec4(0.76F, 0.82F, 0.91F, 1.0F), "Inspector");
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%s",
            m_selectedEntity == ecs::kInvalidEntity ? "No selection" : componentSubtitle(m_world, m_selectedEntity).c_str());
        ImGui::SameLine();
        renderPanelDockControls(EditorPanelId::Inspector, "Dock##Inspector");
        ImGui::Separator();

        if (m_selectedEntity == ecs::kInvalidEntity || !m_world.isAlive(m_selectedEntity)) {
            ImGui::TextDisabled("Select an entity in the hierarchy to inspect its components.");
            ImGui::End();
            return;
        }

        if (ImGui::Button("Add Component", ImVec2(126.0F, 0.0F))) {
            ImGui::OpenPopup("InspectorAddComponent");
        }
        if (ImGui::BeginPopup("InspectorAddComponent")) {
            renderAddComponentMenu(m_selectedEntity);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove", ImVec2(78.0F, 0.0F))) {
            ImGui::OpenPopup("InspectorRemoveComponent");
        }
        if (ImGui::BeginPopup("InspectorRemoveComponent")) {
            renderRemoveComponentMenu(m_selectedEntity);
            ImGui::EndPopup();
        }
        if (ImGui::Button("Duplicate", ImVec2(102.0F, 0.0F))) {
            duplicateEntity(m_selectedEntity);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(82.0F, 0.0F))) {
            deleteEntity(m_selectedEntity);
            ImGui::End();
            return;
        }
        ImGui::Separator();

        if (ImGui::Button("Reset Xform", ImVec2(102.0F, 0.0F))) {
            resetSelectedTransform();
        }
        ImGui::SameLine();
        if (ImGui::Button("Drop", ImVec2(58.0F, 0.0F))) {
            dropSelectedToGround();
        }
        ImGui::SameLine();
        if (ImGui::Button("Tint", ImVec2(56.0F, 0.0F))) {
            randomizeSelectedTint();
        }
        ImGui::SameLine();
        if (ImGui::Button("Zero Vel", ImVec2(74.0F, 0.0F))) {
            zeroSelectedVelocity();
        }
        ImGui::Separator();

        if (auto* tag = m_world.getComponent<ecs::Tag>(m_selectedEntity)) {
            const SceneSnapshot before = canEditScene() ? captureSceneSnapshot() : SceneSnapshot{};
            const bool changed = ImGui::InputText("Name", &tag->value);
            trackEditedItem("Rename Entity", changed, before);
        } else if (ImGui::Button("Add Tag")) {
            recordSceneHistory("Add Tag");
            m_world.addComponent<ecs::Tag>(m_selectedEntity, ecs::Tag{entityDisplayName(m_selectedEntity)});
            m_editorStatusMessage = "Added Tag";
        }

        if (m_editorMode == EditorMode::Play) {
            ImGui::Spacing();
            ImGui::TextColored(
                ImVec4(0.97F, 0.78F, 0.29F, 1.0F),
                "Play mode changes are temporary and will be discarded on Stop.");
        }

        ImGui::Spacing();
        renderEntityComponentEditors(m_selectedEntity);
    }
    ImGui::End();
}

void GameplayState::renderStatsPanel(const renderer::Renderer& renderer) {
    if (!m_showStatsPanel) {
        return;
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    placeEditorPanel(layout, m_statsDockSlot, m_resetEditorLayout);
    if (ImGui::Begin(
            "Statistics",
            &m_showStatsPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_statsDockSlot),
                dockedPanelChromeFlags(m_statsDockSlot)))) {
        ImGui::TextColored(ImVec4(0.76F, 0.82F, 0.91F, 1.0F), "Statistics");
        ImGui::SameLine();
        ImGui::TextDisabled("Runtime");
        ImGui::SameLine();
        renderPanelDockControls(EditorPanelId::Stats, "Dock##Stats");
        ImGui::Separator();

        float fps = ImGui::GetIO().Framerate;
        if (fps < 0.001F) {
            fps = 0.0F;
        }
        const float frameTimeMs = fps > 0.001F ? 1000.0F / fps : 0.0F;

        std::size_t cameraCount = 0;
        std::size_t renderableCount = 0;
        std::size_t dynamicBodyCount = 0;
        m_world.forEachEntity([&](const ecs::EntityId entity) {
            if (m_world.hasComponent<ecs::Camera>(entity)) {
                ++cameraCount;
            }
            if (m_world.hasComponent<ecs::MeshRenderer>(entity)) {
                ++renderableCount;
            }
            if (const auto* rigidbody = m_world.getComponent<ecs::Rigidbody>(entity);
                rigidbody != nullptr && rigidbody->isDynamic()) {
                ++dynamicBodyCount;
            }
        });

        const rhi::Extent2D extent = renderer.frameExtent();
        ImGui::Text("FPS: %.1f", fps);
        ImGui::Text("Frame: %.2f ms", frameTimeMs);
        ImGui::Text("Scene render CPU: %.3f ms", m_lastSceneRenderMs);
        ImGui::PlotLines(
            "Frame ms",
            m_frameTimeHistory.data(),
            static_cast<int>(m_frameTimeHistory.size()),
            static_cast<int>(m_historyCursor),
            nullptr,
            0.0F,
            40.0F,
            ImVec2(0.0F, 42.0F));
        ImGui::PlotLines(
            "Render ms",
            m_renderTimeHistory.data(),
            static_cast<int>(m_renderTimeHistory.size()),
            static_cast<int>(m_historyCursor),
            nullptr,
            0.0F,
            12.0F,
            ImVec2(0.0F, 42.0F));
        ImGui::Separator();
        ImGui::Text("Entities: %zu", m_world.aliveCount());
        ImGui::Text("Renderable: %zu", renderableCount);
        ImGui::Text("Cameras: %zu", cameraCount);
        ImGui::Text("Dynamic bodies: %zu", dynamicBodyCount);
        ImGui::Text("Physics bodies: %zu", m_physicsSystem.bodyCount());
        ImGui::Text("Sleeping bodies: %zu", m_physicsSystem.sleepingBodyCount());
        ImGui::Text("Contacts: %zu", m_physicsSystem.activeCollisionCount());
        ImGui::Text("Undo / Redo: %zu / %zu", m_undoStack.size(), m_redoStack.size());
        ImGui::Text("Viewport: %.0f x %.0f", m_viewportSize.x, m_viewportSize.y);
        ImGui::Text("Backbuffer: %u x %u", extent.width, extent.height);
        ImGui::Text("Queued spawns: %zu", m_pendingSpawnJobs.size());
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_lastCollisionMessage.c_str());
    }
    ImGui::End();
}

void GameplayState::renderViewportOverlay(const CameraFrame& cameraFrame) const {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 overlayMin(m_viewportPosition.x + 12.0F, m_viewportPosition.y + 12.0F);
    const ImVec2 overlayMax(overlayMin.x + 312.0F, overlayMin.y + 74.0F);
    const std::string aspectText = "AR " + std::to_string(cameraFrame.aspectRatio).substr(0, 4);
    const std::string hoverText = m_hoveredEntity == ecs::kInvalidEntity
                                      ? std::string("RMB + WASDQE camera | W/E/R gizmos | Space play")
                                      : "Hover: " + entityDisplayName(m_hoveredEntity);

    drawList->AddRectFilled(overlayMin, overlayMax, IM_COL32(10, 14, 18, 180), 8.0F);
    drawList->AddRect(overlayMin, overlayMax, IM_COL32(58, 73, 95, 220), 8.0F);
    drawList->AddText(ImVec2(overlayMin.x + 12.0F, overlayMin.y + 10.0F), IM_COL32(226, 232, 240, 255), "Scene View");
    drawList->AddText(
        ImVec2(overlayMin.x + 12.0F, overlayMin.y + 30.0F),
        IM_COL32(142, 163, 186, 255),
        m_editorMode == EditorMode::Play ? "Mode: Play" : "Mode: Edit");
    drawList->AddText(
        ImVec2(overlayMin.x + 132.0F, overlayMin.y + 30.0F),
        IM_COL32(142, 163, 186, 255),
        m_useLocalGizmoSpace ? "Space: Local" : "Space: World");
    drawList->AddText(
        ImVec2(overlayMin.x + 12.0F, overlayMin.y + 50.0F),
        IM_COL32(123, 145, 170, 255),
        hoverText.c_str());
    drawList->AddText(
        ImVec2(overlayMin.x + 236.0F, overlayMin.y + 30.0F),
        IM_COL32(123, 145, 170, 255),
        aspectText.c_str());
}

void GameplayState::renderViewportGizmo(const CameraFrame& cameraFrame) {
    if (m_selectedEntity == ecs::kInvalidEntity || !m_world.isAlive(m_selectedEntity)) {
        return;
    }

    auto* transform = m_world.getComponent<ecs::Transform>(m_selectedEntity);
    if (transform == nullptr) {
        return;
    }

    ImGuizmo::BeginFrame();
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(m_viewportPosition.x, m_viewportPosition.y, m_viewportSize.x, m_viewportSize.y);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    switch (m_gizmoOperation) {
        case GizmoOperation::Translate:
            operation = ImGuizmo::TRANSLATE;
            break;
        case GizmoOperation::Rotate:
            operation = ImGuizmo::ROTATE;
            break;
        case GizmoOperation::Scale:
            operation = ImGuizmo::SCALE;
            break;
    }

    ImGuizmo::MODE mode =
        (m_gizmoOperation == GizmoOperation::Scale || m_useLocalGizmoSpace) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    glm::mat4 worldMatrix = ecs::computeWorldMatrix(m_world, m_selectedEntity);
    glm::mat4 localMatrix = worldMatrix;
    if (const auto* hierarchy = m_world.getComponent<ecs::Hierarchy>(m_selectedEntity);
        hierarchy != nullptr && hierarchy->parent != ecs::kInvalidEntity) {
        const glm::mat4 parentWorld = ecs::computeWorldMatrix(m_world, hierarchy->parent);
        localMatrix = glm::inverse(parentWorld) * worldMatrix;
    }

    float snapValues[3]{0.5F, 0.5F, 0.5F};
    if (m_gizmoOperation == GizmoOperation::Rotate) {
        snapValues[0] = 15.0F;
    } else if (m_gizmoOperation == GizmoOperation::Scale) {
        snapValues[0] = 0.1F;
        snapValues[1] = 0.1F;
        snapValues[2] = 0.1F;
    }

    glm::mat4 deltaMatrix(1.0F);
    const bool manipulated = ImGuizmo::Manipulate(
            glm::value_ptr(cameraFrame.view),
            glm::value_ptr(cameraFrame.projection),
            operation,
            mode,
            glm::value_ptr(worldMatrix),
            glm::value_ptr(deltaMatrix),
            m_snapEnabled ? snapValues : nullptr);

    if (manipulated) {
        if (canEditScene() && !m_gizmoEditSnapshot.has_value()) {
            m_gizmoEditSnapshot = captureSceneSnapshot();
        }

        if (const auto* hierarchy = m_world.getComponent<ecs::Hierarchy>(m_selectedEntity);
            hierarchy != nullptr && hierarchy->parent != ecs::kInvalidEntity) {
            const glm::mat4 parentWorld = ecs::computeWorldMatrix(m_world, hierarchy->parent);
            localMatrix = glm::inverse(parentWorld) * worldMatrix;
        } else {
            localMatrix = worldMatrix;
        }

        const std::optional<TransformComponents> components =
            decomposeEditorTransform(localMatrix, transform->rotationEulerRadians, transform->scale);
        if (components.has_value()) {
            transform->position = components->position;
            switch (m_gizmoOperation) {
                case GizmoOperation::Translate:
                    break;
                case GizmoOperation::Rotate:
                    transform->rotationEulerRadians = components->rotationEulerRadians;
                    break;
                case GizmoOperation::Scale:
                    transform->scale = components->scale;
                    break;
            }

            if (auto* rigidbody = m_world.getComponent<ecs::Rigidbody>(m_selectedEntity)) {
                stabilizeDirectlyEditedBody(*rigidbody);
            }
            m_gizmoChanged = true;
        }
    }

    const bool gizmoUsing = ImGuizmo::IsUsing();
    if (m_gizmoWasUsing && !gizmoUsing) {
        if (canEditScene() && m_gizmoChanged && m_gizmoEditSnapshot.has_value()) {
            pushHistorySnapshot("Gizmo Transform", std::move(*m_gizmoEditSnapshot));
            m_editorStatusMessage = "Gizmo Transform";
        }
        m_gizmoEditSnapshot.reset();
        m_gizmoChanged = false;
    }
    m_gizmoWasUsing = gizmoUsing;
}

void GameplayState::renderViewportPanel(renderer::Renderer& renderer) {
    if (!m_showViewportPanel) {
        m_viewportHovered = false;
        m_viewportFocused = false;
        return;
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    placeEditorPanel(layout, m_viewportDockSlot, m_resetEditorLayout);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    if (ImGui::Begin(
            "Viewport",
            &m_showViewportPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_viewportDockSlot),
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                    dockedPanelChromeFlags(m_viewportDockSlot)))) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 windowPos = ImGui::GetCursorScreenPos();
        const ImVec2 windowSize = ImGui::GetContentRegionAvail();
        const std::string selectionLabel =
            m_selectedEntity == ecs::kInvalidEntity ? std::string("No Selection") : entityDisplayName(m_selectedEntity);

        drawList->AddRectFilled(windowPos, windowPos + windowSize, IM_COL32(7, 10, 13, 38), 8.0F);
        drawList->AddRectFilled(
            windowPos,
            ImVec2(windowPos.x + windowSize.x, windowPos.y + kViewportHeaderHeight),
            IM_COL32(11, 16, 22, 205),
            8.0F,
            ImDrawFlags_RoundCornersTop);
        drawList->AddRect(windowPos, windowPos + windowSize, IM_COL32(58, 73, 95, 215), 8.0F);
        drawList->AddText(
            ImVec2(windowPos.x + 12.0F, windowPos.y + 10.0F),
            IM_COL32(226, 232, 240, 255),
            "Viewport");
        drawList->AddText(
            ImVec2(windowPos.x + windowSize.x - 160.0F, windowPos.y + 10.0F),
            IM_COL32(142, 163, 186, 255),
            selectionLabel.c_str());

        m_viewportPosition = glm::vec2(windowPos.x, windowPos.y + kViewportHeaderHeight);
        m_viewportSize = glm::vec2(windowSize.x, std::max(1.0F, windowSize.y - kViewportHeaderHeight));
        m_viewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        m_viewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        const CameraFrame cameraFrame = buildActiveCameraFrame(renderer);
        updateHoveredEntity(cameraFrame);
        renderViewportOverlay(cameraFrame);
        renderViewportGizmo(cameraFrame);
        handleViewportPicking(cameraFrame);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void GameplayState::renderStatusBar() {
    if (!m_showStatusPanel) {
        return;
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit);
    placeEditorPanel(layout, m_statusDockSlot, m_resetEditorLayout);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 6.0F));
    if (ImGui::Begin(
            "Status Bar",
            &m_showStatusPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_statusDockSlot),
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                    dockedPanelChromeFlags(m_statusDockSlot)))) {
        ImGui::TextDisabled("%s", m_editorMode == EditorMode::Play ? "PLAY MODE" : "EDIT MODE");
        ImGui::SameLine(0.0F, 18.0F);
        renderPanelDockControls(EditorPanelId::Status, "Dock##Status");
        ImGui::SameLine(0.0F, 18.0F);
        ImGui::TextDisabled("Selection");
        ImGui::SameLine();
        ImGui::TextUnformatted(
            m_selectedEntity == ecs::kInvalidEntity ? "None" : entityDisplayName(m_selectedEntity).c_str());
        ImGui::SameLine(0.0F, 18.0F);
        ImGui::TextDisabled("Camera Speed");
        ImGui::SameLine();
        ImGui::Text("%.1f", m_cameraController.settings().moveSpeed);
        ImGui::SameLine(0.0F, 18.0F);
        ImGui::TextDisabled("History");
        ImGui::SameLine();
        ImGui::Text("%zu/%zu", m_undoStack.size(), m_redoStack.size());
        ImGui::SameLine(0.0F, 18.0F);
        ImGui::TextDisabled("Status");
        ImGui::SameLine();
        ImGui::TextUnformatted(m_editorStatusMessage.c_str());
        ImGui::SameLine(0.0F, 18.0F);
        ImGui::TextDisabled("Hint");
        ImGui::SameLine();
        ImGui::TextUnformatted("Click scene select | F focus | Ctrl+S save");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void GameplayState::renderEditorUi(renderer::Renderer& renderer) {
    processEditorShortcuts();
    updateFrameHistory();
    renderMainMenuBar();
    renderToolbar();
    renderViewportPanel(renderer);
    renderHierarchyPanel();
    renderProjectPanel();
    renderInspectorPanel();
    renderStatsPanel(renderer);
    renderStatusBar();
    renderDockResizeHandles();
    renderDockDropOverlay();
    applyViewportHotkeys();

    if (m_showControlsPopup) {
        ImGui::OpenPopup("Editor Controls");
        m_showControlsPopup = false;
    }

    if (ImGui::BeginPopupModal("Editor Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("TurboGameEngine Scene Editor");
        ImGui::Separator();
        ImGui::BulletText("RMB + WASDQE: free camera movement");
        ImGui::BulletText("Mouse wheel: change camera move speed");
        ImGui::BulletText("W / E / R: translate, rotate, scale gizmos");
        ImGui::BulletText("Left click viewport: select entity under cursor");
        ImGui::BulletText("F: focus camera on selected entity");
        ImGui::BulletText("GameObject menu: frame scene, drop to ground, reset transform, hide/show");
        ImGui::BulletText("Project panel: browse assets and apply mesh, texture, or shader to the selection");
        ImGui::BulletText("Panel grip: drag to highlighted zones, double-click to float, right-click for fallback dock menu");
        ImGui::BulletText("Dock splitters: drag thin separators to resize docked columns and stacked panels");
        ImGui::BulletText("Editor layout is saved automatically to assets/editor_layout.json");
        ImGui::BulletText("Ctrl+N / Ctrl+D / Delete: create empty, duplicate, delete");
        ImGui::BulletText("Ctrl+S / Ctrl+O: save and load editor scene");
        ImGui::BulletText("Ctrl+Z / Ctrl+Y: undo, redo editor changes");
        ImGui::BulletText("Space: enter Play mode, launch striker while playing");
        ImGui::BulletText("Esc: stop camera look or return to Edit mode");
        ImGui::BulletText("F1: toggle collider debug rendering");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120.0F, 0.0F))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_editorLayoutDirty && !m_dockDragActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        saveEditorLayout();
        m_editorLayoutDirty = false;
    }

    m_resetEditorLayout = false;
}

void GameplayState::renderUi(renderer::Renderer& rendererInstance) {
    if (!rendererInstance.isImGuiEnabled()) {
        return;
    }

    renderEditorUi(rendererInstance);
}

} // namespace engine::game
