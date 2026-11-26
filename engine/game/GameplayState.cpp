#include "engine/game/GameplayState.h"
#include "engine/core/AtomicFile.h"
#include "engine/editor/EditorTheme.h"
#include "engine/editor/MaterialInspector.h"

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
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/core/CrashReport.h"
#include "engine/core/JobSystem.h"
#include "engine/core/LabOptions.h"
#include "engine/core/PlaySession.h"
#include "engine/core/Profiling.h"
#include "engine/ecs/collision_utils.h"
#include "engine/ecs/components.h"
#include "engine/ecs/light_json.h"
#include "engine/ecs/render_interpolation.h"
#include "engine/ecs/transform_utils.h"
#include "engine/scripting/react/ReactRuntime.h"
#include "engine/scripting/react/ReactUI.h"
#include "engine/game/StateStack.h"
#include "engine/game/SceneNetwork.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/renderer/RenderAdapter.h"
#include "engine/renderer/Renderer.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui_internal.h"
#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/misc/cpp/imgui_stdlib.h"
#include "ImGuizmo.h"

namespace {

using engine::ecs::EntityId;

std::uint32_t nextReplicaKey(const engine::ecs::World& world) {
    std::unordered_set<std::uint32_t> used;
    world.forEach<engine::net::NetworkReplica>([&](EntityId, const engine::net::NetworkReplica& replica) {
        used.insert(replica.key);
    });
    std::uint32_t key = 1;
    while (key <= engine::net::MaxReplicaKey && used.contains(key)) ++key;
    return key <= engine::net::MaxReplicaKey ? key : 0;
}

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
constexpr float kWindowMargin = 1.0F;
constexpr float kStatusBarHeight = 68.0F;
constexpr float kViewportHeaderHeight = 68.0F;
constexpr float kMaterialGraphSplitterSize = 8.0F;
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
    ImVec2 assistantPos;
    ImVec2 assistantSize;
    ImVec2 statusPos;
    ImVec2 statusSize;
};

struct PanelBody final {
    explicit PanelBody(const ImVec2 padding = ImVec2(6, 2)) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        ImGui::BeginChild("##PanelBody", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    }
    ~PanelBody() { ImGui::EndChild(); ImGui::PopStyleVar(); }
};
struct FlatPanelStyle final {
    FlatPanelStyle() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0F);
    }
    ~FlatPanelStyle() { ImGui::PopStyleVar(3); }
};
struct PropertySection final {
    bool open{};
    explicit PropertySection(const char* label, const bool defaultOpen = true) {
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(32, 34, 39, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(43, 45, 53, 255));
        ImGui::PushFont(engine::editor::nativeSemiboldFont(), engine::editor::kEditorFontSize);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 5.5F));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
        const bool expanded = ImGui::CollapsingHeader(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleVar(2); ImGui::PopFont(); ImGui::PopStyleColor(2);
        if (!expanded) return;
        ImGui::PushID(label);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 3));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(25, 26, 31, 255));
        open = ImGui::BeginTable("##Properties", 2, ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV);
        if (open) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, .48F);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, .52F);
        } else { ImGui::PopStyleColor(); ImGui::PopStyleVar(3); ImGui::PopID(); }
    }
    ~PropertySection() {
        if (open) { ImGui::EndTable(); ImGui::PopStyleColor(); ImGui::PopStyleVar(3); ImGui::PopID(); }
    }
    explicit operator bool() const { return open; }
};
struct PropertyCell final {
    explicit PropertyCell(const char* label, const float labelInset = 24.0F, const char* id = nullptr) {
        ImGui::TableNextRow(0, engine::editor::kPropertyRowHeight);
        ImGui::TableSetColumnIndex(0); ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelInset);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1); ImGui::PushID(id ? id : label);
    }
    ~PropertyCell() { ImGui::PopID(); }
};
bool propertyCheckbox(const char* name, bool* value) { PropertyCell cell(name); return ImGui::Checkbox("##Value", value); }
bool propertyInput(const char* name, std::string* value) { PropertyCell cell(name); return ImGui::InputText("##Value", value); }
bool propertyColor(const char* name, float* value) { PropertyCell cell(name); return ImGui::ColorEdit4("##Value", value, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview); }
bool propertyDrag(const char* name, float* value, float speed, float minimum, float maximum, const char* format) {
    PropertyCell cell(name); return ImGui::DragFloat("##Value", value, speed, minimum, maximum, format);
}
bool propertyDrag2(const char* name, float* value, float speed, float minimum, float maximum, const char* format) {
    PropertyCell cell(name); return ImGui::DragFloat2("##Value", value, speed, minimum, maximum, format);
}
bool propertyDrag3(const char* name, float* value, float speed, float minimum, float maximum, const char* format) {
    PropertyCell cell(name); return ImGui::DragFloat3("##Value", value, speed, minimum, maximum, format);
}
bool propertySlider(const char* name, float* value, float minimum, float maximum, const char* format) {
    PropertyCell cell(name); return ImGui::SliderFloat("##Value", value, minimum, maximum, format);
}
bool propertyCombo(const char* name, int* value, const char* const* labels, int count) {
    PropertyCell cell(name); return ImGui::Combo("##Value", value, labels, count);
}
engine::editor::EditorIcons::Resolver iconResolver(engine::renderer::Renderer* renderer) {
    if (!renderer) return {};
    return [renderer](const std::string_view id) {
        const auto image = renderer->resolveEditorIcon(id);
        return engine::editor::EditorIcons::Image{image.textureId, image.width, image.height};
    };
}

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

std::filesystem::path legacyEditorScenePath() {
#if defined(ENGINE_SOURCE_ROOT)
    if (std::filesystem::exists(std::filesystem::path(ENGINE_SOURCE_ROOT) / "assets")) {
        return std::filesystem::path(ENGINE_SOURCE_ROOT) / kEditorScenePath;
    }
    return std::filesystem::path(kEditorScenePath);
#else
    return std::filesystem::path(kEditorScenePath);
#endif
}

std::filesystem::path legacyEditorLayoutPath() {
#if defined(ENGINE_SOURCE_ROOT)
    if (std::filesystem::exists(std::filesystem::path(ENGINE_SOURCE_ROOT) / "assets")) {
        return std::filesystem::path(ENGINE_SOURCE_ROOT) / kEditorLayoutPath;
    }
    return std::filesystem::path(kEditorLayoutPath);
#else
    return std::filesystem::path(kEditorLayoutPath);
#endif
}

std::filesystem::path legacyEditorAssetsRoot() {
#if defined(ENGINE_SOURCE_ROOT)
    const auto sourceAssets = std::filesystem::path(ENGINE_SOURCE_ROOT) / "assets";
    return std::filesystem::exists(sourceAssets) ? sourceAssets : std::filesystem::path("assets");
#else
    return std::filesystem::path("assets");
#endif
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
    if (extension == ".lua" || extension == ".luau") { return "Script"; }
    if (filename.ends_with(".prefab.json") || path.parent_path().filename() == "prefabs") { return "Prefab"; }
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
        {"materialId", meshRenderer.materialId},
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
    meshRenderer.materialId = value.value("materialId", meshRenderer.materialId);
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
    const float requestedLeftWidth, const float requestedRightWidth,
    const float requestedLeftSplit, const float requestedRightSplit,
    const float toolbarHeight, const bool leftTop, const bool leftBottom,
    const bool rightTop, const bool rightBottom, const bool assistant,
    const bool toolbar, const bool status) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::PushFont(engine::editor::nativeFont(), engine::editor::kEditorFontSize);
    const float menuBarHeight = ImGui::GetFrameHeight();
    ImGui::PopFont();
    const float assistantWidth = assistant ? std::min(466.0F, viewport->Size.x * 0.5F) : 0.0F;
    const float availableWidth = std::max(1.0F, viewport->Size.x);
    const bool left = leftTop || leftBottom, right = rightTop || rightBottom;
    float leftWidth = left ? (requestedLeftWidth > 0 ? requestedLeftWidth : 270.0F) : 0.0F;
    float rightWidth = right ? (requestedRightWidth > 0 ? requestedRightWidth : 310.0F) : 0.0F;
    if (assistant) rightWidth = std::max(rightWidth, assistantWidth);
    if (left) leftWidth = std::clamp(leftWidth, 180.0F, 560.0F);
    if (right || assistant) rightWidth = std::min(std::clamp(rightWidth, 220.0F, 620.0F), availableWidth * .5F);
    const float sidebarBudget = std::max(0.0F, availableWidth - 320.0F);
    if (leftWidth + rightWidth > sidebarBudget) {
        const float scale = sidebarBudget / std::max(1.0F, leftWidth + rightWidth);
        leftWidth *= scale; rightWidth *= scale;
    }
    const float statusHeight = status ? kStatusBarHeight : 0.0F;
    const float panelsTop = viewport->Pos.y + menuBarHeight + (toolbar ? toolbarHeight : 0.0F);
    const float statusPosY = viewport->Pos.y + viewport->Size.y - statusHeight;
    const float panelsHeight = std::max(1.0F, statusPosY - panelsTop);
    const float leftSplit = std::clamp(requestedLeftSplit, 0.22F, 0.82F);
    const float rightSplit = std::clamp(requestedRightSplit, 0.22F, 0.82F);
    const float hierarchyHeight = leftTop && leftBottom ? panelsHeight * leftSplit : (leftTop ? panelsHeight : 0.0F);
    const float assistantHeight = !assistant ? 0.0F : !right ? panelsHeight : std::min(panelsHeight * .4F, 360.0F);
    const float rightPanelsHeight = panelsHeight - assistantHeight;
    const float inspectorHeight = rightTop && rightBottom ? rightPanelsHeight * rightSplit : (rightTop ? rightPanelsHeight : 0.0F);
    EditorLayout layout{};
    layout.toolbarPos = ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight);
    layout.toolbarSize = ImVec2(viewport->Size.x, toolbarHeight);
    layout.hierarchyPos = ImVec2(viewport->Pos.x, panelsTop);
    layout.hierarchySize = ImVec2(leftWidth, hierarchyHeight);
    layout.projectPos = ImVec2(viewport->Pos.x, panelsTop + hierarchyHeight);
    layout.projectSize = ImVec2(leftWidth, leftBottom ? panelsHeight - hierarchyHeight : 0.0F);
    layout.viewportPos = ImVec2(viewport->Pos.x + leftWidth, panelsTop);
    layout.viewportSize = ImVec2(std::max(1.0F, availableWidth - leftWidth - rightWidth), panelsHeight);
    layout.inspectorPos = ImVec2(layout.viewportPos.x + layout.viewportSize.x, panelsTop);
    layout.inspectorSize = ImVec2(rightWidth, inspectorHeight);
    layout.statsPos = ImVec2(layout.inspectorPos.x, panelsTop + inspectorHeight);
    layout.statsSize = ImVec2(rightWidth, rightBottom ? rightPanelsHeight - inspectorHeight : 0.0F);
    layout.assistantPos = ImVec2(viewport->Pos.x + viewport->Size.x - rightWidth, panelsTop + rightPanelsHeight);
    layout.assistantSize = ImVec2(rightWidth, assistantHeight);
    layout.statusPos = ImVec2(viewport->Pos.x, statusPosY);
    layout.statusSize = ImVec2(viewport->Size.x, statusHeight);
    return layout;
}

std::string componentSubtitle(const engine::ecs::World& world, const EntityId entity) {
    std::string subtitle = "#" + std::to_string(entity);
    if (world.hasComponent<engine::ecs::Camera>(entity)) {
        subtitle += "  CAM";
    }
    if (world.hasComponent<engine::ecs::Light>(entity)) subtitle += "  LIGHT";
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

std::optional<engine::editor::MaterialGraphSession> readMaterialGraphSession(const nlohmann::json& graph) {
    const auto session = graph.find("session");
    if (session == graph.end() || !session->is_object()) return std::nullopt;
    const auto materialId = session->find("materialId");
    if (materialId == session->end() || !materialId->is_string() || materialId->get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }

    engine::editor::MaterialGraphSession result{materialId->get<std::string>(), std::nullopt};
    const auto canvas = session->find("canvas");
    if (canvas == session->end() || !canvas->is_object()) return result;
    const auto pan = canvas->find("pan");
    const auto zoom = canvas->find("zoom");
    if (pan == canvas->end() || !pan->is_array() || pan->size() != 2 ||
        !(*pan)[0].is_number() || !(*pan)[1].is_number() || zoom == canvas->end() || !zoom->is_number()) {
        return result;
    }

    const glm::vec2 offset{(*pan)[0].get<float>(), (*pan)[1].get<float>()};
    const float scale = zoom->get<float>();
    if (std::isfinite(offset.x) && std::isfinite(offset.y) && std::isfinite(scale)) {
        result.canvas = engine::editor::MaterialGraphCanvasState{offset, scale};
    }
    return result;
}

bool renderSplitterHandle(const char* id, const ImVec2& position, const ImVec2& size, const bool vertical,
                          const bool applyReleaseDelta = false) {
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0F, 1.0F));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground |
                                    ImGuiWindowFlags_NoScrollWithMouse;

    bool dragging = false;
    if (ImGui::Begin(id, nullptr, flags)) {
        ImGui::InvisibleButton("##DockSplitter", size);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool active = ImGui::IsItemActive();
        dragging = active || (applyReleaseDelta && ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left));
        if (hovered || active) {
            ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
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
    ImGui::PopStyleVar(4);
    return dragging;
}

}

