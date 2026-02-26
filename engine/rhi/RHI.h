#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "engine/rhi/Device.h"

namespace engine::rhi {

std::unique_ptr<IDevice> createDevice(
    BackendType backend,
    const DeviceCreateDesc& desc,
    std::string* errorMessage = nullptr);

BackendType backendFromString(std::string_view value);
const char* toString(BackendType backend);

} // namespace engine::rhi
