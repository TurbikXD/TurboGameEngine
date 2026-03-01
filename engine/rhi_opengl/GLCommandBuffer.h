#pragma once

#include <cstdint>

#include "engine/rhi/CommandBuffer.h"

namespace engine::rhi::gl {

class GLGraphicsPipeline;

class GLCommandBuffer final : public ICommandBuffer {
public:
    GLCommandBuffer();
    ~GLCommandBuffer() override;

    void begin() override;
    void end() override;
    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;
    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect2D& scissor) override;
    void bindPipeline(IGraphicsPipeline& pipeline) override;
    void bindVertexBuffer(IBuffer& buffer) override;
    void bindIndexBuffer(IBuffer& buffer) override;
    void bindImage(std::uint32_t slot, IImage& image) override;
    void bindBindGroup(std::uint32_t slot, IBindGroup& bindGroup) override;
    void pushConstants(const void* data, std::size_t size) override;
    void draw(std::uint32_t vertexCount, std::uint32_t firstVertex) override;
    void drawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex) override;
    void barrier(const BarrierDesc& barrierDesc) override;

private:
    bool m_recording{false};
    bool m_renderPassActive{false};
    std::uint32_t m_vaoId{0};
    const GLGraphicsPipeline* m_boundPipeline{nullptr};
    bool m_indexBufferBound{false};
};

} // namespace engine::rhi::gl
