#pragma once

#include <memory>
#include <string>

#include "engine/rhi/Device.h"

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ISwapChain;
} // namespace Diligent

namespace engine::rhi::diligent {

class DiligentDevice;

class DiligentGraphicsQueue final : public IQueue {
public:
    explicit DiligentGraphicsQueue(DiligentDevice& device);

    void submit(
        ICommandBuffer& commandBuffer,
        ISemaphore* waitSemaphore,
        ISemaphore* signalSemaphore,
        IFence* signalFence) override;

private:
    DiligentDevice* m_device{nullptr};
};

class DiligentDevice final : public IDevice {
public:
    struct Impl;

    explicit DiligentDevice(const DeviceCreateDesc& desc);
    ~DiligentDevice() override;

    std::unique_ptr<ISwapchain> createSwapchain(const SwapchainDesc& desc) override;
    std::unique_ptr<ICommandBuffer> createCommandBuffer() override;
    std::unique_ptr<IBuffer> createBuffer(const BufferDesc& desc, const void* initialData) override;
    std::unique_ptr<IImage> createImage(const ImageDesc& desc, const void* initialData) override;
    std::unique_ptr<IShaderModule> createShaderModule(const ShaderModuleDesc& desc) override;
    std::unique_ptr<IPipelineLayout> createPipelineLayout() override;
    std::unique_ptr<IGraphicsPipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    std::unique_ptr<IBindGroupLayout> createBindGroupLayout() override;
    std::unique_ptr<IBindGroup> createBindGroup() override;
    std::unique_ptr<ISemaphore> createSemaphore() override;
    std::unique_ptr<IFence> createFence(bool signaled) override;
    IQueue& graphicsQueue() override;
    BackendType backendType() const override;

    Diligent::IRenderDevice* nativeRenderDevice() const;
    Diligent::IDeviceContext* nativeImmediateContext() const;
    Diligent::ISwapChain* nativeSwapChain() const;

private:
    std::unique_ptr<Impl> m_impl;
    DiligentGraphicsQueue m_queue;

    friend class DiligentGraphicsQueue;
};

std::unique_ptr<IDevice> createDiligentDevice(
    const DeviceCreateDesc& desc,
    std::string* errorMessage = nullptr);

} // namespace engine::rhi::diligent
