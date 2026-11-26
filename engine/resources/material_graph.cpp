#include "engine/resources/material_graph.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace engine::resources {
namespace {
using Json = nlohmann::json;
using Kind = MaterialGraphNodeKind;
using NodeId = std::uint32_t;

constexpr MaterialGraphPinInfo texturePins[] = {{"uv", "UV"}};
constexpr MaterialGraphPinInfo mathPins[] = {{"a", "A"}, {"b", "B"}};
constexpr MaterialGraphPinInfo lerpPins[] = {{"a", "A"}, {"b", "B"}, {"alpha", "Alpha"}};
constexpr MaterialGraphPinInfo channelPins[] = {{"value", "Value"}};
constexpr MaterialGraphNodeInfo nodeTypes[] = {
    {Kind::UV, "uv", "Texture Coordinate", {}},
    {Kind::Texture, "texture", "Texture Sample", texturePins},
    {Kind::Scalar, "scalar", "Scalar", {}},
    {Kind::Color, "color", "Color", {}},
    {Kind::Vector2, "vector2", "Vector 2", {}},
    {Kind::Add, "add", "Add", mathPins},
    {Kind::Multiply, "multiply", "Multiply", mathPins},
    {Kind::Lerp, "lerp", "Lerp", lerpPins},
    {Kind::Channel, "channel", "Channel", channelPins},
};
constexpr MaterialGraphPinInfo outputPins[] = {
    {"baseColor", "Base Color"}, {"opacity", "Opacity"}, {"emissive", "Emissive"},
    {"normal", "Normal"}, {"roughness", "Roughness"}, {"metallic", "Metallic"},
};

constexpr std::string_view pixelHeader = R"(
struct PointLight
{
    float4 positionRange;
    float4 colorIntensity;
};

cbuffer PixelConstants
{
    float4 g_TintColor;
    float4 g_LightDirection;
    float4 g_AmbientColor;
    float4 g_CameraPosition;
    float4 g_DirectionalColorIntensity;
    float4 g_PointLightInfo;
    PointLight g_PointLights[24];
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 WorldNormal : TEXCOORD0;
    float3 WorldPosition : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

float3 normalizeOr(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1e-12 ? value * rsqrt(lengthSquared) : fallback;
}

float3 applyNormalMap(PSInput input, float3 normal, float3 encodedNormal)
{
    const float3 dx = ddx(input.WorldPosition);
    const float3 dy = ddy(input.WorldPosition);
    const float2 uvDx = ddx(input.TexCoord);
    const float2 uvDy = ddy(input.TexCoord);
    const float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(determinant) <= 1e-12)
        return normal;

    const float direction = sign(determinant);
    const float3 tangentCandidate = (dx * uvDy.y - dy * uvDx.y) * direction;
    const float3 orthogonal = tangentCandidate - normal * dot(tangentCandidate, normal);
    const float tangentLengthSquared = dot(orthogonal, orthogonal);
    if (tangentLengthSquared <= 1e-20)
        return normal;

    const float3 tangent = orthogonal * rsqrt(tangentLengthSquared);
    const float3 bitangentCandidate = (dy * uvDx.x - dx * uvDy.x) * direction;
    const float3 perpendicular = cross(normal, tangent);
    const float3 bitangent = perpendicular * sign(dot(perpendicular, bitangentCandidate));
    const float3 sampled = encodedNormal * 2.0 - 1.0;
    return normalizeOr(tangent * sampled.x + bitangent * sampled.y + normal * sampled.z, normal);
}

float3 lightSurface(PSInput input, float3 normal, float3 baseColor, float roughness, float metallic)
{
    const float3 light = normalizeOr(-g_LightDirection.xyz, normal);
    const float3 view = normalizeOr(g_CameraPosition.xyz - input.WorldPosition, normal);
    const float3 halfway = normalizeOr(light + view, normal);
    const float nDotL = saturate(dot(normal, light));
    const float nDotV = saturate(dot(normal, view));
    const float nDotH = saturate(dot(normal, halfway));
    const float vDotH = saturate(dot(view, halfway));

    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float distributionBase = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    const float distribution = alphaSquared / max(3.14159265 * distributionBase * distributionBase, 1e-8);
    const float geometryK = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    const float geometry = (nDotL / lerp(nDotL, 1.0, geometryK)) * (nDotV / lerp(nDotV, 1.0, geometryK));
    const float3 reflectance = lerp(float3(0.04, 0.04, 0.04), saturate(baseColor), metallic);
    const float3 fresnel = reflectance + (1.0 - reflectance) * pow(1.0 - vDotH, 5.0);
    const float3 specular = fresnel * distribution * geometry / max(4.0 * nDotV * nDotL, 1e-5);
    const float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * baseColor;
    const float hemi = normal.y * 0.5 + 0.5;
    const float3 ambient = lerp(float3(0.12, 0.13, 0.16), g_AmbientColor.rgb, hemi);
    const float3 sun = g_DirectionalColorIntensity.rgb * g_DirectionalColorIntensity.w;
    float3 pointLight = 0.0;
    for (uint i = 0; i < (uint)g_PointLightInfo.x; ++i)
    {
        const PointLight lamp = g_PointLights[i];
        const float3 offset = lamp.positionRange.xyz - input.WorldPosition;
        const float distanceSquared = dot(offset, offset);
        const float rangeSquared = lamp.positionRange.w * lamp.positionRange.w;
        if (distanceSquared >= rangeSquared) continue;
        const float fade = 1.0 - distanceSquared / rangeSquared;
        const float incidence = saturate(dot(normal, offset * rsqrt(max(distanceSquared, 1e-6))));
        pointLight += lamp.colorIntensity.rgb * lamp.colorIntensity.w * incidence * fade * fade / (1.0 + 0.3 * distanceSquared);
    }
    return (diffuse + specular * 3.14159265) * nDotL * sun + diffuse * pointLight + ambient * baseColor * lerp(1.0, 0.35, metallic);
}
)";

