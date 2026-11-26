#pragma once

#include <stdexcept>

#include <nlohmann/json.hpp>

#include "engine/ecs/components.h"

namespace engine::ecs {

inline Light lightFromJson(const nlohmann::json& value) {
    Light light;
    const auto type = value.value("type", std::string("point"));
    if (type == "directional") light.kind = LightKind::Directional;
    else if (type != "point") throw std::runtime_error("Light type must be directional or point");
    if (const auto color = value.find("color"); color != value.end()) {
        if (!color->is_array() || color->size() != 3) throw std::runtime_error("Light color needs three linear RGB channels");
        light.color = {(*color)[0].get<float>(), (*color)[1].get<float>(), (*color)[2].get<float>()};
    }
    light.intensity = value.value("intensity", light.intensity);
    light.range = value.value("range", light.range);
    light.enabled = value.value("enabled", light.enabled);
    const auto contains = [](const float number, const float low, const float high) { return number >= low && number <= high; };
    if (!contains(light.color.r, 0.0F, 1.0F) || !contains(light.color.g, 0.0F, 1.0F) || !contains(light.color.b, 0.0F, 1.0F) ||
        !contains(light.intensity, 0.0F, Light::MaxIntensity) || !contains(light.range, Light::MinRange, Light::MaxRange)) {
        throw std::runtime_error("Light color, intensity or range is out of bounds");
    }
    return light;
}

inline nlohmann::json lightToJson(const Light& light) {
    return {{"type", light.kind == LightKind::Directional ? "directional" : "point"},
        {"color", nlohmann::json::array({light.color.r, light.color.g, light.color.b})},
        {"intensity", light.intensity}, {"range", light.range}, {"enabled", light.enabled}};
}

}
