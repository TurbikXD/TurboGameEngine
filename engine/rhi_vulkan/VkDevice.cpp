#include "engine/rhi_vulkan/VkDevice.h"

#include <stdexcept>

#include "engine/core/Log.h"
#include "engine/rhi_vulkan/VkCommandBuffer.h"
#include "engine/rhi_vulkan/VkPipeline.h"
#include "engine/rhi_vulkan/VkResources.h"
#include "engine/rhi_vulkan/VkSwapchain.h"
#include "engine/rhi_vulkan/VkSync.h"

namespace engine::rhi::vk {

VkDevice::VkDevice(const DeviceCreateDesc& desc) : m_window(desc.window) {
    if (m_window == nullptr) {
        throw std::runtime_error("Vulkan backend requires a window");
    }
    ENGINE_LOG_WARN("Vulkan backend is compile-ready but runtime functionality is not implemented yet");
}

std::unique_ptr<ISwapchain> VkDevice::createSwapchain(const SwapchainDesc& desc) {
    return std::make_unique<VkSwapchain>(desc);
}

std::unique_ptr<ICommandBuffer> VkDevice::createCommandBuffer() {
    return std::make_unique<VkCommandBuffer>();
}

std::unique_ptr<IBuffer> VkDevice::createBuffer(const BufferDesc& desc, const void* initialData) {
    (void)initialData;
    return std::make_unique<VkBufferResource>(desc);
}

std::unique_ptr<IImage> VkDevice::createImage(const ImageDesc& desc, const void* initialData) {
    (void)initialData;
    return std::make_unique<VkImageResource>(desc);
}

std::unique_ptr<IShaderModule> VkDevice::createShaderModule(const ShaderModuleDesc& desc) {
    return std::make_unique<VkShaderModuleResource>(desc);
}

std::unique_ptr<IPipelineLayout> VkDevice::createPipelineLayout() {
    return std::make_unique<VkPipelineLayoutResource>();
}

std::unique_ptr<IGraphicsPipeline> VkDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    if (desc.vertexShader == nullptr || desc.fragmentShader == nullptr) {
        return nullptr;
    }
    return std::make_unique<VkGraphicsPipelineResource>(desc.topology);
}

std::unique_ptr<IBindGroupLayout> VkDevice::createBindGroupLayout() {
    return std::make_unique<VkBindGroupLayoutResource>();
}

std::unique_ptr<IBindGroup> VkDevice::createBindGroup() {
    return std::make_unique<VkBindGroupResource>();
}

std::unique_ptr<ISemaphore> VkDevice::createSemaphore() {
    return std::make_unique<VkSemaphoreSync>();
}

std::unique_ptr<IFence> VkDevice::createFence(bool signaled) {
    return std::make_unique<VkFenceSync>(signaled);
}

IQueue& VkDevice::graphicsQueue() {
    return m_queue;
}

BackendType VkDevice::backendType() const {
    return BackendType::Vulkan;
}

void VkGraphicsQueue::submit(
    ICommandBuffer& commandBuffer,
    ISemaphore* waitSemaphore,
    ISemaphore* signalSemaphore,
    IFence* signalFence) {
    (void)commandBuffer;
    (void)waitSemaphore;
    (void)signalSemaphore;
    if (signalFence != nullptr) {
        signalFence->signal();
    }
}

std::unique_ptr<IDevice> createVulkanDevice(const DeviceCreateDesc& desc, std::string* errorMessage) {
    try {
        return std::make_unique<VkDevice>(desc);
    } catch (const std::exception& ex) {
        if (errorMessage != nullptr) {
            *errorMessage = ex.what();
        }
        return nullptr;
    }
}

} // namespace engine::rhi::vk
