#pragma once

#include <functional>
#include <string_view>

struct ImDrawList;

namespace engine::scripting {
class ScriptingSystem;

// Draw only within an active ImGui frame, on the scripting system's owner thread.
class ReactUI final {
public:
    struct Image { void* textureId = nullptr; float width = 0, height = 0; };
    using ImageResolver = std::function<Image(std::string_view)>;
    // The borrowed draw list is complete and standalone, with global logical
    // coordinates. A resolver that queues GPU work must retain its own copy.
    using CanvasResolver = std::function<Image(const void* key, const ImDrawList& list, float x, float y, float width, float height)>;
    static void draw(ScriptingSystem& scripting, bool gameOnly = false, const ImageResolver& images = {}, const CanvasResolver& canvases = {});
};
} // namespace engine::scripting