void checkKeys(const Json& object, std::initializer_list<std::string_view> keys) {
    if (!object.is_object()) throw std::runtime_error("expected a graph object");
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        if (std::find(keys.begin(), keys.end(), entry.key()) == keys.end())
            throw std::runtime_error("unknown property '" + entry.key() + "'");
    }
}

float number(const Json& value) {
    if (!value.is_number()) throw std::runtime_error("expected a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result) || std::abs(result) > 1'000'000.0)
        throw std::runtime_error("number must be finite and between -1000000 and 1000000");
    return static_cast<float>(result);
}

void readVector(const Json& value, std::span<float> components) {
    if (!value.is_array() || value.size() != components.size())
        throw std::runtime_error("expected an array of " + std::to_string(components.size()) + " numbers");
    for (std::size_t index = 0; index < components.size(); ++index)
        components[index] = number(value[index]);
}

NodeId readId(const Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() <= 0))
        throw std::runtime_error("node ID must be a positive integer");
    const auto id = value.get<std::uint64_t>();
    if (id == 0 || id > std::numeric_limits<NodeId>::max())
        throw std::runtime_error("node ID is outside the 32-bit unsigned range");
    return static_cast<NodeId>(id);
}

template <typename Pins>
void readLinks(const Json& value, const Pins& pins, std::span<NodeId> links) {
    if (!value.is_object()) throw std::runtime_error("links must be an object");
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
        const auto pin = std::find_if(pins.begin(), pins.end(), [&](const auto& candidate) {
            return entry.key() == candidate.key;
        });
        if (pin == pins.end()) throw std::runtime_error("unknown pin '" + entry.key() + "'");
        try {
            links[static_cast<std::size_t>(pin - pins.begin())] = readId(entry.value());
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(entry.key() + ": " + error.what());
        }
    }
}

struct Node final {
    NodeId id{};
    const MaterialGraphNodeInfo* info{};
    std::array<NodeId, 3> inputs{};
    std::array<float, 4> value{};
    std::string texture;
    std::size_t channel{};
    std::size_t width{};
    bool checking{};
    bool emitted{};
};

