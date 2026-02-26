#pragma once

#include "engine/rhi/Sync.h"

namespace engine::rhi::dx12 {

class DxSemaphoreSync final : public ISemaphore {};

class DxFenceSync final : public IFence {
public:
    explicit DxFenceSync(bool signaled);

    void wait() override;
    void reset() override;
    void signal() override;

private:
    bool m_signaled{false};
};

} // namespace engine::rhi::dx12
