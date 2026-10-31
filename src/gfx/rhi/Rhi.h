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
    R32G32B32A32_FLOAT,
    R32G32_FLOAT,
    R32_FLOAT,
    R32_TYPELESS,
    D32_FLOAT,
};

inline uint32_t FormatBytesPerPixel(Format format) {
    switch (format) {
    case Format::B8G8R8A8_UNORM:
    case Format::R8G8B8A8_UNORM:
    case Format::R8G8B8A8_UNORM_SRGB:
    case Format::R32_FLOAT:
    case Format::R32_TYPELESS:
    case Format::D32_FLOAT:
        return 4;
    case Format::R16G16B16A16_FLOAT:
    case Format::R32G32_FLOAT:
        return 8;
    case Format::R32G32B32A32_FLOAT:
        return 16;
    case Format::Unknown:
    default:
        return 0;
    }
}

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

enum class TextureState {
    General,
    ShaderRead,
    // Depth-only states for sampling the scene depth or returning it to the
    // depth attachment layout after a full-screen pass.
    DepthRead,
    DepthAttachment,
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
    // Repeat addressing for tiling textures; takes precedence over `clamp`.
    bool wrap = false;
    float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool linear = true;
    // 1 disables anisotropic filtering; 2..16 request that many taps.
    uint32_t maxAnisotropy = 1;
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
    // Replaces the whole texture with `data` (tightly packed rows: width times
    // the format's texel size, then height, then depth). Used by the solid
    // obstacle mask, which changes every few frames. The texture must have been
    // created with TextureUsage::Sampled. The call blocks until the upload is
    // complete.
    virtual bool Update(const void* data) = 0;
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
    Shader* computeShader = nullptr;
    // Depth-only graphics pipeline (shadow passes). The backend uses the
    // matching depth-only render pass; a pixel shader is omitted.
    bool depthOnly = false;
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

// A set of GPU timestamp queries. Values returned by Resolve are nanoseconds
// elapsed at each recorded index, so callers can subtract pairs for pass
// timings. Reset starts a new timing window; it must not be called while a
// previous window is still in flight.
class QueryPool {
public:
    virtual ~QueryPool() = default;
    virtual uint32_t Count() const = 0;
    virtual void Reset() = 0;
    // Blocks until every recorded timestamp is available. Returns false when
    // the backend has no timestamp support or the window was disjoint.
    virtual bool Resolve(uint64_t* values, uint32_t count) = 0;
};

// Records rendering commands. On D3D11 this wraps the immediate device
// context; on Vulkan it records into the device's active command buffer. The
// render pass API is the backend-neutral replacement for OMSetRenderTargets /
// RSSetViewports and for the Vulkan render-pass begin/end pair.
class CommandList {
public:
    virtual ~CommandList() = default;

    virtual void Begin() = 0;
    virtual void End() = 0;

    // Binds `color` (may be null for depth-only passes) and `depth` (may be
    // null) and sets the viewport to the target extent.
    virtual void BeginRenderPass(Texture* color, Texture* depth, bool clearColor,
                                 bool clearDepth,
                                 const float clearColorValue[4]) = 0;
    virtual void EndRenderPass() = 0;
    virtual void SetViewport(uint32_t width, uint32_t height) = 0;
    virtual void ClearDepth(Texture* depth, float value = 1.0f) = 0;

    virtual void BindPipeline(Pipeline* pipeline) = 0;
    virtual void BindDescriptorSet(DescriptorSet* set, uint32_t index = 0) = 0;
    virtual void BindVertexBuffer(Buffer* buffer, uint32_t stride,
                                  uint64_t offset = 0) = 0;
    virtual void BindIndexBuffer(Buffer* buffer, uint64_t offset = 0) = 0;

    // Explicit resource binding used by the renderer while its passes are
    // migrated to descriptor sets. `slot` is the D3D register index (b#, t#,
    // s#); the Vulkan backend maps it through the shader binding convention.
    virtual void BindConstantBuffer(Buffer* buffer, ShaderStage stage,
                                    uint32_t slot) = 0;
    virtual void BindShaderResource(Buffer* buffer, ShaderStage stage,
                                    uint32_t slot) = 0;
    virtual void BindShaderResource(Texture* texture, ShaderStage stage,
                                    uint32_t slot) = 0;
    virtual void BindSampler(Sampler* sampler, ShaderStage stage,
                             uint32_t slot) = 0;

    virtual void Draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;
    virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                               uint32_t firstVertex = 0,
                               uint32_t firstInstance = 0) = 0;
    virtual void DrawIndexedInstanced(uint32_t indexCount,
                                      uint32_t instanceCount,
                                      uint32_t firstIndex = 0,
                                      int32_t baseVertex = 0,
                                      uint32_t firstInstance = 0) = 0;
    virtual void DrawInstancedIndirect(Buffer* arguments,
                                       uint64_t offset = 0) = 0;

    // Compute pass. BeginCompute/EndCompute bracket a dispatch sequence; on
    // D3D11 they are no-ops because the immediate context records directly.
    virtual void BeginCompute() = 0;
    virtual void EndCompute() = 0;
    virtual void Dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;
    // Global compute-to-compute memory dependency between dispatches.
    virtual void ComputeBarrier() = 0;
    // Transitions a storage image between general and shader-read layouts.
    virtual void TransitionTexture(Texture* texture, TextureState state) = 0;
    // Records a GPU timestamp into `pool` at `index`. Must be called while a
    // compute pass or render pass is active.
    virtual void WriteTimestamp(QueryPool* pool, uint32_t index) = 0;

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
    virtual std::unique_ptr<Sampler> CreateSampler(const SamplerDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> CreatePipeline(const PipelineDesc& desc) = 0;
    virtual std::unique_ptr<DescriptorSet> CreateDescriptorSet(Pipeline* pipeline) = 0;
    virtual std::unique_ptr<CommandList> CreateCommandList() = 0;
    // Creates a GPU timestamp query pool. Returns null when the backend or
    // device does not support timestamps.
    virtual std::unique_ptr<QueryPool> CreateQueryPool(uint32_t count) = 0;

    // Shared depth target for the current swapchain/offscreen extent. Null on
    // backends that do not expose one yet.
    virtual Texture* DepthTarget() = 0;

    virtual int Width() const = 0;
    virtual int Height() const = 0;

    // Frame lifecycle for the application shell. BeginFrame binds and clears
    // the current target; EndFrame presents it (windowed) or leaves it ready
    // for ReadBack (offscreen).
    virtual void BeginFrame(float r, float g, float b, float a) = 0;
    virtual void EndFrame(bool vsync) = 0;
    virtual bool ReadBack(std::vector<uint8_t>& bgra, int& outWidth,
                          int& outHeight) = 0;

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
