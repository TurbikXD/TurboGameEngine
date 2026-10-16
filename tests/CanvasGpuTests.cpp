#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "engine/core/Log.h"
#include "engine/platform/Window.h"
#include "engine/renderer/UiCanvasCompositor.h"
#include "engine/rhi_diligent/DiligentDevice.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Fence.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Texture.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/TextureView.h"
#include "third_party/DiligentEngine/DiligentCore/Platforms/Basic/interface/BasicPlatformDebug.hpp"
#include "third_party/DiligentEngine/DiligentCore/Primitives/interface/DebugOutput.h"
#include "third_party/DiligentEngine/DiligentTools/Imgui/interface/ImGuiDiligentRenderer.hpp"
#include "third_party/DiligentEngine/DiligentTools/Imgui/interface/ImGuiImplDiligent.hpp"

namespace {
using namespace Diligent;
using Compositor = engine::renderer::UiCanvasCompositor;
std::atomic_size_t validationErrors{0};
DebugMessageCallbackType previousDebugCallback = nullptr;

void DILIGENT_CALL_TYPE captureDiagnostic(DEBUG_MESSAGE_SEVERITY severity, const Char* message,
                                        const Char* function, const Char* file, const int line) {
    if (severity >= DEBUG_MESSAGE_SEVERITY_ERROR) validationErrors.fetch_add(1, std::memory_order_relaxed);
    if (previousDebugCallback) previousDebugCallback(severity, message, function, file, line);
}
class Diagnostics final {
public:
    Diagnostics() : previousBreak(BasicPlatformDebug::GetBreakOnError()) {
        previousDebugCallback = DebugMessageCallback;
        BasicPlatformDebug::SetBreakOnError(false);
        SetDebugMessageCallback(captureDiagnostic);
    }
    ~Diagnostics() {
        SetDebugMessageCallback(previousDebugCallback);
        BasicPlatformDebug::SetBreakOnError(previousBreak);
    }
private:
    bool previousBreak;
};
void expect(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct ContextDeleter { void operator()(ImGuiContext* context) const { ImGui::DestroyContext(context); } };
struct FinishRendering {
    IDeviceContext* context;
    ImGuiDiligentRenderer& renderer;
    ~FinishRendering() { context->WaitForIdle(); renderer.InvalidateDeviceObjects(); }
};
struct Pixel { std::uint8_t r, g, b, a; };
struct Pixels {
    unsigned width, height;
    std::vector<Pixel> values;
    const Pixel& at(const unsigned x, const unsigned y) const { return values.at(std::size_t{y} * width + x); }
};
void pixel(const Pixel actual, const std::array<int, 4> expected, const char* message, const int tolerance = 3) {
    const std::array<int, 4> channels{actual.r, actual.g, actual.b, actual.a};
    for (std::size_t index = 0; index < channels.size(); ++index) if (std::abs(channels[index] - expected[index]) > tolerance) {
        throw std::runtime_error(std::string(message) + ": actual RGBA=" + std::to_string(actual.r) + "," +
            std::to_string(actual.g) + "," + std::to_string(actual.b) + "," + std::to_string(actual.a));
    }
}
RefCntAutoPtr<ITexture> texture(IRenderDevice* device, const unsigned width, const unsigned height, const bool white = false) {
    TextureDesc desc;
    desc.Name = white ? "Canvas test opaque white" : "Canvas test final composite";
    desc.Type = RESOURCE_DIM_TEX_2D; desc.Width = width; desc.Height = height;
    desc.MipLevels = 1; desc.Format = TEX_FORMAT_RGBA8_UNORM;
    desc.BindFlags = white ? BIND_SHADER_RESOURCE : BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    const std::array<std::uint8_t, 4> data{255, 255, 255, 255};
    TextureSubResData subresource;
    subresource.pData = data.data(); subresource.Stride = 4;
    TextureData initial;
    initial.pSubResources = &subresource; initial.NumSubresources = 1;
    RefCntAutoPtr<ITexture> result;
    device->CreateTexture(desc, white ? &initial : nullptr, &result);
    expect(result != nullptr, "Canvas test texture creation failed");
    return result;
}
ImTextureRef imageRef(void* view) {
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(view)));
}
std::unique_ptr<ImDrawList> drawList(void* white, const ImVec2 origin, const ImVec2 size) {
    auto list = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
    list->_ResetForNewFrame();
    list->PushClipRect(origin, {origin.x + size.x, origin.y + size.y});
    list->PushTexture(imageRef(white));
    return list;
}
void rectangle(ImDrawList& list, const ImVec2 minimum, const ImVec2 maximum, const ImU32 color) {
    // The active 1x1 white SRV makes the font atlas' shared white-pixel UV valid
    // without depending on font coverage, glyphs or CPU rasterization.
    list.AddRectFilled(minimum, maximum, color);
}
void wait(IDeviceContext* context, IFence* fence, const Uint64 value) {
    context->EnqueueSignal(fence, value); context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (fence->GetCompletedValue() < value) {
        expect(std::chrono::steady_clock::now() < deadline, "Canvas GPU fence timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
Pixels readback(IRenderDevice* device, IDeviceContext* context, ITexture* source, IFence* fence, Uint64& sequence) {
    auto desc = source->GetDesc();
    desc.Name = "Canvas GPU pixel readback"; desc.BindFlags = BIND_NONE;
    desc.Usage = USAGE_STAGING; desc.CPUAccessFlags = CPU_ACCESS_READ; desc.ImmediateContextMask = 1;
    RefCntAutoPtr<ITexture> staging;
    device->CreateTexture(desc, nullptr, &staging);
    expect(staging != nullptr, "Canvas readback texture creation failed");
    context->CopyTexture(CopyTextureAttribs{source, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                          staging, RESOURCE_STATE_TRANSITION_MODE_TRANSITION});
    wait(context, fence, ++sequence);
    MappedTextureSubresource mapped;
    context->MapTextureSubresource(staging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
    expect(mapped.pData != nullptr, "Canvas readback map failed");
    Pixels result{desc.Width, desc.Height, std::vector<Pixel>(std::size_t{desc.Width} * desc.Height)};
    for (unsigned y = 0; y < desc.Height; ++y)
        std::memcpy(result.values.data() + std::size_t{y} * desc.Width,
            static_cast<const std::byte*>(mapped.pData) + y * mapped.Stride, std::size_t{desc.Width} * sizeof(Pixel));
    context->UnmapTextureSubresource(staging, 0, 0);
    return result;
}
ITexture* layerTexture(const Compositor::Image image) {
    expect(image.textureId != nullptr, "Canvas layer was unexpectedly rejected");
    return static_cast<ITextureView*>(image.textureId)->GetTexture();
}
void composite(ImGuiDiligentRenderer& renderer, IDeviceContext* context, ITexture* target,
               const Compositor::Image image, const ImU32 tint) {
    const auto& desc = target->GetDesc();
    auto* rtv = target->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    context->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clear[4]{0, 0, 0, 0};
    context->ClearRenderTarget(rtv, clear, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    auto list = drawList(image.textureId, {}, {static_cast<float>(desc.Width), static_cast<float>(desc.Height)});
    list->AddImage(imageRef(image.textureId), {}, {image.width, image.height}, {}, {1, 1}, tint);
    ImDrawData data;
    data.Valid = true; data.DisplayPos = {};
    data.DisplaySize = {static_cast<float>(desc.Width), static_cast<float>(desc.Height)};
    data.FramebufferScale = {1, 1}; data.Textures = ImGui::GetDrawData()->Textures;
    data.AddDrawList(list.get());
    renderer.NewFrame(desc.Width, desc.Height, SURFACE_TRANSFORM_IDENTITY);
    renderer.RenderDrawData(context, &data);
}
void begin(Compositor& compositor, ImGuiDiligentRenderer& renderer, const std::size_t slot) {
    compositor.beginFrame(slot);
    auto& io = ImGui::GetIO(); io.DisplaySize = {64, 64}; io.DeltaTime = 1.0F / 60.0F;
    renderer.NewFrame(64, 64, SURFACE_TRANSFORM_IDENTITY);
    ImGui::NewFrame();
}
void noOp(const ImDrawList*, const ImDrawCmd*) {}

void verify(IRenderDevice* device, IDeviceContext* context) {
    std::unique_ptr<ImGuiContext, ContextDeleter> imgui{ImGui::CreateContext()};
    ImGui::GetIO().IniFilename = nullptr;
    Compositor compositor(device, 2);
    ImGuiDiligentCreateInfo info{device, TEX_FORMAT_RGBA8_UNORM, TEX_FORMAT_UNKNOWN};
    info.ColorConversion = IMGUI_COLOR_CONVERSION_MODE_NONE;
    ImGuiDiligentRenderer renderer{info};
    auto white = texture(device, 1, 1, true);
    void* whiteView = white->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    FenceDesc fenceDesc; fenceDesc.Name = "Canvas test frame fence";
    RefCntAutoPtr<IFence> fence;
    device->CreateFence(fenceDesc, &fence);
    expect(fence != nullptr, "Canvas test fence creation failed");
    const FinishRendering finish{context, renderer};
    Uint64 sequence = 0;

    begin(compositor, renderer, 0);
    const ImVec2 origin{13, 17};
    auto children = drawList(whiteView, origin, {64, 64});
    rectangle(*children, origin, {origin.x + 32, origin.y + 32}, IM_COL32(255, 0, 0, 255));
    rectangle(*children, {origin.x + 16, origin.y}, {origin.x + 48, origin.y + 32}, IM_COL32(0, 0, 255, 255));
    rectangle(*children, {origin.x, origin.y + 40}, {origin.x + 16, origin.y + 56}, IM_COL32(255, 0, 0, 64));
    const auto layer = compositor.enqueue(nullptr, *children, origin.x, origin.y, 64, 64);
    children.reset(); // enqueue must retain a snapshot, not borrow this buffer.
    ImGui::Render(); compositor.render(context);
    const auto straight = readback(device, context, layerTexture(layer), fence, sequence);
    pixel(straight.at(8, 8), {255, 0, 0, 255}, "Opaque first child changed during flattening");
    pixel(straight.at(24, 8), {0, 0, 255, 255}, "Opaque overlapping children were faded independently");
    pixel(straight.at(8, 48), {255, 0, 0, 64}, "Flattened translucent edge was not resolved to straight RGBA");
    pixel(straight.at(60, 60), {0, 0, 0, 0}, "Transparent canvas background was not cleared");
    auto output = texture(device, 64, 64);
    composite(renderer, context, output, layer, IM_COL32(128, 255, 64, 128));
    auto pixels = readback(device, context, output, fence, sequence);
    pixel(pixels.at(8, 8), {64, 0, 0, 128}, "Group color/transparency did not apply once to the flattened child");
    pixel(pixels.at(24, 8), {0, 0, 32, 128}, "Overlap opacity is wrong (per-child alpha would produce 191)");
    pixel(pixels.at(40, 8), {0, 0, 32, 128}, "Overlapping and non-overlapping opacity differ");
    composite(renderer, context, output, layer, IM_COL32(255, 255, 255, 128));
    pixels = readback(device, context, output, fence, sequence);
    pixel(pixels.at(8, 48), {32, 0, 0, 32}, "Transparent edge was premultiplied twice (dark fringe)");
    context->FinishFrame();

    begin(compositor, renderer, 1);
    auto inner = drawList(whiteView, {}, {32, 32});
    rectangle(*inner, {}, {24, 32}, IM_COL32(255, 0, 0, 255));
    rectangle(*inner, {8, 0}, {32, 32}, IM_COL32(0, 255, 0, 255));
    const auto innerLayer = compositor.enqueue(nullptr, *inner, 0, 0, 32, 32);
    auto outer = drawList(whiteView, {}, {48, 40});
    rectangle(*outer, {}, {32, 32}, IM_COL32(0, 0, 255, 255));
    outer->AddImage(imageRef(innerLayer.textureId), {}, {32, 32}, {}, {1, 1}, IM_COL32(255, 255, 255, 128));
    const auto outerLayer = compositor.enqueue(nullptr, *outer, 0, 0, 48, 40);
    inner.reset(); outer.reset();
    ImGui::Render(); compositor.render(context);
    const auto nested = readback(device, context, layerTexture(outerLayer), fence, sequence);
    pixel(nested.at(16, 16), {0, 128, 127, 255}, "Nested layers were not rendered child-before-parent");
    auto nestedOutput = texture(device, 48, 40);
    composite(renderer, context, nestedOutput, outerLayer, IM_COL32(255, 255, 255, 128));
    const auto nestedPixels = readback(device, context, nestedOutput, fence, sequence);
    pixel(nestedPixels.at(16, 16), {0, 64, 64, 128}, "Nested opacity was not applied once at each group boundary");
    context->FinishFrame();

    // Slots are reused only after actual GPU fence completion. A different
    // geometry exercises target-size changes rather than a fresh compositor.
    expect(fence->GetCompletedValue() >= sequence, "Canvas frame slot reused before its fence");
    begin(compositor, renderer, 0);
    expect(!compositor.pending(), "Completed frame slot retained old canvas jobs");
    auto resized = drawList(whiteView, {}, {19, 11});
    rectangle(*resized, {}, {19, 11}, IM_COL32(255, 255, 0, 255));
    const auto resizedLayer = compositor.enqueue(nullptr, *resized, 0, 0, 19, 11);
    expect(resizedLayer.width == 19 && resizedLayer.height == 11, "Canvas resize returned stale dimensions");
    ImGui::Render(); compositor.render(context);
    const auto resizedPixels = readback(device, context, layerTexture(resizedLayer), fence, sequence);
    pixel(resizedPixels.at(18, 10), {255, 255, 0, 255}, "Resized canvas retained stale contents or clipped its edge");
    context->FinishFrame();

    begin(compositor, renderer, 1);
    auto limits = drawList(whiteView, {}, {16, 16});
    rectangle(*limits, {}, {16, 16}, IM_COL32(255, 255, 255, 255));
    expect(!compositor.enqueue(nullptr, *limits, 0, 0, 2049, 16).textureId, "Canvas accepted an oversized target");
    expect(!compositor.enqueue(nullptr, *limits, 0, 0, std::numeric_limits<float>::quiet_NaN(), 16).textureId, "Canvas accepted non-finite dimensions");
    expect(!compositor.enqueue(nullptr, *limits, 0, 0, 0, 16).textureId, "Canvas accepted an empty target");
    auto callback = drawList(whiteView, {}, {16, 16}); callback->AddCallback(noOp, nullptr);
    expect(!compositor.enqueue(nullptr, *callback, 0, 0, 16, 16).textureId, "Canvas accepted an arbitrary draw callback");
    auto malformed = drawList(whiteView, {}, {16, 16});
    rectangle(*malformed, {}, {16, 16}, IM_COL32(255, 255, 255, 255));
    expect(!malformed->CmdBuffer.empty() && !malformed->IdxBuffer.empty(), "Malformed capture fixture has no geometry");
    const auto originalCommand = malformed->CmdBuffer[0];
    malformed->CmdBuffer[0].IdxOffset = std::numeric_limits<unsigned int>::max();
    expect(!compositor.enqueue(nullptr, *malformed, 0, 0, 16, 16).textureId, "Canvas accepted an out-of-range command index offset");
    malformed->CmdBuffer[0] = originalCommand;
    malformed->CmdBuffer[0].ElemCount = std::numeric_limits<unsigned int>::max();
    expect(!compositor.enqueue(nullptr, *malformed, 0, 0, 16, 16).textureId, "Canvas accepted an overflowing command index range");
    malformed->CmdBuffer[0] = originalCommand;
    malformed->CmdBuffer[0].VtxOffset = std::numeric_limits<unsigned int>::max();
    expect(!compositor.enqueue(nullptr, *malformed, 0, 0, 16, 16).textureId, "Canvas accepted an out-of-range command vertex offset");
    malformed->CmdBuffer[0] = originalCommand;
    const auto originalIndex = malformed->IdxBuffer[0];
    malformed->IdxBuffer[0] = static_cast<ImDrawIdx>(malformed->VtxBuffer.Size);
    expect(!compositor.enqueue(nullptr, *malformed, 0, 0, 16, 16).textureId, "Canvas accepted an index past its vertex buffer");
    malformed->IdxBuffer[0] = originalIndex;
    expect(!compositor.pending(), "Rejected captures left queued render jobs");
    for (int index = 0; index < 32; ++index)
        expect(compositor.enqueue(nullptr, *limits, 0, 0, 16, 16).textureId != nullptr, "Canvas rejected a valid layer below its bounded limit");
    expect(!compositor.enqueue(nullptr, *limits, 0, 0, 16, 16).textureId, "Canvas exceeded its 32-layer frame limit");
    ImGui::Render(); compositor.render(context); wait(context, fence, ++sequence); context->FinishFrame();

    // Release both completed slots before exhausting the shared target cache.
    // Every 2048-square layer owns two RGBA8 targets: exactly 32 MiB. Earlier
    // small targets must be evicted for two such layers to fit in 64 MiB.
    compositor.beginFrame(0); compositor.beginFrame(1);
    begin(compositor, renderer, 0);
    malformed->CmdBuffer[0].IdxOffset = std::numeric_limits<unsigned int>::max();
    expect(!compositor.enqueue(nullptr, *malformed, 0, 0, 2048, 2048).textureId, "Canvas accepted a malformed maximum-size capture");
    malformed->CmdBuffer[0] = originalCommand;
    auto maximum = drawList(whiteView, {}, {2048, 2048});
    rectangle(*maximum, {}, {2048, 2048}, IM_COL32(255, 0, 0, 255));
    const auto firstMaximum = compositor.enqueue(nullptr, *maximum, 0, 0, 2048, 2048);
    expect(firstMaximum.textureId != nullptr, "Canvas cache rejected its first 32 MiB layer");
    for (auto& vertex : maximum->VtxBuffer) vertex.col = IM_COL32(0, 255, 0, 255);
    const auto secondMaximum = compositor.enqueue(nullptr, *maximum, 0, 0, 2048, 2048);
    expect(secondMaximum.textureId != nullptr && secondMaximum.textureId != firstMaximum.textureId,
           "Canvas cache did not fit two independent 32 MiB layers after evicting completed targets");
    expect(!compositor.enqueue(nullptr, *maximum, 0, 0, 2048, 2048).textureId, "Canvas target cache exceeded its 64 MiB budget");
    ImGui::Render(); compositor.render(context);
    {
        const auto firstPixels = readback(device, context, layerTexture(firstMaximum), fence, sequence);
        pixel(firstPixels.at(2047, 2047), {255, 0, 0, 255}, "First maximum canvas was overwritten by a later layer");
    }
    {
        const auto secondPixels = readback(device, context, layerTexture(secondMaximum), fence, sequence);
        pixel(secondPixels.at(2047, 2047), {0, 255, 0, 255}, "Second maximum canvas did not render its complete target");
    }
    context->FinishFrame();

    begin(compositor, renderer, 1);
    expect(!compositor.enqueue(nullptr, *limits, 0, 0, 16, 16).textureId,
           "Canvas cache evicted targets still pinned by the other frame slot");
    ImGui::Render(); context->FinishFrame();
    begin(compositor, renderer, 0);
    const auto recovered = compositor.enqueue(nullptr, *limits, 0, 0, 16, 16);
    expect(recovered.textureId != nullptr, "Canvas cache did not recover capacity after releasing its completed frame pins");
    ImGui::Render(); compositor.render(context);
    const auto recoveredPixels = readback(device, context, layerTexture(recovered), fence, sequence);
    pixel(recoveredPixels.at(8, 8), {255, 255, 255, 255}, "Canvas cache recovery returned stale target contents");
    context->FinishFrame();
}
} // namespace

int main(int argc, char** argv) {
    try {
        const Diagnostics diagnostics;
        const std::string mode = argc > 1 ? argv[1] : "d3d12";
        engine::core::Log::init();
        {
            engine::platform::WindowDesc windowDesc;
            windowDesc.width = windowDesc.height = 64;
            windowDesc.title = "CanvasGroup GPU composition tests"; windowDesc.useOpenGLContext = false;
            auto window = engine::platform::Window::create(windowDesc);
            glfwHideWindow(static_cast<GLFWwindow*>(window->nativeHandle()));
            engine::rhi::DeviceCreateDesc desc;
            desc.window = window.get(); desc.diligentDeviceType = mode; desc.enableValidation = true;
            engine::rhi::diligent::DiligentDevice device(desc);
            verify(device.nativeRenderDevice(), device.nativeImmediateContext());
        }
        expect(validationErrors.load(std::memory_order_relaxed) == 0, "Diligent reported CanvasGroup validation errors");
        std::cout << "CANVAS_GPU_READBACK_PASS backend=" << mode
                  << " opaque_overlap=once straight_alpha_edge=verified nested_layers=verified resize_fences=verified limits=verified cache_64MiB=verified malformed_captures=rejected validation_errors=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Canvas GPU test failure: " << error.what() << '\n';
        return 1;
    }
}
