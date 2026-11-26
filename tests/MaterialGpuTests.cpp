#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/Log.h"
#include "engine/ecs/render_system.h"
#include "engine/ecs/world.h"
#include "engine/platform/Window.h"
#include "engine/renderer/RenderAdapter.h"
#include "engine/renderer/Renderer.h"
#include "engine/resources/material_graph.h"
#include "engine/rhi_diligent/DiligentDevice.h"
#include "third_party/renderdoc/renderdoc_app.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Fence.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/SwapChain.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Texture.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/TextureView.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngineD3D12/interface/RenderDeviceD3D12.h"
#include "third_party/DiligentEngine/DiligentCore/Platforms/Basic/interface/BasicPlatformDebug.hpp"
#include "third_party/DiligentEngine/DiligentCore/Primitives/interface/DebugOutput.h"

namespace engine::renderer {
struct MaterialGpuTestAccess final {
    static rhi::diligent::DiligentDevice& device(Renderer& renderer) {
        return static_cast<rhi::diligent::DiligentDevice&>(*renderer.m_device);
    }
};
}

namespace {
using engine::renderer::Renderer;
using engine::renderer::MaterialGpuTestAccess;
using engine::resources::Material;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
std::atomic_size_t validationErrors{0};
std::atomic_bool expectedShaderError{false};
Diligent::DebugMessageCallbackType previousCallback = nullptr;

void DILIGENT_CALL_TYPE diagnostic(Diligent::DEBUG_MESSAGE_SEVERITY severity, const Diligent::Char* message,
                                   const Diligent::Char* function, const Diligent::Char* file, const int line) {
    if (severity >= Diligent::DEBUG_MESSAGE_SEVERITY_ERROR && !expectedShaderError.load()) ++validationErrors;
    if (previousCallback != nullptr) previousCallback(severity, message, function, file, line);
}

class Diagnostics final {
public:
    Diagnostics() : previousBreak(Diligent::BasicPlatformDebug::GetBreakOnError()) {
        previousCallback = Diligent::DebugMessageCallback;
        Diligent::BasicPlatformDebug::SetBreakOnError(false);
        Diligent::SetDebugMessageCallback(diagnostic);
    }
    ~Diagnostics() {
        Diligent::SetDebugMessageCallback(previousCallback);
        Diligent::BasicPlatformDebug::SetBreakOnError(previousBreak);
    }
private:
    bool previousBreak;
};

void expect(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr char vertexSource[] = R"(
cbuffer Constants { float4x4 g_ModelViewProjection; float4x4 g_Model; float4 g_UvScaleBias; };
struct VSInput { float3 Position : ATTRIB0; float3 Normal : ATTRIB1; float2 TexCoord : ATTRIB2; };
struct VSOutput { float4 Position : SV_POSITION; float2 TexCoord : TEXCOORD0; };
VSOutput main(VSInput input) {
    VSOutput output;
    output.Position = mul(g_ModelViewProjection, float4(input.Position, 1));
    output.TexCoord = input.TexCoord * g_UvScaleBias.xy + g_UvScaleBias.zw;
    return output;
})";
constexpr char pixelSource[] = R"(
Texture2D g_Texture;
SamplerState g_Texture_sampler;
cbuffer PixelConstants { float4 g_TintColor; float4 g_LightDirection; float4 g_AmbientColor; };
struct PSInput { float4 Position : SV_POSITION; float2 TexCoord : TEXCOORD0; };
float4 main(PSInput input) : SV_TARGET { return g_Texture.Sample(g_Texture_sampler, input.TexCoord) * g_TintColor; }
)";

class Fixtures final {
public:
    Fixtures() {
        root = std::filesystem::temp_directory_path() /
            ("tge-material-gpu-" + std::to_string(Clock::now().time_since_epoch().count()));
        write("shaders_hlsl/surface.vert.hlsl", vertexSource);
        write("shaders_hlsl/surface.frag.hlsl", pixelSource);
        write("shaders_hlsl/test.shader.json", Json{{"vertex", "surface.vert.hlsl"},
                                                    {"fragment", "surface.frag.hlsl"}}.dump());
        write("textures/white.ppm", "P3\n1 1\n255\n255 255 255\n");
        write("textures/red.ppm", "P3\n1 1\n255\n255 0 0\n");
        write("textures/green.ppm", "P3\n1 1\n255\n0 255 0\n");
        std::string checker = "P3\n8 8\n255\n";
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
            checker += x < 4 ? "255 0 0\n" : "0 255 0\n";
        write("textures/checker.ppm", checker);
        write("materials/base.json", base().dump());
        write("materials/instance.json", Json{{"base", "base.json"}, {"tint", {0.25, 0.5, 0.75, 1}}}.dump());
    }
    ~Fixtures() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void write(const std::string& path, const std::string& value) const {
        const auto target = root / path;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream output(target, std::ios::binary);
        output << value;
        expect(static_cast<bool>(output), "Cannot write GPU fixture");
    }
    static Json base() {
        return {{"version", 1}, {"shader", "shaders_hlsl/test.shader.json"}, {"texture", "textures/white.ppm"},
                {"tint", {1, 1, 1, 1}}, {"uvScale", {1, 1}}, {"blend", "opaque"},
                {"depthTest", true}, {"depthWrite", true}};
    }
    std::filesystem::path root;
};

struct Pixel final { std::uint8_t r, g, b, a; };
using Pixels = std::vector<Pixel>;

void waitForGraphics(Renderer& renderer) {
    auto fence = MaterialGpuTestAccess::device(renderer).createFence(false);
    expect(fence != nullptr, "Cannot allocate material GPU completion fence");
    fence->signal();
    fence->wait();
}

