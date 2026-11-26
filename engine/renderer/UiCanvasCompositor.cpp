#include "engine/renderer/UiCanvasCompositor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "engine/core/Log.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/PipelineState.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Shader.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Texture.h"
#include "third_party/DiligentEngine/DiligentTools/Imgui/interface/ImGuiDiligentRenderer.hpp"
#include "third_party/DiligentEngine/DiligentTools/Imgui/interface/ImGuiImplDiligent.hpp"

namespace engine::renderer {
namespace {
using namespace Diligent;
constexpr std::uint64_t maximumBytes = 64ULL * 1024 * 1024;
constexpr unsigned maximumLayers = 32, maximumSide = 2048;
constexpr char resolveVertex[] = R"(
struct Output { float4 pos:SV_POSITION; float2 uv:TEXCOORD; };
Output main(uint id:SV_VertexID) {
    Output o;
    o.uv=float2((id<<1)&2,id&2);
    o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);
    return o;
})";
constexpr char resolvePixel[] = R"(
Texture2D Layer; SamplerState Layer_sampler;
float4 main(float4 pos:SV_POSITION,float2 uv:TEXCOORD):SV_Target {
    float4 c=Layer.Sample(Layer_sampler,uv);
    return float4(c.a>0 ? saturate(c.rgb/c.a) : float3(0,0,0),c.a);
})";
struct DrawListDeleter { void operator()(ImDrawList* list) const { IM_DELETE(list); } };
using DrawList = std::unique_ptr<ImDrawList, DrawListDeleter>;
struct Surface {
    RefCntAutoPtr<ITexture> accumulation, straight;
    unsigned width = 0, height = 0;
    std::uint64_t bytes = 0, used = 0;
};
struct Job {
    std::shared_ptr<Surface> surface;
    DrawList list;
    float x, y, width, height;
};
}

struct UiCanvasCompositor::Impl {
    RefCntAutoPtr<IRenderDevice> device;
    std::unique_ptr<ImGuiDiligentRenderer> renderer;
    RefCntAutoPtr<IPipelineState> resolve;
    RefCntAutoPtr<IShaderResourceBinding> resources;
    std::vector<std::shared_ptr<Surface>> pool;
    std::vector<std::vector<Job>> frames;
    std::size_t slot = 0;
    std::uint64_t frame = 0, allocated = 0;
    bool warned = false;

    Impl(IRenderDevice* native, std::size_t count) : device(native), frames(std::max(count, std::size_t{1})) {
        if (!device) throw std::invalid_argument("CanvasGroup requires a render device");
        ImGuiDiligentCreateInfo info{device, TEX_FORMAT_RGBA8_UNORM, TEX_FORMAT_UNKNOWN};
        info.ColorConversion = IMGUI_COLOR_CONVERSION_MODE_NONE;
        renderer = std::make_unique<ImGuiDiligentRenderer>(info);
        ShaderCreateInfo shader;
        shader.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
        shader.Desc.UseCombinedTextureSamplers = True;
        shader.EntryPoint = "main";
        shader.Source = resolveVertex;
        shader.Desc.Name = "CanvasGroup resolve vertex";
        shader.Desc.ShaderType = SHADER_TYPE_VERTEX;
        RefCntAutoPtr<IShader> vertex, pixel;
        device->CreateShader(shader, &vertex);
        shader.Source = resolvePixel;
        shader.Desc.Name = "CanvasGroup resolve pixel";
        shader.Desc.ShaderType = SHADER_TYPE_PIXEL;
        device->CreateShader(shader, &pixel);
        if (!vertex || !pixel) throw std::runtime_error("CanvasGroup resolve shader creation failed");
        GraphicsPipelineStateCreateInfo pipeline;
        pipeline.PSODesc.Name = "CanvasGroup straight RGBA resolve";
        pipeline.pVS = vertex; pipeline.pPS = pixel;
        auto& graphics = pipeline.GraphicsPipeline;
        graphics.NumRenderTargets = 1;
        graphics.RTVFormats[0] = TEX_FORMAT_RGBA8_UNORM;
        graphics.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        graphics.RasterizerDesc.CullMode = CULL_MODE_NONE;
        graphics.DepthStencilDesc.DepthEnable = False;
        ShaderResourceVariableDesc variable{SHADER_TYPE_PIXEL, "Layer", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC};
        SamplerDesc sampler;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = TEXTURE_ADDRESS_CLAMP;
        ImmutableSamplerDesc immutable{SHADER_TYPE_PIXEL, "Layer", sampler};
        pipeline.PSODesc.ResourceLayout.Variables = &variable;
        pipeline.PSODesc.ResourceLayout.NumVariables = 1;
        pipeline.PSODesc.ResourceLayout.ImmutableSamplers = &immutable;
        pipeline.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
        device->CreateGraphicsPipelineState(pipeline, &resolve);
        if (!resolve) throw std::runtime_error("CanvasGroup resolve pipeline creation failed");
        resolve->CreateShaderResourceBinding(&resources, true);
        if (!resources || !resources->GetVariableByName(SHADER_TYPE_PIXEL, "Layer"))
            throw std::runtime_error("CanvasGroup resolve binding creation failed");
    }

