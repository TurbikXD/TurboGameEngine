#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "engine/rhi/Resources.h"
#include "engine/rhi/Types.h"

namespace engine::rhi {

// Unlike the legacy frame IFence, this represents a real GPU timeline value.
class IUploadTicket {
public:
    virtual ~IUploadTicket() = default;
    [[nodiscard]] virtual bool isComplete() const = 0; // non-blocking, thread-safe
    [[nodiscard]] virtual std::uint64_t value() const = 0;
    virtual void wait() const = 0; // shutdown / synchronous tests only
    virtual void acquire() = 0; // renderer thread: GPU-side cross-queue visibility
};

struct BufferUpload final {
    BufferDesc desc;
    std::span<const std::byte> data;
};

struct ImageMipUpload final {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::span<const std::uint8_t> pixels;
};

struct UploadRequest final {
    std::span<const BufferUpload> buffers;
    ImageDesc imageDesc;
    std::span<const ImageMipUpload> imageMips;
};

struct UploadSubmission final {
    // Destroy the ticket last; backend retains destinations until GPU completion.
    std::shared_ptr<IUploadTicket> ticket;
    std::vector<std::unique_ptr<IBuffer>> buffers;
    std::unique_ptr<IImage> image;
    std::uint64_t bytes{0};
};

class IUploadQueue {
public:
    virtual ~IUploadQueue() = default;
    // Concurrent jobs are serialized only while recording this transfer context.
    // CPU spans can be released on return; Diligent owns GPU staging lifetimes.
    virtual UploadSubmission submit(const UploadRequest& request) = 0;
    virtual void waitIdle() = 0; // callers must join producers first
};

} // namespace engine::rhi
