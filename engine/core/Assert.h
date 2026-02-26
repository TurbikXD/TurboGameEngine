#pragma once

#include <cstdlib>
#include <source_location>

#include "engine/core/Log.h"

namespace engine::core {

inline void reportAssert(
    bool condition,
    const char* expression,
    const char* message,
    const std::source_location& location = std::source_location::current()) {
    if (condition) {
        return;
    }

    Log::engineLogger()->critical(
        "Assertion failed: '{}' message='{}' at {}:{} ({})",
        expression,
        message != nullptr ? message : "",
        location.file_name(),
        location.line(),
        location.function_name());
    std::abort();
}

} // namespace engine::core

#define ENGINE_ASSERT(expr, msg) ::engine::core::reportAssert((expr), #expr, (msg))
