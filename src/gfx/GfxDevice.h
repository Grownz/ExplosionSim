#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ex::gfx {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Owns the D3D11 device, the swap chain (interactive mode) or an off-screen
// render target (headless screenshot mode), and the shared depth buffer.
class GfxDevice {
public:
    GfxDevice() = default;
    ~GfxDevice();

    GfxDevice(const GfxDevice&) = delete;
    GfxDevice& operator=(const GfxDevice&) = delete;

    bool Init(HWND window, int width, int height, bool offscreen, bool debugLayer);
    void Shutdown();

    bool Resize(int width, int height);

    void ClearFinal(float r, float g, float b, float a, float depth = 1.0f);
    void BindFinalTarget();
    void Present(bool vsync);

    // Copies the final LDR target into a tightly packed BGRA8 buffer.
    bool ReadBack(std::vector<uint8_t>& bgra, int& outWidth, int& outHeight);

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    ID3D11RenderTargetView* FinalRTV() const { return finalRTV_.Get(); }
    ID3D11DepthStencilView* DepthDSV() const { return dsv_.Get(); }
    ID3D11ShaderResourceView* DepthSRV() const { return depthSRV_.Get(); }
    ID3D11Texture2D* DepthTexture() const { return depthTexture_.Get(); }

    int Width() const { return width_; }
    int Height() const { return height_; }
    float Aspect() const { return height_ > 0 ? float(width_) / float(height_) : 1.0f; }
    bool Offscreen() const { return offscreen_; }
    const std::string& AdapterName() const { return adapterName_; }
    uint64_t DedicatedVideoMemory() const { return dedicatedVideoMemory_; }

private:
    bool CreateDevice(bool debugLayer);
    bool CreateSwapChain(HWND window);
    bool CreateSizeDependentResources();
    void ReleaseSizeDependentResources();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<IDXGIFactory2> factory_;

    ComPtr<ID3D11Texture2D> backbuffer_;
    ComPtr<ID3D11RenderTargetView> backbufferRTV_;

    ComPtr<ID3D11Texture2D> finalTexture_;
    ComPtr<ID3D11RenderTargetView> finalRTV_;

    ComPtr<ID3D11Texture2D> depthTexture_;
    ComPtr<ID3D11DepthStencilView> dsv_;
    ComPtr<ID3D11ShaderResourceView> depthSRV_;

    ComPtr<ID3D11Texture2D> staging_;

    std::string adapterName_ = "unknown";
    uint64_t dedicatedVideoMemory_ = 0;
    HWND window_ = nullptr;
    bool offscreen_ = false;
    int width_ = 0;
    int height_ = 0;
};

} // namespace ex::gfx