Node readNode(const Json& value, NodeId id) {
    checkKeys(value, {"id", "type", "position", "inputs", "value", "texture"});
    const auto type = value.find("type");
    if (type == value.end() || !type->is_string())
        throw std::runtime_error("type must name a node type");
    const auto& typeName = type->get_ref<const std::string&>();
    const auto* info = std::find_if(std::begin(nodeTypes), std::end(nodeTypes), [&](const auto& candidate) {
        return candidate.type == typeName;
    });
    if (info == std::end(nodeTypes)) throw std::runtime_error("unknown node type '" + typeName + "'");

    Node node;
    node.id = id;
    node.info = info;
    if (const auto position = value.find("position"); position != value.end()) {
        std::array<float, 2> coordinates{};
        readVector(*position, coordinates);
    }
    if (const auto inputs = value.find("inputs"); inputs != value.end())
        readLinks(*inputs, info->inputs, node.inputs);
    if (const auto texture = value.find("texture"); texture != value.end()) {
        if (info->kind != Kind::Texture || !texture->is_string())
            throw std::runtime_error("texture must be a path on a Texture Sample node");
        node.texture = texture->get<std::string>();
        if (node.texture.size() > 4096 || node.texture.find('\0') != std::string::npos)
            throw std::runtime_error("invalid texture path");
    }
    const auto literal = value.find("value");
    switch (info->kind) {
        case Kind::Scalar:
            if (literal != value.end()) node.value[0] = number(*literal);
            break;
        case Kind::Color:
            node.value.fill(1.0F);
            if (literal != value.end()) readVector(*literal, node.value);
            break;
        case Kind::Vector2:
            node.value.fill(1.0F);
            if (literal != value.end()) readVector(*literal, std::span{node.value}.first<2>());
            break;
        case Kind::Channel:
            if (literal != value.end()) {
                if (!literal->is_string()) throw std::runtime_error("channel must be r, g, b or a");
                const auto& channel = literal->get_ref<const std::string&>();
                constexpr std::string_view channels = "rgba";
                if (channel.size() != 1 || (node.channel = channels.find(channel[0])) == std::string_view::npos)
                    throw std::runtime_error("channel must be r, g, b or a");
            }
            break;
        default:
            if (literal != value.end()) throw std::runtime_error("value is not supported for this node type");
    }
    return node;
}

std::string hlslNumber(float value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::scientific, std::numeric_limits<float>::max_digits10 - 1);
    if (result.ec != std::errc{}) throw std::runtime_error("failed to encode graph number");
    return {buffer.data(), result.ptr};
}

std::string hlslType(std::size_t width) {
    return width == 1 ? "float" : "float" + std::to_string(width);
}

std::string hlslLiteral(const Node& node) {
    if (node.width == 1) return hlslNumber(node.value[0]);
    std::string expression = hlslType(node.width) + "(" + hlslNumber(node.value[0]);
    for (std::size_t index = 1; index < node.width; ++index)
        expression += ", " + hlslNumber(node.value[index]);
    return expression + ")";
}

std::string nodeName(NodeId id) {
    return "node" + std::to_string(id);
}

class GraphCompiler final {
public:
    explicit GraphCompiler(const Json& value) {
        checkKeys(value, {"nodes", "outputs", "outputPosition", "nextNodeId"});
        if (const auto cursor = value.find("nextNodeId"); cursor != value.end()) {
            const auto id = cursor->is_number_integer() ? cursor->get<std::uint64_t>() : 0;
            const auto exhausted = std::uint64_t{std::numeric_limits<NodeId>::max()} + 1;
            if (id == 0 || id > exhausted) throw std::runtime_error("nextNodeId must be between 1 and 4294967296");
        }
        const auto nodes = value.find("nodes");
        if (nodes == value.end() || !nodes->is_array()) throw std::runtime_error("nodes must be an array");
        if (nodes->size() > maxMaterialGraphNodes) throw std::runtime_error("material graph exceeds 64 nodes");
        m_nodes.reserve(nodes->size());
        for (const auto& item : *nodes) {
            if (!item.is_object() || !item.contains("id")) throw std::runtime_error("each graph node needs an id");
            const auto id = readId(item["id"]);
            try {
                if (!m_indices.emplace(id, m_nodes.size()).second) throw std::runtime_error("duplicate ID");
                m_nodes.push_back(readNode(item, id));
            } catch (const std::runtime_error& error) {
                throw std::runtime_error("node " + std::to_string(id) + ": " + error.what());
            }
        }
        const auto outputs = value.find("outputs");
        if (outputs != value.end()) readLinks(*outputs, std::span{outputPins}, m_outputs);
        if (const auto position = value.find("outputPosition"); position != value.end()) {
            std::array<float, 2> coordinates{};
            readVector(*position, coordinates);
        }
    }

