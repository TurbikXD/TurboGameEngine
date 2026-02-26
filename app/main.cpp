#include "engine/core/Application.h"
#include "engine/core/Log.h"
#include "engine/rhi/RHI.h"

int main() {
    constexpr const char* backendName = ENGINE_BACKEND_NAME;
    const engine::rhi::BackendType backend = engine::rhi::backendFromString(backendName);

    engine::core::Application app(backend);
    if (!app.init()) {
        ENGINE_LOG_ERROR("App initialization failed");
        return 1;
    }

    return app.run();
}
