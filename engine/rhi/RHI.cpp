#include "engine/rhi/RHI.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "engine/core/Log.h"
#include "engine/rhi_opengl/GLDevice.h"

#if ENGINE_ENABLE_VULKAN_BACKEND
#include "engine/rhi_vulkan/VkDevice.h"
#endif

#if ENGINE_ENABLE_D3D12_BACKEND && defined(_WIN32)
#include "engine/rhi_d3d12/DxDevice.h"
#endif

namespace engine::rhi {

std::unique_ptr<IDevice> createDevice(
    BackendType backend,
    const DeviceCreateDesc& desc,
    std::string* errorMessage) {
    switch (backend) {
        case BackendType::OpenGL:
            return gl::createOpenGLDevice(desc, errorMessage);
        case BackendType::Vulkan:
#if ENGINE_ENABLE_VULKAN_BACKEND
            return vk::createVulkanDevice(desc, errorMessage);
#else
            if (errorMessage != nullptr) {
                *errorMessage = "Vulkan backend is disabled at build time";
            }
            return nullptr;
#endif
        case BackendType::D3D12:
#if ENGINE_ENABLE_D3D12_BACKEND && defined(_WIN32)
            return dx12::createD3D12Device(desc, errorMessage);
#else
            if (errorMessage != nullptr) {
                *errorMessage = "D3D12 backend is unavailable on this platform/build";
            }
            return nullptr;
#endif
        default:
            if (errorMessage != nullptr) {
                *errorMessage = "Unknown backend requested";
            }
            return nullptr;
    }
}

BackendType backendFromString(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lowered == "vulkan") {
        return BackendType::Vulkan;
    }
    if (lowered == "d3d12") {
        return BackendType::D3D12;
    }
    return BackendType::OpenGL;
}

const char* toString(BackendType backend) {
    switch (backend) {
        case BackendType::OpenGL:
            return "OpenGL";
        case BackendType::Vulkan:
            return "Vulkan";
        case BackendType::D3D12:
            return "D3D12";
        default:
            return "Unknown";
    }
}

} // namespace engine::rhi
