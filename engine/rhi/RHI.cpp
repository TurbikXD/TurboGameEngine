#include "engine/rhi/RHI.h"

#include "engine/rhi_diligent/DiligentDevice.h"

namespace engine::rhi {

std::unique_ptr<IDevice> createDevice(
    BackendType backend,
    const DeviceCreateDesc& desc,
    std::string* errorMessage) {
    if (backend != BackendType::Diligent) {
        if (errorMessage != nullptr) {
            *errorMessage =
                "Legacy backends were removed. TurboGameEngine now supports only BackendType::Diligent";
        }
        return nullptr;
    }

    return diligent::createDiligentDevice(desc, errorMessage);
}

BackendType backendFromString(std::string_view) {
    return BackendType::Diligent;
}

const char* toString(BackendType backend) {
    switch (backend) {
        case BackendType::OpenGL:
            return "OpenGL (removed)";
        case BackendType::Vulkan:
            return "Vulkan (removed)";
        case BackendType::D3D12:
            return "D3D12 (removed)";
        case BackendType::Diligent:
            return "Diligent";
        default:
            return "Unknown";
    }
}

}