Pixels readback(Renderer& renderer) {
    using namespace Diligent;
    auto& device = MaterialGpuTestAccess::device(renderer);
    auto* context = device.nativeImmediateContext();
    auto* source = device.nativeSwapChain()->GetCurrentBackBufferRTV()->GetTexture();
    auto desc = source->GetDesc();
    desc.Name = "Material regression readback";
    desc.BindFlags = BIND_NONE; desc.Usage = USAGE_STAGING;
    desc.CPUAccessFlags = CPU_ACCESS_READ; desc.ImmediateContextMask = 1;
    RefCntAutoPtr<ITexture> staging;
    device.nativeRenderDevice()->CreateTexture(desc, nullptr, &staging);
    expect(staging != nullptr, "Cannot allocate material readback");
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    context->CopyTexture(CopyTextureAttribs{source, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                          staging, RESOURCE_STATE_TRANSITION_MODE_TRANSITION});
    waitForGraphics(renderer);
    MappedTextureSubresource mapped;
    context->MapTextureSubresource(staging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
    expect(mapped.pData != nullptr, "Cannot map material readback");
    Pixels pixels(std::size_t{desc.Width} * desc.Height);
    for (unsigned row = 0; row < desc.Height; ++row)
        std::memcpy(pixels.data() + std::size_t{row} * desc.Width,
                    static_cast<const std::byte*>(mapped.pData) + row * mapped.Stride,
                    std::size_t{desc.Width} * sizeof(Pixel));
    context->UnmapTextureSubresource(staging, 0, 0);
    return pixels;
}

Pixels capture(Renderer& renderer, const std::function<void()>& draw,
               const glm::vec4 clear = {0, 0, 0, 1}) {
    renderer.beginFrame(clear);
    draw();
    auto pixels = readback(renderer);
    renderer.endFrame();
    return pixels;
}

int srgb(const float linear) {
    const float value = linear <= 0.0031308F ? 12.92F * linear : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
    return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

void pixel(const Pixel actual, const std::array<int, 3> expected, const char* message, const int tolerance = 4) {
    const std::array<int, 3> channels{actual.r, actual.g, actual.b};
    for (std::size_t index = 0; index < channels.size(); ++index) {
        if (std::abs(channels[index] - expected[index]) > tolerance)
            throw std::runtime_error(std::string(message) + ": RGB=" + std::to_string(actual.r) + "," +
                                     std::to_string(actual.g) + "," + std::to_string(actual.b));
    }
}

void equalPixels(const Pixels& a, const Pixels& b, const char* message) {
    expect(a.size() == b.size(), "Readback dimensions differ");
    for (std::size_t index = 0; index < a.size(); ++index)
        if (std::memcmp(&a[index], &b[index], sizeof(Pixel)) != 0) throw std::runtime_error(message);
}

void writeImage(const std::filesystem::path& path, const Pixels& pixels, const unsigned width) {
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << pixels.size() / width << "\n255\n";
    for (const auto p : pixels) {
        const std::array bytes{p.r, p.g, p.b};
        output.write(reinterpret_cast<const char*>(bytes.data()), 3);
    }
    expect(static_cast<bool>(output), "Cannot write material comparison image");
}

void initializeQuad(Renderer& renderer, engine::resources::Mesh& mesh, const float localDepth = 0) {
    using engine::resources::MeshVertex;
    const std::array<MeshVertex, 4> corners{{
        {{-1, -1, 0}, {0, 0, 1}, {0, 1}}, {{-1, 1, 0}, {0, 0, 1}, {0, 0}},
        {{1, 1, 0}, {0, 0, 1}, {1, 0}}, {{1, -1, 0}, {0, 0, 1}, {1, 1}}}};
    constexpr std::array order{0, 1, 2, 0, 2, 3, 2, 1, 0, 3, 2, 0};
    for (const auto index : order) {
        auto vertex = corners[static_cast<std::size_t>(index)];
        vertex.position.z += localDepth;
        mesh.data.vertices.push_back(vertex);
    }
    mesh.vertexCount = static_cast<std::uint32_t>(mesh.data.vertices.size());
    engine::rhi::BufferDesc desc;
    desc.size = mesh.data.vertices.size() * sizeof(MeshVertex);
    desc.usage = engine::rhi::BufferUsage::Vertex;
    desc.vertexLayout = engine::rhi::VertexLayout::Position3Normal3Uv2;
    mesh.vertexBuffer = renderer.createGpuBuffer(desc, mesh.data.vertices.data());
    mesh.setLoadState(engine::resources::ResourceLoadState::Loaded);
    expect(mesh.isReady(), "GPU quad could not be uploaded");
}

void draw(Renderer& renderer, const engine::resources::Mesh& mesh, Material& material,
          const float depth = 0.5F) {
    renderer.drawMaterialMesh(mesh, material, glm::translate(glm::mat4(1), glm::vec3(0, 0, depth)),
                              glm::mat4(1), glm::vec4(1), glm::vec2(1));
}

void save(Renderer& renderer, const std::string& path, const Json& data) {
    std::string error;
    if (!renderer.materialLibrary().save(path, data, &error)) throw std::runtime_error(error);
}

void verifyMaterialPreview(Renderer& renderer, const Fixtures& fixture, engine::resources::Mesh& mesh,
                           const std::shared_ptr<Material>& saved, const Json& data, const Json& swapped) {
    engine::ecs::World world;
    engine::ecs::RenderSystem system;
    engine::renderer::RenderAdapter adapter(renderer);
    const auto borrowedMesh = std::shared_ptr<engine::resources::Mesh>(&mesh, [](engine::resources::Mesh*) {});
    const auto add = [&](engine::ecs::World& scene, const char* material, const glm::vec3 position, const glm::vec3 scale) {
        const auto entity = scene.createEntity();
        engine::ecs::Transform transform;
        transform.position = position;
        transform.scale = scale;
        scene.addComponent<engine::ecs::Transform>(entity, transform);
        engine::ecs::MeshRenderer component;
        component.mesh = borrowedMesh;
        component.materialId = material;
        scene.addComponent<engine::ecs::MeshRenderer>(entity, std::move(component));
        return entity;
    };
    const auto target = add(world, saved->path.c_str(), {-0.5F, 0, 0.5F}, {0.4F, 0.75F, 1});
    add(world, saved->path.c_str(), {0.5F, 0, 0.5F}, {0.4F, 0.75F, 1});
    const auto captureScene = [&](const std::shared_ptr<Material>& draft) {
        return capture(renderer, [&] { system.render(world, adapter, glm::mat4(1), target, draft); });
    };
    const auto baseline = captureScene({});
    const auto savedRevision = saved->revision;
    auto draft = std::make_shared<Material>();
    draft->path = saved->path;
    draft->revision = 1;
    draft->ready = renderer.materialLibrary().preview(saved->path, swapped, draft->definition);
    expect(draft->ready, "GPU preview fixture failed draft validation");
    const auto previewed = captureScene(draft);
    pixel(previewed.at(32 * 64 + 16), {srgb(0.25F), srgb(0.75F), 0}, "Selected mesh did not show the draft");
    pixel(previewed.at(32 * 64 + 48), {srgb(0.75F), srgb(0.25F), 0}, "A second mesh using the saved asset showed the draft");

    fixture.write("textures/preview-blue.ppm", "P3\n1 1\n255\n0 0 255\n");
    auto changed = swapped;
    changed["graph"]["nodes"][0]["texture"] = "textures/preview-blue.ppm";
    auto loadingTexture = renderer.loadTexture("textures/preview-blue.ppm");
    expect(loadingTexture && loadingTexture->isReady(), "GPU preview texture fixture failed to load");
    loadingTexture->setLoadState(engine::resources::ResourceLoadState::Loading);
    expect(renderer.materialLibrary().preview(saved->path, changed, draft->definition), "Changed GPU preview fixture did not validate");
    ++draft->revision;
    equalPixels(previewed, captureScene(draft), "An uploading texture replaced the ready preview");
    loadingTexture->setLoadState(engine::resources::ResourceLoadState::Loaded);
    const auto updated = captureScene(draft);
    pixel(updated.at(32 * 64 + 16), {srgb(0.25F), 0, srgb(0.75F)}, "Completed preview upload did not publish");

    auto invalid = std::make_shared<Material>(*draft);
    auto invalidProgram = std::make_shared<engine::resources::MaterialGraphProgram>(*invalid->definition.graphProgram);
    invalidProgram->pixelSource = "invalid material preview shader";
    invalid->definition.graphProgram = std::move(invalidProgram);
    ++invalid->revision;
    expectedShaderError.store(true);
    const auto previous = captureScene(invalid);
    expectedShaderError.store(false);
    equalPixels(updated, previous, "A failed preview shader replaced the ready preview");
    expect(!invalid->runtimeError.empty() && saved->runtimeError.empty(), "Preview shader failure leaked into the saved material");
    equalPixels(baseline, captureScene({}), "Clearing the preview did not restore the saved appearance");
    std::ifstream file(fixture.root / saved->path, std::ios::binary);
    expect(Json::parse(file) == data && saved->revision == savedRevision,
           "Preview modified the saved asset or its live revision");

    draft->definition.blend = engine::resources::MaterialBlend::Alpha;
    draft->definition.depthWrite = false;
    draft->definition.tint.a = 0.5F;
    ++draft->revision;
    const auto layered = [&](const bool reverse) {
        engine::ecs::World scene;
        if (reverse) add(scene, "materials/submission-opaque.json", {0, 0, 0.9F}, {1, 1, 1});
        const auto front = add(scene, saved->path.c_str(), {0, 0, 0.2F}, {1, 1, 1});
        if (!reverse) add(scene, "materials/submission-opaque.json", {0, 0, 0.9F}, {1, 1, 1});
        return capture(renderer, [&] { system.render(scene, adapter, glm::mat4(1), front, draft); });
    };
    const auto blended = layered(false);
    equalPixels(blended, layered(true), "Transparent preview order depends on entity creation order");
    pixel(blended.at(32 * 64 + 32), {srgb(0.125F), 0, srgb(0.875F)}, "Preview transparency did not blend over opaque geometry");
    static_cast<void>(renderer.prepareMaterialPreview({}));
    std::cout << "MATERIAL_PREVIEW_GPU_PASS selected_only=1 pending_texture_retains=1 failed_shader_retains=1 saved_untouched=1 transparent_order=1\n";
}

void verifyGraph(Renderer& renderer, const Fixtures& fixture, engine::resources::Mesh& mesh) {
    const std::string path = "materials/graph.json";
    Json nodes = Json::array({
        {{"id", 1}, {"type", "texture"}, {"texture", "textures/red.ppm"}},
        {{"id", 2}, {"type", "texture"}, {"texture", "textures/green.ppm"}},
        {{"id", 3}, {"type", "scalar"}, {"value", 0.25}},
        {{"id", 4}, {"type", "lerp"}, {"inputs", {{"a", 1}, {"b", 2}, {"alpha", 3}}}},
        {{"id", 5}, {"type", "scalar"}, {"value", 0}},
        {{"id", 6}, {"type", "scalar"}, {"value", 1}}
    });
    const Json graph = {{"nodes", std::move(nodes)}, {"outputs", {{"baseColor", 5}, {"emissive", 4}, {"metallic", 6}}}};
    const Json data = {{"graph", graph}};
    save(renderer, path, data);
    auto material = renderer.materialLibrary().load(path);
    expect(material->ready && material->definition.graphProgram, "Material graph fixture failed to compile");
    const auto captureGraph = [&] { return capture(renderer, [&] { draw(renderer, mesh, *material); }); };
    const auto original = captureGraph();
    pixel(original.at(32 * 64 + 32), {srgb(0.75F), srgb(0.25F), 0},
          "Graph did not sample and blend two textures independently");
    expect(material->runtimeError.empty(), "Graph reported an unexpected GPU error");

    auto swapped = data;
    std::swap(swapped["graph"]["nodes"][0]["texture"], swapped["graph"]["nodes"][1]["texture"]);
    save(renderer, "materials/graph-swapped.json", swapped);
    auto other = renderer.materialLibrary().load("materials/graph-swapped.json");
    const auto reversed = capture(renderer, [&] { draw(renderer, mesh, *other); });
    pixel(reversed.at(32 * 64 + 32), {srgb(0.25F), srgb(0.75F), 0},
          "Graph shader reuse bound textures from another material");
    expect(renderer.materialStats().pipelineHits == 1, "Identical graph programs did not share their pipeline");

    auto cycle = data;
    cycle["graph"]["nodes"][3]["inputs"]["alpha"] = 4;
    fixture.write(path, cycle.dump());
    renderer.materialLibrary().poll(true);
    expect(!material->lastError.empty(), "Invalid graph reload was accepted without a diagnostic");
    equalPixels(original, captureGraph(), "Invalid graph reload replaced the last good material");

    auto broken = std::make_shared<engine::resources::MaterialGraphProgram>(*material->definition.graphProgram);
    broken->pixelSource = "invalid generated shader source";
    material->definition.graphProgram = std::move(broken);
    ++material->revision;
    expectedShaderError.store(true);
    const auto previous = captureGraph();
    expectedShaderError.store(false);
    expect(!material->runtimeError.empty(), "Generated shader failure was not exposed to the editor");
    equalPixels(original, previous, "Generated shader failure replaced the last good pipeline");

    save(renderer, path, data);
    equalPixels(original, captureGraph(), "Restoring graph source did not recover shader binding");
    expect(material->runtimeError.empty(), "Successful graph reload left a stale GPU error");
    verifyMaterialPreview(renderer, fixture, mesh, material, data, swapped);
    std::cout << "MATERIAL_GRAPH_GPU_PASS texture_slots=2 graph_pipeline_reuse=verified invalid_reload=retained\n";
}

void verifySceneSubmission(Renderer& renderer, engine::resources::Mesh& mesh) {
    auto opaque = Fixtures::base();
    opaque["tint"] = {0, 0, 1, 1};
    auto red = Fixtures::base();
    red["tint"] = {1, 0, 0, 0.5}; red["blend"] = "alpha"; red["depthWrite"] = false;
    auto green = red; green["tint"] = {0, 1, 0, 0.5};
    save(renderer, "materials/submission-opaque.json", opaque);
    save(renderer, "materials/submission-front.json", red);
    save(renderer, "materials/submission-middle.json", green);
    const auto scene = [&](const bool reverse, const bool layered) {
        engine::ecs::World world;
        engine::ecs::RenderSystem system;
        engine::renderer::RenderAdapter adapter(renderer);
        const auto borrowedMesh = std::shared_ptr<engine::resources::Mesh>(&mesh, [](engine::resources::Mesh*) {});
        const auto add = [&](const char* materialId, const float depth) {
            const auto entity = world.createEntity();
            engine::ecs::Transform transform;
            transform.position.z = depth;
            world.addComponent<engine::ecs::Transform>(entity, transform);
            engine::ecs::MeshRenderer component;
            component.mesh = borrowedMesh;
            component.materialId = materialId;
            world.addComponent<engine::ecs::MeshRenderer>(entity, std::move(component));
        };
        if (reverse) {
            add("materials/submission-front.json", 0.2F);
            if (layered) add("materials/submission-middle.json", 0.6F);
            add("materials/submission-opaque.json", 0.9F);
        } else {
            add("materials/submission-opaque.json", 0.9F);
            if (layered) add("materials/submission-middle.json", 0.6F);
            add("materials/submission-front.json", 0.2F);
        }
        auto result = capture(renderer, [&] { system.render(world, adapter, glm::mat4(1)); });
        const std::size_t count = layered ? 3 : 2;
        expect(system.submittedCount() == count && system.culledCount() == 0 && renderer.materialStats().draws == count,
               "ECS material submission lost or culled an overlapping visible quad");
        return result;
    };
    const auto ordered = scene(false, false);
    equalPixels(ordered, scene(true, false), "Opaque/transparent ECS output depends on entity creation order");
    pixel(ordered.at(32 * 64 + 32), {srgb(0.5F), 0, srgb(0.5F)}, "ECS did not draw opaque geometry before transparent");
    const auto layers = scene(false, true);
    equalPixels(layers, scene(true, true), "Transparent ECS depth order depends on entity creation order");
    pixel(layers.at(32 * 64 + 32), {srgb(0.5F), srgb(0.25F), srgb(0.25F)},
          "ECS transparent materials were not submitted back-to-front");
    auto frontMesh = std::make_shared<engine::resources::Mesh>();
    auto rearMesh = std::make_shared<engine::resources::Mesh>();
    initializeQuad(renderer, *frontMesh, 0.2F);
    initializeQuad(renderer, *rearMesh, 0.6F);
    const auto offsetScene = [&](const bool reverse) {
        engine::ecs::World world;
        engine::ecs::RenderSystem system;
        engine::renderer::RenderAdapter adapter(renderer);
        const auto add = [&](const char* material, const std::shared_ptr<engine::resources::Mesh>& geometry) {
            const auto entity = world.createEntity();
            world.addComponent<engine::ecs::Transform>(entity);
            engine::ecs::MeshRenderer component;
            component.mesh = geometry; component.materialId = material;
            world.addComponent<engine::ecs::MeshRenderer>(entity, std::move(component));
        };
        if (reverse) { add("materials/submission-front.json", frontMesh); add("materials/submission-middle.json", rearMesh); }
        else { add("materials/submission-middle.json", rearMesh); add("materials/submission-front.json", frontMesh); }
        return capture(renderer, [&] { system.render(world, adapter, glm::mat4(1)); });
    };
    const auto offsetLayers = offsetScene(false);
    equalPixels(offsetLayers, offsetScene(true), "Local mesh offsets made transparent ordering depend on creation order");
    pixel(offsetLayers.at(32 * 64 + 32), {srgb(0.5F), srgb(0.25F), 0},
          "Transparent mesh sorting ignored local geometry depth");
    std::cout << "MATERIAL_ECS_SUBMISSION_PASS opaque_then_transparent=verified back_to_front=verified"
                 " reversed_creation_order=exact local_mesh_centers=verified submitted_counts=verified\n";
}

void verifyAuthoredLighting(Renderer& renderer, engine::resources::Mesh& mesh) {
    using namespace engine::ecs;
    World world;
    RenderSystem system;
    engine::renderer::RenderAdapter adapter(renderer);
    const auto surface = world.createEntity();
    Transform surfaceTransform;
    surfaceTransform.position.z = 0.5F;
    world.addComponent<Transform>(surface, surfaceTransform);
    MeshRenderer material;
    material.mesh = std::shared_ptr<engine::resources::Mesh>(&mesh, [](engine::resources::Mesh*) {});
    material.shader = renderer.loadShaderProgram("assets/shaders_hlsl/textured.shader.json");
    material.tint = glm::vec4(0.5F, 0.5F, 0.5F, 1.0F);
    expect(material.shader != nullptr, "Authored lighting fixture has no textured shader");
    world.addComponent<MeshRenderer>(surface, std::move(material));

    const auto lightEntity = world.createEntity();
    Transform lightTransform;
    lightTransform.position.z = 1.0F;
    world.addComponent<Transform>(lightEntity, lightTransform);
    Light light;
    light.color = {1.0F, 0.0F, 0.0F};
    light.range = 0.9F;
    light.enabled = false;
    world.addComponent<Light>(lightEntity, light);
    auto* component = world.getComponent<Light>(lightEntity);
    const auto frame = [&] { return capture(renderer, [&] { system.render(world, adapter, glm::mat4(1)); }); };
    constexpr std::size_t Center = 32 * 64 + 32;
    constexpr std::size_t Edge = 32 * 64 + 2;
    const auto unlit = frame();
    component->enabled = true;
    const auto withPoint = frame();
    expect(withPoint[Center].r > unlit[Center].r + 20 && withPoint[Center].g == unlit[Center].g &&
        withPoint[Edge].r == unlit[Edge].r, "Point illumination must honor color and radius");
    component->kind = LightKind::Directional;
    component->color = {0.0F, 1.0F, 0.0F};
    world.getComponent<Transform>(lightEntity)->rotationEulerRadians.y = glm::radians(180.0F);
    const auto withSun = frame();
    expect(withSun[Center].g > unlit[Center].g + 20 && withSun[Center].r == unlit[Center].r,
        "Directional illumination must use the authored transform and color");
    component->enabled = false;
    equalPixels(unlit, frame(), "Disabled scene lights left stale GPU constants");
    std::cout << "AUTHORED_LIGHTS_GPU_PASS directional=verified point_radius=verified disabled=verified\n";
}

void verifyShutdownWait(Renderer& renderer) {
    using namespace Diligent;
    auto& device = MaterialGpuTestAccess::device(renderer);
    auto* context = device.nativeImmediateContext();
    FenceDesc desc;
    desc.Type = FENCE_TYPE_GENERAL;
    RefCntAutoPtr<IFence> gate;
    device.nativeRenderDevice()->CreateFence(desc, &gate);
    desc.Type = FENCE_TYPE_CPU_WAIT_ONLY;
    RefCntAutoPtr<IFence> marker;
    device.nativeRenderDevice()->CreateFence(desc, &marker);
    expect(gate && marker, "Cannot allocate renderer shutdown synchronization");
    context->DeviceWaitForFence(gate, 1);
    context->EnqueueSignal(marker, 1);
    context->Flush();
    std::atomic_bool returned{false};
    bool returnedEarly = false, gpuBlocked = false;
    std::jthread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        returnedEarly = returned.load(std::memory_order_acquire);
        gpuBlocked = marker->GetCompletedValue() < 1;
        gate->Signal(1);
    });
    renderer.shutdown();
    returned.store(true, std::memory_order_release);
    release.join();
    expect(gpuBlocked && !returnedEarly && marker->GetCompletedValue() >= 1,
           "Renderer shutdown released resources before blocked graphics work completed");
    std::cout << "MATERIAL_GPU_SHUTDOWN_WAIT_PASS gpu_gate=verified graphics_timeline=completed\n";
}

