#include "engine/rhi_d3d12/DxCommandBuffer.h"

namespace engine::rhi::dx12 {

void DxCommandBuffer::begin() {}

void DxCommandBuffer::end() {}

void DxCommandBuffer::beginRenderPass(const RenderPassDesc& desc) {
    (void)desc;
}

void DxCommandBuffer::endRenderPass() {}

void DxCommandBuffer::setViewport(const Viewport& viewport) {
    (void)viewport;
}

void DxCommandBuffer::setScissor(const Rect2D& scissor) {
    (void)scissor;
}

void DxCommandBuffer::bindPipeline(IGraphicsPipeline& pipeline) {
    (void)pipeline;
}

void DxCommandBuffer::bindVertexBuffer(IBuffer& buffer) {
    (void)buffer;
}

void DxCommandBuffer::bindIndexBuffer(IBuffer& buffer) {
    (void)buffer;
}

void DxCommandBuffer::bindImage(std::uint32_t slot, IImage& image) {
    (void)slot;
    (void)image;
}

void DxCommandBuffer::bindBindGroup(std::uint32_t slot, IBindGroup& bindGroup) {
    (void)slot;
    (void)bindGroup;
}

void DxCommandBuffer::pushConstants(const void* data, std::size_t size) {
    (void)data;
    (void)size;
}

void DxCommandBuffer::draw(std::uint32_t vertexCount, std::uint32_t firstVertex) {
    (void)vertexCount;
    (void)firstVertex;
}

void DxCommandBuffer::drawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex) {
    (void)indexCount;
    (void)firstIndex;
}

void DxCommandBuffer::barrier(const BarrierDesc& barrierDesc) {
    (void)barrierDesc;
}

} // namespace engine::rhi::dx12
