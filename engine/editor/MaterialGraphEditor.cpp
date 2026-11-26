#include "engine/editor/MaterialGraphEditor.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <glm/common.hpp>
#include <nlohmann/json.hpp>

#include "engine/resources/material_graph.h"

namespace engine::editor {
namespace {
using Json = nlohmann::json;
using PinInfo = resources::MaterialGraphPinInfo;
using NodeInfo = resources::MaterialGraphNodeInfo;

constexpr float nodeWidth = 176.0F;
constexpr float titleHeight = 32.0F;
constexpr float rowHeight = 23.0F;
constexpr float maxPosition = 1'000'000.0F;

const Json& field(const Json& value, const char* key) {
    static const Json missing;
    const auto item = value.find(key);
    return item != value.end() ? *item : missing;
}

std::uint32_t nodeId(const Json& value) {
    if (!value.is_number_integer()) return 0;
    const auto id = value.get<std::uint64_t>();
    return id <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(id) : 0;
}

glm::vec2 graphPosition(const Json& value, const glm::vec2 fallback) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) return fallback;
    const glm::vec2 position{value[0].get<float>(), value[1].get<float>()};
    return std::abs(position.x) <= maxPosition && std::abs(position.y) <= maxPosition ? position : fallback;
}

glm::vec2 defaultPosition(std::size_t index) {
    return {40.0F + static_cast<float>(index % 3) * 215.0F, 40.0F + static_cast<float>(index / 3) * 130.0F};
}

ImVec2 screen(const glm::vec2 value) { return {value.x, value.y}; }
glm::vec2 point(const ImVec2 value) { return {value.x, value.y}; }

struct CanvasNode final {
    std::uint32_t id{};
    const NodeInfo* type{};
    glm::vec2 position{0.0F};

    [[nodiscard]] std::span<const PinInfo> pins() const { return type ? type->inputs : resources::materialGraphOutputs(); }
    [[nodiscard]] float height() const { return titleHeight + rowHeight * static_cast<float>(std::max<std::size_t>(1, pins().size())) + 8.0F; }
    [[nodiscard]] glm::vec2 input(std::size_t index) const {
        return position + glm::vec2{0.0F, titleHeight + rowHeight * (static_cast<float>(index) + 0.5F)};
    }
    [[nodiscard]] glm::vec2 output() const { return input(0) + glm::vec2{nodeWidth, 0.0F}; }
};

struct CanvasPin final {
    const CanvasNode* node{};
    const PinInfo* input{};
    glm::vec2 position{0.0F};
};

std::vector<CanvasNode> canvasNodes(const Json& graph) {
    std::vector<CanvasNode> result;
    const auto& nodes = field(graph, "nodes");
    if (nodes.is_array()) {
        result.reserve(std::min(nodes.size(), resources::maxMaterialGraphNodes) + 1);
        for (const auto& node : nodes) {
            if (result.size() >= resources::maxMaterialGraphNodes) break;
            const auto id = nodeId(field(node, "id"));
            if (const auto* type = materialGraphNodeType(node); id && type) {
                result.push_back({id, type, graphPosition(field(node, "position"), defaultPosition(result.size()))});
            }
        }
    }
    result.push_back({0, nullptr, graphPosition(field(graph, "outputPosition"), {710.0F, 55.0F})});
    return result;
}

void fitNodes(MaterialGraphView& view, const std::vector<CanvasNode>& nodes, const glm::vec2 size) {
    glm::vec2 minimum{std::numeric_limits<float>::max()};
    glm::vec2 maximum{std::numeric_limits<float>::lowest()};
    for (const auto& node : nodes) {
        minimum = glm::min(minimum, node.position);
        maximum = glm::max(maximum, node.position + glm::vec2{nodeWidth, node.height()});
    }
    const auto extent = maximum - minimum;
    view.zoom = std::clamp(std::min((size.x - 64.0F) / extent.x, (size.y - 64.0F) / extent.y), minMaterialGraphZoom, 1.0F);
    view.pan = (size - extent * view.zoom) * 0.5F - minimum * view.zoom;
    view.fit = false;
}

