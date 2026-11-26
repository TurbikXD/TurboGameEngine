#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "engine/resources/material_graph.h"

namespace {
using Json = nlohmann::json;
using engine::resources::compileMaterialGraph;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expectRejected(const Json& graph, std::string_view diagnostic) {
    try {
        (void)compileMaterialGraph(graph);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(diagnostic) != std::string_view::npos) return;
        throw std::runtime_error("Unexpected graph diagnostic: " + std::string(error.what()));
    }
    throw std::runtime_error("Invalid graph was accepted: " + std::string(diagnostic));
}

Json blendGraph() {
    return Json::parse(R"({
        "nodes": [
            {"id": 1, "type": "uv"},
            {"id": 2, "type": "vector2", "value": [2, 3]},
            {"id": 3, "type": "multiply", "inputs": {"a": 1, "b": 2}},
            {"id": 4, "type": "texture", "inputs": {"uv": 3}, "texture": "textures/first.png"},
            {"id": 5, "type": "texture", "texture": "textures/second.png"},
            {"id": 6, "type": "channel", "inputs": {"value": 5}, "value": "a"},
            {"id": 7, "type": "lerp", "inputs": {"a": 4, "b": 5, "alpha": 6}},
            {"id": 8, "type": "texture", "texture": "textures/unused.png"},
            {"id": 9, "type": "add", "inputs": {"a": 6}},
            {"id": 10, "type": "color", "value": [0.5, 0.5, 1.0, 1.0]}
        ],
        "outputs": {"baseColor": 7, "normal": 10, "roughness": 9, "metallic": 6}
    })");
}

void multipleTexturesAndSurfaceOutputs() {
    auto graph = blendGraph();
    const auto program = compileMaterialGraph(graph);
    expect(program.textures.size() == 2 && program.textures[0] == "textures/first.png" &&
           program.textures[1] == "textures/second.png", "Connected samples lost distinct or deterministic slots");
    expect(program.pixelSource.find("g_Texture.Sample(g_Texture_sampler, node3)") != std::string::npos &&
           program.pixelSource.find("g_Texture1.Sample(g_Texture1_sampler, input.TexCoord)") != std::string::npos &&
           program.pixelSource.find("lerp(node4, node5, node6)") != std::string::npos &&
           program.pixelSource.find("node5.w") != std::string::npos &&
           program.pixelSource.find("node8") == std::string::npos,
           "Graph code lost its UV branch, second texture, alpha mask, or reachability");
    expect(program.pixelSource.find("applyNormalMap(input, geometricNormal, node10.rgb)") != std::string::npos &&
           program.pixelSource.find("clamp(node9, 0.04, 1.0)") != std::string::npos &&
           program.pixelSource.find("saturate(node6)") != std::string::npos &&
           program.pixelSource.find("node7.a * g_TintColor.a") != std::string::npos,
           "Graph did not route authored surface values");
    graph["nodes"][3]["position"] = {50, 80};
    graph["outputPosition"] = {500, 200};
    graph["nextNodeId"] = 21;
    std::reverse(graph["nodes"].begin(), graph["nodes"].end());
    expect(compileMaterialGraph(graph) == program, "Authoring layout affected the generated shader");
    graph["nodes"][6].erase("texture");
    const auto whiteSample = compileMaterialGraph(graph);
    expect(whiteSample.textures.size() == 2 && whiteSample.textures[0].empty() &&
           whiteSample.pixelSource == program.pixelSource,
           "Clearing a sample path failed to preserve its binding and shader");
}

void invalidConnectionsAndValues() {
    const auto original = blendGraph();
    auto graph = original;
    graph["nodes"][5]["inputs"]["value"] = 9;
    expectRejected(graph, "cycle");
    graph = original;
    graph["nodes"][7] = {{"id", 8}, {"type", "add"}, {"inputs", {{"b", 8}}}};
    expectRejected(graph, "cycle");
    graph = original;
    graph["nodes"][3]["inputs"]["uv"] = 6;
    expectRejected(graph, "UV requires");
    graph = original;
    graph["nodes"][6]["inputs"]["alpha"] = 5;
    expectRejected(graph, "Alpha requires");
    graph = original;
    graph["nodes"][2]["inputs"]["a"] = 5;
    expectRejected(graph, "same vector width");
    graph = original;
    graph["nodes"][5]["inputs"]["value"] = 2;
    expectRejected(graph, "outside the input");
    graph = original;
    graph["outputs"]["normal"] = 6;
    expectRejected(graph, "tangent-space normal");
    graph = original;
    graph["outputs"]["baseColor"] = 15;
    expectRejected(graph, "missing source node 15");
    graph = original;
    graph["nodes"][1]["id"] = 1;
    expectRejected(graph, "duplicate ID");
    graph = original;
    graph["nodes"][1]["id"] = -1;
    expectRejected(graph, "positive integer");
    graph = original;
    graph["nodes"][1]["value"] = {0.5, 1e15};
    expectRejected(graph, "must be finite");
    graph = original;
    graph["nodes"][4]["inputs"]["colour"] = 10;
    expectRejected(graph, "unknown pin");
    graph = original;
    graph["nextNodeId"] = -1;
    expectRejected(graph, "nextNodeId");
}

void limitsAndDefaults() {
    const Json empty = {{"nodes", Json::array()}, {"outputs", Json::object()}};
    const auto defaults = compileMaterialGraph(empty);
    expect(defaults.textures.empty() && defaults.pixelSource.find("const float opacity = saturate(1.0") != std::string::npos,
           "Disconnected graph had no valid opaque defaults");
    auto graph = empty;
    std::size_t previous{};
    for (std::size_t index = 0; index < engine::resources::maxMaterialGraphTextures + 1; ++index) {
        const auto texture = index * 2 + 1;
        graph["nodes"].push_back({{"id", texture}, {"type", "texture"}});
        if (previous != 0) {
            graph["nodes"].push_back({{"id", texture + 1}, {"type", "add"}, {"inputs", {{"a", previous}, {"b", texture}}}});
            previous = texture + 1;
        } else {
            previous = texture;
        }
    }
    expect(compileMaterialGraph(graph) == defaults, "Unconnected textures consumed shader slots");
    graph["outputs"]["baseColor"] = previous;
    expectRejected(graph, "8 connected Texture Sample");
    graph = empty;
    for (std::size_t index = 0; index < engine::resources::maxMaterialGraphNodes + 1; ++index)
        graph["nodes"].push_back({{"id", index + 1}, {"type", "scalar"}});
    expectRejected(graph, "64 nodes");
}
}

int main() {
    try {
        multipleTexturesAndSurfaceOutputs();
        invalidConnectionsAndValues();
        limitsAndDefaults();
        std::cout << "Material graph compilation and validation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Material graph tests failed: " << error.what() << '\n';
        return 1;
    }
}