namespace engine::game {

GameplayState::GameplayState(StateStack& stack) : IGameState(stack) {
    if (stack.project()) { m_project = *stack.project(); }
    const auto* options = stack.labOptions();
    auto* jobs = options == nullptr || options->parallelEcs ? stack.jobs() : nullptr;
    m_physicsSystem.setJobSystem(jobs);
    m_renderSystem.setJobSystem(jobs);
    initializeEditorCommands();
}

GameplayState::~GameplayState() = default;

uint16_t GameplayState::networkScenePort() const {
    return m_sceneNetwork ? m_sceneNetwork->localPort() : 0;
}

void GameplayState::queueScriptEvent(std::string event, nlohmann::json payload) {
    m_scripts.queueEvent(std::move(event), std::move(payload));
}

std::filesystem::path GameplayState::editorAssetsRoot() const {
    return m_project ? m_project->assetRoot : legacyEditorAssetsRoot();
}

std::filesystem::path GameplayState::editorScenePath() const {
    return m_project ? m_project->scenePath : legacyEditorScenePath();
}

std::filesystem::path GameplayState::editorLayoutPath() const {
    return m_project ? m_project->layoutPath : legacyEditorLayoutPath();
}

std::string GameplayState::toAssetPath(const std::filesystem::path& path) const {
    std::error_code error;
    auto relative = std::filesystem::relative(path, editorAssetsRoot(), error);
    if (error) { relative = path.filename(); }
    return (std::filesystem::path("assets") / relative).generic_string();
}

bool GameplayState::labActive() const {
    return stack().labOptions() != nullptr && stack().labOptions()->active();
}

void GameplayState::onEnter() {
    const auto sceneName = editorScenePath().filename().u8string();
    core::CrashReport::setScene(std::string_view(reinterpret_cast<const char*>(sceneName.data()), sceneName.size()));
    core::CrashReport::setAction("scene-enter");
    ENGINE_LOG_INFO("Entered GameplayState editor scene");
    m_eventBus.clear();
    if (const auto* launch = stack().sceneNetworkLaunch()) {
        if (!initializeNetworkScene(*launch)) {
            ENGINE_LOG_ERROR("Network scene initialization failed: {}", m_networkSceneError);
        }
        return;
    }
    if (!labActive() && !stack().standalone()) { loadEditorLayout(); }
    bindCollisionEventHandlers();
    resetDemoScene();
    if (m_project && !labActive()) {
        loadSceneFromDisk();
        m_undoStack.clear();
        m_redoStack.clear();
    }
    if (!labActive()) {
        m_scripts.initialize(m_world, stack().jobs(), editorAssetsRoot());
        m_scripts.setInputWindow(stack().window());
        m_scripts.setDestroyCallback([this](const ecs::EntityId entity) { deleteEntityNow(entity); });
        scripting::ReactRuntime::install(m_scripts);
        m_scriptWorkspace.initialize(editorAssetsRoot(), m_scripts, {
            [this] { if (m_editorMode == EditorMode::Edit) { enterPlayMode(); } else { startSimulation(); } },
            [this] { pauseSimulation(); }, [this] { stopPlayMode(); },
            [this] { if (m_editorMode == EditorMode::Play) { m_simulationRunning = false; m_stepScriptTick = true; } },
            [this] { createScriptDemo(); }, [this] { launchStandalonePlay(); }
        }, stack().jobs());
        m_scriptWorkspace.setCommandRegistry(&m_editorCommands);
        m_codexAssistant.setProjectRoot(m_project ? m_project->root : std::filesystem::absolute(editorAssetsRoot()).parent_path());
        m_codexAssistant.setContextProvider([this] {
            return "Saved Luau, require dependencies, React UI and startup-scene JSON update live in both Play modes. "
                "Preserve existing scene sourceId values. Runtime self.state survives reload; Init is not rerun. "
                "Use OnReload(self) for migrations or Init-created scene/UI changes, preserving existing objects. "
                "UI uses real React Luau with a native renderer and Roblox UI Instance/Enum compatibility. "
                "Use local UI=require('ReactTurbo'); UI.UDim2.new/fromScale/fromOffset for Size/Position, "
                "UI.Vector2.new for AnchorPoint and UI.Color3.fromRGB for colors. "
                "Global Instance, Enum, UDim, UDim2, Vector2, Color3 and require('ReactRoblox') are also available. "
                "UI refs expose Instance properties, IsA, GetChildren, FindFirstChild and property signals. "
                "ReactRoblox.createRoot(instance) and createPortal support portable UI bootstrap; roots are synchronous. "
                "Frame/TextLabel/TextButton support BackgroundColor3, BackgroundTransparency, TextColor3 and TextSize. "
                "UICorner(CornerRadius=UI.UDim.new), UIPadding and UIListLayout(Padding=UI.UDim.new, "
                "FillDirection='Vertical'/'Horizontal', SortOrder='LayoutOrder') provide styling/layout; "
                "children use LayoutOrder. Typed UI Enum values and legacy strings are accepted. "
                "UIStroke, AutomaticSize and read-only AbsoluteSize/AbsolutePosition/TextBounds are supported. "
                "UIGradient uses global ColorSequence/NumberSequence and keypoints, Rotation/Offset. "
                "ImageLabel/ImageButton load project-local assets; rbxassetid:// ids map through assets/ui_images.json. "
                "Images support tint/alpha, atlas rectangles and ScaleType Stretch/Fit/Crop/Tile/Slice. "
                "TextBox, ScrollingFrame, UIGridLayout, UIScale and size/aspect constraints are supported. "
                "Use React.Change.Text/CanvasPosition for controlled inputs; ImageButton supports hover/pressed variants. "
                "For UI animation use game:GetService('TweenService'):Create(uiInstance,TweenInfo.new(seconds,Enum.EasingStyle.Quad,Enum.EasingDirection.Out),goals). "
                "TweenService supports Play/Pause/Cancel/Destroy, Completed and repeat/reverse/delay for number/UDim/UDim2/Vector2/Color3. "
                "Animate UIGradient.Rotation/Offset; sequences are not tweenable. Do not tween a property owned by a React binding; clean up tweens and connections in effects. "
                "CanvasGroup flattens a whole UI subtree; tween GroupTransparency for window fades and GroupColor3 for tint, including nested groups. "
                "ClipsDescendants is always true; its UICorner/UIGradient apply to the whole layer. Fully transparent groups and Interactable=false ancestors block descendant input in this engine. "
                "UI.getNativeFeatures() reports native support; updating bootstrap/C++ requires restart. "
                "HTTP/Roblox CDN images, CSS/flex and full Roblox platform services are not implemented. "
                "Engine.getRotation/setRotation use Euler radians. Startup scene: " + toAssetPath(editorScenePath()) +
                "\n" + m_scriptWorkspace.assistantContext();
        });
        m_sceneWatcher.initialize(stack().jobs());
        const auto liveScene = m_project && !m_project->liveScenePath.empty() ? m_project->liveScenePath : editorScenePath();
        m_sceneWatcher.watch(liveScene);
        try { std::ifstream input(liveScene); m_diskScene = nlohmann::json::parse(input); }
        catch (...) { m_diskScene = nlohmann::json::object(); }
    }
    const auto* options = stack().labOptions();
    if (stack().standalone() || (options != nullptr && options->stressActive())) { enterPlayMode(); }
    ENGINE_LOG_INFO("Scene ready: mode={} entities={} stress={}",
        m_editorMode == EditorMode::Play ? "Play" : "Edit", m_world.aliveCount(),
        options != nullptr && options->stressActive());
}

bool GameplayState::initializeNetworkScene(const core::PlayLaunch& launch) {
    m_networkSceneError.clear();
    bindCollisionEventHandlers();
    if (!loadSceneFromDisk()) {
        m_networkSceneError = "Could not load the network scene.";
        return false;
    }
    if (!m_scripts.initialize(m_world, stack().jobs(), editorAssetsRoot()) ||
        !scripting::ReactRuntime::install(m_scripts, launch.reactPackage)) {
        m_networkSceneError = "Could not initialize the scene's Luau runtime.";
        return false;
    }
    m_scripts.setDestroyCallback([this](ecs::EntityId entity) { deleteEntityNow(entity, false); });
    m_scripts.setInputWindow(stack().window());
    m_scripts.setInputEnabled(false);
    auto network = std::make_unique<SceneNetwork>(m_world, m_scripts,
        [this](ecs::EntityId entity) { deleteEntityNow(entity, false); });
    if (!network->start(launch, m_networkSceneError)) return false;
    m_sceneNetwork = std::move(network);
    m_scripts.setNetwork(m_sceneNetwork.get());
    m_scripts.setSteam(m_sceneNetwork->steam());
    m_editorMode = EditorMode::Play;
    m_scripts.start();
    startSimulation();
    m_sceneNetwork->endTick();
    ENGINE_LOG_INFO("Network scene ready: role={} entities={}", m_sceneNetwork->role(), m_world.aliveCount());
    return true;
}

void GameplayState::onExit() {
    ENGINE_LOG_INFO("Exited GameplayState editor scene");
    if (!labActive() && !stack().standalone()) {
        if (m_materialGraphDraftsInitialized) {
            editor::flushMaterialGraphDrafts(true);
            editor::closeMaterialGraphDrafts();
            m_materialGraphDraftsInitialized = false;
        }
        saveEditorLayout();
        editor::closeMaterialGraphEditor();
    }
    setCameraLookActive(false);
    m_codexAssistant.shutdown();
    m_sceneWatcher.clear();
    m_scripts.shutdown();
    m_scripts.setNetwork(nullptr);
    m_scripts.setSteam(nullptr);
    m_sceneNetwork.reset();
    m_pendingSpawnJobs.clear();
    m_pendingRuntimeDeletes.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_editUndoStack.clear();
    m_editRedoStack.clear();
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
    m_materialPreviewEntity = ecs::kInvalidEntity;
    m_materialPreview.reset();
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
    const bool escapePressed = event.type == platform::EventType::KeyPressed &&
        event.key == platform::KeyCode::Escape && !event.repeat;
    if (event.type == platform::EventType::FocusLost) {
        m_scripts.releaseCursorLock();
        setCameraLookActive(false);
        return;
    }
    if (escapePressed && m_scripts.releaseCursorLock()) return;
    if (stack().standalone()) {
        if (escapePressed && stack().window()) {
            stack().window()->requestClose();
        }
        return;
    }
    if (labActive() || m_scripts.cursorLocked()) { return; }
    if (event.type == platform::EventType::MouseButtonPressed && event.mouseButton == platform::MouseButton::Right) {
        if (m_viewportHovered && !m_viewportUiCaptured) {
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

    using Command = editor::EditorCommandId;
    if (sceneShortcutsBlocked()) return;
    if (m_viewportUiCaptured && event.key != platform::KeyCode::Escape && event.key != platform::KeyCode::F1) return;
    if (event.key == platform::KeyCode::Delete) { m_editorCommands.execute(Command::Delete); return; }
    if (event.key == platform::KeyCode::Escape) {
        if (m_cameraLookActive) setCameraLookActive(false);
        else m_editorCommands.execute(Command::Stop);
        return;
    }
    if (event.key == platform::KeyCode::F1) { m_editorCommands.execute(Command::ToggleColliderDebug); return; }
    if (event.key == platform::KeyCode::Space) {
        if (m_editorMode == EditorMode::Edit) m_editorCommands.execute(Command::Play);
        else launchStriker();
    }
}

void GameplayState::bindCollisionEventHandlers() {
    m_eventBus.subscribe<ecs::CollisionEnterEvent>([this](const ecs::CollisionEnterEvent& event) {
        ++m_collisionEnterCount;
        m_lastCollisionMessage = "Enter: " + std::to_string(event.contact.entityA) + " <-> " +
                                 std::to_string(event.contact.entityB) + " depth=" +
                                 std::to_string(event.contact.penetrationDepth);
        m_scripts.queueEvent("CollisionEnter", {{"entityA", event.contact.entityA}, {"entityB", event.contact.entityB}});
    });
    m_eventBus.subscribe<ecs::CollisionStayEvent>([this](const ecs::CollisionStayEvent& event) {
        ++m_collisionStayCount;
        if (m_scripts.playing()) { m_scripts.queueEvent("CollisionStay", {{"entityA", event.contact.entityA}, {"entityB", event.contact.entityB}}); }
    });
    m_eventBus.subscribe<ecs::CollisionExitEvent>([this](const ecs::CollisionExitEvent& event) {
        ++m_collisionExitCount;
        m_lastCollisionMessage =
            "Exit: " + std::to_string(event.contact.entityA) + " <-> " + std::to_string(event.contact.entityB);
        m_scripts.queueEvent("CollisionExit", {{"entityA", event.contact.entityA}, {"entityB", event.contact.entityB}});
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

void GameplayState::createEditorCamera() {
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
}

void GameplayState::createDemoScene() {
    ENGINE_PROFILE_ZONE("Create Demo Scene");
    if (m_sceneInitialized) { return; }
    createEditorCamera();

    if (labActive()) {
        createLabScene();
        m_sceneInitialized = true;
        return;
    }

    if (m_project) {
        m_sceneInitialized = true;
        m_selectedEntity = ecs::kInvalidEntity;
        m_lastCollisionMessage = "Blank project scene ready.";
        return;
    }

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

    const auto* options = stack().labOptions();
    if (options != nullptr && options->stressActive()) {
        for (int row = 0; row < 32; ++row) {
            for (int column = 0; column < 40; ++column) {
                const float x = -12.675F + static_cast<float>(column) * 0.65F;
                const float y = -1.75F + static_cast<float>((row + column) % 2) * 0.02F;
                const float z = 0.35F + static_cast<float>(row) * 0.62F;
                spawnDynamicBody(
                    "profile_body_" + std::to_string(row) + "_" + std::to_string(column),
                    glm::vec3(x, y, z),
                    glm::vec3(0.42F, 0.42F, 0.42F),
                    paletteColor(static_cast<std::size_t>(row * 40 + column)),
                    1.0F);
            }
        }
    }

    m_sceneInitialized = true;
    m_selectedEntity = m_strikerEntity;
    m_lastCollisionMessage = "Editor scene ready.";
}

void GameplayState::createLabScene() {
    const auto& options = *stack().labOptions();
    const bool ecsScene = options.scene == "ecs";
    const std::size_t count = ecsScene ? options.entityCount : 16U;
    m_labEntities.reserve(count);
    const std::size_t columns = ecsScene ? static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(count)))) : 4U;

    const auto root = m_world.createEntity();
    m_world.addComponent<ecs::Transform>(root, ecs::Transform{});
    m_world.addComponent<ecs::Tag>(root, ecs::Tag{"lab_scene_root"});
    ecs::EntityId rowEntity = root;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t column = index % columns;
        const std::size_t row = index / columns;
        if (column == 0U) {
            rowEntity = m_world.createEntity();
            ecs::Transform group;
            group.position.z = ecsScene ? 0.25F + static_cast<float>(row) * (19.0F / static_cast<float>(columns))
                                        : 3.0F + static_cast<float>(row) * 3.5F;
            m_world.addComponent<ecs::Transform>(rowEntity, group);
            m_world.addComponent<ecs::Hierarchy>(rowEntity, ecs::Hierarchy{root});
        }
        const auto entity = m_world.createEntity();
        ecs::Transform transform;
        transform.position = glm::vec3(
            ecsScene ? -12.0F + static_cast<float>(column) * (24.0F / static_cast<float>(columns))
                     : -5.25F + static_cast<float>(column) * 3.5F,
            -0.7F, 0.0F);
        transform.scale = glm::vec3(ecsScene ? 0.22F : 1.7F);
        transform.rotationEulerRadians = glm::vec3(0.07F * static_cast<float>(index % 5U),
                                                    0.13F * static_cast<float>(index % 11U), 0.0F);
        ecs::MeshRenderer mesh;
        mesh.meshId = kCubeMeshId;
        mesh.textureId = kLogoTextureId;
        mesh.shaderId = kShaderId;
        mesh.tint = ecsScene ? paletteColor(index) : glm::vec4(1.0F);
        m_world.addComponent<ecs::Transform>(entity, transform);
        m_world.addComponent<ecs::Hierarchy>(entity, ecs::Hierarchy{rowEntity});
        m_world.addComponent<ecs::MeshRenderer>(entity, mesh);
        m_labEntities.push_back(entity);
    }
    ENGINE_LOG_INFO("LAB_SCENE_READY scene={} render_entities={} total_entities={}",
                    options.scene, count, m_world.aliveCount());
}

void GameplayState::requestLabAssets() {
    const auto& options = *stack().labOptions();
    m_labLoadRequested = true;
    ENGINE_PROFILE_MESSAGE("LAB_LOAD_START");
    ENGINE_LOG_INFO("LAB_LOAD_START elapsed={} asset_dir={}", stack().labElapsedSeconds(), options.assetDirectory);
    const std::filesystem::path directory(options.assetDirectory);
    for (std::size_t index = 0; index < m_labEntities.size(); ++index) {
        auto* mesh = m_world.getComponent<ecs::MeshRenderer>(m_labEntities[index]);
        if (mesh == nullptr) { continue; }
        std::ostringstream name;
        if (index < 12U) {
            name << "texture_" << std::setw(2) << std::setfill('0') << index << ".png";
            mesh->textureId = (directory / name.str()).string();
            mesh->texture.reset();
        } else {
            name << "mesh_" << std::setw(2) << std::setfill('0') << (index - 12U) << ".obj";
            mesh->meshId = (directory / name.str()).string();
            mesh->mesh.reset();
        }
    }
    if (options.missingAsset && !m_labEntities.empty()) {
        auto* mesh = m_world.getComponent<ecs::MeshRenderer>(m_labEntities.front());
        mesh->textureId = (directory / "intentionally_missing.png").string();
    }
}

void GameplayState::renderLabUi(renderer::Renderer& rendererInstance) {
    const auto& options = *stack().labOptions();
    const auto loads = rendererInstance.assetLoadingStats();
    updateFrameHistory();
    if (m_labLoadRequested && !m_labLoadCompleted && loads.pendingCpu == 0U && loads.pendingUploads == 0U) {
        m_labLoadCompleted = true;
        ENGINE_PROFILE_MESSAGE("LAB_LOAD_COMPLETE");
        ENGINE_LOG_INFO("LAB_LOAD_COMPLETE elapsed={} loaded={} failed={} cancelled={}",
                        stack().labElapsedSeconds(), loads.loaded, loads.failed, loads.cancelled);
    }
    ImGui::SetNextWindowPos(ImVec2(16.0F, 16.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(490.0F, 265.0F), ImGuiCond_Always);
    ImGui::Begin("TurboGameEngine / Lab 1", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Scene: %s | Objects: %zu", options.scene.c_str(), m_labEntities.size());
    ImGui::Text("ECS: %s | Loading: %s | Workers: %zu",
                options.parallelEcs ? "jobs" : "serial", options.asyncLoading ? "async" : "sync",
                stack().jobs() != nullptr ? stack().jobs()->workerCount() : 0U);
    ImGui::Text("Elapsed: %.1fs | CPU: %zu | Uploads: %zu", stack().labElapsedSeconds(),
                loads.pendingCpu, loads.pendingUploads);
    ImGui::Text("Ready: %zu | Failed: %zu | Pump: %.2fms", loads.loaded, loads.failed, loads.lastPumpMilliseconds);
    ImGui::Text("Upload: %s | GPU pending: %zu | Fence: %llu", loads.gpuUploadEnabled ? "transfer queue" : "main pump",
                loads.pendingGpu, static_cast<unsigned long long>(loads.lastGpuFence));
    ImGui::Text("Transfer completed: %zu | Data: %.1f MiB", loads.gpuUploads,
                static_cast<double>(loads.gpuBytes) / (1024.0 * 1024.0));
    if (options.scene == "loading" && !m_labLoadRequested) {
        ImGui::Text("Load batch at %.1f seconds", options.loadAtSeconds);
    } else {
        ImGui::TextUnformatted(options.scene == "loading"
            ? (m_labLoadCompleted ? "Resource batch complete" : "Loading resources...")
            : "Read-only transform jobs; GPU submission on main");
    }
    ImGui::PlotLines("Frame ms", m_frameTimeHistory.data(), static_cast<int>(m_frameTimeHistory.size()),
                     static_cast<int>(m_historyCursor), nullptr, 0.0F, 50.0F, ImVec2(320.0F, 55.0F));
    ImGui::End();
}

void GameplayState::resetDemoScene() {
    scripting::ReactRuntime::reset(m_scripts);
    m_scripts.stop();
    m_stepScriptTick = false;
    m_pendingRuntimeDeletes.clear();
    m_world.clear();
    m_labEntities.clear();
    m_labLoadRequested = false;
    m_labLoadCompleted = false;
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
    m_viewportUiCaptured = false;
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
    m_editorStatusMessage = m_project ? "Blank scene ready." : "Demo scene reset.";
}

void GameplayState::createScriptDemo() {
    if (!canEditScene()) { m_editorStatusMessage = "Stop Play before loading the Lab 2 demo."; return; }
    if (m_project) {
        try {
            const auto assets = std::filesystem::weakly_canonical(editorAssetsRoot());
            for (const auto* relative : {"scripts/Collectathon.luau", "scripts/Collectible.luau", "scripts/ui/demo_ui.luau",
                    "prefabs/collectathon_controller.json", "prefabs/collectathon_player.json",
                    "prefabs/collectible.json", "prefabs/react_ui.json"}) {
                const auto target = std::filesystem::weakly_canonical(assets / relative);
                const auto within = target.lexically_relative(assets);
                if (within.empty() || *within.begin() == "..") { throw std::runtime_error("Demo destination is outside project assets"); }
                if (std::filesystem::exists(target)) { continue; }
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::copy_file(legacyEditorAssetsRoot() / relative, target);
            }
        } catch (const std::exception& error) {
            m_editorStatusMessage = std::string("Cannot import script demo: ") + error.what();
            return;
        }
    }
    commitSceneEdit();
    ecs::EntityId controller = ecs::kInvalidEntity;
    ecs::EntityId hud = ecs::kInvalidEntity;
    m_world.forEach<scripting::ScriptComponent>([&](const ecs::EntityId entity, const auto& component) {
        if (component.path == "scripts/Collectathon.luau") { controller = entity; }
        if (component.path == "scripts/ui/demo_ui.luau") { hud = entity; }
    });
    if (controller == ecs::kInvalidEntity || hud == ecs::kInvalidEntity) { recordSceneHistory("Add Lab 2 Script Demo"); }
    if (controller == ecs::kInvalidEntity) { controller = m_scripts.spawnPrefab("prefabs/collectathon_controller.json"); }
    if (controller == ecs::kInvalidEntity) { m_editorStatusMessage = "Demo prefab failed; see Script Output."; return; }
    m_selectedEntity = controller;
    if (hud == ecs::kInvalidEntity) { m_scripts.spawnPrefab("prefabs/react_ui.json"); }
    m_showScriptWorkspace = true;
    m_scriptWorkspace.openScript("scripts/Collectathon.luau");
    m_projectAssetsDirty = true;
    m_editorStatusMessage = "Collectathon demo ready. Play, then use arrows to collect tokens.";
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
    m_scripts.releaseCursorLock();
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
    ENGINE_PROFILE_ZONE("Process Pending Spawns");
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

    commitSceneEdit();
    m_editModeSnapshot = captureSceneSnapshot();
    m_editUndoStack = std::move(m_undoStack);
    m_editRedoStack = std::move(m_redoStack);
    m_undoStack.clear(); m_redoStack.clear();
    m_editorMode = EditorMode::Play;
    m_scripts.start();
    startSimulation();
}

void GameplayState::stopPlayMode() {
    if (m_editorMode == EditorMode::Edit) {
        return;
    }

    scripting::ReactRuntime::reset(m_scripts);
    m_scripts.stop();
    m_stepScriptTick = false;
    m_pendingRuntimeDeletes.clear();
    if (m_editModeSnapshot.has_value()) {
        restoreSceneSnapshot(*m_editModeSnapshot);
        m_editModeSnapshot.reset();
    }
    m_editorMode = EditorMode::Edit;
    m_undoStack = std::move(m_editUndoStack);
    m_viewportUiCaptured = false;
    m_redoStack = std::move(m_editRedoStack);
    setCameraLookActive(false);
    m_lastCollisionMessage = "Returned to Edit mode.";
}

GameplayState::EntitySnapshot GameplayState::captureEntitySnapshot(const ecs::EntityId entity) const {
    EntitySnapshot snapshot{};
    snapshot.sourceId = entity;
    if (const auto* component = m_world.getComponent<ecs::Transform>(entity)) { snapshot.transform = *component; }
    if (const auto* component = m_world.getComponent<ecs::Tag>(entity)) { snapshot.tag = *component; }
    if (const auto* component = m_world.getComponent<ecs::Hierarchy>(entity)) { snapshot.hierarchy = *component; }
    if (const auto* component = m_world.getComponent<ecs::Camera>(entity)) { snapshot.camera = *component; }
    if (const auto* component = m_world.getComponent<ecs::Light>(entity)) { snapshot.light = *component; }
    if (const auto* component = m_world.getComponent<ecs::Rigidbody>(entity)) { snapshot.rigidbody = *component; }
    if (const auto* component = m_world.getComponent<ecs::Collider>(entity)) { snapshot.collider = *component; }
    if (const auto* component = m_world.getComponent<ecs::MeshRenderer>(entity)) { snapshot.meshRenderer = *component; }
    if (const auto* component = m_world.getComponent<scripting::ScriptComponent>(entity)) { snapshot.script = *component; }
    if (const auto* component = m_world.getComponent<net::NetworkReplica>(entity)) { snapshot.networkReplica = *component; }
    return snapshot;
}

GameplayState::SceneSnapshot GameplayState::captureSceneSnapshot() const {
    ENGINE_PROFILE_ZONE("Editor Scene Snapshot");
    SceneSnapshot snapshot{};
    snapshot.fileEntities = m_fileEntities;
    snapshot.cameraEntity = m_cameraEntity;
    snapshot.strikerEntity = m_strikerEntity;
    snapshot.showcaseSphereEntity = m_showcaseSphereEntity;
    snapshot.selectedEntity = m_selectedEntity;
    snapshot.spawnSequence = m_spawnSequence;
    snapshot.showColliderDebug = m_showColliderDebug;
    snapshot.showcaseSphereLaunched = m_showcaseSphereLaunched;

    m_world.forEachEntity([&](const ecs::EntityId entity) {
        snapshot.entities.push_back(captureEntitySnapshot(entity));
    });

    if (m_editorMode == EditorMode::Edit) {
        std::unordered_set<ecs::EntityId> sources, mappedEntities;
        for (const auto& [source, entity] : snapshot.fileEntities) {
            sources.insert(source);
            mappedEntities.insert(entity);
        }
        std::vector<ecs::EntityId> unmapped;
        for (const auto& entity : snapshot.entities) if (!mappedEntities.contains(entity.sourceId)) unmapped.push_back(entity.sourceId);
        std::sort(unmapped.begin(), unmapped.end());
        ecs::EntityId available = 1;
        for (const auto entity : unmapped) {
            auto source = entity;
            if (sources.contains(source)) {
                while (sources.contains(available)) {
                    if (available == std::numeric_limits<ecs::EntityId>::max()) throw std::runtime_error("Scene source ID space exhausted");
                    ++available;
                }
                source = available;
            }
            sources.insert(source);
            snapshot.fileEntities.emplace(source, entity);
        }
    }

    return snapshot;
}

void GameplayState::restoreSceneSnapshot(const SceneSnapshot& snapshot) {
    scripting::ReactRuntime::reset(m_scripts);
    m_scripts.stop();
    m_pendingRuntimeDeletes.clear();
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
    m_fileEntities.clear();
    for (const auto& [source, oldEntity] : snapshot.fileEntities) {
        if (const auto it = entityMap.find(oldEntity); it != entityMap.end()) m_fileEntities.emplace(source, it->second);
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
        if (entitySnapshot.light) m_world.addComponent<ecs::Light>(entity, *entitySnapshot.light);
        if (entitySnapshot.rigidbody.has_value()) {
            m_world.addComponent<ecs::Rigidbody>(entity, *entitySnapshot.rigidbody);
        }
        if (entitySnapshot.collider.has_value()) {
            m_world.addComponent<ecs::Collider>(entity, *entitySnapshot.collider);
        }
        if (entitySnapshot.meshRenderer.has_value()) {
            m_world.addComponent<ecs::MeshRenderer>(entity, *entitySnapshot.meshRenderer);
        }
        if (entitySnapshot.script.has_value()) {
            m_world.addComponent<scripting::ScriptComponent>(entity, *entitySnapshot.script);
        }
        if (entitySnapshot.networkReplica) {
            m_world.addComponent<net::NetworkReplica>(entity, *entitySnapshot.networkReplica);
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
    if (!m_world.hasComponent<ecs::Camera>(m_cameraEntity) || !m_world.hasComponent<ecs::Transform>(m_cameraEntity)) {
        createEditorCamera();
    }
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
    return !stack().standalone();
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

void GameplayState::beginSceneEdit(const std::string& label, SceneSnapshot before, const ecs::EntityId entity) {
    if (!canEditScene() || m_pendingSceneEdit.has_value()) {
        return;
    }
    m_pendingSceneEdit = PendingSceneEdit{label, std::move(before), false, entity};
}

void GameplayState::trackEditedItem(const std::string& label, const bool changed, const EntitySnapshot& before) {
    if (!canEditScene()) {
        return;
    }

    const bool activated = ImGui::IsItemActivated();
    if ((activated || changed) && m_pendingSceneEdit.has_value() &&
        (m_pendingSceneEdit->label != label || m_pendingSceneEdit->entity != before.sourceId)) {
        commitSceneEdit();
    }
    if ((activated || changed) && !m_pendingSceneEdit.has_value()) {
        SceneSnapshot snapshot = captureSceneSnapshot();
        const auto entity = std::find_if(snapshot.entities.begin(), snapshot.entities.end(), [&](const EntitySnapshot& item) {
            return item.sourceId == before.sourceId;
        });
        if (entity == snapshot.entities.end()) {
            return;
        }
        *entity = before;
        if (activated) {
            beginSceneEdit(label, std::move(snapshot), before.sourceId);
        } else {
            pushHistorySnapshot(label, std::move(snapshot));
            m_editorStatusMessage = label;
        }
    }

    if (changed && m_pendingSceneEdit.has_value()) {
        m_pendingSceneEdit->changed = true;
    }

    if (ImGui::IsItemDeactivated() && m_pendingSceneEdit.has_value() &&
        m_pendingSceneEdit->label == label && m_pendingSceneEdit->entity == before.sourceId) {
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
    const bool wasRunning = m_simulationRunning;
    restoreSceneSnapshot(entry.snapshot);
    if (m_editorMode == EditorMode::Play) { m_scripts.start(); m_simulationRunning = wasRunning; }
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
    const bool wasRunning = m_simulationRunning;
    restoreSceneSnapshot(entry.snapshot);
    if (m_editorMode == EditorMode::Play) { m_scripts.start(); m_simulationRunning = wasRunning; }
    m_editorStatusMessage = "Redo: " + entry.label;
}

std::string GameplayState::serializeScene(const SceneSnapshot& snapshot, const bool playSession) const {
    std::unordered_map<ecs::EntityId, ecs::EntityId> authoredIds;
    if (!playSession) for (const auto& [source, entity] : snapshot.fileEntities) authoredIds.emplace(entity, source);
    const auto serializedId = [&](const ecs::EntityId entity) {
        const auto found = authoredIds.find(entity);
        return found == authoredIds.end() ? entity : found->second;
    };
    nlohmann::json sceneJson;
    sceneJson["version"] = 1;
    if (playSession) {
        sceneJson["liveSourceMap"] = nlohmann::json::object();
        for (const auto& [source, entity] : snapshot.fileEntities) sceneJson["liveSourceMap"][std::to_string(source)] = entity;
    }
    sceneJson["cameraEntity"] = serializedId(snapshot.cameraEntity);
    sceneJson["strikerEntity"] = serializedId(snapshot.strikerEntity);
    sceneJson["showcaseSphereEntity"] = serializedId(snapshot.showcaseSphereEntity);
    sceneJson["selectedEntity"] = serializedId(snapshot.selectedEntity);
    sceneJson["spawnSequence"] = snapshot.spawnSequence;
    sceneJson["showColliderDebug"] = snapshot.showColliderDebug;
    sceneJson["showcaseSphereLaunched"] = snapshot.showcaseSphereLaunched;
    sceneJson["entities"] = nlohmann::json::array();

    for (const EntitySnapshot& entitySnapshot : snapshot.entities) {
        nlohmann::json entityJson;
        entityJson["sourceId"] = serializedId(entitySnapshot.sourceId);
        if (entitySnapshot.transform) entityJson["transform"] = transformToJson(*entitySnapshot.transform);
        if (entitySnapshot.tag) entityJson["tag"] = {{"value", entitySnapshot.tag->value}};
        if (entitySnapshot.hierarchy) entityJson["hierarchy"] = {{"parent", serializedId(entitySnapshot.hierarchy->parent)}};
        if (entitySnapshot.camera) entityJson["camera"] = cameraToJson(*entitySnapshot.camera);
        if (entitySnapshot.light) entityJson["light"] = ecs::lightToJson(*entitySnapshot.light);
        if (entitySnapshot.rigidbody) entityJson["rigidbody"] = rigidbodyToJson(*entitySnapshot.rigidbody);
        if (entitySnapshot.collider) entityJson["collider"] = colliderToJson(*entitySnapshot.collider);
        if (entitySnapshot.meshRenderer) entityJson["meshRenderer"] = meshRendererToJson(*entitySnapshot.meshRenderer);
        if (entitySnapshot.script) {
            const auto& script = *entitySnapshot.script;
            entityJson["script"] = {{"path", script.path}, {"enabled", script.enabled}, {"fields", script.fields}};
        }
        if (entitySnapshot.networkReplica) {
            entityJson["networkReplica"] = {{"key", entitySnapshot.networkReplica->key}};
        }
        sceneJson["entities"].push_back(std::move(entityJson));
    }
    return sceneJson.dump(4);
}

void GameplayState::launchStandalonePlay() {
    commitSceneEdit();
    std::string error;
    try {
        if (stack().launchPlayWindow(serializeScene(captureSceneSnapshot(), true), editorAssetsRoot(), error)) {
            if (m_editorMode == EditorMode::Play) stopPlayMode();
            m_editorStatusMessage = "Game started in a separate window.";
        } else m_editorStatusMessage = error;
    } catch (const std::exception& failure) { m_editorStatusMessage = failure.what(); }
}

bool GameplayState::saveSceneToDisk() {
    if (m_editorMode == EditorMode::Play || !canEditScene()) {
        m_editorStatusMessage = "Stop Play mode before saving.";
        return false;
    }

    commitSceneEdit();
    const std::filesystem::path scenePath = editorScenePath();
    try {
        if (scenePath.has_parent_path()) {
            std::filesystem::create_directories(scenePath.parent_path());
        }

        std::string error;
        const auto snapshot = captureSceneSnapshot();
        const auto contents = serializeScene(snapshot);
        if (!core::writeFileAtomically(scenePath, contents, error)) {
            m_editorStatusMessage = "Failed to save scene.";
            ENGINE_LOG_WARN("Failed to save scene '{}': {}", scenePath.string(), error);
            return false;
        }
        m_diskScene = nlohmann::json::parse(contents);
        m_fileEntities.clear();
        for (const auto& [source, entity] : snapshot.fileEntities) if (m_world.isAlive(entity)) m_fileEntities.emplace(source, entity);
        m_projectAssetsDirty = true;
        m_editorStatusMessage = "Saved scene to " + scenePath.string();
        ENGINE_LOG_INFO("Saved editor scene to '{}'", scenePath.string());
        return true;
    } catch (const std::exception& ex) {
        m_editorStatusMessage = "Failed to save scene.";
        ENGINE_LOG_WARN("Failed saving scene '{}': {}", scenePath.string(), ex.what());
        return false;
    }
}

GameplayState::SceneSnapshot GameplayState::parseScene(const nlohmann::json& sceneJson) const {
    if (!sceneJson.is_object() || sceneJson.value("version", 0) != 1 ||
        !sceneJson.contains("entities") || !sceneJson["entities"].is_array()) {
        throw std::runtime_error("Scene requires version 1 and an entities array");
    }

    SceneSnapshot snapshot{};
    std::unordered_set<ecs::EntityId> sourceIds;
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
        if (entitySnapshot.sourceId == ecs::kInvalidEntity || !sourceIds.insert(entitySnapshot.sourceId).second) {
            throw std::runtime_error("Scene entity IDs must be unique and valid");
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
        if (entityJson.contains("light")) entitySnapshot.light = ecs::lightFromJson(entityJson["light"]);
        if (entityJson.contains("rigidbody")) {
            entitySnapshot.rigidbody = rigidbodyFromJson(entityJson["rigidbody"]);
        }
        if (entityJson.contains("collider")) {
            entitySnapshot.collider = colliderFromJson(entityJson["collider"]);
        }
        if (entityJson.contains("meshRenderer")) {
            entitySnapshot.meshRenderer = meshRendererFromJson(entityJson["meshRenderer"]);
        }
        if (entityJson.contains("script")) {
            const auto& script = entityJson["script"];
            scripting::ScriptComponent component;
            component.path = script.value("path", std::string{});
            component.enabled = script.value("enabled", true);
            component.fields = script.value("fields", nlohmann::json::object());
            if (!component.fields.is_object()) { throw std::runtime_error("Script fields must be an object"); }
            entitySnapshot.script = std::move(component);
        }
        if (entityJson.contains("networkReplica")) {
            const auto& replica = entityJson.at("networkReplica");
            const auto key = replica.contains("key") && replica.at("key").is_number_integer()
                ? replica.at("key").get<std::uint64_t>() : 0;
            if (key == 0 || key > net::MaxReplicaKey) {
                throw std::runtime_error("Network Replica key must be an integer from 1 to 2147483647");
            }
            entitySnapshot.networkReplica = net::NetworkReplica{static_cast<std::uint32_t>(key)};
        }
        snapshot.fileEntities.emplace(entitySnapshot.sourceId, entitySnapshot.sourceId);
        snapshot.entities.push_back(std::move(entitySnapshot));
    }
    if (stack().standalone() && sceneJson.contains("liveSourceMap")) {
        snapshot.fileEntities.clear();
        for (const auto& [source, entity] : sceneJson.at("liveSourceMap").items()) snapshot.fileEntities.emplace(static_cast<ecs::EntityId>(std::stoul(source)), entity.get<ecs::EntityId>());
    }
    return snapshot;
}

void GameplayState::pollSceneChanges() {
    for (const auto& change : m_sceneWatcher.poll()) {
        try {
            if (!change.error.empty()) throw std::runtime_error(change.error);
            const auto scene = nlohmann::json::parse(change.contents);
            if (scene == m_diskScene) continue;
            applyLiveScene(scene);
        } catch (const std::exception& error) {
            m_editorStatusMessage = "Live scene retained: " + std::string(error.what());
            ENGINE_LOG_WARN("Live scene rejected: {}", error.what());
        }
    }
}

void GameplayState::applyLiveScene(const nlohmann::json& scene) {
    using Json = nlohmann::json;
    const auto normalized = [&](const Json& value) {
        auto snapshot = parseScene(value);
        snapshot.fileEntities.clear();
        return Json::parse(serializeScene(snapshot));
    };
    const auto nextScene = normalized(scene);
    std::unordered_map<ecs::EntityId, Json> oldEntities, newEntities;
    if (m_diskScene.contains("entities")) {
        const auto previousScene = normalized(m_diskScene);
        for (const auto& entity : previousScene.at("entities")) oldEntities.emplace(entity.at("sourceId").get<ecs::EntityId>(), entity);
    }
    for (const auto& entity : nextScene.at("entities")) newEntities.emplace(entity.at("sourceId").get<ecs::EntityId>(), entity);
    const auto merge = [&](auto&& visit, Json current, const Json& old, const Json& next) -> Json {
        if (old == next) return current;
        if (!old.is_object() || !next.is_object()) return next;
        if (!current.is_object()) current = Json::object();
        for (const auto& [key, value] : old.items()) { (void)value; if (!next.contains(key)) current.erase(key); }
        for (const auto& [key, value] : next.items()) {
            if (!old.contains(key)) current[key] = value;
            else if (old.at(key) != value) current[key] = visit(visit, current.value(key, Json()), old.at(key), value);
        }
        return current;
    };
    struct Edit { ecs::EntityId source; EntitySnapshot snapshot; bool remapParent; };
    std::vector<Edit> edits;
    for (const auto& [source, next] : newEntities) {
        const auto old = oldEntities.find(source);
        if (old != oldEntities.end() && old->second == next) continue;
        Json current = Json::object();
        if (const auto found = m_fileEntities.find(source); found != m_fileEntities.end() && m_world.isAlive(found->second)) {
            SceneSnapshot single; single.entities.push_back(captureEntitySnapshot(found->second));
            current = Json::parse(serializeScene(single)).at("entities").front();
        }
        auto merged = merge(merge, current, old == oldEntities.end() ? Json() : old->second, next);
        merged["sourceId"] = source;
        const Json one = {{"version", 1}, {"entities", Json::array({merged})}};
        const bool parentChanged = old == oldEntities.end() || old->second.value("hierarchy", Json()) != next.value("hierarchy", Json());
        edits.push_back({source, parseScene(one).entities.front(), parentChanged});
    }
    commitSceneEdit();
    if (canEditScene()) recordSceneHistory("Live Scene Update");
    for (const auto& [source, previous] : oldEntities) {
        (void)previous;
        if (newEntities.contains(source)) continue;
        if (const auto found = m_fileEntities.find(source); found != m_fileEntities.end()) {
            deleteEntityNow(found->second, false);
            m_fileEntities.erase(source);
        }
    }
    for (const auto& edit : edits) {
        const auto found = m_fileEntities.find(edit.source);
        if (found == m_fileEntities.end() || !m_world.isAlive(found->second)) m_fileEntities[edit.source] = m_world.createEntity();
    }
    const auto remap = [&](const ecs::EntityId source) {
        const auto found = m_fileEntities.find(source);
        return found == m_fileEntities.end() ? ecs::kInvalidEntity : found->second;
    };
    for (auto& edit : edits) {
        const auto entity = m_fileEntities.at(edit.source);
        auto& data = edit.snapshot;
        if (data.hierarchy && edit.remapParent) data.hierarchy->parent = remap(data.hierarchy->parent);
        if (data.hierarchy && !m_world.isAlive(data.hierarchy->parent)) data.hierarchy->parent = ecs::kInvalidEntity;
        const auto assign = [&]<class T>(const std::optional<T>& component) {
            if (component) m_world.addComponent<T>(entity, *component);
            else m_world.removeComponent<T>(entity);
        };
        assign(data.transform); assign(data.tag); assign(data.hierarchy); assign(data.camera);
        assign(data.light);
        assign(data.rigidbody); assign(data.collider); assign(data.meshRenderer); assign(data.script);
        assign(data.networkReplica);
    }
    if (scene.value("cameraEntity", ecs::kInvalidEntity) != m_diskScene.value("cameraEntity", ecs::kInvalidEntity))
        m_cameraEntity = remap(scene.value("cameraEntity", ecs::kInvalidEntity));
    if (!m_world.isAlive(m_cameraEntity)) createEditorCamera();
    ensureSelectedEntityValid();
    m_diskScene = scene; m_projectAssetsDirty = true;
    m_editorStatusMessage = "Live scene updated (game state preserved).";
    ENGINE_LOG_INFO("Live scene applied: {} changes, {} entities", edits.size(), m_world.aliveCount());
}

bool GameplayState::loadSceneFromDisk() {
    if (m_editorMode != EditorMode::Edit) {
        m_editorStatusMessage = "Stop Play mode before loading.";
        return false;
    }

    const std::filesystem::path scenePath = editorScenePath();
    if (!std::filesystem::exists(scenePath)) {
        m_editorStatusMessage = "Scene file not found.";
        ENGINE_LOG_WARN("Scene file '{}' not found", scenePath.string());
        return false;
    }

    try {
        std::ifstream file(scenePath);
        if (!file.is_open()) {
            m_editorStatusMessage = "Failed to load scene.";
            ENGINE_LOG_WARN("Failed to open scene '{}' for reading", scenePath.string());
            return false;
        }

        nlohmann::json sceneJson;
        file >> sceneJson;
        const auto snapshot = parseScene(sceneJson);

        if (canEditScene()) {
            recordSceneHistory("Load Scene");
        }
        restoreSceneSnapshot(snapshot);
        m_diskScene = sceneJson;
        m_editorStatusMessage = "Loaded scene from " + scenePath.string();
        ENGINE_LOG_INFO("Loaded editor scene from '{}'", scenePath.string());
        return true;
    } catch (const std::exception& ex) {
        m_editorStatusMessage = "Failed to load scene.";
        ENGINE_LOG_WARN("Failed loading scene '{}': {}", scenePath.string(), ex.what());
        return false;
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
    if (const auto* light = m_world.getComponent<ecs::Light>(entity)) m_world.addComponent<ecs::Light>(copy, *light);
    if (const auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(entity)) {
        m_world.addComponent<ecs::MeshRenderer>(copy, *meshRenderer);
    }
    if (const auto* collider = m_world.getComponent<ecs::Collider>(entity)) {
        m_world.addComponent<ecs::Collider>(copy, *collider);
    }
    if (const auto* script = m_world.getComponent<scripting::ScriptComponent>(entity)) {
        m_world.addComponent<scripting::ScriptComponent>(copy, *script);
    }
    if (m_world.hasComponent<net::NetworkReplica>(entity)) {
        if (const auto key = nextReplicaKey(m_world); key != 0) {
            m_world.addComponent<net::NetworkReplica>(copy, net::NetworkReplica{key});
        }
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
    if (!m_world.isAlive(entity)) {
        return;
    }

    if (m_editorMode == EditorMode::Play) {
        if (std::find(m_pendingRuntimeDeletes.begin(), m_pendingRuntimeDeletes.end(), entity) ==
            m_pendingRuntimeDeletes.end()) {
            recordSceneHistory("Delete Entity");
            m_pendingRuntimeDeletes.push_back(entity);
        }
        m_editorStatusMessage = "Runtime delete queued: " + entityDisplayName(entity);
        return;
    }
    deleteEntityNow(entity);
}

void GameplayState::applyPendingRuntimeDeletes() {
    auto pending = std::move(m_pendingRuntimeDeletes);
    m_pendingRuntimeDeletes.clear();
    for (const auto entity : pending) {
        deleteEntityNow(entity);
    }
}

void GameplayState::deleteEntityNow(const ecs::EntityId entity, const bool recordHistory) {
    if (!m_world.isAlive(entity)) { return; }
    if (recordHistory && m_editorMode == EditorMode::Edit && canEditScene()) { recordSceneHistory("Delete Entity"); }
    m_world.forEachEntity([&](const ecs::EntityId candidate) {
        if (auto* hierarchy = m_world.getComponent<ecs::Hierarchy>(candidate);
            hierarchy != nullptr && hierarchy->parent == entity) {
            hierarchy->parent = ecs::kInvalidEntity;
        }
    });

    const std::string deletedName = entityDisplayName(entity);
    m_physicsSystem.forgetEntity(entity, m_eventBus);
    m_scripts.forgetEntity(entity);
    m_world.destroyEntity(entity);
    if (m_cameraEntity == entity) {
        m_cameraEntity = ecs::kInvalidEntity;
        setCameraLookActive(false);
    }
    if (m_strikerEntity == entity) {
        m_strikerEntity = ecs::kInvalidEntity;
    }
    if (m_showcaseSphereEntity == entity) {
        m_showcaseSphereEntity = ecs::kInvalidEntity;
        m_showcaseSphereLaunched = false;
    }
    if (m_hoveredEntity == entity) { m_hoveredEntity = ecs::kInvalidEntity; }
    if (m_selectedEntity == entity) {
        m_selectedEntity = ecs::kInvalidEntity;
        m_gizmoEditSnapshot.reset();
        m_gizmoWasUsing = false;
        m_gizmoChanged = false;
    }
    ensureSelectedEntityValid();
    m_editorStatusMessage = "Deleted " + deletedName;
}

void GameplayState::setCameraLookActive(const bool active) {
    if (m_cameraLookActive == active || (active && m_scripts.cursorLocked())) {
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
    using Command = editor::EditorCommandId;
    if (ImGui::GetCurrentContext() && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    const bool controlDown = platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftControl) ||
                             platform::InputManager::isKeyDownRaw(platform::KeyCode::RightControl);
    if (!controlDown) return;
    if (platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftAlt) ||
        platform::InputManager::isKeyDownRaw(platform::KeyCode::RightAlt)) return;
    const bool shiftDown = platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftShift) ||
                           platform::InputManager::isKeyDownRaw(platform::KeyCode::RightShift);
    const auto pressed = [](platform::KeyCode key) { return platform::InputManager::wasKeyPressedRaw(key); };
    if (m_showScriptWorkspace && m_scriptWorkspace.focused()) {
        if (pressed(platform::KeyCode::S)) m_editorCommands.execute(shiftDown ? Command::ScriptSaveAll : Command::ScriptSave);
        else if (pressed(platform::KeyCode::O)) m_editorCommands.execute(Command::ScriptOpen);
        else if (pressed(platform::KeyCode::N)) m_editorCommands.execute(Command::ScriptNew);
        else if (pressed(platform::KeyCode::F)) {
            if (!m_editorCommands.state(Command::ScriptFind).checked) m_editorCommands.execute(Command::ScriptFind);
        }
        else if (ImGui::GetCurrentContext() && ImGui::IsKeyPressed(ImGuiKey_G, false)) m_editorCommands.execute(Command::ScriptGoToLine);
        return;
    }
    if (sceneShortcutsBlocked() || m_cameraLookActive || m_scripts.cursorLocked()) return;
    if (pressed(platform::KeyCode::S)) m_editorCommands.execute(Command::SaveScene);
    else if (pressed(platform::KeyCode::O)) m_editorCommands.execute(Command::LoadScene);
    else if (pressed(platform::KeyCode::N)) m_editorCommands.execute(Command::CreateEmpty);
    else if (pressed(platform::KeyCode::D)) m_editorCommands.execute(Command::Duplicate);
    else if (pressed(platform::KeyCode::Y) || (shiftDown && pressed(platform::KeyCode::Z))) m_editorCommands.execute(Command::Redo);
    else if (pressed(platform::KeyCode::Z)) m_editorCommands.execute(Command::Undo);
}

void GameplayState::applyViewportHotkeys() {
    if (!(m_viewportHovered || m_viewportFocused) || m_viewportUiCaptured || sceneShortcutsBlocked() ||
        m_cameraLookActive || m_scripts.cursorLocked()) return;
    const bool modified = platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftControl) ||
        platform::InputManager::isKeyDownRaw(platform::KeyCode::RightControl) ||
        platform::InputManager::isKeyDownRaw(platform::KeyCode::LeftAlt) ||
        platform::InputManager::isKeyDownRaw(platform::KeyCode::RightAlt);
    if (modified) return;
    using Command = editor::EditorCommandId;
    if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::Q)) m_editorCommands.execute(Command::ToolSelect);
    else if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::W)) m_editorCommands.execute(Command::ToolMove);
    else if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::E)) m_editorCommands.execute(Command::ToolRotate);
    else if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::R)) m_editorCommands.execute(Command::ToolScale);
    else if (platform::InputManager::wasKeyPressedRaw(platform::KeyCode::F)) m_editorCommands.execute(Command::FocusSelected);
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
        if (asset.path.starts_with("assets/scripts/react-luau/")) { continue; }
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

bool GameplayState::hasDockedPanel(const EditorDockSlot slot) const {
    return (m_showToolbarPanel && m_toolbarDockSlot == slot) ||
        (m_showHierarchyPanel && m_hierarchyDockSlot == slot) ||
        (m_showProjectPanel && m_projectDockSlot == slot) ||
        (m_showInspectorPanel && m_inspectorDockSlot == slot) ||
        (m_showStatsPanel && m_statsDockSlot == slot) ||
        (m_showStatusPanel && m_statusDockSlot == slot);
}

void GameplayState::resetEditorLayout() {
    m_ribbonState = {};
    m_showToolbarPanel = true;
    m_showHierarchyPanel = false;
    m_showProjectPanel = false;
    m_showInspectorPanel = false;
    m_showStatsPanel = false;
    m_showAssistant = true;
    m_showViewportPanel = true;
    m_materialGraphDocked = true;
    m_showStatusPanel = true;
    m_toolbarDockSlot = EditorDockSlot::Top;
    m_hierarchyDockSlot = EditorDockSlot::RightTop;
    m_projectDockSlot = EditorDockSlot::LeftBottom;
    m_inspectorDockSlot = EditorDockSlot::RightBottom;
    m_statsDockSlot = EditorDockSlot::LeftTop;
    m_viewportDockSlot = EditorDockSlot::Center;
    m_statusDockSlot = EditorDockSlot::Bottom;
    m_editorLeftWidth = 0.0F;
    m_editorRightWidth = 0.0F;
    m_editorLeftSplit = 0.72F;
    m_editorRightSplit = 0.76F;
    m_materialGraphSplit = 0.55F;
    m_resetEditorLayout = true;
    markEditorLayoutDirty();
}

void GameplayState::loadEditorLayout() {
    m_materialGraphSession.reset();
    m_materialGraphSelectionSource = ecs::kInvalidEntity;
    m_restoreMaterialGraphPending = false;
    m_ribbonState.networkTest = net::NetworkTestOptions{};
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
            m_showAssistant = visibility->value("assistant", m_showAssistant);
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

        if (const auto graph = layout.find("materialGraph"); graph != layout.end() && graph->is_object()) {
            if (const auto dock = graph->find("dockBesideViewport"); dock != graph->end() && dock->is_boolean()) {
                m_materialGraphDocked = dock->get<bool>();
            }
            if (const auto split = graph->find("split"); split != graph->end() && split->is_number()) {
                const float value = split->get<float>();
                if (std::isfinite(value)) m_materialGraphSplit = std::clamp(value, 0.20F, 0.80F);
            }
            if (m_project && m_materialGraphDocked) {
                m_materialGraphSession = readMaterialGraphSession(*graph);
                m_restoreMaterialGraphPending = m_materialGraphSession.has_value();
                if (m_materialGraphSession) {
                    const auto& session = graph->at("session");
                    const auto selected = session.find("selectedSourceId");
                    if (selected != session.end() && selected->is_number_unsigned() &&
                        selected->get<std::uint64_t>() <= std::numeric_limits<ecs::EntityId>::max()) {
                        m_materialGraphSelectionSource = selected->get<ecs::EntityId>();
                    }
                }
            }
        }

        if (const auto ribbon = layout.find("ribbon"); ribbon != layout.end() && ribbon->is_object()) {
            m_ribbonState.tab = static_cast<editor::EditorRibbonTab>(ribbon->value("tab", 0));
            m_ribbonState.compact = ribbon->value("compact", false);
            m_ribbonState.showLabels = ribbon->value("showLabels", true);
            m_ribbonState.createCommand = static_cast<editor::EditorCommandId>(ribbon->value("createCommand", static_cast<int>(editor::EditorCommandId::CreatePhysicsCube)));
            m_ribbonState.playCommand = static_cast<editor::EditorCommandId>(ribbon->value("playCommand", static_cast<int>(editor::EditorCommandId::Play)));
            m_ribbonState.moveSnap = ribbon->value("moveSnap", 0.5F);
            m_ribbonState.rotateSnap = ribbon->value("rotateSnap", 15.0F);
            m_ribbonState.scaleSnap = ribbon->value("scaleSnap", 0.1F);
            if (const auto options = ribbon->find("networkTest"); options != ribbon->end() && options->is_object()) {
                const auto readOption = [&options](const char* name, const int fallback, const int minimum, const int maximum) {
                    const auto value = options->find(name);
                    if (value == options->end()) return fallback;
                    if (value->is_number_unsigned()) {
                        return static_cast<int>(std::clamp(value->get<std::uint64_t>(),
                            static_cast<std::uint64_t>(minimum), static_cast<std::uint64_t>(maximum)));
                    }
                    if (value->is_number_integer()) {
                        return static_cast<int>(std::clamp(value->get<std::int64_t>(),
                            static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum)));
                    }
                    return fallback;
                };
                auto& network = m_ribbonState.networkTest;
                network.clientCount = readOption("clientCount", network.clientCount, 1, net::MaxNetworkTestClients);
                network.latencyMs = readOption("latencyMs", network.latencyMs, 0, net::MaxNetworkTestLatencyMs);
                network.lossPercent = readOption("lossPercent", network.lossPercent, 0, net::MaxNetworkTestLossPercent);
            }
            editor::EditorRibbon::sanitizeState(m_ribbonState);
            m_useLocalGizmoSpace = ribbon->value("localSpace", m_useLocalGizmoSpace);
            m_snapEnabled = ribbon->value("snap", m_snapEnabled);
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
    layout["ribbon"] = {
        {"tab", static_cast<int>(m_ribbonState.tab)}, {"compact", m_ribbonState.compact}, {"showLabels", m_ribbonState.showLabels},
        {"createCommand", static_cast<int>(m_ribbonState.createCommand)}, {"playCommand", static_cast<int>(m_ribbonState.playCommand)},
        {"moveSnap", m_ribbonState.moveSnap}, {"rotateSnap", m_ribbonState.rotateSnap}, {"scaleSnap", m_ribbonState.scaleSnap},
        {"localSpace", m_useLocalGizmoSpace}, {"snap", m_snapEnabled},
        {"networkTest", {
            {"clientCount", m_ribbonState.networkTest.clientCount},
            {"latencyMs", m_ribbonState.networkTest.latencyMs},
            {"lossPercent", m_ribbonState.networkTest.lossPercent}
        }}
    };
    layout["visibility"] = {
        {"toolbar", m_showToolbarPanel},
        {"hierarchy", m_showHierarchyPanel},
        {"project", m_showProjectPanel},
        {"inspector", m_showInspectorPanel},
        {"stats", m_showStatsPanel},
        {"viewport", m_showViewportPanel},
        {"status", m_showStatusPanel},
        {"assistant", m_showAssistant},
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
    layout["materialGraph"] = {
        {"dockBesideViewport", m_materialGraphDocked},
        {"split", m_materialGraphSplit},
    };
    if (m_materialGraphDocked && m_materialGraphSession) {
        auto& session = layout["materialGraph"]["session"];
        session["materialId"] = m_materialGraphSession->materialId;
        if (const auto& canvas = m_materialGraphSession->canvas) {
            session["canvas"] = {{"pan", {canvas->pan.x, canvas->pan.y}}, {"zoom", canvas->zoom}};
        }
        if (m_materialGraphSelectionSource != ecs::kInvalidEntity) {
            session["selectedSourceId"] = m_materialGraphSelectionSource;
        }
    }

    std::string failure;
    if (!core::writeFileAtomically(path, layout.dump(2) + "\n", failure)) {
        ENGINE_LOG_WARN("Failed to save editor layout '{}': {}", path.string(), failure);
    }
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
    ENGINE_PROFILE_ZONE("Gameplay Update");
    applyPendingRuntimeDeletes();
    m_world.forEach<ecs::RenderPoseHistory, ecs::Transform>(
        [&](ecs::EntityId entity, ecs::RenderPoseHistory& history, const ecs::Transform& transform) {
            history = ecs::captureRenderPose(m_world, entity, transform);
        });
    if (stack().sceneNetworkLaunch()) {
        if (!m_sceneNetwork) return;
        m_sceneNetwork->beginTick();
        m_scripts.setInputEnabled(m_sceneNetwork->acceptsLocalInput() && !m_viewportUiCaptured &&
            !(ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput));
        updateSimulation(dt);
        m_sceneNetwork->endTick();
        if (m_sceneNetwork->acceptsLocalInput()) platform::InputManager::clearTickKeyEdges();
        return;
    }
    if (!m_sceneInitialized) {
        createDemoScene();
    }

    if (labActive()) {
        const auto& options = *stack().labOptions();
        if (options.scene == "loading" && !m_labLoadRequested &&
            stack().labElapsedSeconds() >= options.loadAtSeconds) {
            requestLabAssets();
        }
        return;
    }

    ensureSelectedEntityValid();

    m_codexAssistant.update();
    m_scriptWorkspace.update();
    pollSceneChanges();
    m_scripts.pollReload();
    const bool cursorLocked = m_scripts.cursorLocked();
    const bool gameViewportInput = (stack().standalone() || m_viewportHovered || m_viewportFocused) && !m_viewportUiCaptured;
    m_scripts.setInputEnabled(m_editorMode == EditorMode::Play && (cursorLocked || gameViewportInput) &&
        !m_cameraLookActive && !(ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput));
    m_scripts.setPointerInputEnabled(cursorLocked || ((stack().standalone() || m_viewportHovered) && !m_viewportUiCaptured));

    if (((m_viewportHovered && !m_viewportUiCaptured) || m_cameraLookActive) && stack().window() != nullptr) {
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

    if (auto* cameraTransform = m_world.getComponent<ecs::Transform>(m_cameraEntity); cameraTransform && !stack().standalone()) {
        const bool allowMouseLook = m_cameraLookActive && !m_skipNextCameraLookDelta;
        if (m_skipNextCameraLookDelta) {
            platform::InputManager::resetMouseDelta();
        }
        m_cameraController.update(*cameraTransform, dt, m_cameraLookActive, allowMouseLook);
        m_skipNextCameraLookDelta = false;
    }

    if (m_editorMode == EditorMode::Play) updateSimulation(dt);
}

void GameplayState::updateSimulation(double dt) {
    processPendingSpawns();
    if (!m_simulationRunning && !m_stepScriptTick) return;
    const double tickDt = m_stepScriptTick ? 1.0 / 60.0 : dt;
    m_stepScriptTick = false;
    m_scripts.update(tickDt);
    if (!m_sceneNetwork || !m_sceneNetwork->isClient()) {
        ENGINE_PROFILE_ZONE("Physics");
        m_physicsSystem.update(m_world, tickDt, m_eventBus);
    }
    m_scripts.dispatchEvents();
    scripting::ReactRuntime::pump(m_scripts, tickDt);
}

GameplayState::CameraFrame GameplayState::buildActiveCameraFrame(const renderer::Renderer& renderer, float renderFraction) const {
    const rhi::Extent2D frameExtent = renderer.frameExtent();
    const float viewportWidth = m_viewportSize.x > 1.0F ? m_viewportSize.x : static_cast<float>(frameExtent.width);
    const float viewportHeight = m_viewportSize.y > 1.0F ? m_viewportSize.y : static_cast<float>(frameExtent.height);
    const float aspectRatio = viewportHeight > 0.0F ? viewportWidth / viewportHeight : (16.0F / 9.0F);

    CameraFrame cameraFrame{};
    cameraFrame.aspectRatio = aspectRatio;

    bool foundCamera = false;
    m_world.forEach<ecs::Transform, ecs::Camera>([&](ecs::EntityId entity, const ecs::Transform& transform, const ecs::Camera& camera) {
        if (foundCamera || !camera.active) {
            return;
        }

        const auto pose = ecs::sampleRenderTransform(m_world, entity, transform, renderFraction);
        const float fov = ecs::sampleRenderCameraFov(m_world, entity, camera.verticalFovRadians, renderFraction);
        const glm::vec3 forward = ecs::forwardFromEuler(pose.rotationEulerRadians);
        const glm::vec3 up = ecs::upFromEuler(pose.rotationEulerRadians);
        cameraFrame.projection =
            glm::perspectiveRH_ZO(fov, aspectRatio, camera.nearPlane, camera.farPlane);
        cameraFrame.view = glm::lookAtRH(pose.position, pose.position + forward, up);
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
    const glm::vec2 renderPosition = m_viewportUsesContentRect
        ? m_viewportPosition : glm::vec2(mainViewport->Pos.x, mainViewport->Pos.y);
    const glm::vec2 renderSize = m_viewportUsesContentRect
        ? m_viewportSize : glm::vec2(mainViewport->Size.x, mainViewport->Size.y);
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
    if (!m_viewportHovered || m_viewportUiCaptured || m_cameraLookActive || m_scripts.cursorLocked() ||
        ImGuizmo::IsUsing() || ImGuizmo::IsOver()) {
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

    inspectEntity(hit->entity);
    m_editorStatusMessage = "Selected " + entityDisplayName(hit->entity);
}

void GameplayState::updateHoveredEntity(const CameraFrame& cameraFrame) {
    if (!m_viewportHovered || m_viewportUiCaptured || m_cameraLookActive || m_scripts.cursorLocked() || ImGuizmo::IsUsing()) {
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
    ENGINE_PROFILE_ZONE("Gameplay Render");
    const auto renderStart = std::chrono::high_resolution_clock::now();
    m_viewportUsesContentRect = false;
    if (rendererInstance.isImGuiEnabled()) restoreMaterialGraphLayout(rendererInstance.materialLibrary());
    if (rendererInstance.isImGuiEnabled() && m_materialGraphDocked &&
        editor::materialGraphEditorOpen(rendererInstance.materialLibrary())) {
        if (const auto workspace = materialGraphWorkspace()) {
            m_viewportPosition = workspace->viewportPos + glm::vec2(0.0F, kViewportHeaderHeight);
            m_viewportSize = workspace->viewportSize - glm::vec2(0.0F, kViewportHeaderHeight);
            const ImVec2 framePosition = ImGui::GetMainViewport()->Pos;
            rendererInstance.setSceneViewport(m_viewportPosition - glm::vec2(framePosition.x, framePosition.y), m_viewportSize);
            m_viewportUsesContentRect = true;
        }
    }
    const float renderFraction = m_simulationRunning ? stack().renderInterpolationFraction(*this) : 1.0F;
    const CameraFrame cameraFrame = buildActiveCameraFrame(rendererInstance, renderFraction);
    renderer::RenderAdapter renderAdapter{rendererInstance, glm::vec3(glm::inverse(cameraFrame.view)[3])};
    {
        ENGINE_PROFILE_ZONE("Scene Render");
        const bool previewSelected = m_materialPreview && m_materialPreviewEntity == m_selectedEntity &&
            materialGraphViewportVisible() && rendererInstance.isImGuiEnabled();
        m_renderSystem.render(m_world, renderAdapter, cameraFrame.viewProjection,
            previewSelected ? m_materialPreviewEntity : ecs::kInvalidEntity,
            previewSelected ? m_materialPreview : nullptr, renderFraction);
    }
    if (m_showColliderDebug && !stack().standalone()) {
        ENGINE_PROFILE_ZONE("Collider Debug Render");
        m_debugRenderSystem.render(m_world, renderAdapter, cameraFrame.viewProjection);
    }
    {
        ENGINE_PROFILE_ZONE("Selection Outline");
        if (!stack().standalone()) renderSelectionOutline(renderAdapter, cameraFrame.viewProjection);
    }
    if (m_viewportUsesContentRect) rendererInstance.resetSceneViewport();

    m_lastSceneRenderMs = std::chrono::duration<double, std::milli>(
                              std::chrono::high_resolution_clock::now() - renderStart)
                              .count();
    static const bool diagnostics = std::getenv("TGE_FRAME_DIAGNOSTICS") != nullptr;
    static auto nextDiagnostic = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (diagnostics && now >= nextDiagnostic) {
        nextDiagnostic = now + std::chrono::seconds(1);
        ENGINE_LOG_INFO("Game scene: entities={} draws={} culled={} Luau={:.2f} MiB scene={:.2f} ms",
            m_world.aliveCount(), m_renderSystem.submittedCount(), m_renderSystem.culledCount(),
            static_cast<double>(m_scripts.memoryUsed()) / (1024.0*1024.0), m_lastSceneRenderMs);
    }
}

void GameplayState::renderCreateEntityMenu() {
    using Command = editor::EditorCommandId;
    for (const auto id : {Command::CreateEmpty, Command::CreateRenderCube, Command::CreatePhysicsCube,
        Command::CreatePhysicsSphere, Command::CreatePhysicsPyramid, Command::CreateCamera}) renderCommandMenuItem(id);
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
    if (m_world.hasComponent<ecs::Transform>(entity) && !m_world.hasComponent<ecs::Light>(entity) && ImGui::MenuItem("Light")) {
        recordSceneHistory("Add Light");
        m_world.addComponent<ecs::Light>(entity, ecs::Light{});
        m_editorStatusMessage = "Added Light";
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
    if (!m_world.hasComponent<scripting::ScriptComponent>(entity) && ImGui::MenuItem("Luau Behavior")) {
        recordSceneHistory("Add Luau Behavior");
        m_world.addComponent<scripting::ScriptComponent>(entity, scripting::ScriptComponent{});
        inspectEntity(entity);
    }
    if (!m_world.hasComponent<net::NetworkReplica>(entity) && ImGui::MenuItem("Network Replica")) {
        if (const auto key = nextReplicaKey(m_world); key != 0) {
            recordSceneHistory("Add Network Replica");
            m_world.addComponent<net::NetworkReplica>(entity, net::NetworkReplica{key});
            m_editorStatusMessage = "Added Network Replica.";
            inspectEntity(entity);
        }
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
    if (m_world.hasComponent<ecs::Light>(entity) && ImGui::MenuItem("Light")) {
        recordSceneHistory("Remove Light");
        m_world.removeComponent<ecs::Light>(entity);
        m_editorStatusMessage = "Removed Light";
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
    if (m_world.hasComponent<scripting::ScriptComponent>(entity) && ImGui::MenuItem("Luau Behavior")) {
        recordSceneHistory("Remove Luau Behavior");
        m_scripts.forgetEntity(entity);
        m_world.removeComponent<scripting::ScriptComponent>(entity);
    }
    if (m_world.hasComponent<net::NetworkReplica>(entity) && ImGui::MenuItem("Network Replica")) {
        recordSceneHistory("Remove Network Replica");
        m_world.removeComponent<net::NetworkReplica>(entity);
        m_editorStatusMessage = "Removed Network Replica.";
    }

    if (!canEditScene() || !m_world.isAlive(entity)) {
        ImGui::EndDisabled();
    }
}

void GameplayState::renderEntityContextMenu(const ecs::EntityId entity) {
    m_selectedEntity = entity;
    if (ImGui::MenuItem("Select")) {
        inspectEntity(entity);
    }
    if (ImGui::MenuItem("Properties")) inspectEntity(entity);
    renderCommandMenuItem(editor::EditorCommandId::Duplicate);
    if (ImGui::BeginMenu("Create")) {
        renderCreateEntityMenu();
        ImGui::EndMenu();
    }
    renderCommandMenuItem(editor::EditorCommandId::Delete);
}

void GameplayState::renderPanelHeader(const EditorPanelId panelId, const char* title) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = editor::kPanelTitleHeight;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(32, 34, 39, 255));
    const ImVec2 text = editor::nativeFont() ? editor::nativeFont()->CalcTextSizeA(editor::kEditorFontSize, width, 0, title) : ImGui::CalcTextSize(title);
    draw->AddText(editor::nativeFont(), editor::kEditorFontSize, ImVec2(origin.x + std::max(4.0F, (width - 36 - text.x) * .5F), origin.y + (height - editor::kEditorFontSize) * .5F), IM_COL32(197, 200, 207, 255), title);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + std::max(0.0F, width - 44), origin.y));
    renderPanelDockControls(panelId, "##PanelActions");
    ImGui::SetCursorScreenPos(ImVec2(origin.x + std::max(20.0F, width - 24), origin.y));
    if (ImGui::InvisibleButton("##ClosePanel", ImVec2(18, height))) {
        bool* visible = panelId == EditorPanelId::Hierarchy ? &m_showHierarchyPanel :
            panelId == EditorPanelId::Inspector ? &m_showInspectorPanel :
            panelId == EditorPanelId::Project ? &m_showProjectPanel : &m_showStatsPanel;
        commitSceneEdit(); *visible = false; markEditorLayoutDirty();
    }
    const ImU32 ink = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 close(origin.x + width - 15, origin.y + height * .5F);
    draw->AddLine(ImVec2(close.x - 3, close.y - 3), ImVec2(close.x + 3, close.y + 3), ink);
    draw->AddLine(ImVec2(close.x + 3, close.y - 3), ImVec2(close.x - 3, close.y + 3), ink);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height)); ImGui::Dummy(ImVec2(0, 0));
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
    const float handleHeight = editor::kPanelTitleHeight;
    const float handleWidth = 20.0F;
    const ImVec2 handleMin = ImGui::GetCursorScreenPos();
    const ImVec2 handleSize(handleWidth, handleHeight);

    ImGui::PushID(popupId);
    if (ImGui::InvisibleButton("DockDragGrip", handleSize)) ImGui::OpenPopup(popupId);
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
    if (hovered || active) drawList->AddRectFilled(handleMin, handleMax, bgColor, 2.0F);
    if (hovered || active) drawList->AddRect(handleMin, handleMax, borderColor, 2.0F);
    for (int i = 0; i < 3; ++i) {
        const float x = handleMin.x + 6.0F + static_cast<float>(i) * 4.0F;
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
        if (panelId == EditorPanelId::Inspector) {
            renderCommandMenuItem(editor::EditorCommandId::AddComponent);
            if (ImGui::BeginMenu("Remove Component")) { renderRemoveComponentMenu(m_selectedEntity); ImGui::EndMenu(); }
            for (const auto id : {editor::EditorCommandId::Duplicate, editor::EditorCommandId::Delete,
                editor::EditorCommandId::ResetTransform, editor::EditorCommandId::DropToGround,
                editor::EditorCommandId::RandomizeTint, editor::EditorCommandId::ZeroVelocity}) renderCommandMenuItem(id);
            ImGui::Separator();
        } else if (panelId == EditorPanelId::Hierarchy) {
            if (ImGui::BeginMenu("Create")) { renderCreateEntityMenu(); ImGui::EndMenu(); }
            renderCommandMenuItem(editor::EditorCommandId::Duplicate); renderCommandMenuItem(editor::EditorCommandId::Delete);
            ImGui::Separator();
        } else if (panelId == EditorPanelId::Project && ImGui::MenuItem("Refresh")) { m_projectAssetsDirty = true; }

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
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), true, true, true, true, m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
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
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float handleThickness = 8.0F;

    auto applyChanged = [this](const bool changed) {
        if (changed) {
            markEditorLayoutDirty();
            m_editorStatusMessage = "Editor layout resized.";
        }
    };

    const bool leftColumnActive = layout.hierarchySize.x > 0.0F && renderSplitterHandle(
        "##DockSplitterLeftColumn",
        ImVec2(layout.hierarchyPos.x + layout.hierarchySize.x, layout.hierarchyPos.y),
        ImVec2(handleThickness, layout.hierarchySize.y + kWindowMargin + layout.projectSize.y),
        true);
    if (leftColumnActive && std::abs(io.MouseDelta.x) > 0.001F) {
        m_editorLeftWidth = std::clamp(layout.hierarchySize.x + io.MouseDelta.x, 210.0F, 560.0F);
        applyChanged(true);
    }

    const bool rightColumnActive = layout.inspectorSize.x > 0.0F && renderSplitterHandle(
        "##DockSplitterRightColumn",
        ImVec2(layout.viewportPos.x + layout.viewportSize.x, layout.viewportPos.y),
        ImVec2(handleThickness, layout.viewportSize.y),
        true);
    if (rightColumnActive && std::abs(io.MouseDelta.x) > 0.001F) {
        m_editorRightWidth = std::clamp(layout.inspectorSize.x - io.MouseDelta.x, 280.0F, 620.0F);
        applyChanged(true);
    }

    const bool leftStackActive = layout.hierarchySize.y > 0 && layout.projectSize.y > 0 && renderSplitterHandle(
        "##DockSplitterLeftStack",
        ImVec2(layout.hierarchyPos.x, layout.hierarchyPos.y + layout.hierarchySize.y),
        ImVec2(layout.hierarchySize.x, handleThickness),
        false);
    if (leftStackActive && std::abs(io.MouseDelta.y) > 0.001F) {
        const float stackHeight = std::max(1.0F, layout.hierarchySize.y + layout.projectSize.y);
        m_editorLeftSplit = std::clamp((layout.hierarchySize.y + io.MouseDelta.y) / stackHeight, 0.22F, 0.82F);
        applyChanged(true);
    }

    const bool rightStackActive = layout.inspectorSize.y > 0 && layout.statsSize.y > 0 && renderSplitterHandle(
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
    using Command = editor::EditorCommandId;
    ImGui::PushFont(editor::nativeFont(), editor::kEditorFontSize);
    if (!ImGui::BeginMainMenuBar()) { ImGui::PopFont(); return; }
    if (ImGui::BeginMenu("File")) {
        renderCommandMenuItem(Command::SaveScene);
        renderCommandMenuItem(Command::LoadScene);
        ImGui::Separator();
        if (ImGui::MenuItem(m_project ? "New Blank Scene" : "Reset Demo Scene")) {
            if (canEditScene()) { commitSceneEdit(); recordSceneHistory("Reset Demo Scene"); }
            resetDemoScene();
        }
        ImGui::Separator();
        renderCommandMenuItem(Command::ProjectSwitch);
        if (ImGui::MenuItem("Exit")) { if (auto* window = stack().window()) window->requestClose(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        for (const auto id : {Command::Undo, Command::Redo, Command::Duplicate, Command::Delete}) renderCommandMenuItem(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("GameObject")) {
        renderCreateEntityMenu();
        ImGui::Separator();
        if (ImGui::MenuItem("Select Next", nullptr, false, m_world.aliveCount() > 1U)) selectNextEntity(1);
        if (ImGui::MenuItem("Select Previous", nullptr, false, m_world.aliveCount() > 1U)) selectNextEntity(-1);
        for (const auto id : {Command::FocusSelected, Command::FrameScene, Command::ResetTransform, Command::DropToGround,
            Command::ZeroVelocity, Command::RandomizeTint, Command::ToggleVisible, Command::ToggleStatic, Command::ToggleGravity, Command::ToggleCollider}) renderCommandMenuItem(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Zero All Velocities", nullptr, false, canEditScene())) zeroAllVelocities();
        if (ImGui::MenuItem("Freeze All Rigidbodies", nullptr, false, canEditScene())) setAllDynamicsStatic(true);
        if (ImGui::MenuItem("Unfreeze All Rigidbodies", nullptr, false, canEditScene())) setAllDynamicsStatic(false);
        if (ImGui::BeginMenu("Camera Bookmarks")) {
            for (std::size_t index=0; index<m_cameraBookmarks.size(); ++index) {
                if (index == 0) { renderCommandMenuItem(Command::BookmarkStore1); renderCommandMenuItem(Command::BookmarkRecall1); }
                else {
                    const auto store = "Store " + std::to_string(index+1U), recall = "Recall " + std::to_string(index+1U);
                    if (ImGui::MenuItem(store.c_str())) storeCameraBookmark(index);
                    if (ImGui::MenuItem(recall.c_str(), nullptr, false, m_cameraBookmarks[index].has_value())) recallCameraBookmark(index);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Scene")) {
        for (const auto id : {Command::Play, Command::Pause, Command::Stop, Command::StepTick, Command::PlayWindow, Command::StopWindow}) renderCommandMenuItem(id);
        ImGui::Separator();
        renderCommandMenuItem(Command::StartNetworkTest);
        renderCommandMenuItem(Command::StopNetworkTest);
        if (m_editorMode == EditorMode::Play) {
            if (ImGui::MenuItem("Launch Striker")) launchStriker();
            if (ImGui::MenuItem("Spawn Drop Wave")) spawnDropWave();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (const auto id : {Command::ToggleHoverOutline, Command::ToggleColliderDebug, Command::ToggleLocalSpace,
            Command::ToggleSnap, Command::ToggleLayoutLock, Command::ResetLayout, Command::SaveLayout}) renderCommandMenuItem(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        if (ImGui::MenuItem("Ribbon", nullptr, &m_showToolbarPanel)) markEditorLayoutDirty();
        for (const auto id : {Command::ShowScriptWorkspace, Command::ShowAssistant, Command::ShowViewport, Command::ShowHierarchy,
            Command::ShowContent, Command::ShowInspector, Command::ShowStats, Command::ShowStatus}) renderCommandMenuItem(id);
        if (ImGui::MenuItem("Show All Panels")) {
            m_showToolbarPanel=m_showHierarchyPanel=m_showProjectPanel=m_showInspectorPanel=m_showStatsPanel=m_showViewportPanel=m_showStatusPanel=true;
            markEditorLayoutDirty();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Script")) {
        renderCommandMenuItem(Command::ShowScriptWorkspace);
        renderCommandMenuItem(Command::ShowAssistant);
        for (const auto id : {Command::ScriptNew, Command::ScriptOpen, Command::ScriptSave, Command::ScriptSaveReload, Command::ScriptSaveAll,
            Command::ScriptCheck, Command::ScriptFind, Command::ScriptGoToLine, Command::ScriptComment, Command::ScriptFormat,
            Command::ScriptReloadDisk, Command::ScriptOutput, Command::ScriptCommand, Command::ScriptState, Command::ScriptErrors}) renderCommandMenuItem(id);
        ImGui::Separator();
        renderCommandMenuItem(Command::ScriptDemo);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Controls")) m_showControlsPopup=true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
    ImGui::PopFont();
    renderProjectSwitchDialog();
}

void GameplayState::renderProjectSwitchDialog() {
    if (m_projectSwitchRequested) {
        ImGui::OpenPopup("Switch Project");
        m_projectSwitchRequested = false;
    }
    if (!ImGui::BeginPopupModal("Switch Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) { return; }
    ImGui::TextWrapped("Save your scene and script documents before returning to Projects?");
    if (m_editorMode == EditorMode::Play) { ImGui::TextWrapped("Play will stop before the editor scene is saved."); }
    if (ImGui::Button("Save All & Switch")) {
        if (m_editorMode == EditorMode::Play) { stopPlayMode(); }
        if (m_scriptWorkspace.saveAll() && saveSceneToDisk()) {
            ImGui::CloseCurrentPopup();
            stack().requestProjectSelector();
        } else {
            m_showScriptWorkspace = true;
            m_editorStatusMessage = "Project remains open: saving failed. Check Scene status and Script Output.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard & Switch")) {
        ImGui::CloseCurrentPopup();
        stack().requestProjectSelector();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void GameplayState::renderToolbar(renderer::Renderer* renderer) {
    if (!m_showToolbarPanel) return;
    const EditorLayout layout = computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
    placeEditorPanel(layout, m_toolbarDockSlot, m_resetEditorLayout);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    if (ImGui::Begin("Toolbar", &m_showToolbarPanel,
        editorPanelFlags(m_editorLayoutLocked || !isFloatingSlot(m_toolbarDockSlot),
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | dockedPanelChromeFlags(m_toolbarDockSlot)))) {
        editor::EditorIcons::Resolver resolveIcon;
        if (renderer) resolveIcon = [renderer](const std::string_view id) {
            const auto image = renderer->resolveEditorIcon(id);
            return editor::EditorIcons::Image{image.textureId, image.width, image.height};
        };
        const editor::NetworkTestRibbonActions networkActions{
            [this] { return stack().networkTestClients(); },
            [this](unsigned clientIndex, net::NetworkTestConditions conditions, std::string& error) {
                return stack().setNetworkTestConditions(clientIndex, conditions, error);
            },
            [this](unsigned clientIndex) { return stack().networkTestHistory(clientIndex); },
            [this] { return stack().hasNetworkTestHistory(); },
            [this](const std::filesystem::path& path, std::string& error) {
                return stack().exportNetworkTestHistory(path, error);
            }
        };
        const auto& online = stack().onlinePlayCallbacks();
        editor::MultiplayerRibbonActions multiplayer;
        if (online.start) {
            using core::SceneNetworkRole;
            const bool canLaunch = m_editorMode == EditorMode::Edit && m_project && !m_project->manifestPath.empty();
            multiplayer = {
                [&online, canLaunch] {
                    const auto& server = online.status(SceneNetworkRole::Server);
                    const auto& client = online.status(SceneNetworkRole::Client);
                    editor::MultiplayerRibbonStatus status;
                    status.canLaunch = canLaunch;
                    status.serverRunning = server.role == SceneNetworkRole::Server;
                    status.serverReady = server.ready;
                    status.serverSteam = server.endpoint.steamId != 0;
                    status.clientRunning = client.role == SceneNetworkRole::Client;
                    status.invitePending = server.invitePending;
                    status.friends = server.friends;
                    status.serverStatus = server.error;
                    status.clientStatus = client.error;
                    status.inviteStatus = server.inviteStatus;
                    if (status.serverRunning) {
                        if (status.serverStatus.empty()) status.serverStatus = server.ready ? "Listening" : "Starting server...";
                        if (server.ready) status.serverEndpoint = server.endpoint.steamId
                            ? std::to_string(server.endpoint.steamId) : net::formatEndpoint(server.endpoint);
                    }
                    if (status.clientRunning && status.clientStatus.empty()) {
                        status.clientStatus = client.ready ? "Game window open" : "Opening game window...";
                    }
                    return status;
                },
                [&online](bool steam, std::string_view address, unsigned capacity, std::string& error) {
                    return online.start(SceneNetworkRole::Server, steam, address, capacity, error);
                },
                [&online] { online.stop(SceneNetworkRole::Server); },
                [&online](bool steam, std::string_view address, std::string& error) {
                    return online.start(SceneNetworkRole::Client, steam, address, 0, error);
                },
                [&online] { online.stop(SceneNetworkRole::Client); },
                online.invite,
                [this](std::string& error) {
                    if (saveSceneToDisk()) return true;
                    error = m_editorStatusMessage;
                    return false;
                },
                [&online, renderer](std::uint64_t friendId) -> editor::EditorIcons::Image {
                    if (!renderer || !online.steamAvatar) return {};
                    const auto image = renderer->resolveSteamFriendAvatar(online.steamAvatar(friendId));
                    return {image.textureId, image.width, image.height};
                }
            };
        }
        const auto* project = stack().project();
        if (m_editorRibbon.render(m_editorCommands, m_ribbonState, resolveIcon, networkActions, multiplayer,
                project ? project->networkPlayerLimit : 0)) markEditorLayoutDirty();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void GameplayState::renderHierarchyPanel(renderer::Renderer* renderer) {
    const auto resolveIcon = iconResolver(renderer);
    if (!m_showHierarchyPanel) {
        return;
    }

    FlatPanelStyle panelStyle;
    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
    placeEditorPanel(layout, m_hierarchyDockSlot, m_resetEditorLayout);
    if (ImGui::Begin(
            "Scene Hierarchy",
            &m_showHierarchyPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_hierarchyDockSlot),
                dockedPanelChromeFlags(m_hierarchyDockSlot)))) {
        renderPanelHeader(EditorPanelId::Hierarchy, "Explorer");
        PanelBody body;
        ImGui::PushFont(editor::nativeFont(), editor::kExplorerFontSize);
        struct RestoreFont { ~RestoreFont() { ImGui::PopFont(); } } restoreFont;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 1));
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##HierarchyFilter", "Search", &m_hierarchyFilter);
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 39, 204, 255));
        if (ImGui::BeginChild("HierarchyList", ImVec2(0.0F, 0.0F), false)) {
            const std::vector<ecs::EntityId> entities = sortedEntities();

            bool anyVisible = false;
            for (const ecs::EntityId entity : entities) {
                if (!entityMatchesFilter(entity)) {
                    continue;
                }

                anyVisible = true;
                std::string label = "##Entity" + std::to_string(entity);
                const ImVec2 row = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable(label.c_str(), m_selectedEntity == entity, ImGuiSelectableFlags_None, ImVec2(0, editor::kPanelRowHeight))) {
                    inspectEntity(entity);
                }
                const auto icon = m_world.hasComponent<ecs::Camera>(entity) ? editor::EditorCommandId::CreateCamera :
                    m_world.hasComponent<scripting::ScriptComponent>(entity) ? editor::EditorCommandId::ScriptNew : editor::EditorCommandId::CreateRenderCube;
                editor::EditorIcons::draw(icon, row.x + 4, row.y + 2, 16, resolveIcon);
                ImGui::GetWindowDrawList()->AddText(ImVec2(row.x + 24, row.y + 1), ImGui::GetColorU32(ImGuiCol_Text), entityDisplayName(entity).c_str());
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
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

void GameplayState::renderProjectPanel(renderer::Renderer* renderer) {
    const auto resolveIcon = iconResolver(renderer);
    if (!m_showProjectPanel) {
        return;
    }

    if (m_projectAssetsDirty) {
        refreshProjectAssets();
    }

    FlatPanelStyle panelStyle;
    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
    placeEditorPanel(layout, m_projectDockSlot, m_resetEditorLayout);
    if (ImGui::Begin(
            "Content Browser",
            &m_showProjectPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_projectDockSlot),
                dockedPanelChromeFlags(m_projectDockSlot)))) {
        renderPanelHeader(EditorPanelId::Project, "Asset Manager");
        PanelBody body;
        ImGui::PushFont(editor::nativeFont(), editor::kEditorFontSize);
        struct RestoreFont { ~RestoreFont() { ImGui::PopFont(); } } restoreFont;

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

        const bool showAssetPath = ImGui::GetContentRegionAvail().x >= 560.0F;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0F, 4.5F));
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(32.0F / 255.0F, 34.0F / 255.0F, 39.0F / 255.0F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(48.0F / 255.0F, 51.0F / 255.0F, 59.0F / 255.0F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0F, 55.0F / 255.0F, 214.0F / 255.0F, 1.0F));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(39.0F / 255.0F, 41.0F / 255.0F, 48.0F / 255.0F, 1.0F));
        const auto assetTableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("ProjectAssetTiles", showAssetPath ? 3 : 2, assetTableFlags, ImVec2(0.0F, -74.0F))) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4F);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0F);
            if (showAssetPath) ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 2.0F);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers, 24.0F);
            for (int column = 0; column < (showAssetPath ? 3 : 2); ++column) {
                ImGui::TableSetColumnIndex(column);
                ImGui::TableHeader(column == 0 ? "Name" : column == 1 ? "Type" : "Path");
            }
            for (const AssetEntry& asset : m_projectAssets) {
                if (!assetMatchesFilter(asset.path, asset.name, asset.kind, loweredFilter)) {
                    continue;
                }
                ++visibleAssetCount;

                ImGui::PushID(asset.path.c_str());
                const bool selected = m_selectedAssetPath == asset.path;
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 24.0F);
                ImGui::TableSetColumnIndex(0);
                const ImVec2 nameMin = ImGui::GetCursorScreenPos();
                const float nameWidth = ImGui::GetContentRegionAvail().x;
                ImGui::Selectable("##AssetTile", selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0F, 15.0F));

                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    m_selectedAssetPath = asset.path;
                    m_editorStatusMessage = "Selected asset " + asset.path;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%s", asset.path.c_str(), asset.details.c_str());
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_selectedAssetPath = asset.path;
                    if (asset.kind == "Script") {
                        m_showScriptWorkspace = true;
                        m_scriptWorkspace.openScript(asset.path);
                    } else if (asset.kind == "Prefab" && canEditScene()) {
                        recordSceneHistory("Spawn Prefab");
                        m_selectedEntity = m_scripts.spawnPrefab(asset.path);
                        inspectEntity(m_selectedEntity);
                    } else if (asset.kind == "Scene") {
                        if (asset.path == toAssetPath(editorScenePath())) {
                            loadSceneFromDisk();
                        } else {
                            m_editorStatusMessage = "Only editor_scene.json can be loaded for now.";
                        }
                    } else {
                        applyAssetToSelection(asset.path, asset.kind);
                    }
                }

                auto* draw = ImGui::GetWindowDrawList();
                const auto icon = asset.kind == "Script" || asset.kind == "Shader" || asset.kind == "Shader Source" ? editor::EditorCommandId::ScriptNew :
                    asset.kind == "Scene" ? editor::EditorCommandId::ShowViewport : asset.kind == "Mesh" || asset.kind == "Prefab" ? editor::EditorCommandId::CreateRenderCube : editor::EditorCommandId::ShowContent;
                editor::EditorIcons::draw(icon, nameMin.x, nameMin.y - 0.5F, 16.0F, resolveIcon);
                draw->PushClipRect(ImVec2(nameMin.x + 22.0F, nameMin.y),
                    ImVec2(nameMin.x + std::max(22.0F, nameWidth), nameMin.y + 15.0F), true);
                draw->AddText(ImVec2(nameMin.x + 22.0F, nameMin.y), ImGui::GetColorU32(ImGuiCol_Text), asset.name.c_str());
                draw->PopClipRect();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(asset.kind.c_str());
                if (showAssetPath) {
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextDisabled("%s", asset.path.c_str());
                }
                ImGui::PopID();
            }

            if (visibleAssetCount == 0U) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 24.0F);
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("No matching assets.");
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

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
            if (m_selectedAssetPath == toAssetPath(editorScenePath())) {
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

void GameplayState::renderEntityComponentEditors(const ecs::EntityId entity, renderer::Renderer* renderer) {
    auto captureBeforeEdit = [this, entity]() {
        return canEditScene() ? captureEntitySnapshot(entity) : EntitySnapshot{};
    };

    auto drawVec3Editor = [this, &captureBeforeEdit](const char* label, glm::vec3& value, const float speed, const char* format) {
        const EntitySnapshot before = captureBeforeEdit();
        bool changed = false;
        changed = propertyDrag3(label, glm::value_ptr(value), speed, 0, 0, format);
        trackEditedItem(std::string("Edit ") + label, changed, before);
        return changed;
    };

    if (auto* transform = m_world.getComponent<ecs::Transform>(entity); transform && inspectorMatches("Transform Position Rotation Scale")) {
        if (PropertySection properties{"Transform"}; properties) {
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

    if (auto* replica = m_world.getComponent<net::NetworkReplica>(entity); replica && inspectorMatches("Network Replica Key")) {
        if (PropertySection properties{"Network Replica"}; properties) {
            const EntitySnapshot before = captureBeforeEdit();
            const std::uint32_t minimum = 1;
            const std::uint32_t maximum = net::MaxReplicaKey;
            PropertyCell cell("Replica Key");
            const bool changed = ImGui::DragScalar("##Value", ImGuiDataType_U32, &replica->key,
                1.0F, &minimum, &maximum, "%u", ImGuiSliderFlags_AlwaysClamp);
            trackEditedItem("Edit Network Replica Key", changed, before);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Use a unique scene key. The server replicates position and rotation.");
            }
        }
    }

    if (auto* meshRenderer = m_world.getComponent<ecs::MeshRenderer>(entity); meshRenderer && inspectorMatches("Mesh Renderer Visible Tint UV Scale Primitive Mesh Texture Shader Material Color Opacity Tiling Blend Depth Base Inheritance")) {
        if (PropertySection properties{"Mesh Renderer"}; properties) {
            EntitySnapshot before = captureBeforeEdit();
            const bool visibleChanged = propertyCheckbox("Visible", &meshRenderer->visible);
            trackEditedItem("Edit Mesh Visibility", visibleChanged, before);
            if (visibleChanged) {
                wakeEntity(entity);
            }
            before = captureBeforeEdit();
            trackEditedItem(
                "Edit Mesh Tint",
                propertyColor(meshRenderer->materialId.empty() ? "Tint" : "Object Tint", glm::value_ptr(meshRenderer->tint)),
                before);
            if (!meshRenderer->materialId.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("Multiplies the material color for this object only. Edit the material below to change every object using it.");
            before = captureBeforeEdit();
            trackEditedItem(
                "Edit UV Scale",
                propertyDrag2(meshRenderer->materialId.empty() ? "UV Scale" : "Object UV Scale", glm::value_ptr(meshRenderer->uvScale), 0.05F, 0.05F, 16.0F, "%.2f"),
                before);

            static constexpr const char* kPrimitiveLabels[] = {"Triangle"};
            int primitiveIndex = static_cast<int>(meshRenderer->primitiveType);
            before = captureBeforeEdit();
            const bool primitiveChanged = propertyCombo("Primitive", &primitiveIndex, kPrimitiveLabels, IM_ARRAYSIZE(kPrimitiveLabels));
            if (primitiveChanged) {
                meshRenderer->primitiveType = static_cast<ecs::PrimitiveType>(primitiveIndex);
            }
            trackEditedItem("Edit Primitive", primitiveChanged, before);

            before = captureBeforeEdit();
            const bool meshChanged = propertyInput("Mesh", &meshRenderer->meshId);
            trackEditedItem("Edit Mesh Resource", meshChanged, before);
            if (meshRenderer->materialId.empty()) {
                before = captureBeforeEdit();
                const bool textureChanged = propertyInput("Texture", &meshRenderer->textureId);
                trackEditedItem("Edit Texture Resource", textureChanged, before);
                if (textureChanged) meshRenderer->texture.reset();
                before = captureBeforeEdit();
                const bool shaderChanged = propertyInput("Shader", &meshRenderer->shaderId);
                trackEditedItem("Edit Shader Resource", shaderChanged, before);
                if (shaderChanged) meshRenderer->shader.reset();
            }
            if (!renderer) {
                before = captureBeforeEdit();
                const bool materialChanged = propertyInput("Material", &meshRenderer->materialId);
                trackEditedItem("Edit Material Resource", materialChanged, before);
                if (materialChanged) meshRenderer->material.reset();
            }
            if (meshChanged) {
                meshRenderer->mesh.reset();
            }
        }
        if (renderer) {
            const auto materialBefore = captureBeforeEdit();
            const auto previousMaterial = meshRenderer->materialId;
            editor::renderMaterialInspector(*meshRenderer, renderer->materialLibrary());
            if (meshRenderer->materialId != previousMaterial) {
                trackEditedItem("Assign Material", true, materialBefore);
                commitSceneEdit();
            }
        }
    }

    if (auto* camera = m_world.getComponent<ecs::Camera>(entity); camera && inspectorMatches("Camera Active Vertical FOV Near Plane Far Plane")) {
        if (PropertySection properties{"Camera"}; properties) {
            EntitySnapshot before = captureBeforeEdit();
            trackEditedItem("Edit Camera Active", propertyCheckbox("Active", &camera->active), before);
            float fovDegrees = glm::degrees(camera->verticalFovRadians);
            before = captureBeforeEdit();
            const bool fovChanged = propertySlider("Vertical FOV", &fovDegrees, 20.0F, 110.0F, "%.1f deg");
            if (fovChanged) {
                camera->verticalFovRadians = glm::radians(fovDegrees);
            }
            trackEditedItem("Edit Camera FOV", fovChanged, before);
            before = captureBeforeEdit();
            trackEditedItem(
                "Edit Near Plane",
                propertyDrag("Near Plane", &camera->nearPlane, 0.01F, 0.01F, camera->farPlane - 0.1F, "%.2f"),
                before);
            before = captureBeforeEdit();
            trackEditedItem(
                "Edit Far Plane",
                propertyDrag("Far Plane", &camera->farPlane, 1.0F, camera->nearPlane + 0.1F, 1000.0F, "%.1f"),
                before);
        }
    }

    if (auto* light = m_world.getComponent<ecs::Light>(entity); light && inspectorMatches("Light Directional Point Enabled Color Intensity Range")) {
        if (PropertySection properties{"Light"}; properties) {
            EntitySnapshot before = captureBeforeEdit();
            trackEditedItem("Edit Light Enabled", propertyCheckbox("Enabled", &light->enabled), before);
            constexpr const char* kinds[] = {"Directional", "Point"};
            int kind = static_cast<int>(light->kind);
            before = captureBeforeEdit();
            const bool kindChanged = propertyCombo("Type", &kind, kinds, 2);
            if (kindChanged) light->kind = static_cast<ecs::LightKind>(kind);
            trackEditedItem("Edit Light Type", kindChanged, before);
            before = captureBeforeEdit();
            {
                PropertyCell cell("Linear Color");
                const bool changed = ImGui::ColorEdit3("##Value", glm::value_ptr(light->color));
                trackEditedItem("Edit Light Color", changed, before);
            }
            before = captureBeforeEdit();
            const bool intensityChanged = propertyDrag("Intensity", &light->intensity, 0.01F, 0.0F, ecs::Light::MaxIntensity, "%.2f");
            if (intensityChanged) light->intensity = std::clamp(light->intensity, 0.0F, ecs::Light::MaxIntensity);
            trackEditedItem("Edit Light Intensity", intensityChanged, before);
            if (light->kind == ecs::LightKind::Point) {
                before = captureBeforeEdit();
                const bool rangeChanged = propertyDrag("Range", &light->range, 0.1F, ecs::Light::MinRange, ecs::Light::MaxRange, "%.2f");
                if (rangeChanged) light->range = std::clamp(light->range, ecs::Light::MinRange, ecs::Light::MaxRange);
                trackEditedItem("Edit Light Range", rangeChanged, before);
            }
        }
    }

    if (auto* rigidbody = m_world.getComponent<ecs::Rigidbody>(entity); rigidbody && inspectorMatches("Rigidbody Use Gravity Static Kinematic Mass Restitution Friction Damping Velocity Angular")) {
        if (PropertySection properties{"Rigidbody"}; properties) {
            bool massPropertiesDirty = false;
            bool changed = false;
            EntitySnapshot before = captureBeforeEdit();
            bool fieldChanged = propertyCheckbox("Use Gravity", &rigidbody->useGravity);
            trackEditedItem("Edit Gravity", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = propertyCheckbox("Static", &rigidbody->isStatic);
            trackEditedItem("Edit Static Flag", fieldChanged, before);
            changed |= fieldChanged;
            massPropertiesDirty |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = propertyCheckbox("Kinematic", &rigidbody->isKinematic);
            trackEditedItem("Edit Kinematic Flag", fieldChanged, before);
            changed |= fieldChanged;
            massPropertiesDirty |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = propertyDrag("Mass", &rigidbody->mass, 0.05F, 0.0F, 100.0F, "%.2f");
            trackEditedItem("Edit Mass", fieldChanged, before);
            changed |= fieldChanged;
            massPropertiesDirty |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = propertyDrag("Restitution", &rigidbody->restitution, 0.01F, 0.0F, 1.0F, "%.2f");
            trackEditedItem("Edit Restitution", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = propertyDrag("Friction", &rigidbody->friction, 0.01F, 0.0F, 2.0F, "%.2f");
            trackEditedItem("Edit Friction", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = propertyDrag("Linear Damping", &rigidbody->linearDamping, 0.01F, 0.0F, 3.0F, "%.2f");
            trackEditedItem("Edit Linear Damping", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = propertyDrag("Angular Damping", &rigidbody->angularDamping, 0.01F, 0.0F, 3.0F, "%.2f");
            trackEditedItem("Edit Angular Damping", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged =
                propertyDrag3("Velocity", glm::value_ptr(rigidbody->velocity), 0.05F, -50.0F, 50.0F, "%.2f");
            trackEditedItem("Edit Velocity", fieldChanged, before);
            changed |= fieldChanged;

            before = captureBeforeEdit();
            fieldChanged = propertyDrag3(
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

    if (auto* collider = m_world.getComponent<ecs::Collider>(entity); collider && inspectorMatches("Collider Enabled Offset Shape Half Extents Radius")) {
        if (PropertySection properties{"Collider"}; properties) {
            EntitySnapshot before = captureBeforeEdit();
            trackEditedItem("Edit Collider Enabled", propertyCheckbox("Enabled", &collider->enabled), before);
            drawVec3Editor("Offset", collider->offset, 0.02F, "%.2f");

            static constexpr const char* kColliderLabels[] = {"AABB", "Sphere"};
            int colliderIndex = static_cast<int>(collider->type);
            before = captureBeforeEdit();
            const bool shapeChanged = propertyCombo("Shape", &colliderIndex, kColliderLabels, IM_ARRAYSIZE(kColliderLabels));
            if (shapeChanged) {
                collider->type = static_cast<ecs::ColliderType>(colliderIndex);
            }
            trackEditedItem("Edit Collider Shape", shapeChanged, before);

            if (collider->type == ecs::ColliderType::Aabb) {
                drawVec3Editor("Half Extents", collider->aabb.halfExtents, 0.02F, "%.2f");
                collider->aabb.halfExtents = glm::max(collider->aabb.halfExtents, glm::vec3(0.05F));
            } else {
                before = captureBeforeEdit();
                trackEditedItem(
                    "Edit Sphere Radius",
                    propertyDrag("Radius", &collider->sphere.radius, 0.02F, 0.05F, 50.0F, "%.2f"),
                    before);
            }
        }
    }

    if (auto* hierarchy = m_world.getComponent<ecs::Hierarchy>(entity); hierarchy && inspectorMatches("Hierarchy Parent")) {
        if (PropertySection properties{"Hierarchy", false}; properties) {
            const ecs::EntityId currentParent = hierarchy->parent;
            const std::string preview =
                currentParent == ecs::kInvalidEntity ? std::string("None") : entityDisplayName(currentParent);
            PropertyCell parentCell("Parent");
            if (ImGui::BeginCombo("##Value", preview.c_str())) {
                const bool noParentSelected = hierarchy->parent == ecs::kInvalidEntity;
                if (ImGui::Selectable("None", noParentSelected)) {
                    recordSceneHistory("Change Parent");
                    hierarchy->parent = ecs::kInvalidEntity;
                    m_editorStatusMessage = "Changed parent";
                }

                m_world.forEachEntity([&](const ecs::EntityId candidate) {
                    if (!ecs::isHierarchyParentValid(m_world, entity, candidate)) {
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

void GameplayState::inspectEntity(const ecs::EntityId entity) {
    if (!m_world.isAlive(entity)) return;
    if (m_selectedEntity != entity) commitSceneEdit();
    m_selectedEntity = entity;
    if (m_world.hasComponent<scripting::ScriptComponent>(entity)) m_inspectorFilter.clear();
    if (!m_showInspectorPanel) {
        m_showInspectorPanel = true;
        markEditorLayoutDirty();
    }
}

bool GameplayState::saveEntityPrefab(const ecs::EntityId entity, const std::string& assetPath) {
    if (m_editorMode != EditorMode::Edit || !canEditScene() || !m_world.isAlive(entity)) {
        m_editorStatusMessage = "Select an object and stop Play before saving a prefab.";
        return false;
    }
    try {
        const auto relative = std::filesystem::u8path(assetPath).lexically_normal();
        if (relative.empty() || relative.has_root_path() || relative.extension() != ".json" ||
            *relative.begin() != "prefabs") throw std::runtime_error("Use a relative prefabs/name.json path.");
        for (const auto& part : relative) if (part == "..") throw std::runtime_error("Prefab path must stay inside project assets.");
        const auto root = std::filesystem::weakly_canonical(editorAssetsRoot());
        const auto target = std::filesystem::weakly_canonical(root / relative);
        const auto inside = target.lexically_relative(root);
        if (inside.empty() || inside.is_absolute() || *inside.begin() == "..") throw std::runtime_error("Prefab path escapes project assets.");
        if (std::filesystem::exists(target)) throw std::runtime_error("Prefab already exists. Choose a new file name.");
        commitSceneEdit();
        SceneSnapshot single;
        single.entities.push_back(captureEntitySnapshot(entity));
        auto components = nlohmann::json::parse(serializeScene(single)).at("entities").front();
        components.erase("sourceId"); components.erase("hierarchy"); components.erase("tag");
        components.erase("networkReplica");
        const nlohmann::json prefab = {{"name", entityDisplayName(entity)}, {"components", std::move(components)}};
        std::filesystem::create_directories(target.parent_path());
        std::string error;
        if (!core::writeFileAtomically(target, prefab.dump(4) + "\n", error)) throw std::runtime_error(error);
        m_projectAssetsDirty = true;
        m_editorStatusMessage = "Saved prefab " + relative.generic_string() + "; spawn it from Content.";
        return true;
    } catch (const std::exception& error) {
        m_editorStatusMessage = "Prefab save failed: " + std::string(error.what());
        return false;
    }
}

void GameplayState::renderScriptInspector(const ecs::EntityId entity) {
    auto* component = m_world.getComponent<scripting::ScriptComponent>(entity);
    if (!component) return;
    const auto schema = m_scripts.fields(component->path);
    std::string searchable = "Luau Behavior Script Fields Enabled Module " + component->path;
    for (const auto& field : schema) searchable += " " + field.name + " " + field.label + " " + field.category;
    if (!inspectorMatches(searchable)) return;
    PropertySection properties("Luau Behavior");
    if (!properties) return;
    const bool editable = canEditScene();
    ImGui::BeginDisabled(!editable);
    auto before = editable ? captureEntitySnapshot(entity) : EntitySnapshot{};
    bool changed = propertyCheckbox("Enabled", &component->enabled);
    trackEditedItem("Enable Luau Behavior", changed, before);
    before = editable ? captureEntitySnapshot(entity) : EntitySnapshot{};
    changed = propertyInput("Module", &component->path);
    trackEditedItem("Edit Luau Module", changed, before);
    ImGui::EndDisabled();
    {
        PropertyCell cell("Choose Module");
        ImGui::BeginDisabled(!editable);
        if (ImGui::BeginCombo("##ModuleAsset", "Project scripts")) {
            if (m_projectAssetsDirty) refreshProjectAssets();
            for (const auto& asset : m_projectAssets) if (asset.kind == "Script" && ImGui::Selectable(asset.path.c_str(), component->path == asset.path)) {
                recordSceneHistory("Assign Luau Module");
                component->path = asset.path;
                m_scripts.populateDefaults(*component);
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    {
        PropertyCell cell("Actions");
        if (ImGui::SmallButton("Open Script") && m_scriptWorkspace.openScript(component->path)) { m_showScriptWorkspace = true; }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_scripts.reloadPending() || component->path.empty());
        if (ImGui::SmallButton("Reload")) { m_scripts.requestReload(component->path); }
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled(!editable);
    {
        PropertyCell cell("Defaults");
        if (ImGui::SmallButton("Fill Missing")) {
            auto updated = *component;
            m_scripts.populateDefaults(updated);
            if (updated.fields != component->fields) {
                recordSceneHistory("Load Script Defaults");
                component->fields = std::move(updated.fields);
            }
        }
    }
    std::string category;
    for (const auto& field : schema) {
        if (field.category != category && !field.category.empty()) {
            PropertyCell cell(field.category.c_str(), 8.0F); ImGui::TextDisabled("Parameters");
        }
        category = field.category;
        ImGui::PushID(static_cast<int>(ImHashData(field.name.data(), field.name.size())));
        before = editable ? captureEntitySnapshot(entity) : EntitySnapshot{};
        const auto stored = component->fields.find(field.name);
        auto value = stored != component->fields.end() ? *stored : field.defaultValue;
        const bool overridden = value != field.defaultValue;
        const std::string label = (field.label.empty() ? field.name : field.label) + (overridden ? " *" : "");
        {
        PropertyCell cell(label.c_str(), 24.0F, field.name.c_str());
        const float resetWidth = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(std::max(20.0F, ImGui::GetContentRegionAvail().x - resetWidth - ImGui::GetStyle().ItemSpacing.x));
        bool edited = false;
        if (field.type == "boolean") {
            bool boolean = value.is_boolean() ? value.get<bool>() : field.defaultValue.get<bool>();
            edited = ImGui::Checkbox("##Value", &boolean);
            if (edited) { value = boolean; }
        } else if (field.type == "number") {
            double number = value.is_number() ? value.get<double>() : field.defaultValue.get<double>();
            const double minimum = field.minimum, maximum = field.maximum;
            edited = ImGui::DragScalar("##Value", ImGuiDataType_Double, &number, static_cast<float>(field.step), &minimum, &maximum, field.integer ? "%.0f" : "%.3f", ImGuiSliderFlags_AlwaysClamp);
            if (edited && std::isfinite(number)) { value = field.integer ? std::round(number) : number; }
            else { edited = false; }
        } else if (field.type == "string") {
            auto string = value.is_string() ? value.get<std::string>() : std::string{};
            edited = ImGui::InputText("##Value", &string);
            if (edited) { value = string; }
        } else { ImGui::TextWrapped("%s: %s", field.name.c_str(), value.dump().c_str()); }
        if (edited) edited = m_scripts.setField(*component, field.name, value);
        trackEditedItem("Edit Script Field " + field.name, edited, before);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s (%s)", field.name.c_str(), field.integer ? "integer" : field.type.c_str());
            ImGui::Text("Default: %s", field.defaultValue.dump().c_str());
            if (!field.description.empty()) { ImGui::PushTextWrapPos(360.0F); ImGui::TextUnformatted(field.description.c_str()); ImGui::PopTextWrapPos(); }
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!overridden);
        if (ImGui::Button("R", ImVec2(resetWidth, 0))) {
            recordSceneHistory("Reset Script Field " + field.name);
            m_scripts.setField(*component, field.name, field.defaultValue);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Reset to the module default");
        ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    {
        PropertyCell cell("Persistence");
        ImGui::BeginDisabled(m_editorMode != EditorMode::Edit);
        if (ImGui::SmallButton("Save Scene")) saveSceneToDisk();
        ImGui::EndDisabled();
        ImGui::TextWrapped(m_editorMode == EditorMode::Edit ? "* overrides module default. Save for next Play." : "Play edits are temporary; Stop restores authored values.");
    }
    {
        PropertyCell cell("New Prefab");
        ImGui::BeginDisabled(m_editorMode != EditorMode::Edit);
        ImGui::InputText("##PrefabPath", &m_scriptPrefabPath);
        if (ImGui::SmallButton("Save New Prefab")) saveEntityPrefab(entity, m_scriptPrefabPath);
        ImGui::EndDisabled();
    }
    if (schema.empty()) { PropertyCell cell("Fields"); ImGui::TextWrapped("Choose a behavior with a fields table. Module errors appear in Script Output."); }
}

bool GameplayState::inspectorMatches(const std::string_view properties) const {
    return m_inspectorFilter.empty() || toLowerCopy(std::string(properties)).find(toLowerCopy(m_inspectorFilter)) != std::string::npos;
}

void GameplayState::renderInspectorPanel(renderer::Renderer* renderer) {
    ENGINE_PROFILE_ZONE("Inspector UI");
    if (!m_showInspectorPanel) {
        return;
    }

    FlatPanelStyle panelStyle;
    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
    placeEditorPanel(layout, m_inspectorDockSlot, m_resetEditorLayout);
    if (ImGui::Begin(
            "Inspector",
            &m_showInspectorPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_inspectorDockSlot),
                dockedPanelChromeFlags(m_inspectorDockSlot)))) {
        renderPanelHeader(EditorPanelId::Inspector, "Properties");
        PanelBody body(ImVec2(2, 0));
        ImGui::PushFont(editor::nativeFont(), editor::kEditorFontSize);
        struct RestoreFont { ~RestoreFont() { ImGui::PopFont(); } } restoreFont;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 1));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(25, 25, 33, 255));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##PropertyFilter", "Filter Properties", &m_inspectorFilter)) commitSceneEdit();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
        if (m_selectedEntity == ecs::kInvalidEntity || !m_world.isAlive(m_selectedEntity)) {
            ImGui::TextDisabled("No object selected.");
        } else {

        if (inspectorMatches("Data Name")) {
        if (PropertySection data{"Data"}; data) {
        if (auto* tag = m_world.getComponent<ecs::Tag>(m_selectedEntity)) {
            const EntitySnapshot before = canEditScene() ? captureEntitySnapshot(m_selectedEntity) : EntitySnapshot{};
            const bool changed = propertyInput("Name", &tag->value);
            trackEditedItem("Rename Entity", changed, before);
        } else if (ImGui::Button("Add Tag")) {
            recordSceneHistory("Add Tag");
            m_world.addComponent<ecs::Tag>(m_selectedEntity, ecs::Tag{entityDisplayName(m_selectedEntity)});
            m_editorStatusMessage = "Added Tag";
        }

        }
        }
        if (m_editorMode == EditorMode::Play) {
            ImGui::Spacing();
            ImGui::TextColored(
                ImVec4(0.97F, 0.78F, 0.29F, 1.0F),
                "Play mode changes are temporary and will be discarded on Stop.");
        }

        ImGui::Spacing();
        renderScriptInspector(m_selectedEntity);
        renderEntityComponentEditors(m_selectedEntity, renderer);
        }
    }
    ImGui::End();
}

void GameplayState::renderStatsPanel(const renderer::Renderer& renderer) {
    if (!m_showStatsPanel) {
        return;
    }

    FlatPanelStyle panelStyle;
    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
    placeEditorPanel(layout, m_statsDockSlot, m_resetEditorLayout);
    if (ImGui::Begin(
            "Statistics",
            &m_showStatsPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_statsDockSlot),
                dockedPanelChromeFlags(m_statsDockSlot)))) {
        renderPanelHeader(EditorPanelId::Stats, "Statistics");
        PanelBody body;
        ImGui::PushFont(editor::nativeFont(), editor::kEditorFontSize);
        struct RestoreFont { ~RestoreFont() { ImGui::PopFont(); } } restoreFont;

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
        const auto metric = [](const char* label, const std::string& value) {
            PropertyCell cell(label, 0); ImGui::TextUnformatted(value.c_str());
        };
        if (PropertySection metrics("Performance"); metrics) {
            metric("FPS", std::to_string(static_cast<int>(fps)));
            metric("Frame", std::to_string(frameTimeMs).substr(0, 5) + " ms");
            metric("Scene render CPU", std::to_string(m_lastSceneRenderMs).substr(0, 5) + " ms");
        }
        if (PropertySection metrics("Materials / frame"); metrics) {
            const auto& materials = renderer.materialStats();
            metric("Draws", std::to_string(materials.draws));
            metric("PSO cache hits", std::to_string(materials.pipelineHits));
            metric("PSO created", std::to_string(materials.pipelineCreates));
            metric("Errors", std::to_string(materials.errors));
        }
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
        if (PropertySection metrics("Runtime"); metrics) {
            metric("Entities", std::to_string(m_world.aliveCount()));
            metric("Renderable", std::to_string(renderableCount));
            metric("Cameras", std::to_string(cameraCount));
            metric("Dynamic bodies", std::to_string(dynamicBodyCount));
            metric("Physics bodies", std::to_string(m_physicsSystem.bodyCount()));
            metric("Sleeping bodies", std::to_string(m_physicsSystem.sleepingBodyCount()));
            metric("Contacts", std::to_string(m_physicsSystem.activeCollisionCount()));
            metric("Undo / Redo", std::to_string(m_undoStack.size()) + " / " + std::to_string(m_redoStack.size()));
            metric("Viewport", std::to_string(int(m_viewportSize.x)) + " x " + std::to_string(int(m_viewportSize.y)));
            metric("Backbuffer", std::to_string(extent.width) + " x " + std::to_string(extent.height));
            metric("Queued spawns", std::to_string(m_pendingSpawnJobs.size()));
        }
        ImGui::TextWrapped("%s", m_lastCollisionMessage.c_str());
    }
    ImGui::End();
}


void GameplayState::renderViewportGizmo(const CameraFrame& cameraFrame) {
    if (m_selectionToolActive) return;
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

    float snapValues[3]{m_ribbonState.moveSnap, m_ribbonState.moveSnap, m_ribbonState.moveSnap};
    if (m_gizmoOperation == GizmoOperation::Rotate) {
        snapValues[0] = m_ribbonState.rotateSnap;
    } else if (m_gizmoOperation == GizmoOperation::Scale) {
        snapValues[0] = m_ribbonState.scaleSnap;
        snapValues[1] = m_ribbonState.scaleSnap;
        snapValues[2] = m_ribbonState.scaleSnap;
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

void GameplayState::renderViewportPanel(renderer::Renderer& renderer, const MaterialGraphWorkspace* workspace) {
    m_viewportHovered = m_viewportFocused = m_viewportUiCaptured = false;
    m_viewportUsesContentRect = workspace != nullptr;
    if (!m_showViewportPanel) {
        m_viewportHovered = false;
        m_viewportFocused = false;
        return;
    }

    if (workspace) {
        ImGui::SetNextWindowPos(ImVec2(workspace->viewportPos.x, workspace->viewportPos.y));
        ImGui::SetNextWindowSize(ImVec2(workspace->viewportSize.x, workspace->viewportSize.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    } else {
        const EditorLayout layout =
            computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
        placeEditorPanel(layout, m_viewportDockSlot, m_resetEditorLayout);
    }
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
        const float tabHeight = 26.0F;
        drawList->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + tabHeight), IM_COL32(25, 26, 31, 255));
        drawList->AddRectFilled(ImVec2(windowPos.x, windowPos.y + tabHeight),
            ImVec2(windowPos.x + windowSize.x, windowPos.y + kViewportHeaderHeight), IM_COL32(18, 18, 21, 255));
        drawList->AddRectFilled(ImVec2(windowPos.x + 1, windowPos.y + 1),
            ImVec2(windowPos.x + 120, windowPos.y + tabHeight), IM_COL32(39, 41, 48, 255));
        const std::string sceneName = m_project ? m_project->name : "Scene";
        drawList->AddText(ImVec2(windowPos.x + 12, windowPos.y + 6), IM_COL32(197, 200, 207, 255), sceneName.c_str());
        drawList->AddLine(ImVec2(windowPos.x, windowPos.y + tabHeight),
            ImVec2(windowPos.x + windowSize.x, windowPos.y + tabHeight), IM_COL32(48, 51, 59, 255));
        const ImVec2 phone(windowPos.x + 17, windowPos.y + tabHeight + 14);
        drawList->AddRect(phone, ImVec2(phone.x + 7, phone.y + 12), IM_COL32(143, 147, 156, 255), 1);
        drawList->AddLine(ImVec2(phone.x + 2, phone.y + 10), ImVec2(phone.x + 5, phone.y + 10), IM_COL32(143, 147, 156, 255));
        ImGui::SetCursorScreenPos(ImVec2(windowPos.x + 36, windowPos.y + tabHeight + 8));
        ImGui::PushFont(editor::semiboldFont(), 18.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
        if (ImGui::Button("   Current device   ", ImVec2(172, 28))) ImGui::OpenPopup("ViewportDevice");
        const ImVec2 deviceMin = ImGui::GetItemRectMin();
        drawList->AddRect(ImVec2(deviceMin.x + 12, deviceMin.y + 8), ImVec2(deviceMin.x + 24, deviceMin.y + 17), IM_COL32(197, 200, 207, 255), 1.0F);
        drawList->AddLine(ImVec2(deviceMin.x + 18, deviceMin.y + 17), ImVec2(deviceMin.x + 18, deviceMin.y + 20), IM_COL32(197, 200, 207, 255));
        drawList->AddLine(ImVec2(deviceMin.x + 151, deviceMin.y + 12), ImVec2(deviceMin.x + 156, deviceMin.y + 17), IM_COL32(197, 200, 207, 255));
        drawList->AddLine(ImVec2(deviceMin.x + 156, deviceMin.y + 17), ImVec2(deviceMin.x + 161, deviceMin.y + 12), IM_COL32(197, 200, 207, 255));
        ImGui::PopStyleVar(2);
        if (ImGui::BeginPopup("ViewportDevice")) {
            ImGui::TextUnformatted("Desktop viewport");
            ImGui::Text("%.0f x %.0f", m_viewportSize.x, m_viewportSize.y);
            ImGui::TextDisabled("RMB + WASDQE: camera | Q/W/E/R: tools | F: focus");
            ImGui::EndPopup();
        }
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_editorMode == EditorMode::Play ? "Playing" : "");
        drawList->AddLine(ImVec2(windowPos.x, windowPos.y + kViewportHeaderHeight - 1),
            ImVec2(windowPos.x + windowSize.x, windowPos.y + kViewportHeaderHeight - 1), IM_COL32(48, 51, 59, 255));

        m_viewportPosition = glm::vec2(windowPos.x, windowPos.y + kViewportHeaderHeight);
        m_viewportSize = glm::vec2(windowSize.x, std::max(1.0F, windowSize.y - kViewportHeaderHeight));
        m_viewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
            ImGui::IsMouseHoveringRect({m_viewportPosition.x, m_viewportPosition.y},
                {m_viewportPosition.x + m_viewportSize.x, m_viewportPosition.y + m_viewportSize.y});
        m_viewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        ImDrawListSplitter viewportLayers;
        viewportLayers.Split(drawList, 2);
        viewportLayers.SetCurrentChannel(drawList, 1);
        if (m_editorMode == EditorMode::Play) {
            m_viewportUiCaptured = scripting::ReactUI::drawInViewport(m_scripts,
                {m_viewportPosition.x, m_viewportPosition.y, m_viewportSize.x, m_viewportSize.y},
                [&renderer](const std::string_view id) {
                    const auto image = renderer.resolveUiImage(id);
                    return scripting::ReactUI::Image{image.textureId, image.width, image.height};
                }, [&renderer](const void* key, const ImDrawList& list, float x, float y, float width, float height) {
                    const auto image = renderer.resolveUiCanvas(key, list, x, y, width, height);
                    return scripting::ReactUI::Image{image.textureId, image.width, image.height};
                });
        }
        viewportLayers.SetCurrentChannel(drawList, 0);

        const float renderFraction = m_simulationRunning ? stack().renderInterpolationFraction(*this) : 1.0F;
        const CameraFrame cameraFrame = buildActiveCameraFrame(renderer, renderFraction);
        updateHoveredEntity(cameraFrame);
        if (!m_viewportUiCaptured) renderViewportGizmo(cameraFrame);
        handleViewportPicking(cameraFrame);
        viewportLayers.Merge(drawList);
    }
    ImGui::End();
    ImGui::PopStyleVar(workspace ? 2 : 1);
}

void GameplayState::renderStatusBar() {
    if (!m_showStatusPanel) {
        return;
    }

    const EditorLayout layout =
        computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
    placeEditorPanel(layout, m_statusDockSlot, m_resetEditorLayout);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0F, 7.0F));
    if (ImGui::Begin(
            "Status Bar",
            &m_showStatusPanel,
            editorPanelFlags(
                m_editorLayoutLocked || !isFloatingSlot(m_statusDockSlot),
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                    dockedPanelChromeFlags(m_statusDockSlot)))) {
        ImGui::PushFont(editor::codeFont(), editor::kCodeFontSize);
        const float statusWidth = ImGui::GetContentRegionAvail().x;
        const ImVec2 commandOrigin = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(commandOrigin,
            ImVec2(commandOrigin.x + statusWidth, commandOrigin.y + 20), IM_COL32(33, 34, 39, 255));
        if (ImGui::Selectable("Execute a command...", false, 0, ImVec2(std::max(100.0F, statusWidth), 20.0F))) {
            m_editorCommands.execute(editor::EditorCommandId::ScriptCommand);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_editorStatusMessage.c_str());
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::SetCursorPosY(39);
        ImGui::TextDisabled("%s   History %zu / %zu", m_editorMode == EditorMode::Play ? "PLAY" : "EDIT", m_undoStack.size(), m_redoStack.size());
        if (const auto error = stack().networkPlayError(); !error.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.40F, 1.0F), "Network test failed");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.*s", static_cast<int>(error.size()), error.data());
        } else if (stack().networkPlayRunning()) {
            ImGui::SameLine();
            ImGui::TextDisabled("Network test active");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void GameplayState::restoreMaterialGraphLayout(resources::MaterialLibrary& library) {
    if (m_project && !m_materialGraphDraftsInitialized && !labActive() && !stack().standalone()) {
        editor::loadMaterialGraphDrafts(library, editorLayoutPath().parent_path() / "material_graph_drafts.json");
        m_materialGraphDraftsInitialized = true;
    }
    if (!std::exchange(m_restoreMaterialGraphPending, false)) return;
    if (!m_materialGraphDocked || !m_materialGraphSession) return;
    if (!editor::restoreMaterialGraphSession(library, *m_materialGraphSession)) {
        m_materialGraphSession.reset();
        m_materialGraphSelectionSource = ecs::kInvalidEntity;
        markEditorLayoutDirty();
        return;
    }

    const auto selected = m_fileEntities.find(m_materialGraphSelectionSource);
    if (selected != m_fileEntities.end() && m_world.isAlive(selected->second)) m_selectedEntity = selected->second;
    if (!materialGraphViewportVisible()) return;
    const auto* mesh = m_world.getComponent<ecs::MeshRenderer>(m_selectedEntity);
    m_materialPreview = editor::materialGraphPreview(library, m_selectedEntity, mesh && mesh->visible ? mesh : nullptr);
    m_materialPreviewEntity = m_materialPreview ? m_selectedEntity : ecs::kInvalidEntity;
}

ecs::EntityId GameplayState::materialGraphSelectedSource() const {
    if (!m_materialGraphSession || !m_world.isAlive(m_selectedEntity)) return ecs::kInvalidEntity;
    const auto remembered = m_fileEntities.find(m_materialGraphSelectionSource);
    if (remembered != m_fileEntities.end() && remembered->second == m_selectedEntity) return remembered->first;
    const auto selected = std::find_if(m_fileEntities.begin(), m_fileEntities.end(),
        [this](const auto& entry) { return entry.second == m_selectedEntity; });
    return selected == m_fileEntities.end() ? ecs::kInvalidEntity : selected->first;
}

bool GameplayState::materialGraphViewportVisible() const {
    return m_showViewportPanel && !m_showScriptWorkspace && !stack().standalone() && !labActive();
}

std::optional<GameplayState::MaterialGraphWorkspace> GameplayState::materialGraphWorkspace() const {
    if (!materialGraphViewportVisible()) return std::nullopt;
    const auto layout = computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit,
        editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop),
        hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop),
        hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top),
        hasDockedPanel(EditorDockSlot::Bottom));
    const glm::vec2 position(std::floor(layout.viewportPos.x), std::floor(layout.viewportPos.y));
    const glm::vec2 end(std::floor(layout.viewportPos.x + layout.viewportSize.x),
        std::floor(layout.viewportPos.y + layout.viewportSize.y));
    const glm::vec2 size = end - position;
    const bool sideBySide = size.x >= 760.0F;
    if (size.x < 240.0F || size.y < (sideBySide ? 140.0F : 244.0F)) return std::nullopt;

    const float span = (sideBySide ? size.x : size.y) - kMaterialGraphSplitterSize;
    const float minViewport = sideBySide ? 320.0F : std::clamp(span * 0.42F, kViewportHeaderHeight + 56.0F, 170.0F);
    const float minGraph = std::min(sideBySide ? 410.0F : 230.0F, span - minViewport);
    const float graphSpan = std::floor(std::clamp(span * m_materialGraphSplit, minGraph, span - minViewport));
    const float viewportSpan = span - graphSpan;
    if (sideBySide) {
        return MaterialGraphWorkspace{position, {viewportSpan, size.y},
            {position.x + viewportSpan + kMaterialGraphSplitterSize, position.y}, {graphSpan, size.y}, true};
    }
    return MaterialGraphWorkspace{position, {size.x, viewportSpan},
        {position.x, position.y + viewportSpan + kMaterialGraphSplitterSize}, {size.x, graphSpan}, false};
}

void GameplayState::renderMaterialGraphSplitter(const MaterialGraphWorkspace& workspace) {
    if (m_editorLayoutLocked || m_dockDragActive) return;
    const bool sideBySide = workspace.sideBySide;
    const ImVec2 position = sideBySide
        ? ImVec2(workspace.graphPos.x - kMaterialGraphSplitterSize, workspace.graphPos.y)
        : ImVec2(workspace.graphPos.x, workspace.graphPos.y - kMaterialGraphSplitterSize);
    const ImVec2 size = sideBySide
        ? ImVec2(kMaterialGraphSplitterSize, workspace.graphSize.y)
        : ImVec2(workspace.graphSize.x, kMaterialGraphSplitterSize);
    if (!renderSplitterHandle("##MaterialGraphWorkspaceSplitter", position, size, sideBySide, true)) return;

    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    const float movement = sideBySide ? delta.x : delta.y;
    if (std::abs(movement) < 0.001F) return;
    const float graphSpan = sideBySide ? workspace.graphSize.x : workspace.graphSize.y;
    const float viewportSpan = sideBySide ? workspace.viewportSize.x : workspace.viewportSize.y;
    m_materialGraphSplit = std::clamp((graphSpan - movement) / (graphSpan + viewportSpan), 0.20F, 0.80F);
    markEditorLayoutDirty();
}

void GameplayState::renderEditorUi(renderer::Renderer& renderer) {
    auto& materialLibrary = renderer.materialLibrary();
    restoreMaterialGraphLayout(materialLibrary);
    if (m_gizmoWasUsing && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) finishGizmoEdit();
    if (m_showScriptWorkspace && !m_scriptWorkspaceWasVisible) {
        m_ribbonState.tab = editor::EditorRibbonTab::Script;
        markEditorLayoutDirty();
    }
    m_scriptWorkspaceWasVisible = m_showScriptWorkspace;
    processEditorShortcuts();
    updateFrameHistory();
    renderMainMenuBar();
    renderToolbar(&renderer);
    const bool graphWasOpen = editor::materialGraphEditorOpen(materialLibrary);
    const bool graphDockPreference = m_materialGraphDocked;
    const auto graphWorkspace = graphWasOpen ? materialGraphWorkspace() : std::nullopt;
    const bool graphDocked = graphDockPreference && graphWorkspace.has_value();
    if (m_showScriptWorkspace) {
        m_viewportHovered = m_viewportFocused = m_viewportUiCaptured = false;
        const auto layout = computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
        ImGui::SetNextWindowPos(layout.viewportPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(layout.viewportSize, ImGuiCond_Always);
        m_scriptWorkspace.render(&m_showScriptWorkspace, m_selectedEntity, m_editorMode == EditorMode::Play, m_simulationRunning);
    } else { renderViewportPanel(renderer, graphDocked ? &*graphWorkspace : nullptr); }
    renderHierarchyPanel(&renderer);
    renderProjectPanel(&renderer);
    renderInspectorPanel(&renderer);
    const auto* selectedMesh = materialGraphViewportVisible()
        ? m_world.getComponent<ecs::MeshRenderer>(m_selectedEntity) : nullptr;
    const bool graphOpeningDocked = graphDockPreference && !graphWasOpen &&
        editor::materialGraphEditorOpen(materialLibrary) && materialGraphWorkspace().has_value();
    const editor::MaterialGraphDock graphDock{
        graphWorkspace ? graphWorkspace->graphPos : glm::vec2(0.0F),
        graphWorkspace ? graphWorkspace->graphSize : glm::vec2(0.0F), m_materialGraphDocked};
    m_materialPreview = graphOpeningDocked ? nullptr
        : editor::renderMaterialGraphEditor(materialLibrary, m_selectedEntity,
            selectedMesh && selectedMesh->visible ? selectedMesh : nullptr, &graphDock);
    if (m_materialGraphDocked != graphDockPreference) markEditorLayoutDirty();
    auto graphSession = m_project && m_materialGraphDocked ? editor::materialGraphSession(materialLibrary) : std::nullopt;
    if (graphSession != m_materialGraphSession) {
        m_materialGraphSession = std::move(graphSession);
        markEditorLayoutDirty();
    }
    const auto graphSelection = materialGraphSelectedSource();
    if (graphSelection != m_materialGraphSelectionSource) {
        m_materialGraphSelectionSource = graphSelection;
        markEditorLayoutDirty();
    }
    if (m_materialGraphDraftsInitialized) editor::flushMaterialGraphDrafts();
    m_materialPreviewEntity = m_materialPreview ? m_selectedEntity : ecs::kInvalidEntity;
    if (m_addComponentRequested) {
        ImGui::OpenPopup("Add Entity Component");
        m_addComponentRequested = false;
    }
    if (ImGui::BeginPopup("Add Entity Component")) {
        if (m_world.isAlive(m_componentMenuEntity)) renderAddComponentMenu(m_componentMenuEntity);
        else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    renderStatsPanel(renderer);
    renderStatusBar();
    renderDockResizeHandles();
    if (graphDocked && m_materialGraphDocked && editor::materialGraphEditorOpen(materialLibrary)) {
        renderMaterialGraphSplitter(*graphWorkspace);
    }
    renderDockDropOverlay();
    if (m_showAssistant) {
        const auto layout = computeEditorLayout(m_editorLeftWidth, m_editorRightWidth, m_editorLeftSplit, m_editorRightSplit, editor::EditorRibbon::preferredHeight(m_ribbonState), hasDockedPanel(EditorDockSlot::LeftTop), hasDockedPanel(EditorDockSlot::LeftBottom), hasDockedPanel(EditorDockSlot::RightTop), hasDockedPanel(EditorDockSlot::RightBottom), m_showAssistant, hasDockedPanel(EditorDockSlot::Top), hasDockedPanel(EditorDockSlot::Bottom));
        ImGui::SetNextWindowPos(layout.assistantPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(layout.assistantSize, ImGuiCond_Always);
        m_codexAssistant.render(&m_showAssistant);
    }
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

    editor::focusRestoredMaterialGraph();
    if (m_editorLayoutDirty && !m_dockDragActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        saveEditorLayout();
        m_editorLayoutDirty = false;
    }

    m_resetEditorLayout = false;
}

void GameplayState::renderStandaloneUi(renderer::Renderer& renderer) {
    m_viewportPosition = glm::vec2(0.0F);
    m_viewportSize = glm::vec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    m_viewportHovered = m_viewportFocused = true;
    m_viewportUiCaptured = false;
    scripting::ReactUI::draw(m_scripts, true, [this, &renderer](const std::string_view id) {
        const auto image = m_sceneNetwork && id.starts_with("steamavatar://")
            ? renderer.resolveSteamFriendAvatar(m_sceneNetwork->steamAvatar(id)) : renderer.resolveUiImage(id);
        return scripting::ReactUI::Image{image.textureId, image.width, image.height};
    }, [&renderer](const void* key, const ImDrawList& list, float x, float y, float width, float height) {
        const auto image = renderer.resolveUiCanvas(key, list, x, y, width, height);
        return scripting::ReactUI::Image{image.textureId, image.width, image.height};
    });
    const auto notice = m_sceneNetwork ? m_sceneNetwork->connectionNotice() : std::string_view{};
    if (notice.empty()) return;
    ImGui::SetNextWindowPos(ImVec2(m_viewportSize.x - 16.0F, 16.0F), ImGuiCond_Always, ImVec2(1.0F, 0.0F));
    ImGui::SetNextWindowBgAlpha(0.92F);
    if (ImGui::Begin("Network connection##Standalone", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextUnformatted(notice.data(), notice.data() + notice.size());
    }
    ImGui::End();
}

void GameplayState::renderUi(renderer::Renderer& rendererInstance) {
    if (!rendererInstance.isImGuiEnabled()) {
        return;
    }

    if (stack().standalone()) {
        renderStandaloneUi(rendererInstance);
    } else if (labActive()) {
        renderLabUi(rendererInstance);
    } else {
        renderEditorUi(rendererInstance);
    }
}

}
