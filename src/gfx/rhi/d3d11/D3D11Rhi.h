#pragma once

#include "gfx/rhi/Rhi.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ex::rhi::d3d11 {

using Microsoft::WRL::ComPtr;

// D3D11 implementation of the RHI. The legacy accessors exist only for the
// renderer code that has not been ported to the RHI yet; they are removed once
// SceneRenderer/Renderer/GpuEulerSolver use the generic interfaces.
class Device final : public rhi::Device {
public:
    Device() = default;
    ~Device() override;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool Initialize(const DeviceDesc& desc);
    void Shutdown();

    Backend GetBackend() const override { return Backend::D3D11; }
    std::unique_ptr<Buffer> CreateBuffer(const BufferDesc& desc) override;
    std::unique_ptr<Texture> CreateTexture(const TextureDesc& desc) override;
    std::unique_ptr<Shader> CreateShader(const ShaderDesc& desc) override;
    bool Resize(int width, int height) override;
    void WaitIdle() override;
    void* NativeHandle() const override;

    // --- Temporary legacy bridge for the unported renderer -----------------
    ID3D11Device* D3DDevice() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    ID3D11RenderTargetView* FinalRTV() const { return finalRTV_.Get(); }
    ID3D11DepthStencilView* DepthDSV() const { return dsv_.Get(); }
    ID3D11ShaderResourceView* DepthSRV() const { return depthSRV_.Get(); }
    ID3D11Texture2D* DepthTexture() const { return depthTexture_.Get(); }

    void BindFinalTarget();
    void Present(bool vsync);
    bool ReadBack(std::vector<uint8_t>& bgra, int& outWidth, int& outHeight);

    int Width() const { return width_; }
    int Height() const { return height_; }
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

} // namespace ex::rhi::d3d11
