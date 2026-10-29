#include "gfx/rhi/d3d11/D3D11Rhi.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>

namespace ex::rhi::d3d11 {

namespace {

constexpr DXGI_FORMAT kFinalFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

DXGI_FORMAT ToDxgiFormat(Format format) {
    switch (format) {
    case Format::B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case Format::R32_TYPELESS:
        return DXGI_FORMAT_R32_TYPELESS;
    case Format::D32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

UINT ToBindFlags(BufferUsage usage) {
    UINT flags = 0;
    if (HasUsage(usage, BufferUsage::Vertex)) {
        flags |= D3D11_BIND_VERTEX_BUFFER;
    }
    if (HasUsage(usage, BufferUsage::Index)) {
        flags |= D3D11_BIND_INDEX_BUFFER;
    }
    if (HasUsage(usage, BufferUsage::Uniform)) {
        flags |= D3D11_BIND_CONSTANT_BUFFER;
    }
    if (HasUsage(usage, BufferUsage::Storage)) {
        flags |= D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    }
    if (HasUsage(usage, BufferUsage::ShaderRead)) {
        flags |= D3D11_BIND_SHADER_RESOURCE;
    }
    return flags;
}

UINT ToBindFlags(TextureUsage usage) {
    UINT flags = 0;
    if (HasUsage(usage, TextureUsage::Sampled)) {
        flags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (HasUsage(usage, TextureUsage::Storage)) {
        flags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    if (HasUsage(usage, TextureUsage::RenderTarget)) {
        flags |= D3D11_BIND_RENDER_TARGET;
    }
    if (HasUsage(usage, TextureUsage::DepthStencil)) {
        flags |= D3D11_BIND_DEPTH_STENCIL;
    }
    return flags;
}

DXGI_FORMAT ToDxgiFormat(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float:
        return DXGI_FORMAT_R32_FLOAT;
    case VertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VertexFormat::UInt:
        return DXGI_FORMAT_R32_UINT;
    case VertexFormat::UInt2:
        return DXGI_FORMAT_R32G32_UINT;
    case VertexFormat::UInt3:
        return DXGI_FORMAT_R32G32B32_UINT;
    case VertexFormat::UInt4:
        return DXGI_FORMAT_R32G32B32A32_UINT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

D3D11_COMPARISON_FUNC ToComparisonFunc(CompareOp op) {
    switch (op) {
    case CompareOp::Never:
        return D3D11_COMPARISON_NEVER;
    case CompareOp::Less:
        return D3D11_COMPARISON_LESS;
    case CompareOp::Equal:
        return D3D11_COMPARISON_EQUAL;
    case CompareOp::LessEqual:
        return D3D11_COMPARISON_LESS_EQUAL;
    case CompareOp::Greater:
        return D3D11_COMPARISON_GREATER;
    case CompareOp::NotEqual:
        return D3D11_COMPARISON_NOT_EQUAL;
    case CompareOp::GreaterEqual:
        return D3D11_COMPARISON_GREATER_EQUAL;
    case CompareOp::Always:
        return D3D11_COMPARISON_ALWAYS;
    }
    return D3D11_COMPARISON_LESS_EQUAL;
}

D3D11_CULL_MODE ToCullMode(CullMode mode) {
    switch (mode) {
    case CullMode::None:
        return D3D11_CULL_NONE;
    case CullMode::Front:
        return D3D11_CULL_FRONT;
    case CullMode::Back:
        return D3D11_CULL_BACK;
    }
    return D3D11_CULL_NONE;
}

D3D11_PRIMITIVE_TOPOLOGY ToTopology(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::TriangleList:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::TriangleStrip:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PrimitiveTopology::LineList:
        return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case PrimitiveTopology::PointList:
        return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

class BufferImpl final : public rhi::Buffer {
public:
    BufferImpl(ComPtr<ID3D11Buffer> buffer, ComPtr<ID3D11ShaderResourceView> srv,
               ID3D11DeviceContext* context)
        : buffer_(std::move(buffer)), srv_(std::move(srv)), context_(context) {}

    uint64_t Size() const override {
        D3D11_BUFFER_DESC desc{};
        if (buffer_) {
            buffer_->GetDesc(&desc);
        }
        return desc.ByteWidth;
    }
    void* Map() override {
        if (!buffer_ || !context_ || mapped_) {
            return nullptr;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return nullptr;
        }
        mapped_ = true;
        return mapped.pData;
    }
    void Unmap() override {
        if (mapped_ && context_) {
            context_->Unmap(buffer_.Get(), 0);
            mapped_ = false;
        }
    }
    void* NativeHandle() const override { return buffer_.Get(); }
    void* NativeShaderResourceView() const override { return srv_.Get(); }

private:
    ComPtr<ID3D11Buffer> buffer_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    ID3D11DeviceContext* context_ = nullptr;
    bool mapped_ = false;
};

class TextureImpl final : public rhi::Texture {
public:
    TextureImpl(ComPtr<ID3D11Texture2D> texture2D, ComPtr<ID3D11Texture3D> texture3D,
                ComPtr<ID3D11ShaderResourceView> srv, ComPtr<ID3D11RenderTargetView> rtv,
                ComPtr<ID3D11DepthStencilView> dsv, ComPtr<ID3D11UnorderedAccessView> uav,
                const TextureDesc& desc)
        : texture2D_(std::move(texture2D)),
          texture3D_(std::move(texture3D)),
          srv_(std::move(srv)),
          rtv_(std::move(rtv)),
          dsv_(std::move(dsv)),
          uav_(std::move(uav)),
          desc_(desc) {}

    uint32_t Width() const override { return desc_.width; }
    uint32_t Height() const override { return desc_.height; }
    uint32_t Depth() const override { return desc_.depth; }
    Format GetFormat() const override { return desc_.format; }
    void* NativeHandle() const override {
        return texture2D_ ? static_cast<void*>(texture2D_.Get())
                          : static_cast<void*>(texture3D_.Get());
    }

    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
    ID3D11RenderTargetView* RTV() const { return rtv_.Get(); }
    ID3D11DepthStencilView* DSV() const { return dsv_.Get(); }
    ID3D11UnorderedAccessView* UAV() const { return uav_.Get(); }

    void* NativeShaderResourceView() const override { return srv_.Get(); }
    void* NativeRenderTargetView() const override { return rtv_.Get(); }
    void* NativeDepthStencilView() const override { return dsv_.Get(); }

private:
    ComPtr<ID3D11Texture2D> texture2D_;
    ComPtr<ID3D11Texture3D> texture3D_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11DepthStencilView> dsv_;
    ComPtr<ID3D11UnorderedAccessView> uav_;
    TextureDesc desc_{};
};

class ShaderImpl final : public rhi::Shader {
public:
    ShaderImpl(ShaderStage stage, ComPtr<ID3D11VertexShader> vs,
               ComPtr<ID3D11PixelShader> ps, ComPtr<ID3D11ComputeShader> cs)
        : stage_(stage), vs_(std::move(vs)), ps_(std::move(ps)), cs_(std::move(cs)) {}

    ShaderStage Stage() const override { return stage_; }
    void* NativeHandle() const override {
        if (vs_) {
            return vs_.Get();
        }
        if (ps_) {
            return ps_.Get();
        }
        return cs_.Get();
    }

private:
    ShaderStage stage_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11ComputeShader> cs_;
};

class SamplerImpl final : public rhi::Sampler {
public:
    explicit SamplerImpl(ComPtr<ID3D11SamplerState> sampler)
        : sampler_(std::move(sampler)) {}

    void* NativeHandle() const override { return sampler_.Get(); }

private:
    ComPtr<ID3D11SamplerState> sampler_;
};

class PipelineImpl final : public rhi::Pipeline {
public:
    PipelineImpl(ComPtr<ID3D11InputLayout> inputLayout,
                 ComPtr<ID3D11VertexShader> vs, ComPtr<ID3D11PixelShader> ps,
                 ComPtr<ID3D11DepthStencilState> depthState,
                 ComPtr<ID3D11RasterizerState> rasterizer,
                 ComPtr<ID3D11BlendState> blendState,
                 D3D11_PRIMITIVE_TOPOLOGY topology)
        : inputLayout_(std::move(inputLayout)),
          vs_(std::move(vs)),
          ps_(std::move(ps)),
          depthState_(std::move(depthState)),
          rasterizer_(std::move(rasterizer)),
          blendState_(std::move(blendState)),
          topology_(topology) {}

    void* NativeHandle() const override { return vs_.Get(); }

    void Bind(ID3D11DeviceContext* context) const {
        if (!context) {
            return;
        }
        context->IASetInputLayout(inputLayout_.Get());
        context->IASetPrimitiveTopology(topology_);
        context->VSSetShader(vs_.Get(), nullptr, 0);
        context->PSSetShader(ps_.Get(), nullptr, 0);
        context->OMSetDepthStencilState(depthState_.Get(), 0);
        context->OMSetBlendState(blendState_.Get(), nullptr, 0xffffffff);
        context->RSSetState(rasterizer_.Get());
    }

private:
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11DepthStencilState> depthState_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11BlendState> blendState_;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

} // namespace
} // namespace ex::rhi::d3d11

namespace ex::rhi {

std::unique_ptr<rhi::Device> CreateD3D11Device(const DeviceDesc& desc) {
    auto device = std::make_unique<d3d11::Device>();
    if (!device->Initialize(desc)) {
        return nullptr;
    }
    return device;
}

} // namespace ex::rhi

namespace ex::rhi::d3d11 {

Device::~Device() {
    Shutdown();
}

bool Device::Initialize(const DeviceDesc& desc) {
    window_ = static_cast<HWND>(desc.window);
    offscreen_ = desc.offscreen;
    width_ = std::max(1, desc.width);
    height_ = std::max(1, desc.height);

    if (!CreateDevice(desc.debug)) {
        return false;
    }
    if (!offscreen_ && !CreateSwapChain(window_)) {
        return false;
    }
    if (!CreateSizeDependentResources()) {
        return false;
    }
    return true;
}

void Device::Shutdown() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    ReleaseSizeDependentResources();
    backbuffer_.Reset();
    backbufferRTV_.Reset();
    swapchain_.Reset();
    factory_.Reset();
    context_.Reset();
    device_.Reset();
}

bool Device::CreateDevice(bool debugLayer) {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        LogError("Failed to create DXGI factory.");
        return false;
    }

    ComPtr<IDXGIAdapter1> bestAdapter;
    SIZE_T bestMemory = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        if (desc.DedicatedVideoMemory > bestMemory) {
            bestMemory = desc.DedicatedVideoMemory;
            bestAdapter = adapter;
        }
    }

    UINT flags = 0;
    if (debugLayer) {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    dedicatedVideoMemory_ = uint64_t(bestMemory);

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        bestAdapter.Get(),
        bestAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        UINT(std::size(levels)),
        D3D11_SDK_VERSION,
        &device_,
        &obtained,
        &context_);

    if (FAILED(hr) && debugLayer) {
        LogWarn("Debug layer unavailable (0x%08X), retrying without it.", unsigned(hr));
        flags &= ~UINT(D3D11_CREATE_DEVICE_DEBUG);
        hr = D3D11CreateDevice(
            bestAdapter.Get(),
            bestAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            UINT(std::size(levels)),
            D3D11_SDK_VERSION,
            &device_,
            &obtained,
            &context_);
    }

    if (FAILED(hr)) {
        LogError("D3D11CreateDevice failed (0x%08X). A Direct3D 11 capable GPU is required.",
                 unsigned(hr));
        return false;
    }

    {
        ComPtr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(device_.As(&dxgiDevice1))) {
            dxgiDevice1->SetMaximumFrameLatency(1);
        }
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device_.As(&dxgiDevice))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                char utf8[512]{};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8, sizeof(utf8),
                                    nullptr, nullptr);
                adapterName_ = utf8;
            }
        }
    }