    std::shared_ptr<Surface> acquire(unsigned width, unsigned height) {
        for (auto& surface : pool) {
            if (surface->used != frame && surface->width == width && surface->height == height) {
                surface->used = frame;
                return surface;
            }
        }
        const std::uint64_t bytes = std::uint64_t{width} * height * 8;
        while (allocated + bytes > maximumBytes || pool.size() >= maximumLayers * frames.size()) {
            auto victim = std::find_if(pool.begin(), pool.end(), [&](const auto& surface) {
                return surface.use_count() == 1 && surface->used != frame;
            });
            if (victim == pool.end()) return {};
            allocated -= (*victim)->bytes;
            pool.erase(victim);
        }
        auto result = std::make_shared<Surface>();
        result->width = width; result->height = height; result->bytes = bytes; result->used = frame;
        TextureDesc texture;
        texture.Name = "CanvasGroup accumulation";
        texture.Type = RESOURCE_DIM_TEX_2D;
        texture.Width = width; texture.Height = height;
        texture.Format = TEX_FORMAT_RGBA8_UNORM;
        texture.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
        device->CreateTexture(texture, nullptr, &result->accumulation);
        texture.Name = "CanvasGroup straight RGBA";
        device->CreateTexture(texture, nullptr, &result->straight);
        if (!result->accumulation || !result->straight) return {};
        pool.push_back(result); allocated += bytes;
        return result;
    }
    void warning() {
        if (!warned) {
            ENGINE_LOG_WARN("CanvasGroup layer omitted: limit is 32 layers/frame, 2048 pixels/side and 64 MiB GPU targets");
            warned = true;
        }
    }
};

UiCanvasCompositor::UiCanvasCompositor(Diligent::IRenderDevice* device, std::size_t frames)
    : m_impl(std::make_unique<Impl>(device, frames)) {}
UiCanvasCompositor::~UiCanvasCompositor() = default;
void UiCanvasCompositor::beginFrame(std::size_t slot) {
    auto& state = *m_impl;
    state.slot = slot % state.frames.size();
    state.frames[state.slot].clear();
    ++state.frame;
}
UiCanvasCompositor::Image UiCanvasCompositor::enqueue(const void*, const ImDrawList& list,
                                                     float x, float y, float width, float height) {
    auto& state = *m_impl;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0 || height <= 0) return {};
    if (width > maximumSide || height > maximumSide || state.frames[state.slot].size() >= maximumLayers ||
        list.VtxBuffer.Size > 131072 || list.IdxBuffer.Size > 393216 || list.CmdBuffer.Size > 16384) {
        state.warning(); return {};
    }
    try {
        for (const auto& command : list.CmdBuffer) {
            if (command.UserCallback || std::uint64_t{command.IdxOffset} + command.ElemCount > unsigned(list.IdxBuffer.Size)) return {};
            if (command.ElemCount) {
                const auto first = list.IdxBuffer.Data + command.IdxOffset;
                const auto maximum = *std::max_element(first, first + command.ElemCount);
                if (std::uint64_t{command.VtxOffset} + maximum >= unsigned(list.VtxBuffer.Size)) return {};
            }
        }
        const auto w = static_cast<unsigned>(std::ceil(width));
        const auto h = static_cast<unsigned>(std::ceil(height));
        auto surface = state.acquire(w, h);
        if (!surface) { state.warning(); return {}; }
        DrawList copy{list.CloneOutput()};
        if (!copy) return {};
        const Image image{surface->straight->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), float(w), float(h)};
        state.frames[state.slot].push_back(Job{std::move(surface), std::move(copy), x, y, width, height});
        return image;
    } catch (const std::exception& error) {
        ENGINE_LOG_WARN("CanvasGroup allocation/capture failed: {}", error.what());
        return {};
    }
}
bool UiCanvasCompositor::pending() const { return !m_impl->frames[m_impl->slot].empty(); }
void UiCanvasCompositor::render(Diligent::IDeviceContext* context) {
    using namespace Diligent;
    auto& state = *m_impl;
    if (!context) return;
    for (auto& job : state.frames[state.slot]) {
        const auto& surface = *job.surface;
        auto* target = surface.accumulation->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const float clear[4] = {0, 0, 0, 0};
        context->ClearRenderTarget(target, clear, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        ImDrawData data;
        data.Valid = true;
        data.DisplayPos = ImVec2(job.x, job.y);
        data.DisplaySize = ImVec2(job.width, job.height);
        data.FramebufferScale = ImVec2(surface.width / job.width, surface.height / job.height);
        data.Textures = ImGui::GetDrawData()->Textures;
        data.CmdLists.push_back(job.list.get());
        data.CmdListsCount = 1;
        data.TotalIdxCount = job.list->IdxBuffer.Size;
        data.TotalVtxCount = job.list->VtxBuffer.Size;
        state.renderer->NewFrame(surface.width, surface.height, SURFACE_TRANSFORM_IDENTITY);
        state.renderer->RenderDrawData(context, &data);

        target = surface.straight->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        context->SetRenderTargets(1, &target, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState(state.resolve);
        Viewport viewport;
        viewport.Width = float(surface.width); viewport.Height = float(surface.height);
        context->SetViewports(1, &viewport, surface.width, surface.height);
        state.resources->GetVariableByName(SHADER_TYPE_PIXEL, "Layer")->Set(
            surface.accumulation->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        context->CommitShaderResources(state.resources, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->Draw(DrawAttribs{3, DRAW_FLAG_VERIFY_ALL});
    }
}
}
