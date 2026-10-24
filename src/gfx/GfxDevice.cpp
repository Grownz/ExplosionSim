#include "gfx/GfxDevice.h"

#include "core/Log.h"

#include <algorithm>

namespace ex::gfx {

namespace {

constexpr DXGI_FORMAT kFinalFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

} // namespace

GfxDevice::~GfxDevice() {
    Shutdown();
}

bool GfxDevice::Init(HWND window, int width, int height, bool offscreen, bool debugLayer) {
    window_ = window;
    offscreen_ = offscreen;
    width_ = std::max(1, width);
    height_ = std::max(1, height);

    if (!CreateDevice(debugLayer)) {
        return false;
    }
    if (!offscreen_ && !CreateSwapChain(window)) {
        return false;
    }
    if (!CreateSizeDependentResources()) {
        return false;
    }
    return true;
}

void GfxDevice::Shutdown() {
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

bool GfxDevice::CreateDevice(bool debugLayer) {
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
        LogError("D3D11CreateDevice failed (0x%08X). A Direct3D 11 capable GPU is required.", unsigned(hr));
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
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8, sizeof(utf8), nullptr, nullptr);
                adapterName_ = utf8;
            }
        }
    }

    LogInfo("Direct3D 11 device ready: %s (feature level %d.%d)",
            adapterName_.c_str(), (obtained >> 12) & 0xF, (obtained >> 8) & 0xF);

    ComPtr<IDXGIDevice> dxgiDevice2;
    ComPtr<IDXGIAdapter> dxgiAdapter;
    if (SUCCEEDED(device_.As(&dxgiDevice2)) && SUCCEEDED(dxgiDevice2->GetAdapter(&dxgiAdapter))) {
        dxgiAdapter->GetParent(IID_PPV_ARGS(&factory_));
    }
    if (!factory_) {
        factory_ = factory;
    }
    return true;
}

bool GfxDevice::CreateSwapChain(HWND window) {
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

    HRESULT hr = factory_->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swapchain_);
    if (FAILED(hr)) {
        LogError("CreateSwapChainForHwnd failed (0x%08X).", unsigned(hr));
        return false;
    }

    factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool GfxDevice::CreateSizeDependentResources() {
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

    // Staging texture used for PNG screenshots (works for the swap chain and
    // the offscreen target alike).
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

void GfxDevice::ReleaseSizeDependentResources() {
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

bool GfxDevice::Resize(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == width_ && height == height_) {
        return true;
    }

    width_ = width;
    height_ = height;

    ReleaseSizeDependentResources();
    if (swapchain_) {
        HRESULT hr = swapchain_->ResizeBuffers(0, UINT(width_), UINT(height_), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            LogError("ResizeBuffers failed (0x%08X).", unsigned(hr));
            return false;
        }
    }
    return CreateSizeDependentResources();
}

void GfxDevice::BindFinalTarget() {
    ID3D11RenderTargetView* rtv = finalRTV_.Get();
    context_->OMSetRenderTargets(1, &rtv, dsv_.Get());
    D3D11_VIEWPORT viewport{};
    viewport.Width = float(width_);
    viewport.Height = float(height_);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
}

void GfxDevice::Present(bool vsync) {
    if (swapchain_) {
        swapchain_->Present(vsync ? 1 : 0, 0);
    }
}

bool GfxDevice::ReadBack(std::vector<uint8_t>& bgra, int& outWidth, int& outHeight) {
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
               src + size_t(y) * mapped.RowPitch,
               size_t(width_) * 4);
    }
    context_->Unmap(staging_.Get(), 0);

    outWidth = width_;
    outHeight = height_;
    return true;
}

} // namespace ex::gfx
