#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/rhi/Types.h"

namespace engine::rhi {

class IGraphicsPipeline;
class IBuffer;
class IImage;
class IBindGroup;

class ICommandBuffer {
public:
    virtual ~ICommandBuffer() = default;

    virtual void begin() = 0;
    virtual void end() = 0;

    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;

    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setScissor(const Rect2D& scissor) = 0;

    virtual void bindPipeline(IGraphicsPipeline& pipeline) = 0;
    virtual void bindVertexBuffer(IBuffer& buffer) = 0;
    virtual void bindIndexBuffer(IBuffer& buffer) = 0;
    virtual void bindImage(std::uint32_t slot, IImage& image) = 0;
    virtual void bindBindGroup(std::uint32_t slot, IBindGroup& bindGroup) = 0;
    virtual void pushConstants(const void* data, std::size_t size) = 0;

    virtual void draw(std::uint32_t vertexCount, std::uint32_t firstVertex) = 0;
    virtual void drawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex) = 0;

    virtual void barrier(const BarrierDesc& barrierDesc) = 0;
};

} // namespace engine::rhi
