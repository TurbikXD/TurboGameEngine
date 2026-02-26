#pragma once

#include <memory>

#include <spdlog/logger.h>

namespace engine::core {

class Log final {
public:
    static void init();
    static void shutdown();

    static std::shared_ptr<spdlog::logger> engineLogger();

private:
    static std::shared_ptr<spdlog::logger> s_engineLogger;
};

} // namespace engine::core

#define ENGINE_LOG_TRACE(...) ::engine::core::Log::engineLogger()->trace(__VA_ARGS__)
#define ENGINE_LOG_DEBUG(...) ::engine::core::Log::engineLogger()->debug(__VA_ARGS__)
#define ENGINE_LOG_INFO(...) ::engine::core::Log::engineLogger()->info(__VA_ARGS__)
#define ENGINE_LOG_WARN(...) ::engine::core::Log::engineLogger()->warn(__VA_ARGS__)
#define ENGINE_LOG_ERROR(...) ::engine::core::Log::engineLogger()->error(__VA_ARGS__)
