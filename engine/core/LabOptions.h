#pragma once

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>

namespace engine::core {

// Explicit opt-in lab controls; ordinary editor sessions keep their normal scene.
struct LabOptions final {
    std::string scene;
    std::string assetDirectory;
    bool parallelEcs{true};
    bool asyncLoading{true};
    bool gpuUpload{true};
    bool waitForTracy{false};
    bool missingAsset{false};
    bool stressScene{false}; // Explicit ordinary-app stress scene, independent of Tracy.
    double durationSeconds{0.0};
    double loadAtSeconds{6.0};
    std::size_t entityCount{4096U};
    std::size_t jobWorkers{0U}; // 0 preserves the conservative automatic default.
    std::size_t assetInFlightLimit{0U}; // 0 keeps the normal workers-dependent limit.
    std::size_t uploadsPerFrame{1U};
    double uploadBudgetMilliseconds{2.0};

    [[nodiscard]] bool active() const { return scene == "ecs" || scene == "loading"; }
    [[nodiscard]] bool stressActive() const { return stressScene && !active(); }

    static std::size_t boundedCount(const std::string_view text, const std::size_t maximum) {
        if (text.empty()) { return 0U; }
        std::size_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= maximum
                   ? value : 0U;
    }

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
        options.gpuUpload = environment("TGE_GPU_UPLOAD") != "0";
        options.waitForTracy = environment("TGE_WAIT_FOR_TRACY") == "1";
        options.missingAsset = environment("TGE_MISSING_ASSET") == "1";
        options.stressScene = environment("TGE_STRESS_SCENE") == "1";
        options.jobWorkers = boundedCount(environment("TGE_JOB_WORKERS"), 256U);
        options.assetInFlightLimit = boundedCount(environment("TGE_ASSET_IN_FLIGHT"), 16U);
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
