#include "engine/core/Config.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/core/Log.h"

namespace engine::core {

EngineConfig Config::load(std::string_view path) {
    EngineConfig config{};
    const std::filesystem::path configPath(path);

    if (!std::filesystem::exists(configPath)) {
        ENGINE_LOG_INFO("Config file '{}' not found, using defaults", configPath.string());
        return config;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        ENGINE_LOG_WARN("Failed to open config '{}', using defaults", configPath.string());
        return config;
    }

    try {
        nlohmann::json jsonData;
        file >> jsonData;

        config.width = jsonData.value("width", config.width);
        config.height = jsonData.value("height", config.height);
        config.title = jsonData.value("title", config.title);
        config.vsync = jsonData.value("vsync", config.vsync);
        config.diligentDevice = jsonData.value("diligentDevice", config.diligentDevice);
        config.initialState = jsonData.value("initialState", config.initialState);

        if (jsonData.contains("clearColor") && jsonData["clearColor"].is_array() &&
            jsonData["clearColor"].size() == 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                config.clearColor[i] = jsonData["clearColor"][i].get<float>();
            }
        }

        ENGINE_LOG_INFO("Loaded config from '{}'", configPath.string());
    } catch (const std::exception& ex) {
        ENGINE_LOG_WARN("Failed parsing config '{}': {}", configPath.string(), ex.what());
    }

    return config;
}

} // namespace engine::core
