#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "third_party/DiligentEngine/DiligentTools/ThirdParty/imgui/imgui.h"

namespace engine::editor {

class NetworkChartPng final {
public:
    NetworkChartPng(int width, int height);
    ~NetworkChartPng();
    NetworkChartPng(const NetworkChartPng&) = delete;
    NetworkChartPng& operator=(const NetworkChartPng&) = delete;

    void AddRectFilled(ImVec2 min, ImVec2 max, ImU32 color);
    void AddLine(ImVec2 a, ImVec2 b, ImU32 color, float thickness = 1.0F);
    void AddPolyline(const ImVec2* points, int count, ImU32 color, ImDrawFlags flags, float thickness);
    void AddCircle(ImVec2 center, float radius, ImU32 color, int numSegments);
    void AddCircleFilled(ImVec2 center, float radius, ImU32 color, int numSegments);
    void PushClipRect(ImVec2 min, ImVec2 max, bool intersect);
    void PopClipRect();
    void AddText(ImVec2 pos, ImU32 color, const char* utf8);
    [[nodiscard]] float textWidth(const char* utf8) const;
    [[nodiscard]] float textHeight() const;
    void setTextSize(float pixels);
    [[nodiscard]] std::optional<std::string> encode(std::string& error);
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
