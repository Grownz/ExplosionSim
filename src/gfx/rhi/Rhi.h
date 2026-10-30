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
    R32_TYPELESS,
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

    // Vulkan only: the platform layer supplies the instance extensions and a
    // callback that creates the VkSurfaceKHR for `window`. Non-dispatchable
    // handles are passed as uint64_t so this header stays Vulkan-free.
    struct SurfaceProvider {
        const char* const* instanceExtensions = nullptr;
        uint32_t instanceExtensionCount = 0;
        void* user = nullptr;
        bool (*create)(void* user, void* instance, uint64_t* outSurface) = nullptr;
    } surface;
};

struct BufferDesc {
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    uint32_t structureStride = 0;
    const void* initialData = nullptr;
};

struct TextureSubresource {
    const void* data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
    // Bytes between depth slices for 3D uploads; 0 means rowPitch * height.
    uint32_t slicePitch = 0;
};

struct TextureDesc {
    Format format = Format::Unknown;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    TextureUsage usage = TextureUsage::Sampled;
    const void* initialData = nullptr;
    // Optional CPU-side mip chain; when present the texture is created with
    // these subresources and treated as immutable.
    const TextureSubresource* subresources = nullptr;
    uint32_t subresourceCount = 0;
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    const void* data = nullptr;
    size_t size = 0;
    const char* name = nullptr;
};

struct SamplerDesc {
    bool comparison = false;
    bool clamp = false;
    float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool linear = true;
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
    // Views created for the usages requested in TextureDesc; nullptr when the
    // usage was not requested. Temporary bridge until binding lives in the RHI.
    virtual void* NativeShaderResourceView() const = 0;
    virtual void* NativeRenderTargetView() const = 0;
    virtual void* NativeDepthStencilView() const = 0;
};

class Shader {
public:
    virtual ~Shader() = default;
    virtual ShaderStage Stage() const = 0;
    virtual void* NativeHandle() const = 0;
};

class Sampler {
public:
    virtual ~Sampler() = default;
    virtual void* NativeHandle() const = 0;
};

enum class CompareOp {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

enum class CullMode {
    None,
    Front,
    Back,
};

enum class PrimitiveTopology {
    TriangleList,
    TriangleStrip,
    LineList,
    PointList,
};

enum class VertexFormat {
    Float,
    Float2,
    Float3,
    Float4,
    UInt,
    UInt2,
    UInt3,
    UInt4,
};

enum class DescriptorType {
    UniformBuffer,
    StorageBuffer,
    SampledImage,
    Sampler,
    StorageImage,
};

struct DescriptorBinding {
    uint32_t binding = 0;
    DescriptorType type = DescriptorType::UniformBuffer;
    uint32_t count = 1;
    ShaderStage stage = ShaderStage::Pixel;
    // Optional stage bitmask overriding `stage`: 1 = vertex, 2 = pixel,
    // 4 = compute. Use this when a resource is visible to multiple stages.
    uint32_t stageMask = 0;
};

struct VertexAttribute {
    const char* semantic = nullptr;
    uint32_t semanticIndex = 0;
    VertexFormat format = VertexFormat::Float3;
    uint32_t offset = 0;
    uint32_t bufferSlot = 0;
};

struct PipelineDesc {
    Shader* vertexShader = nullptr;
    Shader* pixelShader = nullptr;
    const VertexAttribute* vertexAttributes = nullptr;
    uint32_t vertexAttributeCount = 0;
    uint32_t vertexStride = 0; // Vulkan vertex binding stride; 0 = derive
    const DescriptorBinding* descriptorBindings = nullptr;
    uint32_t descriptorBindingCount = 0;
    // DXBC signature used to build the D3D11 input layout. Vulkan uses the
    // vertex attributes instead and ignores this.
    const void* vertexShaderBytecode = nullptr;
    size_t vertexShaderBytecodeSize = 0;
    CompareOp depthCompare = CompareOp::LessEqual;
    bool depthTest = true;
    bool depthWrite = true;
    bool blendEnable = false;
    // Premultiplied alpha uses One / InvSrcAlpha instead of SrcAlpha /
    // InvSrcAlpha (volume and particle passes).
    bool premultipliedBlend = false;
    // Additive blending uses One / One (sparks, embers, fire).
    bool additiveBlend = false;
    CullMode cullMode = CullMode::None;
    bool depthClip = true;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
    virtual void* NativeHandle() const = 0;
};

// A set of buffer/image/sampler bindings matching a pipeline's descriptor
// layout. Bindings are written individually so the renderer can update a
// uniform buffer or swap a texture without rebuilding the set.
class DescriptorSet {
public:
    virtual ~DescriptorSet() = default;
    virtual void SetBuffer(uint32_t binding, Buffer* buffer, uint64_t offset = 0,
                           uint64_t range = 0) = 0;
    virtual void SetImage(uint32_t binding, Texture* texture) = 0;
    virtual void SetSampler(uint32_t binding, Sampler* sampler) = 0;
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
    virtual std::unique_ptr<Sampler> CreateSampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> CreatePipeline(const PipelineDesc& desc) = 0;
    virtual std::unique_ptr<DescriptorSet> CreateDescriptorSet(Pipeline* pipeline) = 0;
    // Temporary immediate-mode binding until the command list abstraction
    // lands; sets the pipeline's shaders and fixed-function state.
    virtual void BindPipeline(Pipeline* pipeline) = 0;
    virtual void BindDescriptorSet(DescriptorSet* set, uint32_t index = 0) = 0;
    // Temporary immediate-mode geometry binding and draws. Replaced by a
    // command list in a later milestone.
    virtual void BindVertexBuffer(Buffer* buffer, uint32_t stride,
                                  uint64_t offset = 0) = 0;
    virtual void BindIndexBuffer(Buffer* buffer, uint64_t offset = 0) = 0;
    virtual void Draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;
    virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                               uint32_t firstVertex = 0,
                               uint32_t firstInstance = 0) = 0;
    virtual void DrawIndexedInstanced(uint32_t indexCount,
                                      uint32_t instanceCount,
                                      uint32_t firstIndex = 0,
                                      int32_t baseVertex = 0,
                                      uint32_t firstInstance = 0) = 0;

    virtual bool Resize(int width, int height) = 0;
    virtual void WaitIdle() = 0;
    virtual void* NativeHandle() const = 0;
};

// Backend factory. Implemented by each backend; the caller selects it from
// CMake or a runtime option.
std::unique_ptr<Device> CreateD3D11Device(const DeviceDesc& desc);
std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc);

} // namespace ex::rhi