void verify(Renderer& renderer, const Fixtures& fixture, engine::resources::Mesh& mesh,
            const std::filesystem::path& evidence) {
    auto base = renderer.materialLibrary().load("materials/base.json");
    auto instance = renderer.materialLibrary().load("materials/instance.json");
    expect(base->ready && instance->ready, "Material fixtures failed to resolve");
    auto shader = renderer.loadShaderProgram(base->definition.shader);
    auto texture = renderer.loadTexture(base->definition.texture);
    expect(shader && shader->isReady() && texture && texture->isReady(), "Native material resources did not load");
    const auto model = glm::translate(glm::mat4(1), glm::vec3(0, 0, 0.5F));
    const auto before = capture(renderer, [&] {
        renderer.drawMesh(mesh, *shader, texture.get(), model, glm::mat4(1), instance->definition.tint);
    });
    const auto after = capture(renderer, [&] { draw(renderer, mesh, *instance); });
    equalPixels(before, after, "Data material changed legacy draw output");
    pixel(after.at(32 * 64 + 32), {srgb(0.25F), srgb(0.5F), srgb(0.75F)}, "Material tint did not reach GPU");
    const auto pane = capture(renderer, [&] {
        renderer.setSceneViewport({8, 12}, {32, 24});
        draw(renderer, mesh, *instance);
    });
    pixel(pane.at(24 * 64 + 24), {srgb(0.25F), srgb(0.5F), srgb(0.75F)}, "Scene did not render inside its viewport pane");
    pixel(pane.at(24 * 64 + 48), {0, 0, 0}, "Scene rendered past the viewport pane to the right");
    pixel(pane.at(48 * 64 + 24), {0, 0, 0}, "Scene rendered past the viewport pane below");
    equalPixels(after, capture(renderer, [&] {
        renderer.setSceneViewport({8, 12}, {32, 24});
        renderer.resetSceneViewport();
        draw(renderer, mesh, *instance);
    }), "Reset did not restore the full scene viewport");
    std::cout << "MATERIAL_VIEWPORT_GPU_PASS bounded_pane=1 reset_full_frame=1\n";
    if (!evidence.empty()) {
        std::filesystem::create_directories(evidence);
        writeImage(evidence / "legacy.ppm", before, 64);
        writeImage(evidence / "material.ppm", after, 64);
    }
    capture(renderer, [&] { draw(renderer, mesh, *base); draw(renderer, mesh, *instance); });
    expect(renderer.materialStats().pipelineCreates == 0 && renderer.materialStats().pipelineHits == 2,
           "Opaque instances did not reuse their shared shader/state PSO");

    auto settings = Fixtures::base();
    settings["tint"] = {1, 0, 0, 0.5}; settings["blend"] = "alpha"; settings["depthWrite"] = false;
    save(renderer, "materials/alpha.json", settings);
    auto alpha = renderer.materialLibrary().load("materials/alpha.json");
    auto pixels = capture(renderer, [&] { draw(renderer, mesh, *alpha); }, {0, 0, 1, 1});
    pixel(pixels.at(32 * 64 + 32), {srgb(0.5F), 0, srgb(0.5F)}, "Material alpha blend state was ignored");
    expect(renderer.materialStats().pipelineCreates == 1, "First alpha state did not create a PSO");
    settings["tint"] = {0, 1, 0, 0.5};
    save(renderer, "materials/alpha2.json", settings);
    auto alpha2 = renderer.materialLibrary().load("materials/alpha2.json");
    capture(renderer, [&] { draw(renderer, mesh, *alpha); draw(renderer, mesh, *alpha2); });
    expect(renderer.materialStats().pipelineCreates == 0 && renderer.materialStats().pipelineHits == 2,
           "Different instances recreated identical alpha PSOs");
    settings["blend"] = "additive"; settings["tint"] = {1, 0, 0, 0.5};
    save(renderer, "materials/additive.json", settings);
    auto additive = renderer.materialLibrary().load("materials/additive.json");
    pixels = capture(renderer, [&] { draw(renderer, mesh, *additive); }, {0, 0, 0.5F, 1});
    pixel(pixels.at(32 * 64 + 32), {srgb(0.5F), 0, srgb(0.5F)}, "Material additive blend state was ignored");

    auto redData = Fixtures::base(); redData["tint"] = {1, 0, 0, 1};
    auto greenData = Fixtures::base(); greenData["tint"] = {0, 1, 0, 1};
    save(renderer, "materials/red.json", redData); save(renderer, "materials/green.json", greenData);
    auto red = renderer.materialLibrary().load("materials/red.json");
    auto green = renderer.materialLibrary().load("materials/green.json");
    pixels = capture(renderer, [&] { draw(renderer, mesh, *red, 0.2F); draw(renderer, mesh, *green, 0.8F); });
    pixel(pixels.at(32 * 64 + 32), {255, 0, 0}, "Depth test did not reject the rear material");
    greenData["depthTest"] = false;
    save(renderer, "materials/green.json", greenData);
    pixels = capture(renderer, [&] { draw(renderer, mesh, *red, 0.2F); draw(renderer, mesh, *green, 0.8F); });
    pixel(pixels.at(32 * 64 + 32), {0, 255, 0}, "Disabling material depth test had no effect");
    greenData["depthTest"] = true; redData["depthWrite"] = false;
    save(renderer, "materials/red.json", redData); save(renderer, "materials/green.json", greenData);
    pixels = capture(renderer, [&] { draw(renderer, mesh, *red, 0.2F); draw(renderer, mesh, *green, 0.8F); });
    pixel(pixels.at(32 * 64 + 32), {0, 255, 0}, "Disabling material depth write had no effect");

    auto uvData = Fixtures::base(); uvData["texture"] = "textures/checker.ppm";
    save(renderer, "materials/uv.json", uvData);
    auto uv = renderer.materialLibrary().load("materials/uv.json");
    const auto uvBefore = capture(renderer, [&] { draw(renderer, mesh, *uv); });
    uvData["uvScale"] = {2, 1}; save(renderer, "materials/uv.json", uvData);
    const auto uvAfter = capture(renderer, [&] { draw(renderer, mesh, *uv); });
    expect(uvBefore.at(32 * 64 + 24).r > uvBefore.at(32 * 64 + 24).g &&
           uvAfter.at(32 * 64 + 24).g > uvAfter.at(32 * 64 + 24).r,
           "Material texture/UV scale did not change sampled pixels");

    verifySceneSubmission(renderer, mesh);
    verifyGraph(renderer, fixture, mesh);

    auto missingData = Fixtures::base(); missingData["texture"] = "textures/missing.ppm";
    fixture.write("materials/missing.json", missingData.dump());
    auto missing = renderer.materialLibrary().load("materials/missing.json");
    expect(!missing->ready && missing->lastError.find("textures/missing.ppm") != std::string::npos,
           "Broken material resource was accepted without a file diagnostic");
    pixels = capture(renderer, [&] { draw(renderer, mesh, *missing); });
    expect(renderer.materialStats().errors == 1 && pixels.at(32 * 64 + 32).r > pixels.at(32 * 64 + 32).g &&
           pixels.at(32 * 64 + 32).b > pixels.at(32 * 64 + 32).g,
           "Broken material did not render a visible magenta fallback");

    for (int revision = 0; revision < 5; ++revision) {
        auto changed = Fixtures::base(); changed["tint"] = revision % 2 == 0 ? Json{1, 0, 0, 1} : Json{0, 1, 0, 1};
        fixture.write("materials/base.json", changed.dump());
        renderer.materialLibrary().poll(true);
        const auto good = capture(renderer, [&] { draw(renderer, mesh, *base); });
        const auto goodRevision = base->revision;
        fixture.write("materials/base.json", "{deliberately broken");
        renderer.materialLibrary().poll(true);
        expect(base->ready && base->revision == goodRevision && !base->lastError.empty(),
               "Bad material reload replaced last-good version");
        equalPixels(good, capture(renderer, [&] { draw(renderer, mesh, *base); }),
                    "Bad material reload changed visible output");
    }
    fixture.write("materials/base.json", Fixtures::base().dump()); renderer.materialLibrary().poll(true);

    for (int revision = 0; revision < 3; ++revision) {
        const auto oldPipeline = shader->pipeline;
        fixture.write("shaders_hlsl/surface.frag.hlsl", std::string(pixelSource) + std::string(revision + 1, '\n'));
        const auto deadline = Clock::now() + std::chrono::seconds(4);
        do {
            renderer.pollHotReload();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        } while (shader->pipeline == oldPipeline && Clock::now() < deadline);
        expect(shader->pipeline != oldPipeline && shader->isReady(), "Valid shader hot reload did not rebuild PSO");
        const auto good = capture(renderer, [&] { draw(renderer, mesh, *instance); });
        const auto lastGoodPipeline = shader->pipeline;
        fixture.write("shaders_hlsl/surface.frag.hlsl", "#error intentional_material_gpu_failure\n");
        expectedShaderError = true;
        const auto errorDeadline = Clock::now() + std::chrono::milliseconds(500);
        while (Clock::now() < errorDeadline) {
            renderer.pollHotReload();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        expectedShaderError = false;
        expect(shader->isReady() && shader->pipeline == lastGoodPipeline, "Broken shader destroyed last-good PSO");
        equalPixels(good, capture(renderer, [&] { draw(renderer, mesh, *instance); }),
                    "Broken shader reload changed last-good visible output");
    }
    fixture.write("shaders_hlsl/surface.frag.hlsl", pixelSource);
    waitForGraphics(renderer);
    std::cout << "MATERIAL_GPU_READBACK_PASS legacy_equal=exact tint=verified uv=verified alpha=verified additive=verified"
                 " depth_test=verified depth_write=verified pso_reuse=verified material_reload_cycles=5 shader_reload_cycles=3\n";
}

RENDERDOC_API_1_0_0* captureApi(const std::filesystem::path& prefix) {
#if defined(_WIN32)
    const auto module = GetModuleHandleW(L"renderdoc.dll");
    expect(module != nullptr, "--capture requires renderdoccmd capture injection");
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    expect(getApi != nullptr, "Injected RenderDoc has no supported API entry point");
    RENDERDOC_API_1_0_0* api = nullptr;
    expect(getApi(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&api)) == 1 && api != nullptr,
           "RenderDoc API 1.0 is unavailable");
    const auto encoded = std::filesystem::absolute(prefix).generic_u8string();
    const std::string path{encoded.begin(), encoded.end()};
    api->SetCaptureFilePathTemplate(path.c_str());
    return api;
#else
    (void)prefix;
    throw std::runtime_error("Native RenderDoc capture test requires Windows");
#endif
}

