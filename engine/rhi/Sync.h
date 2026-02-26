#pragma once

namespace engine::rhi {

class ISemaphore {
public:
    virtual ~ISemaphore() = default;
};

class IFence {
public:
    virtual ~IFence() = default;
    virtual void wait() = 0;
    virtual void reset() = 0;
    virtual void signal() = 0;
};

} // namespace engine::rhi
