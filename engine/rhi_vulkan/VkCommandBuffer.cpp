#include "engine/rhi_vulkan/VkCommandBuffer.h"

namespace engine::rhi::vk {

void VkCommandBuffer::begin() {}

void VkCommandBuffer::end() {}

void VkCommandBuffer::beginRenderPass(const RenderPassDesc& desc) {
    (void)desc;
}

void VkCommandBuffer::endRenderPass() {}

void VkCommandBuffer::setViewport(const Viewport& viewport) {
    (void)viewport;
}

void VkCommandBuffer::setScissor(const Rect2D& scissor) {
    (void)scissor;
}

void VkCommandBuffer::bindPipeline(IGraphicsPipeline& pipeline) {
    (void)pipeline;
}

void VkCommandBuffer::bindVertexBuffer(IBuffer& buffer) {
    (void)buffer;
}

void VkCommandBuffer::bindIndexBuffer(IBuffer& buffer) {
    (void)buffer;
}

void VkCommandBuffer::bindImage(std::uint32_t slot, IImage& image) {
    (void)slot;
    (void)image;
}

void VkCommandBuffer::bindBindGroup(std::uint32_t slot, IBindGroup& bindGroup) {
    (void)slot;
    (void)bindGroup;
}

void VkCommandBuffer::pushConstants(const void* data, std::size_t size) {
    (void)data;
    (void)size;
}

void VkCommandBuffer::draw(std::uint32_t vertexCount, std::uint32_t firstVertex) {
    (void)vertexCount;
    (void)firstVertex;
}

void VkCommandBuffer::drawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex) {
    (void)indexCount;
    (void)firstIndex;
}

void VkCommandBuffer::barrier(const BarrierDesc& barrierDesc) {
    (void)barrierDesc;
}

} // namespace engine::rhi::vk