    LogInfo("Direct3D 11 device ready: %s (feature level %d.%d)",
            adapterName_.c_str(), (obtained >> 12) & 0xF, (obtained >> 8) & 0xF);

    ComPtr<IDXGIDevice> dxgiDevice2;
    ComPtr<IDXGIAdapter> dxgiAdapter;
    if (SUCCEEDED(device_.As(&dxgiDevice2)) &&
        SUCCEEDED(dxgiDevice2->GetAdapter(&dxgiAdapter))) {
        dxgiAdapter->GetParent(IID_PPV_ARGS(&factory_));
    }
    if (!factory_) {
        factory_ = factory;
    }
    return true;
}

bool Device::CreateSwapChain(HWND window) {
    if (!factory_) {
        LogError("DXGI factory unavailable.");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(width_);
    desc.Height = UINT(height_);
    desc.Format = kFinalFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = factory_->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr,
                                                  nullptr, &swapchain_);
    if (FAILED(hr)) {
        LogError("CreateSwapChainForHwnd failed (0x%08X).", unsigned(hr));
        return false;
    }

    factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool Device::CreateSizeDependentResources() {
    if (swapchain_) {
        if (!backbuffer_) {
            HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer_));
            if (FAILED(hr)) {
                LogError("Failed to obtain swap chain buffer (0x%08X).", unsigned(hr));
                return false;
            }
        }
        HRESULT hr = device_->CreateRenderTargetView(backbuffer_.Get(), nullptr, &backbufferRTV_);
        if (FAILED(hr)) {
            LogError("Failed to create back buffer RTV (0x%08X).", unsigned(hr));
            return false;
        }
    } else {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(width_);
        desc.Height = UINT(height_);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = kFinalFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &finalTexture_);
        if (FAILED(hr)) {
            LogError("Failed to create offscreen target (0x%08X).", unsigned(hr));
            return false;
        }
        hr = device_->CreateRenderTargetView(finalTexture_.Get(), nullptr, &finalRTV_);
        if (FAILED(hr)) {
            LogError("Failed to create offscreen RTV (0x%08X).", unsigned(hr));
            return false;
        }
    }

    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(width_);
        desc.Height = UINT(height_);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = kFinalFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
        if (FAILED(hr)) {
            LogError("Failed to create readback staging texture (0x%08X).", unsigned(hr));
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = UINT(width_);
    depthDesc.Height = UINT(height_);
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&depthDesc, nullptr, &depthTexture_))) {
        LogError("Failed to create depth texture.");
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device_->CreateDepthStencilView(depthTexture_.Get(), &dsvDesc, &dsv_))) {
        LogError("Failed to create depth stencil view.");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(depthTexture_.Get(), &srvDesc, &depthSRV_))) {
        LogError("Failed to create depth SRV.");
        return false;
    }

    if (swapchain_) {
        finalRTV_ = backbufferRTV_;
    }
    return true;
}

