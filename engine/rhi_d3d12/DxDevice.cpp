#include "engine/rhi_d3d12/DxDevice.h"

#include <stdexcept>

#include "engine/core/Log.h"
#include "engine/rhi_d3d12/DxCommandBuffer.h"
#include "engine/rhi_d3d12/DxPipeline.h"
#include "engine/rhi_d3d12/DxResources.h"
#include "engine/rhi_d3d12/DxSwapchain.h"
#include "engine/rhi_d3d12/DxSync.h"

namespace engine::rhi::dx12 {

DxDevice::DxDevice(const DeviceCreateDesc& desc) : m_window(desc.window) {
    if (m_window == nullptr) {
        throw std::runtime_error("D3D12 backend requires a valid window");
    }
    ENGINE_LOG_WARN("D3D12 backend is compile-ready but runtime functionality is not implemented yet");
}

std::unique_ptr<ISwapchain> DxDevice::createSwapchain(const SwapchainDesc& desc) {
    return std::make_unique<DxSwapchain>(desc);
}

std::unique_ptr<ICommandBuffer> DxDevice::createCommandBuffer() {
    return std::make_unique<DxCommandBuffer>();
}

std::unique_ptr<IBuffer> DxDevice::createBuffer(const BufferDesc& desc, const void* initialData) {
    (void)initialData;
    return std::make_unique<DxBufferResource>(desc);
}

std::unique_ptr<IShaderModule> DxDevice::createShaderModule(const ShaderModuleDesc& desc) {
    return std::make_unique<DxShaderModuleResource>(desc);
}

std::unique_ptr<IPipelineLayout> DxDevice::createPipelineLayout() {
    return std::make_unique<DxPipelineLayoutResource>();
}

std::unique_ptr<IGraphicsPipeline> DxDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    if (desc.vertexShader == nullptr || desc.fragmentShader == nullptr) {
        return nullptr;
    }
    return std::make_unique<DxGraphicsPipelineResource>(desc.topology);
}

std::unique_ptr<IBindGroupLayout> DxDevice::createBindGroupLayout() {
    return std::make_unique<DxBindGroupLayoutResource>();
}

std::unique_ptr<IBindGroup> DxDevice::createBindGroup() {
    return std::make_unique<DxBindGroupResource>();
}

std::unique_ptr<ISemaphore> DxDevice::createSemaphore() {
    return std::make_unique<DxSemaphoreSync>();
}

std::unique_ptr<IFence> DxDevice::createFence(bool signaled) {
    return std::make_unique<DxFenceSync>(signaled);
}

IQueue& DxDevice::graphicsQueue() {
    return m_queue;
}

BackendType DxDevice::backendType() const {
    return BackendType::D3D12;
}

void DxGraphicsQueue::submit(
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

std::unique_ptr<IDevice> createD3D12Device(const DeviceCreateDesc& desc, std::string* errorMessage) {
    try {
        return std::make_unique<DxDevice>(desc);
    } catch (const std::exception& ex) {
        if (errorMessage != nullptr) {
            *errorMessage = ex.what();
        }
        return nullptr;
    }
}

} // namespace engine::rhi::dx12