void benchmark(Renderer& renderer, engine::resources::Mesh& mesh, const std::filesystem::path& output,
               const bool materialMode, RENDERDOC_API_1_0_0* api = nullptr) {
    auto material = renderer.materialLibrary().load("materials/instance.json");
    auto shader = renderer.loadShaderProgram(material->definition.shader);
    auto texture = renderer.loadTexture(material->definition.texture);
    expect(material->ready && shader && shader->isReady() && texture && texture->isReady(), "Benchmark resources failed");
    std::ofstream csv(output, std::ios::binary);
    expect(static_cast<bool>(csv), "Cannot open benchmark CSV");
    csv << "mode,frame,elapsed_s,frame_ms,render_cpu_ms,draws,pipeline_creates,pipeline_hits\n";
    const auto begin = Clock::now();
    std::size_t frame = 0;
    bool captured = false;
    for (;;) {
        glfwPollEvents();
        const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
        if (elapsed >= 8) break;
        const auto frameBegin = Clock::now();
        const bool captureFrame = api != nullptr && !captured && elapsed >= 3;
        if (captureFrame) api->StartFrameCapture(nullptr, nullptr);
        renderer.beginFrame({0, 0, 0, 1});
        const auto renderBegin = Clock::now();
        for (int index = 0; index < 256; ++index) {
            const auto model = glm::translate(glm::mat4(1), glm::vec3(0, 0, 0.5F));
            if (materialMode) renderer.drawMaterialMesh(mesh, *material, model, glm::mat4(1), glm::vec4(1), glm::vec2(1));
            else renderer.drawMesh(mesh, *shader, texture.get(), model, glm::mat4(1), material->definition.tint);
        }
        const double renderMs = std::chrono::duration<double, std::milli>(Clock::now() - renderBegin).count();
        renderer.endFrame();
        if (captureFrame) {
            expect(api->EndFrameCapture(nullptr, nullptr) == 1, "RenderDoc failed to save the representative frame");
            expect(api->GetNumCaptures() > 0, "RenderDoc did not publish a capture artifact");
            captured = true;
            std::cout << "MATERIAL_RENDERDOC_CAPTURE_PASS mode=" << (materialMode ? "material" : "legacy") << '\n';
        }
        const double frameMs = std::chrono::duration<double, std::milli>(Clock::now() - frameBegin).count();
        if (elapsed >= 3) {
            const auto& stats = renderer.materialStats();
            csv << (materialMode ? "material" : "legacy") << ',' << frame++ << ',' << elapsed << ','
                << frameMs << ',' << renderMs << ",256," << stats.pipelineCreates << ',' << stats.pipelineHits << '\n';
        }
    }
    expect(frame > 0 && static_cast<bool>(csv), "Benchmark did not record frames");
    expect(api == nullptr || captured, "Requested RenderDoc capture was not produced");
    std::cout << "MATERIAL_BENCHMARK_PASS mode=" << (materialMode ? "material" : "legacy")
              << " warmup_s=3 sample_s=5 draws_per_frame=256 frames=" << frame << '\n';
}