void Device::ReleaseSizeDependentResources() {
    if (context_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    staging_.Reset();
    finalRTV_.Reset();
    finalTexture_.Reset();
    backbufferRTV_.Reset();
    backbuffer_.Reset();
    depthSRV_.Reset();
    dsv_.Reset();
    depthTexture_.Reset();
}

bool Device::Resize(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == width_ && height == height_) {
        return true;
    }

    width_ = width;
    height_ = height;

    ReleaseSizeDependentResources();
    if (swapchain_) {
        HRESULT hr = swapchain_->ResizeBuffers(0, UINT(width_), UINT(height_),
                                               DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            LogError("ResizeBuffers failed (0x%08X).", unsigned(hr));
            return false;
        }
    }
    return CreateSizeDependentResources();
}

void Device::BindFinalTarget() {
    ID3D11RenderTargetView* rtv = finalRTV_.Get();
    context_->OMSetRenderTargets(1, &rtv, dsv_.Get());
    D3D11_VIEWPORT viewport{};
    viewport.Width = float(width_);
    viewport.Height = float(height_);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
}

void Device::Present(bool vsync) {
    if (swapchain_) {
        swapchain_->Present(vsync ? 1 : 0, 0);
    }
}

bool Device::ReadBack(std::vector<uint8_t>& bgra, int& outWidth, int& outHeight) {
    ID3D11Texture2D* source = offscreen_ ? finalTexture_.Get() : backbuffer_.Get();
    if (!staging_ || !source) {
        return false;
    }
    context_->CopyResource(staging_.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        LogError("Failed to map readback texture.");
        return false;
    }

    bgra.resize(size_t(width_) * size_t(height_) * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < height_; ++y) {
        memcpy(bgra.data() + size_t(y) * size_t(width_) * 4,
               src + size_t(y) * mapped.RowPitch, size_t(width_) * 4);
    }
    context_->Unmap(staging_.Get(), 0);

    outWidth = width_;
    outHeight = height_;
    return true;
}

