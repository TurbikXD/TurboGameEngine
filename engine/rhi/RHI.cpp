#include "engine/rhi/RHI.h"

#include <algorithm>
#include <cctype>
#include <string>

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

#if ENGINE_ENABLE_DILIGENT_BACKEND
    return diligent::createDiligentDevice(desc, errorMessage);
#else
    if (errorMessage != nullptr) {
        *errorMessage = "Diligent backend is disabled at build time";
    }
    return nullptr;
#endif
}

BackendType backendFromString(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lowered.empty() || lowered == "diligent") {
        return BackendType::Diligent;
    }

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

} // namespace engine::rhi
