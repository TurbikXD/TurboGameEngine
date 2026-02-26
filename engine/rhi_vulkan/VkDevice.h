#pragma once

#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "engine/rhi/Device.h"

namespace engine::rhi::vk {

class VkGraphicsQueue final : public IQueue {
public:
    void submit(
        ICommandBuffer& commandBuffer,
        ISemaphore* waitSemaphore,
        ISemaphore* signalSemaphore,
        IFence* signalFence) override;
};

class VkDevice final : public IDevice {
public:
    explicit VkDevice(const DeviceCreateDesc& desc);
    ~VkDevice() override = default;

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
    ::VkInstance m_instance{VK_NULL_HANDLE};
    ::VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    ::VkDevice m_logicalDevice{VK_NULL_HANDLE};
    VkGraphicsQueue m_queue{};
};

std::unique_ptr<IDevice> createVulkanDevice(
    const DeviceCreateDesc& desc,
    std::string* errorMessage = nullptr);

} // namespace engine::rhi::vk
