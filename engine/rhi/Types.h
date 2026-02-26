#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace engine::platform {
class Window;
}

namespace engine::rhi {

enum class BackendType : std::uint8_t {
    OpenGL = 0,
    Vulkan,
    D3D12
};

enum class BufferUsage : std::uint8_t {
    Vertex = 0,
    Index,
    Uniform
};

enum class ShaderStage : std::uint32_t {
    Vertex = 1U << 0U,
    Fragment = 1U << 1U
};

enum class PrimitiveTopology : std::uint8_t {
    TriangleList = 0
};

enum class ResourceState : std::uint8_t {
    Undefined = 0,
    VertexBuffer,
    IndexBuffer,
    ConstantBuffer,
    ShaderRead,
    RenderTarget,
    DepthWrite,
    Present,
    CopySrc,
    CopyDst
};

enum class StageFlags : std::uint32_t {
    None = 0U,
    Top = 1U << 0U,
    VertexInput = 1U << 1U,
    VertexShader = 1U << 2U,
    FragmentShader = 1U << 3U,
    ColorOutput = 1U << 4U,
    Transfer = 1U << 5U,
    Bottom = 1U << 6U
};

inline StageFlags operator|(StageFlags lhs, StageFlags rhs) {
    return static_cast<StageFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

struct Extent2D final {
    std::uint32_t width{0};
    std::uint32_t height{0};
};

struct Viewport final {
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};
    float minDepth{0.0F};
    float maxDepth{1.0F};
};

struct Rect2D final {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

struct ClearColor final {
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
    float a{1.0F};
};

using ResourceHandle = std::uint64_t;

struct BarrierDesc final {
    ResourceHandle resource{0};
    ResourceState before{ResourceState::Undefined};
    ResourceState after{ResourceState::Undefined};
    StageFlags srcStage{StageFlags::None};
    StageFlags dstStage{StageFlags::None};
};

struct DeviceCreateDesc final {
    platform::Window* window{nullptr};
    bool enableValidation{false};
};

struct SwapchainDesc final {
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool vsync{true};
};

struct RenderPassDesc final {
    ClearColor clearColor{};
    bool clearColorTarget{true};
};

struct BufferDesc final {
    std::size_t size{0};
    BufferUsage usage{BufferUsage::Vertex};
    bool dynamic{false};
};

struct ShaderModuleDesc final {
    ShaderStage stage{ShaderStage::Vertex};
    std::string path;
    std::string entryPoint{"main"};
};

class IShaderModule;

struct GraphicsPipelineDesc final {
    IShaderModule* vertexShader{nullptr};
    IShaderModule* fragmentShader{nullptr};
    PrimitiveTopology topology{PrimitiveTopology::TriangleList};
};

} // namespace engine::rhi