void soak(Renderer& renderer, const Fixtures& fixture, engine::resources::Mesh& mesh,
          const int seconds, const std::filesystem::path& output) {
    expect(seconds >= 30 && seconds <= 3600, "Soak duration must be 30..3600 seconds");
    std::ofstream csv(output, std::ios::binary);
    expect(static_cast<bool>(csv), "Cannot open soak CSV");
    csv << "elapsed_s,private_bytes,working_set_bytes,gpu_local_bytes,gpu_nonlocal_bytes,frames,material_revision,pso_variants\n";
    auto material = renderer.materialLibrary().load("materials/base.json");
    auto shader = renderer.loadShaderProgram(material->definition.shader);
    expect(material->ready && shader && shader->isReady(), "Soak fixture failed");
#if defined(_WIN32)
    Diligent::RefCntAutoPtr<Diligent::IRenderDeviceD3D12> d3d12{
        MaterialGpuTestAccess::device(renderer).nativeRenderDevice(), Diligent::IID_RenderDeviceD3D12};
    expect(d3d12 != nullptr, "Cannot query native D3D12 device for GPU soak counters");
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    expect(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "Cannot create read-only DXGI factory for soak counters");
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    expect(SUCCEEDED(factory->EnumAdapterByLuid(d3d12->GetD3D12Device()->GetAdapterLuid(), IID_PPV_ARGS(&adapter))),
           "Cannot match DXGI video-memory adapter to renderer device LUID");
#endif
    struct GpuSample final { double elapsed; std::uint64_t bytes; };
    std::vector<GpuSample> gpuSamples;
    const auto begin = Clock::now();
    std::size_t frames = 0, maximumVariants = 0;
    int nextSample = 0, nextMaterial = 2, nextShader = 10, nextBroken = 30;
    int materialChanges = 0, shaderChanges = 0, rejectedShaders = 0;
    while (true) {
        glfwPollEvents();
        const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
        if (elapsed >= seconds) break;
        if (elapsed >= nextMaterial) {
            auto data = Fixtures::base();
            const int state = materialChanges++ % 12;
            data["blend"] = state % 3 == 0 ? "opaque" : state % 3 == 1 ? "alpha" : "additive";
            data["depthTest"] = (state / 3) % 2 != 0;
            data["depthWrite"] = (state / 6) % 2 != 0;
            data["tint"] = {0.25 + (state % 3) * 0.25, 0.5, 0.75, 0.75};
            save(renderer, "materials/base.json", data);
            const auto good = capture(renderer, [&] { draw(renderer, mesh, *material); });
            fixture.write("materials/base.json", "{invalid soak material");
            renderer.materialLibrary().poll(true);
            equalPixels(good, capture(renderer, [&] { draw(renderer, mesh, *material); }),
                        "Soak invalid material altered last-good pixels");
            save(renderer, "materials/base.json", data);
            nextMaterial += 2;
        }
        if (elapsed >= nextShader) {
            const auto before = shader->pipeline;
            fixture.write("shaders_hlsl/surface.frag.hlsl", std::string(pixelSource) + std::string(++shaderChanges, '\n'));
            const auto deadline = Clock::now() + std::chrono::seconds(4);
            while (shader->pipeline == before && Clock::now() < deadline) {
                renderer.pollHotReload();
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            expect(shader->pipeline != before, "Soak shader failed to reload");
            nextShader += 10;
        }
        if (elapsed >= nextBroken) {
            const auto good = capture(renderer, [&] { draw(renderer, mesh, *material); });
            const auto goodPipeline = shader->pipeline;
            fixture.write("shaders_hlsl/surface.frag.hlsl", "#error intentional_soak_failure\n");
            expectedShaderError = true;
            const auto deadline = Clock::now() + std::chrono::milliseconds(500);
            while (Clock::now() < deadline) {
                renderer.pollHotReload();
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            expectedShaderError = false;
            expect(shader->pipeline == goodPipeline && shader->isReady(), "Soak bad shader removed last-good pipeline");
            equalPixels(good, capture(renderer, [&] { draw(renderer, mesh, *material); }),
                        "Soak bad shader changed last-good pixels");
            ++rejectedShaders;
            fixture.write("shaders_hlsl/surface.frag.hlsl", std::string(pixelSource) + std::string(++shaderChanges, '\n'));
            nextShader = static_cast<int>(elapsed);
            nextBroken += 30;
        }
        renderer.beginFrame({0, 0, 0, 1});
        for (int index = 0; index < 64; ++index) draw(renderer, mesh, *material);
        renderer.endFrame();
        ++frames;
        maximumVariants = std::max(maximumVariants, shader->materialPipelines.size());
        expect(shader->materialPipelines.size() <= 12, "Soak grew PSO variants beyond bounded material states");
        if (elapsed >= nextSample) {
            std::uint64_t privateBytes = 0, workingSet = 0, localBytes = 0, nonlocalBytes = 0;
#if defined(_WIN32)
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = static_cast<DWORD>(sizeof(counters));
            expect(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                        static_cast<DWORD>(sizeof(counters))) != 0, "Cannot read own-process soak memory counters");
            privateBytes = static_cast<std::uint64_t>(counters.PrivateUsage);
            workingSet = static_cast<std::uint64_t>(counters.WorkingSetSize);
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
            expect(SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                   SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal)),
                   "Cannot query own-process DXGI local/nonlocal GPU memory");
            localBytes = local.CurrentUsage;
            nonlocalBytes = nonlocal.CurrentUsage;
#endif
            const double sampledAt = std::chrono::duration<double>(Clock::now() - begin).count();
            gpuSamples.push_back({sampledAt, localBytes + nonlocalBytes});
            csv << sampledAt << ',' << privateBytes << ',' << workingSet << ',' << localBytes << ',' << nonlocalBytes << ',' << frames << ','
                << material->revision << ',' << shader->materialPipelines.size() << '\n';
            csv.flush();
            nextSample = static_cast<int>(elapsed) + 1;
        }
    }
    expect(frames > 0 && materialChanges > 0 && shaderChanges > 0 && static_cast<bool>(csv), "Soak made no progress");
    waitForGraphics(renderer);
    if (seconds >= 60) {
        const auto median = [](std::vector<std::uint64_t> values) {
            expect(!values.empty(), "GPU soak has no post-warmup memory samples");
            std::sort(values.begin(), values.end());
            const auto middle = values.size() / 2;
            return values.size() % 2 != 0 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
        };
        std::vector<std::uint64_t> firstWindow, lastWindow;
        for (const auto& sample : gpuSamples) {
            if (sample.elapsed >= 30 && sample.elapsed < 45) firstWindow.push_back(sample.bytes);
            if (sample.elapsed >= seconds - 15) lastWindow.push_back(sample.bytes);
        }
        const auto warmed = median(std::move(firstWindow));
        const auto ending = median(std::move(lastWindow));
        const auto tolerance = std::max<std::uint64_t>(16U * 1024U * 1024U, warmed / 10U);
        std::cout << "MATERIAL_SOAK_GPU_MEMORY warmed_median_bytes=" << warmed << " final_median_bytes=" << ending
                  << " allowed_growth_bytes=" << tolerance << " adapter=matched_D3D12_LUID"
                     " metric=DXGI_process_local_plus_nonlocal_CurrentUsage\n";
        expect(ending <= warmed + tolerance, "Post-warmup DXGI GPU memory grew beyond the soak tolerance");
    }
    std::cout << "MATERIAL_SOAK_PASS seconds=" << seconds << " frames=" << frames
              << " material_good_bad_cycles=" << materialChanges << " shader_good_reloads=" << shaderChanges
              << " shader_bad_reloads=" << rejectedShaders << " max_pso_variants=" << maximumVariants
              << " memory_scope=DXGI_process_GPU_usage_and_private_working_set_bounded_session\n";
}
}

