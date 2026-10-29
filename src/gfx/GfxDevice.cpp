#include "gfx/GfxDevice.h"

namespace ex::gfx {

GfxDevice::~GfxDevice() {
    Shutdown();
}

bool GfxDevice::Init(HWND window, int width, int height, bool offscreen, bool debugLayer) {
    rhi::DeviceDesc desc{};
    desc.backend = rhi::Backend::D3D11;
    desc.debug = debugLayer;
    desc.window = window;
    desc.width = width;
    desc.height = height;
    desc.offscreen = offscreen;

    std::unique_ptr<rhi::Device> device = rhi::CreateD3D11Device(desc);
    if (!device) {
        return false;
    }
    rhi_.reset(static_cast<rhi::d3d11::Device*>(device.release()));
    return true;
}

void GfxDevice::Shutdown() {
    rhi_.reset();
}

bool GfxDevice::Resize(int width, int height) {
    return rhi_ && rhi_->Resize(width, height);
}

void GfxDevice::BindFinalTarget() {
    if (rhi_) {
        rhi_->BindFinalTarget();
    }
}

void GfxDevice::Present(bool vsync) {
    if (rhi_) {
        rhi_->Present(vsync);
    }
}

bool GfxDevice::ReadBack(std::vector<uint8_t>& bgra, int& outWidth, int& outHeight) {
    return rhi_ && rhi_->ReadBack(bgra, outWidth, outHeight);
}

ID3D11Device* GfxDevice::Device() const {
    return rhi_ ? rhi_->D3DDevice() : nullptr;
}

ID3D11DeviceContext* GfxDevice::Context() const {
    return rhi_ ? rhi_->Context() : nullptr;
}

ID3D11RenderTargetView* GfxDevice::FinalRTV() const {
    return rhi_ ? rhi_->FinalRTV() : nullptr;
}

ID3D11DepthStencilView* GfxDevice::DepthDSV() const {
    return rhi_ ? rhi_->DepthDSV() : nullptr;
}

ID3D11ShaderResourceView* GfxDevice::DepthSRV() const {
    return rhi_ ? rhi_->DepthSRV() : nullptr;
}

ID3D11Texture2D* GfxDevice::DepthTexture() const {
    return rhi_ ? rhi_->DepthTexture() : nullptr;
}

const std::string& GfxDevice::AdapterName() const {
    static const std::string kUnknown = "unknown";
    return rhi_ ? rhi_->AdapterName() : kUnknown;
}

} // namespace ex::gfx
