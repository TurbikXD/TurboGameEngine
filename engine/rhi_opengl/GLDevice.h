#pragma once

#include <memory>
#include <string>

#include "engine/rhi/Device.h"

namespace engine::rhi::gl {

class GLQueue final : public IQueue {
public:
    void submit(
        ICommandBuffer& commandBuffer,
        ISemaphore* waitSemaphore,
        ISemaphore* signalSemaphore,
        IFence* signalFence) override;
};

class GLDevice final : public IDevice {
public:
    explicit GLDevice(const DeviceCreateDesc& desc);
    ~GLDevice() override = default;

    std::unique_ptr<ISwapchain> createSwapchain(const SwapchainDesc& desc) override;
    std::unique_ptr<ICommandBuffer> createCommandBuffer() override;
    std::unique_ptr<IBuffer> createBuffer(const BufferDesc& desc, const void* initialData) override;
    std::unique_ptr<IShaderModule> createShaderModule(const ShaderModuleDesc& desc) override;
    std::unique_ptr<IPipelineLayout> createPipelineLayout() override;
    std::unique_ptr<IGraphicsPipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    std::unique_ptr<IBindGroupLayout> createBindGroupLayout() override;
    std::unique_ptr<IBindGroup> createBindGroup() override;
    std::unique_ptr<ISemaphore> createSemaphore() override;
    std::unique_ptr<IFence> createFence(bool signaled) override;
    IQueue& graphicsQueue() override;
    BackendType backendType() const override;

private:
    platform::Window* m_window{nullptr};
    GLQueue m_queue{};
};

std::unique_ptr<IDevice> createOpenGLDevice(
    const DeviceCreateDesc& desc,
    std::string* errorMessage = nullptr);

} // namespace engine::rhi::gl
