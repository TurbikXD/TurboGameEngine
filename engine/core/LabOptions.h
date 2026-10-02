#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>

namespace engine::core {

// Explicit opt-in lab controls; ordinary editor sessions keep their normal scene.
struct LabOptions final {
    std::string scene;
    std::string assetDirectory;
    bool parallelEcs{true};
    bool asyncLoading{true};
    bool waitForTracy{false};
    bool missingAsset{false};
    double durationSeconds{0.0};
    double loadAtSeconds{6.0};
    std::size_t entityCount{4096U};
    std::size_t uploadsPerFrame{1U};
    double uploadBudgetMilliseconds{2.0};

    [[nodiscard]] bool active() const { return scene == "ecs" || scene == "loading"; }

    static std::string environment(const char* name) {
#if defined(_MSC_VER)
        char* buffer = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
            return {};
        }
        std::string value(buffer);
        std::free(buffer);
        return value;
#else
        const char* value = std::getenv(name);
        return value != nullptr ? std::string(value) : std::string{};
#endif
    }

    static LabOptions fromEnvironment() {
        LabOptions options;
        options.scene = environment("TGE_LAB_SCENE");
        options.assetDirectory = environment("TGE_BENCHMARK_ASSET_DIR");
        options.parallelEcs = environment("TGE_JOBS") != "0";
        options.asyncLoading = environment("TGE_ASYNC_LOADING") != "0";
        options.waitForTracy = environment("TGE_WAIT_FOR_TRACY") == "1";
        options.missingAsset = environment("TGE_MISSING_ASSET") == "1";
        const auto number = [](const char* name, const double fallback) {
            const std::string value = environment(name);
            if (value.empty()) { return fallback; }
            char* end = nullptr;
            const double parsed = std::strtod(value.c_str(), &end);
            return end != value.c_str() && *end == '\0' && parsed >= 0.0 && parsed < 86400.0
                       ? parsed : fallback;
        };
        options.durationSeconds = number("TGE_DEMO_SECONDS", 0.0);
        options.loadAtSeconds = number("TGE_LOAD_AT_SECONDS", 6.0);
        options.entityCount = static_cast<std::size_t>(std::clamp(number("TGE_ECS_ENTITIES", 4096.0), 1.0, 16384.0));
        options.uploadsPerFrame = static_cast<std::size_t>(std::clamp(number("TGE_UPLOADS_PER_FRAME", 1.0), 1.0, 65536.0));
        options.uploadBudgetMilliseconds = number("TGE_UPLOAD_BUDGET_MS", 2.0);
        return options;
    }
};

} // namespace engine::core
