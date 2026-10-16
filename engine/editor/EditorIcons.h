#pragma once

#include <functional>
#include <string_view>
#include "engine/editor/EditorCommands.h"

namespace engine::editor::EditorIcons {

struct Image {
    // Borrowed renderer view. The resolver retains it through frame completion.
    void* textureId{};
    float width{};
    float height{};
};

using Resolver = std::function<Image(std::string_view)>;
enum class Icon { ChevronDown, More, MoveSnap, RotateSnap, Coordinates };

[[nodiscard]] std::string_view assetPath(EditorCommandId command) noexcept;
[[nodiscard]] std::string_view assetPath(Icon icon) noexcept;
// Coordinates are screen pixels in the current ImGui window. Missing images
// use geometric pictograms, including when running headless UI tests.
void draw(EditorCommandId command, float x, float y, float size, const Resolver& resolve = {}, bool enabled = true);
void draw(Icon icon, float x, float y, float size, const Resolver& resolve = {}, bool enabled = true);

} // namespace engine::editor::EditorIcons