CanvasPin hoveredPin(const std::vector<CanvasNode>& nodes, const glm::vec2 mouse, float radius) {
    const auto hit = [&](const glm::vec2 center) {
        const auto delta = center - mouse;
        return delta.x * delta.x + delta.y * delta.y <= radius * radius;
    };
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if (it->id && hit(it->output())) return {&*it, nullptr, it->output()};
        const auto pins = it->pins();
        for (std::size_t index = 0; index < pins.size(); ++index) {
            if (hit(it->input(index))) return {&*it, &pins[index], it->input(index)};
        }
        const auto delta = mouse - it->position;
        if (delta.x >= 0 && delta.y >= 0 && delta.x <= nodeWidth && delta.y <= it->height()) break;
    }
    return {};
}

const CanvasNode* findCanvasNode(const std::vector<CanvasNode>& nodes, std::uint32_t id) {
    const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const CanvasNode& node) { return node.id == id; });
    return it != nodes.end() ? &*it : nullptr;
}

bool hasUpstreamNode(Json& graph, std::uint32_t from, std::uint32_t target) {
    std::vector<std::uint32_t> pending{from};
    std::vector<std::uint32_t> visited;
    while (!pending.empty() && visited.size() < resources::maxMaterialGraphNodes) {
        const auto id = pending.back();
        pending.pop_back();
        if (id == target) return true;
        if (std::find(visited.begin(), visited.end(), id) != visited.end()) continue;
        visited.push_back(id);
        if (const auto* node = findMaterialGraphNode(graph, id)) {
            const auto& inputs = field(*node, "inputs");
            if (inputs.is_object()) {
                for (const auto& input : inputs) {
                    if (const auto dependency = nodeId(input)) pending.push_back(dependency);
                }
            }
        }
    }
    return false;
}

bool setLink(Json& graph, const CanvasPin& pin, std::uint32_t source, std::string& error,
             const std::function<void(MaterialGraphChange)>& beforeEdit) {
    if (!pin.input || !pin.node) return false;
    if (source && pin.node->id && hasUpstreamNode(graph, source, pin.node->id)) {
        error = "This link would create a cycle.";
        return false;
    }
    auto* destination = pin.node->id ? findMaterialGraphNode(graph, pin.node->id) : &graph;
    if (!destination) return false;
    const char* key = pin.node->id ? "inputs" : "outputs";
    const auto& previous = field(*destination, key);
    if (source ? nodeId(field(previous, pin.input->key)) == source : !previous.contains(pin.input->key)) {
        return false;
    }
    beforeEdit(MaterialGraphChange::Appearance);
    auto& inputs = (*destination)[key];
    if (source) {
        if (!inputs.is_object()) inputs = Json::object();
        inputs[pin.input->key] = source;
    } else {
        inputs.erase(pin.input->key);
    }
    return true;
}

void drawWire(ImDrawList& drawList, const glm::vec2 from, const glm::vec2 to, ImU32 color) {
    const glm::vec2 handle{std::clamp(std::abs(to.x - from.x) * 0.55F, 28.0F, 180.0F), 0.0F};
    drawList.AddBezierCubic(screen(from), screen(from + handle), screen(to - handle), screen(to), color, 2.0F);
}

