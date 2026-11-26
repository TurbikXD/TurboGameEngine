#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

#include <GLFW/glfw3.h>

#include "engine/core/JobSystem.h"
#include "engine/core/Log.h"
#include "engine/platform/Window.h"
#include "engine/resources/image_mips.h"
#include "engine/rhi/CommandBuffer.h"
#include "engine/rhi/UploadQueue.h"
#include "engine/rhi_diligent/DiligentDevice.h"
#include "third_party/DiligentEngine/DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Buffer.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "third_party/DiligentEngine/DiligentCore/Graphics/GraphicsEngine/interface/Fence.h"
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

void verifyGraphicsFrameFences(engine::rhi::diligent::DiligentDevice& device) {
    using namespace Diligent;
    auto* native = device.nativeRenderDevice();
    auto* context = device.nativeImmediateContext();
    if (native->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12 &&
        native->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_VULKAN) { return; }
    expect(native->GetDeviceInfo().Features.NativeFence == DEVICE_FEATURE_STATE_ENABLED,
           "Graphics fence gate requires the enabled native fence feature");
    FenceDesc gateDesc;
    gateDesc.Name = "Graphics frame fence regression gate";
    gateDesc.Type = FENCE_TYPE_GENERAL;
    RefCntAutoPtr<Diligent::IFence> gate;
    native->CreateFence(gateDesc, &gate);
    FenceDesc markerDesc;
    markerDesc.Name = "Graphics frame fence completion marker";
    RefCntAutoPtr<Diligent::IFence> marker;
    native->CreateFence(markerDesc, &marker);
    expect(gate && marker, "Cannot allocate graphics fence regression synchronization");
    std::array<std::uint32_t, 64> expected{};
    engine::rhi::BufferDesc sourceDesc;
    sourceDesc.size = sizeof(expected);
    auto source = device.createBuffer(sourceDesc, nullptr);
    expect(source != nullptr, "Cannot allocate graphics fence source buffer");
    auto* sourceNative = reinterpret_cast<Diligent::IBuffer*>(static_cast<std::uintptr_t>(source->handle()));
    auto stagingDesc = sourceNative->GetDesc();
    stagingDesc.Name = "Graphics frame fence regression readback";
    stagingDesc.BindFlags = BIND_NONE;
    stagingDesc.Usage = USAGE_STAGING;
    stagingDesc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<Diligent::IBuffer> staging;
    native->CreateBuffer(stagingDesc, nullptr, &staging);
    expect(staging != nullptr, "Cannot allocate graphics fence staging buffer");
    auto commands = device.createCommandBuffer();
    expect(commands != nullptr, "Cannot allocate graphics fence command buffer");
    std::uint64_t value = 0;
    for (const bool initiallySignaled : {true, false}) {
        auto fence = device.createFence(initiallySignaled);
        expect(fence != nullptr, "Cannot create RHI graphics fence");
        if (initiallySignaled) { fence->wait(); }
        for (int iteration = 0; iteration < 3; ++iteration) {
            ++value;
            if (initiallySignaled || iteration > 0) { fence->reset(); }
            for (std::size_t index = 0; index < expected.size(); ++index) {
                expected[index] = static_cast<std::uint32_t>(value * 1000U + index);
            }
            std::atomic_bool entered{false}, returned{false};
            std::exception_ptr failure;
            std::jthread waiter;
            struct GateRelease final {
                Diligent::IFence* gate;
                std::uint64_t value;
                bool released{false};
                void release() { if (!released) { gate->Signal(value); released = true; } }
                ~GateRelease() { release(); }
            } release{gate, value};
            commands->begin();
            context->DeviceWaitForFence(gate, value);
            context->UpdateBuffer(sourceNative, 0, sizeof(expected), expected.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->CopyBuffer(sourceNative, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                staging, 0, sizeof(expected), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->EnqueueSignal(marker, value);
            commands->end();
            device.graphicsQueue().submit(*commands, nullptr, nullptr, fence.get());
            waiter = std::jthread([&]() {
                entered.store(true, std::memory_order_release);
                try { fence->wait(); }
                catch (...) { failure = std::current_exception(); }
                returned.store(true, std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!entered.load(std::memory_order_acquire)) {
                expect(std::chrono::steady_clock::now() < deadline, "Graphics fence CPU waiter never started");
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const bool returnedBeforeGpu = returned.load(std::memory_order_acquire);
            const bool markerBlocked = marker->GetCompletedValue() < value;
            release.release();
            waiter.join();
            if (failure) { std::rethrow_exception(failure); }
            if (!markerBlocked || returnedBeforeGpu) {
                std::cerr << "FRAME_FENCE_GATE initially_signaled=" << initiallySignaled
                          << " iteration=" << iteration << " value=" << value
                          << " marker_blocked=" << markerBlocked << " wait_returned=" << returnedBeforeGpu << '\n';
            }
            expect(markerBlocked, "Native GPU gate did not block the graphics completion marker");
            expect(!returnedBeforeGpu,
                   "RHI frame fence wait returned while the submitted GPU commands were blocked");
            expect(marker->GetCompletedValue() >= value,
                   "RHI frame fence did not cover commands recorded before queue submission");
            void* mapped = nullptr;
            context->MapBuffer(staging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, mapped);
            expect(mapped != nullptr, "Frame fence wait did not make staging data readable without Present or WaitForIdle");
            const bool correct = std::memcmp(mapped, expected.data(), sizeof(expected)) == 0;
            context->UnmapBuffer(staging, MAP_READ);
            expect(correct, "Graphics fence reuse exposed stale GPU buffer contents");
            context->FinishFrame();
        }
    }
    auto directFence = device.createFence(false);
    context->EnqueueSignal(marker, ++value);
    directFence->signal();
    directFence->wait();
    expect(marker->GetCompletedValue() >= value, "Direct RHI fence signal did not flush pending GPU commands");
    context->FinishFrame();
}

void verifyReadback(engine::rhi::diligent::DiligentDevice& device,
                    engine::rhi::UploadSubmission& result, std::uint32_t seed) {
    using namespace Diligent;
    auto* context = device.nativeImmediateContext();
    auto* native = device.nativeRenderDevice();
    const bool d3d12 = native->GetDeviceInfo().Type == RENDER_DEVICE_TYPE_D3D12;
    expect(result.ticket && result.ticket->isComplete(), "GPU upload never completed");
    result.ticket->acquire();
    result.ticket->acquire();
    auto readbackFence = device.createFence(true);
    expect(readbackFence != nullptr, "Cannot allocate GPU readback completion fence");

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
        readbackFence->reset();
        readbackFence->signal();
        readbackFence->wait();
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
    readbackFence->reset();
    readbackFence->signal();
    readbackFence->wait();
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

void verifyRepeatedUploadTicketWaits(engine::rhi::diligent::DiligentDevice& device) {
    using namespace Diligent;
    auto* native = device.nativeRenderDevice();
    if (native->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12 &&
        native->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_VULKAN) { return; }
    auto* transfer = device.nativeUploadContext();
    auto* uploads = device.uploadQueue();
    expect(transfer && uploads, "Repeated upload wait regression has no transfer context");
    FenceDesc gateDesc;
    gateDesc.Name = "Repeated upload ticket regression gate";
    gateDesc.Type = FENCE_TYPE_GENERAL;
    RefCntAutoPtr<Diligent::IFence> gate;
    native->CreateFence(gateDesc, &gate);
    expect(gate != nullptr, "Cannot allocate repeated upload wait gate");
    for (std::uint64_t value = 1; value <= 4; ++value) {
        const auto seed = static_cast<std::uint32_t>(100U + value);
        std::array<std::array<std::uint32_t, 64>, 2> data{};
        std::array<engine::rhi::BufferUpload, 2> buffers{};
        for (std::size_t index = 0; index < buffers.size(); ++index) {
            for (std::uint32_t element = 0; element < 64; ++element) {
                data[index][element] = seed + static_cast<std::uint32_t>(index) * 1000U + element;
            }
            buffers[index].desc.size = sizeof(data[index]);
            buffers[index].desc.usage = index == 0 ? engine::rhi::BufferUsage::Vertex : engine::rhi::BufferUsage::Index;
            buffers[index].data = std::as_bytes(std::span{data[index]});
        }
        const std::vector<std::uint8_t> pixels(37U * 19U * 4U, static_cast<std::uint8_t>(seed));
        const auto tail = engine::resources::buildRgba8MipTail(37, 19, pixels);
        std::vector<engine::rhi::ImageMipUpload> mips{{37, 19, pixels}};
        for (const auto& mip : tail) { mips.push_back({mip.width, mip.height, mip.pixels}); }
        engine::rhi::UploadRequest request;
        request.buffers = buffers;
        request.imageDesc = {37, 19, engine::rhi::ImageFormat::RGBA8, true};
        request.imageMips = mips;
        std::atomic_bool entered{false}, returned{false};
        std::exception_ptr failure;
        engine::rhi::UploadSubmission result;
        std::jthread waiter;
        struct GateRelease final {
            Diligent::IFence* gate;
            std::uint64_t value;
            bool released{false};
            void release() { if (!released) { gate->Signal(value); released = true; } }
            ~GateRelease() { release(); }
        } release{gate, value};
        transfer->DeviceWaitForFence(gate, value);
        result = uploads->submit(request);
        waiter = std::jthread([&]() {
            entered.store(true, std::memory_order_release);
            try {
                if (value == 4) { uploads->waitIdle(); }
                else { result.ticket->wait(); }
            } catch (...) { failure = std::current_exception(); }
            returned.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!entered.load(std::memory_order_acquire)) {
            expect(std::chrono::steady_clock::now() < deadline, "Upload CPU waiter never started");
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool returnedBeforeGpu = returned.load(std::memory_order_acquire);
        const bool uploadBlocked = !result.ticket->isComplete();
        release.release();
        waiter.join();
        if (failure) { std::rethrow_exception(failure); }
        if (!uploadBlocked || returnedBeforeGpu) {
            std::cerr << "UPLOAD_WAIT_GATE iteration=" << value << " fence=" << result.ticket->value()
                      << " upload_blocked=" << uploadBlocked << " wait_returned=" << returnedBeforeGpu << '\n';
        }
        expect(uploadBlocked, "Native GPU gate did not block the transfer upload");
        expect(!returnedBeforeGpu, "Upload wait or waitIdle returned before transfer GPU completion");
        expect(result.ticket->isComplete(), "Upload wait did not reach its GPU timeline value");
        verifyReadback(device, result, seed);
    }
}
}

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
            verifyGraphicsFrameFences(device);
            verifyRepeatedUploadTicketWaits(device);
        }
        expect(validationErrors.load(std::memory_order_relaxed) == 0U,
               "Diligent reported validation errors or Debug assertions");
        std::cout << "GPU_UPLOAD_READBACK_PASS backend=" << mode
                  << " batches=16 buffers=32 all_texture_mips=verified unique_fences=16 graphics_frame_fences=verified repeated_upload_waits=verified validation_errors=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GPU upload test failure: " << error.what() << '\n';
        return 1;
    }
}