void Device::WaitIdle() {
    if (context_) {
        context_->Flush();
    }
}

void* Device::NativeHandle() const {
    return device_.Get();
}

std::unique_ptr<rhi::Buffer> Device::CreateBuffer(const BufferDesc& desc) {
    if (!device_ || desc.size == 0) {
        return nullptr;
    }

    const bool dynamic = HasUsage(desc.usage, BufferUsage::Dynamic);
    const bool staging = HasUsage(desc.usage, BufferUsage::Staging);
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = UINT(desc.size);
    bufferDesc.Usage = dynamic ? D3D11_USAGE_DYNAMIC
                               : (staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT);
    bufferDesc.BindFlags = ToBindFlags(desc.usage);
    bufferDesc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE
                                        : (staging ? D3D11_CPU_ACCESS_READ : 0);
    if (HasUsage(desc.usage, BufferUsage::Indirect)) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    }
    if ((HasUsage(desc.usage, BufferUsage::Storage) ||
         HasUsage(desc.usage, BufferUsage::ShaderRead)) &&
        desc.structureStride > 0) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufferDesc.StructureByteStride = desc.structureStride;
    }

    D3D11_SUBRESOURCE_DATA initial{};
    if (desc.initialData && !dynamic && !staging) {
        initial.pSysMem = desc.initialData;
    }
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device_->CreateBuffer(&bufferDesc,
                                     initial.pSysMem ? &initial : nullptr, &buffer))) {
        return nullptr;
    }

    ComPtr<ID3D11ShaderResourceView> srv;
    if (HasUsage(desc.usage, BufferUsage::ShaderRead)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements =
            UINT(desc.size / std::max<uint64_t>(desc.structureStride, 4));
        if (FAILED(device_->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv))) {
            return nullptr;
        }
    }
    return std::make_unique<BufferImpl>(std::move(buffer), std::move(srv), context_.Get());
}

