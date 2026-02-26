#include "engine/rhi_d3d12/DxSync.h"

namespace engine::rhi::dx12 {

DxFenceSync::DxFenceSync(bool signaled) : m_signaled(signaled) {}

void DxFenceSync::wait() {
    m_signaled = true;
}

void DxFenceSync::reset() {
    m_signaled = false;
}

void DxFenceSync::signal() {
    m_signaled = true;
}

} // namespace engine::rhi::dx12
