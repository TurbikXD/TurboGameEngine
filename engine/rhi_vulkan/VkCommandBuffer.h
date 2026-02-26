#pragma once

#include "engine/rhi/CommandBuffer.h"

namespace engine::rhi::vk {

class VkCommandBuffer final : public ICommandBuffer {
public:
    void begin() override;
    void end() override;
    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;
    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect2D& scissor) override;
    void bindPipeline(IGraphicsPipeline& pipeline) override;
    void bindVertexBuffer(IBuffer& buffer) override;
    void bindIndexBuffer(IBuffer& buffer) override;
    void bindBindGroup(std::uint32_t slot, IBindGroup& bindGroup) override;
    void pushConstants(const void* data, std::size_t size) override;
    void draw(std::uint32_t vertexCount, std::uint32_t firstVertex) override;
    void drawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex) override;
    void barrier(const BarrierDesc& barrierDesc) override;
};

} // namespace engine::rhi::vk