std::unique_ptr<rhi::Texture> Device::CreateTexture(const TextureDesc& desc) {
    if (!device_ || desc.width == 0 || desc.height == 0) {
        return nullptr;
    }
    const DXGI_FORMAT format = ToDxgiFormat(desc.format);
    if (format == DXGI_FORMAT_UNKNOWN) {
        return nullptr;
    }

    std::vector<D3D11_SUBRESOURCE_DATA> subresources;
    const D3D11_SUBRESOURCE_DATA* initial = nullptr;
    if (desc.subresources && desc.subresourceCount > 0) {
        subresources.resize(desc.subresourceCount);
        for (uint32_t i = 0; i < desc.subresourceCount; ++i) {
            subresources[i].pSysMem = desc.subresources[i].data;
            subresources[i].SysMemPitch = desc.subresources[i].rowPitch;
            subresources[i].SysMemSlicePitch = 0;
        }
        initial = subresources.data();
    }
    const D3D11_USAGE usage = HasUsage(desc.usage, TextureUsage::Staging)
                                  ? D3D11_USAGE_STAGING
                                  : (initial ? D3D11_USAGE_IMMUTABLE
                                             : D3D11_USAGE_DEFAULT);

    ComPtr<ID3D11Texture2D> texture2D;
    ComPtr<ID3D11Texture3D> texture3D;
    if (desc.depth > 1) {
        D3D11_TEXTURE3D_DESC textureDesc{};
        textureDesc.Width = desc.width;
        textureDesc.Height = desc.height;
        textureDesc.Depth = desc.depth;
        textureDesc.MipLevels = desc.mipLevels;
        textureDesc.Format = format;
        textureDesc.Usage = usage;
        textureDesc.BindFlags = ToBindFlags(desc.usage);
        textureDesc.CPUAccessFlags = HasUsage(desc.usage, TextureUsage::Staging)
                                         ? D3D11_CPU_ACCESS_READ
                                         : 0;
        if (FAILED(device_->CreateTexture3D(&textureDesc, initial, &texture3D))) {
            return nullptr;
        }
    } else {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = desc.width;
        textureDesc.Height = desc.height;
        textureDesc.MipLevels = desc.mipLevels;
        textureDesc.ArraySize = 1;
        textureDesc.Format = format;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = usage;
        textureDesc.BindFlags = ToBindFlags(desc.usage);
        textureDesc.CPUAccessFlags = HasUsage(desc.usage, TextureUsage::Staging)
                                         ? D3D11_CPU_ACCESS_READ
                                         : 0;
        if (FAILED(device_->CreateTexture2D(&textureDesc, initial, &texture2D))) {
            return nullptr;
        }
    }

    ID3D11Resource* resource = texture2D ? static_cast<ID3D11Resource*>(texture2D.Get())
                                         : static_cast<ID3D11Resource*>(texture3D.Get());
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (HasUsage(desc.usage, TextureUsage::Sampled)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        const D3D11_SHADER_RESOURCE_VIEW_DESC* srvDescPtr = nullptr;
        if (desc.format == Format::R32_TYPELESS) {
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            if (desc.depth > 1) {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MipLevels = desc.mipLevels;
            } else {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = desc.mipLevels;
            }
            srvDescPtr = &srvDesc;
        }
        if (FAILED(device_->CreateShaderResourceView(resource, srvDescPtr, &srv))) {
            return nullptr;
        }
    }
    if (HasUsage(desc.usage, TextureUsage::RenderTarget) &&
        FAILED(device_->CreateRenderTargetView(resource, nullptr, &rtv))) {
        return nullptr;
    }
    if (HasUsage(desc.usage, TextureUsage::DepthStencil)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        const D3D11_DEPTH_STENCIL_VIEW_DESC* dsvDescPtr = nullptr;
        if (desc.format == Format::R32_TYPELESS) {
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvDescPtr = &dsvDesc;
        }
        if (FAILED(device_->CreateDepthStencilView(resource, dsvDescPtr, &dsv))) {
            return nullptr;
        }
    }
    if (HasUsage(desc.usage, TextureUsage::Storage) &&
        FAILED(device_->CreateUnorderedAccessView(resource, nullptr, &uav))) {
        return nullptr;
    }

    return std::make_unique<TextureImpl>(std::move(texture2D), std::move(texture3D),
                                         std::move(srv), std::move(rtv), std::move(dsv),
                                         std::move(uav), desc);
}

