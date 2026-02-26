#include "engine/core/Log.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace engine::core {

std::shared_ptr<spdlog::logger> Log::s_engineLogger;

void Log::init() {
    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        std::filesystem::create_directories("logs");

        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/engine.log", 1024 * 1024 * 5, 3);

        std::vector<spdlog::sink_ptr> sinks{consoleSink, fileSink};
        s_engineLogger = std::make_shared<spdlog::logger>("engine", sinks.begin(), sinks.end());

        s_engineLogger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
#if defined(NDEBUG)
        s_engineLogger->set_level(spdlog::level::info);
#else
        s_engineLogger->set_level(spdlog::level::debug);
#endif

        s_engineLogger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(s_engineLogger);
        spdlog::flush_every(std::chrono::seconds(1));

        ENGINE_LOG_INFO("Logger initialized");
    });
}

void Log::shutdown() {
    if (s_engineLogger) {
        ENGINE_LOG_INFO("Logger shutdown");
    }
    spdlog::shutdown();
    s_engineLogger.reset();
}

std::shared_ptr<spdlog::logger> Log::engineLogger() {
    if (!s_engineLogger) {
        init();
    }
    return s_engineLogger;
}

} // namespace engine::core