void drawCanvas(ImDrawList& drawList, const std::vector<CanvasNode>& nodes, Json& graph, const MaterialGraphView& view,
                const CanvasPin& hovered, const glm::vec2 origin, const glm::vec2 size, const glm::vec2 mouse) {
    constexpr ImU32 wireColor = IM_COL32(114, 174, 221, 255);
    const auto transform = [&](glm::vec2 value) { return origin + view.pan + value * view.zoom; };
    drawList.AddRectFilled(screen(origin), screen(origin + size), IM_COL32(24, 26, 31, 255));
    const float spacing = 28.0F * view.zoom;
    const auto gridStart = [&](float pan) { return std::fmod(std::fmod(pan, spacing) + spacing, spacing); };
    const ImU32 gridColor = IM_COL32(36, 40, 47, 255);
    for (float x = gridStart(view.pan.x); x < size.x; x += spacing) {
        drawList.AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + size.y}, gridColor);
    }
    for (float y = gridStart(view.pan.y); y < size.y; y += spacing) {
        drawList.AddLine({origin.x, origin.y + y}, {origin.x + size.x, origin.y + y}, gridColor);
    }
    for (const auto& node : nodes) {
        const auto* source = node.id ? findMaterialGraphNode(graph, node.id) : &graph;
        if (!source) continue;
        const auto& inputs = field(*source, node.id ? "inputs" : "outputs");
        const auto pins = node.pins();
        for (std::size_t index = 0; index < pins.size(); ++index) {
            const auto id = nodeId(field(inputs, pins[index].key));
            if (id) {
                if (const auto* upstream = findCanvasNode(nodes, id)) {
                    drawWire(drawList, transform(upstream->output()), transform(node.input(index)), wireColor);
                }
            }
        }
    }
    if (view.linkSource) {
        if (const auto* upstream = findCanvasNode(nodes, view.linkSource)) {
            drawWire(drawList, transform(upstream->output()), hovered.input ? transform(hovered.position) : mouse, wireColor);
        }
    }
    for (const auto& node : nodes) {
        const auto minimum = transform(node.position);
        const auto maximum = transform(node.position + glm::vec2{nodeWidth, node.height()});
        if (maximum.x < origin.x || maximum.y < origin.y || minimum.x > origin.x + size.x || minimum.y > origin.y + size.y) continue;
        const bool selected = node.id && node.id == view.selected;
        const float fontSize = std::max(9.0F, ImGui::GetFontSize() * view.zoom);
        const float rounding = 6.0F * view.zoom;
        drawList.AddRectFilled(screen(minimum), screen(maximum), IM_COL32(43, 47, 55, 255), rounding);
        drawList.AddRectFilled(screen(minimum), {maximum.x, minimum.y + titleHeight * view.zoom},
                               node.id ? IM_COL32(53, 66, 84, 255) : IM_COL32(76, 58, 86, 255),
                               rounding, ImDrawFlags_RoundCornersTop);
        drawList.AddRect(screen(minimum), screen(maximum), selected ? wireColor : IM_COL32(76, 82, 94, 255), rounding);
        const ImVec4 textClip{minimum.x, minimum.y, maximum.x - 5.0F * view.zoom, maximum.y};
        const auto label = [&](glm::vec2 position, const char* name, ImU32 color) {
            drawList.AddText(ImGui::GetFont(), fontSize, screen(position), color, name, nullptr, 0.0F, &textClip);
        };
        label(minimum + glm::vec2{10.0F, 7.0F} * view.zoom, node.type ? node.type->label : "Material Output", IM_COL32(244, 246, 250, 255));
        const auto pins = node.pins();
        for (std::size_t index = 0; index < pins.size(); ++index) {
            const auto center = transform(node.input(index));
            const bool highlight = hovered.node == &node && hovered.input == &pins[index];
            drawList.AddCircleFilled(screen(center), std::max(3.0F, 5.0F * view.zoom), highlight ? IM_COL32_WHITE : wireColor);
            label(center + glm::vec2{10.0F * view.zoom, -fontSize * 0.5F}, pins[index].label, IM_COL32(210, 219, 231, 255));
        }
        if (node.id) {
            const auto center = transform(node.output());
            const bool highlight = hovered.node == &node && !hovered.input;
            drawList.AddCircleFilled(screen(center), std::max(3.0F, 5.0F * view.zoom), highlight ? IM_COL32_WHITE : wireColor);
        }
    }
}
}

const resources::MaterialGraphNodeInfo* materialGraphNodeType(const Json& node) {
    const auto& value = field(node, "type");
    if (!value.is_string()) return nullptr;
    const auto& type = value.get_ref<const std::string&>();
    const auto types = resources::materialGraphNodeTypes();
    const auto it = std::find_if(types.begin(), types.end(), [&](const NodeInfo& info) { return type == info.type; });
    return it != types.end() ? &*it : nullptr;
}