    MaterialGraphProgram compile() {
        for (const auto& node : m_nodes) validate(node.id);
        for (std::size_t index = 0; index < m_outputs.size(); ++index) validateOutput(index);

        for (const auto id : m_outputs) {
            if (id != 0) emit(id);
        }
        const std::string normal = m_outputs[3] == 0 ? "geometricNormal" :
            "applyNormalMap(input, geometricNormal, " + rgb(m_outputs[3], {}) + ")";
        const std::string baseAlpha = m_outputs[0] != 0 && find(m_outputs[0]).width == 4 ?
            nodeName(m_outputs[0]) + ".a" : "1.0";
        std::string source;
        for (std::size_t slot = 0; slot < m_textures.size(); ++slot) {
            const auto name = textureName(slot);
            source += "Texture2D " + name + ";\nSamplerState " + name + "_sampler;\n";
        }
        source += pixelHeader;
        source += "\nfloat4 main(PSInput input) : SV_TARGET\n{\n" + m_body +
            "    const float3 baseColor = " + rgb(m_outputs[0], "float3(1.0, 1.0, 1.0)") + " * g_TintColor.rgb;\n" +
            "    const float opacity = saturate(" + inputName(m_outputs[1], baseAlpha) + " * g_TintColor.a);\n" +
            "    const float3 emissive = " + rgb(m_outputs[2], "float3(0.0, 0.0, 0.0)") + ";\n" +
            "    const float3 geometricNormal = normalizeOr(input.WorldNormal, float3(0.0, 1.0, 0.0));\n" +
            "    const float3 normal = " + normal + ";\n" +
            "    const float roughness = clamp(" + inputName(m_outputs[4], "0.55") + ", 0.04, 1.0);\n" +
            "    const float metallic = saturate(" + inputName(m_outputs[5], "0.0") + ");\n" +
            "    return float4(lightSurface(input, normal, baseColor, roughness, metallic) + emissive, opacity);\n}\n";
        return {std::move(source), std::move(m_textures)};
    }

private:
    Node& find(NodeId id) {
        const auto index = m_indices.find(id);
        if (index == m_indices.end()) throw std::runtime_error("missing source node " + std::to_string(id));
        return m_nodes[index->second];
    }

    std::size_t validate(NodeId id) {
        if (id == 0) return 0;
        auto& node = find(id);
        if (node.width != 0) return node.width;
        if (node.checking) throw std::runtime_error("cycle reaches node " + std::to_string(id));
        node.checking = true;
        try {
            std::array<std::size_t, 3> widths{};
            for (std::size_t index = 0; index < node.info->inputs.size(); ++index)
                widths[index] = validate(node.inputs[index]);
            node.width = nodeWidth(node, widths);
            node.checking = false;
            return node.width;
        } catch (const std::runtime_error& error) {
            throw std::runtime_error("node " + std::to_string(id) + " (" + node.info->label + "): " + error.what());
        }
    }

    static std::size_t nodeWidth(const Node& node, const std::array<std::size_t, 3>& widths) {
        switch (node.info->kind) {
            case Kind::UV:
            case Kind::Vector2:
                return 2;
            case Kind::Texture:
                if (widths[0] != 0 && widths[0] != 2) throw std::runtime_error("UV requires a two-component vector");
                return 4;
            case Kind::Color:
                return 4;
            case Kind::Scalar:
                return 1;
            case Kind::Channel:
                if (widths[0] != 0 && node.channel >= widths[0])
                    throw std::runtime_error("selected channel is outside the input value");
                return 1;
            case Kind::Lerp:
                if (widths[2] > 1) throw std::runtime_error("Alpha requires a scalar");
                [[fallthrough]];
            case Kind::Add:
            case Kind::Multiply:
                if (widths[0] > 1 && widths[1] > 1 && widths[0] != widths[1])
                    throw std::runtime_error("A and B must have the same vector width or be scalars");
                return std::max({std::size_t{1}, widths[0], widths[1]});
        }
        throw std::runtime_error("unknown graph node type");
    }

