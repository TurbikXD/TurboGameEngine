#pragma once

#include <cstddef>
#include <memory>

struct ImDrawList;
namespace Diligent { struct IRenderDevice; struct IDeviceContext; }

namespace engine::renderer {

// GPU-only flattened UI layers. Borrowed views and captured draw lists remain
// pinned until the corresponding frame fence has completed.
class UiCanvasCompositor final {
public:
    struct Image { void* textureId = nullptr; float width = 0, height = 0; };
    explicit UiCanvasCompositor(Diligent::IRenderDevice* device, std::size_t frames);
    ~UiCanvasCompositor();
    UiCanvasCompositor(const UiCanvasCompositor&) = delete;
    UiCanvasCompositor& operator=(const UiCanvasCompositor&) = delete;
    // Call only after this frame slot's fence has completed.
    void beginFrame(std::size_t slot);
    Image enqueue(const void* key, const ImDrawList& list, float x, float y, float width, float height);
    bool pending() const;
    // Render child layers before their parents, before the main ImGui pass.
    void render(Diligent::IDeviceContext* context);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::renderer
