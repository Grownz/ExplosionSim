#include "gfx/ImageIO.h"

#include "core/Log.h"

#include <wincodec.h>
#include <wrl/client.h>

namespace ex::gfx {

bool SavePng(const std::wstring& path, const uint8_t* bgra, int width, int height) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LogError("WIC factory creation failed (0x%08X).", unsigned(hr));
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(hr = factory->CreateStream(&stream)) ||
        FAILED(hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        LogError("Could not open '%ls' for writing (0x%08X).", path.c_str(), unsigned(hr));
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        LogError("Could not create PNG encoder (0x%08X).", unsigned(hr));
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(hr = encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(hr = frame->Initialize(nullptr)) ||
        FAILED(hr = frame->SetSize(UINT(width), UINT(height)))) {
        LogError("Could not configure PNG frame (0x%08X).", unsigned(hr));
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(hr = frame->SetPixelFormat(&format))) {
        LogError("Could not set PNG pixel format (0x%08X).", unsigned(hr));
        return false;
    }

    const UINT stride = UINT(width) * 4;
    const UINT size = stride * UINT(height);
    if (FAILED(hr = frame->WritePixels(UINT(height), stride, size, const_cast<BYTE*>(bgra)))) {
        LogError("Could not write PNG pixels (0x%08X).", unsigned(hr));
        return false;
    }

    if (FAILED(hr = frame->Commit()) || FAILED(hr = encoder->Commit())) {
        LogError("Could not finalize PNG (0x%08X).", unsigned(hr));
        return false;
    }

    LogInfo("Saved screenshot: %ls (%dx%d)", path.c_str(), width, height);
    return true;
}

bool LoadImageBgra(const std::wstring& path, std::vector<uint8_t>& bgra, int& width,
                   int& height) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LogError("WIC factory creation failed (0x%08X).", unsigned(hr));
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnLoad,
                                                       &decoder))) {
        LogError("Could not open image '%ls' (0x%08X).", path.c_str(), unsigned(hr));
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = decoder->GetFrame(0, &frame))) {
        LogError("Could not read image frame '%ls' (0x%08X).", path.c_str(), unsigned(hr));
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter)) ||
        FAILED(hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                          WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom))) {
        LogError("Could not convert image '%ls' (0x%08X).", path.c_str(), unsigned(hr));
        return false;
    }

    UINT w = 0;
    UINT h = 0;
    if (FAILED(hr = converter->GetSize(&w, &h)) || w == 0 || h == 0) {
        LogError("Invalid image size for '%ls'.", path.c_str());
        return false;
    }

    const UINT stride = w * 4;
    bgra.resize(size_t(stride) * size_t(h));
    if (FAILED(hr = converter->CopyPixels(nullptr, stride, UINT(bgra.size()),
                                          bgra.data()))) {
        LogError("Could not copy image pixels '%ls' (0x%08X).", path.c_str(), unsigned(hr));
        return false;
    }

    width = int(w);
    height = int(h);
    return true;
}

} // namespace ex::gfx