    void validateOutput(std::size_t index) {
        if (m_outputs[index] == 0) return;
        const std::string pin = outputPins[index].label;
        try {
            const auto width = find(m_outputs[index]).width;
            if (index == 0 || index == 2) {
                if (width != 1 && width != 4) throw std::runtime_error("requires a color or scalar");
            } else if (index == 3) {
                if (width != 4) throw std::runtime_error("requires a color encoding a tangent-space normal");
            } else if (width != 1) {
                throw std::runtime_error("requires a scalar");
            }
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(pin + ": " + error.what());
        }
    }

    static std::string inputName(NodeId id, const std::string& fallback) {
        return id != 0 ? nodeName(id) : fallback;
    }

    static std::string textureName(std::size_t slot) {
        return slot == 0 ? "g_Texture" : "g_Texture" + std::to_string(slot);
    }

    std::string rgb(NodeId id, const std::string& fallback) {
        if (id == 0) return fallback;
        const auto name = nodeName(id);
        return find(id).width == 1 ? "float3(" + name + ", " + name + ", " + name + ")" : name + ".rgb";
    }

    void emit(NodeId id) {
        auto& node = find(id);
        if (node.emitted) return;
        for (const auto input : node.inputs) {
            if (input != 0) emit(input);
        }
        m_body += "    const " + hlslType(node.width) + " " + nodeName(id) + " = " + expression(node) + ";\n";
        node.emitted = true;
    }

    std::string expression(const Node& node) {
        const auto a = node.inputs[0];
        const auto b = node.inputs[1];
        switch (node.info->kind) {
            case Kind::UV:
                return "input.TexCoord";
            case Kind::Texture: {
                if (m_textures.size() == maxMaterialGraphTextures)
                    throw std::runtime_error("material graph exceeds 8 connected Texture Sample nodes");
                const auto name = textureName(m_textures.size());
                m_textures.push_back(node.texture);
                return name + ".Sample(" + name + "_sampler, " + inputName(a, "input.TexCoord") + ")";
            }
            case Kind::Scalar:
            case Kind::Color:
            case Kind::Vector2:
                return hlslLiteral(node);
            case Kind::Add:
                return inputName(a, "0.0") + " + " + inputName(b, "0.0");
            case Kind::Multiply:
                return inputName(a, "1.0") + " * " + inputName(b, "1.0");
            case Kind::Lerp: {
                const auto first = broadcast(a, node.width, "0.0");
                const auto second = broadcast(b, node.width, "1.0");
                return "lerp(" + first + ", " + second + ", " + inputName(node.inputs[2], "0.5") + ")";
            }
            case Kind::Channel:
                if (a == 0) return "0.0";
                if (find(a).width == 1) return nodeName(a);
                return nodeName(a) + "." + "xyzw"[node.channel];
        }
        throw std::runtime_error("unknown graph node type");
    }

    std::string broadcast(NodeId id, std::size_t width, const std::string& fallback) {
        const auto value = inputName(id, fallback);
        if (width == 1 || (id != 0 && find(id).width == width)) return value;
        auto result = hlslType(width) + "(" + value;
        for (std::size_t index = 1; index < width; ++index) result += ", " + value;
        return result + ")";
    }

    std::vector<Node> m_nodes;
    std::unordered_map<NodeId, std::size_t> m_indices;
    std::array<NodeId, std::size(outputPins)> m_outputs{};
    std::vector<std::string> m_textures;
    std::string m_body;
};
}

std::span<const MaterialGraphNodeInfo> materialGraphNodeTypes() { return nodeTypes; }
std::span<const MaterialGraphPinInfo> materialGraphOutputs() { return outputPins; }

MaterialGraphProgram compileMaterialGraph(const nlohmann::json& graph) {
    return GraphCompiler(graph).compile();
}

}
