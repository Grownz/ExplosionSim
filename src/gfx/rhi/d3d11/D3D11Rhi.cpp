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

class BufferImpl final : public rhi::Buffer {
public:
    explicit BufferImpl(ComPtr<ID3D11Buffer> buffer) : buffer_(std::move(buffer)) {}

    uint64_t Size() const override {
        D3D11_BUFFER_DESC desc{};
        if (buffer_) {
            buffer_->GetDesc(&desc);
        }
        return desc.ByteWidth;
    }
    void* NativeHandle() const override { return buffer_.Get(); }

private:
    ComPtr<ID3D11Buffer> buffer_;
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

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = UINT(desc.size);
    bufferDesc.Usage = HasUsage(desc.usage, BufferUsage::Staging) ? D3D11_USAGE_STAGING
                                                                 : D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = ToBindFlags(desc.usage);
    bufferDesc.CPUAccessFlags = HasUsage(desc.usage, BufferUsage::Staging)
                                    ? D3D11_CPU_ACCESS_READ
                                    : 0;
    if (HasUsage(desc.usage, BufferUsage::Indirect)) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    }
    if (HasUsage(desc.usage, BufferUsage::Storage) && desc.structureStride > 0) {
        bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufferDesc.StructureByteStride = desc.structureStride;
    }

    D3D11_SUBRESOURCE_DATA initial{};
    if (desc.initialData) {
        initial.pSysMem = desc.initialData;
    }
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device_->CreateBuffer(&bufferDesc, desc.initialData ? &initial : nullptr,
                                     &buffer))) {
        return nullptr;
    }
    return std::make_unique<BufferImpl>(std::move(buffer));
}

std::unique_ptr<rhi::Texture> Device::CreateTexture(const TextureDesc& desc) {
    if (!device_ || desc.width == 0 || desc.height == 0) {
        return nullptr;
    }
    const DXGI_FORMAT format = ToDxgiFormat(desc.format);
    if (format == DXGI_FORMAT_UNKNOWN) {
        return nullptr;
    }

    ComPtr<ID3D11Texture2D> texture2D;
    ComPtr<ID3D11Texture3D> texture3D;
    if (desc.depth > 1) {
        D3D11_TEXTURE3D_DESC textureDesc{};
        textureDesc.Width = desc.width;
        textureDesc.Height = desc.height;
        textureDesc.Depth = desc.depth;
        textureDesc.MipLevels = desc.mipLevels;
        textureDesc.Format = format;
        textureDesc.Usage = HasUsage(desc.usage, TextureUsage::Staging)
                                ? D3D11_USAGE_STAGING
                                : D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = ToBindFlags(desc.usage);
        textureDesc.CPUAccessFlags = HasUsage(desc.usage, TextureUsage::Staging)
                                         ? D3D11_CPU_ACCESS_READ
                                         : 0;
        if (FAILED(device_->CreateTexture3D(&textureDesc, nullptr, &texture3D))) {
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
        textureDesc.Usage = HasUsage(desc.usage, TextureUsage::Staging)
                                ? D3D11_USAGE_STAGING
                                : D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = ToBindFlags(desc.usage);
        textureDesc.CPUAccessFlags = HasUsage(desc.usage, TextureUsage::Staging)
                                         ? D3D11_CPU_ACCESS_READ
                                         : 0;
        if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, &texture2D))) {
            return nullptr;
        }
    }

    ID3D11Resource* resource = texture2D ? static_cast<ID3D11Resource*>(texture2D.Get())
                                         : static_cast<ID3D11Resource*>(texture3D.Get());
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (HasUsage(desc.usage, TextureUsage::Sampled) &&
        FAILED(device_->CreateShaderResourceView(resource, nullptr, &srv))) {
        return nullptr;
    }
    if (HasUsage(desc.usage, TextureUsage::RenderTarget) &&
        FAILED(device_->CreateRenderTargetView(resource, nullptr, &rtv))) {
        return nullptr;
    }
    if (HasUsage(desc.usage, TextureUsage::DepthStencil) &&
        FAILED(device_->CreateDepthStencilView(resource, nullptr, &dsv))) {
        return nullptr;
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

} // namespace ex::rhi::d3d11
