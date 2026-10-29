#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ex::rhi {

// Backend-neutral render hardware interface. No D3D11, DXGI, Vulkan or HWND
// type may appear in this header; backend-specific handles are exposed through
// NativeHandle() for the temporary legacy bridge only.
enum class Backend {
    D3D11,
    Vulkan,
};

enum class Format {
    Unknown,
    B8G8R8A8_UNORM,
    R8G8B8A8_UNORM,
    R8G8B8A8_UNORM_SRGB,
    R16G16B16A16_FLOAT,
    R32_FLOAT,
    D32_FLOAT,
};

enum class BufferUsage : uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Indirect = 1u << 4,
    Staging = 1u << 5,
    Dynamic = 1u << 6,
    ShaderRead = 1u << 7,
};

inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return BufferUsage(uint32_t(a) | uint32_t(b));
}
inline bool HasUsage(BufferUsage value, BufferUsage flag) {
    return (uint32_t(value) & uint32_t(flag)) != 0;
}

enum class TextureUsage : uint32_t {
    None = 0,
    Sampled = 1u << 0,
    Storage = 1u << 1,
    RenderTarget = 1u << 2,
    DepthStencil = 1u << 3,
    Staging = 1u << 4,
};

inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return TextureUsage(uint32_t(a) | uint32_t(b));
}
inline bool HasUsage(TextureUsage value, TextureUsage flag) {
    return (uint32_t(value) & uint32_t(flag)) != 0;
}

enum class ShaderStage {
    Vertex,
    Pixel,
    Compute,
};

struct Extent2D {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct DeviceDesc {
    Backend backend = Backend::D3D11;
    bool debug = false;
    void* window = nullptr; // native window handle (HWND on Windows)
    int width = 1;
    int height = 1;
    bool offscreen = false;
};

struct BufferDesc {
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    uint32_t structureStride = 0;
    const void* initialData = nullptr;
};

struct TextureDesc {
    Format format = Format::Unknown;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    TextureUsage usage = TextureUsage::Sampled;
    const void* initialData = nullptr;
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    const void* data = nullptr;
    size_t size = 0;
    const char* name = nullptr;
};

class Buffer {
public:
    virtual ~Buffer() = default;
    virtual uint64_t Size() const = 0;
    // Writable mapping for dynamic buffers. The returned pointer is valid
    // until Unmap(); returns nullptr on failure.
    virtual void* Map() = 0;
    virtual void Unmap() = 0;
    virtual void* NativeHandle() const = 0;
    // Structured/typed buffer view for shader reads; nullptr when the buffer
    // was not created with BufferUsage::ShaderRead.
    virtual void* NativeShaderResourceView() const = 0;
};

class Texture {
public:
    virtual ~Texture() = default;
    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;
    virtual uint32_t Depth() const = 0;
    virtual Format GetFormat() const = 0;
    virtual void* NativeHandle() const = 0;
};

class Shader {
public:
    virtual ~Shader() = default;
    virtual ShaderStage Stage() const = 0;
    virtual void* NativeHandle() const = 0;
};

// Owns the GPU device, the swapchain or offscreen target and the shared depth
// buffer. Backends implement the resource factories; the renderer only sees
// this interface once the legacy D3D11 bridge is removed.
class Device {
public:
    virtual ~Device() = default;

    virtual Backend GetBackend() const = 0;
    virtual std::unique_ptr<Buffer> CreateBuffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<Texture> CreateTexture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<Shader> CreateShader(const ShaderDesc& desc) = 0;

    virtual bool Resize(int width, int height) = 0;
    virtual void WaitIdle() = 0;
    virtual void* NativeHandle() const = 0;
};

// Backend factory. Implemented by each backend; the caller selects it from
// CMake or a runtime option.
std::unique_ptr<Device> CreateD3D11Device(const DeviceDesc& desc);

} // namespace ex::rhi