Json* findMaterialGraphNode(Json& graph, std::uint32_t id) {
    const auto nodes = graph.find("nodes");
    if (!id || nodes == graph.end() || !nodes->is_array()) return nullptr;
    const auto node = std::find_if(nodes->begin(), nodes->end(), [id](const Json& value) { return nodeId(field(value, "id")) == id; });
    return node != nodes->end() ? &*node : nullptr;
}

std::uint64_t nextMaterialGraphNodeId(const Json& graph) {
    const auto& cursor = field(graph, "nextNodeId");
    const auto saved = cursor.is_number_integer() ? cursor.get<std::uint64_t>() : 0;
    const auto exhausted = std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1;
    auto next = saved && saved <= exhausted ? saved : std::uint64_t{1};
    for (const auto& node : field(graph, "nodes")) {
        next = std::max(next, std::uint64_t{nodeId(field(node, "id"))} + 1);
    }
    return next;
}

bool addMaterialGraphNode(Json& graph, MaterialGraphView& view, const NodeInfo& type, std::uint64_t minimumId) {
    if (!field(graph, "nodes").is_array()) return false;
    auto& nodes = graph["nodes"];
    if (nodes.size() >= resources::maxMaterialGraphNodes) return false;
    const auto next = std::max(nextMaterialGraphNodeId(graph), minimumId);
    if (next > std::numeric_limits<std::uint32_t>::max()) return false;
    const auto id = static_cast<std::uint32_t>(next);
    auto position = glm::clamp(view.spawnPosition, glm::vec2{-maxPosition}, glm::vec2{maxPosition});
    const auto canvas = canvasNodes(graph);
    const float height = CanvasNode{id, &type, position}.height();
    for (std::size_t attempt = 0; attempt < canvas.size(); ++attempt) {
        const auto overlap = std::find_if(canvas.begin(), canvas.end(), [&](const CanvasNode& node) {
            return std::abs(position.x - node.position.x) < nodeWidth + 16.0F &&
                position.y + height + 16.0F > node.position.y && position.y < node.position.y + node.height() + 16.0F;
        });
        if (overlap == canvas.end()) break;
        position.y = std::min(maxPosition, overlap->position.y + overlap->height() + 16.0F);
    }
    if (position.y - view.spawnPosition.y > 120.0F) view.fit = true;
    nodes.push_back(Json{{"id", id}, {"type", type.type}, {"position", {position.x, position.y}}});
    graph["nextNodeId"] = next + 1;
    view.selected = id;
    return true;
}

bool removeMaterialGraphNode(Json& graph, MaterialGraphView& view) {
    const auto id = view.selected;
    if (!findMaterialGraphNode(graph, id)) return false;
    graph["nextNodeId"] = nextMaterialGraphNodeId(graph);
    auto& nodes = graph["nodes"];
    const auto node = std::find_if(nodes.begin(), nodes.end(), [id](const Json& value) { return nodeId(field(value, "id")) == id; });
    nodes.erase(node);
    const auto unlink = [id](Json& nodeValue, const char* key) {
        const auto inputs = nodeValue.find(key);
        if (inputs == nodeValue.end() || !inputs->is_object()) return;
        for (auto input = inputs->begin(); input != inputs->end();) {
            input = nodeId(*input) == id ? inputs->erase(input) : std::next(input);
        }
    };
    for (auto& value : nodes) unlink(value, "inputs");
    unlink(graph, "outputs");
    if (view.linkSource == id) view.linkSource = 0;
    view.selected = 0;
    return true;
}

