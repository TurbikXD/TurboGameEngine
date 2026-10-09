#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

#include <GLFW/glfw3.h>

#include "engine/core/JobSystem.h"
#include "engine/core/Log.h"
#include "engine/platform/Window.h"
#include "engine/resources/image_mips.h"
#include "engine/rhi/UploadQueue.h"
#include "engine/rhi_diligent/DiligentDevice.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Texture.h"
#include "third_party/DiligentEngine/DiligentCore/Platforms/Basic/interface/BasicPlatformDebug.hpp"
#include "third_party/DiligentEngine/DiligentCore/Primitives/interface/DebugOutput.h"

namespace {
std::atomic_size_t validationErrors{0};
Diligent::DebugMessageCallbackType previousDebugCallback = nullptr;

void DILIGENT_CALL_TYPE captureDiagnostic(Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                        const Diligent::Char* message, const Diligent::Char* function,
                                        const Diligent::Char* file, const int line) {
    if (severity >= Diligent::DEBUG_MESSAGE_SEVERITY_ERROR) {
        validationErrors.fetch_add(1U, std::memory_order_relaxed);
    }
    if (previousDebugCallback != nullptr) { previousDebugCallback(severity, message, function, file, line); }
}

class Diagnostics final {
public:
    Diagnostics() : breakOnError(Diligent::BasicPlatformDebug::GetBreakOnError()) {
        previousDebugCallback = Diligent::DebugMessageCallback;
        // Tests must fail on diagnostics, not hang on a modal Debug assertion dialog.
        Diligent::BasicPlatformDebug::SetBreakOnError(false);
        Diligent::SetDebugMessageCallback(captureDiagnostic);
    }
    ~Diagnostics() {
        Diligent::SetDebugMessageCallback(previousDebugCallback);
        Diligent::BasicPlatformDebug::SetBreakOnError(breakOnError);
    }
private:
    bool breakOnError;
};

void expect(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void verifyReadback(engine::rhi::diligent::DiligentDevice& device,
                    engine::rhi::UploadSubmission& result, std::uint32_t seed) {
    using namespace Diligent;
    auto* context = device.nativeImmediateContext();
    auto* native = device.nativeRenderDevice();
    const bool d3d12 = native->GetDeviceInfo().Type == RENDER_DEVICE_TYPE_D3D12;
    expect(result.ticket && result.ticket->isComplete(), "GPU upload never completed");
    result.ticket->acquire();
    result.ticket->acquire(); // idempotent: no second binary semaphore consumption

    for (std::size_t index = 0; index < result.buffers.size(); ++index) {
        auto* source = reinterpret_cast<Diligent::IBuffer*>(static_cast<std::uintptr_t>(result.buffers[index]->handle()));
        expect(!d3d12 || source->GetState() == RESOURCE_STATE_COMMON,
               "D3D12 upload buffer was not released to COMMON");
        auto desc = source->GetDesc();
        desc.Name = "Upload test buffer readback";
        desc.BindFlags = BIND_NONE;
        desc.Usage = USAGE_STAGING;
        desc.CPUAccessFlags = CPU_ACCESS_READ;
        desc.ImmediateContextMask = 1;
        RefCntAutoPtr<Diligent::IBuffer> staging;
        native->CreateBuffer(desc, nullptr, &staging);
        expect(staging != nullptr, "Buffer readback allocation failed");
        context->CopyBuffer(source, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
            staging, 0, desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->WaitForIdle(); // tests only, never the runtime loading path
        void* mapped = nullptr;
        context->MapBuffer(staging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, mapped);
        expect(mapped != nullptr, "Buffer readback map failed");
        bool correct = true;
        for (std::uint32_t element = 0; element < 64; ++element) {
            std::uint32_t value = 0;
            std::memcpy(&value, static_cast<const std::byte*>(mapped) + element * sizeof(value), sizeof(value));
            correct &= value == seed + static_cast<std::uint32_t>(index) * 1000U + element;
        }
        context->UnmapBuffer(staging, MAP_READ);
        expect(correct, "Uploaded vertex/index GPU bytes differ from CPU data");
    }

    auto* source = reinterpret_cast<Diligent::ITexture*>(static_cast<std::uintptr_t>(result.image->handle()));
    expect(!d3d12 || source->GetState() == RESOURCE_STATE_COMMON,
           "D3D12 upload texture was not released to COMMON");
    auto desc = source->GetDesc();
    desc.Name = "Upload test texture readback";
    desc.BindFlags = BIND_NONE;
    desc.Usage = USAGE_STAGING;
    desc.CPUAccessFlags = CPU_ACCESS_READ;
    desc.ImmediateContextMask = 1;
    RefCntAutoPtr<Diligent::ITexture> staging;
    native->CreateTexture(desc, nullptr, &staging);
    expect(staging != nullptr, "Texture readback allocation failed");
    for (Uint32 mip = 0; mip < desc.MipLevels; ++mip) {
        CopyTextureAttribs copy{source, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               staging, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
        copy.SrcMipLevel = copy.DstMipLevel = mip;
        context->CopyTexture(copy);
    }
    context->WaitForIdle();
    auto width = desc.Width;
    auto height = desc.Height;
    for (Uint32 mip = 0; mip < desc.MipLevels; ++mip) {
        MappedTextureSubresource mapped;
        context->MapTextureSubresource(staging, mip, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
        expect(mapped.pData != nullptr, "Texture readback map failed");
        bool correct = true;
        for (Uint32 y = 0; y < height; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.Stride;
            for (Uint32 x = 0; x < width * 4U; ++x) { correct &= row[x] == static_cast<std::uint8_t>(seed); }
        }
        context->UnmapTextureSubresource(staging, mip, 0);
        expect(correct, "Uploaded GPU texture mip bytes differ from CPU data");
        width = std::max(1U, width / 2U); height = std::max(1U, height / 2U);
    }
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
            windowDesc.title = "GPU upload readback tests";
            windowDesc.useOpenGLContext = false;
            auto window = engine::platform::Window::create(windowDesc);
            glfwHideWindow(static_cast<GLFWwindow*>(window->nativeHandle()));
            engine::rhi::DeviceCreateDesc desc;
            desc.window = window.get();
            desc.diligentDeviceType = mode;
            desc.enableValidation = true;
            engine::rhi::diligent::DiligentDevice device(desc);
            auto* uploads = device.uploadQueue();
            expect(uploads != nullptr, "This backend/adapter has no dedicated transfer queue");
            engine::core::JobSystem jobs(true, 4);
            std::array<engine::rhi::UploadSubmission, 16> results;
            std::vector<engine::core::JobSystem::Handle> handles;
            for (std::size_t index = 0; index < results.size(); ++index) {
                handles.push_back(jobs.dispatch([&, index]() {
                    const auto seed = static_cast<std::uint32_t>(index + 11U);
                    std::array<std::array<std::uint32_t, 64>, 2> data{};
                    std::array<engine::rhi::BufferUpload, 2> buffers{};
                    for (std::size_t buffer = 0; buffer < 2; ++buffer) {
                        for (std::uint32_t element = 0; element < 64; ++element) {
                            data[buffer][element] = seed + static_cast<std::uint32_t>(buffer) * 1000U + element;
                        }
                        buffers[buffer].desc.size = sizeof(data[buffer]);
                        buffers[buffer].desc.usage = buffer == 0 ? engine::rhi::BufferUsage::Vertex : engine::rhi::BufferUsage::Index;
                        buffers[buffer].data = std::as_bytes(std::span{data[buffer]});
                    }
                    const std::vector<std::uint8_t> pixels(37 * 19 * 4, static_cast<std::uint8_t>(seed));
                    const auto tail = engine::resources::buildRgba8MipTail(37, 19, pixels);
                    std::vector<engine::rhi::ImageMipUpload> mips{{37, 19, pixels}};
                    for (const auto& mip : tail) { mips.push_back({mip.width, mip.height, mip.pixels}); }
                    engine::rhi::UploadRequest request;
                    request.buffers = buffers;
                    request.imageDesc = {37, 19, engine::rhi::ImageFormat::RGBA8, true};
                    request.imageMips = mips;
                    results[index] = uploads->submit(request);
                    // All source data dies here, long before readback on main.
                }));
            }
            for (const auto& handle : handles) { jobs.wait(handle); }
            std::set<std::uint64_t> values;
            for (std::size_t index = 0; index < results.size(); ++index) {
                auto& result = results[index];
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!result.ticket->isComplete()) {
                    expect(std::chrono::steady_clock::now() < deadline, "Upload GPU fence timeout");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                values.insert(result.ticket->value());
                verifyReadback(device, result, static_cast<std::uint32_t>(index + 11U));
            }
            expect(values.size() == results.size() && *values.begin() == 1 && *values.rbegin() == results.size(),
                   "Concurrent uploads lost or duplicated GPU timeline values");
            bool rejected = false;
            try { (void)uploads->submit({}); } catch (const std::invalid_argument&) { rejected = true; }
            expect(rejected, "Empty upload was accepted");
            uploads->waitIdle();
        } // Include context/device destruction in validation-error accounting.
        expect(validationErrors.load(std::memory_order_relaxed) == 0U,
               "Diligent reported validation errors or Debug assertions");
        std::cout << "GPU_UPLOAD_READBACK_PASS backend=" << mode
                  << " batches=16 buffers=32 all_texture_mips=verified unique_fences=16 validation_errors=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU upload test failure: " << error.what() << '\n';
        return 1;
    }
}