std::unique_ptr<rhi::Shader> Device::CreateShader(const ShaderDesc& desc) {
    if (!device_ || !desc.data || desc.size == 0) {
        return nullptr;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    switch (desc.stage) {
    case ShaderStage::Vertex:
        if (FAILED(device_->CreateVertexShader(desc.data, desc.size, nullptr, &vs))) {
            return nullptr;
        }
        break;
    case ShaderStage::Pixel:
        if (FAILED(device_->CreatePixelShader(desc.data, desc.size, nullptr, &ps))) {
            return nullptr;
        }
        break;
    case ShaderStage::Compute:
        if (FAILED(device_->CreateComputeShader(desc.data, desc.size, nullptr, &cs))) {
            return nullptr;
        }
        break;
    }
    return std::make_unique<ShaderImpl>(desc.stage, std::move(vs), std::move(ps),
                                        std::move(cs));
}

std::unique_ptr<rhi::Sampler> Device::CreateSampler(const SamplerDesc& desc) {
    if (!device_) {
        return nullptr;
    }
    D3D11_SAMPLER_DESC samplerDesc{};
    if (desc.comparison) {
        samplerDesc.Filter = desc.linear
                                 ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                 : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        samplerDesc.MaxLOD = 0.0f;
    } else {
        samplerDesc.Filter = desc.linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                                         : D3D11_FILTER_MIN_MAG_MIP_POINT;
    }
    const D3D11_TEXTURE_ADDRESS_MODE address =
        desc.clamp ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.AddressU = address;
    samplerDesc.AddressV = address;
    samplerDesc.AddressW = address;
    samplerDesc.BorderColor[0] = desc.border[0];
    samplerDesc.BorderColor[1] = desc.border[1];
    samplerDesc.BorderColor[2] = desc.border[2];
    samplerDesc.BorderColor[3] = desc.border[3];

    ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device_->CreateSamplerState(&samplerDesc, &sampler))) {
        return nullptr;
    }
    return std::make_unique<SamplerImpl>(std::move(sampler));
}