MaterialGraphChange renderMaterialGraphCanvas(Json& graph, MaterialGraphView& view, bool editable,
                                               std::string& linkError, const std::function<void(MaterialGraphChange)>& beforeEdit) {
    const auto origin = point(ImGui::GetCursorScreenPos());
    const auto size = glm::max(point(ImGui::GetContentRegionAvail()), glm::vec2{1.0F});
    ImGui::InvisibleButton("###MaterialGraphCanvas", screen(size), ImGuiButtonFlags_MouseButtonLeft |
                          ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hover = ImGui::IsItemHovered();
    const auto& io = ImGui::GetIO();
    const auto mouse = point(io.MousePos);
    auto nodes = canvasNodes(graph);
    if (view.fit) fitNodes(view, nodes, size);
    if (hover && io.MouseWheel != 0.0F) {
        const auto pivot = (mouse - origin - view.pan) / view.zoom;
        view.zoom = std::clamp(view.zoom * std::pow(1.14F, io.MouseWheel), minMaterialGraphZoom, maxMaterialGraphZoom);
        view.pan = mouse - origin - pivot * view.zoom;
    }
    if ((hover || ImGui::IsItemActive()) && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0F)) {
        view.pan += point(io.MouseDelta);
    }
    view.spawnPosition = (size * 0.5F - view.pan) / view.zoom - glm::vec2{nodeWidth * 0.5F, titleHeight};
    const auto worldMouse = (mouse - origin - view.pan) / view.zoom;
    const auto pin = hover ? hoveredPin(nodes, worldMouse, std::max(8.0F, 7.0F / view.zoom)) : CanvasPin{};
    bool linksChanged = false;
    bool layoutChanged = false;
    if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (pin.node && !pin.input && editable) {
            view.selected = pin.node->id;
            view.linkSource = pin.node->id;
            view.linkPressed = true;
        } else if (pin.input && view.linkSource && editable) {
            linksChanged = setLink(graph, pin, view.linkSource, linkError, beforeEdit);
            view.linkSource = 0;
        } else {
            view.selected = 0;
            view.linkSource = 0;
            for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
                const auto delta = worldMouse - it->position;
                if (delta.x < 0 || delta.y < 0 || delta.x > nodeWidth || delta.y > it->height()) continue;
                view.selected = it->id;
                if (editable && delta.y < titleHeight) {
                    view.dragNode = it->id;
                    view.dragOutput = !it->id;
                }
                break;
            }
        }
    }
    if (editable && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && view.linkPressed) {
        if (pin.input && view.linkSource) {
            linksChanged = setLink(graph, pin, view.linkSource, linkError, beforeEdit) || linksChanged;
            view.linkSource = 0;
        }
        view.linkPressed = false;
    }
    if (editable && pin.input && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        linksChanged = setLink(graph, pin, 0, linkError, beforeEdit) || linksChanged;
        view.linkSource = 0;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        view.dragNode = 0;
        view.dragOutput = false;
    }
    if (editable && view.draggingLayout() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0F)) {
        const auto id = view.dragNode;
        const auto item = std::find_if(nodes.begin(), nodes.end(), [id](const CanvasNode& node) { return node.id == id; });
        if (item != nodes.end()) {
            auto& position = item->position;
            const auto next = glm::clamp(position + point(io.MouseDelta) / view.zoom, glm::vec2{-maxPosition}, glm::vec2{maxPosition});
            if (next != position) {
                position = next;
                if (auto* destination = id ? findMaterialGraphNode(graph, id) : &graph) {
                    beforeEdit(MaterialGraphChange::Layout);
                    (*destination)[id ? "position" : "outputPosition"] = {position.x, position.y};
                    layoutChanged = true;
                }
            }
        }
    }
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(screen(origin), screen(origin + size), true);
    drawCanvas(*drawList, nodes, graph, view, pin, origin, size, mouse);
    drawList->PopClipRect();
    if (pin.node) {
        if (!pin.input) ImGui::SetTooltip("Output");
        else if (editable) ImGui::SetTooltip("%s  |  Right-click to disconnect", pin.input->label);
        else ImGui::SetTooltip("%s", pin.input->label);
    }
    if (linksChanged) return MaterialGraphChange::Appearance;
    return layoutChanged ? MaterialGraphChange::Layout : MaterialGraphChange::None;
}

}
