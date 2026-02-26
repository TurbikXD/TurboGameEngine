#pragma once

#include <memory>

#include "engine/rhi/Sync.h"
#include "engine/rhi/Types.h"

namespace engine::rhi {

class ICommandBuffer;
class ISwapchain;
class IBuffer;
class IShaderModule;
class IPipelineLayout;
class IGraphicsPipeline;
class IBindGroupLayout;
class IBindGroup;

class IQueue {
public:
    virtual ~IQueue() = default;
    virtual void submit(
        ICommandBuffer& commandBuffer,
        ISemaphore* waitSemaphore,
        ISemaphore* signalSemaphore,
        IFence* signalFence) = 0;
};

class IDevice {
public:
    virtual ~IDevice() = default;

    virtual std::unique_ptr<ISwapchain> createSwapchain(const SwapchainDesc& desc) = 0;
    virtual std::unique_ptr<ICommandBuffer> createCommandBuffer() = 0;
    virtual std::unique_ptr<IBuffer> createBuffer(const BufferDesc& desc, const void* initialData) = 0;
    virtual std::unique_ptr<IShaderModule> createShaderModule(const ShaderModuleDesc& desc) = 0;
    virtual std::unique_ptr<IPipelineLayout> createPipelineLayout() = 0;
    virtual std::unique_ptr<IGraphicsPipeline> createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual std::unique_ptr<IBindGroupLayout> createBindGroupLayout() = 0;
    virtual std::unique_ptr<IBindGroup> createBindGroup() = 0;
    virtual std::unique_ptr<ISemaphore> createSemaphore() = 0;
    virtual std::unique_ptr<IFence> createFence(bool signaled) = 0;
    virtual IQueue& graphicsQueue() = 0;
    virtual BackendType backendType() const = 0;
};

} // namespace engine::rhi
