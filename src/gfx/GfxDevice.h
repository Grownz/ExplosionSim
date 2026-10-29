#pragma once

#include "gfx/rhi/Rhi.h"
#include "gfx/rhi/d3d11/D3D11Rhi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ex::gfx {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// Owns the RHI device, the swap chain (interactive mode) or an off-screen
// render target (headless screenshot mode), and the shared depth buffer.
// The D3D11 accessors are a temporary bridge for renderer code that has not
// been ported to the RHI yet.
class GfxDevice {
public:
    GfxDevice() = default;
    ~GfxDevice();

    GfxDevice(const GfxDevice&) = delete;
    GfxDevice& operator=(const GfxDevice&) = delete;

    bool Init(HWND window, int width, int height, bool offscreen, bool debugLayer);
    void Shutdown();

    bool Resize(int width, int height);

    void BindFinalTarget();
    void Present(bool vsync);

    // Copies the final LDR target into a tightly packed BGRA8 buffer.
    bool ReadBack(std::vector<uint8_t>& bgra, int& outWidth, int& outHeight);

    rhi::Device* Rhi() const { return rhi_.get(); }

    ID3D11Device* Device() const;
    ID3D11DeviceContext* Context() const;
    ID3D11RenderTargetView* FinalRTV() const;
    ID3D11DepthStencilView* DepthDSV() const;
    ID3D11ShaderResourceView* DepthSRV() const;
    ID3D11Texture2D* DepthTexture() const;

    int Width() const { return rhi_ ? rhi_->Width() : 0; }
    int Height() const { return rhi_ ? rhi_->Height() : 0; }
    float Aspect() const {
        return Height() > 0 ? float(Width()) / float(Height()) : 1.0f;
    }
    bool Offscreen() const { return rhi_ ? rhi_->Offscreen() : false; }
    const std::string& AdapterName() const;
    uint64_t DedicatedVideoMemory() const {
        return rhi_ ? rhi_->DedicatedVideoMemory() : 0;
    }

private:
    std::unique_ptr<rhi::d3d11::Device> rhi_;
};

} // namespace ex::gfx