int main(int argc, char** argv) {
    try {
        const Diagnostics diagnostics;
        engine::core::Log::init();
        const Fixtures fixture;
        {
            engine::platform::WindowDesc description;
            description.width = description.height = 64;
            description.title = "Material GPU regression"; description.useOpenGLContext = false; description.vsync = false;
            auto window = engine::platform::Window::create(description);
            glfwHideWindow(static_cast<GLFWwindow*>(window->nativeHandle()));
            Renderer renderer;
            renderer.setAsyncLoadingEnabled(false); renderer.setGpuUploadEnabled(true);
            expect(renderer.init(*window, engine::rhi::BackendType::Diligent, false, "d3d12"), "Cannot initialize native renderer");
            renderer.setProjectAssetRoot(fixture.root);
            engine::resources::Mesh mesh;
            initializeQuad(renderer, mesh);
            if (argc >= 4 && (std::string{argv[1]} == "--benchmark" || std::string{argv[1]} == "--capture")) {
                const std::string mode = argv[3];
                expect(mode == "legacy" || mode == "material", "Benchmark mode must be legacy or material");
                const auto output = std::filesystem::u8path(argv[2]);
                const bool captureRequested = std::string{argv[1]} == "--capture";
                benchmark(renderer, mesh, captureRequested ? std::filesystem::path{output.string() + ".csv"} : output,
                          mode == "material", captureRequested ? captureApi(output) : nullptr);
            } else if (argc >= 4 && std::string{argv[1]} == "--soak") {
                soak(renderer, fixture, mesh, std::stoi(argv[2]), std::filesystem::u8path(argv[3]));
            } else {
                const auto evidence = argc >= 3 && std::string{argv[1]} == "--evidence"
                    ? std::filesystem::u8path(argv[2]) : std::filesystem::path{};
                verify(renderer, fixture, mesh, evidence);
                verifyAuthoredLighting(renderer, mesh);
            }
            verifyShutdownWait(renderer);
        }
        expect(validationErrors.load() == 0, "Native GPU validation reported unexpected errors");
        std::cout << "MATERIAL_GPU_SHUTDOWN_PASS unexpected_validation_errors=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Material GPU test failure: " << error.what() << '\n';
        return 1;
    }
}
