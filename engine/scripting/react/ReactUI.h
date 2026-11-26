#pragma once

#include <functional>
#include <string_view>

struct ImDrawList;

namespace engine::scripting {
class ScriptingSystem;

class ReactUI final {
public:
    struct Image { void* textureId = nullptr; float width = 0, height = 0; };
    struct Viewport { float x = 0, y = 0, width = 0, height = 0; };
    using ImageResolver = std::function<Image(std::string_view)>;
    using CanvasResolver = std::function<Image(const void* key, const ImDrawList& list, float x, float y, float width, float height)>;
    static void draw(ScriptingSystem& scripting, bool gameOnly = false, const ImageResolver& images = {}, const CanvasResolver& canvases = {});
    static bool drawInViewport(ScriptingSystem& scripting, const Viewport& viewport,
        const ImageResolver& images = {}, const CanvasResolver& canvases = {});
};
}
