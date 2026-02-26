#pragma once

#include <memory>
#include <string>

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

#include "engine/rhi/Device.h"

namespace engine::rhi::dx12 {

class DxGraphicsQueue final : public IQueue {
public:
    void submit(
        ICommandBuffer& commandBuffer,
        ISemaphore* waitSemaphore,
        ISemaphore* signalSemaphore,
        IFence* signalFence) override;
};

class DxDevice final : public IDevice {
public:
    explicit DxDevice(const DeviceCreateDesc& desc);
    ~DxDevice() override = default;

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
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
#endif
    DxGraphicsQueue m_queue{};
};

std::unique_ptr<IDevice> createD3D12Device(
    const DeviceCreateDesc& desc,
    std::string* errorMessage = nullptr);

} // namespace engine::rhi::dx12