std::unique_ptr<rhi::Pipeline> Device::CreatePipeline(const PipelineDesc& desc) {
    if (!device_ || !desc.vertexShader) {
        return nullptr;
    }

    std::vector<D3D11_INPUT_ELEMENT_DESC> layout;
    layout.reserve(desc.vertexAttributeCount);
    for (uint32_t i = 0; i < desc.vertexAttributeCount; ++i) {
        const VertexAttribute& attribute = desc.vertexAttributes[i];
        const DXGI_FORMAT format = ToDxgiFormat(attribute.format);
        if (!attribute.semantic || format == DXGI_FORMAT_UNKNOWN) {
            return nullptr;
        }
        D3D11_INPUT_ELEMENT_DESC element{};
        element.SemanticName = attribute.semantic;
        element.SemanticIndex = attribute.semanticIndex;
        element.Format = format;
        element.InputSlot = attribute.bufferSlot;
        element.AlignedByteOffset = attribute.offset;
        element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        element.InstanceDataStepRate = 0;
        layout.push_back(element);
    }

    ComPtr<ID3D11InputLayout> inputLayout;
    if (!layout.empty()) {
        if (!desc.vertexShaderBytecode || desc.vertexShaderBytecodeSize == 0) {
            return nullptr;
        }
        if (FAILED(device_->CreateInputLayout(layout.data(), UINT(layout.size()),
                                              desc.vertexShaderBytecode,
                                              desc.vertexShaderBytecodeSize,
                                              &inputLayout))) {
            return nullptr;
        }
    }

    ComPtr<ID3D11VertexShader> vs(static_cast<ID3D11VertexShader*>(
        desc.vertexShader->NativeHandle()));
    ComPtr<ID3D11PixelShader> ps;
    if (desc.pixelShader) {
        ps = static_cast<ID3D11PixelShader*>(desc.pixelShader->NativeHandle());
    }
    if (!vs) {
        return nullptr;
    }

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = desc.depthTest ? TRUE : FALSE;
    depthDesc.DepthWriteMask =
        desc.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = ToComparisonFunc(desc.depthCompare);
    ComPtr<ID3D11DepthStencilState> depthState;
    if (FAILED(device_->CreateDepthStencilState(&depthDesc, &depthState))) {
        return nullptr;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = ToCullMode(desc.cullMode);
    rasterizerDesc.DepthClipEnable = desc.depthClip ? TRUE : FALSE;
    ComPtr<ID3D11RasterizerState> rasterizer;
    if (FAILED(device_->CreateRasterizerState(&rasterizerDesc, &rasterizer))) {
        return nullptr;
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = desc.blendEnable ? TRUE : FALSE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blendState;
    if (FAILED(device_->CreateBlendState(&blendDesc, &blendState))) {
        return nullptr;
    }

    return std::make_unique<PipelineImpl>(
        std::move(inputLayout), std::move(vs), std::move(ps),
        std::move(depthState), std::move(rasterizer), std::move(blendState),
        ToTopology(desc.topology));
}

void Device::BindPipeline(Pipeline* pipeline) {
    if (!context_ || !pipeline) {
        return;
    }
    if (auto* impl = dynamic_cast<PipelineImpl*>(pipeline)) {
        impl->Bind(context_.Get());
    }
}

} // namespace ex::rhi::d3d11
